#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "hft_backtest/Status.hpp"
#include "hftrec/CorpusContract/EventRows.hpp"
#include "hftrec/CorpusContract/BinaryMarketCorpusFormat.hpp"
#include "cxet/Market/Depth.hpp"
#include "cxet/Api/Market/PublicMarketDataTypes.hpp"

namespace hft_backtest::detail {

constexpr std::size_t kSessionTimelineKindCount = 5u;

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
  // Current binary corpus preserves its declared native key. Legacy JSON
  // has no shape field and retains the existing unavailable-identity path.
  cxet::api::market::PublicMarketEventIdentity nativeIdentity{
      .origin = cxet::api::market::PublicMarketIdentityOrigin::SourcePosition,
      .nativeShape = cxet::api::market::PublicMarketNativeIdentityShape::None,
      .scope = cxet::api::market::PublicMarketIdentityScope::Lane};
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

struct DepthRow {
  std::int64_t tsNs{0};
  std::int64_t ingestSeq{0};
  hftrec::replay::EventArrival arrival{};
  std::vector<PriceLevelRow> levels{};
  bool resetBookBeforeApply{false};
  // Preserve the immutable binary corpus sequence, without replacing it with
  // the local ordering coordinate used by legacy text corpora.
  cxet::market::DepthSequence capturedSequence{};
  std::uint64_t capturedPublicationSequence{0u};
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
  std::int64_t executionQuantityStepE8{};
  bool hasExecutionQuantityGrid{false};
  std::int64_t contractBaseQtyE8{0};
  std::int64_t priceBasisQtyE8{0};
  std::int64_t expiryUtcNs{0};
};

struct SessionLoadPlan {
  bool loadTrades{true};
  bool loadBookTickers{true};
  bool loadDepths{false};
  bool loadCandles{false};
  bool requireTrades{false};
  bool requireBookTickers{false};
  bool requireDepths{false};
  bool requireCandles{false};
};

struct SourceHealthRow {
  hftrec::corpus::BinaryMarketRecord record{};
  hftrec::replay::EventArrival arrival{};
  bool unhealthy{};
};
struct SourceLifecycleRow {
  hftrec::corpus::BinaryMarketSourceLifecyclePayload payload{};
  hftrec::replay::EventArrival arrival{};
  std::int64_t exchangeTsNs{};
};
struct SessionData {
  std::filesystem::path sessionPath{};
  std::string exchange{};
  std::string market{};
  std::string symbol{};
  std::vector<TradeRow> trades{};
  std::vector<TradeRow> tradeWarmup{};
  std::vector<BookTickerRow> bookTickers{};
  std::vector<DepthRow> depths{};
  std::vector<CandleRow> candles{};
  std::vector<SourceLifecycleRow> sourceLifecycle{};
  std::vector<SourceHealthRow> sourceHealth{};
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
  std::uint32_t capturedSourceId{};
  std::uint32_t canonicalInstrumentId{};
  std::uint8_t assetDomainRaw{};
  std::uint8_t capturedMarketKindRaw{};
  std::uint8_t capturedMarketRaw{};
  hftrec::corpus::BinaryMarketQuantityConversion capturedQuantityConversion{};
  std::uint32_t quoteAssetId{};
  bool capturedInitiallyPresent{true};
  std::uint64_t capturedSourceGeneration{};
  std::uint64_t capturedDirectoryRevision{};
  std::uint16_t capturedAvailableChannels{};
  std::uint16_t capturedUnhealthyChannels{};
  std::vector<std::string> warnings{};
};

Status loadSession(const std::filesystem::path& sessionPath,
                   const SessionLoadPlan& plan,
                   SessionData& out,
                   std::string& error) noexcept;
Status loadSession(const std::filesystem::path& sessionPath, SessionData& out, std::string& error) noexcept;

}  // namespace hft_backtest::detail
