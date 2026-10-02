#include "../../Engine/BacktestEngine.hpp"

#include "../Model/BacktestExecutionDepth.hpp"
#include "../BacktestClientOrderId.hpp"
#include "../Model/BacktestFillModel.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <utility>

#include "../Internal/BacktestExecutionHelpers.hpp"

namespace hft_backtest::detail::execution_detail {

}  // namespace hft_backtest::detail::execution_detail

namespace hft_backtest::detail {
using execution_detail::activateOrder;
using execution_detail::makeCanceledOrderEvent;
using execution_detail::matchesExpectedOrder;
using execution_detail::nextOrderId;
using execution_detail::pendingEarlier;
using execution_detail::pushUserEvent;
using execution_detail::serviceOrder;
using execution_detail::submitCancelIntent;

void applyDueActions(EngineState& engine, std::int64_t tsNs) {
  if (engine.liquidated || engine.riskStopped) return;
  bool changed = false;
  while (!engine.pending.empty()) {
    if (engine.pending.front().activeTsNs > tsNs) break;
    std::pop_heap(engine.pending.begin(), engine.pending.end(), pendingEarlier);
    PendingAction action = std::move(engine.pending.back());
    engine.pending.pop_back();
    changed = true;
    if (action.kind == PendingActionKind::CancelAck && action.recordIndex < engine.orders.size()) {
      engine.orders[action.recordIndex].activeTsNs = action.activeTsNs;
      engine.orders[action.recordIndex].status = kRecordStatusAcked;
    }
    if (action.slotIndex >= engine.open.size()) continue;
    OpenOrder& order = engine.open[action.slotIndex];
    if (!matchesExpectedOrder(order, action)) {
      if (action.kind != PendingActionKind::OrderAck && action.recordIndex < engine.orders.size()) engine.orders[action.recordIndex].status = kRecordStatusMissing;
      continue;
    }
    if (action.kind == PendingActionKind::OrderAck) {
      activateOrder(engine, order, action.activeTsNs);
      if (order.state.valid && order.warmup && !order.warmupCancelRequested) {
        const bool ackDue = order.warmupCancelAfterAck;
        const bool timeoutDue = order.warmupCancelTimeoutTsNs != 0 &&
            action.activeTsNs >= order.warmupCancelTimeoutTsNs;
        if (ackDue || timeoutDue) {
          trading_core::CancelIntent cancel{};
          cancel.clientIntentId.raw = nextOrderId(engine);
          cancel.targetClientIntentId = order.state.clientIntentId;
          cancel.targetOrderType = order.state.orderType;
          cancel.routeHandle = order.routeHandle;
          const std::int64_t cancelTs = ackDue ? action.activeTsNs : order.warmupCancelTimeoutTsNs;
          order.warmupCancelRequested = true;
          (void)submitCancelIntent(engine, cancel, action.activeTsNs, cancelTs);
        }
      }
    } else if (action.kind == PendingActionKind::CancelAck) {
      if (!serviceOrder(order)) pushUserEvent(engine, makeCanceledOrderEvent(engine, order, action.activeTsNs), action.activeTsNs);
      if (order.warmup) engine.warmupInFlight = false;
      if (order.serviceAutounwind) engine.serviceExposureOpen = false;
      if (order.recordIndex < engine.orders.size()) engine.orders[order.recordIndex].status = kRecordStatusCancelled;
      order = OpenOrder{};
    }
  }
  if (changed) bumpContextRevision(engine);
}

void applyDueActionsBefore(EngineState& engine, std::int64_t tsNs) {
  if (tsNs <= std::numeric_limits<std::int64_t>::min()) return;
  applyDueActions(engine, tsNs - 1);
}


}  // namespace hft_backtest::detail
