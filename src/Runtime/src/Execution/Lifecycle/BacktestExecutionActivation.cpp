#include "capital/Accounting/FillPositionAccounting.hpp"
#include "../../Accounting/BacktestPositionQuantity.hpp"
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
#include "trading_core/Strategy/Orders/Evidence/OrderDecisionCursor.hpp"

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
  const std::int64_t remaining = openOrderRemainingQty(order);
  if (!triggeredStopLimitBboCapOk(order, topPrice) || remaining <= 0 ||
      (triggeredStopFokLimitOrder(order) && fillableOrderQty(engine, order, remaining) != remaining)) {
    terminalizeUnavailableLiquidity(engine, order, tsNs);
    return;
  }
  // Deliberately optimistic BBO fallback: price cap/TIF still apply, quantity
  // is unbounded. Finite FOK liquidity is checked only on the Depth route.
  (void)fillOrderQty(engine, order, topPrice, remaining, tsNs, true, metadata);
  if (engine.simulationEvidenceFailed) return;
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
  return engine.venueOrderBookExecutionEnabled &&
      (trading_core::orderBook(engine.venueOrderBook).initialized() || engine.venueDepthBook.view().ready)
      ? &engine.venueOrderBook
      : nullptr;
}

bool venueOrderBookExecutionConfigured(const EngineState& engine) noexcept {
  return engine.executionDepthProvider != nullptr ||
         (engine.venueOrderBookExecutionEnabled &&
          (trading_core::orderBook(engine.venueOrderBook).initialized() || engine.venueDepthBook.view().ready));
}

bool needsTopExecutionBook(const OpenOrder& order, std::int64_t bid, std::int64_t ask) noexcept {
  if (!order.state.valid || !order.active) return false;
  if (order.state.orderType == canon::OrderType::Market) return true;
  if (order.state.orderType == canon::OrderType::Stop) return order.stopTriggered;
  if (order.state.orderType != canon::OrderType::Limit) return false;
  if (order.postOnly || order.aggressiveEvaluated) return false;
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

bool marketDepthFreshEnough(const trading_core::LocalOrderBookState& book,
                            const cxet::composite::BookTicker& top) noexcept {
  if (book.ts.raw == 0u || top.ts.raw == 0u) return true;
  if (top.ts.raw <= book.ts.raw) return true;
  return top.ts.raw - book.ts.raw <= static_cast<std::uint64_t>(kMarketDepthMaxAgeNs);
}

AggressiveFillPlan& topExecutionFillPlanScratch(EngineState& engine) {
  if (engine.topExecutionFillPlanScratch.empty()) engine.topExecutionFillPlanScratch.resize(1u);
  return engine.topExecutionFillPlanScratch.front();
}

bool fokPlanExecutable(EngineState& engine, const OpenOrder& order,
                       const AggressiveFillPlan& plan) noexcept {
  const auto requested = openOrderRemainingQty(order);
  if (plan.filledQtyE8 != requested || fillableOrderQty(engine, order, requested) != requested ||
      !spotVenueFillPlanAffordable(engine, order, plan, engine.takerFeeBpsE8)) return false;
  const auto side = order.state.side.raw == Side::Buy().raw
      ? trading_core::BookSide::Ask : trading_core::BookSide::Bid;
  const auto& liquidity = depthLiquidity(engine);
  std::uint32_t newPrices = 0u;
  capital::accounting::FillPositionAccountingState position{engine.netQtyRaw, engine.entryPriceE8};
  for (std::size_t i = 0u; i < plan.count; ++i) {
    const auto& chunk = plan.chunks[i];
    if (!liquidity.canConsume(side, chunk.priceE8, chunk.qtyE8)) return false;
    // Book plans contain each side/price once. Preflight all new ledger slots
    // together so a later chunk cannot run out of storage after an early fill.
    if (liquidity.available(side, chunk.priceE8, std::numeric_limits<std::int64_t>::max()) ==
        std::numeric_limits<std::int64_t>::max()) ++newPrices;
    if (fillPositionQuantityOverflows(position.netQtyRaw, order.state.side, chunk.qtyE8)) {
      engine.simulationEvidenceFailed = true;
      return false;
    }
    const auto accounting = capital::accounting::applyFillPositionAccounting(
        position, order.state.side, chunk.priceE8, chunk.qtyE8, engine.takerFeeBpsE8);
    if (!accounting.applied) return false;
    position = accounting.position;
  }
  return newPrices <= liquidity.remainingCapacity(side);
}

void activateOrder(EngineState& engine, OpenOrder& order, std::int64_t activeTsNs) {
  if (engine.nextActivationSequence == std::numeric_limits<std::uint64_t>::max()) {
    engine.simulationEvidenceFailed = true;
    (void)rejectOrder(engine, order, activeTsNs);
    return;
  }
  order.activationSequence = engine.nextActivationSequence++;
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
    } else initializePassiveQueue(order, engine);
  }
  tryFillOnTop(engine, activeTsNs, false);
}

}  // namespace hft_backtest::detail::execution_detail

