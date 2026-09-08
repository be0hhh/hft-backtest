#include "BinaryCorpusLoader.hpp"

#include "hftrec/CorpusContract/BinaryMarketCorpusReader.hpp"
#include "cxet/Primitives/Buf/CanonConstants.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace hft_backtest::detail {
namespace {

namespace corpus = hftrec::corpus;

inline constexpr std::uint8_t kCanonicalScaleDigits = 8u;

[[nodiscard]] Status convertStatus(hftrec::Status status) noexcept {
  switch (status) {
    case hftrec::Status::Ok:
      return Status::Ok;
    case hftrec::Status::InvalidArgument:
      return Status::InvalidArgument;
    case hftrec::Status::IoError:
      return Status::IoError;
    case hftrec::Status::OutOfRange:
      return Status::CorruptData;
    case hftrec::Status::CorruptData:
      return Status::CorruptData;
    case hftrec::Status::Unimplemented:
      return Status::Unsupported;
    case hftrec::Status::Cancelled:
      return Status::Cancelled;
    case hftrec::Status::Unknown:
      return Status::Unknown;
  }
  return Status::Unknown;
}

[[nodiscard]] std::string_view fixedText(const char* data,
                                         std::size_t capacity,
                                         std::uint8_t bytes) noexcept {
  return data && bytes != 0u && bytes <= capacity
      ? std::string_view{data, bytes}
      : std::string_view{};
}

void appendWarning(SessionData& output, std::string warning) {
  if (std::find(output.warnings.begin(), output.warnings.end(), warning) ==
      output.warnings.end()) {
    output.warnings.push_back(std::move(warning));
  }
}

[[nodiscard]] const char* channelName(corpus::BinaryMarketChannel channel) {
  switch (channel) {
    case corpus::BinaryMarketChannel::BookTicker:
      return "bookticker";
    case corpus::BinaryMarketChannel::Trade:
      return "trades";
    case corpus::BinaryMarketChannel::Depth:
      return "depth";
    case corpus::BinaryMarketChannel::Liquidation:
      return "liquidations";
    case corpus::BinaryMarketChannel::MarkPrice:
      return "mark_price";
    case corpus::BinaryMarketChannel::IndexPrice:
      return "index_price";
    case corpus::BinaryMarketChannel::Funding:
      return "funding";
    case corpus::BinaryMarketChannel::PriceLimit:
      return "price_limit";
  }
  return "unknown";
}

struct ChannelPlan final {
  std::uint16_t loadMask{0u};
  std::uint16_t requiredMask{0u};
};

void includeChannel(ChannelPlan& output,
                    corpus::BinaryMarketChannel channel,
                    bool load,
                    bool required) noexcept {
  if (load) output.loadMask |= corpus::binaryMarketChannelBit(channel);
  if (required) output.requiredMask |= corpus::binaryMarketChannelBit(channel);
}

[[nodiscard]] ChannelPlan channelPlan(const SessionLoadPlan& plan) noexcept {
  ChannelPlan output{};
  includeChannel(output, corpus::BinaryMarketChannel::Trade,
                 plan.loadTrades, plan.requireTrades);
  includeChannel(output, corpus::BinaryMarketChannel::BookTicker,
                 plan.loadBookTickers, plan.requireBookTickers);
  includeChannel(output, corpus::BinaryMarketChannel::PriceLimit,
                 plan.loadPriceLimits, plan.requirePriceLimits);
  includeChannel(output, corpus::BinaryMarketChannel::Depth,
                 plan.loadDepths, plan.requireDepths);
  includeChannel(output, corpus::BinaryMarketChannel::Liquidation,
                 plan.loadLiquidations, plan.requireLiquidations);
  includeChannel(output, corpus::BinaryMarketChannel::MarkPrice,
                 plan.loadMarkPrices, plan.requireMarkPrices);
  includeChannel(output, corpus::BinaryMarketChannel::IndexPrice,
                 plan.loadIndexPrices, plan.requireIndexPrices);
  includeChannel(output, corpus::BinaryMarketChannel::Funding,
                 plan.loadFunding, plan.requireFunding);
  return output;
}

[[nodiscard]] bool rescaleToE8(std::int64_t input,
                               std::uint8_t scaleDigits,
                               std::int64_t& output) noexcept {
  if (scaleDigits > 18u) return false;
  if (scaleDigits == kCanonicalScaleDigits) {
    output = input;
    return true;
  }
  __int128 factor = 1;
  const std::uint8_t difference = scaleDigits < kCanonicalScaleDigits
      ? static_cast<std::uint8_t>(kCanonicalScaleDigits - scaleDigits)
      : static_cast<std::uint8_t>(scaleDigits - kCanonicalScaleDigits);
  for (std::uint8_t index = 0u; index < difference; ++index) factor *= 10;
  if (scaleDigits < kCanonicalScaleDigits) {
    const __int128 value = static_cast<__int128>(input) * factor;
    if (value < std::numeric_limits<std::int64_t>::min() ||
        value > std::numeric_limits<std::int64_t>::max()) {
      return false;
    }
    output = static_cast<std::int64_t>(value);
    return true;
  }
  if (static_cast<__int128>(input) % factor != 0) return false;
  output = static_cast<std::int64_t>(static_cast<__int128>(input) / factor);
  return true;
}

[[nodiscard]] bool rescaleToE8(std::uint64_t input,
                               std::uint8_t scaleDigits,
                               std::int64_t& output) noexcept {
  if (input > static_cast<std::uint64_t>(
                  std::numeric_limits<std::int64_t>::max())) {
    return false;
  }
  return rescaleToE8(static_cast<std::int64_t>(input), scaleDigits, output);
}

template <typename Payload>
[[nodiscard]] const Payload* checkedPayload(
    const corpus::BinaryMarketRecord& record) noexcept {
  if (record.header.payloadBytes != sizeof(Payload)) return nullptr;
  const auto* payload = corpus::binaryMarketPayload<Payload>(&record);
  const auto firstTail = record.payload.begin() + sizeof(Payload);
  if (!std::all_of(firstTail, record.payload.end(),
                   [](std::byte value) noexcept {
                     return value == std::byte{};
                   })) {
    return nullptr;
  }
  return payload;
}

[[nodiscard]] const corpus::BinaryMarketBookTickerPayload*
checkedBookTickerPayload(
    const corpus::BinaryMarketRecord& record) noexcept {
  const auto* payload =
      checkedPayload<corpus::BinaryMarketBookTickerPayload>(record);
  const bool bidPresent =
      (record.header.sourceFlags &
       corpus::BinaryMarketSourceBookTickerBidPresent) != 0u;
  const bool askPresent =
      (record.header.sourceFlags &
       corpus::BinaryMarketSourceBookTickerAskPresent) != 0u;
  return payload && (bidPresent || askPresent) &&
          (bidPresent
               ? payload->bidPriceRaw > 0 && payload->bidQtyRaw >= 0
               : payload->bidPriceRaw == 0 && payload->bidQtyRaw == 0) &&
          (askPresent
               ? payload->askPriceRaw > 0 && payload->askQtyRaw >= 0
               : payload->askPriceRaw == 0 && payload->askQtyRaw == 0) &&
          (!bidPresent || !askPresent ||
           payload->bidPriceRaw <= payload->askPriceRaw)
      ? payload
      : nullptr;
}

[[nodiscard]] std::uint32_t arrivalFlags(
    const corpus::BinaryMarketRecordHeader& header) noexcept {
  std::uint32_t flags = hftrec::replay::EventArrivalApplicationFrame;
  if ((header.flags & corpus::BinaryMarketRecordRealtimeRegression) != 0u)
    flags |= hftrec::replay::EventArrivalRealtimeRegression;
  if ((header.flags &
       corpus::BinaryMarketRecordMonotonicNonIncreasing) != 0u) {
    flags |= hftrec::replay::EventArrivalMonotonicNonIncreasing;
  }
  if ((header.flags & corpus::BinaryMarketRecordExchangeAheadOfReceive) != 0u)
    flags |= hftrec::replay::EventArrivalExchangeAheadOfReceive;
  if ((header.flags &
       corpus::BinaryMarketRecordExchangeTimestampMissing) != 0u) {
    flags |= hftrec::replay::EventArrivalExchangeTimestampMissing;
  }
  return flags;
}

[[nodiscard]] hftrec::replay::EventArrival makeArrival(
    const corpus::BinaryMarketManifest& manifest,
    const corpus::BinaryMarketRecordHeader& header) noexcept {
  return {
      .receiveRealtimeNs = header.receiveRealtimeNs,
      .receiveMonotonicNs = header.receiveMonotonicNs,
      .producerEpoch = manifest.producerEpoch,
      .sourceGeneration = header.sourceGeneration,
      .sessionEpoch = header.sessionEpoch,
      .frameSequence = header.frameSequence,
      .shardSequence = header.shardSequence,
      .sourceId = header.sourceId,
      .shardId = header.shardIndex,
      .eventOrdinal = header.eventOrdinal,
      .flags = arrivalFlags(header),
  };
}

void includeEvidence(SessionData& output,
                     const corpus::BinaryMarketRecordHeader& header) noexcept {
  if (output.firstReceiveMonotonicNs == 0u ||
      header.receiveMonotonicNs < output.firstReceiveMonotonicNs) {
    output.firstReceiveMonotonicNs = header.receiveMonotonicNs;
    output.firstReceiveRealtimeNs = header.receiveRealtimeNs;
  }
  if (header.receiveMonotonicNs >= output.lastReceiveMonotonicNs) {
    output.lastReceiveMonotonicNs = header.receiveMonotonicNs;
    output.lastReceiveRealtimeNs = header.receiveRealtimeNs;
  }
  if (header.exchangeTimestampNs > 0) {
    if (output.firstTsNs == 0 ||
        header.exchangeTimestampNs < output.firstTsNs) {
      output.firstTsNs = header.exchangeTimestampNs;
    }
    if (header.exchangeTimestampNs > output.lastTsNs)
      output.lastTsNs = header.exchangeTimestampNs;
  }
  if ((header.flags & corpus::BinaryMarketRecordRealtimeRegression) != 0u)
    ++output.realtimeRegressionCount;
  if ((header.flags &
       corpus::BinaryMarketRecordMonotonicNonIncreasing) != 0u) {
    ++output.monotonicNonIncreasingCount;
  }
  if ((header.flags & corpus::BinaryMarketRecordExchangeAheadOfReceive) != 0u)
    ++output.exchangeAheadOfReceiveCount;
  if ((header.flags &
       corpus::BinaryMarketRecordExchangeTimestampMissing) != 0u) {
    ++output.exchangeTimestampMissingCount;
  }
}

[[nodiscard]] bool channelFlagsValid(
    const corpus::BinaryMarketRecordHeader& header) noexcept {
  constexpr std::uint32_t common =
      corpus::BinaryMarketRecordTraderReplayCompatible |
      corpus::BinaryMarketRecordExchangeTimestampMissing |
      corpus::BinaryMarketRecordRealtimeRegression |
      corpus::BinaryMarketRecordMonotonicNonIncreasing |
      corpus::BinaryMarketRecordExchangeAheadOfReceive;
  std::uint32_t allowed = common;
  if (header.channel == corpus::BinaryMarketChannel::BookTicker) {
    allowed |= corpus::BinaryMarketRecordBboFreshnessOnly;
  } else if (header.channel == corpus::BinaryMarketChannel::Depth) {
    allowed |= corpus::BinaryMarketRecordDepthSnapshot |
        corpus::BinaryMarketRecordDepthDelta |
        corpus::BinaryMarketRecordDepthPartial |
        corpus::BinaryMarketRecordDepthRebase |
        corpus::BinaryMarketRecordDepthState;
  }
  return (header.flags & corpus::BinaryMarketRecordGapBoundary) == 0u &&
      (header.flags & ~allowed) == 0u;
}

[[nodiscard]] bool sameDepthFrame(
    const corpus::BinaryMarketRecordHeader& first,
    const corpus::BinaryMarketRecordHeader& current,
    std::uint16_t partIndex) noexcept {
  constexpr std::uint32_t anomalyFlags =
      corpus::BinaryMarketRecordRealtimeRegression |
      corpus::BinaryMarketRecordMonotonicNonIncreasing |
      corpus::BinaryMarketRecordExchangeAheadOfReceive;
  return current.sourceId == first.sourceId &&
      current.sourceGeneration == first.sourceGeneration &&
      current.sessionEpoch == first.sessionEpoch &&
      current.eventSequence == first.eventSequence &&
      current.frameSequence == first.frameSequence &&
      current.nativeIdentityFirst == first.nativeIdentityFirst &&
      current.nativeIdentitySecond == first.nativeIdentitySecond &&
      current.exchangeTimestampNs == first.exchangeTimestampNs &&
      current.receiveRealtimeNs == first.receiveRealtimeNs &&
      current.receiveMonotonicNs == first.receiveMonotonicNs &&
      current.shardIndex == first.shardIndex &&
      current.sourceFlags == first.sourceFlags &&
      current.channel == corpus::BinaryMarketChannel::Depth &&
      current.nativeIdentityShape == first.nativeIdentityShape &&
      current.eventOrdinal == partIndex &&
      first.shardSequence <=
          std::numeric_limits<std::uint64_t>::max() - partIndex &&
      current.shardSequence == first.shardSequence + partIndex &&
      (current.flags & ~anomalyFlags) == (first.flags & ~anomalyFlags);
}

struct DepthRebaseCursor final {
  std::uint64_t sourceGeneration{0u};
  std::uint64_t sessionEpoch{0u};
  std::uint64_t frameSequence{0u};
  std::uint64_t receiveMonotonicNs{0u};
  std::uint64_t nextShardSequence{0u};
  std::uint64_t publicationSequence{0u};
  std::uint64_t lastUpdateId{0u};
  std::int64_t receiveRealtimeNs{0};
  std::uint32_t sourceId{0u};
  std::uint32_t replayDeltaCount{0u};
  std::uint16_t shardIndex{0u};
  std::uint16_t transactionPartCount{0u};
  std::uint16_t nextTransactionPartIndex{0u};
  bool active{false};
};

[[nodiscard]] bool advanceDepthRebase(
    const corpus::BinaryMarketRecordHeader& header,
    const corpus::BinaryMarketDepthChunkPayload& payload,
    DepthRebaseCursor& cursor) noexcept {
  if (payload.transactionPartCount == 0u ||
      payload.transactionPartIndex >= payload.transactionPartCount) {
    return false;
  }
  const bool rebase = payload.state ==
      corpus::BinaryMarketDepthState::RebaseInFlight;
  if (!rebase) {
    if (cursor.active || payload.transactionPartIndex != 0u ||
        payload.transactionPartCount != 1u) {
      return false;
    }
    cursor = {};
    return true;
  }
  const bool same = cursor.active &&
      header.sourceId == cursor.sourceId &&
      header.sourceGeneration == cursor.sourceGeneration &&
      header.sessionEpoch == cursor.sessionEpoch &&
      header.frameSequence == cursor.frameSequence &&
      header.receiveRealtimeNs == cursor.receiveRealtimeNs &&
      header.receiveMonotonicNs == cursor.receiveMonotonicNs &&
      header.shardIndex == cursor.shardIndex;
  const bool snapshot = payload.frameKind ==
      corpus::BinaryMarketDepthFrameKind::Snapshot;
  if (!cursor.active) {
    if (!snapshot || payload.transactionPartIndex != 0u) return false;
    cursor = {.sourceGeneration = header.sourceGeneration,
              .sessionEpoch = header.sessionEpoch,
              .frameSequence = header.frameSequence,
              .receiveMonotonicNs = header.receiveMonotonicNs,
              .publicationSequence = header.eventSequence,
              .lastUpdateId = payload.sequence.lastUpdateId,
              .receiveRealtimeNs = header.receiveRealtimeNs,
              .sourceId = header.sourceId,
              .shardIndex = header.shardIndex,
              .transactionPartCount = payload.transactionPartCount,
              .nextTransactionPartIndex = 1u,
              .active = true};
  } else {
    if (!same || snapshot ||
        payload.transactionPartCount != cursor.transactionPartCount ||
        payload.transactionPartIndex !=
            cursor.nextTransactionPartIndex ||
        cursor.publicationSequence ==
                        std::numeric_limits<std::uint64_t>::max() ||
        header.eventSequence != cursor.publicationSequence + 1u ||
        header.shardSequence != cursor.nextShardSequence ||
        cursor.lastUpdateId == std::numeric_limits<std::uint64_t>::max() ||
        (cursor.replayDeltaCount != 0u &&
         payload.sequence.previousUpdateId != cursor.lastUpdateId) ||
        payload.sequence.firstUpdateId > cursor.lastUpdateId + 1u ||
        payload.sequence.lastUpdateId < cursor.lastUpdateId + 1u) {
      return false;
    }
    cursor.publicationSequence = header.eventSequence;
    cursor.lastUpdateId = payload.sequence.lastUpdateId;
    if (cursor.replayDeltaCount ==
        std::numeric_limits<std::uint32_t>::max()) {
      return false;
    }
    ++cursor.replayDeltaCount;
    ++cursor.nextTransactionPartIndex;
  }
  if (header.shardSequence >
      std::numeric_limits<std::uint64_t>::max() - payload.partCount) {
    return false;
  }
  cursor.nextShardSequence = header.shardSequence + payload.partCount;
  if (cursor.nextTransactionPartIndex == cursor.transactionPartCount) {
    cursor.active = false;
  }
  return true;
}

[[nodiscard]] bool appendDepth(
    const corpus::BinaryMarketSelection& selection,
    std::size_t& recordIndex,
    std::int64_t ingestSequence,
    SessionData& output,
    std::string& error) {
  const auto& firstRecord = selection.records[recordIndex];
  const auto* first = checkedPayload<corpus::BinaryMarketDepthChunkPayload>(
      firstRecord);
  if (!first || first->partIndex != 0u || first->partCount == 0u ||
      first->totalLevelCount == 0u ||
      first->totalLevelCount > rawdata::kMaxOrderbookLevels ||
      first->partCount !=
          (first->totalLevelCount +
           corpus::kBinaryMarketDepthLevelsPerRecord - 1u) /
              corpus::kBinaryMarketDepthLevelsPerRecord ||
      (first->frameKind !=
           corpus::BinaryMarketDepthFrameKind::Snapshot &&
       first->frameKind != corpus::BinaryMarketDepthFrameKind::Delta) ||
      (first->state != corpus::BinaryMarketDepthState::Partial &&
       first->state != corpus::BinaryMarketDepthState::RebaseInFlight &&
       first->state != corpus::BinaryMarketDepthState::Healthy) ||
      first->failure != 0u ||
      first->sourceReceiveRealtimeNs == 0u ||
      first->sourceReceiveRealtimeNs > static_cast<std::uint64_t>(
          std::numeric_limits<std::int64_t>::max()) ||
      first->sourceFreshnessMonotonicNs == 0u ||
      first->transactionPartCount == 0u ||
      first->transactionPartIndex >= first->transactionPartCount ||
      first->reserved8 != 0u || first->reserved16 != 0u) {
    error = "malformed binary corpus depth frame";
    return false;
  }
  const bool snapshot = first->frameKind ==
      corpus::BinaryMarketDepthFrameKind::Snapshot;
  const std::uint32_t kindFlag = snapshot
      ? corpus::BinaryMarketRecordDepthSnapshot
      : corpus::BinaryMarketRecordDepthDelta;
  const std::uint32_t otherKindFlag = snapshot
      ? corpus::BinaryMarketRecordDepthDelta
      : corpus::BinaryMarketRecordDepthSnapshot;
  const bool partial =
      first->state == corpus::BinaryMarketDepthState::Partial;
  const bool rebaseInFlight =
      first->state == corpus::BinaryMarketDepthState::RebaseInFlight;
  const bool partialFlag =
      (firstRecord.header.flags &
       corpus::BinaryMarketRecordDepthPartial) != 0u;
  const bool rebaseFlag =
      (firstRecord.header.flags &
       corpus::BinaryMarketRecordDepthRebase) != 0u;
  if ((firstRecord.header.flags & kindFlag) == 0u ||
      (firstRecord.header.flags & otherKindFlag) != 0u ||
      (firstRecord.header.flags &
       corpus::BinaryMarketRecordDepthState) != 0u ||
      partialFlag != partial || (snapshot && partial) ||
      rebaseFlag != rebaseInFlight ||
      (snapshot && !rebaseInFlight) ||
      (rebaseInFlight &&
       (snapshot != (first->transactionPartIndex == 0u))) ||
      (!rebaseInFlight &&
       (first->transactionPartIndex != 0u ||
        first->transactionPartCount != 1u)) ||
      (!snapshot && !rebaseInFlight &&
       first->state != corpus::BinaryMarketDepthState::Partial &&
       first->state != corpus::BinaryMarketDepthState::Healthy) ||
      (snapshot
           ? first->sequence.firstUpdateId == 0u ||
                 first->sequence.firstUpdateId !=
                     first->sequence.lastUpdateId ||
                 first->sequence.previousUpdateId != 0u
           : first->sequence.firstUpdateId == 0u ||
                 first->sequence.lastUpdateId <
                     first->sequence.firstUpdateId)) {
    error = "binary corpus depth sequence contract mismatch";
    return false;
  }

  DepthRow row{};
  row.tsNs = firstRecord.header.exchangeTimestampNs;
  row.ingestSeq = ingestSequence;
  row.arrival = makeArrival(selection.manifest, firstRecord.header);
  row.resetBookBeforeApply = snapshot;
  row.applicationAtomicRebase = rebaseInFlight;
  row.levels.reserve(first->totalLevelCount);

  bool asks = false;
  std::uint64_t previousPrice = 0u;
  for (std::uint16_t partIndex = 0u; partIndex < first->partCount;
       ++partIndex) {
    const std::size_t index = recordIndex + partIndex;
    if (index >= selection.records.size()) {
      error = "truncated binary corpus depth frame";
      return false;
    }
    const auto& record = selection.records[index];
    const auto* part = checkedPayload<corpus::BinaryMarketDepthChunkPayload>(
        record);
    const std::uint32_t offset =
        static_cast<std::uint32_t>(partIndex) *
        corpus::kBinaryMarketDepthLevelsPerRecord;
    const std::uint16_t expectedLevels = static_cast<std::uint16_t>(
        std::min<std::uint32_t>(
            corpus::kBinaryMarketDepthLevelsPerRecord,
            first->totalLevelCount - offset));
    if (!part || !channelFlagsValid(record.header) ||
        !sameDepthFrame(firstRecord.header, record.header, partIndex) ||
        part->totalLevelCount != first->totalLevelCount ||
        part->levelOffset != offset || part->partIndex != partIndex ||
        part->partCount != first->partCount ||
        part->levelCount != expectedLevels ||
        part->frameKind != first->frameKind ||
        part->state != first->state || part->failure != first->failure ||
        part->sequence.firstUpdateId != first->sequence.firstUpdateId ||
        part->sequence.lastUpdateId != first->sequence.lastUpdateId ||
        part->sequence.previousUpdateId !=
            first->sequence.previousUpdateId ||
        part->transactionPartIndex != first->transactionPartIndex ||
        part->transactionPartCount != first->transactionPartCount ||
        part->sourceReceiveRealtimeNs !=
            first->sourceReceiveRealtimeNs ||
        part->sourceFreshnessMonotonicNs !=
            first->sourceFreshnessMonotonicNs ||
        part->reserved8 != 0u || part->reserved16 != 0u) {
      error = "binary corpus depth chunk contract mismatch";
      return false;
    }
    for (std::uint16_t levelIndex = 0u;
         levelIndex < part->levelCount; ++levelIndex) {
      const auto& level = part->levels[levelIndex];
      if ((level.side != 1u && level.side != 2u) ||
          (level.action != 1u && level.action != 2u) ||
          level.priceRaw == 0u ||
          (level.action == 1u && level.quantityRaw == 0u) ||
          (level.action == 2u && level.quantityRaw != 0u) ||
          (snapshot && level.action != 1u) || level.reserved16 != 0u ||
          level.reserved32 != 0u) {
        error = "invalid binary corpus depth level";
        return false;
      }
      if (level.side == 2u) {
        if (!asks) {
          asks = true;
          previousPrice = 0u;
        }
        if (previousPrice != 0u && level.priceRaw <= previousPrice) {
          error = "binary corpus asks are not canonical";
          return false;
        }
      } else {
        if (asks ||
            (previousPrice != 0u && level.priceRaw >= previousPrice)) {
          error = "binary corpus bids are not canonical";
          return false;
        }
      }
      previousPrice = level.priceRaw;
      PriceLevelRow converted{};
      if (!rescaleToE8(level.priceRaw, selection.source.priceScale,
                       converted.priceE8) ||
          !rescaleToE8(level.quantityRaw, selection.source.quantityScale,
                       converted.qtyE8)) {
        error = "binary corpus depth value cannot be represented exactly in E8";
        return false;
      }
      converted.side = level.side == 1u ? 0 : 1;
      row.levels.push_back(converted);
    }
  }
  includeEvidence(output, firstRecord.header);
  output.depths.push_back(std::move(row));
  recordIndex += first->partCount;
  return true;
}

[[nodiscard]] bool requiredRowsPresent(const SessionLoadPlan& plan,
                                       const SessionData& output,
                                       std::string& error) {
  auto require = [&](bool required, bool empty, const char* name) {
    if (!required || !empty) return true;
    error = "selected binary corpus interval has no required ";
    error += name;
    error += " events";
    return false;
  };
  return require(plan.requireTrades, output.trades.empty(), "trades") &&
      require(plan.requireBookTickers, output.bookTickers.empty(),
              "bookticker") &&
      require(plan.requirePriceLimits, output.priceLimits.empty(),
              "price_limit") &&
      require(plan.requireDepths, output.depths.empty(), "depth") &&
      require(plan.requireLiquidations, output.liquidations.empty(),
              "liquidations") &&
      require(plan.requireMarkPrices, output.markPrices.empty(),
              "mark_price") &&
      require(plan.requireIndexPrices, output.indexPrices.empty(),
              "index_price") &&
      require(plan.requireFunding, output.funding.empty(), "funding");
}

void appendEvidenceWarnings(SessionData& output) {
  if (output.realtimeRegressionCount != 0u) {
    appendWarning(output, "CLOCK_REALTIME regressions preserved: " +
        std::to_string(output.realtimeRegressionCount));
  }
  if (output.monotonicNonIncreasingCount != 0u) {
    appendWarning(output, "non-increasing CLOCK_MONOTONIC samples preserved: " +
        std::to_string(output.monotonicNonIncreasingCount));
  }
  if (output.exchangeAheadOfReceiveCount != 0u) {
    appendWarning(output, "exchange timestamps ahead of local receive preserved: " +
        std::to_string(output.exchangeAheadOfReceiveCount));
  }
  if (output.exchangeTimestampMissingCount != 0u) {
    appendWarning(output, "missing exchange timestamps preserved: " +
        std::to_string(output.exchangeTimestampMissingCount));
  }
}

}  // namespace

