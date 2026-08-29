#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "hft_backtest/status.hpp"
#include "hftrec/corpus_contract/EventRows.hpp"

namespace hft_backtest::detail {

constexpr std::size_t kSessionTimelineKindCount = 10u;

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
  hftrec::replay::EventArrival arrival{};
};

struct BookTickerRow {
  std::int64_t bidPriceE8{0};
  std::int64_t bidQtyE8{0};
  std::int64_t askPriceE8{0};
  std::int64_t askQtyE8{0};
  std::int64_t tsNs{0};
  std::int64_t ingestSeq{0};
  hftrec::replay::EventArrival arrival{};
  bool bidPresent{true};
  bool askPresent{true};
  // A caught-up parser consumer refreshes receive evidence for this retained
  // BBO without emitting another replaceable-state strategy event.
  bool freshnessOnly{false};
};

struct PriceLimitRow {
  std::int64_t tsNs{0};
  std::int64_t buyLimitE8{0};
  std::int64_t sellLimitE8{0};
  std::uint8_t enabled{0};
  std::int64_t ingestSeq{0};
  hftrec::replay::EventArrival arrival{};
};

struct LiquidationRow {
  std::int64_t priceE8{0};
  std::int64_t qtyE8{0};
  std::int64_t side{0};
  std::int64_t tsNs{0};
  std::int64_t avgPriceE8{0};
  std::int64_t filledQtyE8{0};
  std::int64_t orderType{0};
  std::int64_t timeInForce{0};
  std::int64_t status{0};
  std::int64_t sourceMode{0};
  std::int64_t ingestSeq{0};
  hftrec::replay::EventArrival arrival{};
};

struct MarkPriceRow {
  std::int64_t tsNs{0};
  std::int64_t markPriceE8{0};
  std::int64_t ingestSeq{0};
  hftrec::replay::EventArrival arrival{};
};

struct IndexPriceRow {
  std::int64_t tsNs{0};
  std::int64_t indexPriceE8{0};
  std::int64_t ingestSeq{0};
  hftrec::replay::EventArrival arrival{};
};

struct FundingRow {
  std::int64_t tsNs{0};
  std::int64_t fundingRateE8{0};
  std::int64_t fundingTsNs{0};
  std::int64_t nextFundingTsNs{0};
  std::int64_t ingestSeq{0};
  hftrec::replay::EventArrival arrival{};
};

struct OpenInterestRow {
  std::int64_t tsNs{0};
  std::int64_t openInterestE8{0};
  std::int64_t openInterestValueE8{0};
  std::int64_t ingestSeq{0};
  hftrec::replay::EventArrival arrival{};
};

struct DepthRow {
  std::int64_t tsNs{0};
  std::int64_t ingestSeq{0};
  hftrec::replay::EventArrival arrival{};
  std::vector<PriceLevelRow> levels{};
  bool resetBookBeforeApply{false};
  // Snapshot plus replay deltas were committed by the parser as one local
  // recovery transaction. The venue-side replay must apply that recovery at
  // its captured application arrival, not reorder its parts by their distinct
  // original exchange timestamps.
  bool applicationAtomicRebase{false};
};

struct CandleRow {
  std::int64_t tier{0};
  std::int64_t tsNs{0};
  std::int64_t highE8{0};
  std::int64_t lowE8{0};
  std::int64_t quoteAmountE8{0};
  std::int64_t openE8{0};
  std::int64_t closeE8{0};
  std::int64_t volumeE8{0};
  bool hasOhlc{false};
  hftrec::replay::EventArrival arrival{};
};

struct InstrumentMetadata {
  std::int64_t tickSizeE8{1};
  std::int64_t lotSizeE8{1};
  std::int64_t contractBaseQtyE8{0};
  std::int64_t priceBasisQtyE8{0};
  std::int64_t expiryUtcNs{0};
};

struct SessionLoadPlan {
  bool loadTrades{true};
  bool loadBookTickers{true};
  bool loadPriceLimits{true};
  bool loadDepths{false};
  bool loadCandles{false};
  bool loadLiquidations{false};
  bool loadMarkPrices{false};
  bool loadIndexPrices{false};
  bool loadFunding{false};
  bool loadOpenInterest{false};
  bool requireTrades{false};
  bool requireBookTickers{false};
  bool requirePriceLimits{false};
  bool requireDepths{false};
  bool requireCandles{false};
  bool requireLiquidations{false};
  bool requireMarkPrices{false};
  bool requireIndexPrices{false};
  bool requireFunding{false};
  bool requireOpenInterest{false};
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
  std::vector<LiquidationRow> liquidations{};
  std::vector<MarkPriceRow> markPrices{};
  std::vector<IndexPriceRow> indexPrices{};
  std::vector<FundingRow> funding{};
  std::vector<OpenInterestRow> openInterest{};
  InstrumentMetadata instrument{};
  std::int64_t tradeHistoryWarmupSec{0};
  std::int64_t tradeHistoryRequestedStartNs{0};
  std::int64_t tradeHistoryRequestedEndNs{0};
  std::int64_t tradeHistoryRows{0};
  std::int64_t firstTsNs{0};
  std::int64_t lastTsNs{0};
  std::uint64_t firstReceiveMonotonicNs{0u};
  std::uint64_t lastReceiveMonotonicNs{0u};
  std::int64_t firstReceiveRealtimeNs{0};
  std::int64_t lastReceiveRealtimeNs{0};
  std::uint64_t replayAnchorMonotonicNs{0u};
  std::int64_t replayAnchorReceiveNs{0};
  std::uint64_t realtimeRegressionCount{0u};
  std::uint64_t monotonicNonIncreasingCount{0u};
  std::uint64_t exchangeAheadOfReceiveCount{0u};
  std::uint64_t exchangeTimestampMissingCount{0u};
  std::array<bool, kSessionTimelineKindCount> timelineChannelSorted{};
  bool timelineOrderKnown{false};
  bool binaryMarketCorpus{false};
  std::vector<std::string> warnings{};
};

Status loadSession(const std::filesystem::path& sessionPath,
                   const SessionLoadPlan& plan,
                   SessionData& out,
                   std::string& error) noexcept;
Status loadSession(const std::filesystem::path& sessionPath, SessionData& out, std::string& error) noexcept;

}  // namespace hft_backtest::detail
