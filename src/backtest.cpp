#include "hft_backtest/backtest.hpp"

#include "session_loader.hpp"

#include "hft_trader/core/ControlState.hpp"
#include "hft_trader/core/Intent.hpp"
#include "hft_trader/core/LocalOrderBook.hpp"
#include "hft_trader/core/OrderState.hpp"
#include "hft_trader/core/PositionState.hpp"
#include "hft_trader/core/StrategyContract.hpp"
#include "hft_trader/runtime/GeneratedStrategyRegistry.hpp"
#include "hft_trader/runtime/RuntimeConfig.hpp"

#include "canon/PositionAndExchange.hpp"
#include "canon/Subtypes.hpp"
#include "primitives/composite/BookTickerRuntimeV1.hpp"
#include "primitives/composite/OrderBookDeltaRuntimeV1.hpp"
#include "primitives/composite/OrderBookSnapshot.hpp"
#include "primitives/composite/TradeRuntimeV1.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string_view>
#include <vector>

namespace hft_backtest {
namespace {

using hft_backtest::detail::BookTickerRow;
using hft_backtest::detail::DepthRow;
using hft_backtest::detail::PriceLevelRow;
using hft_backtest::detail::SessionData;
using hft_backtest::detail::TradeRow;
using hft_trader::runtime::StrategyDescriptor;
using hft_trader::runtime::StrategyInstrumentSpec;
using hft_trader::runtime::StrategyParamStorage;

constexpr std::size_t kIntentCapacity = 64;
constexpr std::size_t kMaxOrderStates = 256;
constexpr std::size_t kNoRecordIndex = std::numeric_limits<std::size_t>::max();

enum class PendingActionKind : std::uint8_t {
  OrderAck = 0,
  CancelAck = 1,
  AmendAck = 2,
};

std::string makeRunId() {
  const auto nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  return "run-" + std::to_string(nowNs);
}

std::string quote(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 2);
  out.push_back('"');
  for (char c : value) {
    if (c == '\\' || c == '"') out.push_back('\\');
    if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else out.push_back(c);
  }
  out.push_back('"');
  return out;
}

const char* resultStatusName(Status status) noexcept {
  switch (status) {
    case Status::Ok: return "complete";
    case Status::Cancelled: return "cancelled";
    default: return "error";
  }
}

ExchangeId exchangeFromText(const std::string& value) noexcept {
  if (value == "binance") return canon::kExchangeIdBinance;
  if (value == "bybit") return canon::kExchangeIdBybit;
  if (value == "kucoin") return canon::kExchangeIdKucoin;
  if (value == "gate") return canon::kExchangeIdGate;
  if (value == "bitget") return canon::kExchangeIdBitget;
  if (value == "okx") return canon::kExchangeIdOkx;
  return canon::kExchangeIdUnknown;
}

canon::MarketType marketFromText(const std::string& value) noexcept {
  if (value == "futures" || value == "futures_usd") return canon::kMarketTypeFutures;
  if (value == "spot") return canon::kMarketTypeSpot;
  if (value == "swap") return canon::kMarketTypeSwap;
  return canon::kMarketTypeUnknown;
}

Side sideFromRow(std::int64_t raw) noexcept {
  return raw == 1 ? Side::Buy() : Side::Sell();
}

cxet::composite::TradeRuntimeV1 toRuntime(const TradeRow& row) noexcept {
  cxet::composite::TradeRuntimeV1 out{};
  out.ts.raw = static_cast<std::uint64_t>(row.tsNs > 0 ? row.tsNs : 0);
  out.price.raw = row.priceE8;
  out.qty.raw = row.qtyE8;
  out.side = sideFromRow(row.side);
  return out;
}

cxet::composite::BookTickerRuntimeV1 toRuntime(const BookTickerRow& row) noexcept {
  cxet::composite::BookTickerRuntimeV1 out{};
  out.ts.raw = static_cast<std::uint64_t>(row.tsNs > 0 ? row.tsNs : 0);
  out.bid.px.raw = row.bidPriceE8;
  out.bid.qty.raw = row.bidQtyE8;
  out.ask.px.raw = row.askPriceE8;
  out.ask.qty.raw = row.askQtyE8;
  return out;
}

cxet::composite::OrderBookDeltaRuntimeV1 toRuntime(const DepthRow& row) noexcept {
  cxet::composite::OrderBookDeltaRuntimeV1 out{};
  out.ts.raw = static_cast<std::uint64_t>(row.tsNs > 0 ? row.tsNs : 0);
  for (const PriceLevelRow& level : row.levels) {
    Price price{};
    Amount qty{};
    price.raw = level.priceE8;
    qty.raw = level.qtyE8;
    (void)cxet::composite::appendOrderBookLevel(out, price, qty, sideFromRow(level.side));
  }
  return out;
}

cxet::composite::OrderBookSnapshot toRuntimeSnapshot(const detail::SnapshotDocument& row) noexcept {
  cxet::composite::OrderBookSnapshot out{};
  out.ts.raw = static_cast<std::uint64_t>(row.tsNs > 0 ? row.tsNs : 0);
  for (const PriceLevelRow& level : row.levels) {
    Price price{};
    Amount qty{};
    price.raw = level.priceE8;
    qty.raw = level.qtyE8;
    (void)cxet::composite::appendOrderBookLevel(out, price, qty, sideFromRow(level.side));
  }
  return out;
}

std::int64_t mulPriceQtyE8(std::int64_t px, std::int64_t qty) noexcept {
  if (px == 0 || qty <= 0) return 0;
  const bool negative = px < 0;
  const std::uint64_t absPx = static_cast<std::uint64_t>(negative ? -px : px);
  const unsigned __int128 product = static_cast<unsigned __int128>(absPx) *
                                    static_cast<unsigned __int128>(static_cast<std::uint64_t>(qty));
  const std::int64_t value = static_cast<std::int64_t>(product / 100000000ull);
  return negative ? -value : value;
}

struct JsonOrder {
  std::uint64_t id{0};
  std::uint64_t targetId{0};
  std::int64_t submitTsNs{0};
  std::int64_t activeTsNs{0};
  std::int64_t tsNs{0};
  std::string action{};
  std::string side{};
  std::string type{};
  std::string status{};
  std::int64_t priceE8{0};
  std::int64_t qtyE8{0};
};

struct JsonFill {
  std::uint64_t orderId{0};
  std::int64_t entryTsNs{0};
  std::int64_t exitTsNs{0};
  std::string side{};
  std::int64_t priceE8{0};
  std::int64_t qtyE8{0};
  std::int64_t realizedPnlE8{0};
};

struct OpenOrder {
  hft_trader::core::StrategyOrderState state{};
  std::size_t recordIndex{kNoRecordIndex};
  std::int64_t createdTsNs{0};
  std::int64_t activeTsNs{0};
  bool active{false};
};

struct PendingAction {
  PendingActionKind kind{PendingActionKind::OrderAck};
  std::size_t slotIndex{kNoRecordIndex};
  std::size_t recordIndex{kNoRecordIndex};
  std::int64_t activeTsNs{0};
  hft_trader::core::AmendIntent amend{};
};

struct EngineState {
  std::vector<JsonOrder> orders{};
  std::vector<JsonFill> fills{};
  std::vector<PendingAction> pending{};
  std::array<OpenOrder, kMaxOrderStates> open{};
  hft_trader::core::StrategyPositionState position{};
  hft_trader::core::StrategyMarketSnapshot snapshot{};
  hft_trader::core::LocalOrderBookState orderBook{};
  std::int64_t netQtyRaw{0};
  std::int64_t entryPriceE8{0};
  std::int64_t realizedPnlE8{0};
};

std::vector<hft_trader::core::StrategyOrderState> visibleOrders(const EngineState& engine) {
  std::vector<hft_trader::core::StrategyOrderState> out;
  out.reserve(kMaxOrderStates);
  for (const OpenOrder& order : engine.open) {
    if (order.state.valid) out.push_back(order.state);
  }
  return out;
}

std::int64_t latencyNs(std::uint64_t latencyUs) noexcept {
  constexpr std::uint64_t kMaxSafeUs = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / 1000);
  if (latencyUs > kMaxSafeUs) return std::numeric_limits<std::int64_t>::max();
  return static_cast<std::int64_t>(latencyUs * 1000u);
}