namespace hft_backtest::detail {
using execution_detail::executionDepthBook;
using execution_detail::fillTriggeredStopLimitFromBbo;
using execution_detail::fillableOrderQty;
using execution_detail::hasTopExecutionCandidate;
using execution_detail::latencyNs;
using execution_detail::makeFillEvent;
using execution_detail::marketExecutionOrder;
using execution_detail::needsTopExecutionBook;
using execution_detail::oppositeSide;
using execution_detail::pushUserEvent;
using execution_detail::recordSide;
using execution_detail::reduceOnlyPositionCloseQty;
using execution_detail::rejectOrder;
using execution_detail::serviceOrder;
using execution_detail::stopMarketCrossed;
using execution_detail::terminalizeUnavailableLiquidity;
using execution_detail::topExecutionFillPlanScratch;
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
  if (engine.liquidated || engine.riskStopped || engine.simulationEvidenceFailed ||
      !order.state.valid || !order.active || order.state.qty.raw <= 0 || priceE8 <= 0 ||
      (engine.capturedSourceManaged && (!engine.capturedSourceMember ||
       order.sourceGeneration != engine.capturedSourceGeneration ||
       order.executionBoundaryEpoch != engine.executionBoundaryEpoch))) return false;
  const auto fillQty = fillableOrderQty(engine, order, requestedQty);
  if (fillQty <= 0 || !engine.feePolicyKnown) return rejectOrder(engine, order, tsNs);
  const auto actualFee = capital::accounting::fillFeeE8(priceE8, fillQty,
      taker ? engine.takerFeeBpsE8 : engine.makerFeeBpsE8);
  if (!spotVenueFillAffordable(engine, order, priceE8, fillQty, actualFee)) return rejectOrder(engine, order, tsNs);
  return applyAcceptedOrderFill(engine, order, priceE8, fillQty, actualFee, tsNs, taker, metadata);
}

