#include "execution/backtest_execution_reprice.hpp"

#include "execution/backtest_execution_depth.hpp"

#include "hft_trader/core/market/LocalOrderBook.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <vector>

namespace hft_backtest::detail {
namespace {

constexpr std::int64_t kBpsE8 = 10000ll * 100000000ll;
constexpr std::int64_t kPct100E8 = 100ll * 100000000ll;
constexpr std::int64_t kDepthTopToleranceBps = 5;
constexpr std::uint32_t kDisableDepthAfterRejects = 3u;

struct BboTop {
  std::int64_t bidPxE8{0};
  std::int64_t bidQtyE8{0};
  std::int64_t askPxE8{0};
  std::int64_t askQtyE8{0};
  std::int64_t tsNs{0};

  [[nodiscard]] bool valid() const noexcept {
    return bidPxE8 > 0 && askPxE8 > 0 && bidPxE8 < askPxE8 && tsNs > 0;
  }
};

struct TradeTick {
  std::int64_t priceE8{0};
  std::int64_t qtyE8{0};
  std::int64_t side{0};
  std::int64_t tsNs{0};

  [[nodiscard]] bool valid() const noexcept {
    return priceE8 > 0 && qtyE8 > 0 && (side == 0 || side == 1) && tsNs > 0;
  }
};

struct BookTop {
  std::int64_t bidPxE8{0};
  std::int64_t bidQtyE8{0};
  std::int64_t askPxE8{0};
  std::int64_t askQtyE8{0};

  [[nodiscard]] bool valid() const noexcept {
    return bidPxE8 > 0 && askPxE8 > 0;
  }
};

void skipWs(std::string_view text, std::size_t& pos) noexcept {
  while (pos < text.size()) {
    const char c = text[pos];
    if (c != ' ' && c != '\n' && c != '\r' && c != '\t') break;
    ++pos;
  }
}

[[nodiscard]] bool consume(std::string_view text, std::size_t& pos, char expected) noexcept {
  skipWs(text, pos);
  if (pos >= text.size() || text[pos] != expected) return false;
  ++pos;
  return true;
}

[[nodiscard]] bool parseI64(std::string_view text, std::size_t& pos, std::int64_t& out) noexcept {
  skipWs(text, pos);
  if (pos >= text.size()) return false;
  const bool negative = text[pos] == '-';
  if (negative || text[pos] == '+') ++pos;
  if (pos >= text.size()) return false;
  constexpr std::uint64_t kMaxPositive = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  constexpr std::uint64_t kMaxNegativeAbs = kMaxPositive + 1u;
  const std::uint64_t limit = negative ? kMaxNegativeAbs : kMaxPositive;
  std::uint64_t value = 0u;
  bool any = false;
  while (pos < text.size()) {
    const unsigned digit = static_cast<unsigned>(static_cast<unsigned char>(text[pos]) - static_cast<unsigned char>('0'));
    if (digit > 9u) break;
    if (value > (limit - digit) / 10u) return false;
    value = value * 10u + digit;
    any = true;
    ++pos;
  }
  if (!any) return false;
  out = negative
      ? (value == kMaxNegativeAbs ? std::numeric_limits<std::int64_t>::min() : -static_cast<std::int64_t>(value))
      : static_cast<std::int64_t>(value);
  return true;
}

[[nodiscard]] bool parseBboLine(std::string_view line, BboTop& out) noexcept {
  BboTop parsed{};
  std::size_t pos = 0u;
  if (!consume(line, pos, '[') || !parseI64(line, pos, parsed.bidPxE8) ||
      !consume(line, pos, ',') || !parseI64(line, pos, parsed.bidQtyE8) ||
      !consume(line, pos, ',') || !parseI64(line, pos, parsed.askPxE8) ||
      !consume(line, pos, ',') || !parseI64(line, pos, parsed.askQtyE8) ||
      !consume(line, pos, ',') || !parseI64(line, pos, parsed.tsNs)) {
    return false;
  }
  out = parsed;
  return true;
}

[[nodiscard]] bool parseTradeLine(std::string_view line, TradeTick& out) noexcept {
  TradeTick parsed{};
  std::size_t pos = 0u;
  if (!consume(line, pos, '[') || !parseI64(line, pos, parsed.priceE8) ||
      !consume(line, pos, ',') || !parseI64(line, pos, parsed.qtyE8) ||
      !consume(line, pos, ',') || !parseI64(line, pos, parsed.side) ||
      !consume(line, pos, ',') || !parseI64(line, pos, parsed.tsNs)) {
    return false;
  }
  if (!parsed.valid()) return false;
  out = parsed;
  return true;
}

class ExecutionBboProvider {
 public:
  [[nodiscard]] bool open(const std::filesystem::path& sessionPath) noexcept {
    close();
    const std::filesystem::path nested = sessionPath / "jsonl" / "bookticker.jsonl";
    const std::filesystem::path root = sessionPath / "bookticker.jsonl";
    stream_.open(nested, std::ios::in | std::ios::binary);
    if (!stream_.is_open()) stream_.open(root, std::ios::in | std::ios::binary);
    available_ = stream_.is_open();
    return available_;
  }

  void close() noexcept {
    if (stream_.is_open()) stream_.close();
    latest_ = {};
    pending_ = {};
    pendingValid_ = false;
    eof_ = false;
    available_ = false;
  }

  [[nodiscard]] bool advanceTo(std::int64_t tsNs) noexcept {
    if (!available_ || tsNs <= 0) return latest_.valid();
    while (!eof_) {
      if (pendingValid_) {
        if (pending_.tsNs > tsNs) break;
        latest_ = pending_;
        pending_ = {};
        pendingValid_ = false;
        continue;
      }
      std::string line;
      if (!std::getline(stream_, line)) {
        eof_ = true;
        break;
      }
      BboTop row{};
      if (!parseBboLine(line, row) || !row.valid()) continue;
      if (row.tsNs <= tsNs) {
        latest_ = row;
      } else {
        pending_ = row;
        pendingValid_ = true;
        break;
      }
    }
    return latest_.valid();
  }

  [[nodiscard]] const BboTop& top() const noexcept { return latest_; }

  [[nodiscard]] bool readNext(BboTop& out) noexcept {
    if (!available_) return false;
    if (pendingValid_) {
      out = pending_;
      latest_ = pending_;
      pending_ = {};
      pendingValid_ = false;
      return true;
    }
    while (!eof_) {
      std::string line;
      if (!std::getline(stream_, line)) {
        eof_ = true;
        break;
      }
      BboTop row{};
      if (!parseBboLine(line, row) || !row.valid()) continue;
      latest_ = row;
      out = row;
      return true;
    }
    return false;
  }

 private:
  std::ifstream stream_{};
  BboTop latest_{};
  BboTop pending_{};
  bool pendingValid_{false};
  bool eof_{false};
  bool available_{false};
};

class ExecutionTradeProvider {
 public:
  [[nodiscard]] bool open(const std::filesystem::path& sessionPath) noexcept {
    close();
    const std::filesystem::path nested = sessionPath / "jsonl" / "trades.jsonl";
    const std::filesystem::path root = sessionPath / "trades.jsonl";
    stream_.open(nested, std::ios::in | std::ios::binary);
    if (!stream_.is_open()) stream_.open(root, std::ios::in | std::ios::binary);
    available_ = stream_.is_open();
    return available_;
  }

  void close() noexcept {
    if (stream_.is_open()) stream_.close();
    eof_ = false;
    available_ = false;
  }

  [[nodiscard]] bool readNext(TradeTick& out) noexcept {
    if (!available_) return false;
    while (!eof_) {
      std::string line;
      if (!std::getline(stream_, line)) {
        eof_ = true;
        break;
      }
      TradeTick row{};
      if (!parseTradeLine(line, row)) continue;
      out = row;
      return true;
    }
    return false;
  }

