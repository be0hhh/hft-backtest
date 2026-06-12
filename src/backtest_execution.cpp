#include "backtest_engine.hpp"

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
                                             std::int64_t tsNs,
                                             std::int64_t feeE8,
                                             std::int64_t realizedPnlE8,
                                             bool taker) noexcept {
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
  event.orderStatus = canon::OrderStatus::Closed;
  event.orderStatusSet = 1u;
  event.price.raw = priceE8;
  event.avgPrice.raw = priceE8;
  event.lastPrice.raw = priceE8;
  event.qty.raw = qtyE8;
  event.filledQty.raw = qtyE8;
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

std::int64_t reduceOnlyFillQty(const EngineState& engine, const OpenOrder& order) noexcept {
  const std::int64_t requested = order.state.qty.raw;
  if (!order.reduceOnly) return requested;
  if (requested <= 0 || engine.netQtyRaw == 0) return 0;
  if (engine.netQtyRaw > 0) {
    if (order.state.side.raw != Side::Sell().raw) return 0;
    return requested < engine.netQtyRaw ? requested : engine.netQtyRaw;
  }
  const std::int64_t shortQty = -engine.netQtyRaw;
  if (order.state.side.raw != Side::Buy().raw) return 0;
  return requested < shortQty ? requested : shortQty;
}

bool rejectOrder(EngineState& engine, OpenOrder& order, std::int64_t tsNs) {
  if (!order.state.valid) return false;
  engine.userEvents.push_back(makeRejectedOrderEvent(engine, order, tsNs));
  if (order.recordIndex < engine.orders.size()) engine.orders[order.recordIndex].status = kRecordStatusRejected;
  order = OpenOrder{};
  return false;
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

bool fillOrder(EngineState& engine, OpenOrder& order, std::int64_t priceE8, std::int64_t tsNs, bool taker) {
  if (!order.state.valid || !order.active || order.state.qty.raw <= 0 || priceE8 <= 0) return false;
  const std::int64_t fillQty = reduceOnlyFillQty(engine, order);
  if (fillQty <= 0) return rejectOrder(engine, order, tsNs);
  const std::int64_t feeBpsE8 = taker ? engine.takerFeeBpsE8 : engine.makerFeeBpsE8;
  const std::int64_t feeE8 = feeForFillE8(priceE8, fillQty, feeBpsE8);
  std::int64_t netRealized = 0;
  updatePosition(engine, order.state.side, priceE8, fillQty, tsNs, feeBpsE8, netRealized);
  engine.fills.push_back(JsonFill{engine.legIndex,
                                  order.state.clientIntentId.raw,
                                  order.activeTsNs,
                                  tsNs,
                                  recordSide(order.state.side),
                                  priceE8,
                                  fillQty,
                                  netRealized,
                                  order.reduceOnly});
  appendEquityPoint(engine, tsNs);
  appendRiskSnapshot(engine, tsNs, kRiskSnapshotReasonFill);
  if (order.reduceOnly) ++engine.reduceOnlyFills;
  engine.userEvents.push_back(makeFillEvent(engine, order, priceE8, fillQty, tsNs, feeE8, netRealized, taker));
  if (order.recordIndex < engine.orders.size()) engine.orders[order.recordIndex].status = kRecordStatusFilled;
  order = OpenOrder{};
  return true;
}

void tryFillOnTop(EngineState& engine, std::int64_t tsNs) {
  const cxet::composite::BookTickerRuntimeV1& top = effectiveBookTicker(engine.venueSnapshot);
  const std::int64_t bid = top.bid.px.raw;
  const std::int64_t ask = top.ask.px.raw;
  if (bid <= 0 || ask <= 0) return;
  for (OpenOrder& order : engine.open) {
    if (!order.state.valid || !order.active) continue;
    if (order.state.orderType == canon::OrderType::Market) {
      (void)fillOrder(engine, order, order.state.side.raw == Side::Buy().raw ? ask : bid, tsNs, true);
    } else if (order.state.side.raw == Side::Buy().raw && order.state.price.raw >= ask) {
      (void)fillOrder(engine, order, ask, tsNs, true);
    } else if (order.state.side.raw == Side::Sell().raw && order.state.price.raw <= bid) {
      (void)fillOrder(engine, order, bid, tsNs, true);
    }
  }
}

void tryFillOnTrade(EngineState& engine, const cxet::composite::TradeRuntimeV1& trade) {
  for (OpenOrder& order : engine.open) {
    if (!order.state.valid || !order.active || order.state.orderType == canon::OrderType::Market) continue;
    if (trade.side.raw == Side::Buy().raw && order.state.side.raw == Side::Sell().raw && trade.price.raw >= order.state.price.raw) {
      (void)fillOrder(engine, order, order.state.price.raw, static_cast<std::int64_t>(trade.ts.raw), false);
    } else if (trade.side.raw == Side::Sell().raw && order.state.side.raw == Side::Buy().raw && trade.price.raw <= order.state.price.raw) {
      (void)fillOrder(engine, order, order.state.price.raw, static_cast<std::int64_t>(trade.ts.raw), false);
    }
  }
}

void applyDueActions(EngineState& engine, std::int64_t tsNs) {
  while (!engine.pending.empty()) {
    if (engine.pending.front().activeTsNs > tsNs) break;
    std::pop_heap(engine.pending.begin(), engine.pending.end(), pendingEarlier);
    PendingAction action = std::move(engine.pending.back());
    engine.pending.pop_back();
    if (action.kind == PendingActionKind::CancelAck && action.recordIndex < engine.orders.size()) {
      engine.orders[action.recordIndex].activeTsNs = action.activeTsNs;
      engine.orders[action.recordIndex].status = kRecordStatusAcked;
    } else if (action.kind == PendingActionKind::AmendAck && action.replaceCancelRecordIndex < engine.orders.size() &&
               engine.orders[action.replaceCancelRecordIndex].status != kRecordStatusAcked) {
      engine.orders[action.replaceCancelRecordIndex].activeTsNs = action.activeTsNs;
      engine.orders[action.replaceCancelRecordIndex].status = kRecordStatusAcked;
    }
    if (action.slotIndex >= engine.open.size()) continue;
    OpenOrder& order = engine.open[action.slotIndex];
    if (!matchesExpectedOrder(order, action)) {
      if (action.kind == PendingActionKind::AmendAck && action.replaceCancelRecordIndex < engine.orders.size()) engine.orders[action.replaceCancelRecordIndex].status = kRecordStatusMissing;
      if (action.kind != PendingActionKind::OrderAck && action.recordIndex < engine.orders.size()) engine.orders[action.recordIndex].status = kRecordStatusMissing;
      continue;
    }
    if (action.kind == PendingActionKind::OrderAck) {
      order.active = true;
      order.activeTsNs = action.activeTsNs;
      if (order.recordIndex < engine.orders.size()) {
        engine.orders[order.recordIndex].activeTsNs = action.activeTsNs;
        engine.orders[order.recordIndex].status = kRecordStatusActive;
      }
      tryFillOnTop(engine, action.activeTsNs);
    } else if (action.kind == PendingActionKind::CancelAck) {
      if (order.recordIndex < engine.orders.size()) engine.orders[order.recordIndex].status = kRecordStatusCancelled;
      order = OpenOrder{};
    } else if (action.kind == PendingActionKind::ReplaceCancelAck) {
      if (order.recordIndex < engine.orders.size()) engine.orders[order.recordIndex].status = kRecordStatusCancelled;
      if (action.replaceCancelRecordIndex < engine.orders.size()) engine.orders[action.replaceCancelRecordIndex].status = kRecordStatusAcked;
      order.active = false;
    } else if (action.kind == PendingActionKind::AmendAck) {
      if (order.recordIndex < engine.orders.size()) engine.orders[order.recordIndex].status = kRecordStatusCancelled;
      order.state.clientIntentId = action.amend.clientIntentId;
      order.state.desiredClientIntentId = action.amend.clientIntentId;
      order.state.pendingClientIntentId = Id{};
      order.state.side = action.amend.side;
      order.state.orderType = action.amend.targetOrderType;
      order.state.price = action.amend.price;
      order.state.qty = action.amend.qty;
      order.state.desiredPrice = Price{};
      order.state.desiredQty = Amount{};
      order.state.amendPending = false;
      order.state.cancelPending = false;
      order.state.state = hft_trader::core::StrategyOrderLocalState::LiveLocal;
      order.recordIndex = action.recordIndex;
      order.active = true;
      order.activeTsNs = action.activeTsNs;
      if (order.recordIndex < engine.orders.size()) {
        engine.orders[order.recordIndex].activeTsNs = action.activeTsNs;
        engine.orders[order.recordIndex].status = kRecordStatusActive;
      }
      tryFillOnTop(engine, action.activeTsNs);
    }
  }
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
  applyIntents(engine, io, tsNs, orderLatencyUs, 0u, 0u);
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
  applyIntents(engine, io, tsNs, 0u, 0u, cancelLatencyUs);
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
  applyIntents(engine, io, tsNs, latency);
}

void applyIntents(EngineState& engine,
                  const hft_trader::core::StrategyIo& io,
                  std::int64_t tsNs,
                  std::uint64_t orderLatencyUs,
                  std::uint64_t amendLatencyUs,
                  std::uint64_t cancelLatencyUs) {
  for (std::size_t i = 0; i < io.cancelCount; ++i) {
    const auto& intent = io.cancelOut[i];
    const std::int64_t activeTs = delayedTs(tsNs, cancelLatencyUs);
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
    } else {
      engine.orders[recordIndex].status = kRecordStatusMissing;
    }
  }
  for (std::size_t i = 0; i < io.amendCount; ++i) {
    const auto& intent = io.amendOut[i];
    const std::int64_t cancelTs = delayedTs(tsNs, cancelLatencyUs);
    const std::int64_t activeTs = delayedTs(cancelTs, amendLatencyUs);
    const std::size_t cancelRecordIndex = engine.orders.size();
    engine.orders.push_back(JsonOrder{engine.legIndex, intent.clientIntentId.raw, intent.targetClientIntentId.raw, tsNs, cancelTs, tsNs, kRecordActionCancel, kRecordSideNone, kRecordTypeNone, kRecordStatusPending, 0, 0, false});
    const std::size_t replacementRecordIndex = engine.orders.size();
    engine.orders.push_back(JsonOrder{engine.legIndex, intent.clientIntentId.raw, intent.targetClientIntentId.raw, tsNs, activeTs, tsNs, kRecordActionOrder, recordSide(intent.side), kRecordTypeLimit, kRecordStatusPending, intent.price.raw, intent.qty.raw, false});
    const std::size_t slotIndex = findOpenIndex(engine, intent.targetClientIntentId);
    if (slotIndex != kNoRecordIndex) {
      OpenOrder& order = engine.open[slotIndex];
      order.state.amendPending = true;
      order.state.cancelPending = true;
      order.state.pendingClientIntentId = intent.clientIntentId;
      order.state.desiredClientIntentId = intent.clientIntentId;
      order.state.desiredPrice = intent.price;
      order.state.desiredQty = intent.qty;
      order.state.state = hft_trader::core::StrategyOrderLocalState::AmendPending;
      PendingAction cancelAction{};
      cancelAction.kind = PendingActionKind::ReplaceCancelAck;
      cancelAction.slotIndex = slotIndex;
      cancelAction.recordIndex = replacementRecordIndex;
      cancelAction.replaceCancelRecordIndex = cancelRecordIndex;
      cancelAction.activeTsNs = cancelTs;
      cancelAction.targetClientIntentId = intent.targetClientIntentId;
      cancelAction.commandClientIntentId = intent.clientIntentId;
      cancelAction.amend = intent;
      pushPendingAction(engine, std::move(cancelAction));
      PendingAction orderAction{};
      orderAction.kind = PendingActionKind::AmendAck;
      orderAction.slotIndex = slotIndex;
      orderAction.recordIndex = replacementRecordIndex;
      orderAction.replaceCancelRecordIndex = cancelRecordIndex;
      orderAction.activeTsNs = activeTs;
      orderAction.targetClientIntentId = intent.targetClientIntentId;
      orderAction.commandClientIntentId = intent.clientIntentId;
      orderAction.amend = intent;
      pushPendingAction(engine, std::move(orderAction));
    } else {
      engine.orders[cancelRecordIndex].status = kRecordStatusMissing;
      engine.orders[replacementRecordIndex].status = kRecordStatusMissing;
    }
  }
  for (std::size_t i = 0; i < io.orderCount; ++i) {
    const auto& intent = io.orderOut[i];
    const std::int64_t activeTs = delayedTs(tsNs, orderLatencyUs);
    const std::size_t recordIndex = engine.orders.size();
    engine.orders.push_back(JsonOrder{engine.legIndex, intent.clientIntentId.raw, 0, tsNs, activeTs, tsNs, kRecordActionOrder, recordSide(intent.side), recordType(intent.orderType), kRecordStatusPending, intent.price.raw, intent.qty.raw, intent.reduceOnly});
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
      slot.state.price = intent.price;
      slot.state.qty = intent.qty;
      slot.state.state = hft_trader::core::StrategyOrderLocalState::LiveLocal;
      slot.recordIndex = recordIndex;
      slot.createdTsNs = tsNs;
      slot.activeTsNs = activeTs;
      slot.active = false;
      slot.reduceOnly = intent.reduceOnly;
      PendingAction action{};
      action.kind = PendingActionKind::OrderAck;
      action.slotIndex = slotIndex;
      action.recordIndex = recordIndex;
      action.activeTsNs = activeTs;
      action.targetClientIntentId = intent.clientIntentId;
      action.commandClientIntentId = intent.clientIntentId;
      pushPendingAction(engine, std::move(action));
    } else {
      engine.orders[recordIndex].status = kRecordStatusRejected;
    }
  }
  applyDueActions(engine, tsNs);
}