std::int64_t delayedTs(std::int64_t tsNs, std::uint64_t latencyUs) noexcept {
  const std::int64_t add = latencyNs(latencyUs);
  if (tsNs > std::numeric_limits<std::int64_t>::max() - add) return std::numeric_limits<std::int64_t>::max();
  return tsNs + add;
}

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

OpenOrder* findOpen(EngineState& engine, Id id) noexcept {
  for (OpenOrder& order : engine.open) {
    if (order.state.valid && (order.state.clientIntentId.raw == id.raw || order.state.desiredClientIntentId.raw == id.raw || order.state.pendingClientIntentId.raw == id.raw)) return &order;
  }
  return nullptr;
}

OpenOrder* findFree(EngineState& engine) noexcept {
  for (OpenOrder& order : engine.open) {
    if (!order.state.valid) return &order;
  }
  return nullptr;
}

void updatePosition(EngineState& engine, Side side, std::int64_t priceE8, std::int64_t qtyE8, std::int64_t tsNs, std::int64_t& realized) noexcept {
  realized = 0;
  const std::int64_t signedQty = side.raw == Side::Buy().raw ? qtyE8 : -qtyE8;
  if (engine.netQtyRaw == 0 || (engine.netQtyRaw > 0 && signedQty > 0) || (engine.netQtyRaw < 0 && signedQty < 0)) {
    const std::int64_t oldAbs = engine.netQtyRaw < 0 ? -engine.netQtyRaw : engine.netQtyRaw;
    const std::int64_t addAbs = signedQty < 0 ? -signedQty : signedQty;
    const std::int64_t newAbs = oldAbs + addAbs;
    if (newAbs > 0) {
      const unsigned __int128 oldNotional = static_cast<unsigned __int128>(static_cast<std::uint64_t>(engine.entryPriceE8 > 0 ? engine.entryPriceE8 : priceE8)) * static_cast<unsigned __int128>(static_cast<std::uint64_t>(oldAbs));
      const unsigned __int128 addNotional = static_cast<unsigned __int128>(static_cast<std::uint64_t>(priceE8)) * static_cast<unsigned __int128>(static_cast<std::uint64_t>(addAbs));
      engine.entryPriceE8 = static_cast<std::int64_t>((oldNotional + addNotional) / static_cast<unsigned __int128>(static_cast<std::uint64_t>(newAbs)));
    }
    engine.netQtyRaw += signedQty;
  } else {
    const std::int64_t oldAbs = engine.netQtyRaw < 0 ? -engine.netQtyRaw : engine.netQtyRaw;
    const std::int64_t closeAbs = (signedQty < 0 ? -signedQty : signedQty) < oldAbs ? (signedQty < 0 ? -signedQty : signedQty) : oldAbs;
    if (engine.netQtyRaw > 0) realized = mulPriceQtyE8(priceE8 - engine.entryPriceE8, closeAbs);
    else realized = mulPriceQtyE8(engine.entryPriceE8 - priceE8, closeAbs);
    engine.realizedPnlE8 += realized;
    engine.netQtyRaw += signedQty;
    if (engine.netQtyRaw == 0) engine.entryPriceE8 = 0;
    if ((engine.netQtyRaw > 0 && signedQty > oldAbs) || (engine.netQtyRaw < 0 && -signedQty > oldAbs)) engine.entryPriceE8 = priceE8;
  }
  engine.position.netQty.raw = engine.netQtyRaw;
  engine.position.entryPrice.raw = engine.entryPriceE8;
  engine.position.lastPrice.raw = priceE8;
  engine.position.lastUpdateUtc.raw = static_cast<std::uint64_t>(tsNs > 0 ? tsNs : 0);
  engine.position.valid = true;
}

