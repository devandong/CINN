#include "cinn/optim/vectorize_loops.h"

#include <algorithm>
#include <map>
#include <string>

#include "cinn/common/ir_util.h"
#include "cinn/ir/ir_operators.h"
#include "cinn/ir/ir_printer.h"
#include "cinn/optim/ir_replace.h"
#include "cinn/optim/ir_simplify.h"
#include "cinn/utils/functional.h"

namespace cinn {
namespace optim {
using namespace ir;  // NOLINT
using common::make_const;
using common::make_one;
using common::make_zero;

//! Widen an expression to the given number of lanes.
Expr Widen(Expr e, int lanes) {
  if (e.type().lanes() == lanes) return e;
  if (const ir::Broadcast *op = e.As<ir::Broadcast>()) {
    if (lanes % op->lanes == 0) {
      return ir::Broadcast::Make(op->value, lanes);
    }
  }

  CHECK_EQ(e.type().lanes(), 1) << "Cannot broadcast lanes from " << e.type().lanes() << " to " << lanes;
  return ir::Broadcast::Make(e, lanes);
}

//! Substitutes a vector for a scalar var in a Stmt.
class Vectorizer : public IRMutator<Expr *> {
  //! The name of the variable to be vectorized.
  Var var;

  int lanes_{-1};

  bool need_scalarize_{false};

  bool to_vectorize_{false};

  Expr ramp_;

  //! A suffix to attach to widened variables.
  std::string widen_suffix;

 public:
  Vectorizer(const Var &var, int lanes) : var(var), lanes_(lanes) {
    // the identity ramp.
    ramp_ = Ramp::Make(make_zero(), make_one(), lanes_);
  }

  void Visit(Expr *expr) {
    CHECK(!need_scalarize_);
    IRMutator<Expr *>::Visit(expr, expr);

    if (need_scalarize_) {
      need_scalarize_ = false;
      Scalarize(expr);
    }
  }

  void Visit(const Cast *op, Expr *expr) override {
    auto *node = expr->As<Cast>();
    auto v0    = node->v();
    Visit(&node->v());
    if (v0.same_as(node->v())) return;

    Type t = op->type().with_lanes(node->v().type().lanes());
    node->set_type(t);
  }

  void Visit(const _Var_ *op, Expr *expr) override {
    if (op->name == var->name) {
      *expr = Expr(ramp_);
      return;
    }
  }

  void Visit(const Add *op, Expr *expr) override { MutateAddSubOperator(op, expr); }
  void Visit(const Sub *op, Expr *expr) override { MutateAddSubOperator(op, expr); }
  void Visit(const Mul *op, Expr *expr) override { MutateMulDivOperator(op, expr); }
  void Visit(const Div *op, Expr *expr) override { MutateMulDivOperator(op, expr); }
  void Visit(const Mod *op, Expr *expr) override { BinaryOperatorVec(op, expr); }
  void Visit(const Min *op, Expr *expr) override { BinaryOperatorVec(op, expr); }
  void Visit(const Max *op, Expr *expr) override { BinaryOperatorVec(op, expr); }
  void Visit(const EQ *op, Expr *expr) override { BinaryOperatorVec(op, expr); }
  void Visit(const NE *op, Expr *expr) override { BinaryOperatorVec(op, expr); }
  void Visit(const LT *op, Expr *expr) override { BinaryOperatorVec(op, expr); }
  void Visit(const LE *op, Expr *expr) override { BinaryOperatorVec(op, expr); }
  void Visit(const GT *op, Expr *expr) override { BinaryOperatorVec(op, expr); }
  void Visit(const GE *op, Expr *expr) override { BinaryOperatorVec(op, expr); }
  void Visit(const And *op, Expr *expr) override { BinaryOperatorVec(op, expr); }
  void Visit(const Or *op, Expr *expr) override { BinaryOperatorVec(op, expr); }

  void Visit(const Ramp *op, Expr *expr) override {}

  void Visit(const Select *op, Expr *expr) override {
    auto *node        = expr->As<Select>();
    auto condition0   = node->condition;
    auto true_value0  = node->true_value;
    auto false_value0 = node->false_value;

    Visit(&node->condition);
    Visit(&node->true_value);
    Visit(&node->false_value);

    if (condition0.same_as(node->condition) && true_value0.same_as(node->true_value) &&
        false_value0.same_as(node->false_value))
      return;

    int lanes =
        utils::Max(node->condition.type().lanes(), node->true_value.type().lanes(), node->false_value.type().lanes());
    node->true_value  = Widen(node->true_value, lanes);
    node->false_value = Widen(node->false_value, lanes);
  }