 private:
  std::ifstream stream_{};
  bool eof_{false};
  bool available_{false};
};

[[nodiscard]] std::int64_t clampI128ToI64(__int128 value) noexcept {
  if (value > static_cast<__int128>(std::numeric_limits<std::int64_t>::max())) {
    return std::numeric_limits<std::int64_t>::max();
  }
  if (value < static_cast<__int128>(std::numeric_limits<std::int64_t>::min())) {
    return std::numeric_limits<std::int64_t>::min();
  }
  return static_cast<std::int64_t>(value);
}

[[nodiscard]] hft_trader::core::BookSide fillBookSide(const JsonFill& fill) noexcept {
  return fill.side == kRecordSideBuy ? hft_trader::core::BookSide::Ask
                                     : hft_trader::core::BookSide::Bid;
}

[[nodiscard]] BookTop bestBookTop(const hft_trader::core::LocalOrderBookState& book) noexcept {
  BookTop out{};
  const hft_trader::core::LocalOrderBookView view = hft_trader::core::orderBook(book);
  if (const auto* bids = view.side(hft_trader::core::BookSide::Bid)) {
    for (std::uint32_t i = 0u; i < bids->activeCount.raw; ++i) {
      const auto& level = bids->levels[i];
      if (level.px.raw <= 0 || level.qty.raw <= 0) continue;
      if (level.px.raw > out.bidPxE8) {
        out.bidPxE8 = level.px.raw;
        out.bidQtyE8 = level.qty.raw;
      }
    }
  }
  if (const auto* asks = view.side(hft_trader::core::BookSide::Ask)) {
    for (std::uint32_t i = 0u; i < asks->activeCount.raw; ++i) {
      const auto& level = asks->levels[i];
      if (level.px.raw <= 0 || level.qty.raw <= 0) continue;
      if (out.askPxE8 == 0 || level.px.raw < out.askPxE8) {
        out.askPxE8 = level.px.raw;
        out.askQtyE8 = level.qty.raw;
      }
    }
  }
  return out;
}

[[nodiscard]] bool bpsDiffGreaterThan(std::int64_t a,
                                      std::int64_t b,
                                      std::int64_t bps) noexcept {
  if (a <= 0 || b <= 0) return true;
  const std::int64_t reference = a > b ? a : b;
  const std::int64_t diff = a > b ? a - b : b - a;
  return static_cast<__int128>(diff) * 10000 > static_cast<__int128>(reference) * bps;
}

[[nodiscard]] bool depthTopCompatibleWithBbo(const BookTop& depthTop,
                                             const BboTop& bbo) noexcept {
  if (!depthTop.valid() || !bbo.valid()) return false;
  if (depthTop.bidPxE8 >= depthTop.askPxE8) return false;
  return !bpsDiffGreaterThan(depthTop.bidPxE8, bbo.bidPxE8, kDepthTopToleranceBps) &&
         !bpsDiffGreaterThan(depthTop.askPxE8, bbo.askPxE8, kDepthTopToleranceBps);
}

[[nodiscard]] Side fillPositionSide(const JsonFill& fill) noexcept {
  return fill.side == kRecordSideSell ? Side::Sell() : Side::Buy();
}

[[nodiscard]] std::int64_t feeBpsForFill(const EngineState& engine,
                                         const JsonFill& fill) noexcept {
  return fill.liquidity == kFillLiquidityTaker ? engine.takerFeeBpsE8 : engine.makerFeeBpsE8;
}

[[nodiscard]] std::int64_t slippageBpsE8(std::int64_t slippageE8,
                                         std::int64_t referencePriceE8) noexcept {
  if (referencePriceE8 <= 0 || slippageE8 == 0) return 0;
  const __int128 numerator = static_cast<__int128>(slippageE8) * static_cast<__int128>(kBpsE8);
  return clampI128ToI64(numerator / static_cast<__int128>(referencePriceE8));
}

[[nodiscard]] std::int64_t consumedPctE8(std::int64_t filledQtyE8,
                                         std::int64_t visibleQtyE8) noexcept {
  if (filledQtyE8 <= 0 || visibleQtyE8 <= 0) return 0;
  const __int128 numerator = static_cast<__int128>(filledQtyE8) * static_cast<__int128>(kPct100E8);
  return clampI128ToI64(numerator / static_cast<__int128>(visibleQtyE8));
}

void assignRepricedFill(JsonFill& fill,
                        std::int64_t executionPrice,
                        std::int64_t referencePrice,
                        std::int64_t referenceQty,
                        std::int64_t visibleExecutableQty) noexcept {
  fill.referencePriceE8 = referencePrice;
  fill.bookLevelQtyE8 = referenceQty;
  fill.bookVisibleExecutableQtyE8 = visibleExecutableQty;
  fill.bookConsumedPctE8 = consumedPctE8(fill.qtyE8, visibleExecutableQty);
  fill.priceE8 = executionPrice;
  fill.avgPriceE8 = executionPrice;
  fill.executionQtyE8 = fill.qtyE8;
  fill.executionAvgPriceE8 = executionPrice;
  fill.executionBookConsumedPctE8 = fill.bookConsumedPctE8;
  fill.slippageE8 = fill.side == kRecordSideBuy
      ? executionPrice - fill.referencePriceE8
      : fill.referencePriceE8 - executionPrice;
  fill.slippageBpsE8 = slippageBpsE8(fill.slippageE8, fill.referencePriceE8);
  if (fill.fillReason == kFillReasonTop || fill.fillReason == kFillReasonUnknown) {
    fill.fillReason = kFillReasonAggressiveBook;
  }
}

[[nodiscard]] bool repriceFillFromBbo(JsonFill& fill, const BboTop& bbo) noexcept {
  if (fill.qtyE8 <= 0 || !bbo.valid()) return false;
  const std::int64_t referencePrice = fill.side == kRecordSideBuy ? bbo.askPxE8 : bbo.bidPxE8;
  const std::int64_t referenceQty = fill.side == kRecordSideBuy ? bbo.askQtyE8 : bbo.bidQtyE8;
  if (referencePrice <= 0 || referenceQty <= 0) return false;
  assignRepricedFill(fill, referencePrice, referencePrice, referenceQty, referenceQty);
  return true;
}

[[nodiscard]] std::int64_t bboTopQtyForFill(const JsonFill& fill, const BboTop& bbo) noexcept {
  return fill.side == kRecordSideBuy ? bbo.askQtyE8 : bbo.bidQtyE8;
}

[[nodiscard]] bool repriceFillFromBboDepth(JsonFill& fill,
                                           const BboTop& bbo,
                                           const hft_trader::core::LocalOrderBookState& book,
                                           bool& usedDepthBeyondBbo) noexcept {
  usedDepthBeyondBbo = false;
  if (fill.qtyE8 <= 0 || !bbo.valid()) return false;
  const std::int64_t referencePrice = fill.side == kRecordSideBuy ? bbo.askPxE8 : bbo.bidPxE8;
  const std::int64_t referenceQty = fill.side == kRecordSideBuy ? bbo.askQtyE8 : bbo.bidQtyE8;
  if (referencePrice <= 0 || referenceQty <= 0) return false;

  std::int64_t remainingQty = fill.qtyE8;
  std::int64_t visibleExecutableQty = referenceQty;
  __int128 weightedNotional = 0;
  const std::int64_t topTakeQty = referenceQty < remainingQty ? referenceQty : remainingQty;
  weightedNotional += static_cast<__int128>(referencePrice) * static_cast<__int128>(topTakeQty);
  remainingQty -= topTakeQty;

  if (remainingQty > 0) {
    const hft_trader::core::LocalOrderBookView view = hft_trader::core::orderBook(book);
    const hft_trader::core::LocalOrderBookSideState* side = view.side(fillBookSide(fill));
    if (side == nullptr || side->activeCount.raw == 0u) return false;
    for (std::uint32_t i = 0u; i < side->activeCount.raw; ++i) {
      const hft_trader::core::LocalOrderBookLevel& level = side->levels[i];
      if (level.px.raw <= 0 || level.qty.raw <= 0) continue;
      if (fill.side == kRecordSideBuy) {
        if (level.px.raw <= referencePrice) continue;
      } else if (level.px.raw >= referencePrice) {
        continue;
      }
      if (visibleExecutableQty > std::numeric_limits<std::int64_t>::max() - level.qty.raw) {
        visibleExecutableQty = std::numeric_limits<std::int64_t>::max();
      } else {
        visibleExecutableQty += level.qty.raw;
      }
      const std::int64_t takeQty = level.qty.raw < remainingQty ? level.qty.raw : remainingQty;
      weightedNotional += static_cast<__int128>(level.px.raw) * static_cast<__int128>(takeQty);
      remainingQty -= takeQty;
      usedDepthBeyondBbo = true;
      if (remainingQty <= 0) break;
    }
  }
  if (remainingQty > 0) return false;

  const std::int64_t executionPrice = clampI128ToI64(weightedNotional / static_cast<__int128>(fill.qtyE8));
  assignRepricedFill(fill, executionPrice, referencePrice, referenceQty, visibleExecutableQty);
  return true;
}

}  // namespace

bool repriceFillFromBook(JsonFill& fill,
                         const hft_trader::core::LocalOrderBookState& book) noexcept {
  if (fill.qtyE8 <= 0 || fill.priceE8 <= 0) return false;
  const hft_trader::core::LocalOrderBookView view = hft_trader::core::orderBook(book);
  const hft_trader::core::LocalOrderBookSideState* side = view.side(fillBookSide(fill));
  if (side == nullptr || side->activeCount.raw == 0u) return false;

  std::int64_t visibleExecutableQty = 0;
  std::int64_t remainingQty = fill.qtyE8;
  __int128 weightedNotional = 0;
  const hft_trader::core::LocalOrderBookLevel* referenceLevel = nullptr;
  for (std::uint32_t i = 0u; i < side->activeCount.raw; ++i) {
    const hft_trader::core::LocalOrderBookLevel& level = side->levels[i];
    if (level.px.raw <= 0 || level.qty.raw <= 0) continue;
    if (referenceLevel == nullptr) referenceLevel = &level;
    if (visibleExecutableQty > std::numeric_limits<std::int64_t>::max() - level.qty.raw) {
      visibleExecutableQty = std::numeric_limits<std::int64_t>::max();
      break;
    }
    visibleExecutableQty += level.qty.raw;
    if (remainingQty > 0) {
      const std::int64_t takeQty = level.qty.raw < remainingQty ? level.qty.raw : remainingQty;
      weightedNotional += static_cast<__int128>(level.px.raw) * static_cast<__int128>(takeQty);
      remainingQty -= takeQty;
    }
    if (remainingQty <= 0) break;
  }
  if (referenceLevel == nullptr || visibleExecutableQty <= 0 || remainingQty > 0) return false;

  const std::int64_t executionPrice = clampI128ToI64(weightedNotional / static_cast<__int128>(fill.qtyE8));
  assignRepricedFill(fill, executionPrice, referenceLevel->px.raw, referenceLevel->qty.raw, visibleExecutableQty);
  return true;
}

namespace {

struct Phase2LimitOrderState {
  JsonOrder* order{nullptr};
  std::int64_t endTsNs{0};
  std::int64_t retainedFilledQtyE8{0};
  std::int64_t filledQtyE8{0};
  std::int64_t queueAheadE8{0};
  std::int64_t topPriceE8{0};
  bool topActive{false};
  bool touchedTop{false};
  bool fullyFilled{false};
  bool cancelScheduled{false};