bool fillOrder(EngineState& engine, OpenOrder& order, std::int64_t priceE8, std::int64_t tsNs) {
  if (!order.state.valid || !order.active || order.state.qty.raw <= 0 || priceE8 <= 0) return false;
  std::int64_t realized = 0;
  updatePosition(engine, order.state.side, priceE8, order.state.qty.raw, tsNs, realized);
  engine.fills.push_back(JsonFill{order.state.clientIntentId.raw,
                                  order.activeTsNs,
                                  tsNs,
                                  order.state.side.raw == Side::Buy().raw ? "buy" : "sell",
                                  priceE8,
                                  order.state.qty.raw,
                                  realized});
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
      (void)fillOrder(engine, order, order.state.side.raw == Side::Buy().raw ? ask : bid, tsNs);
    } else if (order.state.side.raw == Side::Buy().raw && order.state.price.raw >= ask) {
      (void)fillOrder(engine, order, ask, tsNs);
    } else if (order.state.side.raw == Side::Sell().raw && order.state.price.raw <= bid) {
      (void)fillOrder(engine, order, bid, tsNs);
    }
  }
}

void tryFillOnTrade(EngineState& engine, const cxet::composite::TradeRuntimeV1& trade) {
  for (OpenOrder& order : engine.open) {
    if (!order.state.valid || !order.active || order.state.orderType == canon::OrderType::Market) continue;
    if (trade.side.raw == Side::Buy().raw && order.state.side.raw == Side::Sell().raw && trade.price.raw >= order.state.price.raw) {
      (void)fillOrder(engine, order, order.state.price.raw, static_cast<std::int64_t>(trade.ts.raw));
    } else if (trade.side.raw == Side::Sell().raw && order.state.side.raw == Side::Buy().raw && trade.price.raw <= order.state.price.raw) {
      (void)fillOrder(engine, order, order.state.price.raw, static_cast<std::int64_t>(trade.ts.raw));
    }
  }
}

