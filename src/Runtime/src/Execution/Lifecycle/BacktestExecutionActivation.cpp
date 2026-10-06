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
#include "trading_core/Strategy/Orders/OrderDecisionCursor.hpp"

namespace hft_backtest::detail::execution_detail {

std::int64_t boundedPositionMagnitude(std::int64_t netQtyRaw) noexcept {
  if (netQtyRaw >= 0) return netQtyRaw;
  // Unsigned modulo arithmetic obtains the magnitude without negating
  // INT64_MIN in the signed domain.
  const std::uint64_t magnitude =
      std::uint64_t{0} - static_cast<std::uint64_t>(netQtyRaw);
  const std::uint64_t limit = static_cast<std::uint64_t>(
      std::numeric_limits<std::int64_t>::max());
  return magnitude > limit ? std::numeric_limits<std::int64_t>::max()
                           : static_cast<std::int64_t>(magnitude);
}

std::int64_t reduceOnlyPositionCloseQty(const EngineState& engine, const OpenOrder& order) noexcept {
  if (!order.reduceOnly) return std::numeric_limits<std::int64_t>::max();
  if (engine.netQtyRaw == 0) return 0;
  if (engine.netQtyRaw > 0) {
    if (order.state.side.raw != Side::Sell().raw) return 0;
    return engine.netQtyRaw;
  }
  if (order.state.side.raw != Side::Buy().raw) return 0;
  return boundedPositionMagnitude(engine.netQtyRaw);
}

std::int64_t fillableOrderQty(const EngineState& engine,
                              const OpenOrder& order,
                              std::int64_t requestedQty) noexcept {
  if (requestedQty <= 0) return 0;
  const std::int64_t remaining = openOrderRemainingQty(order);
  if (remaining <= 0) return 0;
  std::int64_t out = requestedQty < remaining ? requestedQty : remaining;
  if (!order.reduceOnly) return out;
  const std::int64_t closeQty = reduceOnlyPositionCloseQty(engine, order);
  if (closeQty <= 0) return 0;
  if (out > closeQty) out = closeQty;
  return out;
}

bool rejectOrder(EngineState& engine, OpenOrder& order, std::int64_t tsNs) {
  if (!order.state.valid) return false;
  updateSpotVenueReservation(engine, order, 0);
  pushUserEvent(engine, makeRejectedOrderEvent(engine, order, tsNs), tsNs, order.state.clientIntentId, order.contextIndex, serviceOrder(order));
  if (order.serviceAutounwind) engine.serviceExposureOpen = engine.netQtyRaw != 0;
  if (order.recordIndex < engine.orders.size()) engine.orders[order.recordIndex].status = kRecordStatusRejected;
  order = OpenOrder{};
  bumpContextRevision(engine);
  return false;
}

void terminalizeUnavailableLiquidity(EngineState& engine, OpenOrder& order, std::int64_t tsNs) {
  if (!order.state.valid) return;
  updateSpotVenueReservation(engine, order, 0);
  pushUserEvent(engine, makeCanceledOrderEvent(engine, order, tsNs), tsNs, order.state.clientIntentId, order.contextIndex, serviceOrder(order));
  if (order.serviceAutounwind) engine.serviceExposureOpen = engine.netQtyRaw != 0;
  if (order.recordIndex < engine.orders.size()) engine.orders[order.recordIndex].status = kRecordStatusPartialUnfilled;
  order = OpenOrder{};
  bumpContextRevision(engine);
}

std::int64_t triggeredStopLimitBboPrice(const OpenOrder& order,
                                        const cxet::composite::BookTicker& top) noexcept {
  return order.state.side.raw == Side::Buy().raw ? top.ask.px.raw : top.bid.px.raw;
}

std::int64_t triggeredStopLimitBboQty(const OpenOrder& order,
                                      const cxet::composite::BookTicker& top) noexcept {
  return order.state.side.raw == Side::Buy().raw ? top.ask.qty.raw : top.bid.qty.raw;
}

bool triggeredStopLimitBboCapOk(const OpenOrder& order, std::int64_t topPrice) noexcept {
  if (topPrice <= 0) return false;
  return order.state.side.raw == Side::Buy().raw
      ? topPrice <= order.state.price.raw
      : topPrice >= order.state.price.raw;
}

void fillTriggeredStopLimitFromBbo(EngineState& engine,
                                   OpenOrder& order,
                                   const cxet::composite::BookTicker& top,
                                   std::int64_t tsNs,
                                   FillExecutionMetadata metadata) {
  const std::int64_t topPrice = triggeredStopLimitBboPrice(order, top);
  const std::int64_t topQty = triggeredStopLimitBboQty(order, top);
  const std::int64_t remaining = openOrderRemainingQty(order);
  if (!triggeredStopLimitBboCapOk(order, topPrice) || topQty <= 0 || remaining <= 0) {
    terminalizeUnavailableLiquidity(engine, order, tsNs);
    return;
  }
  if (triggeredStopFokLimitOrder(order) && topQty < remaining) {
    terminalizeUnavailableLiquidity(engine, order, tsNs);
    return;
  }
  const std::int64_t fillQty = topQty < remaining ? topQty : remaining;
  (void)fillOrderQty(engine, order, topPrice, fillQty, tsNs, true, metadata);
  if (order.state.valid) terminalizeUnavailableLiquidity(engine, order, tsNs);
}

bool isPostOnlyPolicy(trading_core::LimitTimeInForcePolicy policy) noexcept {
  return policy == trading_core::LimitTimeInForcePolicy::PostOnlyPrefer ||
         policy == trading_core::LimitTimeInForcePolicy::PostOnlyStrict;
}

bool postOnlyCrossesTop(const EngineState& engine, const OpenOrder& order) noexcept {
  if (!order.postOnly || order.state.orderType != canon::OrderType::Limit) return false;
  const cxet::composite::BookTicker& top = effectiveBookTicker(engine.venueSnapshot);
  const std::int64_t bid = top.bid.px.raw;
  const std::int64_t ask = top.ask.px.raw;
  if (bid <= 0 || ask <= 0) return false;
  if (order.state.side.raw == Side::Buy().raw) return order.state.price.raw >= ask;
  if (order.state.side.raw == Side::Sell().raw) return order.state.price.raw <= bid;
  return false;
}

bool stopMarketCrossed(const OpenOrder& order, std::int64_t sourcePrice) noexcept {
  if (!order.state.valid || order.state.orderType != canon::OrderType::Stop ||
      order.state.triggerPrice.raw <= 0 || sourcePrice <= 0) return false;
  if (order.state.side.raw == Side::Buy().raw) return sourcePrice >= order.state.triggerPrice.raw;
  if (order.state.side.raw == Side::Sell().raw) return sourcePrice <= order.state.triggerPrice.raw;
  return false;
}

bool knownStopMarketTriggerSource(canon::OrderTriggerSource source) noexcept {
  // Retired public mark/index feeds cannot authorize a simulated stop.
  return source == canon::OrderTriggerSource::Last;
}

bool badStopMarketIntent(const trading_core::OrderIntent& intent) noexcept {
  if (intent.orderType != canon::OrderType::Stop) return false;
  if (intent.triggerPrice.raw <= 0 || !knownStopMarketTriggerSource(intent.triggerSource)) return true;
  if (intent.price.raw < 0) return true;
  if (intent.price.raw == 0) return intent.triggeredTimeInForce != canon::TimeInForce::Unknown;
  return intent.triggeredTimeInForce != canon::TimeInForce::IOC &&
         intent.triggeredTimeInForce != canon::TimeInForce::FOK;
}

bool stopMarketSourcePrice(const EngineState& engine,
                           const OpenOrder& order,
                           std::int64_t& outPrice) noexcept {
  outPrice = 0;
  const canon::OrderTriggerSource source = order.state.triggerSource;
  if (source == canon::OrderTriggerSource::Last) {
    const cxet::composite::BookTicker& top = effectiveBookTicker(engine.venueSnapshot);
    if (order.state.side.raw == Side::Buy().raw) {
      outPrice = top.ask.px.raw;
    } else if (order.state.side.raw == Side::Sell().raw) {
      outPrice = top.bid.px.raw;
    }
    return outPrice > 0;
  }
  return false;
}

bool stopMarketCrossesAtActivation(const EngineState& engine, const OpenOrder& order) noexcept {
  std::int64_t sourcePrice = 0;
  if (!stopMarketSourcePrice(engine, order, sourcePrice)) return false;
  return stopMarketCrossed(order, sourcePrice);
}

bool marketExecutionOrder(const OpenOrder& order) noexcept {
  return order.state.orderType == canon::OrderType::Market ||
         (order.state.orderType == canon::OrderType::Stop && order.stopTriggered);
}

const trading_core::LocalOrderBookState* executionDepthBook(EngineState& engine,
                                                               std::int64_t tsNs) noexcept {
  if (engine.executionDepthProvider != nullptr) {
    if (engine.executionDepthProvider->advanceTo(tsNs) && engine.executionDepthProvider->ready()) {
      return &engine.executionDepthProvider->book();
    }
    if (engine.executionDepthProvider->failed()) {
      engine.depthExecutionMode = BacktestDepthExecutionMode::BboFallback;
      if (engine.depthFallbackReason.empty()) {
        engine.depthFallbackReason = engine.executionDepthProvider->failureReason();
      }
    }
    return nullptr;
  }
  return engine.venueOrderBookExecutionEnabled && trading_core::orderBook(engine.venueOrderBook).initialized()
      ? &engine.venueOrderBook
      : nullptr;
}

bool venueOrderBookExecutionConfigured(const EngineState& engine) noexcept {
  return engine.executionDepthProvider != nullptr ||
         (engine.venueOrderBookExecutionEnabled && trading_core::orderBook(engine.venueOrderBook).initialized());
}

bool needsTopExecutionBook(const OpenOrder& order, std::int64_t bid, std::int64_t ask) noexcept {
  if (!order.state.valid || !order.active) return false;
  if (order.state.orderType == canon::OrderType::Market) return true;
  if (order.state.orderType == canon::OrderType::Stop) return order.stopTriggered;
  if (order.state.orderType != canon::OrderType::Limit) return false;
  if (order.postOnly || order.lastAggressiveBookTsNs != 0) return false;
  if (bid <= 0 || ask <= 0) return true;
  if (order.state.side.raw == Side::Buy().raw) return order.state.price.raw >= ask;
  if (order.state.side.raw == Side::Sell().raw) return order.state.price.raw <= bid;
  return false;
}

bool hasTopExecutionCandidate(const EngineState& engine, std::int64_t bid, std::int64_t ask) noexcept {
  for (const OpenOrder& order : engine.open) {
    if (needsTopExecutionBook(order, bid, ask)) return true;
  }
  return false;
}

bool bboOverlayCandidate(const cxet::composite::BookTicker& top) noexcept {
  return top.bid.px.raw > 0 && top.ask.px.raw > 0 && top.bid.px.raw < top.ask.px.raw;
}

trading_core::AlgoFlag effectiveBidQtyKnown(
    const trading_core::StrategyMarketSnapshot& snapshot,
    const cxet::composite::BookTicker& top) noexcept {
  return snapshot.hasReconstructedBbo.raw == trading_core::kAlgoFlagTrue.raw
      ? snapshot.reconstructedBidQtyKnown
      : (top.bid.qty.raw > 0 ? trading_core::kAlgoFlagTrue : trading_core::kAlgoFlagFalse);
}

trading_core::AlgoFlag effectiveAskQtyKnown(
    const trading_core::StrategyMarketSnapshot& snapshot,
    const cxet::composite::BookTicker& top) noexcept {
  return snapshot.hasReconstructedBbo.raw == trading_core::kAlgoFlagTrue.raw
      ? snapshot.reconstructedAskQtyKnown
      : (top.ask.qty.raw > 0 ? trading_core::kAlgoFlagTrue : trading_core::kAlgoFlagFalse);
}

const trading_core::LocalOrderBookState& overlayedTakerExecutionBook(
    const trading_core::StrategyMarketSnapshot& snapshot,
    const cxet::composite::BookTicker& top,
    const trading_core::LocalOrderBookState& rawBook,
    trading_core::LocalOrderBookState& scratch) noexcept {
  if (!bboOverlayCandidate(top)) return rawBook;
  if (!scratch.copyFromBounded(rawBook)) return rawBook;
  const trading_core::AlgoFlag bidQtyKnown = effectiveBidQtyKnown(snapshot, top);
  const trading_core::AlgoFlag askQtyKnown = effectiveAskQtyKnown(snapshot, top);
  return trading_core::applyBboOverlay(scratch, top, bidQtyKnown, askQtyKnown) ? scratch : rawBook;
}

bool marketDepthFreshEnough(const trading_core::LocalOrderBookState& book,
                            const cxet::composite::BookTicker& top) noexcept {
  if (book.ts.raw == 0u || top.ts.raw == 0u) return true;
  if (top.ts.raw <= book.ts.raw) return true;
  return top.ts.raw - book.ts.raw <= static_cast<std::uint64_t>(kMarketDepthMaxAgeNs);
}

trading_core::LocalOrderBookState& topExecutionOverlayScratch(
    EngineState& engine,
    const trading_core::LocalOrderBookState& source) {
  if (engine.topExecutionOverlayScratch.capacityLevelsPerSide().raw !=
      source.capacityLevelsPerSide().raw) {
    if (!engine.topExecutionOverlayScratch.reserveLevelsPerSide(
            source.capacityLevelsPerSide())) {
      throw std::bad_alloc{};
    }
  }
  return engine.topExecutionOverlayScratch;
}

AggressiveFillPlan& topExecutionFillPlanScratch(EngineState& engine) {
  if (engine.topExecutionFillPlanScratch.empty()) engine.topExecutionFillPlanScratch.resize(1u);
  return engine.topExecutionFillPlanScratch.front();
}

bool marketDepthPlanUsable(const OpenOrder& order,
                           const AggressiveFillPlan& plan,
                           const trading_core::LocalOrderBookState& book,
                           const cxet::composite::BookTicker& top) noexcept {
  if (!marketExecutionOrder(order)) return true;
  if (!marketDepthFreshEnough(book, top)) return false;
  if (plan.count == 0u || plan.visibleDepthCapped || plan.remainingQtyE8 != 0) return false;
  return plan.filledQtyE8 == openOrderRemainingQty(order);
}

void activateOrder(EngineState& engine, OpenOrder& order, std::int64_t activeTsNs) {
  order.active = true;
  order.activeTsNs = activeTsNs;
  order.state.state = trading_core::StrategyOrderLocalState::LiveLocal;
  if (order.recordIndex < engine.orders.size()) {
    engine.orders[order.recordIndex].activeTsNs = activeTsNs;
    engine.orders[order.recordIndex].status = kRecordStatusActive;
  }
  if (postOnlyCrossesTop(engine, order)) {
    (void)rejectOrder(engine, order, activeTsNs);
    return;
  }
  if (stopMarketCrossesAtActivation(engine, order)) {
    (void)rejectOrder(engine, order, activeTsNs);
    return;
  }
  if (!reserveSpotVenueOrder(engine, order)) {
    (void)rejectOrder(engine, order, activeTsNs);
    return;
  }
  auto ack = makeCanceledOrderEvent(engine, order, activeTsNs);
  ack.orderStatus = canon::OrderStatus::Open;
  pushUserEvent(engine, ack, activeTsNs, order.state.clientIntentId, order.contextIndex, serviceOrder(order));
  if (order.state.orderType == canon::OrderType::Limit) {
    if (const trading_core::LocalOrderBookState* book = executionDepthBook(engine, activeTsNs); book != nullptr) {
      initializePassiveQueue(order, engine, *book);
    } else if (!order.queueInitialized) {
      order.queueAheadQtyRaw = 0;
      order.queueInitialized = true;
    }
  }
  tryFillOnTop(engine, activeTsNs, false);
}

}  // namespace hft_backtest::detail::execution_detail