  void Visit(const Load *op, Expr *expr) override {
    auto *node                = expr->As<Load>();
    std::vector<Expr> indices = node->indices;
    // We ignore the predicate here.
    bool need_visit = false;
    for (int i = 0; i < indices.size(); i++) {
      Visit(&node->indices[i]);
      if (!node->indices[i].same_as(indices[i])) {
        need_visit = true;
      }
    }
    if (!need_visit) return;

    *expr = Load::Make(node->tensor, node->indices);
  }

  void Visit(const Store *op, Expr *expr) override {
    auto *node  = expr->As<Store>();
    auto value0 = node->value;
    Visit(&node->value);

    std::vector<Expr> indices = node->indices;
    // We ignore the predicate here.
    for (auto &idx : node->indices) {
      Visit(&idx);
    }

    bool need_visit = false;
    for (int i = 0; i < indices.size(); i++) {
      if (!node->indices[i].same_as(indices[i])) {
        need_visit = true;
      }
    }
    if (!need_visit) return;

    int lanes = 0;
    for (auto &idx : node->indices) lanes = std::max(idx.type().lanes(), lanes);
    lanes = std::max(lanes, node->value.type().lanes());

    node->value = Widen(node->value, lanes);

    std::vector<Expr> new_indices;
    for (auto &idx : node->indices) {
      new_indices.push_back(Widen(idx, lanes));
    }
    *expr = Store::Make(node->tensor, node->value, new_indices);
  }

  void Visit(const Call *op, Expr *expr) override { LOG(ERROR) << "Ignore widen Call node"; }

  void Visit(const Let *op, Expr *expr) override {
    auto *node = expr->As<Let>();
    Visit(&node->symbol);
    LOG(ERROR) << "Let not supported";
  }

  void Visit(const IfThenElse *op, Expr *expr) override {
    auto *node = expr->As<IfThenElse>();
    Visit(&node->condition);
    int lanes = node->condition.type().lanes();
    Visit(&node->true_case);
    Visit(&node->false_case);
    LOG(ERROR) << "Ignore Width IfThenElse";
  }

  void Visit(const For *op, Expr *expr) override { ir::IRMutator<>::Visit(op, expr); }

  void Scalarize(Expr *expr) {
    Var idx(var->name + "_s", Int(32));
    std::map<const ir::_Var_ *, Expr> var_map;
    var_map[var.As<ir::_Var_>()] = idx;

    common::Substitute(expr, var_map);
    *expr =
        ir::For::Make(idx, common::make_const(0), common::make_const(lanes_), ForType::Serial, DeviceAPI::Host, *expr);
  }

  template <typename T>
  void MutateAddSubOperator(const T *op, Expr *expr) {
    auto *node = expr->As<T>();
    Expr a0    = node->a();
    Expr b0    = node->b();

    Visit(&node->a());
    Visit(&node->b());

    // if (a0.same_as(node->a()) && b0.same_as(node->b())) return;

    int lanes = std::max(node->a().type().lanes(), node->b().type().lanes());
    if (lanes != 1) {
      const Ramp *a_ramp_n = node->a().template As<Ramp>();
      const Ramp *b_ramp_n = node->b().template As<Ramp>();
      if (node->a().type().lanes() == 1 && b_ramp_n) {
        // a + Ramp(base,stride,lanes) = Ramp(base+a, stride,lanes)
        *expr = Ramp::Make(T::Make(node->a(), b_ramp_n->base),  // base
                           b_ramp_n->stride,                    // stride
                           b_ramp_n->lanes);
        return;
      }
      if (node->b().type().lanes() == 1 && a_ramp_n) {
        *expr = Ramp::Make(T::Make(node->b(), a_ramp_n->base),  // base
                           a_ramp_n->stride,                    // stride
                           a_ramp_n->lanes);
        return;
      }
    }

    *expr = T::Make(Widen(node->a(), lanes), Widen(node->b(), lanes));
  }