Status loadBinaryMarketCorpus(
    const std::filesystem::path& corpusPath,
    const BacktestCorpusSourceRequest& sourceRequest,
    const SessionLoadPlan& plan,
    SessionData& output,
    std::string& error) noexcept {
  output = {};
  error.clear();
  if (corpusPath.empty() || sourceRequest.exchange.empty() ||
      sourceRequest.market.empty() || sourceRequest.canonicalSymbol.empty() ||
      sourceRequest.beginReceiveNs <= 0 ||
      sourceRequest.endReceiveNs <= sourceRequest.beginReceiveNs) {
    error = "invalid binary corpus source selection";
    return Status::InvalidArgument;
  }
  if (plan.requireCandles) {
    error = "strategy requires candle history, which is not part of the live binary market corpus";
    return Status::Unsupported;
  }
  if (plan.requireOpenInterest) {
    error = "strategy requires open_interest, which is not in the binary market corpus contract";
    return Status::Unsupported;
  }

  try {
    corpus::BinaryMarketCorpusReader reader{};
    corpus::BinaryMarketManifest manifest{};
    std::vector<corpus::BinaryMarketSource> sources{};
    const hftrec::Status catalogStatus =
        reader.catalog(corpusPath, manifest, sources, error);
    if (!hftrec::isOk(catalogStatus)) return convertStatus(catalogStatus);

    const corpus::BinaryMarketSource* selectedSource = nullptr;
    for (const auto& source : sources) {
      if (fixedText(source.venue.data(), source.venue.size(),
                    source.venueBytes) != sourceRequest.exchange ||
          fixedText(source.market.data(), source.market.size(),
                    source.marketBytes) != sourceRequest.market ||
          fixedText(source.canonicalSymbol.data(),
                    source.canonicalSymbol.size(),
                    source.canonicalSymbolBytes) !=
              sourceRequest.canonicalSymbol) {
        continue;
      }
      if (selectedSource) {
        error = "binary corpus source selection is ambiguous";
        return Status::CorruptData;
      }
      selectedSource = &source;
    }
    if (!selectedSource) {
      error = "binary corpus source was not found";
      return Status::InvalidArgument;
    }

    const ChannelPlan requested = channelPlan(plan);
    const std::uint16_t missingRequired = static_cast<std::uint16_t>(
        requested.requiredMask & ~selectedSource->availableChannelMask);
    if (missingRequired != 0u) {
      error = "required strategy channel was not captured for selected source";
      return Status::CorruptData;
    }
    const std::uint16_t incompatibleRequired = static_cast<std::uint16_t>(
        requested.requiredMask & ~selectedSource->traderReplayChannelMask);
    if (incompatibleRequired != 0u) {
      error = "required strategy channel is not trader-replay compatible";
      return Status::Unsupported;
    }
    const std::uint16_t optionalRecordedOnly = static_cast<std::uint16_t>(
        requested.loadMask & selectedSource->availableChannelMask &
        ~selectedSource->traderReplayChannelMask);
    const std::uint16_t selectedMask = static_cast<std::uint16_t>(
        requested.loadMask & selectedSource->availableChannelMask &
        selectedSource->traderReplayChannelMask);
    if (selectedMask == 0u) {
      error = "binary corpus has no trader-replay compatible channel requested by the strategy";
      return Status::Unsupported;
    }

    corpus::BinaryMarketSelectionRequest selectionRequest{};
    selectionRequest.root = corpusPath;
    selectionRequest.exchange = sourceRequest.exchange;
    selectionRequest.market = sourceRequest.market;
    selectionRequest.canonicalSymbol = sourceRequest.canonicalSymbol;
    selectionRequest.beginReceiveNs = sourceRequest.beginReceiveNs;
    selectionRequest.endReceiveNs = sourceRequest.endReceiveNs;
    selectionRequest.channelMask = selectedMask;
    selectionRequest.requiredChannelMask = requested.requiredMask;
    selectionRequest.requireTraderReplayCompatibility = true;
    corpus::BinaryMarketSelection selection{};
    const hftrec::Status selectStatus =
        reader.select(selectionRequest, selection, error);
    if (!hftrec::isOk(selectStatus)) return convertStatus(selectStatus);

    output.sessionPath = corpusPath;
    output.exchange = sourceRequest.exchange;
    output.market = sourceRequest.market;
    output.symbol = sourceRequest.canonicalSymbol;
    output.binaryMarketCorpus = true;
    output.replayAnchorMonotonicNs =
        selection.manifest.replayAnchorMonotonicNs;
    output.replayAnchorReceiveNs =
        selection.manifest.replayAnchorReceiveNs;
    if (plan.loadCandles) {
      appendWarning(output,
          "optional candle history is unavailable in live binary corpus");
    }
    if (plan.loadOpenInterest) {
      appendWarning(output,
          "optional open_interest channel is unavailable in binary corpus");
    }
    for (std::size_t index = 0u;
         index < corpus::kBinaryMarketChannelCount; ++index) {
      const std::uint16_t bit = static_cast<std::uint16_t>(1u << index);
      if ((optionalRecordedOnly & bit) != 0u) {
        appendWarning(output,
            std::string{"optional recorded-only channel ignored: "} +
                channelName(static_cast<corpus::BinaryMarketChannel>(
                    index + 1u)));
      } else if ((requested.loadMask & bit) != 0u &&
                 (selectedSource->availableChannelMask & bit) == 0u) {
        appendWarning(output,
            std::string{"optional channel was not captured: "} +
                channelName(static_cast<corpus::BinaryMarketChannel>(
                    index + 1u)));
      }
    }

    if (selection.source.tickSizeRaw <= 0 ||
        !rescaleToE8(selection.source.tickSizeRaw,
                     selection.source.priceScale,
                     output.instrument.tickSizeE8) ||
        output.instrument.tickSizeE8 <= 0) {
      error = "binary corpus tick size cannot be represented exactly in E8";
      return Status::CorruptData;
    }
    if ((selection.source.instrumentMetadataFlags &
         corpus::BinaryMarketInstrumentStepSize) == 0u ||
        !rescaleToE8(selection.source.stepSizeRaw,
                     selection.source.quantityScale,
                     output.instrument.lotSizeE8) ||
        output.instrument.lotSizeE8 <= 0) {
      error = "binary corpus has no exact E8 quantity step";
      return Status::CorruptData;
    }
    if (selection.source.contractBaseQtyRaw > 0 &&
        (!rescaleToE8(selection.source.contractBaseQtyRaw,
                      selection.source.quantityScale,
                      output.instrument.contractBaseQtyE8) ||
         output.instrument.contractBaseQtyE8 <= 0)) {
      error = "binary corpus contract base quantity cannot be represented exactly in E8";
      return Status::CorruptData;
    }
    if (selection.source.priceBasisQtyRaw > 0 &&
        (!rescaleToE8(selection.source.priceBasisQtyRaw,
                      selection.source.quantityScale,
                      output.instrument.priceBasisQtyE8) ||
         output.instrument.priceBasisQtyE8 <= 0)) {
      error = "binary corpus price basis cannot be represented exactly in E8";
      return Status::CorruptData;
    }

    std::array<std::size_t, corpus::kBinaryMarketChannelCount> counts{};
    for (const auto& record : selection.records) {
      ++counts[corpus::binaryMarketChannelIndex(record.header.channel)];
    }
    output.trades.reserve(
        counts[corpus::binaryMarketChannelIndex(
            corpus::BinaryMarketChannel::Trade)]);
    output.bookTickers.reserve(
        counts[corpus::binaryMarketChannelIndex(
            corpus::BinaryMarketChannel::BookTicker)]);
    output.priceLimits.reserve(
        counts[corpus::binaryMarketChannelIndex(
            corpus::BinaryMarketChannel::PriceLimit)]);
    output.depths.reserve(
        counts[corpus::binaryMarketChannelIndex(
            corpus::BinaryMarketChannel::Depth)]);
    output.liquidations.reserve(
        counts[corpus::binaryMarketChannelIndex(
            corpus::BinaryMarketChannel::Liquidation)]);
    output.markPrices.reserve(
        counts[corpus::binaryMarketChannelIndex(
            corpus::BinaryMarketChannel::MarkPrice)]);
    output.indexPrices.reserve(
        counts[corpus::binaryMarketChannelIndex(
            corpus::BinaryMarketChannel::IndexPrice)]);
    output.funding.reserve(
        counts[corpus::binaryMarketChannelIndex(
            corpus::BinaryMarketChannel::Funding)]);

    std::int64_t ingestSequence = 0;
    std::uint64_t retainedBboSequence = 0u;
    std::uint64_t retainedBboSessionEpoch = 0u;
    corpus::BinaryMarketBookTickerPayload retainedBboPayload{};
    std::uint16_t retainedBboPresenceFlags = 0u;
    DepthRebaseCursor depthRebase{};
    std::size_t recordIndex = 0u;
    while (recordIndex < selection.records.size()) {
      const auto& record = selection.records[recordIndex];
      bool bboFreshnessOnly = false;
      if (!channelFlagsValid(record.header)) {
        error = "binary corpus record flags do not match its channel";
        return Status::CorruptData;
      }
      if ((record.header.flags &
           corpus::BinaryMarketRecordTraderReplayCompatible) == 0u) {
        if (depthRebase.active) {
          error = "binary corpus depth rebase transaction is incomplete";
          return Status::CorruptData;
        }
        depthRebase = {};
        appendWarning(output,
            std::string{"optional non-replay-compatible record ignored: "} +
                channelName(record.header.channel));
        ++recordIndex;
        continue;
      }
      if (record.header.channel == corpus::BinaryMarketChannel::BookTicker) {
        const auto* payload = checkedBookTickerPayload(record);
        if (!payload) {
          error = "invalid binary corpus bookticker payload";
          return Status::CorruptData;
        }
        if (retainedBboSessionEpoch != 0u &&
            retainedBboSessionEpoch != record.header.sessionEpoch) {
          retainedBboSequence = 0u;
          retainedBboPayload = {};
          retainedBboPresenceFlags = 0u;
        }
        retainedBboSessionEpoch = record.header.sessionEpoch;
        const bool freshnessOnly =
            (record.header.flags &
             corpus::BinaryMarketRecordBboFreshnessOnly) != 0u;
        constexpr std::uint16_t presenceMask =
            corpus::BinaryMarketSourceBookTickerBidPresent |
            corpus::BinaryMarketSourceBookTickerAskPresent;
        const std::uint16_t presenceFlags =
            record.header.sourceFlags & presenceMask;
        if (retainedBboSequence != 0u &&
            record.header.eventSequence <= retainedBboSequence) {
          if (!freshnessOnly ||
              record.header.eventSequence != retainedBboSequence) {
            error = "binary corpus retained BBO sequence regressed";
            return Status::CorruptData;
          }
          if (payload->bidPriceRaw != retainedBboPayload.bidPriceRaw ||
              payload->bidQtyRaw != retainedBboPayload.bidQtyRaw ||
              payload->askPriceRaw != retainedBboPayload.askPriceRaw ||
              payload->askQtyRaw != retainedBboPayload.askQtyRaw ||
              presenceFlags != retainedBboPresenceFlags) {
            error = "binary corpus freshness-only BBO changed retained state";
            return Status::CorruptData;
          }
          bboFreshnessOnly = true;
        } else {
          retainedBboSequence = record.header.eventSequence;
          retainedBboPayload = *payload;
          retainedBboPresenceFlags = presenceFlags;
        }
      }
      if (ingestSequence == std::numeric_limits<std::int64_t>::max()) {
        error = "binary corpus event count exceeds backtest ingest sequence";
        return Status::CorruptData;
      }
      ++ingestSequence;
      if (record.header.channel == corpus::BinaryMarketChannel::Depth) {
        const auto* depth =
            checkedPayload<corpus::BinaryMarketDepthChunkPayload>(record);
        if (!depth ||
            !advanceDepthRebase(record.header, *depth, depthRebase)) {
          error = "binary corpus depth rebase transaction is incomplete";
          return Status::CorruptData;
        }
        if (!appendDepth(selection, recordIndex, ingestSequence,
                         output, error)) {
          return Status::CorruptData;
        }
        continue;
      }
      if (depthRebase.active) {
        error = "binary corpus depth rebase transaction is incomplete";
        return Status::CorruptData;
      }
      depthRebase = {};

      const auto arrival = makeArrival(selection.manifest, record.header);
      switch (record.header.channel) {
        case corpus::BinaryMarketChannel::BookTicker: {
          const auto* payload = checkedBookTickerPayload(record);
          BookTickerRow row{};
          if (!rescaleToE8(payload->bidPriceRaw,
                           selection.source.priceScale, row.bidPriceE8) ||
              !rescaleToE8(payload->bidQtyRaw,
                           selection.source.quantityScale, row.bidQtyE8) ||
              !rescaleToE8(payload->askPriceRaw,
                           selection.source.priceScale, row.askPriceE8) ||
              !rescaleToE8(payload->askQtyRaw,
                           selection.source.quantityScale, row.askQtyE8)) {
            error = "binary corpus bookticker cannot be represented exactly in E8";
            return Status::CorruptData;
          }
          row.tsNs = record.header.exchangeTimestampNs;
          row.ingestSeq = ingestSequence;
          row.arrival = arrival;
          row.bidPresent =
              (record.header.sourceFlags &
               corpus::BinaryMarketSourceBookTickerBidPresent) != 0u;
          row.askPresent =
              (record.header.sourceFlags &
               corpus::BinaryMarketSourceBookTickerAskPresent) != 0u;
          row.freshnessOnly = bboFreshnessOnly;
          output.bookTickers.push_back(row);
          break;
        }
        case corpus::BinaryMarketChannel::Trade: {
          const auto* payload =
              checkedPayload<corpus::BinaryMarketTradePayload>(record);
          if (!payload || (payload->side != 1u && payload->side != 2u) ||
              payload->reserved != decltype(payload->reserved){}) {
            error = "invalid binary corpus trade payload";
            return Status::CorruptData;
          }
          TradeRow row{};
          if (!rescaleToE8(payload->priceRaw, selection.source.priceScale,
                           row.priceE8) ||
              !rescaleToE8(payload->qtyRaw, selection.source.quantityScale,
                           row.qtyE8) ||
              row.priceE8 <= 0 || row.qtyE8 <= 0) {
            error = "binary corpus trade cannot be represented exactly in E8";
            return Status::CorruptData;
          }
          row.side = payload->side == 1u ? 1 : 0;
          row.tsNs = record.header.exchangeTimestampNs;
          row.ingestSeq = ingestSequence;
          row.arrival = arrival;
          output.trades.push_back(row);
          break;
        }
        case corpus::BinaryMarketChannel::PriceLimit: {
          const auto* payload =
              checkedPayload<corpus::BinaryMarketPriceLimitPayload>(record);
          if (!payload || payload->enabled > 1u ||
              payload->reserved != decltype(payload->reserved){}) {
            error = "invalid binary corpus price_limit payload";
            return Status::CorruptData;
          }
          PriceLimitRow row{};
          if (!rescaleToE8(payload->buyLimitRaw,
                           selection.source.priceScale, row.buyLimitE8) ||
              !rescaleToE8(payload->sellLimitRaw,
                           selection.source.priceScale, row.sellLimitE8) ||
              (payload->enabled != 0u &&
               (row.buyLimitE8 <= 0 || row.sellLimitE8 <= 0))) {
            error = "binary corpus price_limit cannot be represented exactly in E8";
            return Status::CorruptData;
          }
          row.tsNs = record.header.exchangeTimestampNs;
          row.enabled = payload->enabled;
          row.ingestSeq = ingestSequence;
          row.arrival = arrival;
          output.priceLimits.push_back(row);
          break;
        }
        case corpus::BinaryMarketChannel::Liquidation: {
          const auto* payload =
              checkedPayload<corpus::BinaryMarketLiquidationPayload>(record);
          if (!payload || (payload->side != 1u && payload->side != 2u) ||
              payload->reserved != decltype(payload->reserved){}) {
            error = "invalid binary corpus liquidation payload";
            return Status::CorruptData;
          }
          LiquidationRow row{};
          if (!rescaleToE8(payload->priceRaw, selection.source.priceScale,
                           row.priceE8) ||
              !rescaleToE8(payload->qtyRaw, selection.source.quantityScale,
                           row.qtyE8) ||
              !rescaleToE8(payload->averagePriceRaw,
                           selection.source.priceScale, row.avgPriceE8) ||
              !rescaleToE8(payload->filledQtyRaw,
                           selection.source.quantityScale,
                           row.filledQtyE8) ||
              row.priceE8 <= 0 || row.qtyE8 <= 0) {
            error = "binary corpus liquidation cannot be represented exactly in E8";
            return Status::CorruptData;
          }
          row.side = payload->side == 1u ? 1 : 0;
          row.tsNs = record.header.exchangeTimestampNs;
          row.orderType = payload->orderType;
          row.timeInForce = payload->timeInForce;
          row.status = payload->status;
          row.sourceMode = payload->sourceMode;
          row.ingestSeq = ingestSequence;
          row.arrival = arrival;
          output.liquidations.push_back(row);
          break;
        }
        case corpus::BinaryMarketChannel::MarkPrice:
        case corpus::BinaryMarketChannel::IndexPrice: {
          const auto* payload =
              checkedPayload<corpus::BinaryMarketScalarPricePayload>(record);
          std::int64_t priceE8 = 0;
          if (!payload ||
              !rescaleToE8(payload->priceRaw, selection.source.priceScale,
                           priceE8) ||
              priceE8 <= 0) {
            error = "binary corpus scalar price cannot be represented exactly in E8";
            return Status::CorruptData;
          }
          if (record.header.channel ==
              corpus::BinaryMarketChannel::MarkPrice) {
            output.markPrices.push_back({record.header.exchangeTimestampNs,
                                         priceE8, ingestSequence, arrival});
          } else {
            output.indexPrices.push_back({record.header.exchangeTimestampNs,
                                          priceE8, ingestSequence, arrival});
          }
          break;
        }
        case corpus::BinaryMarketChannel::Funding: {
          const auto* payload =
              checkedPayload<corpus::BinaryMarketFundingPayload>(record);
          if (!payload || payload->fundingTimestampNs < 0 ||
              payload->nextFundingTimestampNs < 0) {
            error = "invalid binary corpus funding payload";
            return Status::CorruptData;
          }
          output.funding.push_back({
              record.header.exchangeTimestampNs,
              payload->fundingRateRaw,
              payload->fundingTimestampNs,
              payload->nextFundingTimestampNs,
              ingestSequence,
              arrival});
          break;
        }
        case corpus::BinaryMarketChannel::Depth:
          break;
      }
      includeEvidence(output, record.header);
      ++recordIndex;
    }

    if (depthRebase.active) {
      error = "binary corpus depth rebase transaction is incomplete";
      return Status::CorruptData;
    }

    if (!requiredRowsPresent(plan, output, error)) return Status::CorruptData;
    if (output.firstReceiveMonotonicNs == 0u ||
        output.firstReceiveRealtimeNs <= 0 || ingestSequence == 0) {
      error = "selected binary corpus interval has no replayable market events";
      return Status::CorruptData;
    }
    output.timelineChannelSorted.fill(false);
    output.timelineOrderKnown = false;
    appendEvidenceWarnings(output);
    return Status::Ok;
  } catch (...) {
    output = {};
    error = "binary corpus backtest loading failed";
    return Status::IoError;
  }
}

}  // namespace hft_backtest::detail
