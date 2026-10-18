#include "../Engine/BacktestEngine.hpp"

#include "Model/BacktestExecutionDepth.hpp"
#include "BacktestClientOrderId.hpp"
#include "Model/BacktestFillModel.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <utility>

#include "Internal/BacktestExecutionHelpers.hpp"

namespace hft_backtest::detail::execution_detail {

std::size_t findOpenIndex(EngineState& engine, Id id) noexcept {
  for (std::size_t i = 0; i < engine.open.size(); ++i) {
    const OpenOrder& order = engine.open[i];
    if (order.contextIndex == engine.activeContextIndex && order.state.valid && (order.state.clientIntentId.raw == id.raw || order.state.desiredClientIntentId.raw == id.raw || order.state.pendingClientIntentId.raw == id.raw)) return i;
  }
  return kNoRecordIndex;
}

std::size_t findFreeIndex(EngineState& engine) noexcept {
  for (std::size_t i = 0; i < engine.open.size(); ++i) {
    if (!engine.open[i].state.valid) return i;
  }
  return kNoRecordIndex;
}

bool matchesExpectedOrder(const OpenOrder& order, const PendingAction& action) noexcept {
  if (!order.state.valid || order.contextIndex != action.contextIndex) return false;
  if (action.targetClientIntentId.raw != 0 && order.state.clientIntentId.raw != action.targetClientIntentId.raw) return false;
  if (action.commandClientIntentId.raw == 0) return true;
  if (action.kind == PendingActionKind::OrderAck) return order.state.clientIntentId.raw == action.commandClientIntentId.raw;
  return order.state.pendingClientIntentId.raw == action.commandClientIntentId.raw;
}

std::int64_t latencyNs(std::uint64_t latencyUs) noexcept {
  constexpr std::uint64_t kMaxSafeUs = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / 1000);
  if (latencyUs > kMaxSafeUs) return std::numeric_limits<std::int64_t>::max();
  return static_cast<std::int64_t>(latencyUs * 1000u);
}

std::uint8_t recordSide(Side side) noexcept {
  return side.raw == Side::Buy().raw ? kRecordSideBuy : kRecordSideSell;
}

std::uint8_t recordType(canon::OrderType type) noexcept {
  if (type == canon::OrderType::Market) return kRecordTypeMarket;
  if (type == canon::OrderType::Stop) return kRecordTypeStopMarket;
  return kRecordTypeLimit;
}

std::uint64_t nextOrderId(const EngineState& engine) noexcept {
  std::uint64_t out = 1u;
  for (const JsonOrder& order : engine.orders) {
    if (order.id >= out) out = order.id + 1u;
  }
  return out;
}

Side oppositeSide(Side side) noexcept {
  return side.raw == Side::Buy().raw ? Side::Sell() : Side::Buy();
}

bool serviceOrder(const OpenOrder& order) noexcept {
  return order.service || order.warmup || order.serviceAutounwind;
}

std::int64_t addMs(std::int64_t tsNs, std::uint32_t ms) noexcept {
  const std::uint64_t deltaRaw = static_cast<std::uint64_t>(ms) * 1000000ull;
  const std::int64_t delta = deltaRaw > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
      ? std::numeric_limits<std::int64_t>::max()
      : static_cast<std::int64_t>(deltaRaw);
  if (tsNs > std::numeric_limits<std::int64_t>::max() - delta) return std::numeric_limits<std::int64_t>::max();
  return tsNs + delta;
}

bool pendingEarlier(const PendingAction& lhs, const PendingAction& rhs) noexcept {
  if (lhs.activeTsNs != rhs.activeTsNs) return lhs.activeTsNs > rhs.activeTsNs;
  return lhs.submitSequence > rhs.submitSequence;
}

void pushPendingAction(EngineState& engine, PendingAction action) {
  action.submitSequence = engine.nextPendingSubmitSequence++;
  engine.pending.push_back(std::move(action));
  std::push_heap(engine.pending.begin(), engine.pending.end(), pendingEarlier);
}

