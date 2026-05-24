#include "backtest_engine.hpp"

#include "hft_trader/runtime/ClientOrderId.hpp"

#include <limits>

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

cxet::composite::UserDataEvent makeFillEvent(EngineState& engine,
                                             const OpenOrder& order,
                                             std::int64_t priceE8,
                                             std::int64_t qtyE8,
                                             std::int64_t tsNs,
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
  event.eventTime.raw = static_cast<std::uint64_t>(tsNs > 0 ? tsNs : 0);
  event.updateTime = event.eventTime;
  event.reduceOnly = order.reduceOnly ? 1u : 0u;
  event.maker = taker ? 0u : 1u;
  ++engine.fillSequence;
  return event;
}

}  // namespace

std::vector<hft_trader::core::StrategyOrderState> visibleOrders(const EngineState& engine) {
  std::vector<hft_trader::core::StrategyOrderState> out;
  out.reserve(kMaxOrderStates);
  for (const OpenOrder& order : engine.open) {
    if (order.state.valid) out.push_back(order.state);
  }
  return out;
}

std::int64_t delayedTs(std::int64_t tsNs, std::uint64_t latencyUs) noexcept {
  const std::int64_t add = latencyNs(latencyUs);
  if (tsNs > std::numeric_limits<std::int64_t>::max() - add) return std::numeric_limits<std::int64_t>::max();
  return tsNs + add;
}

bool fillOrder(EngineState& engine, OpenOrder& order, std::int64_t priceE8, std::int64_t tsNs, bool taker) {
  if (!order.state.valid || !order.active || order.state.qty.raw <= 0 || priceE8 <= 0) return false;
  std::int64_t netRealized = 0;
  updatePosition(engine, order.state.side, priceE8, order.state.qty.raw, tsNs, taker ? engine.takerFeeBpsE8 : engine.makerFeeBpsE8, netRealized);
  engine.fills.push_back(JsonFill{order.state.clientIntentId.raw,
                                  order.activeTsNs,
                                  tsNs,
                                  order.state.side.raw == Side::Buy().raw ? "buy" : "sell",
                                  priceE8,
                                  order.state.qty.raw,
                                  netRealized,
                                  order.reduceOnly});
  if (order.reduceOnly) ++engine.reduceOnlyFills;
  engine.userEvents.push_back(makeFillEvent(engine, order, priceE8, order.state.qty.raw, tsNs, taker));
  if (order.recordIndex < engine.orders.size()) engine.orders[order.recordIndex].status = "filled";
  order = OpenOrder{};
  return true;
}

