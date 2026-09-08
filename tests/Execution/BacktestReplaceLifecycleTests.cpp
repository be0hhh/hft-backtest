#include "trading_core/Execution/Order/OrderStateRules.hpp"
#include "trading_core/Contracts/State/Orders/DesiredOrderState.hpp"

#include <iostream>

namespace {

[[nodiscard]] bool expect(bool value, const char* message) {
  if (!value) std::cerr << "FAIL: " << message << '\n';
  return value;
}

[[nodiscard]] trading_core::StrategyOrderState liveOrder() noexcept {
  trading_core::StrategyOrderState out{};
  out.valid = true;
  out.clientIntentId.raw = 10u;
  out.quoteSlotId.raw = 7u;
  out.side = Side::Buy();
  out.orderType = canon::OrderType::Limit;
  out.limitTimeInForcePolicy =
      trading_core::LimitTimeInForcePolicy::PostOnlyPrefer;
  out.price.raw = 9900000000ll;
  out.desiredPrice = out.price;
  out.qty.raw = 100000000ll;
  out.desiredQty = out.qty;
  out.state = trading_core::StrategyOrderLocalState::LiveLocal;
  return out;
}

[[nodiscard]] trading_core::StrategyDesiredOrderSlot desiredReplacement()
    noexcept {
  trading_core::StrategyDesiredOrderSlot out{};
  out.route = trading_core::kDefaultExecutionRouteHandle;
  out.clientIntentId.raw = 11u;
  out.cancelClientIntentId.raw = 12u;
  out.quoteSlotId.raw = 7u;
  out.side = Side::Buy();
  out.orderType = canon::OrderType::Limit;
  out.limitTimeInForcePolicy =
      trading_core::LimitTimeInForcePolicy::PostOnlyPrefer;
  out.price.raw = 9800000000ll;
  out.qty.raw = 100000000ll;
  out.active = true;
  return out;
}

[[nodiscard]] bool testCanonicalReplaceDecisionLifecycle() {
  trading_core::StrategyOrderStateView view{};
  trading_core::StrategyOrderState order = liveOrder();
  using trading_core::OrderReplaceAction;

  const auto liveMismatch =
      trading_core::strategy::planStrategyOrderReplace(&view, &order, false);
  order.state = trading_core::StrategyOrderLocalState::CancelPending;
  order.cancelPending = true;
  const auto cancelPending =
      trading_core::strategy::planStrategyOrderReplace(&view, &order, false);
  order.cancelPending = false;
  order.state = trading_core::StrategyOrderLocalState::PendingNew;
  const auto pendingNew =
      trading_core::strategy::planStrategyOrderReplace(&view, &order, false);
  order.state = trading_core::StrategyOrderLocalState::Missing;
  order.missing = true;
  const auto missing =
      trading_core::strategy::planStrategyOrderReplace(&view, &order, false);
  order = liveOrder();
  order.state = trading_core::StrategyOrderLocalState::Rejected;
  const auto rejected =
      trading_core::strategy::planStrategyOrderReplace(&view, &order, false);
  order = liveOrder();
  const auto sameDesired =
      trading_core::strategy::planStrategyOrderReplace(&view, &order, true);
  const auto absent =
      trading_core::strategy::planStrategyOrderReplace(
          &view, nullptr, false);

  return expect(liveMismatch.action == OrderReplaceAction::EmitCancelOnly,
                "live mismatch must emit cancel only") &&
      expect(cancelPending.action == OrderReplaceAction::NoEmitNow,
             "cancel-pending mismatch must emit no new order") &&
      expect(pendingNew.action == OrderReplaceAction::NoEmitNow,
             "pending-new mismatch must emit no second order") &&
      expect(missing.action == OrderReplaceAction::EmitNew,
             "missing owner must permit a new order next cycle") &&
      expect(rejected.action == OrderReplaceAction::EmitNew,
             "rejected owner must permit a new order next cycle") &&
      expect(absent.action == OrderReplaceAction::EmitNew,
             "absent owner must permit a new order next cycle") &&
      expect(sameDesired.action == OrderReplaceAction::Keep,
             "same live desired order must be kept");
}

[[nodiscard]] bool testDesiredOrderPlannerWaitsForTerminalSnapshot() {
  trading_core::StrategyOrderState owner = liveOrder();
  trading_core::StrategyOrderStateView view{};
  view.orders = &owner;
  view.count = 1u;
  const trading_core::StrategyDesiredOrderSlot desired =
      desiredReplacement();

  const trading_core::StrategyDesiredOrderPlan liveMismatch =
      trading_core::planDesiredOrderSlot(view, desired);
  owner.state = trading_core::StrategyOrderLocalState::CancelPending;
  owner.cancelPending = true;
  const trading_core::StrategyDesiredOrderPlan cancelPending =
      trading_core::planDesiredOrderSlot(view, desired);
  view.orders = nullptr;
  view.count = 0u;
  const trading_core::StrategyDesiredOrderPlan afterTerminal =
      trading_core::planDesiredOrderSlot(view, desired);

  return expect(liveMismatch.valid &&
                    liveMismatch.action ==
                        trading_core::StrategyDesiredOrderAction::Cancel &&
                    liveMismatch.cancel.targetClientIntentId.raw == 10u,
                "live mismatch must produce only the old-order cancel") &&
      expect(!cancelPending.valid &&
                 cancelPending.action ==
                     trading_core::StrategyDesiredOrderAction::None,
             "cancel-pending owner must suppress replacement") &&
      expect(afterTerminal.valid &&
                 afterTerminal.action ==
                     trading_core::StrategyDesiredOrderAction::Place &&
                 afterTerminal.order.clientIntentId.raw == 11u,
             "next cycle after terminal removal must place the replacement");
}

}  // namespace

int main() {
  bool ok = true;
  ok = testCanonicalReplaceDecisionLifecycle() && ok;
  ok = testDesiredOrderPlannerWaitsForTerminalSnapshot() && ok;
  return ok ? 0 : 1;
}