namespace hft_backtest::detail {
using execution_detail::bboOverlayCandidate;
using execution_detail::executionDepthBook;
using execution_detail::fillTriggeredStopLimitFromBbo;
using execution_detail::fillableOrderQty;
using execution_detail::hasTopExecutionCandidate;
using execution_detail::latencyNs;
using execution_detail::makeFillEvent;
using execution_detail::marketDepthPlanUsable;
using execution_detail::marketExecutionOrder;
using execution_detail::needsTopExecutionBook;
using execution_detail::oppositeSide;
using execution_detail::overlayedTakerExecutionBook;
using execution_detail::pushUserEvent;
using execution_detail::recordSide;
using execution_detail::reduceOnlyPositionCloseQty;
using execution_detail::rejectOrder;
using execution_detail::serviceOrder;
using execution_detail::stopMarketCrossed;
using execution_detail::terminalizeUnavailableLiquidity;
using execution_detail::topExecutionFillPlanScratch;
using execution_detail::topExecutionOverlayScratch;
using execution_detail::venueOrderBookExecutionConfigured;

std::size_t visibleOrders(const EngineState& engine,
                          trading_core::StrategyOrderState* out,
                          std::size_t capacity) noexcept {
  if (!out || capacity == 0u) return 0u;
  if (!engine.simulationOrders || !engine.simulationOrders->ready) return 0u;
  std::size_t count = 0u;
  const auto view = simulationOrderEvidenceView(engine);
  const auto& owner = *engine.simulationOrders;
  for (std::size_t i = 0u; i < owner.identities.size(); ++i) {
    const auto& identity = owner.identities[i];
    if (!identity.clientIntentId.raw || identity.contextIndex != engine.activeContextIndex || owner.serviceFlags[i]) continue;
    const auto* record = trading_core::strategy::canonicalOrderEvidence(view, identity.clientIntentId, identity.generation);
    if (!record || record->terminal()) continue;
    if (count >= capacity) break;
    auto state = owner.localStates[i];
    state.cancelPending = record->cancelStatus == trading_core::execution::CancelStatus::Pending;
    state.state = state.cancelPending ? trading_core::StrategyOrderLocalState::CancelPending :
        record->venueObserved ? trading_core::StrategyOrderLocalState::LiveLocal : trading_core::StrategyOrderLocalState::PendingNew;
    out[count++] = state;
  }
  return count;
}

std::int64_t delayedTs(std::int64_t tsNs, std::uint64_t latencyUs) noexcept {
  const std::int64_t add = latencyNs(latencyUs);
  if (tsNs > std::numeric_limits<std::int64_t>::max() - add) return std::numeric_limits<std::int64_t>::max();
  return tsNs + add;
}

bool fillOrderQty(EngineState& engine,
                  OpenOrder& order,
                  std::int64_t priceE8,
                  std::int64_t requestedQty,
                  std::int64_t tsNs,
                  bool taker,
                  const FillExecutionMetadata& metadata) {
  if (engine.liquidated || engine.riskStopped) return false;
  if (!order.state.valid || !order.active || order.state.qty.raw <= 0 || priceE8 <= 0 ||
      (engine.capturedSourceManaged && (!engine.capturedSourceMember || (order.sourceGeneration != engine.capturedSourceGeneration || order.executionBoundaryEpoch != engine.executionBoundaryEpoch)))) return false;
  const std::int64_t fillQty = fillableOrderQty(engine, order, requestedQty);
  if (fillQty <= 0) return rejectOrder(engine, order, tsNs);
  const std::int64_t feeBpsE8 = taker ? engine.takerFeeBpsE8 : engine.makerFeeBpsE8;
  const std::int64_t closeQtyBefore = reduceOnlyPositionCloseQty(engine, order);
  if (!spotVenueFillAffordable(engine, order, priceE8, fillQty, feeBpsE8)) return rejectOrder(engine, order, tsNs);
  const trading_core::FillPositionAccountingResult accounting = updatePosition(
      engine, order.state.side, priceE8, fillQty, tsNs, feeBpsE8);
  if (!accounting.applied) return rejectOrder(engine, order, tsNs);
  const std::int64_t feeE8 = accounting.feeE8;
  const std::int64_t netRealized = accounting.netRealizedPnlE8;
  order.filledQtyRaw += fillQty;
  const bool finalFill = order.filledQtyRaw >= order.state.qty.raw ||
                         (order.reduceOnly && closeQtyBefore != std::numeric_limits<std::int64_t>::max() && fillQty >= closeQtyBefore);
  updateSpotVenueReservation(engine, order, finalFill ? 0 : order.state.qty.raw - order.filledQtyRaw);
  const std::uint8_t fillReason = metadata.fillReason != kFillReasonUnknown
                                      ? metadata.fillReason
                                      : (taker ? kFillReasonTop : kFillReasonTrade);
  engine.fills.push_back(JsonFill{engine.legIndex,
                                  order.state.clientIntentId.raw,
                                  order.activeTsNs,
                                  tsNs,
                                  recordSide(order.state.side),
                                  priceE8,
                                  fillQty,
                                  netRealized,
                                  order.reduceOnly,
                                  fillReason,
                                  taker ? kFillLiquidityTaker : kFillLiquidityMaker,
                                  order.state.qty.raw,
                                  order.filledQtyRaw,
                                  order.state.qty.raw > order.filledQtyRaw ? order.state.qty.raw - order.filledQtyRaw : 0,
                                  metadata.avgPriceE8 > 0 ? metadata.avgPriceE8 : priceE8,
                                  metadata.bookLevelQtyE8,
                                  metadata.bookVisibleExecutableQtyE8,
                                  metadata.bookConsumedPctE8,
                                  metadata.queueAheadBeforeE8,
                                  metadata.queueAheadAfterE8,
                                  metadata.chunkIndex,
                                  metadata.chunkCount,
                                  metadata.executionQtyE8,
                                  metadata.executionAvgPriceE8,
                                  metadata.referencePriceE8,
                                  metadata.slippageE8,
                                  metadata.slippageBpsE8,
                                  metadata.executionBookConsumedPctE8});
  engine.fills.back().contextIndex = order.contextIndex;
  appendEquityPoint(engine, tsNs);
  appendRiskSnapshot(engine, tsNs, kRiskSnapshotReasonFill);
  if (order.reduceOnly) ++engine.reduceOnlyFills;
  const bool serviceFill = serviceOrder(order);
  pushUserEvent(engine, makeFillEvent(engine, order, priceE8, fillQty, order.filledQtyRaw, tsNs, feeE8, netRealized, taker, finalFill),
      tsNs, order.state.clientIntentId, order.contextIndex, serviceFill);
  if (order.warmup) {
    engine.serviceExposureOpen = true;
    const Side unwindSide = oppositeSide(order.state.side);
    std::int64_t unwindPrice{};
    {
      const auto& venueTop = effectiveBookTicker(engine.venueSnapshot);
      const auto& opposite = unwindSide.raw == Side::Sell().raw ? venueTop.bid : venueTop.ask;
      unwindPrice = opposite.px.raw;
      if (unwindPrice <= 0 || opposite.qty.raw < fillQty) {
        engine.simulationEvidenceFailed = true;
        if (finalFill) {
          if (order.recordIndex < engine.orders.size()) engine.orders[order.recordIndex].status = kRecordStatusFilled;
          order = OpenOrder{};
        }
        bumpContextRevision(engine);
        return true;
      }
    }
    const auto savedContext = engine.activeContextIndex;
    engine.activeContextIndex = order.contextIndex;
    trading_core::OrderIntent unwindIntent;
    unwindIntent.clientIntentId = strategy_runtime_detail::allocateBacktestServiceClientIntentId(engine);
    unwindIntent.orderType = canon::OrderType::Market;
    unwindIntent.side = unwindSide; unwindIntent.qty.raw = fillQty; unwindIntent.service = true;
    if (!unwindIntent.clientIntentId.raw || !installSimulationOrderIntent(engine, unwindIntent) ||
        !markSimulationWarmupIntent(engine, unwindIntent.clientIntentId, true) ||
        !settleSimulationOrderSubmit(engine, unwindIntent.clientIntentId, true)) {
      engine.activeContextIndex = savedContext;
      engine.simulationEvidenceFailed = true;
      if (finalFill) {
        if (order.recordIndex < engine.orders.size()) engine.orders[order.recordIndex].status = kRecordStatusFilled;
        order = OpenOrder{};
      }
      return true;
    }
    engine.activeContextIndex = savedContext;
    OpenOrder unwind;
    unwind.contextIndex = order.contextIndex; unwind.state.clientIntentId = unwindIntent.clientIntentId;
    unwind.state.orderType = canon::OrderType::Market; unwind.state.side = unwindSide;
    unwind.state.qty.raw = fillQty; unwind.reduceOnly = unwind.service = unwind.serviceAutounwind = true;
    if (!reserveSpotVenueOrder(engine, unwind) ||
        !spotVenueFillAffordable(engine, unwind, unwindPrice, fillQty, engine.takerFeeBpsE8)) {
      updateSpotVenueReservation(engine, unwind, 0);
      pushUserEvent(engine, makeRejectedOrderEvent(engine, unwind, tsNs), tsNs,
          unwind.state.clientIntentId, unwind.contextIndex, true);
      engine.simulationEvidenceFailed = true;
      if (finalFill) {
        if (order.recordIndex < engine.orders.size()) engine.orders[order.recordIndex].status = kRecordStatusFilled;
        order = OpenOrder{};
      }
      return true;
    }
    auto unwindAck = makeCanceledOrderEvent(engine, unwind, tsNs);
    unwindAck.orderStatus = canon::OrderStatus::Open;
    pushUserEvent(engine, unwindAck, tsNs, unwind.state.clientIntentId, unwind.contextIndex, true);
    const trading_core::FillPositionAccountingResult unwindAccounting =
        updatePosition(engine,
                       unwindSide,
                       unwindPrice,
                       fillQty,
                       tsNs,
                       engine.takerFeeBpsE8);
    if (!unwindAccounting.applied) {
      updateSpotVenueReservation(engine, unwind, 0);
      pushUserEvent(engine, makeRejectedOrderEvent(engine, unwind, tsNs), tsNs,
          unwind.state.clientIntentId, unwind.contextIndex, true);
      engine.simulationEvidenceFailed = true;
      if (finalFill) {
        if (order.recordIndex < engine.orders.size()) engine.orders[order.recordIndex].status = kRecordStatusFilled;
        order = OpenOrder{};
      }
      return true;
    }
    updateSpotVenueReservation(engine, unwind, 0);
    const std::int64_t unwindRealized = unwindAccounting.netRealizedPnlE8;
    JsonOrder unwindOrder{};
    unwindOrder.legIndex = engine.legIndex;
    unwindOrder.id = unwindIntent.clientIntentId.raw;
    unwindOrder.contextIndex = order.contextIndex;
    unwindOrder.submitTsNs = tsNs;
    unwindOrder.activeTsNs = tsNs;
    unwindOrder.tsNs = tsNs;
    unwindOrder.action = kRecordActionOrder;
    unwindOrder.side = recordSide(unwindSide);
    unwindOrder.type = kRecordTypeMarket;
    unwindOrder.status = kRecordStatusFilled;
    unwindOrder.priceE8 = unwindPrice;
    unwindOrder.qtyE8 = fillQty;
    unwindOrder.reduceOnly = true;
    unwindOrder.service = true;
    unwindOrder.serviceAutounwind = true;
    engine.orders.push_back(unwindOrder);
    JsonFill unwindFill{};
    unwindFill.legIndex = engine.legIndex;
    unwindFill.orderId = unwindOrder.id;
    unwindFill.contextIndex = order.contextIndex;
    unwindFill.entryTsNs = tsNs;
    unwindFill.exitTsNs = tsNs;
    unwindFill.side = recordSide(unwindSide);
    unwindFill.priceE8 = unwindPrice;
    unwindFill.qtyE8 = fillQty;
    unwindFill.realizedPnlE8 = unwindRealized;
    unwindFill.reduceOnly = true;
    unwindFill.fillReason = kFillReasonTop;
    unwindFill.liquidity = kFillLiquidityTaker;
    unwindFill.orderQtyE8 = fillQty;
    unwindFill.cumulativeFilledQtyE8 = fillQty;
    unwindFill.avgPriceE8 = unwindPrice;
    engine.fills.push_back(unwindFill);
    pushUserEvent(engine, makeFillEvent(engine, unwind, unwindPrice, fillQty, fillQty, tsNs,
        unwindAccounting.feeE8, unwindRealized, true, true), tsNs,
        unwind.state.clientIntentId, unwind.contextIndex, true);
    appendEquityPoint(engine, tsNs);
    appendRiskSnapshot(engine, tsNs, kRiskSnapshotReasonFill);
    ++engine.reduceOnlyFills;
    engine.serviceExposureOpen = false;
  }
  if (finalFill) {
      if (order.serviceAutounwind) engine.serviceExposureOpen = engine.netQtyRaw != 0;
    if (order.recordIndex < engine.orders.size()) engine.orders[order.recordIndex].status = kRecordStatusFilled;
    order = OpenOrder{};
  } else if (order.recordIndex < engine.orders.size()) {
    engine.orders[order.recordIndex].status = kRecordStatusActive;
  }
  bumpContextRevision(engine);
  return true;
}

bool fillOrder(EngineState& engine,
               OpenOrder& order,
               std::int64_t priceE8,
               std::int64_t tsNs,
               bool taker,
               const FillExecutionMetadata& metadata) {
  return fillOrderQty(engine, order, priceE8, openOrderRemainingQty(order), tsNs, taker, metadata);
}

bool fillOrder(EngineState& engine, OpenOrder& order, std::int64_t priceE8, std::int64_t tsNs, bool taker) {
  FillExecutionMetadata metadata{};
  metadata.fillReason = taker ? kFillReasonTop : kFillReasonTrade;
  return fillOrder(engine, order, priceE8, tsNs, taker, metadata);
}

void markStopMarketTriggered(OpenOrder& order, std::int64_t sourcePrice, std::int64_t tsNs) noexcept {
  order.stopTriggered = true;
  order.triggerSourcePriceRaw = sourcePrice;
  order.triggerTsNs = tsNs;
}

void updateTriggeredStopMarketRecord(EngineState& engine,
                                     const OpenOrder& order,
                                     std::int64_t sourcePrice,
                                     std::int64_t tsNs) noexcept {
  if (order.recordIndex >= engine.orders.size()) return;
  JsonOrder& record = engine.orders[order.recordIndex];
  record.triggerSourcePriceE8 = sourcePrice;
  record.triggerTsNs = tsNs;
  record.status = kRecordStatusActive;
}

void triggerStopMarketsOnLastBbo(EngineState& engine,
                                 std::int64_t bid,
                                 std::int64_t ask,
                                 std::int64_t tsNs) {
  if (bid <= 0 || ask <= 0) return;
  for (OpenOrder& order : engine.open) {
    if (engine.capturedSourceManaged && (!engine.capturedSourceMember || (order.sourceGeneration != engine.capturedSourceGeneration || order.executionBoundaryEpoch != engine.executionBoundaryEpoch))) continue;
    if (!order.state.valid || !order.active || order.stopTriggered) continue;
    if (order.state.orderType != canon::OrderType::Stop ||
        order.state.triggerSource != canon::OrderTriggerSource::Last) continue;
    const std::int64_t sourcePrice = order.state.side.raw == Side::Buy().raw ? ask : bid;
    if (!stopMarketCrossed(order, sourcePrice)) continue;
    markStopMarketTriggered(order, sourcePrice, tsNs);
    updateTriggeredStopMarketRecord(engine, order, sourcePrice, tsNs);
  }
}

void tryFillOnTop(EngineState& engine,
                  std::int64_t tsNs,
                  bool venueBookTickerUpdate) {
  if (engine.liquidated || engine.riskStopped) return;
  const cxet::composite::BookTicker& top = effectiveBookTicker(engine.venueSnapshot);
  const std::int64_t bid = top.bid.px.raw;
  const std::int64_t ask = top.ask.px.raw;
  triggerStopMarketsOnLastBbo(engine, bid, ask, tsNs);
  const bool depthExecutionConfigured = venueOrderBookExecutionConfigured(engine);
  const trading_core::LocalOrderBookState* rawBook =
      depthExecutionConfigured && hasTopExecutionCandidate(engine, bid, ask)
          ? executionDepthBook(engine, tsNs)
          : nullptr;
  const trading_core::LocalOrderBookState* fillBook = rawBook;
  if (rawBook != nullptr && bboOverlayCandidate(top)) {
    trading_core::LocalOrderBookState& overlayBook =
        topExecutionOverlayScratch(engine, *rawBook);
    fillBook = &overlayedTakerExecutionBook(engine.venueSnapshot, top, *rawBook, overlayBook);
  }
  for (OpenOrder& order : engine.open) {
    if (engine.capturedSourceManaged && (!engine.capturedSourceMember || (order.sourceGeneration != engine.capturedSourceGeneration || order.executionBoundaryEpoch != engine.executionBoundaryEpoch))) continue;
    if (!order.state.valid || !order.active) continue;
    if (order.state.orderType == canon::OrderType::Stop && !order.stopTriggered) continue;
    AggressiveFillPlan* plan = fillBook != nullptr ? &topExecutionFillPlanScratch(engine) : nullptr;
    if (plan != nullptr && buildAggressiveBookFillPlan(engine, order, *fillBook, *plan)) {
      const bool marketOrder = marketExecutionOrder(order);
      const bool cappedTriggeredStop = triggeredStopLimitOrder(order);
      if (cappedTriggeredStop || !marketOrder || marketDepthPlanUsable(order, *plan, *rawBook, top)) {
        if (triggeredStopFokLimitOrder(order) && plan->filledQtyE8 < openOrderRemainingQty(order)) {
          terminalizeUnavailableLiquidity(engine, order, tsNs);
          continue;
        }
        for (std::size_t i = 0u; i < plan->count && order.state.valid; ++i) {
          const AggressiveFillChunk& chunk = plan->chunks[i];
          FillExecutionMetadata metadata{};
          metadata.fillReason = kFillReasonAggressiveBook;
          metadata.avgPriceE8 = chunk.avgPriceE8;
          metadata.bookLevelQtyE8 = chunk.bookLevelQtyE8;
          metadata.bookVisibleExecutableQtyE8 = chunk.bookVisibleExecutableQtyE8;
          metadata.bookConsumedPctE8 = chunk.bookConsumedPctE8;
          metadata.chunkIndex = chunk.chunkIndex;
          metadata.chunkCount = chunk.chunkCount;
          metadata.executionQtyE8 = chunk.executionQtyE8;
          metadata.executionAvgPriceE8 = chunk.executionAvgPriceE8;
          metadata.referencePriceE8 = chunk.referencePriceE8;
          metadata.slippageE8 = chunk.slippageE8;
          metadata.slippageBpsE8 = chunk.slippageBpsE8;
          metadata.executionBookConsumedPctE8 = chunk.executionBookConsumedPctE8;
          (void)fillOrderQty(engine, order, chunk.priceE8, chunk.qtyE8, tsNs, true, metadata);
        }
        if (order.state.valid && order.state.orderType == canon::OrderType::Limit) {
          order.lastAggressiveBookTsNs = plan->bookTsNs != 0 ? plan->bookTsNs : tsNs;
        }
        if (order.state.valid && marketOrder && plan->visibleDepthCapped) {
          if (order.reduceOnly && reduceOnlyPositionCloseQty(engine, order) <= 0) {
            (void)rejectOrder(engine, order, tsNs);
          } else {
            terminalizeUnavailableLiquidity(engine, order, tsNs);
          }
        }
        continue;
      }
    }
    if (venueBookTickerUpdate &&
        order.state.orderType == canon::OrderType::Limit) {
      const PassiveBboTouchFill fill = consumePassiveQueueForBboTouch(engine, order, top, tsNs);
      if (fill.requestedQtyE8 > 0) {
        FillExecutionMetadata metadata{};
        metadata.fillReason = kFillReasonPassiveBboTouch;
        metadata.queueAheadBeforeE8 = fill.queueAheadBeforeE8;
        metadata.queueAheadAfterE8 = fill.queueAheadAfterE8;
        (void)fillOrderQty(engine, order, order.state.price.raw, fill.requestedQtyE8, tsNs, false, metadata);
        continue;
      }
    }
    if (order.state.orderType == canon::OrderType::Limit &&
        depthExecutionConfigured &&
        (rawBook != nullptr || !needsTopExecutionBook(order, bid, ask))) {
      continue;
    }
    if (bid <= 0 || ask <= 0) continue;
    const std::size_t fillsBeforeBbo = engine.fills.size();
    FillExecutionMetadata metadata{};
    metadata.fillReason = kFillReasonTop;
    if (triggeredStopLimitOrder(order)) {
      fillTriggeredStopLimitFromBbo(engine, order, top, tsNs, metadata);
    } else if (marketExecutionOrder(order)) {
      (void)fillOrder(engine, order, order.state.side.raw == Side::Buy().raw ? ask : bid, tsNs, true, metadata);
    } else if (!order.postOnly && order.state.side.raw == Side::Buy().raw && order.state.price.raw >= ask) {
      (void)fillOrder(engine, order, ask, tsNs, true, metadata);
    } else if (!order.postOnly && order.state.side.raw == Side::Sell().raw && order.state.price.raw <= bid) {
      (void)fillOrder(engine, order, bid, tsNs, true, metadata);
    }
    if (engine.depthExecutionMode == BacktestDepthExecutionMode::BboFallback &&
        engine.fills.size() > fillsBeforeBbo) {
      engine.depthFallbackFills += engine.fills.size() - fillsBeforeBbo;
      if (engine.firstDepthFallbackTsNs == 0) engine.firstDepthFallbackTsNs = tsNs;
    }
  }
}

void tryFillOnTrade(EngineState& engine,
                    const cxet::composite::Trade& trade,
                    std::int64_t venueTsNs) {
  if (engine.liquidated || engine.riskStopped) return;
  for (OpenOrder& order : engine.open) {
    if (engine.capturedSourceManaged && (!engine.capturedSourceMember || (order.sourceGeneration != engine.capturedSourceGeneration || order.executionBoundaryEpoch != engine.executionBoundaryEpoch))) continue;
    if (!order.state.valid || !order.active || order.state.orderType == canon::OrderType::Market ||
        order.state.orderType == canon::OrderType::Stop) continue;
    const PassiveQueueTradeFill fill = consumePassiveQueueForTrade(engine, order, trade);
    if (fill.requestedQtyE8 <= 0) continue;
    FillExecutionMetadata metadata{};
    metadata.fillReason = kFillReasonPassiveQueue;
    metadata.queueAheadBeforeE8 = fill.queueAheadBeforeE8;
    metadata.queueAheadAfterE8 = fill.queueAheadAfterE8;
    (void)fillOrderQty(engine, order, order.state.price.raw,
                       fill.requestedQtyE8, venueTsNs, false, metadata);
  }
}



}  // namespace hft_backtest::detail