void applyDueActions(EngineState& engine, std::int64_t tsNs) {
  std::size_t i = 0;
  while (i < engine.pending.size()) {
    PendingAction action = engine.pending[i];
    if (action.activeTsNs > tsNs) {
      ++i;
      continue;
    }
    engine.pending.erase(engine.pending.begin() + static_cast<std::ptrdiff_t>(i));
    if (action.recordIndex < engine.orders.size()) {
      engine.orders[action.recordIndex].activeTsNs = action.activeTsNs;
      engine.orders[action.recordIndex].status = "acked";
    }
    if (action.slotIndex >= engine.open.size()) continue;
    OpenOrder& order = engine.open[action.slotIndex];
    if (action.kind == PendingActionKind::OrderAck) {
      if (!order.state.valid) continue;
      order.active = true;
      order.activeTsNs = action.activeTsNs;
      if (order.recordIndex < engine.orders.size()) {
        engine.orders[order.recordIndex].activeTsNs = action.activeTsNs;
        engine.orders[order.recordIndex].status = "active";
      }
      tryFillOnTop(engine, tsNs);
    } else if (action.kind == PendingActionKind::CancelAck) {
      if (order.state.valid && order.recordIndex < engine.orders.size()) engine.orders[order.recordIndex].status = "cancelled";
      order = OpenOrder{};
    } else if (action.kind == PendingActionKind::AmendAck) {
      if (!order.state.valid) continue;
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
      tryFillOnTop(engine, tsNs);
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
    engine.orders.push_back(JsonOrder{intent.clientIntentId.raw, intent.targetClientIntentId.raw, tsNs, activeTs, tsNs, "cancel", "", "", "pending", 0, 0});
    const std::size_t slotIndex = findOpenIndex(engine, intent.targetClientIntentId);
    if (slotIndex != kNoRecordIndex) {
      OpenOrder& order = engine.open[slotIndex];
      order.state.cancelPending = true;
      order.state.pendingClientIntentId = intent.clientIntentId;
      order.state.state = hft_trader::core::StrategyOrderLocalState::CancelPending;
      engine.pending.push_back(PendingAction{PendingActionKind::CancelAck, slotIndex, recordIndex, activeTs, {}});
    } else {
      engine.orders[recordIndex].status = "missing";
    }
  }
  for (std::size_t i = 0; i < io.amendCount; ++i) {
    const auto& intent = io.amendOut[i];
    const std::int64_t activeTs = delayedTs(tsNs, amendLatencyUs);
    const std::size_t recordIndex = engine.orders.size();
    engine.orders.push_back(JsonOrder{intent.clientIntentId.raw, intent.targetClientIntentId.raw, tsNs, activeTs, tsNs, "amend", intent.side.raw == Side::Buy().raw ? "buy" : "sell", "limit", "pending", intent.price.raw, intent.qty.raw});
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
    engine.orders.push_back(JsonOrder{intent.clientIntentId.raw, 0, tsNs, activeTs, tsNs, "order", intent.side.raw == Side::Buy().raw ? "buy" : "sell", intent.orderType == canon::OrderType::Market ? "market" : "limit", "pending", intent.price.raw, intent.qty.raw});
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
      engine.pending.push_back(PendingAction{PendingActionKind::OrderAck, slotIndex, recordIndex, activeTs, {}});
    } else {
      engine.orders[recordIndex].status = "rejected";
    }
  }
  applyDueActions(engine, tsNs);
}

