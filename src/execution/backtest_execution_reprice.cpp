#include "execution/backtest_execution_reprice.hpp"

#include "execution/backtest_execution_depth.hpp"

#include "hft_trader/core/market/LocalOrderBook.hpp"

#include <fstream>
#include <limits>
#include <string>
#include <string_view>

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

 private:
  std::ifstream stream_{};
  BboTop latest_{};
  BboTop pending_{};
  bool pendingValid_{false};
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

bool repriceFillsWithDepth(const std::filesystem::path& sessionPath,
                           const Symbol& symbol,
                           std::int64_t tickSizeE8,
                           std::vector<JsonFill>& fills,
                           ExecutionRepriceStats& stats) noexcept {
  std::uint64_t takerFillCount = 0;
  for (const JsonFill& fill : fills) {
    if (fill.liquidity == kFillLiquidityTaker) ++takerFillCount;
  }
  if (takerFillCount == 0u) {
    stats.fillsVisited = fills.size();
    return true;
  }

  hft_trader::core::LocalOrderBookConfig bookConfig{};
  bookConfig.maxStoredLevelsPerSide.raw = rawdata::kMaxOrderbookLevels;
  bookConfig.tickSizeRaw = tickSizeE8;
  hft_trader::core::LocalOrderBookState book{};
  ExecutionDepthProvider provider{};
  bool depthOpened = false;
  bool depthAvailable = false;
  bool depthDisabled = false;
  std::uint32_t consecutiveDepthRejects = 0u;
  ExecutionBboProvider bboProvider{};
  if (!bboProvider.open(sessionPath)) {
    stats.fillsVisited = fills.size();
    stats.fillsMissingBbo = takerFillCount;
    stats.fillsMissingDepth = takerFillCount;
    return false;
  }

  for (JsonFill& fill : fills) {
    ++stats.fillsVisited;
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
        depthAvailable = provider.open(sessionPath, symbol, bookConfig, book);
        depthOpened = true;
      }
      bool depthRejected = false;
      if (!depthAvailable) {
        ++stats.fillsDepthRejectedUnseeded;
        depthRejected = true;
      } else if (!provider.advanceTo(fill.exitTsNs) || !provider.ready()) {
        ++stats.fillsDepthRejectedUnseeded;
        depthRejected = true;
      } else {
        const BookTop depthTop = bestBookTop(provider.book());
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
          if (repriceFillFromBboDepth(fill, bbo, provider.book(), usedDepthBeyondBbo)) {
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
  stats.depthRowsRead = depthOpened ? provider.rowsRead() : 0u;
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
