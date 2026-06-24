#include "backtest_engine.hpp"

#include "backtest_execution_depth.hpp"
#include "backtest_fill_model.hpp"

#include "hft_trader/runtime/order/ClientOrderId.hpp"

#include <algorithm>
#include <array>
#include <limits>
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
  return type == canon::OrderType::Market ? kRecordTypeMarket : kRecordTypeLimit;
}

std::uint64_t nextOrderId(const EngineState& engine) noexcept {
  std::uint64_t out = 1u;
  for (const JsonOrder& order : engine.orders) {
    if (order.id >= out) out = order.id + 1u;
  }
  return out;
}

bool pendingEarlier(const PendingAction& lhs, const PendingAction& rhs) noexcept {
  return lhs.activeTsNs > rhs.activeTsNs;
}

void pushPendingAction(EngineState& engine, PendingAction action) {
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
  event.orderStatus = canon::OrderStatus::Rejected;
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
  pushUserEvent(engine, makeRejectedOrderEvent(engine, order, tsNs), tsNs);
  if (order.recordIndex < engine.orders.size()) engine.orders[order.recordIndex].status = kRecordStatusRejected;
  order = OpenOrder{};
  bumpContextRevision(engine);
  return false;
}

void terminalizeUnavailableLiquidity(EngineState& engine, OpenOrder& order, std::int64_t tsNs) {
  if (!order.state.valid) return;
  pushUserEvent(engine, makeCanceledOrderEvent(engine, order, tsNs), tsNs);
  if (order.recordIndex < engine.orders.size()) engine.orders[order.recordIndex].status = kRecordStatusPartialUnfilled;
  order = OpenOrder{};
  bumpContextRevision(engine);
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

const hft_trader::core::LocalOrderBookState* executionDepthBook(EngineState& engine,
                                                               std::int64_t tsNs) noexcept {
  if (engine.executionDepthProvider != nullptr) {
    if (engine.executionDepthProvider->advanceTo(tsNs) && engine.executionDepthProvider->ready()) {
      return &engine.executionDepthProvider->book();
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

void activateOrder(EngineState& engine, OpenOrder& order, std::int64_t activeTsNs) {
  order.active = true;
  order.activeTsNs = activeTsNs;
  order.state.state = hft_trader::core::StrategyOrderLocalState::LiveLocal;
  if (order.recordIndex < engine.orders.size()) {
    engine.orders[order.recordIndex].activeTsNs = activeTsNs;
    engine.orders[order.recordIndex].status = kRecordStatusActive;
  }
  if (postOnlyCrossesTop(engine, order)) {
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
  const std::size_t recordIndex = engine.orders.size();
  engine.orders.push_back(JsonOrder{engine.legIndex, intent.clientIntentId.raw, intent.targetClientIntentId.raw, tsNs, activeTs, tsNs, kRecordActionCancel, kRecordSideNone, kRecordTypeNone, kRecordStatusPending, 0, 0, false});
  const std::size_t slotIndex = findOpenIndex(engine, intent.targetClientIntentId);
  if (slotIndex != kNoRecordIndex) {
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
  engine.orders[recordIndex].status = kRecordStatusMissing;
  return false;
}

void submitOrderIntent(EngineState& engine,
                       const hft_trader::core::OrderIntent& intent,
                       std::int64_t tsNs,
                       std::int64_t activeTs) {
  const bool postOnly = intent.orderType == canon::OrderType::Limit && isPostOnlyPolicy(intent.limitTimeInForcePolicy);
  const std::size_t recordIndex = engine.orders.size();
  engine.orders.push_back(JsonOrder{engine.legIndex, intent.clientIntentId.raw, 0, tsNs, activeTs, tsNs, kRecordActionOrder, recordSide(intent.side), recordType(intent.orderType), kRecordStatusPending, intent.price.raw, intent.qty.raw, intent.reduceOnly, postOnly});
  if (intent.reduceOnly) ++engine.reduceOnlyOrders;
  const std::size_t slotIndex = findFreeIndex(engine);
  if (slotIndex != kNoRecordIndex) {
    OpenOrder& slot = engine.open[slotIndex];
    slot.state.valid = true;
    slot.state.clientIntentId = intent.clientIntentId;
    slot.state.desiredClientIntentId = intent.clientIntentId;
    slot.state.quoteSlotId = intent.quoteSlotId;
    slot.state.side = intent.side;
    slot.state.orderType = intent.orderType;
    slot.state.limitTimeInForcePolicy = intent.limitTimeInForcePolicy;
    slot.state.price = intent.price;
    slot.state.qty = intent.qty;
    slot.state.desiredPrice = intent.price;
    slot.state.desiredQty = intent.qty;
    slot.state.state = hft_trader::core::StrategyOrderLocalState::PendingNew;
    slot.recordIndex = recordIndex;
    slot.createdTsNs = tsNs;
    slot.activeTsNs = activeTs;
    slot.filledQtyRaw = 0;
    slot.queueAheadQtyRaw = 0;
    slot.lastAggressiveBookTsNs = 0;
    slot.active = false;
    slot.reduceOnly = intent.reduceOnly;
    slot.postOnly = postOnly;
    slot.queueInitialized = false;
    PendingAction action{};
    action.kind = PendingActionKind::OrderAck;
    action.slotIndex = slotIndex;
    action.recordIndex = recordIndex;
    action.activeTsNs = activeTs;
    action.targetClientIntentId = intent.clientIntentId;
    action.commandClientIntentId = intent.clientIntentId;
    pushPendingAction(engine, std::move(action));
  } else {
    OpenOrder rejected{};
    rejected.state.valid = true;
    rejected.state.clientIntentId = intent.clientIntentId;
    rejected.state.desiredClientIntentId = intent.clientIntentId;
    rejected.state.quoteSlotId = intent.quoteSlotId;
    rejected.state.side = intent.side;
    rejected.state.orderType = intent.orderType;
    rejected.state.limitTimeInForcePolicy = intent.limitTimeInForcePolicy;
    rejected.state.price = intent.price;
    rejected.state.qty = intent.qty;
    rejected.state.desiredPrice = intent.price;
    rejected.state.desiredQty = intent.qty;
    rejected.recordIndex = recordIndex;
    rejected.createdTsNs = tsNs;
    rejected.activeTsNs = activeTs;
    rejected.reduceOnly = intent.reduceOnly;
    rejected.postOnly = postOnly;
    pushUserEvent(engine, makeRejectedOrderEvent(engine, rejected, tsNs), tsNs);
    engine.orders[recordIndex].status = kRecordStatusRejected;
  }
}

}  // namespace

std::size_t visibleOrders(const EngineState& engine,
                          hft_trader::core::StrategyOrderState* out,
                          std::size_t capacity) noexcept {
  if (!out || capacity == 0u) return 0u;
  std::size_t count = 0u;
  for (const OpenOrder& order : engine.open) {
    if (!order.state.valid) continue;
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
  pushUserEvent(engine, makeFillEvent(engine, order, priceE8, fillQty, order.filledQtyRaw, tsNs, feeE8, netRealized, taker, finalFill), tsNs);
  if (finalFill) {
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

void tryFillOnTop(EngineState& engine, std::int64_t tsNs) {
  if (engine.liquidated || engine.riskStopped) return;
  const cxet::composite::BookTickerRuntimeV1& top = effectiveBookTicker(engine.venueSnapshot);
  const std::int64_t bid = top.bid.px.raw;
  const std::int64_t ask = top.ask.px.raw;
  const bool depthExecutionConfigured = venueOrderBookExecutionConfigured(engine);
  const hft_trader::core::LocalOrderBookState* book =
      depthExecutionConfigured && hasTopExecutionCandidate(engine, bid, ask)
          ? executionDepthBook(engine, tsNs)
          : nullptr;
  for (OpenOrder& order : engine.open) {
    if (!order.state.valid || !order.active) continue;
    AggressiveFillPlan plan{};
    if (book != nullptr && buildAggressiveBookFillPlan(engine, order, *book, plan)) {
      const bool marketOrder = order.state.orderType == canon::OrderType::Market;
      for (std::size_t i = 0u; i < plan.count && order.state.valid; ++i) {
        const AggressiveFillChunk& chunk = plan.chunks[i];
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
        order.lastAggressiveBookTsNs = plan.bookTsNs != 0 ? plan.bookTsNs : tsNs;
      }
      if (order.state.valid && marketOrder && plan.visibleDepthCapped) {
        if (order.reduceOnly && reduceOnlyPositionCloseQty(engine, order) <= 0) {
          (void)rejectOrder(engine, order, tsNs);
        } else {
          terminalizeUnavailableLiquidity(engine, order, tsNs);
        }
      }
      continue;
    }
    if (order.state.orderType == canon::OrderType::Limit &&
        depthExecutionConfigured &&
        (book != nullptr || !needsTopExecutionBook(order, bid, ask))) {
      continue;
    }
    if (bid <= 0 || ask <= 0) continue;
    FillExecutionMetadata metadata{};
    metadata.fillReason = kFillReasonTop;
    if (order.state.orderType == canon::OrderType::Market) {
      (void)fillOrder(engine, order, order.state.side.raw == Side::Buy().raw ? ask : bid, tsNs, true, metadata);
    } else if (!order.postOnly && order.state.side.raw == Side::Buy().raw && order.state.price.raw >= ask) {
      (void)fillOrder(engine, order, ask, tsNs, true, metadata);
    } else if (!order.postOnly && order.state.side.raw == Side::Sell().raw && order.state.price.raw <= bid) {
      (void)fillOrder(engine, order, bid, tsNs, true, metadata);
    }
  }
}

void tryFillOnTrade(EngineState& engine, const cxet::composite::TradeRuntimeV1& trade) {
  if (engine.liquidated || engine.riskStopped) return;
  for (OpenOrder& order : engine.open) {
    if (!order.state.valid || !order.active || order.state.orderType == canon::OrderType::Market) continue;
    const PassiveQueueTradeFill fill = consumePassiveQueueForTrade(engine, order, trade);
    if (fill.requestedQtyE8 <= 0) continue;
    FillExecutionMetadata metadata{};
    metadata.fillReason = kFillReasonPassiveQueue;
    metadata.queueAheadBeforeE8 = fill.queueAheadBeforeE8;
    metadata.queueAheadAfterE8 = fill.queueAheadAfterE8;
    (void)fillOrderQty(engine, order, order.state.price.raw, fill.requestedQtyE8, static_cast<std::int64_t>(trade.ts.raw), false, metadata);
  }
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
    } else if (action.kind == PendingActionKind::CancelAck) {
      pushUserEvent(engine, makeCanceledOrderEvent(engine, order, action.activeTsNs), action.activeTsNs);
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

void applyIntents(EngineState& engine,
                  const hft_trader::core::StrategyIo& io,
                  std::int64_t tsNs,
                  std::uint64_t orderLatencyUs,
                  std::uint64_t cancelLatencyUs) {
  if (engine.liquidated || engine.riskStopped) return;
  const bool hasIntents = io.cancelCount != 0u || io.orderCount != 0u || io.commandCount != 0u;
  for (std::size_t i = 0; i < io.commandCount; ++i) {
    const auto& command = io.commandOut[i];
    if (command.kind == hft_trader::core::StrategyCommandIntentKind::Cancel) {
      const std::int64_t activeTs = delayedTs(tsNs, cancelLatencyUs);
      (void)submitCancelIntent(engine, command.cancel, tsNs, activeTs);
    } else if (command.kind == hft_trader::core::StrategyCommandIntentKind::Order) {
      const std::int64_t activeTs = delayedTs(tsNs, orderLatencyUs);
      submitOrderIntent(engine, command.order, tsNs, activeTs);
    }
  }
  for (std::size_t i = 0; i < io.cancelCount; ++i) {
    const auto& intent = io.cancelOut[i];
    const std::int64_t activeTs = delayedTs(tsNs, cancelLatencyUs);
    (void)submitCancelIntent(engine, intent, tsNs, activeTs);
  }
  for (std::size_t i = 0; i < io.orderCount; ++i) {
    const auto& intent = io.orderOut[i];
    const std::int64_t activeTs = delayedTs(tsNs, orderLatencyUs);
    submitOrderIntent(engine, intent, tsNs, activeTs);
  }
  if (hasIntents) bumpContextRevision(engine);
  applyDueActions(engine, tsNs);
}

void applyIntents(EngineState& engine,
                  const hft_trader::core::StrategyIo& io,
                  std::int64_t tsNs,
                  BacktestLatencyRuntime& latency) {
  if (engine.liquidated || engine.riskStopped) return;
  const bool hasIntents = io.cancelCount != 0u || io.orderCount != 0u || io.commandCount != 0u;
  for (std::size_t i = 0; i < io.commandCount; ++i) {
    const auto& command = io.commandOut[i];
    if (command.kind == hft_trader::core::StrategyCommandIntentKind::Cancel) {
      const std::int64_t activeTs = delayedTs(tsNs, sampleLatencyUs(latency, BacktestLatencyChannel::CancelOrder));
      (void)submitCancelIntent(engine, command.cancel, tsNs, activeTs);
    } else if (command.kind == hft_trader::core::StrategyCommandIntentKind::Order) {
      const BacktestLatencyChannel channel = command.order.orderType == canon::OrderType::Market
          ? BacktestLatencyChannel::MarketOrder
          : BacktestLatencyChannel::LimitOrder;
      const std::int64_t activeTs = delayedTs(tsNs, sampleLatencyUs(latency, channel));
      submitOrderIntent(engine, command.order, tsNs, activeTs);
    }
  }
  for (std::size_t i = 0; i < io.cancelCount; ++i) {
    const auto& intent = io.cancelOut[i];
    const std::int64_t activeTs = delayedTs(tsNs, sampleLatencyUs(latency, BacktestLatencyChannel::CancelOrder));
    (void)submitCancelIntent(engine, intent, tsNs, activeTs);
  }
  for (std::size_t i = 0; i < io.orderCount; ++i) {
    const auto& intent = io.orderOut[i];
    const BacktestLatencyChannel channel = intent.orderType == canon::OrderType::Market
        ? BacktestLatencyChannel::MarketOrder
        : BacktestLatencyChannel::LimitOrder;
    const std::int64_t activeTs = delayedTs(tsNs, sampleLatencyUs(latency, channel));
    submitOrderIntent(engine, intent, tsNs, activeTs);
  }
  if (hasIntents) bumpContextRevision(engine);
  applyDueActions(engine, tsNs);
}

}  // namespace hft_backtest::detail