bool runCycle(const StrategyDescriptor& descriptor,
              void* state,
              EngineState& engine,
              const hft_trader::core::SymbolShardConfig& shard,
              const hft_trader::core::ShardControlState& control,
              std::int64_t tsNs,
              const BacktestRunRequest& request) {
  std::array<hft_trader::core::OrderIntent, kIntentCapacity> orderBuf{};
  std::array<hft_trader::core::AmendIntent, kIntentCapacity> amendBuf{};
  std::array<hft_trader::core::CancelIntent, kIntentCapacity> cancelBuf{};
  std::vector<hft_trader::core::StrategyOrderState> orderViewStorage = visibleOrders(engine);
  hft_trader::core::StrategyContext ctx{};
  ctx.shard = &shard;
  ctx.control = &control;
  ctx.snapshot = &engine.snapshot;
  ctx.marketData.snapshots = &engine.snapshot;
  ctx.marketData.count = 1;
  ctx.orderState.orders = orderViewStorage.data();
  ctx.orderState.count = orderViewStorage.size();
  ctx.positionState = engine.position;
  ctx.nowMono.raw = static_cast<std::uint64_t>(tsNs > 0 ? tsNs : 0);
  ctx.nowUtc = ctx.nowMono;
  ctx.initialOrderSeedAllowed = true;
  ctx.io.orderOut = Span<hft_trader::core::OrderIntent>(orderBuf.data(), orderBuf.size());
  ctx.io.amendOut = Span<hft_trader::core::AmendIntent>(amendBuf.data(), amendBuf.size());
  ctx.io.cancelOut = Span<hft_trader::core::CancelIntent>(cancelBuf.data(), cancelBuf.size());
  ctx.io.clear();
  if (!descriptor.runCycle(state, ctx)) return false;
  applyIntents(engine, ctx.io, tsNs, request.orderLatencyUs, request.amendLatencyUs, request.cancelLatencyUs);
  return true;
}

struct TimelineEvent {
  std::int64_t tsNs{0};
  std::int64_t ingestSeq{0};
  std::uint8_t kind{0};
  std::size_t index{0};
};

void buildTimeline(const SessionData& session, std::vector<TimelineEvent>& out) {
  out.clear();
  for (std::size_t i = 0; i < session.depths.size(); ++i) out.push_back(TimelineEvent{session.depths[i].tsNs, session.depths[i].ingestSeq, 0, i});
  for (std::size_t i = 0; i < session.trades.size(); ++i) out.push_back(TimelineEvent{session.trades[i].tsNs, session.trades[i].ingestSeq, 1, i});
  for (std::size_t i = 0; i < session.bookTickers.size(); ++i) out.push_back(TimelineEvent{session.bookTickers[i].tsNs, session.bookTickers[i].ingestSeq, 2, i});
  std::stable_sort(out.begin(), out.end(), [](const TimelineEvent& lhs, const TimelineEvent& rhs) noexcept {
    if (lhs.tsNs != rhs.tsNs) return lhs.tsNs < rhs.tsNs;
    if (lhs.ingestSeq != rhs.ingestSeq) return lhs.ingestSeq < rhs.ingestSeq;
    return lhs.kind < rhs.kind;
  });
}

std::filesystem::path resultPathFor(const BacktestRunRequest& request, const std::string& runId) {
  if (!request.outputPath.empty()) return request.outputPath;
  return request.sessionPath / "backtests" / (runId + ".json");
}