bool submitCancelIntent(EngineState& engine,
                        const trading_core::CancelIntent& intent,
                        std::int64_t tsNs,
                        std::int64_t activeTs) {
  if (engine.capturedSourceManaged && (!engine.capturedSourceMember ||
      (engine.capturedExecutionRequiredChannels & ~engine.capturedActiveChannels) != 0u ||
      (engine.capturedExecutionRequiredChannels & engine.capturedUnhealthyChannels) != 0u)) return false;
  const std::size_t slotIndex = findOpenIndex(engine, intent.targetClientIntentId);
  if (slotIndex != kNoRecordIndex && engine.capturedSourceManaged &&
      (engine.open[slotIndex].sourceGeneration != engine.capturedSourceGeneration || engine.open[slotIndex].executionBoundaryEpoch != engine.executionBoundaryEpoch)) return false;
  const std::size_t recordIndex = engine.orders.size();
  engine.orders.push_back(JsonOrder{engine.legIndex, intent.clientIntentId.raw, intent.targetClientIntentId.raw, tsNs, activeTs, tsNs, kRecordActionCancel, kRecordSideNone, kRecordTypeNone, kRecordStatusPending, 0, 0, false});
  engine.orders.back().contextIndex = engine.activeContextIndex;
  if (slotIndex != kNoRecordIndex) {
    const OpenOrder& target = engine.open[slotIndex];
    engine.orders.back().service = serviceOrder(target);
    engine.orders.back().warmup = target.warmup;
    engine.orders.back().serviceAutounwind = target.serviceAutounwind;
  }
  if (slotIndex == kNoRecordIndex) {
    engine.orders[recordIndex].status = kRecordStatusMissing;
    return false;
  }
  const trading_core::execution::PermitDecision rateLimitDecision =
      engine.quotaOwner().admit(trading_core::RateLimitActionKind::CancelOrder, timeNsFromI64Local(tsNs));
  if (!rateLimitDecision.ok()) {
    recordRateLimitReject(engine, 3u, tsNs);
    const bool notify = beginRateLimitCooldown(engine, rateLimitDecision, tsNs);
    appendRateLimitUsageSnapshot(engine, tsNs);
    engine.orders[recordIndex].status = kRecordStatusRejected;
    if (notify) {
      publishProblem(engine,
                     canon::OrderProblem::RateLimited,
                     3u,
                     intent.clientIntentId,
                     intent.targetClientIntentId);
    }
    return false;
  }
  appendRateLimitUsageSnapshot(engine, tsNs);
  if (!beginSimulationCancel(engine, intent.targetClientIntentId, intent.clientIntentId)) {
    engine.orders[recordIndex].status = kRecordStatusRejected;
    engine.simulationEvidenceFailed = true;
    return false;
  }
  OpenOrder& order = engine.open[slotIndex];
  order.state.cancelPending = true;
  order.state.pendingClientIntentId = intent.clientIntentId;
  order.state.state = trading_core::StrategyOrderLocalState::CancelPending;
  PendingAction action{};
  action.contextIndex = engine.activeContextIndex;
  action.sourceGeneration = engine.capturedSourceGeneration;
  action.executionBoundaryEpoch = engine.executionBoundaryEpoch;
  action.kind = PendingActionKind::CancelAck;
  action.slotIndex = slotIndex;
  action.recordIndex = recordIndex;
  action.activeTsNs = activeTs;
  action.targetClientIntentId = intent.targetClientIntentId;
  action.commandClientIntentId = intent.clientIntentId;
  pushPendingAction(engine, std::move(action));
  return true;
}

