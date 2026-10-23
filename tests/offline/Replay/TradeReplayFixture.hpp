#pragma once

#include "OfflineCase.hpp"
#include "Engine/BacktestEngine.hpp"
#include "Engine/BacktestEngineSetup.hpp"
#include "Engine/BacktestEngineView.hpp"
#include "Accounting/BacktestResultAggregation.hpp"
#include "Strategy/BacktestStrategyRuntime.hpp"
#include "Strategy/Inputs/BacktestStrategyInputFreshness.hpp"
#include "Strategy/Diagnostics/BacktestStrategyTrace.hpp"
#include "Strategy/Execution/BacktestStrategyIntentLowering.hpp"
#include "Strategy/Internal/BacktestStrategyRuntimeHelpers.hpp"
#include "Execution/Internal/BacktestExecutionHelpers.hpp"
#include "Execution/BacktestOrderEvidence.hpp"
#include "trading_core/Strategy/Events/UserEventView.hpp"
#include "strategy/trader/DeliverySumSignal/Descriptor.hpp"
#include "Input/Session/Internal/SessionLoaderHelpers.hpp"
#include "Input/Binary/BinaryCorpusLoader.hpp"
#include "Input/Binary/BinaryCorpusStream.hpp"
#include "Run/BacktestUniverse.hpp"
#include "hft_backtest/StrategyMetadata.hpp"
#include "hftrec/CorpusContract/BinaryMarketCorpusReader.hpp"
#include "hftrec/CorpusContract/BinaryMarketCorpusWriter.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <unistd.h>