  template <typename T>
  void MutateMulDivOperator(const T *op, Expr *expr) {
    Expr a0    = op->a();
    Expr b0    = op->b();
    auto *node = expr->As<T>();
    Visit(&node->a());
    Visit(&node->b());

    // if (a0.same_as(node->a()) && b0.same_as(node->b())) return;
    int lanes = std::max(node->a().type().lanes(), node->b().type().lanes());
    if (lanes != 1) {
      const Ramp *a_ramp_n = node->a().template As<Ramp>();
      const Ramp *b_ramp_n = node->b().template As<Ramp>();
      if (node->a().type().lanes() == 1 && b_ramp_n) {
        // a * Ramp(base,stride,lanes) = Ramp(base*a, stride*a,lanes)
        *expr = Ramp::Make(T::Make(node->a(), b_ramp_n->base),    // base
                           T::Make(node->a(), b_ramp_n->stride),  // stride
                           b_ramp_n->lanes);

        return;
      }
      // Ramp(base,stride,lanes) * b  = Ramp(base*b, stride*b,lanes)
      if (node->b().type().lanes() == 1 && a_ramp_n) {
        *expr = Ramp::Make(T::Make(a_ramp_n->base, node->b()),    // base
                           T::Make(a_ramp_n->stride, node->b()),  // stride
                           a_ramp_n->lanes);
        return;
      }
    }

    *expr = T::Make(Widen(node->a(), lanes), Widen(node->b(), lanes));
  }

  template <typename T>
  Expr BinaryOperatorVec(const T *op, Expr *expr) {
    auto *node = expr->As<T>();
    Expr a0    = node->a();
    Expr b0    = node->b();
    Visit(&node->a());
    Visit(&node->b());
    // if (a0.same_as(node->a()) && b0.same_as(node->b())) return *expr;

    int lanes = std::max(node->a().type().lanes(), node->b().type().lanes());
    return T::Make(Widen(node->a(), lanes), Widen(node->b(), lanes));
  }
};

struct VectorizeLoops_ : public IRMutator<Expr *> {
  const Target &target;

  explicit VectorizeLoops_(const Target &t) : target(t) {}

  void operator()(Expr *expr) { IRMutator::Visit(expr, expr); }

  void Visit(const For *forloop, Expr *expr) {
    auto *node = expr->As<For>();

    // the extent the forloops marked as Vectorized should be int constant
    if (forloop->is_vectorized()) {
      Context::Global().info_rgt().Get<int>("vectorized_forloop_count")++;

      CHECK(forloop->vectorize_info().valid());
      auto _new_forloop = SplitForLoop(node, forloop->vectorize_info().factor);
      if (!_new_forloop.defined()) {
        IRMutator<>::Visit(&node->body, &node->body);
        return;
      }

      node->reset_vectorize_info();

      auto *new_forloop = _new_forloop.As<ir::For>();

      // The forloop generated from polyhedral analysis might have a complex condition that is not something like
      // "i<20" or "i<=20", those cases is not possible to extract the extent.
      auto *extent_int = new_forloop->extent.As<IntImm>();

      int extent = extent_int->value;
      CHECK_GT(extent, 0) << "Loop over " << Expr(new_forloop->loop_var) << " has extent " << new_forloop->extent
                          << ". Can only vectorize loops over a constant extent > 1";

      VLOG(2) << "Vectorizing " << new_forloop->loop_var << " extent " << extent;
      VLOG(2) << "body:\n" << node->body;

      Vectorizer(new_forloop->loop_var, extent).Visit(&new_forloop->body);

      VLOG(2) << "after vectorize body:\n" << node->body;

      // Remove the forloop, the new_forloop's body is vectorized to Ramp, so no forloop is needed.
      node->body = new_forloop->body;
    } else {
      IRMutator::Visit(forloop, expr);
    }
  }

  //! Split the forloop with size \p factor.
  //! @return The new forloop.
  Expr SplitForLoop(For *forloop, int factor) {
    CHECK_GT(factor, 1);
    auto *for_min_i = forloop->min.As<IntImm>();
    CHECK(forloop);
    if (!for_min_i) return Expr();
    if (for_min_i->value != 0) return Expr();

    Expr times = Div::Make(forloop->extent, make_const(factor));
    Simplify(&times);

    // update the current forloop
    forloop->extent = times;
    forloop->set_vectorized(false);

    // create the new forloop
    {
      Var new_iterator(Context::Global().NewName("vi"));
      Expr new_index = Expr(forloop->loop_var) * factor + Expr(new_iterator);
      optim::IrReplace(&forloop->body, forloop->loop_var, new_index);
      auto new_forloop = For::Make(new_iterator,
                                   forloop->min,
                                   make_const(factor),
                                   ForType::Vectorized,
                                   DeviceAPI::UNK,
                                   forloop->body,
                                   forloop->vectorize_info());
      forloop->body    = Block::Make({new_forloop});
      return new_forloop;
    }
  }
};

void VectorizeLoops(Expr *expr, const Target &target) { return VectorizeLoops_(target)(expr); }

namespace detail {

void Vectorize(Var var, int lanes, Expr *expr) {
  Vectorizer vectorizer(var, lanes);
  vectorizer.Visit(expr);
}

}  // namespace detail

}  // namespace optim
}  // namespace cinn