[[nodiscard]] bool submitOrderIntent(EngineState& engine,
                                     const trading_core::OrderIntent& intent,
                                     std::int64_t tsNs,
                                     std::int64_t activeTs,
                                     bool warmup,
                                     std::uint32_t warmupCancelTimeoutMs,
                                     bool warmupCancelAfterAck) {
  if (engine.capturedSourceManaged && (!engine.capturedSourceMember ||
      (engine.capturedExecutionRequiredChannels & ~engine.capturedActiveChannels) != 0u ||
      (engine.capturedExecutionRequiredChannels & engine.capturedUnhealthyChannels) != 0u)) return false;
  const bool postOnly = intent.orderType == canon::OrderType::Limit && isPostOnlyPolicy(intent.limitTimeInForcePolicy);
  const bool service = intent.service || warmup;
  const bool reduceOnly =
      trading_core::positionReductionRequested(intent.positionReduction);
  const std::size_t recordIndex = engine.orders.size();
  engine.orders.push_back(JsonOrder{engine.legIndex, intent.clientIntentId.raw, 0, tsNs, activeTs, tsNs, kRecordActionOrder, recordSide(intent.side), recordType(intent.orderType), kRecordStatusPending, intent.price.raw, intent.qty.raw, reduceOnly, postOnly});
  engine.orders.back().contextIndex = engine.activeContextIndex;
  if (!installSimulationOrderIntent(engine, intent)) {
    engine.orders.back().status = kRecordStatusRejected;
    engine.simulationEvidenceFailed = true;
    return false;
  }
  if (warmup && !markSimulationWarmupIntent(engine, intent.clientIntentId, false)) {
    engine.simulationEvidenceFailed = true; return false;
  }
  engine.orders.back().triggerPriceE8 = intent.triggerPrice.raw;
  engine.orders.back().triggerSource = static_cast<std::uint8_t>(intent.triggerSource);
  engine.orders.back().service = service;
  engine.orders.back().warmup = warmup;
  // This simulator has no immediate match-and-cancel IOC model. Reject the
  // newly expressible policy before quota/active-order installation instead
  // of silently turning it into a resting GTC order.
  if (badStopMarketIntent(intent) ||
      (intent.orderType == canon::OrderType::Limit &&
       intent.limitTimeInForcePolicy == trading_core::LimitTimeInForcePolicy::Ioc)) {
    OpenOrder rejected{};
    rejected.contextIndex = engine.activeContextIndex;
    rejected.state.valid = true;
    rejected.state.clientIntentId = intent.clientIntentId;
    rejected.state.desiredClientIntentId = intent.clientIntentId;
    rejected.state.quoteSlotId = intent.quoteSlotId;
    rejected.state.side = intent.side;
    rejected.state.orderType = intent.orderType;
    rejected.state.limitTimeInForcePolicy = intent.limitTimeInForcePolicy;
    rejected.state.price = intent.price;
    rejected.state.triggerPrice = intent.triggerPrice;
    rejected.state.triggerSource = intent.triggerSource;
    rejected.state.triggeredTimeInForce = intent.triggeredTimeInForce;
    rejected.state.qty = intent.qty;
    rejected.state.desiredPrice = intent.price;
    rejected.state.desiredTriggerPrice = intent.triggerPrice;
    rejected.state.desiredTriggerSource = intent.triggerSource;
    rejected.state.desiredTriggeredTimeInForce = intent.triggeredTimeInForce;
    rejected.state.desiredQty = intent.qty;
    rejected.routeHandle = intent.routeHandle;
    rejected.recordIndex = recordIndex;
    rejected.createdTsNs = tsNs;
    rejected.activeTsNs = activeTs;
    rejected.reduceOnly = reduceOnly;
    rejected.postOnly = postOnly;
    rejected.service = service;
    rejected.warmup = warmup;
    if (!settleSimulationOrderSubmit(engine, intent.clientIntentId, false)) engine.simulationEvidenceFailed = true;
    pushUserEvent(engine, makeRejectedOrderEvent(engine, rejected, tsNs), tsNs, rejected.state.clientIntentId, rejected.contextIndex, serviceOrder(rejected));
    engine.orders[recordIndex].status = kRecordStatusRejected;
    return false;
  }
  const std::size_t slotIndex = findFreeIndex(engine);
  if (slotIndex == kNoRecordIndex) {
    OpenOrder rejected{};
    rejected.contextIndex = engine.activeContextIndex;
    rejected.state.valid = true;
    rejected.state.clientIntentId = intent.clientIntentId;
    rejected.state.desiredClientIntentId = intent.clientIntentId;
    rejected.state.quoteSlotId = intent.quoteSlotId;
    rejected.state.side = intent.side;
    rejected.state.orderType = intent.orderType;
    rejected.state.limitTimeInForcePolicy = intent.limitTimeInForcePolicy;
    rejected.state.price = intent.price;
    rejected.state.triggerPrice = intent.triggerPrice;
    rejected.state.triggerSource = intent.triggerSource;
    rejected.state.triggeredTimeInForce = intent.triggeredTimeInForce;
    rejected.state.qty = intent.qty;
    rejected.state.desiredPrice = intent.price;
    rejected.state.desiredTriggerPrice = intent.triggerPrice;
    rejected.state.desiredTriggerSource = intent.triggerSource;
    rejected.state.desiredTriggeredTimeInForce = intent.triggeredTimeInForce;
    rejected.state.desiredQty = intent.qty;
    rejected.routeHandle = intent.routeHandle;
    rejected.recordIndex = recordIndex;
    rejected.createdTsNs = tsNs;
    rejected.activeTsNs = activeTs;
    rejected.reduceOnly = reduceOnly;
    rejected.postOnly = postOnly;
    rejected.service = service;
    rejected.warmup = warmup;
    if (!settleSimulationOrderSubmit(engine, intent.clientIntentId, false)) engine.simulationEvidenceFailed = true;
    pushUserEvent(engine, makeRejectedOrderEvent(engine, rejected, tsNs), tsNs, rejected.state.clientIntentId, rejected.contextIndex, serviceOrder(rejected));
    engine.orders[recordIndex].status = kRecordStatusRejected;
    return false;
  }
  if (!reserveSpotObservedIntent(engine, intent)) {
    OpenOrder rejected{};
    rejected.contextIndex = engine.activeContextIndex; rejected.state.clientIntentId = intent.clientIntentId;
    rejected.state.side = intent.side; rejected.state.orderType = intent.orderType;
    rejected.state.qty = intent.qty; rejected.state.price = intent.price;
    rejected.reduceOnly = reduceOnly; rejected.service = service; rejected.warmup = warmup;
    if (!settleSimulationOrderSubmit(engine, intent.clientIntentId, false)) engine.simulationEvidenceFailed = true;
    pushUserEvent(engine, makeRejectedOrderEvent(engine, rejected, tsNs, canon::OrderProblem::ValidationRejected),
        tsNs, intent.clientIntentId, engine.activeContextIndex, service);
    engine.orders[recordIndex].status = kRecordStatusRejected;
    return false;
  }
  const trading_core::execution::PermitDecision rateLimitDecision =
      engine.quotaOwner().admit((reduceOnly
          ? (intent.orderType == canon::OrderType::Limit
              ? trading_core::RateLimitActionKind::ReduceOnlyLimitOrder
              : trading_core::RateLimitActionKind::ReduceOnlyMarketOrder)
          : (intent.orderType == canon::OrderType::Limit
              ? trading_core::RateLimitActionKind::LimitOrder
          : trading_core::RateLimitActionKind::MarketOrder)), timeNsFromI64Local(tsNs));
  if (!rateLimitDecision.ok()) {
    recordRateLimitReject(engine, 1u, tsNs);
    const bool notify = beginRateLimitCooldown(engine, rateLimitDecision, tsNs);
    appendRateLimitUsageSnapshot(engine, tsNs);
    OpenOrder rejected{};
    rejected.contextIndex = engine.activeContextIndex;
    rejected.state.valid = true;
    rejected.state.clientIntentId = intent.clientIntentId;
    rejected.state.desiredClientIntentId = intent.clientIntentId;
    rejected.state.quoteSlotId = intent.quoteSlotId;
    rejected.state.side = intent.side;
    rejected.state.orderType = intent.orderType;
    rejected.state.limitTimeInForcePolicy = intent.limitTimeInForcePolicy;
    rejected.state.price = intent.price;
    rejected.state.triggerPrice = intent.triggerPrice;
    rejected.state.triggerSource = intent.triggerSource;
    rejected.state.triggeredTimeInForce = intent.triggeredTimeInForce;
    rejected.state.qty = intent.qty;
    rejected.state.desiredPrice = intent.price;
    rejected.state.desiredTriggerPrice = intent.triggerPrice;
    rejected.state.desiredTriggerSource = intent.triggerSource;
    rejected.state.desiredTriggeredTimeInForce = intent.triggeredTimeInForce;
    rejected.state.desiredQty = intent.qty;
    rejected.routeHandle = intent.routeHandle;
    rejected.recordIndex = recordIndex;
    rejected.createdTsNs = tsNs;
    rejected.activeTsNs = activeTs;
    rejected.reduceOnly = reduceOnly;
    rejected.postOnly = postOnly;
    rejected.service = service;
    rejected.warmup = warmup;
    if (!settleSimulationOrderSubmit(engine, intent.clientIntentId, false)) engine.simulationEvidenceFailed = true;
    pushUserEvent(engine, makeRejectedOrderEvent(engine, rejected, tsNs, canon::OrderProblem::RateLimited), tsNs,
        rejected.state.clientIntentId, rejected.contextIndex, serviceOrder(rejected));
    if (notify) {
      publishProblem(engine, canon::OrderProblem::RateLimited, 1u, intent.clientIntentId, Id{});
    }
    engine.orders[recordIndex].status = kRecordStatusRejected;
    return false;
  }
  appendRateLimitUsageSnapshot(engine, tsNs);
  if (reduceOnly) ++engine.reduceOnlyOrders;
  if (!settleSimulationOrderSubmit(engine, intent.clientIntentId, true)) {
    engine.orders[recordIndex].status = kRecordStatusRejected;
    engine.simulationEvidenceFailed = true;
    return false;
  }
  OpenOrder& slot = engine.open[slotIndex];
  slot.contextIndex = engine.activeContextIndex;
  slot.sourceGeneration = engine.capturedSourceGeneration;
  slot.executionBoundaryEpoch = engine.executionBoundaryEpoch;
  slot.state.valid = true;
  slot.state.clientIntentId = intent.clientIntentId;
  slot.state.desiredClientIntentId = intent.clientIntentId;
  slot.state.quoteSlotId = intent.quoteSlotId;
  slot.state.side = intent.side;
  slot.state.orderType = intent.orderType;
  slot.state.limitTimeInForcePolicy = intent.limitTimeInForcePolicy;
  slot.state.price = intent.price;
  slot.state.triggerPrice = intent.triggerPrice;
  slot.state.triggerSource = intent.triggerSource;
  slot.state.triggeredTimeInForce = intent.triggeredTimeInForce;
  slot.state.qty = intent.qty;
  slot.state.desiredPrice = intent.price;
  slot.state.desiredTriggerPrice = intent.triggerPrice;
  slot.state.desiredTriggerSource = intent.triggerSource;
  slot.state.desiredTriggeredTimeInForce = intent.triggeredTimeInForce;
  slot.state.desiredQty = intent.qty;
  slot.state.state = trading_core::StrategyOrderLocalState::PendingNew;
  slot.routeHandle = intent.routeHandle;
  slot.recordIndex = recordIndex;
  slot.createdTsNs = tsNs;
  slot.activeTsNs = activeTs;
  slot.filledQtyRaw = 0;
  slot.filledNotionalRaw = 0;
  slot.activationSequence = 0u;
  slot.queueAheadQtyRaw = 0;
  slot.aggressiveEvaluated = false;
  slot.active = false;
  slot.reduceOnly = reduceOnly;
  slot.postOnly = postOnly;
  slot.queueInitialized = false;
  slot.stopTriggered = intent.orderType != canon::OrderType::Stop;
  slot.service = service;
  slot.warmup = warmup;
  slot.serviceAutounwind = false;
  slot.warmupCancelAfterAck = warmup && warmupCancelAfterAck;
  slot.warmupCancelRequested = false;
  slot.warmupCancelTimeoutTsNs = warmup && warmupCancelTimeoutMs != 0u ? addMs(activeTs, warmupCancelTimeoutMs) : 0;
  engine.lastOrderPathCommandMono = timeNsFromI64Local(tsNs);
  if (warmup) {
    engine.lastOrderPathAckMono = TimeNs{};
    engine.lastOrderPathWarmupMono = timeNsFromI64Local(tsNs);
    engine.warmupClientIntentId = intent.clientIntentId;
    engine.warmupContextIndex = engine.activeContextIndex;
    engine.lastWarmupSkipReason = 0u;
    engine.warmupInFlight = true;
  } else if (!service) {
    engine.lastOrderPathCommandMono = timeNsFromI64Local(tsNs);
    engine.lastWarmupSkipReason = 0u;
  }
  slot.triggerSourcePriceRaw = 0;
  slot.triggerTsNs = 0;
  PendingAction action{};
  action.contextIndex = engine.activeContextIndex;
  action.sourceGeneration = engine.capturedSourceGeneration;
  action.executionBoundaryEpoch = engine.executionBoundaryEpoch;
  action.kind = PendingActionKind::OrderAck;
  action.slotIndex = slotIndex;
  action.recordIndex = recordIndex;
  action.activeTsNs = activeTs;
  action.targetClientIntentId = intent.clientIntentId;
  action.commandClientIntentId = intent.clientIntentId;
  pushPendingAction(engine, std::move(action));
  return true;
}

}  // namespace hft_backtest::detail::execution_detail