namespace {
namespace detail = hft_backtest::detail;
using Initiator = cxet::composite::TradeInitiatorSide;

void prepareCashTradeReplayEngine(detail::EngineState& engine) {
  const auto nativeProduct = engine.market.raw;
  engine.accountingModel = detail::SimulationAccountingModel::Cash;
  hft_backtest::BacktestRunRequest request{}; request.initialBalanceE8 = 100'000'000;
  request.hasMakerFeeBps = request.hasTakerFeeBps = true;
  detail::initializeAccounting(engine, request);
  CXET_CHECK(engine.market.raw == nativeProduct);
}

constexpr std::string_view negativePrice =
    R"([-1,2,1,100,7,7,7,2,0,"BTCUSDT","Binance","Futures",1,1,0,0,0,0,0,0,0,0,0,0,0])";
constexpr std::string_view negativeQuantity =
    R"([1,-2,1,100,7,7,7,2,0,"BTCUSDT","Binance","Futures",1,1,0,0,0,0,0,0,0,0,0,0,0])";
constexpr std::string_view zeroMagnitudes =
    R"([0,0,0,100,7,7,7,0,0,"BTCUSDT","Binance","Futures",1,1,0,0,0,0,0,0,0,0,0,0,0])";

struct BinaryTradeFixture {
  std::filesystem::path root = std::filesystem::current_path() /
      ("backtest-identity-fixture-" + std::to_string(::getpid()) + "-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::array<hftrec::corpus::BinaryMarketSource, 1u> sources{};
  hftrec::corpus::BinaryMarketCorpusWriter writer;
  BinaryTradeFixture(std::uint8_t shape, std::uint64_t first, std::uint64_t second, std::uint32_t rows = 1u, bool depthMode = false, bool lifecycleMode = false, bool healthMode = false, bool captureGapMode = false, bool nativeDepthGapMode = false) {
    namespace corpus = hftrec::corpus;
    auto& source = sources[0];
    source.sourceId = 1u; source.canonicalSymbolId = 7u; source.initialSourceGeneration = 11u;
    source.venueId = 1u; source.marketRaw = 1u; source.marketKindRaw = 2u;
    source.economicBaseAssetId = 1u; source.quoteAssetId = 2u;
    source.priceScale = source.quantityScale = 8u;
    source.tickSizeRaw = source.stepSizeRaw = 1;
    source.canonicalStepSizeRaw=source.executionQuantityStepRaw=1;
    source.quantityConversion.canonicalBaseMultiplier=source.quantityConversion.nativeBaseMultiplier=1u;
    source.quantityConversion.kinds[depthMode?2u:1u]=corpus::BinaryMarketQuantityKind::NativeBase;
    source.directoryFlags = (depthMode ? corpus::BinaryMarketDirectoryDepth : corpus::BinaryMarketDirectoryTrade) | corpus::BinaryMarketDirectoryTickSize;
    source.instrumentMetadataFlags = corpus::BinaryMarketInstrumentStepSize |
        corpus::BinaryMarketInstrumentCanonicalStepSize | corpus::BinaryMarketInstrumentExecutionQuantityStep;
    source.configuredChannelMask = source.availableChannelMask = source.traderReplayChannelMask =
        (depthMode ? corpus::binaryMarketChannelBit(corpus::BinaryMarketChannel::Depth) :
            corpus::binaryMarketChannelBit(corpus::BinaryMarketChannel::Trade));
    if (depthMode) source.compatibility[corpus::binaryMarketChannelIndex(corpus::BinaryMarketChannel::Depth)] = corpus::BinaryMarketCompatibility::ExactTraderReplay;
    if (!depthMode) source.compatibility[corpus::binaryMarketChannelIndex(corpus::BinaryMarketChannel::Trade)] =
        corpus::BinaryMarketCompatibility::ExactTraderReplay;
    constexpr std::string_view exchange = "fixture", market = "futures", symbol = "BTCUSDT";
    for (std::size_t i = 0; i < exchange.size(); ++i) source.venue[i] = exchange[i];
    for (std::size_t i = 0; i < market.size(); ++i) source.market[i] = market[i];
    for (std::size_t i = 0; i < symbol.size(); ++i) source.canonicalSymbol[i] = source.nativeSymbol[i] = symbol[i];
    source.venueBytes = exchange.size(); source.marketBytes = market.size();
    source.canonicalSymbolBytes = source.nativeSymbolBytes = symbol.size();
    corpus::BinaryMarketWriterConfig config{};
    config.root = root; config.sources = sources; config.producerEpoch = 13u;
    config.maximumBytes = 128u * 1024u; config.segmentTargetBytes = 4096u;
    config.targetDurationNs = 10'000u; config.startedReceiveNs = 100;
    config.startedMonotonicNs = 100u; config.ringCapacity = 2u; config.shardCount = 1u;
    CXET_CHECK(writer.start(config) == hftrec::Status::Ok);
    corpus::BinaryMarketRecord record{}; auto& header = record.header;
    header.sourceId = 1u; header.sourceGeneration = 11u; header.sessionEpoch = 17u;
    header.eventSequence = header.frameSequence = header.shardSequence = 1u;
    header.exchangeTimestampNs = 199; header.receiveRealtimeNs = 200;
    header.receiveMonotonicNs = 200u; header.channel = corpus::BinaryMarketChannel::Trade;
    header.flags = corpus::BinaryMarketRecordTraderReplayCompatible;
    header.payloadBytes = sizeof(corpus::BinaryMarketTradePayload);
    header.nativeIdentityShape = shape; header.nativeIdentityFirst = first; header.nativeIdentitySecond = second;
    *corpus::binaryMarketPayload<corpus::BinaryMarketTradePayload>(&record) = {1, 2, 1u, {}};
    if (depthMode) {
      header.channel = corpus::BinaryMarketChannel::Depth;
      header.payloadBytes = sizeof(corpus::BinaryMarketDepthChunkPayload);
      header.nativeIdentityShape = 0u;
      header.nativeIdentityFirst = header.nativeIdentitySecond = 0u;
      for (std::uint16_t part = 0u; part < 2u; ++part) {
        header.eventSequence = header.shardSequence = part + 1u;
        header.frameSequence = 1u;
        header.exchangeTimestampNs = part == 0u ? 199 : 150;
        header.flags = corpus::BinaryMarketRecordTraderReplayCompatible |
            corpus::BinaryMarketRecordDepthRebase |
            (part == 0u ? corpus::BinaryMarketRecordDepthSnapshot : corpus::BinaryMarketRecordDepthDelta);
        auto& payload = *corpus::binaryMarketPayload<corpus::BinaryMarketDepthChunkPayload>(&record);
        payload = {};
        payload.totalLevelCount = payload.levelCount = part == 0u ? 2u : 1u;
        payload.partCount = 1u;
        payload.frameKind = part == 0u ? corpus::BinaryMarketDepthFrameKind::Snapshot : corpus::BinaryMarketDepthFrameKind::Delta;
        payload.state = corpus::BinaryMarketDepthState::RebaseInFlight;
        payload.sequence.firstUpdateId = payload.sequence.lastUpdateId = 100u + part;
        payload.sequence.previousUpdateId = part == 0u ? 0u : 100u;
        payload.transactionPartIndex = part;
        payload.transactionPartCount = 2u;
        payload.sourceReceiveRealtimeNs = payload.sourceFreshnessMonotonicNs = 200u;
        payload.levels[0].priceRaw = 100u;
        payload.levels[0].quantityRaw = part == 0u ? 1u : 3u;
        payload.levels[0].side = payload.levels[0].action = 1u;
        if (part == 0u) {
          payload.levels[1].priceRaw = 101u; payload.levels[1].quantityRaw = 2u;
          payload.levels[1].side = 2u; payload.levels[1].action = 1u;
        }
        CXET_CHECK(writer.append(record) == hftrec::Status::Ok);
      }
    } else {
    for (std::uint32_t row = 0u; row < rows; ++row) {
      header.eventSequence = header.frameSequence = header.shardSequence = row + 1u;
      header.receiveRealtimeNs = 200 + row;
      header.receiveMonotonicNs = 200u + row;
      // Deliberately regress exchange time: it must never reorder arrivals.
      header.exchangeTimestampNs = 199 - row;
      CXET_CHECK(writer.append(record) == hftrec::Status::Ok);
    }
    }
    if (lifecycleMode) {
      const auto marketRecord = record;
      for (std::uint64_t sequence = 2u; sequence <= 3u; ++sequence) {
        record = {}; auto& lifeHeader = record.header;
        lifeHeader.sourceId = 1u; lifeHeader.sourceGeneration = 11u;
        lifeHeader.sessionEpoch = sequence == 2u ? 17u : 18u;
        lifeHeader.eventSequence = lifeHeader.shardSequence = sequence;
        lifeHeader.receiveRealtimeNs = 150 + sequence * 50;
        lifeHeader.receiveMonotonicNs = lifeHeader.receiveRealtimeNs;
        lifeHeader.channel = corpus::BinaryMarketChannel::SourceLifecycle;
        lifeHeader.flags = corpus::BinaryMarketRecordSourceLifecycle |
            corpus::BinaryMarketRecordTraderReplayCompatible | corpus::BinaryMarketRecordExchangeTimestampMissing;
        lifeHeader.payloadBytes = sizeof(corpus::BinaryMarketSourceLifecyclePayload);
        auto& life = *corpus::binaryMarketPayload<corpus::BinaryMarketSourceLifecyclePayload>(&record);
        life.source = sources[0];
        life.directoryRevision = life.requestSequence = sequence;
        life.previousSourceGeneration = 11u;
        life.affectedChannel = static_cast<std::uint8_t>(corpus::BinaryMarketChannel::Trade);
        life.kind = sequence == 2u ? corpus::BinaryMarketSourceLifecycleKind::Removed : corpus::BinaryMarketSourceLifecycleKind::Readded;
        life.channelMaskBefore = sequence == 2u ? source.availableChannelMask : 0u;
        life.channelMaskAfter = sequence == 2u ? 0u : source.availableChannelMask;
        if (sequence == 2u) {
          life.source.availableChannelMask = life.source.traderReplayChannelMask = 0u;
          life.source.compatibility.fill(corpus::BinaryMarketCompatibility::Unavailable);
        }
        CXET_CHECK(writer.append(record) == hftrec::Status::Ok);
      }
      record = marketRecord;
      record.header.eventSequence = record.header.shardSequence = 4u;
      record.header.frameSequence = 2u; record.header.sessionEpoch = 18u;
      record.header.receiveRealtimeNs = record.header.receiveMonotonicNs = 350u;
      CXET_CHECK(writer.append(record) == hftrec::Status::Ok);
    }
    if (healthMode || captureGapMode) {
      const auto good = record;
      record.header.eventSequence = record.header.shardSequence = 2u;
      record.header.frameSequence = 2u;
      record.header.receiveRealtimeNs = record.header.receiveMonotonicNs = 250u;
      if (healthMode) record.header.sourceFlags = corpus::BinaryMarketSourceStale;
      if (captureGapMode) record.header.flags |= corpus::BinaryMarketRecordGapBoundary;
      CXET_CHECK(writer.append(record) == hftrec::Status::Ok);
      if (healthMode) {
        record = good;
        record.header.eventSequence = record.header.shardSequence = 3u;
        record.header.frameSequence = 3u;
        record.header.receiveRealtimeNs = record.header.receiveMonotonicNs = 300u;
        CXET_CHECK(writer.append(record) == hftrec::Status::Ok);
      }
    }
    if (nativeDepthGapMode) {
      record.header.eventSequence = record.header.shardSequence = 3u;
      record.header.frameSequence = 2u;
      record.header.receiveRealtimeNs = record.header.receiveMonotonicNs = 250u;
      record.header.flags = corpus::BinaryMarketRecordDepthState;
      record.header.sourceFlags = corpus::BinaryMarketSourceSequenceGap;
      record.payload = {};
      auto& payload = *corpus::binaryMarketPayload<corpus::BinaryMarketDepthChunkPayload>(&record);
      payload.frameKind = corpus::BinaryMarketDepthFrameKind::State;
      payload.state = corpus::BinaryMarketDepthState::Gap; payload.failure = 7u;
      payload.partCount = payload.transactionPartCount = 1u;
      payload.sourceReceiveRealtimeNs = payload.sourceFreshnessMonotonicNs = 250u;
      CXET_CHECK(writer.append(record) == hftrec::Status::Ok);
    }
    CXET_CHECK(writer.finalize(corpus::BinaryMarketStopReason::Requested, 10'000) == hftrec::Status::Ok);
  }
  ~BinaryTradeFixture() { std::error_code error; std::filesystem::remove_all(root, error); }
  hft_backtest::BacktestCorpusSourceRequest request() const {
    return {"fixture", "futures", "BTCUSDT", 100, 10'000};
  }
  detail::SessionLoadPlan plan() const {
    detail::SessionLoadPlan result{}; result.loadBookTickers = false; result.requireTrades = true;
    return result;
  }
};

// Topology fixture only; no strategy callbacks or executable replay adapter.
trading_core::StrategyDescriptor singleSourceTopology() {
  trading_core::StrategyDescriptor descriptor{};
  descriptor.minLegs = descriptor.maxLegs = 1u;
  descriptor.domainMask = trading_core::StrategyDomainMask::All;
  descriptor.universeTopology = trading_core::StrategyUniverseTopology::SingleSource;
  return descriptor;
}

}
