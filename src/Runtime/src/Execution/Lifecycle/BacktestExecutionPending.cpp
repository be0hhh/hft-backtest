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
#include "../../Strategy/Inputs/BacktestStrategyWarmup.hpp"

namespace hft_backtest::detail::execution_detail {

}  // namespace hft_backtest::detail::execution_detail

namespace hft_backtest::detail {
using execution_detail::activateOrder;
using execution_detail::makeCanceledOrderEvent;
using execution_detail::matchesExpectedOrder;
using execution_detail::pendingEarlier;
using execution_detail::pushUserEvent;
using execution_detail::serviceOrder;
using execution_detail::submitCancelIntent;

void applyDueActions(EngineState& engine, std::int64_t tsNs) {
  if (engine.liquidated || engine.riskStopped || engine.simulationEvidenceFailed ||
      (engine.capturedSourceManaged && (!engine.capturedSourceMember ||
       (engine.capturedExecutionRequiredChannels & engine.capturedUnhealthyChannels) != 0u))) return;
  bool changed = false;
  while (!engine.pending.empty()) {
    std::size_t next = engine.pending.size();
    for (std::size_t i = 0; i < engine.pending.size(); ++i) {
      const auto& candidate = engine.pending[i];
      if (candidate.activeTsNs > tsNs || (engine.capturedSourceManaged &&
          (candidate.sourceGeneration != engine.capturedSourceGeneration || candidate.executionBoundaryEpoch != engine.executionBoundaryEpoch))) continue;
      if (next == engine.pending.size() || candidate.activeTsNs < engine.pending[next].activeTsNs ||
          (candidate.activeTsNs == engine.pending[next].activeTsNs &&
           candidate.submitSequence < engine.pending[next].submitSequence)) next = i;
    }
    if (next == engine.pending.size()) break;
    PendingAction action = std::move(engine.pending[next]);
    engine.pending[next] = std::move(engine.pending.back());
    engine.pending.pop_back();
    std::make_heap(engine.pending.begin(), engine.pending.end(), pendingEarlier);
    action.activeTsNs = std::max(action.activeTsNs, engine.replayNotBeforeNs);
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
      if (engine.simulationEvidenceFailed) return;
      if (order.state.valid && order.warmup && !order.warmupCancelRequested) {
        const bool ackDue = order.warmupCancelAfterAck;
        const bool timeoutDue = order.warmupCancelTimeoutTsNs != 0 &&
            action.activeTsNs >= order.warmupCancelTimeoutTsNs;
        if (ackDue || timeoutDue) {
          trading_core::CancelIntent cancel{};
          cancel.clientIntentId = strategy_runtime_detail::allocateBacktestServiceClientIntentId(engine);
          if (!cancel.clientIntentId.raw) { engine.simulationEvidenceFailed = true; continue; }
          cancel.targetClientIntentId = order.state.clientIntentId;
          cancel.targetOrderType = order.state.orderType;
          cancel.routeHandle = order.routeHandle;
          const std::int64_t cancelTs = ackDue ? action.activeTsNs : order.warmupCancelTimeoutTsNs;
          order.warmupCancelRequested = true;
          const auto savedContext = engine.activeContextIndex;
          engine.activeContextIndex = order.contextIndex;
          (void)submitCancelIntent(engine, cancel, action.activeTsNs, cancelTs);
          engine.activeContextIndex = savedContext;
        }
      }
    } else if (action.kind == PendingActionKind::CancelAck) {
      updateSpotVenueReservation(engine, order, 0);
      pushUserEvent(engine, makeCanceledOrderEvent(engine, order, action.activeTsNs), action.activeTsNs, order.state.clientIntentId, order.contextIndex, serviceOrder(order));
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