bool applyAcceptedOrderFill(EngineState& engine,
                  OpenOrder& order,
                  std::int64_t priceE8,
                  std::int64_t fillQty,
                  std::int64_t actualFeeE8,
                  std::int64_t tsNs,
                  bool taker,
                  const FillExecutionMetadata& metadata) {
  if(engine.streamingOutput && (engine.fills.size()+2u>kMaxOrderStates*16u || engine.orders.size()+2u>kMaxOrderStates*8u)) {
    engine.simulationEvidenceFailed=true;return false;
  }

  if (engine.simulationEvidenceFailed || !knownAccountingModel(engine) || !order.state.valid || !order.active ||
      order.state.qty.raw <= 0 || priceE8 <= 0 || fillQty <= 0 ||
      actualFeeE8 < 0 ||
      order.filledQtyRaw < 0 || order.filledQtyRaw > order.state.qty.raw ||
      fillQty > order.state.qty.raw - order.filledQtyRaw) return false;
  const auto liquiditySide = order.state.side.raw == Side::Buy().raw
      ? trading_core::BookSide::Ask : trading_core::BookSide::Bid;
  const bool depthFill = taker && metadata.fillReason == kFillReasonAggressiveBook;
  if (depthFill && !depthLiquidity(engine).canConsume(liquiditySide, priceE8, fillQty)) {
    engine.simulationEvidenceFailed = true;
    return false;
  }
  const std::int64_t closeQtyBefore = reduceOnlyPositionCloseQty(engine, order);
  const capital::accounting::FillPositionAccountingResult accounting = updatePositionActualFee(
      engine, order.state.side, priceE8, fillQty, tsNs, actualFeeE8);
  if (!accounting.applied) { engine.simulationEvidenceFailed = true; return false; }
  // Single-owner commit: canConsume was checked before accounting and no
  // other execution can mutate this ledger between preflight and commit.
  if (depthFill && !depthLiquidity(engine).consume(liquiditySide, priceE8, fillQty)) {
    engine.simulationEvidenceFailed = true;
    return false;
  }
  const std::int64_t feeE8 = accounting.feeE8;
  const std::int64_t netRealized = accounting.netRealizedPnlE8;
  order.filledQtyRaw += fillQty;
  // Positive price/quantity and cumulative quantity <= int64 keep this sum
  // below INT64_MAX squared, within signed 128-bit range.
  order.filledNotionalRaw += static_cast<__int128>(priceE8) * fillQty;
  const auto cumulativeAvgPrice = static_cast<std::int64_t>(order.filledNotionalRaw / order.filledQtyRaw);
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
                                  cumulativeAvgPrice,
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
  if (fillReason == kFillReasonTop) {
    ++engine.depthFallbackFills;
    if (engine.firstDepthFallbackTsNs == 0) engine.firstDepthFallbackTsNs = tsNs;
  }
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
    const auto unwindBookSide = unwindSide.raw == Side::Buy().raw
        ? trading_core::BookSide::Ask : trading_core::BookSide::Bid;
    bool unwindDepthFill = false;
    {
      const auto& venueTop = effectiveBookTicker(engine.venueSnapshot);
      const auto& opposite = unwindSide.raw == Side::Sell().raw ? venueTop.bid : venueTop.ask;
      unwindPrice = opposite.px.raw;
      std::int64_t available = opposite.qty.raw;
      const auto* unwindBook = executionDepthBook(engine,
          metadata.depthCursorTsNs > 0 ? metadata.depthCursorTsNs : tsNs);
      unwindDepthFill = unwindBook && execution_detail::marketDepthFreshEnough(*unwindBook, venueTop);
      if (unwindDepthFill) {
        available = 0;
        const auto view = trading_core::orderBook(*unwindBook);
        const auto* side = view.side(unwindBookSide);
        for (std::uint32_t i = 0u; side && i < side->activeCount.raw; ++i) {
          trading_core::LocalOrderBookLevel level{};
          if (!view.readLevel(unwindBookSide, Index{i}, level)) break;
          if (level.px.raw == unwindPrice) {
            available = depthLiquidity(engine).available(unwindBookSide, unwindPrice, level.qty.raw);
            break;
          }
        }
      }
      if (!engine.feePolicyKnown || unwindPrice <= 0 || opposite.qty.raw < fillQty || available < fillQty ||
          (unwindDepthFill && !depthLiquidity(engine).canConsume(unwindBookSide, unwindPrice, fillQty))) {
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
        !markSimulationWarmupIntent(engine, unwindIntent.clientIntentId, true)) {
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
    const auto unwindActualFee = capital::accounting::fillFeeE8(unwindPrice, fillQty, engine.takerFeeBpsE8);
    if (!reserveSpotVenueOrder(engine, unwind) ||
        !spotVenueFillAffordable(engine, unwind, unwindPrice, fillQty, unwindActualFee)) {
      updateSpotVenueReservation(engine, unwind, 0);
      engine.activeContextIndex = unwind.contextIndex;
      if (!settleSimulationOrderSubmit(engine, unwind.state.clientIntentId, false)) engine.simulationEvidenceFailed = true;
      engine.activeContextIndex = savedContext;
      pushUserEvent(engine, execution_detail::makeRejectedOrderEvent(engine, unwind, tsNs), tsNs,
          unwind.state.clientIntentId, unwind.contextIndex, true);
      engine.simulationEvidenceFailed = true;
      if (finalFill) {
        if (order.recordIndex < engine.orders.size()) engine.orders[order.recordIndex].status = kRecordStatusFilled;
        order = OpenOrder{};
      }
      return true;
    }
    engine.activeContextIndex = unwind.contextIndex;
    const bool unwindSent = settleSimulationOrderSubmit(engine, unwind.state.clientIntentId, true);
    engine.activeContextIndex = savedContext;
    if (!unwindSent) { engine.simulationEvidenceFailed = true; return true; }
    auto unwindAck = execution_detail::makeCanceledOrderEvent(engine, unwind, tsNs);
    unwindAck.orderStatus = canon::OrderStatus::Open;
    pushUserEvent(engine, unwindAck, tsNs, unwind.state.clientIntentId, unwind.contextIndex, true);
    const capital::accounting::FillPositionAccountingResult unwindAccounting =
        updatePositionActualFee(engine,
                       unwindSide,
                       unwindPrice,
                       fillQty,
                       tsNs,
                       unwindActualFee);
    if (!unwindAccounting.applied) {
      updateSpotVenueReservation(engine, unwind, 0);
      pushUserEvent(engine, execution_detail::makeRejectedOrderEvent(engine, unwind, tsNs), tsNs,
          unwind.state.clientIntentId, unwind.contextIndex, true);
      engine.simulationEvidenceFailed = true;
      if (finalFill) {
        if (order.recordIndex < engine.orders.size()) engine.orders[order.recordIndex].status = kRecordStatusFilled;
        order = OpenOrder{};
      }
      return true;
    }
    if (unwindDepthFill && !depthLiquidity(engine).consume(unwindBookSide, unwindPrice, fillQty)) {
      engine.simulationEvidenceFailed = true;
      return true;
    }
    unwind.filledQtyRaw = fillQty;
    unwind.filledNotionalRaw = static_cast<__int128>(unwindPrice) * fillQty;
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
    unwindFill.fillReason = unwindDepthFill ? kFillReasonAggressiveBook : kFillReasonTop;
    unwindFill.liquidity = kFillLiquidityTaker;
    unwindFill.orderQtyE8 = fillQty;
    unwindFill.cumulativeFilledQtyE8 = fillQty;
    unwindFill.avgPriceE8 = unwindPrice;
    engine.fills.push_back(unwindFill);
    if (!unwindDepthFill) {
      ++engine.depthFallbackFills;
      if (engine.firstDepthFallbackTsNs == 0) engine.firstDepthFallbackTsNs = tsNs;
    }
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
                  bool /*venueBookTickerUpdate*/,
                  std::int64_t depthCursorTsNs) {
  if (engine.liquidated || engine.riskStopped || engine.simulationEvidenceFailed) return;
  const cxet::composite::BookTicker& top = effectiveBookTicker(engine.venueSnapshot);
  const std::int64_t bid = top.bid.px.raw;
  const std::int64_t ask = top.ask.px.raw;
  triggerStopMarketsOnLastBbo(engine, bid, ask, tsNs);
  const bool depthConfigured = venueOrderBookExecutionConfigured(engine);
  const auto cursorTs = depthCursorTsNs > 0 ? depthCursorTsNs : tsNs;
  const trading_core::LocalOrderBookState* rawBook =
      depthConfigured && hasTopExecutionCandidate(engine, bid, ask)
          ? executionDepthBook(engine, cursorTs) : nullptr;
  const trading_core::LocalOrderBookState* fillBook =
      rawBook && execution_detail::marketDepthFreshEnough(*rawBook, top) ? rawBook : nullptr;
  std::array<OpenOrder*, kMaxOrderStates> candidates{};
  std::size_t count = 0u;
  for (OpenOrder& order : engine.open) {
    if (engine.capturedSourceManaged && (!engine.capturedSourceMember ||
        order.sourceGeneration != engine.capturedSourceGeneration ||
        order.executionBoundaryEpoch != engine.executionBoundaryEpoch)) continue;
    if (!order.state.valid || !order.active || order.activeTsNs > tsNs || order.postOnly) continue;
    if (order.state.orderType == canon::OrderType::Stop && !order.stopTriggered) continue;
    if (order.state.orderType == canon::OrderType::Limit && order.aggressiveEvaluated) continue;
    candidates[count++] = &order;
  }
  std::sort(candidates.begin(), candidates.begin() + count,
      [](const OpenOrder* left, const OpenOrder* right) noexcept {
        if (left->activeTsNs != right->activeTsNs) return left->activeTsNs < right->activeTsNs;
        if (left->activationSequence != right->activationSequence)
          return left->activationSequence < right->activationSequence;
        return left->recordIndex < right->recordIndex;
      });
  for (std::size_t candidate = 0u; candidate < count; ++candidate) {
    OpenOrder& order = *candidates[candidate];
    if (!order.state.valid) continue;
    const bool marketOrder = marketExecutionOrder(order);
    const bool iocLimit = order.state.orderType == canon::OrderType::Limit &&
        order.state.limitTimeInForcePolicy == trading_core::LimitTimeInForcePolicy::Ioc;
    if (fillBook) {
      AggressiveFillPlan& plan = topExecutionFillPlanScratch(engine);
      const bool planned = buildAggressiveBookFillPlan(engine, order, *fillBook, plan);
      if (triggeredStopFokLimitOrder(order) &&
          (!planned || !execution_detail::fokPlanExecutable(engine, order, plan))) {
        if (engine.simulationEvidenceFailed) return;
        terminalizeUnavailableLiquidity(engine, order, tsNs);
        continue;
      }
      if (planned) {
        for (std::size_t i = 0u; i < plan.count && order.state.valid; ++i) {
          const AggressiveFillChunk& chunk = plan.chunks[i];
          FillExecutionMetadata metadata{};
          metadata.depthCursorTsNs = cursorTs;
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
          if (engine.simulationEvidenceFailed) return;
        }
      }
      if (order.state.valid && (marketOrder || iocLimit)) {
        terminalizeUnavailableLiquidity(engine, order, tsNs);
      } else if (order.state.valid) {
        order.aggressiveEvaluated = true;
      }
      // A finite book, including an exhausted book, never falls through to
      // unlimited BBO execution merely because it could not fill this order.
      continue;
    }
    if (bid <= 0 || ask <= 0) {
      if (iocLimit || triggeredStopLimitOrder(order)) terminalizeUnavailableLiquidity(engine, order, tsNs);
      continue;
    }
    FillExecutionMetadata metadata{};
    metadata.depthCursorTsNs = cursorTs;
    metadata.fillReason = kFillReasonTop;
    const auto fillsBefore = engine.fills.size();
    if (triggeredStopLimitOrder(order)) {
      fillTriggeredStopLimitFromBbo(engine, order, top, tsNs, metadata);
    } else if (marketOrder) {
      (void)fillOrder(engine, order, order.state.side.raw == Side::Buy().raw ? ask : bid, tsNs, true, metadata);
    } else if (order.state.side.raw == Side::Buy().raw && order.state.price.raw >= ask) {
      (void)fillOrder(engine, order, ask, tsNs, true, metadata);
    } else if (order.state.side.raw == Side::Sell().raw && order.state.price.raw <= bid) {
      (void)fillOrder(engine, order, bid, tsNs, true, metadata);
    }
    if (engine.simulationEvidenceFailed) return;
    if (order.state.valid && iocLimit) terminalizeUnavailableLiquidity(engine, order, tsNs);
    if (order.state.valid && order.state.orderType == canon::OrderType::Limit)
      order.aggressiveEvaluated = true;
    if (engine.fills.size() > fillsBefore && depthConfigured) {
      engine.depthExecutionMode = BacktestDepthExecutionMode::BboFallback;
      if (engine.depthFallbackReason.empty())
        engine.depthFallbackReason = rawBook ? "stale_depth" : "depth_unavailable";
    }
    if (engine.simulationEvidenceFailed) return;
  }
}

}  // namespace hft_backtest::detail