void applyIntents(EngineState& engine,
                  const hft_trader::core::StrategyIo& io,
                  std::int64_t tsNs,
                  BacktestLatencyRuntime& latency) {
  for (std::size_t i = 0; i < io.cancelCount; ++i) {
    const auto& intent = io.cancelOut[i];
    const std::int64_t activeTs = delayedTs(tsNs, sampleLatencyUs(latency, BacktestLatencyChannel::LimitOrder));
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
    } else {
      engine.orders[recordIndex].status = kRecordStatusMissing;
    }
  }
  for (std::size_t i = 0; i < io.amendCount; ++i) {
    const auto& intent = io.amendOut[i];
    const std::int64_t cancelTs = delayedTs(tsNs, sampleLatencyUs(latency, BacktestLatencyChannel::LimitOrder));
    const std::int64_t activeTs = delayedTs(cancelTs, sampleLatencyUs(latency, BacktestLatencyChannel::LimitOrder));
    const std::size_t cancelRecordIndex = engine.orders.size();
    engine.orders.push_back(JsonOrder{engine.legIndex, intent.clientIntentId.raw, intent.targetClientIntentId.raw, tsNs, cancelTs, tsNs, kRecordActionCancel, kRecordSideNone, kRecordTypeNone, kRecordStatusPending, 0, 0, false});
    const std::size_t replacementRecordIndex = engine.orders.size();
    engine.orders.push_back(JsonOrder{engine.legIndex, intent.clientIntentId.raw, intent.targetClientIntentId.raw, tsNs, activeTs, tsNs, kRecordActionOrder, recordSide(intent.side), kRecordTypeLimit, kRecordStatusPending, intent.price.raw, intent.qty.raw, false});
    const std::size_t slotIndex = findOpenIndex(engine, intent.targetClientIntentId);
    if (slotIndex != kNoRecordIndex) {
      OpenOrder& order = engine.open[slotIndex];
      order.state.amendPending = true;
      order.state.cancelPending = true;
      order.state.pendingClientIntentId = intent.clientIntentId;
      order.state.desiredClientIntentId = intent.clientIntentId;
      order.state.desiredPrice = intent.price;
      order.state.desiredQty = intent.qty;
      order.state.state = hft_trader::core::StrategyOrderLocalState::AmendPending;
      PendingAction action{};
      action.kind = PendingActionKind::AmendAck;
      action.slotIndex = slotIndex;
      action.recordIndex = replacementRecordIndex;
      action.replaceCancelRecordIndex = cancelRecordIndex;
      action.activeTsNs = activeTs;
      action.targetClientIntentId = intent.targetClientIntentId;
      action.commandClientIntentId = intent.clientIntentId;
      action.amend = intent;
      pushPendingAction(engine, std::move(action));
    } else {
      engine.orders[cancelRecordIndex].status = kRecordStatusMissing;
      engine.orders[replacementRecordIndex].status = kRecordStatusMissing;
    }
  }
  for (std::size_t i = 0; i < io.orderCount; ++i) {
    const auto& intent = io.orderOut[i];
    const BacktestLatencyChannel channel = intent.orderType == canon::OrderType::Market
        ? BacktestLatencyChannel::MarketOrder
        : BacktestLatencyChannel::LimitOrder;
    const std::int64_t activeTs = delayedTs(tsNs, sampleLatencyUs(latency, channel));
    const std::size_t recordIndex = engine.orders.size();
    engine.orders.push_back(JsonOrder{engine.legIndex, intent.clientIntentId.raw, 0, tsNs, activeTs, tsNs, kRecordActionOrder, recordSide(intent.side), recordType(intent.orderType), kRecordStatusPending, intent.price.raw, intent.qty.raw, intent.reduceOnly});
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
      slot.state.price = intent.price;
      slot.state.qty = intent.qty;
      slot.state.state = hft_trader::core::StrategyOrderLocalState::LiveLocal;
      slot.recordIndex = recordIndex;
      slot.createdTsNs = tsNs;
      slot.activeTsNs = activeTs;
      slot.active = false;
      slot.reduceOnly = intent.reduceOnly;
      PendingAction action{};
      action.kind = PendingActionKind::OrderAck;
      action.slotIndex = slotIndex;
      action.recordIndex = recordIndex;
      action.activeTsNs = activeTs;
      action.targetClientIntentId = intent.clientIntentId;
      action.commandClientIntentId = intent.clientIntentId;
      pushPendingAction(engine, std::move(action));
    } else {
      engine.orders[recordIndex].status = kRecordStatusRejected;
    }
  }
  applyDueActions(engine, tsNs);
}

}  // namespace hft_backtest::detail
