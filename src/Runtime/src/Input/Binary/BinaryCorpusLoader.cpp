#include "Internal/BinaryCorpusHelpers.hpp"
#include "BinaryCorpusLoader.hpp"

#include "hftrec/CorpusContract/BinaryMarketCorpusReader.hpp"
#include "trading_core/Algorithm/Market/StrategyInputs.hpp"
#include "cxet/Primitives/Buf/CanonConstants.hpp"
#include "cxet/Network/Common/NativeOrderQuantity.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace hft_backtest::detail {
namespace binary_corpus {





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
    case corpus::BinaryMarketChannel::SourceLifecycle:
      return "source_lifecycle";
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
  includeChannel(output, corpus::BinaryMarketChannel::Depth,
                 plan.loadDepths, plan.requireDepths);
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

bool canonicalMarketQuantityToE8(const corpus::BinaryMarketSource& source,
    corpus::BinaryMarketChannel channel, std::int64_t input, std::int64_t& output) noexcept {
  const auto& conversion = source.quantityConversion;
  const auto index = corpus::binaryMarketChannelIndex(channel);
  if (index >= conversion.kinds.size() || !corpus::validBinaryMarketQuantityConversion(conversion)) return false;
  std::int64_t scaled{};
  if (!rescaleToE8(input, source.quantityScale, scaled)) return false;
  auto canonicalPackage = conversion.canonicalBaseMultiplier;
  auto nativePackage = conversion.nativeBaseMultiplier;
  auto unit = cxet::contract::Unit::None;
  switch (conversion.kinds[index]) {
    case corpus::BinaryMarketQuantityKind::CanonicalBase:
      unit = cxet::contract::Unit::BaseQuantity; canonicalPackage = nativePackage = 1u; break;
    case corpus::BinaryMarketQuantityKind::NativeBase: unit = cxet::contract::Unit::BaseQuantity; break;
    case corpus::BinaryMarketQuantityKind::NativeContract: unit = cxet::contract::Unit::ContractQuantity; break;
    case corpus::BinaryMarketQuantityKind::NativeLot: unit = cxet::contract::Unit::LotQuantity; break;
    case corpus::BinaryMarketQuantityKind::Unknown: return false;
  }
  return cxet::network::common::canonicalNativeQuantityRawExact(scaled,
      conversion.nativeContractBaseQtyRaw, unit, canonicalPackage, nativePackage, &output,
      conversion.nativeLotBaseQtyRaw);
}

bool canonicalMarketQuantityToE8(const corpus::BinaryMarketSource& source,
    corpus::BinaryMarketChannel channel, std::uint64_t input, std::int64_t& output) noexcept {
  return input <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) &&
      canonicalMarketQuantityToE8(source, channel, static_cast<std::int64_t>(input), output);
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
  return payload && cxet::api::market::validMarketNativeEvidence(payload->nativeEvidence) &&
          (bidPresent || askPresent) &&
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
  const bool lifecycle = header.channel == corpus::BinaryMarketChannel::SourceLifecycle;
  std::uint32_t flags = lifecycle ? hftrec::replay::EventArrivalSourceLifecycle :
      hftrec::replay::EventArrivalApplicationFrame;
  if ((header.flags & corpus::BinaryMarketRecordRealtimeRegression) != 0u)
    flags |= hftrec::replay::EventArrivalRealtimeRegression;
  if ((header.flags &
       corpus::BinaryMarketRecordMonotonicNonIncreasing) != 0u) {
    flags |= hftrec::replay::EventArrivalMonotonicNonIncreasing;
  }
  if (!lifecycle && (header.flags & corpus::BinaryMarketRecordExchangeAheadOfReceive) != 0u)
    flags |= hftrec::replay::EventArrivalExchangeAheadOfReceive;
  if (!lifecycle && (header.flags &
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
        corpus::BinaryMarketRecordDepthState |
        corpus::BinaryMarketRecordDepthCoverage |
        corpus::BinaryMarketRecordDepthHasCoverage |
        corpus::BinaryMarketRecordDepthTransactionEnd |
        corpus::BinaryMarketRecordDepthSourcePosition;
  }
  return (header.flags & corpus::BinaryMarketRecordGapBoundary) == 0u &&
      (header.flags & ~allowed) == 0u;
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
      require(plan.requireBookTickers, output.bookTickers.empty(), "bookticker") &&
      require(plan.requireDepths, output.depths.empty(), "depth");
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

Status decodeBinaryMarketRecords(const corpus::BinaryMarketSelection& selection,
    BinaryDecodeState& state, SessionData& output, std::string& error) {
  auto& ingestSequence = state.ingestSequence;
  auto& retainedBboSequence = state.retainedBboSequence;
  auto& retainedBboSessionEpoch = state.retainedBboSessionEpoch;
  auto& retainedBboPayload = state.retainedBboPayload;
  auto& retainedBboPresenceFlags = state.retainedBboPresenceFlags;
  auto& depthRebase = state.depthRebase;
    std::size_t recordIndex = 0u;
    while (recordIndex < selection.records.size()) {
      const auto& record = selection.records[recordIndex];
      bool bboFreshnessOnly = false;
      if (!channelFlagsValid(record.header)) {
        error = "binary corpus record flags do not match its channel";
        return Status::CorruptData;
      }
      // Selection asks only for BBO/trades/depth. Defensively recognize
      // historical tags without reviving their old production replay chain.
      if (record.header.channel != corpus::BinaryMarketChannel::BookTicker &&
          record.header.channel != corpus::BinaryMarketChannel::Trade &&
          record.header.channel != corpus::BinaryMarketChannel::Depth) {
        if (depthRebase.active) {
          error = "retired channel interrupts binary corpus depth rebase transaction";
          return Status::CorruptData;
        }
        appendWarning(output, std::string{"retired public channel ignored: "} +
            channelName(record.header.channel));
        ++recordIndex;
        continue;
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
        if (!appendDepthTransaction(selection, recordIndex, state,
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
              !canonicalMarketQuantityToE8(selection.source, corpus::BinaryMarketChannel::BookTicker,
                           payload->bidQtyRaw, row.bidQtyE8) ||
              !rescaleToE8(payload->askPriceRaw,
                           selection.source.priceScale, row.askPriceE8) ||
              !canonicalMarketQuantityToE8(selection.source, corpus::BinaryMarketChannel::BookTicker,
                           payload->askQtyRaw, row.askQtyE8)) {
            error = "binary corpus bookticker cannot be represented exactly in E8";
            return Status::CorruptData;
          }
          row.tsNs = record.header.exchangeTimestampNs;
          row.ingestSeq = ingestSequence;
          row.arrival = arrival;
          row.nativeEvidence = payload->nativeEvidence;
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
          if (!payload || !cxet::api::market::validMarketNativeEvidence(payload->nativeEvidence) ||
              (payload->side != 1u && payload->side != 2u) ||
              payload->reserved != decltype(payload->reserved){}) {
            error = "invalid binary corpus trade payload";
            return Status::CorruptData;
          }
          TradeRow row{};
          if (!rescaleToE8(payload->priceRaw, selection.source.priceScale,
                           row.priceE8) ||
              !canonicalMarketQuantityToE8(selection.source, corpus::BinaryMarketChannel::Trade,
                           payload->qtyRaw, row.qtyE8) ||
              row.priceE8 <= 0 || row.qtyE8 <= 0) {
            error = "binary corpus trade cannot be represented exactly in E8";
            return Status::CorruptData;
          }
          row.side = payload->side == 1u ? 1 : 0;
          row.tsNs = record.header.exchangeTimestampNs;
          row.ingestSeq = ingestSequence;
          row.arrival = arrival;
          row.nativeEvidence = payload->nativeEvidence;
          row.nativeIdentity.nativeFirst = record.header.nativeIdentityFirst;
          row.nativeIdentity.nativeSecond = record.header.nativeIdentitySecond;
          row.nativeIdentity.nativeShape = static_cast<cxet::api::market::PublicMarketNativeIdentityShape>(
              record.header.nativeIdentityShape);
          row.nativeIdentity.origin = record.header.nativeIdentityShape == 4u
              ? cxet::api::market::PublicMarketIdentityOrigin::NativeTail64
              : (record.header.nativeIdentityShape == 0u
                  ? cxet::api::market::PublicMarketIdentityOrigin::SourcePosition
                  : cxet::api::market::PublicMarketIdentityOrigin::NativeExact);
          row.nativeIdentity.scope = record.header.nativeIdentityShape == 0u
              ? cxet::api::market::PublicMarketIdentityScope::Lane
              : cxet::api::market::PublicMarketIdentityScope::NativeSymbol;
          using Shape = cxet::api::market::PublicMarketNativeIdentityShape;
          Id projected{};
          projected.raw = row.nativeIdentity.nativeShape == Shape::U64Pair
              ? row.nativeIdentity.nativeSecond : row.nativeIdentity.nativeFirst;
          if (!trading_core::nativeTradeIdentityMatches(projected, row.nativeIdentity)) {
            error = "invalid binary corpus native trade identity";
            return Status::CorruptData;
          }
          output.trades.push_back(row);
          break;
        }
        // Recorder owns these immutable numeric tags; their public channels
        // are retired and never decoded or presented to strategies.
        case corpus::BinaryMarketChannel::PriceLimit:
        case corpus::BinaryMarketChannel::Liquidation:
        case corpus::BinaryMarketChannel::MarkPrice:
        case corpus::BinaryMarketChannel::IndexPrice:
        case corpus::BinaryMarketChannel::Funding:
          break;
        default:
          error = "unknown binary corpus channel tag";
          return Status::CorruptData;
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

  return Status::Ok;
}

Status initializeBinaryMarketSession(const corpus::BinaryMarketSelectionRequest& selectionRequest,
    const corpus::BinaryMarketSelection& selection, const SessionLoadPlan& plan,
    SessionData& output, std::string& error) {
    const ChannelPlan requested = channelPlan(plan);
    const auto selectedQuantities = static_cast<std::uint16_t>(requested.loadMask &
        selection.source.availableChannelMask & selection.source.traderReplayChannelMask);
    for (const auto channel : {corpus::BinaryMarketChannel::BookTicker, corpus::BinaryMarketChannel::Trade,
                              corpus::BinaryMarketChannel::Depth}) {
      if ((selectedQuantities & corpus::binaryMarketChannelBit(channel)) == 0u) continue;
      std::int64_t ignored{};
      if (!canonicalMarketQuantityToE8(selection.source, channel, std::int64_t{0}, ignored)) {
        error = "selected binary market quantity lane has no exact canonical conversion authority";
        return Status::Unsupported;
      }
    }
    const std::uint16_t optionalRecordedOnly = requested.loadMask &
        selection.source.availableChannelMask & ~selection.source.traderReplayChannelMask;
    output.sessionPath = selectionRequest.root;
    output.exchange = selectionRequest.exchange;
    output.market = selectionRequest.market;
    output.symbol = selectionRequest.canonicalSymbol;
    output.binaryMarketCorpus = true;
    output.capturedSourceId = selection.source.sourceId;
    output.canonicalInstrumentId = selection.source.canonicalSymbolId;
    output.assetDomainRaw = selection.source.assetDomainRaw;
    output.capturedMarketKindRaw = selection.source.marketKindRaw;
    output.capturedMarketRaw = selection.source.marketRaw;
    output.capturedQuantityConversion = selection.source.quantityConversion;
    output.quoteAssetId = selection.source.quoteAssetId;
    output.capturedInitiallyPresent = selection.source.initiallyPresent != 0u;
    output.capturedSourceGeneration = selection.source.initialSourceGeneration;
    output.capturedAvailableChannels = selection.source.availableChannelMask;
    output.instrument = {};
    output.replayAnchorMonotonicNs =
        selection.manifest.replayAnchorMonotonicNs;
    output.replayAnchorReceiveNs =
        selection.manifest.replayAnchorReceiveNs;
    if (plan.loadCandles) {
      appendWarning(output,
          "optional candle history is unavailable in live binary corpus");
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
                 (selection.source.availableChannelMask & bit) == 0u) {
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
    // Native contract lots remain corpus evidence. Execution requires the
    // separate producer-projected canonical base quantity grid, already E8.
    output.instrument.lotSizeE8 = 0;
    constexpr auto quantityFlags = corpus::BinaryMarketInstrumentCanonicalStepSize |
        corpus::BinaryMarketInstrumentExecutionQuantityStep;
    if ((selection.source.instrumentMetadataFlags & quantityFlags) == quantityFlags) {
      output.instrument.lotSizeE8 = selection.source.canonicalStepSizeRaw;
      output.instrument.executionQuantityStepE8 = selection.source.executionQuantityStepRaw;
      output.instrument.hasExecutionQuantityGrid = true;
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

    return Status::Ok;
}

}  // namespace binary_corpus

using namespace binary_corpus;

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

    const Status initialized = initializeBinaryMarketSession(selectionRequest, selection, plan, output, error);
    if (!isOk(initialized)) return initialized;

    std::array<std::size_t, corpus::kBinaryMarketChannelCount> counts{};
    for (const auto& record : selection.records) {
      switch (record.header.channel) {
        case corpus::BinaryMarketChannel::BookTicker:
        case corpus::BinaryMarketChannel::Trade:
        case corpus::BinaryMarketChannel::Depth:
        case corpus::BinaryMarketChannel::PriceLimit:
        case corpus::BinaryMarketChannel::Liquidation:
        case corpus::BinaryMarketChannel::MarkPrice:
        case corpus::BinaryMarketChannel::IndexPrice:
        case corpus::BinaryMarketChannel::Funding:
          break;
        default:
          error = "unknown binary corpus channel tag";
          return Status::CorruptData;
      }
      ++counts[corpus::binaryMarketChannelIndex(record.header.channel)];
    }
    output.trades.reserve(
        counts[corpus::binaryMarketChannelIndex(
            corpus::BinaryMarketChannel::Trade)]);
    output.bookTickers.reserve(
        counts[corpus::binaryMarketChannelIndex(
            corpus::BinaryMarketChannel::BookTicker)]);
    output.depths.reserve(
        counts[corpus::binaryMarketChannelIndex(
            corpus::BinaryMarketChannel::Depth)]);

    BinaryDecodeState decodeState{};
    const Status decoded = decodeBinaryMarketRecords(selection, decodeState, output, error);
    if (!isOk(decoded)) return decoded;
    const auto ingestSequence = decodeState.ingestSequence;

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