void tryFillOnTop(EngineState& engine, std::int64_t tsNs) {
  const std::int64_t bid = engine.snapshot.lastBookTicker.bid.px.raw;
  const std::int64_t ask = engine.snapshot.lastBookTicker.ask.px.raw;
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
  while (true) {
    std::size_t actionIndex = kNoRecordIndex;
    std::int64_t actionTs = std::numeric_limits<std::int64_t>::max();
    for (std::size_t i = 0; i < engine.pending.size(); ++i) {
      if (engine.pending[i].activeTsNs <= tsNs && engine.pending[i].activeTsNs < actionTs) {
        actionIndex = i;
        actionTs = engine.pending[i].activeTsNs;
      }
    }
    if (actionIndex == kNoRecordIndex) break;
    PendingAction action = engine.pending[actionIndex];
    engine.pending.erase(engine.pending.begin() + static_cast<std::ptrdiff_t>(actionIndex));
    if (action.kind != PendingActionKind::OrderAck && action.recordIndex < engine.orders.size()) {
      engine.orders[action.recordIndex].activeTsNs = action.activeTsNs;
      engine.orders[action.recordIndex].status = "acked";
    }
    if (action.slotIndex >= engine.open.size()) continue;
    OpenOrder& order = engine.open[action.slotIndex];
    if (!matchesExpectedOrder(order, action)) {
      if (action.kind != PendingActionKind::OrderAck && action.recordIndex < engine.orders.size()) engine.orders[action.recordIndex].status = "missing";
      continue;
    }
    if (action.kind == PendingActionKind::OrderAck) {
      order.active = true;
      order.activeTsNs = action.activeTsNs;
      if (order.recordIndex < engine.orders.size()) {
        engine.orders[order.recordIndex].activeTsNs = action.activeTsNs;
        engine.orders[order.recordIndex].status = "active";
      }
      tryFillOnTop(engine, action.activeTsNs);
    } else if (action.kind == PendingActionKind::CancelAck) {
      if (order.recordIndex < engine.orders.size()) engine.orders[order.recordIndex].status = "cancelled";
      order = OpenOrder{};
    } else if (action.kind == PendingActionKind::AmendAck) {
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
      order.state.state = hft_trader::core::StrategyOrderLocalState::LiveLocal;
      order.active = true;
      order.activeTsNs = action.activeTsNs;
      if (order.recordIndex < engine.orders.size()) engine.orders[order.recordIndex].status = "amended";
      tryFillOnTop(engine, action.activeTsNs);
    }
  }
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
    engine.orders.push_back(JsonOrder{intent.clientIntentId.raw, intent.targetClientIntentId.raw, tsNs, activeTs, tsNs, "cancel", "", "", "pending", 0, 0, false});
    const std::size_t slotIndex = findOpenIndex(engine, intent.targetClientIntentId);
    if (slotIndex != kNoRecordIndex) {
      OpenOrder& order = engine.open[slotIndex];
      order.state.cancelPending = true;
      order.state.pendingClientIntentId = intent.clientIntentId;
      order.state.state = hft_trader::core::StrategyOrderLocalState::CancelPending;
      engine.pending.push_back(PendingAction{PendingActionKind::CancelAck, slotIndex, recordIndex, activeTs, intent.targetClientIntentId, intent.clientIntentId, {}});
    } else {
      engine.orders[recordIndex].status = "missing";
    }
  }
  for (std::size_t i = 0; i < io.amendCount; ++i) {
    const auto& intent = io.amendOut[i];
    const std::int64_t activeTs = delayedTs(tsNs, amendLatencyUs);
    const std::size_t recordIndex = engine.orders.size();
    engine.orders.push_back(JsonOrder{intent.clientIntentId.raw, intent.targetClientIntentId.raw, tsNs, activeTs, tsNs, "amend", intent.side.raw == Side::Buy().raw ? "buy" : "sell", "limit", "pending", intent.price.raw, intent.qty.raw, false});
    const std::size_t slotIndex = findOpenIndex(engine, intent.targetClientIntentId);
    if (slotIndex != kNoRecordIndex) {
      OpenOrder& order = engine.open[slotIndex];
      order.state.amendPending = true;
      order.state.pendingClientIntentId = intent.clientIntentId;
      order.state.desiredClientIntentId = intent.clientIntentId;
      order.state.desiredPrice = intent.price;
      order.state.desiredQty = intent.qty;
      order.state.state = hft_trader::core::StrategyOrderLocalState::AmendPending;
      PendingAction action{};
      action.kind = PendingActionKind::AmendAck;
      action.slotIndex = slotIndex;
      action.recordIndex = recordIndex;
      action.activeTsNs = activeTs;
      action.targetClientIntentId = intent.targetClientIntentId;
      action.commandClientIntentId = intent.clientIntentId;
      action.amend = intent;
      engine.pending.push_back(action);
    } else {
      engine.orders[recordIndex].status = "missing";
    }
  }
  for (std::size_t i = 0; i < io.orderCount; ++i) {
    const auto& intent = io.orderOut[i];
    const std::int64_t activeTs = delayedTs(tsNs, orderLatencyUs);
    const std::size_t recordIndex = engine.orders.size();
    engine.orders.push_back(JsonOrder{intent.clientIntentId.raw, 0, tsNs, activeTs, tsNs, "order", intent.side.raw == Side::Buy().raw ? "buy" : "sell", intent.orderType == canon::OrderType::Market ? "market" : "limit", "pending", intent.price.raw, intent.qty.raw, intent.reduceOnly});
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
      engine.pending.push_back(PendingAction{PendingActionKind::OrderAck, slotIndex, recordIndex, activeTs, intent.clientIntentId, intent.clientIntentId, {}});
    } else {
      engine.orders[recordIndex].status = "rejected";
    }
  }
  applyDueActions(engine, tsNs);
}

}  // namespace hft_backtest::detail
