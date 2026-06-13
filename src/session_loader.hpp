#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "hft_backtest/status.hpp"

namespace hft_backtest::detail {

struct PriceLevelRow {
  std::int64_t priceE8{0};
  std::int64_t qtyE8{0};
  std::int64_t side{0};
};

struct TradeRow {
  std::int64_t priceE8{0};
  std::int64_t qtyE8{0};
  std::int64_t side{0};
  std::int64_t tsNs{0};
  std::int64_t ingestSeq{0};
};

struct BookTickerRow {
  std::int64_t bidPriceE8{0};
  std::int64_t bidQtyE8{0};
  std::int64_t askPriceE8{0};
  std::int64_t askQtyE8{0};
  std::int64_t tsNs{0};
  std::int64_t ingestSeq{0};
};

struct PriceLimitRow {
  std::int64_t buyLimitE8{0};
  std::int64_t sellLimitE8{0};
  std::uint8_t enabled{0};
  std::int64_t tsNs{0};
  std::int64_t ingestSeq{0};
};

struct DepthRow {
  std::int64_t tsNs{0};
  std::int64_t ingestSeq{0};
  std::vector<PriceLevelRow> levels{};
};

struct CandleRow {
  std::int64_t tier{0};
  std::int64_t tsNs{0};
  std::int64_t highE8{0};
  std::int64_t lowE8{0};
  std::int64_t quoteAmountE8{0};
};

struct SnapshotDocument {
  std::int64_t tsNs{0};
  std::vector<PriceLevelRow> levels{};
};

struct InstrumentMetadata {
  std::int64_t tickSizeE8{1};
  std::int64_t lotSizeE8{1};
  std::int64_t contractBaseQtyE8{100000000};
};

struct SessionLoadPlan {
  bool loadTrades{true};
  bool loadBookTickers{true};
  bool loadPriceLimits{true};
  bool loadDepths{true};
  bool loadCandles{false};
  bool requireTrades{false};
  bool requireBookTickers{false};
  bool requirePriceLimits{false};
  bool requireDepths{false};
  bool requireCandles{false};
};

struct SessionData {
  std::filesystem::path sessionPath{};
  std::string exchange{};
  std::string market{};
  std::string symbol{};
  std::vector<TradeRow> trades{};
  std::vector<TradeRow> tradeWarmup{};
  std::vector<BookTickerRow> bookTickers{};
  std::vector<PriceLimitRow> priceLimits{};
  std::vector<DepthRow> depths{};
  std::vector<CandleRow> candles{};
  SnapshotDocument snapshot{};
  bool hasSnapshot{false};
  InstrumentMetadata instrument{};
  std::int64_t tradeHistoryWarmupSec{0};
  std::int64_t tradeHistoryRequestedStartNs{0};
  std::int64_t tradeHistoryRequestedEndNs{0};
  std::int64_t tradeHistoryRows{0};
  std::int64_t firstTsNs{0};
  std::int64_t lastTsNs{0};
};

Status loadSession(const std::filesystem::path& sessionPath,
                   const SessionLoadPlan& plan,
                   SessionData& out,
                   std::string& error) noexcept;
Status loadSession(const std::filesystem::path& sessionPath, SessionData& out, std::string& error) noexcept;

}  // namespace hft_backtest::detail
