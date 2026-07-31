#include "engine/backtest_engine.hpp"

#include "execution/backtest_execution_depth.hpp"
#include "execution/backtest_fill_model.hpp"

#include "hft_trader/runtime/order/ClientOrderId.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <utility>

namespace hft_backtest::detail {
namespace {

std::size_t findOpenIndex(EngineState& engine, Id id) noexcept {
  for (std::size_t i = 0; i < engine.open.size(); ++i) {
    const OpenOrder& order = engine.open[i];
    if (order.state.valid && (order.state.clientIntentId.raw == id.raw || order.state.desiredClientIntentId.raw == id.raw || order.state.pendingClientIntentId.raw == id.raw)) return i;
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
  if (!order.state.valid) return false;
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

cxet::composite::UserDataEvent makeFillEvent(EngineState& engine,
                                             const OpenOrder& order,
                                             std::int64_t priceE8,
                                             std::int64_t qtyE8,
                                             std::int64_t cumulativeQtyE8,
                                             std::int64_t tsNs,
                                             std::int64_t feeE8,
                                             std::int64_t realizedPnlE8,
                                             bool taker,
                                             bool finalFill) noexcept {
  cxet::composite::UserDataEvent event{};
  event.exchangeId = engine.exchange;
  event.kind = cxet::composite::UserDataEventKind::Order;
  event.symbol = engine.symbol;
  (void)hft_trader::runtime::makeRuntimeClientOrderId(engine.exchange, 'o', order.state.clientIntentId, 1u, event.clientOrderId);
  event.orderId = event.clientOrderId;
  event.side = order.state.side;
  event.sideSet = 1u;
  event.orderType = order.state.orderType;
  event.orderTypeSet = 1u;
  event.orderStatus = finalFill ? canon::OrderStatus::Closed : canon::OrderStatus::PartiallyFilled;
  event.orderStatusSet = 1u;
  event.price.raw = priceE8;
  event.avgPrice.raw = priceE8;
  event.lastPrice.raw = priceE8;
  event.qty = order.state.qty;
  event.filledQty.raw = cumulativeQtyE8;
  event.lastFilledQty.raw = qtyE8;
  event.fee.raw = feeE8;
  event.realizedPnl.raw = realizedPnlE8;
  event.eventTime.raw = static_cast<std::uint64_t>(tsNs > 0 ? tsNs : 0);
  event.updateTime = event.eventTime;
  event.reduceOnly = order.reduceOnly ? 1u : 0u;
  event.maker = taker ? 0u : 1u;
  ++engine.fillSequence;
  return event;
}

cxet::composite::UserDataEvent makeRejectedOrderEvent(EngineState& engine,
                                                      const OpenOrder& order,
                                                      std::int64_t tsNs,
                                                      canon::OrderProblem problem = canon::OrderProblem::None) noexcept {
  cxet::composite::UserDataEvent event{};
  event.exchangeId = engine.exchange;
  event.kind = cxet::composite::UserDataEventKind::Order;
  event.symbol = engine.symbol;
  (void)hft_trader::runtime::makeRuntimeClientOrderId(engine.exchange, 'o', order.state.clientIntentId, 1u, event.clientOrderId);
  event.orderId = event.clientOrderId;
  event.side = order.state.side;
  event.sideSet = 1u;
  event.orderType = order.state.orderType;
  event.orderTypeSet = 1u;
  event.orderStatus = canon::OrderStatus::Rejected;
  event.orderProblem = problem;
  event.orderStatusSet = 1u;
  event.qty = order.state.qty;
  event.eventTime.raw = static_cast<std::uint64_t>(tsNs > 0 ? tsNs : 0);
  event.updateTime = event.eventTime;
  event.reduceOnly = order.reduceOnly ? 1u : 0u;
  return event;
}

cxet::composite::UserDataEvent makeCanceledOrderEvent(EngineState& engine,
                                                      const OpenOrder& order,
                                                      std::int64_t tsNs) noexcept {
  cxet::composite::UserDataEvent event{};
  event.exchangeId = engine.exchange;
  event.kind = cxet::composite::UserDataEventKind::Order;
  event.symbol = engine.symbol;
  (void)hft_trader::runtime::makeRuntimeClientOrderId(engine.exchange, 'o', order.state.clientIntentId, 1u, event.clientOrderId);
  event.orderId = event.clientOrderId;
  event.side = order.state.side;
  event.sideSet = 1u;
  event.orderType = order.state.orderType;
  event.orderTypeSet = 1u;
  event.orderStatus = canon::OrderStatus::Canceled;
  event.orderStatusSet = 1u;
  event.price = order.state.price;
  event.qty = order.state.qty;
  event.filledQty.raw = order.filledQtyRaw;
  event.lastFilledQty.raw = 0;
  event.eventTime.raw = static_cast<std::uint64_t>(tsNs > 0 ? tsNs : 0);
  event.updateTime = event.eventTime;
  event.reduceOnly = order.reduceOnly ? 1u : 0u;
  return event;
}

std::int64_t userEventDeliveryTs(EngineState& engine, std::int64_t eventTsNs) noexcept {
  const std::uint64_t latencyUs = sampleLatencyUs(engine.latency, BacktestLatencyChannel::UserData);
  const std::int64_t add = latencyNs(latencyUs);
  if (eventTsNs > std::numeric_limits<std::int64_t>::max() - add) return std::numeric_limits<std::int64_t>::max();
  return eventTsNs + add;
}

void pushUserEvent(EngineState& engine, cxet::composite::UserDataEvent event, std::int64_t eventTsNs) {
  engine.userEvents.push_back(event);
  engine.userEventDeliveryTsNs.push_back(userEventDeliveryTs(engine, eventTsNs));
}

void publishProblem(EngineState& engine,
                    canon::OrderProblem problem,
                    std::uint8_t commandKind,
                    Id clientIntentId,
                    Id targetClientIntentId) noexcept {
  if (problem == canon::OrderProblem::None) return;
  engine.pendingProblem = hft_trader::core::StrategyProblemEvent{};
  engine.pendingProblem.problem = problem;
  engine.pendingProblem.commandKind = commandKind;
  engine.pendingProblem.clientIntentId = clientIntentId;
  engine.pendingProblem.targetClientIntentId = targetClientIntentId;
  engine.pendingProblem.valid = true;
  engine.hasPendingProblem = true;
}

void recordRateLimitReject(EngineState& engine, std::uint8_t commandKind, std::int64_t tsNs) noexcept {
  if (commandKind == 3u) ++engine.rateLimitRejectedCancels;
  else ++engine.rateLimitRejectedOrders;
  if (engine.firstRateLimitRejectTsNs == 0 || tsNs < engine.firstRateLimitRejectTsNs) {
    engine.firstRateLimitRejectTsNs = tsNs;
  }
  if (tsNs > engine.lastRateLimitRejectTsNs) engine.lastRateLimitRejectTsNs = tsNs;
}

std::int64_t rateLimitCooldownTsNs(const hft_trader::core::RateLimitDecision& decision,
                                   std::int64_t tsNs) noexcept {
  const std::uint64_t raw = decision.resetMono.raw;
  std::int64_t out = tsNs < std::numeric_limits<std::int64_t>::max()
      ? tsNs + 1
      : std::numeric_limits<std::int64_t>::max();
  if (raw > 0u && raw <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    out = static_cast<std::int64_t>(raw);
  }
  if (out <= tsNs && tsNs < std::numeric_limits<std::int64_t>::max()) out = tsNs + 1;
  return out;
}

bool beginRateLimitCooldown(EngineState& engine,
                            const hft_trader::core::RateLimitDecision& decision,
                            std::int64_t tsNs) noexcept {
  const std::int64_t cooldownTs = rateLimitCooldownTsNs(decision, tsNs);
  const bool notify = engine.rateLimitProblemUntilTsNs <= tsNs;
  if (cooldownTs > engine.rateLimitCooldownUntilTsNs) engine.rateLimitCooldownUntilTsNs = cooldownTs;
  if (cooldownTs > engine.rateLimitProblemUntilTsNs) engine.rateLimitProblemUntilTsNs = cooldownTs;
  return notify;
}

TimeNs timeNsFromI64Local(std::int64_t value) noexcept {
  TimeNs out{};
  out.raw = static_cast<std::uint64_t>(value > 0 ? value : 0);
  return out;
}

std::int64_t remainingPctE4(std::int64_t remaining, std::int64_t limit) noexcept {
  if (limit <= 0) return 0;
  if (remaining <= 0) return 0;
  if (remaining >= limit) return 1000000ll;
  const __int128 numerator = static_cast<__int128>(remaining) * 1000000ll;
  return static_cast<std::int64_t>(numerator / limit);
}

bool sameRateLimitUsagePoint(const JsonRateLimitUsagePoint& lhs,
                             const JsonRateLimitUsagePoint& rhs) noexcept {
  return lhs.legIndex == rhs.legIndex &&
         lhs.tsNs == rhs.tsNs &&
         lhs.minRemainingPctE4 == rhs.minRemainingPctE4 &&
         lhs.remaining == rhs.remaining &&
         lhs.limit == rhs.limit &&
         lhs.bucketKind == rhs.bucketKind &&
         lhs.intervalNs == rhs.intervalNs &&
         lhs.exhausted == rhs.exhausted;
}

std::int64_t reduceOnlyPositionCloseQty(const EngineState& engine, const OpenOrder& order) noexcept {
  if (!order.reduceOnly) return std::numeric_limits<std::int64_t>::max();
  if (engine.netQtyRaw == 0) return 0;
  if (engine.netQtyRaw > 0) {
    if (order.state.side.raw != Side::Sell().raw) return 0;
    return engine.netQtyRaw;
  }
  const std::int64_t shortQty = -engine.netQtyRaw;
  if (order.state.side.raw != Side::Buy().raw) return 0;
  return shortQty;
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
  if (!serviceOrder(order)) pushUserEvent(engine, makeRejectedOrderEvent(engine, order, tsNs), tsNs);
  if (order.warmup) engine.warmupInFlight = false;
  if (order.serviceAutounwind) engine.serviceExposureOpen = false;
  if (order.recordIndex < engine.orders.size()) engine.orders[order.recordIndex].status = kRecordStatusRejected;
  order = OpenOrder{};
  bumpContextRevision(engine);
  return false;
}

void terminalizeUnavailableLiquidity(EngineState& engine, OpenOrder& order, std::int64_t tsNs) {
  if (!order.state.valid) return;
  if (!serviceOrder(order)) pushUserEvent(engine, makeCanceledOrderEvent(engine, order, tsNs), tsNs);
  if (order.warmup) engine.warmupInFlight = false;
  if (order.serviceAutounwind) engine.serviceExposureOpen = false;
  if (order.recordIndex < engine.orders.size()) engine.orders[order.recordIndex].status = kRecordStatusPartialUnfilled;
  order = OpenOrder{};
  bumpContextRevision(engine);
}

std::int64_t triggeredStopLimitBboPrice(const OpenOrder& order,
                                        const cxet::composite::BookTickerRuntimeV1& top) noexcept {
  return order.state.side.raw == Side::Buy().raw ? top.ask.px.raw : top.bid.px.raw;
}

std::int64_t triggeredStopLimitBboQty(const OpenOrder& order,
                                      const cxet::composite::BookTickerRuntimeV1& top) noexcept {
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
                                   const cxet::composite::BookTickerRuntimeV1& top,
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

bool isPostOnlyPolicy(hft_trader::core::LimitTimeInForcePolicy policy) noexcept {
  return policy == hft_trader::core::LimitTimeInForcePolicy::PostOnlyPrefer ||
         policy == hft_trader::core::LimitTimeInForcePolicy::PostOnlyStrict;
}

bool postOnlyCrossesTop(const EngineState& engine, const OpenOrder& order) noexcept {
  if (!order.postOnly || order.state.orderType != canon::OrderType::Limit) return false;
  const cxet::composite::BookTickerRuntimeV1& top = effectiveBookTicker(engine.venueSnapshot);
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
  return source == canon::OrderTriggerSource::Last ||
         source == canon::OrderTriggerSource::Mark ||
         source == canon::OrderTriggerSource::Index;
}

bool badStopMarketIntent(const hft_trader::core::OrderIntent& intent) noexcept {
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
    const cxet::composite::BookTickerRuntimeV1& top = effectiveBookTicker(engine.venueSnapshot);
    if (order.state.side.raw == Side::Buy().raw) {
      outPrice = top.ask.px.raw;
    } else if (order.state.side.raw == Side::Sell().raw) {
      outPrice = top.bid.px.raw;
    }
    return outPrice > 0;
  }
  if (source == canon::OrderTriggerSource::Mark) {
    if (engine.venueSnapshot.hasMarkPrice.raw != hft_trader::core::kAlgoFlagTrue.raw) return false;
    outPrice = engine.venueSnapshot.lastMarkPrice.markPrice.raw;
    return outPrice > 0;
  }
  if (source == canon::OrderTriggerSource::Index) {
    if (engine.venueSnapshot.hasIndexPrice.raw != hft_trader::core::kAlgoFlagTrue.raw) return false;
    outPrice = engine.venueSnapshot.lastIndexPrice.indexPrice.raw;
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

const hft_trader::core::LocalOrderBookState* executionDepthBook(EngineState& engine,
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
  return engine.venueOrderBookExecutionEnabled && hft_trader::core::orderBook(engine.venueOrderBook).initialized()
      ? &engine.venueOrderBook
      : nullptr;
}

bool venueOrderBookExecutionConfigured(const EngineState& engine) noexcept {
  return engine.executionDepthProvider != nullptr ||
         (engine.venueOrderBookExecutionEnabled && hft_trader::core::orderBook(engine.venueOrderBook).initialized());
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

constexpr std::int64_t kMarketDepthMaxAgeNs = 500000000ll;

bool bboOverlayCandidate(const cxet::composite::BookTickerRuntimeV1& top) noexcept {
  return top.bid.px.raw > 0 && top.ask.px.raw > 0 && top.bid.px.raw < top.ask.px.raw;
}

hft_trader::core::AlgoFlag effectiveBidQtyKnown(
    const hft_trader::core::StrategyMarketSnapshot& snapshot,
    const cxet::composite::BookTickerRuntimeV1& top) noexcept {
  return snapshot.hasReconstructedBbo.raw == hft_trader::core::kAlgoFlagTrue.raw
      ? snapshot.reconstructedBidQtyKnown
      : (top.bid.qty.raw > 0 ? hft_trader::core::kAlgoFlagTrue : hft_trader::core::kAlgoFlagFalse);
}

hft_trader::core::AlgoFlag effectiveAskQtyKnown(
    const hft_trader::core::StrategyMarketSnapshot& snapshot,
    const cxet::composite::BookTickerRuntimeV1& top) noexcept {
  return snapshot.hasReconstructedBbo.raw == hft_trader::core::kAlgoFlagTrue.raw
      ? snapshot.reconstructedAskQtyKnown
      : (top.ask.qty.raw > 0 ? hft_trader::core::kAlgoFlagTrue : hft_trader::core::kAlgoFlagFalse);
}

const hft_trader::core::LocalOrderBookState& overlayedTakerExecutionBook(
    const hft_trader::core::StrategyMarketSnapshot& snapshot,
    const cxet::composite::BookTickerRuntimeV1& top,
    const hft_trader::core::LocalOrderBookState& rawBook,
    hft_trader::core::LocalOrderBookState& scratch) noexcept {
  if (!bboOverlayCandidate(top)) return rawBook;
  if (!scratch.copyFromBounded(rawBook)) return rawBook;
  const hft_trader::core::AlgoFlag bidQtyKnown = effectiveBidQtyKnown(snapshot, top);
  const hft_trader::core::AlgoFlag askQtyKnown = effectiveAskQtyKnown(snapshot, top);
  return hft_trader::core::applyBboOverlay(scratch, top, bidQtyKnown, askQtyKnown) ? scratch : rawBook;
}

bool marketDepthFreshEnough(const hft_trader::core::LocalOrderBookState& book,
                            const cxet::composite::BookTickerRuntimeV1& top) noexcept {
  if (book.ts.raw == 0u || top.ts.raw == 0u) return true;
  if (top.ts.raw <= book.ts.raw) return true;
  return top.ts.raw - book.ts.raw <= static_cast<std::uint64_t>(kMarketDepthMaxAgeNs);
}

hft_trader::core::LocalOrderBookState& topExecutionOverlayScratch(
    EngineState& engine,
    const hft_trader::core::LocalOrderBookState& source) {
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
                           const hft_trader::core::LocalOrderBookState& book,
                           const cxet::composite::BookTickerRuntimeV1& top) noexcept {
  if (!marketExecutionOrder(order)) return true;
  if (!marketDepthFreshEnough(book, top)) return false;
  if (plan.count == 0u || plan.visibleDepthCapped || plan.remainingQtyE8 != 0) return false;
  return plan.filledQtyE8 == openOrderRemainingQty(order);
}

void activateOrder(EngineState& engine, OpenOrder& order, std::int64_t activeTsNs) {
  order.active = true;
  order.activeTsNs = activeTsNs;
  order.state.state = hft_trader::core::StrategyOrderLocalState::LiveLocal;
  if (order.warmup) {
    engine.lastOrderPathHotMono = timeNsFromI64Local(activeTsNs);
    engine.warmupInFlight = false;
    engine.lastWarmupSkipReason = 0u;
  }
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
  if (order.state.orderType == canon::OrderType::Limit) {
    if (const hft_trader::core::LocalOrderBookState* book = executionDepthBook(engine, activeTsNs); book != nullptr) {
      initializePassiveQueue(order, engine, *book);
    } else if (!order.queueInitialized) {
      order.queueAheadQtyRaw = 0;
      order.queueInitialized = true;
    }
  }
  tryFillOnTop(engine, activeTsNs);
}

bool submitCancelIntent(EngineState& engine,
                        const hft_trader::core::CancelIntent& intent,
                        std::int64_t tsNs,
                        std::int64_t activeTs) {
  const std::size_t slotIndex = findOpenIndex(engine, intent.targetClientIntentId);
  const std::size_t recordIndex = engine.orders.size();
  engine.orders.push_back(JsonOrder{engine.legIndex, intent.clientIntentId.raw, intent.targetClientIntentId.raw, tsNs, activeTs, tsNs, kRecordActionCancel, kRecordSideNone, kRecordTypeNone, kRecordStatusPending, 0, 0, false});
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
  const hft_trader::core::RateLimitDecision rateLimitDecision =
      engine.rateLimits.consumeDecision(hft_trader::core::RateLimitActionKind::CancelOrder, timeNsFromI64Local(tsNs));
  if (!rateLimitDecision.allowed) {
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
  OpenOrder& order = engine.open[slotIndex];
  order.state.cancelPending = true;
  order.state.pendingClientIntentId = intent.clientIntentId;
  order.state.state = hft_trader::core::StrategyOrderLocalState::CancelPending;
  PendingAction action{};
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
                                     const hft_trader::core::OrderIntent& intent,
                                     std::int64_t tsNs,
                                     std::int64_t activeTs,
                                     bool warmup = false,
                                     std::uint32_t warmupCancelTimeoutMs = 0u,
                                     bool warmupCancelAfterAck = false) {
  const bool postOnly = intent.orderType == canon::OrderType::Limit && isPostOnlyPolicy(intent.limitTimeInForcePolicy);
  const bool service = intent.service || warmup;
  const std::size_t recordIndex = engine.orders.size();
  engine.orders.push_back(JsonOrder{engine.legIndex, intent.clientIntentId.raw, 0, tsNs, activeTs, tsNs, kRecordActionOrder, recordSide(intent.side), recordType(intent.orderType), kRecordStatusPending, intent.price.raw, intent.qty.raw, intent.reduceOnly, postOnly});
  engine.orders.back().triggerPriceE8 = intent.triggerPrice.raw;
  engine.orders.back().triggerSource = static_cast<std::uint8_t>(intent.triggerSource);
  engine.orders.back().service = service;
  engine.orders.back().warmup = warmup;
  if (badStopMarketIntent(intent)) {
    OpenOrder rejected{};
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
    rejected.reduceOnly = intent.reduceOnly;
    rejected.postOnly = postOnly;
    rejected.service = service;
    rejected.warmup = warmup;
    if (!serviceOrder(rejected)) pushUserEvent(engine, makeRejectedOrderEvent(engine, rejected, tsNs), tsNs);
    engine.orders[recordIndex].status = kRecordStatusRejected;
    return false;
  }
  const std::size_t slotIndex = findFreeIndex(engine);
  if (slotIndex == kNoRecordIndex) {
    OpenOrder rejected{};
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
    rejected.reduceOnly = intent.reduceOnly;
    rejected.postOnly = postOnly;
    rejected.service = service;
    rejected.warmup = warmup;
    if (!serviceOrder(rejected)) pushUserEvent(engine, makeRejectedOrderEvent(engine, rejected, tsNs), tsNs);
    engine.orders[recordIndex].status = kRecordStatusRejected;
    return false;
  }
  const hft_trader::core::RateLimitDecision rateLimitDecision =
      engine.rateLimits.consumeDecision(hft_trader::core::rateLimitActionForOrderIntent(intent), timeNsFromI64Local(tsNs));
  if (!rateLimitDecision.allowed) {
    recordRateLimitReject(engine, 1u, tsNs);
    const bool notify = beginRateLimitCooldown(engine, rateLimitDecision, tsNs);
    appendRateLimitUsageSnapshot(engine, tsNs);
    OpenOrder rejected{};
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
    rejected.reduceOnly = intent.reduceOnly;
    rejected.postOnly = postOnly;
    rejected.service = service;
    rejected.warmup = warmup;
    if (!serviceOrder(rejected)) {
      pushUserEvent(engine, makeRejectedOrderEvent(engine, rejected, tsNs, canon::OrderProblem::RateLimited), tsNs);
    }
    if (notify) {
      publishProblem(engine, canon::OrderProblem::RateLimited, 1u, intent.clientIntentId, Id{});
    }
    engine.orders[recordIndex].status = kRecordStatusRejected;
    return false;
  }
  appendRateLimitUsageSnapshot(engine, tsNs);
  if (intent.reduceOnly) ++engine.reduceOnlyOrders;
  OpenOrder& slot = engine.open[slotIndex];
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
  slot.state.state = hft_trader::core::StrategyOrderLocalState::PendingNew;
  slot.routeHandle = intent.routeHandle;
  slot.recordIndex = recordIndex;
  slot.createdTsNs = tsNs;
  slot.activeTsNs = activeTs;
  slot.filledQtyRaw = 0;
  slot.queueAheadQtyRaw = 0;
  slot.lastAggressiveBookTsNs = 0;
  slot.lastPassiveBboTouchTsNs = 0;
  slot.active = false;
  slot.reduceOnly = intent.reduceOnly;
  slot.postOnly = postOnly;
  slot.queueInitialized = false;
  slot.stopTriggered = intent.orderType != canon::OrderType::Stop;
  slot.service = service;
  slot.warmup = warmup;
  slot.serviceAutounwind = false;
  slot.warmupCancelAfterAck = warmup && warmupCancelAfterAck;
  slot.warmupCancelRequested = false;
  slot.warmupCancelTimeoutTsNs = warmup && warmupCancelTimeoutMs != 0u ? addMs(activeTs, warmupCancelTimeoutMs) : 0;
  if (warmup) {
    engine.lastOrderPathHotMono = TimeNs{};
    engine.lastWarmupSkipReason = 0u;
    engine.warmupInFlight = true;
  } else if (!service) {
    engine.lastOrderPathHotMono = timeNsFromI64Local(tsNs);
    engine.lastWarmupSkipReason = 0u;
  }
  slot.triggerSourcePriceRaw = 0;
  slot.triggerTsNs = 0;
  PendingAction action{};
  action.kind = PendingActionKind::OrderAck;
  action.slotIndex = slotIndex;
  action.recordIndex = recordIndex;
  action.activeTsNs = activeTs;
  action.targetClientIntentId = intent.clientIntentId;
  action.commandClientIntentId = intent.clientIntentId;
  pushPendingAction(engine, std::move(action));
  return true;
}

}  // namespace

void appendRateLimitUsageSnapshot(EngineState& engine, std::int64_t tsNs) {
  if (!engine.rateLimits.configured()) return;
  const hft_trader::core::StrategyRateLimitBook book =
      engine.rateLimits.snapshotBook(hft_trader::core::kDefaultExecutionRouteHandle,
                                     engine.exchange,
                                     engine.market,
                                     1u,
                                     timeNsFromI64Local(tsNs));
  JsonRateLimitUsagePoint best{};
  bool found = false;
  for (std::uint8_t i = 0u; i < book.count; ++i) {
    const hft_trader::core::RateLimitBucketView& bucket = book.buckets[i];
    if (!bucket.valid || bucket.limit <= 0) continue;
    JsonRateLimitUsagePoint candidate{};
    candidate.legIndex = engine.legIndex;
    candidate.tsNs = tsNs;
    candidate.minRemainingPctE4 = remainingPctE4(bucket.remaining, bucket.limit);
    candidate.remaining = bucket.remaining;
    candidate.limit = bucket.limit;
    candidate.bucketKind = bucket.kind;
    candidate.intervalNs = bucket.intervalNs;
    candidate.exhausted = bucket.exhausted;
    if (!found ||
        candidate.minRemainingPctE4 < best.minRemainingPctE4 ||
        (candidate.minRemainingPctE4 == best.minRemainingPctE4 && candidate.intervalNs < best.intervalNs)) {
      best = candidate;
      found = true;
    }
  }
  if (!found) return;
  if (!engine.rateLimitUsage.empty() && sameRateLimitUsagePoint(engine.rateLimitUsage.back(), best)) return;
  engine.rateLimitUsage.push_back(best);
}

std::size_t visibleOrders(const EngineState& engine,
                          hft_trader::core::StrategyOrderState* out,
                          std::size_t capacity) noexcept {
  if (!out || capacity == 0u) return 0u;
  std::size_t count = 0u;
  for (const OpenOrder& order : engine.open) {
    if (!order.state.valid) continue;
    if (serviceOrder(order)) continue;
    if (count >= capacity) break;
    out[count++] = order.state;
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
  if (!order.state.valid || !order.active || order.state.qty.raw <= 0 || priceE8 <= 0) return false;
  const std::int64_t fillQty = fillableOrderQty(engine, order, requestedQty);
  if (fillQty <= 0) return rejectOrder(engine, order, tsNs);
  const std::int64_t feeBpsE8 = taker ? engine.takerFeeBpsE8 : engine.makerFeeBpsE8;
  const std::int64_t feeE8 = feeForFillE8(priceE8, fillQty, feeBpsE8);
  std::int64_t netRealized = 0;
  const std::int64_t closeQtyBefore = reduceOnlyPositionCloseQty(engine, order);
  updatePosition(engine, order.state.side, priceE8, fillQty, tsNs, feeBpsE8, netRealized);
  order.filledQtyRaw += fillQty;
  const bool finalFill = order.filledQtyRaw >= order.state.qty.raw ||
                         (order.reduceOnly && closeQtyBefore != std::numeric_limits<std::int64_t>::max() && fillQty >= closeQtyBefore);
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
  appendEquityPoint(engine, tsNs);
  appendRiskSnapshot(engine, tsNs, kRiskSnapshotReasonFill);
  if (order.reduceOnly) ++engine.reduceOnlyFills;
  const bool serviceFill = serviceOrder(order);
  if (!serviceFill) {
    pushUserEvent(engine, makeFillEvent(engine, order, priceE8, fillQty, order.filledQtyRaw, tsNs, feeE8, netRealized, taker, finalFill), tsNs);
  } else if (order.warmup) {
    engine.serviceExposureOpen = true;
    const Side unwindSide = oppositeSide(order.state.side);
    std::int64_t unwindRealized = 0;
    updatePosition(engine, unwindSide, priceE8, fillQty, tsNs, engine.takerFeeBpsE8, unwindRealized);
    JsonOrder unwindOrder{};
    unwindOrder.legIndex = engine.legIndex;
    unwindOrder.id = nextOrderId(engine);
    unwindOrder.submitTsNs = tsNs;
    unwindOrder.activeTsNs = tsNs;
    unwindOrder.tsNs = tsNs;
    unwindOrder.action = kRecordActionOrder;
    unwindOrder.side = recordSide(unwindSide);
    unwindOrder.type = kRecordTypeMarket;
    unwindOrder.status = kRecordStatusFilled;
    unwindOrder.priceE8 = priceE8;
    unwindOrder.qtyE8 = fillQty;
    unwindOrder.reduceOnly = true;
    unwindOrder.service = true;
    unwindOrder.serviceAutounwind = true;
    engine.orders.push_back(unwindOrder);
    JsonFill unwindFill{};
    unwindFill.legIndex = engine.legIndex;
    unwindFill.orderId = unwindOrder.id;
    unwindFill.entryTsNs = tsNs;
    unwindFill.exitTsNs = tsNs;
    unwindFill.side = recordSide(unwindSide);
    unwindFill.priceE8 = priceE8;
    unwindFill.qtyE8 = fillQty;
    unwindFill.realizedPnlE8 = unwindRealized;
    unwindFill.reduceOnly = true;
    unwindFill.fillReason = kFillReasonTop;
    unwindFill.liquidity = kFillLiquidityTaker;
    unwindFill.orderQtyE8 = fillQty;
    unwindFill.cumulativeFilledQtyE8 = fillQty;
    unwindFill.avgPriceE8 = priceE8;
    engine.fills.push_back(unwindFill);
    appendEquityPoint(engine, tsNs);
    appendRiskSnapshot(engine, tsNs, kRiskSnapshotReasonFill);
    ++engine.reduceOnlyFills;
    engine.serviceExposureOpen = false;
  }
  if (finalFill) {
    if (order.warmup) engine.warmupInFlight = false;
    if (order.serviceAutounwind) engine.serviceExposureOpen = false;
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
    if (!order.state.valid || !order.active || order.stopTriggered) continue;
    if (order.state.orderType != canon::OrderType::Stop ||
        order.state.triggerSource != canon::OrderTriggerSource::Last) continue;
    const std::int64_t sourcePrice = order.state.side.raw == Side::Buy().raw ? ask : bid;
    if (!stopMarketCrossed(order, sourcePrice)) continue;
    markStopMarketTriggered(order, sourcePrice, tsNs);
    updateTriggeredStopMarketRecord(engine, order, sourcePrice, tsNs);
  }
}

void triggerStopMarketsBySource(EngineState& engine,
                                canon::OrderTriggerSource source,
                                std::int64_t sourcePrice,
                                std::int64_t tsNs) {
  if (sourcePrice <= 0) return;
  bool triggered = false;
  for (OpenOrder& order : engine.open) {
    if (!order.state.valid || !order.active || order.stopTriggered) continue;
    if (order.state.orderType != canon::OrderType::Stop || order.state.triggerSource != source) continue;
    if (!stopMarketCrossed(order, sourcePrice)) continue;
    markStopMarketTriggered(order, sourcePrice, tsNs);
    updateTriggeredStopMarketRecord(engine, order, sourcePrice, tsNs);
    triggered = true;
  }
  if (triggered) tryFillOnTop(engine, tsNs);
}

void tryFillOnTop(EngineState& engine, std::int64_t tsNs) {
  if (engine.liquidated || engine.riskStopped) return;
  const cxet::composite::BookTickerRuntimeV1& top = effectiveBookTicker(engine.venueSnapshot);
  const std::int64_t bid = top.bid.px.raw;
  const std::int64_t ask = top.ask.px.raw;
  triggerStopMarketsOnLastBbo(engine, bid, ask, tsNs);
  const bool depthExecutionConfigured = venueOrderBookExecutionConfigured(engine);
  const hft_trader::core::LocalOrderBookState* rawBook =
      depthExecutionConfigured && hasTopExecutionCandidate(engine, bid, ask)
          ? executionDepthBook(engine, tsNs)
          : nullptr;
  const hft_trader::core::LocalOrderBookState* fillBook = rawBook;
  if (rawBook != nullptr && bboOverlayCandidate(top)) {
    hft_trader::core::LocalOrderBookState& overlayBook =
        topExecutionOverlayScratch(engine, *rawBook);
    fillBook = &overlayedTakerExecutionBook(engine.venueSnapshot, top, *rawBook, overlayBook);
  }
  for (OpenOrder& order : engine.open) {
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
    if (order.state.orderType == canon::OrderType::Limit) {
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

void tryFillOnTrade(EngineState& engine, const cxet::composite::TradeRuntimeV1& trade) {
  if (engine.liquidated || engine.riskStopped) return;
  for (OpenOrder& order : engine.open) {
    if (!order.state.valid || !order.active || order.state.orderType == canon::OrderType::Market ||
        order.state.orderType == canon::OrderType::Stop) continue;
    const PassiveQueueTradeFill fill = consumePassiveQueueForTrade(engine, order, trade);
    if (fill.requestedQtyE8 <= 0) continue;
    FillExecutionMetadata metadata{};
    metadata.fillReason = kFillReasonPassiveQueue;
    metadata.queueAheadBeforeE8 = fill.queueAheadBeforeE8;
    metadata.queueAheadAfterE8 = fill.queueAheadAfterE8;
    (void)fillOrderQty(engine, order, order.state.price.raw, fill.requestedQtyE8, static_cast<std::int64_t>(trade.ts.raw), false, metadata);
  }
}

void tryTriggerStopMarketsOnMark(EngineState& engine, const cxet::composite::MarkPriceRuntimeV1& markPrice) {
  triggerStopMarketsBySource(engine,
                             canon::OrderTriggerSource::Mark,
                             markPrice.markPrice.raw,
                             static_cast<std::int64_t>(markPrice.ts.raw));
}

void tryTriggerStopMarketsOnIndex(EngineState& engine, const cxet::composite::IndexPriceRuntimeV1& indexPrice) {
  triggerStopMarketsBySource(engine,
                             canon::OrderTriggerSource::Index,
                             indexPrice.indexPrice.raw,
                             static_cast<std::int64_t>(indexPrice.ts.raw));
}

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
          hft_trader::core::CancelIntent cancel{};
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

bool forceClosePosition(EngineState& engine, std::int64_t tsNs, std::uint64_t orderLatencyUs) {
  if (engine.netQtyRaw == 0) return true;
  const cxet::composite::BookTickerRuntimeV1& top = effectiveBookTicker(engine.venueSnapshot);
  if (top.bid.px.raw <= 0 || top.ask.px.raw <= 0) return false;

  hft_trader::core::OrderIntent intent{};
  intent.clientIntentId.raw = nextOrderId(engine);
  intent.side = engine.netQtyRaw > 0 ? Side::Sell() : Side::Buy();
  intent.orderType = canon::OrderType::Market;
  intent.qty.raw = engine.netQtyRaw > 0 ? engine.netQtyRaw : -engine.netQtyRaw;
  intent.reduceOnly = true;

  std::array<hft_trader::core::OrderIntent, 1> orders{intent};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, tsNs, orderLatencyUs, 0u);
  applyDueActions(engine, delayedTs(tsNs, orderLatencyUs));
  return engine.netQtyRaw == 0;
}

bool forceClosePosition(EngineState& engine, std::int64_t tsNs, BacktestLatencyRuntime& latency) {
  if (engine.netQtyRaw == 0) return true;
  const cxet::composite::BookTickerRuntimeV1& top = effectiveBookTicker(engine.venueSnapshot);
  if (top.bid.px.raw <= 0 || top.ask.px.raw <= 0) return false;

  hft_trader::core::OrderIntent intent{};
  intent.clientIntentId.raw = nextOrderId(engine);
  intent.side = engine.netQtyRaw > 0 ? Side::Sell() : Side::Buy();
  intent.orderType = canon::OrderType::Market;
  intent.qty.raw = engine.netQtyRaw > 0 ? engine.netQtyRaw : -engine.netQtyRaw;
  intent.reduceOnly = true;

  std::array<hft_trader::core::OrderIntent, 1> orders{intent};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, tsNs, latency);
  return true;
}

void cancelOpenOrders(EngineState& engine, std::int64_t tsNs, std::uint64_t cancelLatencyUs) {
  std::array<hft_trader::core::CancelIntent, kMaxOrderStates> cancels{};
  std::size_t cancelCount = 0;
  std::uint64_t commandId = nextOrderId(engine);
  for (const OpenOrder& order : engine.open) {
    if (!order.state.valid || cancelCount >= cancels.size()) continue;
    hft_trader::core::CancelIntent& intent = cancels[cancelCount++];
    intent.clientIntentId.raw = commandId++;
    intent.targetClientIntentId = order.state.clientIntentId;
    intent.targetOrderType = order.state.orderType;
  }
  if (cancelCount == 0) return;

  hft_trader::core::StrategyIo io{};
  io.cancelOut = Span<hft_trader::core::CancelIntent>(cancels.data(), cancelCount);
  io.cancelCount = cancelCount;
  applyIntents(engine, io, tsNs, 0u, cancelLatencyUs);
  applyDueActions(engine, delayedTs(tsNs, cancelLatencyUs));
}

void cancelOpenOrders(EngineState& engine, std::int64_t tsNs, BacktestLatencyRuntime& latency) {
  std::array<hft_trader::core::CancelIntent, kMaxOrderStates> cancels{};
  std::size_t cancelCount = 0;
  std::uint64_t commandId = nextOrderId(engine);
  for (const OpenOrder& order : engine.open) {
    if (!order.state.valid || cancelCount >= cancels.size()) continue;
    hft_trader::core::CancelIntent& intent = cancels[cancelCount++];
    intent.clientIntentId.raw = commandId++;
    intent.targetClientIntentId = order.state.clientIntentId;
    intent.targetOrderType = order.state.orderType;
  }
  if (cancelCount == 0) return;

  hft_trader::core::StrategyIo io{};
  io.cancelOut = Span<hft_trader::core::CancelIntent>(cancels.data(), cancelCount);
  io.cancelCount = cancelCount;
  const std::uint64_t maxCancelLatencyUs = latencyProfileMaxUs(latency.config.cancelOrder);
  applyIntents(engine, io, tsNs, latency);
  applyDueActions(engine, delayedTs(tsNs, maxCancelLatencyUs));
}

namespace {

template<typename OrderActiveTsFn, typename CancelActiveTsFn>
IntentApplyResult applyIntentsImpl(EngineState& engine,
                                   const hft_trader::core::StrategyIo& io,
                                   std::int64_t tsNs,
                                   OrderActiveTsFn orderActiveTs,
                                   CancelActiveTsFn cancelActiveTs) {
  IntentApplyResult result{};
  if (engine.liquidated || engine.riskStopped) return result;
  if (io.commandSequenceFiltered || !hft_trader::core::strategyIoCommandSequencesValid(io)) {
    result.disposition = IntentApplyDisposition::RestoreStrategyState;
    return result;
  }

  const bool hasIntents = io.cancelCount != 0u || io.orderCount != 0u || io.commandCount != 0u;
  auto submitCommandOrder = [&](const hft_trader::core::StrategyCommandIntent& command) {
    if (command.kind == hft_trader::core::StrategyCommandIntentKind::WarmupOrder) {
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

  for (std::size_t i = 0u; i < io.commandCount;) {
    const hft_trader::core::StrategyCommandIntent& command = io.commandOut[i];
    if (command.sequenceKind != hft_trader::core::StrategyCommandSequenceKind::None) {
      const bool cancelThenNew =
          command.sequenceKind == hft_trader::core::StrategyCommandSequenceKind::LimitAmendCancelThenNew;
      const hft_trader::core::StrategyCommandIntent& first = command;
      const hft_trader::core::StrategyCommandIntent& second = io.commandOut[i + 1u];
      const hft_trader::core::StrategyCommandIntent& cancelCommand = cancelThenNew ? first : second;
      const hft_trader::core::StrategyCommandIntent& orderCommand = cancelThenNew ? second : first;
      if (cancelThenNew) {
        if (!submitCancelIntent(engine,
                                cancelCommand.cancel,
                                tsNs,
                                cancelActiveTs(cancelCommand.cancel))) {
          result.disposition = IntentApplyDisposition::RestoreStrategyState;
          break;
        }
        if (!submitCommandOrder(orderCommand)) {
          result.disposition = IntentApplyDisposition::RestoreStrategyState;
          break;
        }
      } else {
        if (!submitCommandOrder(orderCommand)) {
          result.disposition = IntentApplyDisposition::RestoreStrategyState;
          break;
        }
        if (!submitCancelIntent(engine,
                                cancelCommand.cancel,
                                tsNs,
                                cancelActiveTs(cancelCommand.cancel))) {
          result.disposition = IntentApplyDisposition::PreserveAcceptedSequence;
          break;
        }
      }
      i += 2u;
      continue;
    }

    if (command.kind == hft_trader::core::StrategyCommandIntentKind::Cancel) {
      (void)submitCancelIntent(engine, command.cancel, tsNs, cancelActiveTs(command.cancel));
    } else if (command.kind == hft_trader::core::StrategyCommandIntentKind::Order ||
               command.kind == hft_trader::core::StrategyCommandIntentKind::WarmupOrder) {
      (void)submitCommandOrder(command);
    }
    ++i;
  }

  if (result.disposition == IntentApplyDisposition::Applied) {
    for (std::size_t i = 0u; i < io.cancelCount; ++i) {
      const hft_trader::core::CancelIntent& intent = io.cancelOut[i];
      (void)submitCancelIntent(engine, intent, tsNs, cancelActiveTs(intent));
    }
    for (std::size_t i = 0u; i < io.orderCount; ++i) {
      const hft_trader::core::OrderIntent& intent = io.orderOut[i];
      (void)submitOrderIntent(engine, intent, tsNs, orderActiveTs(intent));
    }
  }
  if (hasIntents) bumpContextRevision(engine);
  applyDueActions(engine, tsNs);
  return result;
}

}  // namespace

IntentApplyResult applyIntents(EngineState& engine,
                               const hft_trader::core::StrategyIo& io,
                               std::int64_t tsNs,
                               std::uint64_t orderLatencyUs,
                               std::uint64_t cancelLatencyUs) {
  return applyIntentsImpl(
      engine,
      io,
      tsNs,
      [tsNs, orderLatencyUs](const hft_trader::core::OrderIntent&) noexcept {
        return delayedTs(tsNs, orderLatencyUs);
      },
      [tsNs, cancelLatencyUs](const hft_trader::core::CancelIntent&) noexcept {
        return delayedTs(tsNs, cancelLatencyUs);
      });
}

IntentApplyResult applyIntents(EngineState& engine,
                               const hft_trader::core::StrategyIo& io,
                               std::int64_t tsNs,
                               BacktestLatencyRuntime& latency) {
  return applyIntentsImpl(
      engine,
      io,
      tsNs,
      [tsNs, &latency](const hft_trader::core::OrderIntent& intent) noexcept {
        const BacktestLatencyChannel channel =
            (intent.orderType == canon::OrderType::Market || intent.orderType == canon::OrderType::Stop)
            ? BacktestLatencyChannel::MarketOrder
            : BacktestLatencyChannel::LimitOrder;
        return delayedTs(tsNs, sampleLatencyUs(latency, channel));
      },
      [tsNs, &latency](const hft_trader::core::CancelIntent&) noexcept {
        return delayedTs(tsNs, sampleLatencyUs(latency, BacktestLatencyChannel::CancelOrder));
      });
}

}  // namespace hft_backtest::detail