  [[nodiscard]] std::int64_t remainingQty() const noexcept {
    if (order == nullptr || order->qtyE8 <= filledQtyE8) return 0;
    return order->qtyE8 - filledQtyE8;
  }
};

[[nodiscard]] bool phase2LimitOrderCandidate(const JsonOrder& order) noexcept {
  if (order.action != kRecordActionOrder || order.type != kRecordTypeLimit) return false;
  if (order.activeTsNs <= 0 || order.priceE8 <= 0 || order.qtyE8 <= 0) return false;
  if (order.status == kRecordStatusPending ||
      order.status == kRecordStatusRejected ||
      order.status == kRecordStatusMissing ||
      order.status == kRecordStatusNone) {
    return false;
  }
  return order.side == kRecordSideBuy || order.side == kRecordSideSell;
}

[[nodiscard]] bool phase2PassiveMakerFill(const JsonFill& fill) noexcept {
  return fill.liquidity == kFillLiquidityMaker && fill.fillReason == kFillReasonPassiveQueue;
}

[[nodiscard]] bool sameOrderId(const Phase2LimitOrderState& state, std::uint64_t orderId) noexcept {
  return state.order != nullptr && state.order->id == orderId;
}

[[nodiscard]] bool trackedOrderId(const std::vector<Phase2LimitOrderState>& states,
                                  std::uint64_t orderId) noexcept {
  for (const Phase2LimitOrderState& state : states) {
    if (sameOrderId(state, orderId)) return true;
  }
  return false;
}

[[nodiscard]] std::int64_t firstCancelTsForOrder(const EngineState& engine,
                                                 std::uint64_t orderId,
                                                 std::int64_t startTsNs,
                                                 std::int64_t finalTsNs,
                                                 bool& found) noexcept {
  std::int64_t out = finalTsNs;
  found = false;
  for (const JsonOrder& command : engine.orders) {
    if (command.action != kRecordActionCancel || command.targetId != orderId || command.activeTsNs <= 0) continue;
    if (command.activeTsNs < startTsNs) continue;
    if (!found || command.activeTsNs < out) {
      out = command.activeTsNs;
      found = true;
    }
  }
  return out;
}

void seedRetainedFillState(const std::vector<JsonFill>& fills,
                           Phase2LimitOrderState& state) noexcept {
  if (state.order == nullptr) return;
  for (const JsonFill& fill : fills) {
    if (fill.orderId != state.order->id || fill.qtyE8 <= 0) continue;
    if (phase2PassiveMakerFill(fill)) continue;
    if (state.retainedFilledQtyE8 > std::numeric_limits<std::int64_t>::max() - fill.qtyE8) {
      state.retainedFilledQtyE8 = std::numeric_limits<std::int64_t>::max();
      break;
    }
    state.retainedFilledQtyE8 += fill.qtyE8;
  }
  state.filledQtyE8 = state.retainedFilledQtyE8;
  if (state.order->qtyE8 > 0 && state.filledQtyE8 >= state.order->qtyE8) state.fullyFilled = true;
}

std::vector<Phase2LimitOrderState> collectPhase2LimitOrders(EngineState& engine,
                                                            std::int64_t finalTsNs) {
  std::vector<Phase2LimitOrderState> states;
  states.reserve(engine.orders.size());
  const std::int64_t fallbackEnd = finalTsNs > 0 ? finalTsNs : std::numeric_limits<std::int64_t>::max();
  for (JsonOrder& order : engine.orders) {
    if (!phase2LimitOrderCandidate(order)) continue;
    bool cancelScheduled = false;
    const std::int64_t cancelTs = firstCancelTsForOrder(engine, order.id, order.activeTsNs, fallbackEnd, cancelScheduled);
    Phase2LimitOrderState state{};
    state.order = &order;
    state.endTsNs = cancelScheduled ? cancelTs : fallbackEnd;
    state.cancelScheduled = cancelScheduled;
    seedRetainedFillState(engine.fills, state);
    if (state.remainingQty() > 0) states.push_back(state);
  }
  return states;
}

void removePhase2PassiveMakerFills(EngineState& engine,
                                   const std::vector<Phase2LimitOrderState>& states,
                                   Phase2MakerQueueStats& stats) {
  if (states.empty() || engine.fills.empty()) return;
  std::vector<JsonFill> kept;
  kept.reserve(engine.fills.size());
  for (const JsonFill& fill : engine.fills) {
    if (trackedOrderId(states, fill.orderId) && phase2PassiveMakerFill(fill)) {
      ++stats.makerFillsRemoved;
      continue;
    }
    kept.push_back(fill);
  }
  engine.fills.swap(kept);
}

[[nodiscard]] bool orderActiveForTop(const Phase2LimitOrderState& state,
                                     std::int64_t tsNs) noexcept {
  if (state.order == nullptr || state.fullyFilled) return false;
  if (tsNs < state.order->activeTsNs) return false;
  return tsNs <= state.endTsNs;
}

[[nodiscard]] bool orderActiveForTrade(const Phase2LimitOrderState& state,
                                       std::int64_t tsNs) noexcept {
  if (state.order == nullptr || state.fullyFilled) return false;
  if (tsNs <= state.order->activeTsNs) return false;
  return tsNs <= state.endTsNs;
}

[[nodiscard]] bool bboMatchesOrderTop(const Phase2LimitOrderState& state,
                                      const BboTop& bbo,
                                      std::int64_t& topQtyE8) noexcept {
  topQtyE8 = 0;
  if (state.order == nullptr || !bbo.valid()) return false;
  if (state.order->side == kRecordSideBuy && state.order->priceE8 == bbo.bidPxE8) {
    topQtyE8 = bbo.bidQtyE8;
    return topQtyE8 > 0;
  }
  if (state.order->side == kRecordSideSell && state.order->priceE8 == bbo.askPxE8) {
    topQtyE8 = bbo.askQtyE8;
    return topQtyE8 > 0;
  }
  return false;
}

void updateTopQueueState(Phase2LimitOrderState& state,
                         const BboTop& bbo,
                         std::int64_t tsNs,
                         Phase2MakerQueueStats& stats) noexcept {
  if (!orderActiveForTop(state, tsNs)) {
    state.topActive = false;
    return;
  }
  std::int64_t topQty = 0;
  if (!bboMatchesOrderTop(state, bbo, topQty)) {
    state.topActive = false;
    return;
  }
  if (!state.topActive || state.topPriceE8 != state.order->priceE8) {
    state.queueAheadE8 = topQty;
    state.topPriceE8 = state.order->priceE8;
    state.topActive = true;
    if (!state.touchedTop) {
      state.touchedTop = true;
      ++stats.ordersTopOfBook;
    }
    return;
  }
  if (topQty < state.queueAheadE8) state.queueAheadE8 = topQty;
}

[[nodiscard]] std::int64_t makerMinCrossE8(const EngineState& engine) noexcept {
  if (engine.makerFillMinCrossTicks == 0u) return 0;
  if (engine.tickSizeE8 <= 0) return std::numeric_limits<std::int64_t>::max();
  const auto ticks = static_cast<std::uint64_t>(engine.makerFillMinCrossTicks);
  const auto tickSize = static_cast<std::uint64_t>(engine.tickSizeE8);
  const auto maxValue = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  if (tickSize != 0u && ticks > maxValue / tickSize) return std::numeric_limits<std::int64_t>::max();
  return static_cast<std::int64_t>(ticks * tickSize);
}

[[nodiscard]] bool tradeCrossesPhase2Order(const EngineState& engine,
                                           const Phase2LimitOrderState& state,
                                           const TradeTick& trade) noexcept {
  if (state.order == nullptr || !trade.valid()) return false;
  const std::int64_t minCross = makerMinCrossE8(engine);
  if (minCross == std::numeric_limits<std::int64_t>::max()) return false;
  if (state.order->side == kRecordSideBuy && trade.side == kRecordSideSell) {
    if (state.order->priceE8 < std::numeric_limits<std::int64_t>::min() + minCross) return false;
    return trade.priceE8 <= state.order->priceE8 - minCross;
  }
  if (state.order->side == kRecordSideSell && trade.side == kRecordSideBuy) {
    if (state.order->priceE8 > std::numeric_limits<std::int64_t>::max() - minCross) return false;
    return trade.priceE8 >= state.order->priceE8 + minCross;
  }
  return false;
}

void appendPhase2MakerFill(EngineState& engine,
                           Phase2LimitOrderState& state,
                           const TradeTick& trade,
                           std::int64_t fillQty,
                           std::int64_t queueAheadBefore,
                           std::int64_t queueAheadAfter,
                           Phase2MakerQueueStats& stats) {
  if (state.order == nullptr || fillQty <= 0) return;
  state.filledQtyE8 += fillQty;
  const std::int64_t remaining = state.remainingQty();
  JsonFill fill{};
  fill.legIndex = state.order->legIndex;
  fill.orderId = state.order->id;
  fill.entryTsNs = state.order->activeTsNs;
  fill.exitTsNs = trade.tsNs;
  fill.side = state.order->side;
  fill.priceE8 = state.order->priceE8;
  fill.qtyE8 = fillQty;
  fill.realizedPnlE8 = 0;
  fill.reduceOnly = state.order->reduceOnly;
  fill.fillReason = kFillReasonPassiveQueue;
  fill.liquidity = kFillLiquidityMaker;
  fill.orderQtyE8 = state.order->qtyE8;
  fill.cumulativeFilledQtyE8 = state.filledQtyE8;
  fill.remainingQtyE8 = remaining;
  fill.avgPriceE8 = state.order->priceE8;
  fill.bookLevelQtyE8 = queueAheadBefore;
  fill.bookVisibleExecutableQtyE8 = queueAheadBefore + fillQty;
  fill.queueAheadBeforeE8 = queueAheadBefore;
  fill.queueAheadAfterE8 = queueAheadAfter;
  fill.executionQtyE8 = fillQty;
  fill.executionAvgPriceE8 = state.order->priceE8;
  fill.referencePriceE8 = state.order->priceE8;
  engine.fills.push_back(fill);
  ++stats.makerFillsCreated;
  if (remaining <= 0) {
    state.fullyFilled = true;
    ++stats.ordersFullyFilled;
  }
}

void consumeTradeForPhase2Order(EngineState& engine,
                                Phase2LimitOrderState& state,
                                const BboTop& currentBbo,
                                const TradeTick& trade,
                                Phase2MakerQueueStats& stats) {
  if (!orderActiveForTrade(state, trade.tsNs)) return;
  updateTopQueueState(state, currentBbo, trade.tsNs, stats);
  if (!state.topActive || !tradeCrossesPhase2Order(engine, state, trade)) return;
  const std::int64_t queueAheadBefore = state.queueAheadE8;
  if (trade.qtyE8 <= state.queueAheadE8) {
    state.queueAheadE8 -= trade.qtyE8;
    return;
  }
  const std::int64_t executableQty = trade.qtyE8 - state.queueAheadE8;
  state.queueAheadE8 = 0;
  const std::int64_t fillQty = executableQty < state.remainingQty() ? executableQty : state.remainingQty();
  appendPhase2MakerFill(engine,
                        state,
                        trade,
                        fillQty,
                        queueAheadBefore,
                        state.queueAheadE8,
                        stats);
}

void finishPhase2OrderStatuses(std::vector<Phase2LimitOrderState>& states) noexcept {
  for (Phase2LimitOrderState& state : states) {
    if (state.order == nullptr) continue;
    if (state.filledQtyE8 >= state.order->qtyE8) {
      state.order->status = kRecordStatusFilled;
    } else if (state.cancelScheduled) {
      state.order->status = kRecordStatusCancelled;
    } else {
      state.order->status = kRecordStatusActive;
    }
  }
}

void sortFillsForAccounting(std::vector<JsonFill>& fills) {
  std::stable_sort(fills.begin(), fills.end(), [](const JsonFill& lhs, const JsonFill& rhs) noexcept {
    if (lhs.exitTsNs != rhs.exitTsNs) return lhs.exitTsNs < rhs.exitTsNs;
    if (lhs.entryTsNs != rhs.entryTsNs) return lhs.entryTsNs < rhs.entryTsNs;
    return lhs.orderId < rhs.orderId;
  });
}

struct Phase2MarketBurstState {
  JsonOrder* order{nullptr};
  JsonFill templateFill{};
  std::int64_t targetQtyE8{0};
  std::int64_t firstEntryTsNs{0};
  std::uint64_t fillCount{0};
};

struct SweepLevel {
  std::int64_t priceE8{0};
  std::int64_t qtyE8{0};
  std::int64_t originalQtyE8{0};
};

struct SweepChunk {
  std::int64_t priceE8{0};
  std::int64_t qtyE8{0};
  std::int64_t levelQtyE8{0};
  std::int64_t cumulativeAvgPriceE8{0};
};

struct SweepBookSide {
  std::vector<SweepLevel> levels{};
  std::int64_t referencePriceE8{0};
  std::int64_t referenceQtyE8{0};
  std::int64_t visibleQtyE8{0};
  std::int64_t lastKnownPriceE8{0};
};

[[nodiscard]] bool phase2MarketStopCandidate(const JsonOrder& order) noexcept {
  if (order.action != kRecordActionOrder) return false;
  if (order.type != kRecordTypeMarket && order.type != kRecordTypeStopMarket) return false;
  if (order.service || order.warmup || order.serviceAutounwind) return false;
  if (order.activeTsNs <= 0 || order.qtyE8 <= 0) return false;
  if (order.status == kRecordStatusPending ||
      order.status == kRecordStatusRejected ||
      order.status == kRecordStatusMissing ||
      order.status == kRecordStatusNone) {
    return false;
  }
  return order.side == kRecordSideBuy || order.side == kRecordSideSell;
}

[[nodiscard]] bool sameRawSide(std::uint8_t orderSide, const TradeTick& trade) noexcept {
  return (orderSide == kRecordSideBuy && trade.side == kRecordSideBuy) ||
         (orderSide == kRecordSideSell && trade.side == kRecordSideSell);
}

[[nodiscard]] bool orderIdIn(const std::vector<std::uint64_t>& orderIds,
                             std::uint64_t orderId) noexcept {
  for (const std::uint64_t candidate : orderIds) {
    if (candidate == orderId) return true;
  }
  return false;
}

[[nodiscard]] std::int64_t saturatingAddNs(std::int64_t tsNs,
                                           std::int64_t deltaNs) noexcept {
  if (deltaNs <= 0) return tsNs;
  if (tsNs > std::numeric_limits<std::int64_t>::max() - deltaNs) {
    return std::numeric_limits<std::int64_t>::max();
  }
  return tsNs + deltaNs;
}

[[nodiscard]] std::int64_t phase2BurstArrivalTs(const Phase2MarketBurstState& state) noexcept {
  if (state.order == nullptr) return 0;
  if (state.order->type == kRecordTypeStopMarket && state.order->triggerTsNs > 0) {
    return state.order->triggerTsNs;
  }
  if (state.firstEntryTsNs > 0) return state.firstEntryTsNs;
  return state.order->activeTsNs;
}

void addSweepVisibleQty(SweepBookSide& side, std::int64_t qtyE8) noexcept {
  if (qtyE8 <= 0) return;
  if (side.visibleQtyE8 > std::numeric_limits<std::int64_t>::max() - qtyE8) {
    side.visibleQtyE8 = std::numeric_limits<std::int64_t>::max();
  } else {
    side.visibleQtyE8 += qtyE8;
  }
}

void addSweepLevel(SweepBookSide& side,
                   std::uint8_t orderSide,
                   std::int64_t priceE8,
                   std::int64_t qtyE8) {
  if (priceE8 <= 0 || qtyE8 <= 0) return;
  for (SweepLevel& level : side.levels) {
    if (level.priceE8 != priceE8) continue;
    if (level.qtyE8 > std::numeric_limits<std::int64_t>::max() - qtyE8) {
      level.qtyE8 = std::numeric_limits<std::int64_t>::max();
      level.originalQtyE8 = std::numeric_limits<std::int64_t>::max();
    } else {
      level.qtyE8 += qtyE8;
      level.originalQtyE8 += qtyE8;
    }
    addSweepVisibleQty(side, qtyE8);
    return;
  }
  side.levels.push_back(SweepLevel{priceE8, qtyE8, qtyE8});
  addSweepVisibleQty(side, qtyE8);
  if (side.lastKnownPriceE8 <= 0) {
    side.lastKnownPriceE8 = priceE8;
  } else if (orderSide == kRecordSideBuy && priceE8 > side.lastKnownPriceE8) {
    side.lastKnownPriceE8 = priceE8;
  } else if (orderSide == kRecordSideSell && priceE8 < side.lastKnownPriceE8) {
    side.lastKnownPriceE8 = priceE8;
  }
}

void sortSweepLevels(SweepBookSide& side, std::uint8_t orderSide) {
  std::sort(side.levels.begin(), side.levels.end(), [orderSide](const SweepLevel& lhs, const SweepLevel& rhs) noexcept {
    return orderSide == kRecordSideBuy ? lhs.priceE8 < rhs.priceE8 : lhs.priceE8 > rhs.priceE8;
  });
}

[[nodiscard]] bool seedSweepBookSide(const JsonOrder& order,
                                     const BboTop& bbo,
                                     const hft_trader::core::LocalOrderBookState* book,
                                     SweepBookSide& out) {
  out = {};
  if (!bbo.valid()) return false;
  const std::int64_t topPrice = order.side == kRecordSideBuy ? bbo.askPxE8 : bbo.bidPxE8;
  const std::int64_t topQty = order.side == kRecordSideBuy ? bbo.askQtyE8 : bbo.bidQtyE8;
  if (topPrice <= 0 || topQty <= 0) return false;
  out.referencePriceE8 = topPrice;
  out.referenceQtyE8 = topQty;
  out.lastKnownPriceE8 = topPrice;
  addSweepLevel(out, order.side, topPrice, topQty);

  if (book != nullptr) {
    const hft_trader::core::LocalOrderBookView view = hft_trader::core::orderBook(*book);
    const hft_trader::core::LocalOrderBookSideState* bookSide = view.side(
        order.side == kRecordSideBuy ? hft_trader::core::BookSide::Ask : hft_trader::core::BookSide::Bid);
    if (bookSide != nullptr) {
      for (std::uint32_t i = 0u; i < bookSide->activeCount.raw; ++i) {
        const hft_trader::core::LocalOrderBookLevel& level = bookSide->levels[i];
        if (level.px.raw <= 0 || level.qty.raw <= 0) continue;
        if (order.side == kRecordSideBuy) {
          if (level.px.raw <= topPrice) continue;
        } else if (level.px.raw >= topPrice) {
          continue;
        }
        addSweepLevel(out, order.side, level.px.raw, level.qty.raw);
      }
    }
  }
  sortSweepLevels(out, order.side);
  return !out.levels.empty() && out.referencePriceE8 > 0 && out.lastKnownPriceE8 > 0;
}

void compactSweepLevels(SweepBookSide& side) {
  std::vector<SweepLevel> kept;
  kept.reserve(side.levels.size());
  for (const SweepLevel& level : side.levels) {
    if (level.priceE8 > 0 && level.qtyE8 > 0) kept.push_back(level);
  }
  side.levels.swap(kept);
}

void updateLastKnownSweepPrice(SweepBookSide& side,
                               std::uint8_t orderSide,
                               std::int64_t priceE8) noexcept {
  if (priceE8 <= 0) return;
  if (side.lastKnownPriceE8 <= 0) {
    side.lastKnownPriceE8 = priceE8;
  } else if (orderSide == kRecordSideBuy && priceE8 > side.lastKnownPriceE8) {
    side.lastKnownPriceE8 = priceE8;
  } else if (orderSide == kRecordSideSell && priceE8 < side.lastKnownPriceE8) {
    side.lastKnownPriceE8 = priceE8;
  }
}

void consumeSameSideTradeFromSweep(SweepBookSide& side,
                                   std::uint8_t orderSide,
                                   const TradeTick& trade) {
  if (trade.qtyE8 <= 0 || trade.priceE8 <= 0) return;
  updateLastKnownSweepPrice(side, orderSide, trade.priceE8);
  std::int64_t remainingTradeQty = trade.qtyE8;
  for (SweepLevel& level : side.levels) {
    if (level.priceE8 <= 0 || level.qtyE8 <= 0) continue;
    if (orderSide == kRecordSideBuy) {
      if (level.priceE8 > trade.priceE8) break;
      if (level.priceE8 < trade.priceE8) {
        level.qtyE8 = 0;
        continue;
      }
    } else {
      if (level.priceE8 < trade.priceE8) break;
      if (level.priceE8 > trade.priceE8) {
        level.qtyE8 = 0;
        continue;
      }
    }
    if (remainingTradeQty <= 0) break;
    const std::int64_t takeQty = level.qtyE8 < remainingTradeQty ? level.qtyE8 : remainingTradeQty;
    level.qtyE8 -= takeQty;
    remainingTradeQty -= takeQty;
    if (remainingTradeQty <= 0) break;
  }
  compactSweepLevels(side);
}

[[nodiscard]] std::size_t lowerBoundTradeTs(const std::vector<TradeTick>& trades,
                                            std::int64_t tsNs) noexcept {
  std::size_t first = 0u;
  std::size_t count = trades.size();
  while (count > 0u) {
    const std::size_t step = count / 2u;
    const std::size_t it = first + step;
    if (trades[it].tsNs < tsNs) {
      first = it + 1u;
      count -= step + 1u;
    } else {
      count = step;
    }
  }
  return first;
}

[[nodiscard]] bool loadPhase2Trades(const std::filesystem::path& sessionPath,
                                    std::vector<TradeTick>& trades) {
  trades.clear();
  ExecutionTradeProvider provider{};
  if (!provider.open(sessionPath)) return false;
  TradeTick row{};
  while (provider.readNext(row)) trades.push_back(row);
  std::stable_sort(trades.begin(), trades.end(), [](const TradeTick& lhs, const TradeTick& rhs) noexcept {
    return lhs.tsNs < rhs.tsNs;
  });
  return true;
}

[[nodiscard]] std::int64_t applyBurstTrades(const JsonOrder& order,
                                            const std::vector<TradeTick>& trades,
                                            std::int64_t arrivalTsNs,
                                            SweepBookSide& bookSide) {
  constexpr std::int64_t kBurstWindowCapNs = 1000000ll;
  const std::int64_t deadlineTsNs = saturatingAddNs(arrivalTsNs, kBurstWindowCapNs);
  std::size_t idx = lowerBoundTradeTs(trades, arrivalTsNs);
  if (idx >= trades.size()) return arrivalTsNs;
  const TradeTick& first = trades[idx];
  if (first.tsNs > deadlineTsNs || !sameRawSide(order.side, first)) return arrivalTsNs;

  bool started = false;
  bool endedByOpposite = false;
  std::int64_t lastSameSideTsNs = arrivalTsNs;
  for (; idx < trades.size(); ++idx) {
    const TradeTick& trade = trades[idx];
    if (!sameRawSide(order.side, trade)) {
      endedByOpposite = true;
      break;
    }
    if (trade.tsNs > deadlineTsNs) break;
    consumeSameSideTradeFromSweep(bookSide, order.side, trade);
    started = true;
    lastSameSideTsNs = trade.tsNs;
  }
  if (!started) return arrivalTsNs;
  return endedByOpposite ? lastSameSideTsNs : deadlineTsNs;
}

void appendSweepChunk(std::vector<SweepChunk>& chunks,
                      std::int64_t priceE8,
                      std::int64_t qtyE8,
                      std::int64_t levelQtyE8,
                      __int128& notionalSum,
                      std::int64_t& filledQtyE8) {
  if (priceE8 <= 0 || qtyE8 <= 0) return;
  notionalSum += static_cast<__int128>(priceE8) * static_cast<__int128>(qtyE8);
  filledQtyE8 += qtyE8;
  const std::int64_t cumulativeAvg = clampI128ToI64(notionalSum / static_cast<__int128>(filledQtyE8));
  if (!chunks.empty() && chunks.back().priceE8 == priceE8) {
    chunks.back().qtyE8 += qtyE8;
    chunks.back().cumulativeAvgPriceE8 = cumulativeAvg;
    return;
  }
  chunks.push_back(SweepChunk{priceE8, qtyE8, levelQtyE8, cumulativeAvg});
}

[[nodiscard]] bool buildSweepFillChunks(const JsonOrder& order,
                                        SweepBookSide& bookSide,
                                        std::int64_t targetQtyE8,
                                        std::vector<SweepChunk>& chunks,
                                        std::int64_t& executionAvgPriceE8) {
  chunks.clear();
  executionAvgPriceE8 = 0;
  if (targetQtyE8 <= 0 || bookSide.lastKnownPriceE8 <= 0) return false;
  std::int64_t remainingQty = targetQtyE8;
  std::int64_t filledQty = 0;
  __int128 notionalSum = 0;
  for (SweepLevel& level : bookSide.levels) {
    if (remainingQty <= 0) break;
    if (level.priceE8 <= 0 || level.qtyE8 <= 0) continue;
    const std::int64_t takeQty = level.qtyE8 < remainingQty ? level.qtyE8 : remainingQty;
    appendSweepChunk(chunks, level.priceE8, takeQty, level.originalQtyE8, notionalSum, filledQty);
    level.qtyE8 -= takeQty;
    remainingQty -= takeQty;
  }
  if (remainingQty > 0) {
    appendSweepChunk(chunks, bookSide.lastKnownPriceE8, remainingQty, 0, notionalSum, filledQty);
    remainingQty = 0;
  }
  if (chunks.empty() || filledQty != targetQtyE8) return false;
  executionAvgPriceE8 = clampI128ToI64(notionalSum / static_cast<__int128>(targetQtyE8));
  return executionAvgPriceE8 > 0;
}

void appendBurstReplacementFills(const Phase2MarketBurstState& state,
                                 const SweepBookSide& bookSide,
                                 const std::vector<SweepChunk>& chunks,
                                 std::int64_t executionAvgPriceE8,
                                 std::int64_t fillTsNs,
                                 std::vector<JsonFill>& replacements) {
  if (state.order == nullptr || chunks.empty() || executionAvgPriceE8 <= 0) return;
  const std::int64_t slippage = state.order->side == kRecordSideBuy
      ? executionAvgPriceE8 - bookSide.referencePriceE8
      : bookSide.referencePriceE8 - executionAvgPriceE8;
  const std::int64_t slippageBps = slippageBpsE8(slippage, bookSide.referencePriceE8);
  const std::int64_t executionConsumedPct = consumedPctE8(state.targetQtyE8, bookSide.visibleQtyE8);
  std::int64_t cumulativeQty = 0;
  const std::uint32_t chunkCount = static_cast<std::uint32_t>(chunks.size());
  for (std::size_t i = 0u; i < chunks.size(); ++i) {
    const SweepChunk& chunk = chunks[i];
    cumulativeQty += chunk.qtyE8;
    JsonFill fill = state.templateFill;
    fill.legIndex = state.order->legIndex;
    fill.orderId = state.order->id;
    fill.entryTsNs = phase2BurstArrivalTs(state);
    fill.exitTsNs = fillTsNs;
    fill.side = state.order->side;
    fill.priceE8 = chunk.priceE8;
    fill.qtyE8 = chunk.qtyE8;
    fill.realizedPnlE8 = 0;
    fill.reduceOnly = state.order->reduceOnly;
    fill.fillReason = kFillReasonAggressiveBook;
    fill.liquidity = kFillLiquidityTaker;
    fill.orderQtyE8 = state.order->qtyE8;
    fill.cumulativeFilledQtyE8 = cumulativeQty;
    fill.remainingQtyE8 = state.order->qtyE8 > cumulativeQty ? state.order->qtyE8 - cumulativeQty : 0;
    fill.avgPriceE8 = chunk.cumulativeAvgPriceE8;
    fill.bookLevelQtyE8 = chunk.levelQtyE8;
    fill.bookVisibleExecutableQtyE8 = bookSide.visibleQtyE8;
    fill.bookConsumedPctE8 = executionConsumedPct;
    fill.queueAheadBeforeE8 = 0;
    fill.queueAheadAfterE8 = 0;
    fill.chunkIndex = static_cast<std::uint32_t>(i + 1u);
    fill.chunkCount = chunkCount;
    fill.executionQtyE8 = state.targetQtyE8;
    fill.executionAvgPriceE8 = executionAvgPriceE8;
    fill.referencePriceE8 = bookSide.referencePriceE8;
    fill.slippageE8 = slippage;
    fill.slippageBpsE8 = slippageBps;
    fill.executionBookConsumedPctE8 = executionConsumedPct;
    replacements.push_back(fill);
  }
}

void replaceBurstRepricedFills(EngineState& engine,
                               const std::vector<std::uint64_t>& handledOrderIds,
                               const std::vector<JsonFill>& replacements) {
  if (handledOrderIds.empty()) return;
  std::vector<JsonFill> rebuilt;
  rebuilt.reserve(engine.fills.size() + replacements.size());
  for (const JsonFill& fill : engine.fills) {
    if (fill.liquidity == kFillLiquidityTaker && orderIdIn(handledOrderIds, fill.orderId)) continue;
    rebuilt.push_back(fill);
  }
  for (const JsonFill& fill : replacements) rebuilt.push_back(fill);
  engine.fills.swap(rebuilt);
  sortFillsForAccounting(engine.fills);
}

std::vector<Phase2MarketBurstState> collectPhase2MarketBurstOrders(EngineState& engine) {
  std::vector<Phase2MarketBurstState> states;
  states.reserve(engine.orders.size());
  for (JsonOrder& order : engine.orders) {
    if (!phase2MarketStopCandidate(order)) continue;
    Phase2MarketBurstState state{};
    state.order = &order;
    for (const JsonFill& fill : engine.fills) {
      if (fill.orderId != order.id || fill.liquidity != kFillLiquidityTaker || fill.qtyE8 <= 0) continue;
      if (state.fillCount == 0u) state.templateFill = fill;
      if (state.firstEntryTsNs == 0 || (fill.entryTsNs > 0 && fill.entryTsNs < state.firstEntryTsNs)) {
        state.firstEntryTsNs = fill.entryTsNs;
      }
      if (state.targetQtyE8 > std::numeric_limits<std::int64_t>::max() - fill.qtyE8) {
        state.targetQtyE8 = std::numeric_limits<std::int64_t>::max();
      } else {
        state.targetQtyE8 += fill.qtyE8;
      }
      ++state.fillCount;
    }
    if (state.targetQtyE8 > 0 && state.fillCount > 0u) states.push_back(state);
  }
  std::stable_sort(states.begin(), states.end(), [](const Phase2MarketBurstState& lhs,
                                                    const Phase2MarketBurstState& rhs) noexcept {
    return phase2BurstArrivalTs(lhs) < phase2BurstArrivalTs(rhs);
  });
  return states;
}

[[nodiscard]] bool repriceMarketStopBurstsWithTrades(const std::filesystem::path& sessionPath,
                                                     const Symbol& symbol,
                                                     std::int64_t tickSizeE8,
                                                     EngineState& engine,
                                                     std::vector<std::uint64_t>& handledOrderIds,
                                                     ExecutionRepriceStats& stats) {
  handledOrderIds.clear();
  std::vector<Phase2MarketBurstState> states = collectPhase2MarketBurstOrders(engine);
  if (states.empty()) return true;

  std::vector<TradeTick> trades;
  if (!loadPhase2Trades(sessionPath, trades) || trades.empty()) return true;

  hft_trader::core::LocalOrderBookConfig bookConfig{};
  bookConfig.maxStoredLevelsPerSide.raw = rawdata::kMaxOrderbookLevels;
  bookConfig.tickSizeRaw = tickSizeE8;
  std::unique_ptr<hft_trader::core::LocalOrderBookState> depthBook{};
  std::unique_ptr<ExecutionDepthProvider> depthProvider{};
  bool depthOpened = false;
  bool depthAvailable = false;

  ExecutionBboProvider bboProvider{};
  if (!bboProvider.open(sessionPath)) return false;

  std::vector<JsonFill> replacements;
  replacements.reserve(engine.fills.size());
  std::vector<SweepChunk> chunks;
  chunks.reserve(rawdata::kMaxOrderbookLevels + 1u);
  for (Phase2MarketBurstState& state : states) {
    const std::int64_t arrivalTsNs = phase2BurstArrivalTs(state);
    if (state.order == nullptr || arrivalTsNs <= 0 || !bboProvider.advanceTo(arrivalTsNs)) continue;
    const BboTop& bbo = bboProvider.top();
    const hft_trader::core::LocalOrderBookState* currentDepthBook = nullptr;
    if (!depthOpened) {
      depthBook.reset(new (std::nothrow) hft_trader::core::LocalOrderBookState{});
      depthProvider.reset(new (std::nothrow) ExecutionDepthProvider{});
      depthAvailable = depthBook && depthProvider && depthProvider->open(sessionPath, symbol, bookConfig, *depthBook);
      depthOpened = true;
    }
    if (depthAvailable && depthProvider->advanceTo(arrivalTsNs) && depthProvider->ready()) {
      const BookTop depthTop = bestBookTop(depthProvider->book());
      if (depthTop.valid() && depthTop.bidPxE8 < depthTop.askPxE8 && depthTopCompatibleWithBbo(depthTop, bbo)) {
        currentDepthBook = &depthProvider->book();
      }
    }
    SweepBookSide bookSide{};
    if (!seedSweepBookSide(*state.order, bbo, currentDepthBook, bookSide)) continue;
    const std::int64_t fillTsNs = applyBurstTrades(*state.order, trades, arrivalTsNs, bookSide);
    std::int64_t executionAvgPrice = 0;
    if (!buildSweepFillChunks(*state.order, bookSide, state.targetQtyE8, chunks, executionAvgPrice)) continue;
    appendBurstReplacementFills(state, bookSide, chunks, executionAvgPrice, fillTsNs, replacements);
    handledOrderIds.push_back(state.order->id);
    const std::uint64_t repricedChunkCount = chunks.size();
    stats.fillsRepriced += repricedChunkCount;
    if (chunks.size() == 1u && executionAvgPrice == bookSide.referencePriceE8) {
      stats.fillsBboRepriced += repricedChunkCount;
    } else {
      stats.fillsDepthVwapRepriced += repricedChunkCount;
    }
  }
  if (depthProvider) stats.depthRowsRead += depthProvider->rowsRead();
  replaceBurstRepricedFills(engine, handledOrderIds, replacements);
  return true;
}

}  // namespace

bool repriceMakerLimitQueueWithBboTrades(const std::filesystem::path& sessionPath,
                                         EngineState& engine,
                                         std::int64_t finalTsNs,
                                         Phase2MakerQueueStats& stats) noexcept {
  stats = {};
  std::vector<Phase2LimitOrderState> states = collectPhase2LimitOrders(engine, finalTsNs);
  stats.ordersVisited = states.size();
  if (states.empty()) return true;

  ExecutionBboProvider bboProvider{};
  ExecutionTradeProvider tradeProvider{};
  if (!bboProvider.open(sessionPath) || !tradeProvider.open(sessionPath)) {
    return false;
  }
  removePhase2PassiveMakerFills(engine, states, stats);

  BboTop currentBbo{};
  BboTop nextBbo{};
  TradeTick nextTrade{};
  bool hasBbo = bboProvider.readNext(nextBbo);
  bool hasTrade = tradeProvider.readNext(nextTrade);
  while (hasBbo || hasTrade) {
    const bool takeTrade = hasTrade && (!hasBbo || nextTrade.tsNs <= nextBbo.tsNs);
    if (takeTrade) {
      for (Phase2LimitOrderState& state : states) {
        consumeTradeForPhase2Order(engine, state, currentBbo, nextTrade, stats);
      }
      hasTrade = tradeProvider.readNext(nextTrade);
    } else {
      currentBbo = nextBbo;
      for (Phase2LimitOrderState& state : states) {
        updateTopQueueState(state, currentBbo, currentBbo.tsNs, stats);
      }
      hasBbo = bboProvider.readNext(nextBbo);
    }
  }

  finishPhase2OrderStatuses(states);
  sortFillsForAccounting(engine.fills);
  return true;
}

bool repriceFillsWithDepth(const std::filesystem::path& sessionPath,
                           const Symbol& symbol,
                           std::int64_t tickSizeE8,
                           EngineState& engine,
                           ExecutionRepriceStats& stats) noexcept {
  std::vector<std::uint64_t> burstRepricedOrderIds;
  const bool burstOk = repriceMarketStopBurstsWithTrades(sessionPath,
                                                        symbol,
                                                        tickSizeE8,
                                                        engine,
                                                        burstRepricedOrderIds,
                                                        stats);
  std::vector<JsonFill>& fills = engine.fills;
  std::uint64_t takerFillCount = 0;
  for (const JsonFill& fill : fills) {
    if (orderIdIn(burstRepricedOrderIds, fill.orderId)) continue;
    if (fill.liquidity == kFillLiquidityTaker) ++takerFillCount;
  }
  if (takerFillCount == 0u) {
    stats.fillsVisited += fills.size();
    return burstOk;
  }

  hft_trader::core::LocalOrderBookConfig bookConfig{};
  bookConfig.maxStoredLevelsPerSide.raw = rawdata::kMaxOrderbookLevels;
  bookConfig.tickSizeRaw = tickSizeE8;
  std::unique_ptr<hft_trader::core::LocalOrderBookState> initialBook{};
  std::unique_ptr<ExecutionDepthProvider> provider{};
  bool depthOpened = false;
  bool depthAvailable = false;
  bool depthDisabled = false;
  std::uint32_t consecutiveDepthRejects = 0u;
  ExecutionBboProvider bboProvider{};
  if (!bboProvider.open(sessionPath)) {
    stats.fillsVisited += fills.size();
    stats.fillsMissingBbo = takerFillCount;
    stats.fillsMissingDepth = takerFillCount;
    return false;
  }

  for (JsonFill& fill : fills) {
    ++stats.fillsVisited;
    if (orderIdIn(burstRepricedOrderIds, fill.orderId)) continue;
    if (fill.liquidity != kFillLiquidityTaker) continue;
    if (!bboProvider.advanceTo(fill.exitTsNs)) {
      ++stats.fillsMissingBbo;
      ++stats.fillsMissingDepth;
      continue;
    }
    const BboTop& bbo = bboProvider.top();
    if (bboTopQtyForFill(fill, bbo) >= fill.qtyE8 && repriceFillFromBbo(fill, bbo)) {
      ++stats.fillsRepriced;
      ++stats.fillsBboRepriced;
      consecutiveDepthRejects = 0u;
      continue;
    }
    bool repriced = false;
    if (!depthDisabled) {
      if (!depthOpened) {
        initialBook.reset(new (std::nothrow) hft_trader::core::LocalOrderBookState{});
        provider.reset(new (std::nothrow) ExecutionDepthProvider{});
        depthAvailable = initialBook && provider && provider->open(sessionPath, symbol, bookConfig, *initialBook);
        depthOpened = true;
      }
      bool depthRejected = false;
      if (!depthAvailable) {
        ++stats.fillsDepthRejectedUnseeded;
        depthRejected = true;
      } else if (!provider->advanceTo(fill.exitTsNs) || !provider->ready()) {
        ++stats.fillsDepthRejectedUnseeded;
        depthRejected = true;
      } else {
        const BookTop depthTop = bestBookTop(provider->book());
        if (!depthTop.valid()) {
          ++stats.fillsDepthRejectedUnseeded;
          depthRejected = true;
        } else if (depthTop.bidPxE8 >= depthTop.askPxE8) {
          ++stats.fillsDepthRejectedCrossed;
          depthRejected = true;
        } else if (!depthTopCompatibleWithBbo(depthTop, bbo)) {
          ++stats.fillsDepthRejectedStale;
          depthRejected = true;
        } else {
          bool usedDepthBeyondBbo = false;
          if (repriceFillFromBboDepth(fill, bbo, provider->book(), usedDepthBeyondBbo)) {
            ++stats.fillsRepriced;
            if (usedDepthBeyondBbo) ++stats.fillsDepthVwapRepriced;
            else ++stats.fillsBboRepriced;
            repriced = true;
            consecutiveDepthRejects = 0u;
          } else {
            depthRejected = true;
          }
        }
      }
      if (depthRejected) {
        ++consecutiveDepthRejects;
        if (consecutiveDepthRejects >= kDisableDepthAfterRejects) depthDisabled = true;
      }
    } else {
      ++stats.fillsDepthRejectedStale;
    }
    if (!repriced) {
      if (repriceFillFromBbo(fill, bbo)) {
        ++stats.fillsRepriced;
        ++stats.fillsBboRepriced;
      } else {
        ++stats.fillsMissingDepth;
      }
    }
  }
  if (provider) stats.depthRowsRead += provider->rowsRead();
  return true;
}

void rebuildAccountingFromFills(EngineState& engine,
                                std::int64_t startTsNs,
                                std::int64_t finalTsNs) {
  const bool wasLiquidated = engine.liquidated;
  const std::int64_t liquidationTsNs = engine.liquidationTsNs;
  const std::int64_t liquidationEquityE8 = engine.liquidationEquityE8;
  const bool wasRiskStopped = engine.riskStopped;
  const std::int64_t riskStopTsNs = engine.riskStopTsNs;
  const std::int64_t riskStopEquityE8 = engine.riskStopEquityE8;
  const std::int64_t riskStopThresholdEquityE8 = engine.riskStopThresholdEquityE8;

  engine.netQtyRaw = 0;
  engine.entryPriceE8 = 0;
  engine.grossRealizedPnlE8 = 0;
  engine.netRealizedPnlE8 = 0;
  engine.feesPaidE8 = 0;
  engine.walletBalanceE8 = engine.initialBalanceE8;
  engine.position = {};
  engine.equityPoints.clear();
  engine.riskSnapshots.clear();
  engine.lastRiskSnapshotTsNs = 0;
  engine.lastRiskSnapshotUnrealizedPnlE8 = 0;
  engine.hasRiskSnapshot = false;
  engine.liquidated = false;
  engine.riskStopped = false;
  engine.liquidationTsNs = 0;
  engine.liquidationEquityE8 = 0;
  engine.riskStopTsNs = 0;
  engine.riskStopEquityE8 = 0;
  engine.riskStopThresholdEquityE8 = 0;

  appendEquityPoint(engine, startTsNs);
  appendRiskSnapshot(engine, startTsNs, kRiskSnapshotReasonStart);
  for (JsonFill& fill : engine.fills) {
    if (fill.priceE8 <= 0 || fill.qtyE8 <= 0) continue;
    std::int64_t netRealized = 0;
    updatePosition(engine,
                   fillPositionSide(fill),
                   fill.priceE8,
                   fill.qtyE8,
                   fill.exitTsNs,
                   feeBpsForFill(engine, fill),
                   netRealized);
    fill.realizedPnlE8 = netRealized;
    appendEquityPoint(engine, fill.exitTsNs);
    appendRiskSnapshot(engine, fill.exitTsNs, kRiskSnapshotReasonFill);
  }
  if (wasLiquidated) {
    hardLiquidate(engine, liquidationTsNs, liquidationEquityE8);
  } else if (wasRiskStopped) {
    stopForTraderRisk(engine, riskStopTsNs, riskStopEquityE8, riskStopThresholdEquityE8);
  }
  if (finalTsNs > 0) {
    appendEquityPoint(engine, finalTsNs);
    appendRiskSnapshot(engine, finalTsNs, kRiskSnapshotReasonFinal);
  }
}

}  // namespace hft_backtest::detail