Status writeResult(const BacktestRunResult& result, const EngineState& engine) noexcept {
  std::error_code ec;
  std::filesystem::create_directories(result.resultPath.parent_path(), ec);
  if (ec) return Status::IoError;
  std::ofstream out(result.resultPath, std::ios::out | std::ios::trunc);
  if (!out.is_open()) return Status::IoError;
  const char q = '"';
  out << "{\n";
  out << "  " << q << "type" << q << ": " << q << "run.result" << q << ",\n";
  out << "  " << q << "request_id" << q << ": " << quote(result.requestId) << ",\n";
  out << "  " << q << "run_id" << q << ": " << quote(result.runId) << ",\n";
  out << "  " << q << "status" << q << ": " << quote(resultStatusName(result.status)) << ",\n";
  out << "  " << q << "strategy" << q << ": " << quote(result.strategy) << ",\n";
  out << "  " << q << "session_path" << q << ": " << quote(result.sessionPath.generic_string()) << ",\n";
  out << "  " << q << "config_path" << q << ": " << quote(result.configPath.generic_string()) << ",\n";
  out << "  " << q << "execution" << q << ": {" << q << "order_latency_us" << q << ": " << result.orderLatencyUs
      << ", " << q << "amend_latency_us" << q << ": " << result.amendLatencyUs
      << ", " << q << "cancel_latency_us" << q << ": " << result.cancelLatencyUs << "},\n";
  out << "  " << q << "orders" << q << ": [\n";
  for (std::size_t i = 0; i < engine.orders.size(); ++i) {
    const JsonOrder& row = engine.orders[i];
    out << "    {" << q << "id" << q << ": " << row.id
        << ", " << q << "target_id" << q << ": " << row.targetId
        << ", " << q << "submit_ts_ns" << q << ": " << row.submitTsNs
        << ", " << q << "active_ts_ns" << q << ": " << row.activeTsNs
        << ", " << q << "ts_ns" << q << ": " << row.tsNs
        << ", " << q << "action" << q << ": " << quote(row.action)
        << ", " << q << "side" << q << ": " << quote(row.side)
        << ", " << q << "type" << q << ": " << quote(row.type)
        << ", " << q << "status" << q << ": " << quote(row.status)
        << ", " << q << "price_e8" << q << ": " << row.priceE8
        << ", " << q << "qty_e8" << q << ": " << row.qtyE8 << "}"
        << (i + 1 == engine.orders.size() ? "\n" : ",\n");
  }
  out << "  ],\n  " << q << "fills" << q << ": [\n";
  for (std::size_t i = 0; i < engine.fills.size(); ++i) {
    const JsonFill& row = engine.fills[i];
    out << "    {" << q << "order_id" << q << ": " << row.orderId
        << ", " << q << "entry_ts_ns" << q << ": " << row.entryTsNs
        << ", " << q << "exit_ts_ns" << q << ": " << row.exitTsNs
        << ", " << q << "side" << q << ": " << quote(row.side)
        << ", " << q << "price_e8" << q << ": " << row.priceE8
        << ", " << q << "qty_e8" << q << ": " << row.qtyE8
        << ", " << q << "realized_pnl_e8" << q << ": " << row.realizedPnlE8 << "}"
        << (i + 1 == engine.fills.size() ? "\n" : ",\n");
  }
  out << "  ],\n  " << q << "equity_points" << q << ": [\n";
  for (std::size_t i = 0; i < engine.fills.size(); ++i) {
    const JsonFill& row = engine.fills[i];
    out << "    {" << q << "ts_ns" << q << ": " << row.exitTsNs
        << ", " << q << "realized_pnl_e8" << q << ": " << row.realizedPnlE8 << "}"
        << (i + 1 == engine.fills.size() ? "\n" : ",\n");
  }
  out << "  ],\n  " << q << "summary" << q << ": {\n";
  out << "    " << q << "mode" << q << ": " << q << "deterministic_bbo_v1" << q << ",\n";
  out << "    " << q << "events" << q << ": " << result.events << ",\n";
  out << "    " << q << "trades" << q << ": " << result.trades << ",\n";
  out << "    " << q << "booktickers" << q << ": " << result.bookTickers << ",\n";
  out << "    " << q << "depths" << q << ": " << result.depths << ",\n";
  out << "    " << q << "orders" << q << ": " << engine.orders.size() << ",\n";
  out << "    " << q << "fills" << q << ": " << engine.fills.size() << ",\n";
  out << "    " << q << "net_qty_e8" << q << ": " << engine.netQtyRaw << ",\n";
  out << "    " << q << "realized_pnl_e8" << q << ": " << result.realizedPnlE8 << ",\n";
  out << "    " << q << "unrealized_pnl_e8" << q << ": " << result.unrealizedPnlE8 << ",\n";
  out << "    " << q << "total_pnl_e8" << q << ": " << result.totalPnlE8 << "\n";
  out << "  },\n  " << q << "errors" << q << ": ";
  if (result.error.empty()) out << "[]\n";
  else out << "[{" << q << "message" << q << ": " << quote(result.error) << "}]\n";
  out << "}\n";
  return out.good() ? Status::Ok : Status::IoError;
}
bool reportProgress(std::uint64_t done, std::uint64_t total, std::string stage, BacktestProgressCallback cb, void* userData) noexcept {
  if (!cb) return true;
  BacktestProgress progress{};
  progress.eventsDone = done;
  progress.eventsTotal = total;
  progress.percent = total == 0 ? 0 : static_cast<std::uint32_t>((done * 100u) / total);
  if (progress.percent > 100u) progress.percent = 100u;
  progress.stage = std::move(stage);
  return cb(progress, userData);
}

}  // namespace