namespace hft_backtest::detail {
using execution_detail::submitCancelIntent;
using execution_detail::submitOrderIntent;

namespace {

template<typename OrderActiveTsFn, typename CancelActiveTsFn>
void applyIntentsImpl(EngineState& engine,
                      const trading_core::StrategyIo& io,
                      std::int64_t tsNs,
                      OrderActiveTsFn orderActiveTs,
                      CancelActiveTsFn cancelActiveTs) {
  if (engine.liquidated || engine.riskStopped) return;
  if (!trading_core::strategyIoCommandSequencesValid(io)) return;

  const bool hasIntents = io.cancelCount != 0u || io.orderCount != 0u || io.commandCount != 0u;
  auto submitCommandOrder = [&](const trading_core::StrategyCommandIntent& command) {
    if (command.kind == trading_core::StrategyCommandIntentKind::WarmupOrder) {
      return submitOrderIntent(engine,
                               command.warmup.order,
                               tsNs,
                               orderActiveTs(command.warmup.order),
                               true,
                               command.warmup.cancelTimeoutMs,
                               command.warmup.cancelAfterAck);
    }
    return submitOrderIntent(engine,
                             command.order,
                             tsNs,
                             orderActiveTs(command.order));
  };

  for (std::size_t i = 0u; i < io.commandCount; ++i) {
    const trading_core::StrategyCommandIntent& command = io.commandOut[i];
    if (command.kind == trading_core::StrategyCommandIntentKind::Cancel) {
      (void)submitCancelIntent(engine, command.cancel, tsNs, cancelActiveTs(command.cancel));
    } else if (command.kind == trading_core::StrategyCommandIntentKind::Order ||
               command.kind == trading_core::StrategyCommandIntentKind::WarmupOrder) {
      (void)submitCommandOrder(command);
    }
  }

  for (std::size_t i = 0u; i < io.cancelCount; ++i) {
    const trading_core::CancelIntent& intent = io.cancelOut[i];
    (void)submitCancelIntent(engine, intent, tsNs, cancelActiveTs(intent));
  }
  for (std::size_t i = 0u; i < io.orderCount; ++i) {
    const trading_core::OrderIntent& intent = io.orderOut[i];
    (void)submitOrderIntent(engine, intent, tsNs, orderActiveTs(intent));
  }
  if (hasIntents) bumpContextRevision(engine);
  applyDueActions(engine, tsNs);
}

}  // namespace

void applyIntents(EngineState& engine,
                  const trading_core::StrategyIo& io,
                  std::int64_t tsNs,
                  std::uint64_t orderLatencyUs,
                  std::uint64_t cancelLatencyUs) {
  applyIntentsImpl(
      engine,
      io,
      tsNs,
      [tsNs, orderLatencyUs](const trading_core::OrderIntent&) noexcept {
        return delayedTs(tsNs, orderLatencyUs);
      },
      [tsNs, cancelLatencyUs](const trading_core::CancelIntent&) noexcept {
        return delayedTs(tsNs, cancelLatencyUs);
      });
}

void applyIntents(EngineState& engine,
                  const trading_core::StrategyIo& io,
                  std::int64_t tsNs,
                  BacktestLatencyRuntime& latency) {
  applyIntentsImpl(
      engine,
      io,
      tsNs,
      [tsNs, &latency](const trading_core::OrderIntent& intent) noexcept {
        const BacktestLatencyChannel channel =
            (intent.orderType == canon::OrderType::Market || intent.orderType == canon::OrderType::Stop)
            ? BacktestLatencyChannel::MarketOrder
            : BacktestLatencyChannel::LimitOrder;
        return delayedTs(tsNs, sampleLatencyUs(latency, channel));
      },
      [tsNs, &latency](const trading_core::CancelIntent&) noexcept {
        return delayedTs(tsNs, sampleLatencyUs(latency, BacktestLatencyChannel::CancelOrder));
      });
}

}  // namespace hft_backtest::detail