BacktestRunResult runBacktest(const BacktestRunRequest& request,
                              BacktestProgressCallback progressCallback,
                              void* userData) noexcept {
  BacktestRunResult result{};
  result.runId = request.runId.empty() ? makeRunId() : request.runId;
  result.requestId = request.requestId.empty() ? result.runId : request.requestId;
  result.sessionPath = request.sessionPath;
  result.configPath = request.configPath;
  result.resultPath = resultPathFor(request, result.runId);
  result.orderLatencyUs = request.orderLatencyUs;
  result.amendLatencyUs = request.amendLatencyUs;
  result.cancelLatencyUs = request.cancelLatencyUs;

  if (request.sessionPath.empty() || request.configPath.empty()) {
    result.status = Status::InvalidArgument;
    result.error = "session_path and config_path are required";
    return result;
  }

  SessionData session{};
  std::string error;
  result.status = detail::loadSession(request.sessionPath, session, error);
  if (!isOk(result.status)) {
    result.error = error;
    (void)writeResult(result, EngineState{});
    return result;
  }

  hft_trader::runtime::HftRuntimeConfig cfg{};
  if (!hft_trader::runtime::loadRuntimeConfig(request.configPath.string().c_str(), cfg, error)) {
    result.status = Status::InvalidArgument;
    result.error = error;
    (void)writeResult(result, EngineState{});
    return result;
  }
  result.strategy = request.strategy.empty() ? cfg.strategyType : request.strategy;
  if (result.strategy != cfg.strategyType) {
    result.status = Status::ConfigMismatch;
    result.error = "requested strategy does not match config [strategy] type";
    (void)writeResult(result, EngineState{});
    return result;
  }
  const StrategyDescriptor* descriptor = hft_trader::runtime::findStrategyDescriptor(cfg.strategyType);
  if (!descriptor || !descriptor->resolveParams || !descriptor->initState || !descriptor->runCycle) {
    result.status = Status::Unsupported;
    result.error = "strategy descriptor is missing required callbacks";
    (void)writeResult(result, EngineState{});
    return result;
  }

  const ExchangeId sessionExchange = exchangeFromText(session.exchange);
  const canon::MarketType sessionMarket = marketFromText(session.market);
  const hft_trader::runtime::VenueRuntimeConfig* venue = nullptr;
  for (const auto& candidate : cfg.venues) {
    if (candidate.exchange.raw != sessionExchange.raw || candidate.market.raw != sessionMarket.raw) continue;
    for (const Symbol& symbol : candidate.symbols) {
      if (session.symbol == symbol.data) venue = &candidate;
    }
  }
  if (!venue) {
    result.status = Status::ConfigMismatch;
    result.error = "config venue/symbol does not match recorder session";
    (void)writeResult(result, EngineState{});
    return result;
  }

  StrategyParamStorage params{};
  if (!descriptor->resolveParams(cfg, *venue, params, error)) {
    result.status = Status::InvalidArgument;
    result.error = error;
    (void)writeResult(result, EngineState{});
    return result;
  }
  StrategyInstrumentSpec instrument{};
  instrument.tickSizeRaw = session.instrument.tickSizeE8;
  instrument.stepSizeRaw = session.instrument.lotSizeE8;
  instrument.contractBaseQtyRaw = session.instrument.contractBaseQtyE8;
  instrument.tickGrid = hft_trader::core::prepareFastStepGrid(instrument.tickSizeRaw);
  instrument.qtyStepGrid = hft_trader::core::prepareFastStepGrid(instrument.stepSizeRaw);
  instrument.contractBaseQtyGrid = hft_trader::core::prepareFastStepGrid(instrument.contractBaseQtyRaw);

  alignas(hft_trader::runtime::kStrategyRuntimeStateAlign) std::array<unsigned char, hft_trader::runtime::kStrategyRuntimeStateBytes> state{};
  if (!descriptor->initState(state.data(), params, instrument)) {
    result.status = Status::InvalidArgument;
    result.error = "strategy initState failed";
    (void)writeResult(result, EngineState{});
    return result;
  }

  EngineState engine{};
  if (session.hasSnapshot) {
    hft_trader::core::LocalOrderBookConfig bookConfig{};
    bookConfig.maxStoredLevelsPerSide.raw = rawdata::kMaxOrderbookLevels;
    bookConfig.tickSizeRaw = instrument.tickSizeRaw;
    hft_trader::core::applySnapshot(engine.orderBook, toRuntimeSnapshot(session.snapshot), venue->symbols.front(), bookConfig);
    hft_trader::core::bindOrderBookSnapshot(engine.snapshot, engine.orderBook);
  }
  hft_trader::core::SymbolShardConfig shard{};
  shard.shardId.raw = 1;
  shard.strategyId.raw = 1;
  shard.symbolId.raw = 1;
  shard.symbol = venue->symbols.front();
  hft_trader::core::ShardControlState control{};
  control.shardId = shard.shardId;

  std::vector<TimelineEvent> timeline;
  buildTimeline(session, timeline);
  result.trades = session.trades.size();
  result.bookTickers = session.bookTickers.size();
  result.depths = session.depths.size();
  result.events = timeline.size();
  result.firstTsNs = session.firstTsNs;
  result.lastTsNs = session.lastTsNs;

  if (!reportProgress(0, timeline.size(), "running", progressCallback, userData)) {
    result.status = Status::Cancelled;
    result.error = "cancelled";
    (void)writeResult(result, engine);
    return result;
  }
  for (std::size_t i = 0; i < timeline.size(); ++i) {
    const TimelineEvent& event = timeline[i];
    applyDueActions(engine, event.tsNs);
    if (event.kind == 0) {
      const auto delta = toRuntime(session.depths[event.index]);
      hft_trader::core::copyDepthSnapshot(engine.snapshot, delta);
      (void)hft_trader::core::applyDelta(engine.orderBook, delta);
      hft_trader::core::bindOrderBookSnapshot(engine.snapshot, engine.orderBook);
    } else if (event.kind == 1) {
      const auto trade = toRuntime(session.trades[event.index]);
      hft_trader::core::copyTradeSnapshot(engine.snapshot, trade);
      tryFillOnTrade(engine, trade);
    } else {
      const auto ticker = toRuntime(session.bookTickers[event.index]);
      hft_trader::core::copyBookTickerSnapshot(engine.snapshot, ticker);
      (void)hft_trader::core::applyBookTicker(engine.orderBook, ticker);
      hft_trader::core::bindOrderBookSnapshot(engine.snapshot, engine.orderBook);
      tryFillOnTop(engine, event.tsNs);
    }
    (void)runCycle(*descriptor, state.data(), engine, shard, control, event.tsNs, request);
    if ((i % 1024u) == 0u && !reportProgress(i, timeline.size(), "running", progressCallback, userData)) {
      result.status = Status::Cancelled;
      result.error = "cancelled";
      (void)writeResult(result, engine);
      return result;
    }
  }

  const std::int64_t lastPx = engine.position.lastPrice.raw;
  result.realizedPnlE8 = engine.realizedPnlE8;
  if (engine.netQtyRaw > 0 && lastPx > 0 && engine.entryPriceE8 > 0) result.unrealizedPnlE8 = mulPriceQtyE8(lastPx - engine.entryPriceE8, engine.netQtyRaw);
  if (engine.netQtyRaw < 0 && lastPx > 0 && engine.entryPriceE8 > 0) result.unrealizedPnlE8 = mulPriceQtyE8(engine.entryPriceE8 - lastPx, -engine.netQtyRaw);
  result.totalPnlE8 = result.realizedPnlE8 + result.unrealizedPnlE8;
  result.orders = engine.orders.size();
  result.fills = engine.fills.size();
  result.status = Status::Ok;
  Status writeStatus = writeResult(result, engine);
  if (!isOk(writeStatus)) result.status = writeStatus;
  (void)reportProgress(timeline.size(), timeline.size(), "complete", progressCallback, userData);
  return result;
}

}  // namespace hft_backtest
