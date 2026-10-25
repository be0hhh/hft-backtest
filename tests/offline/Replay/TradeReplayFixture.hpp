#pragma once

#include "OfflineCase.hpp"
#include "Engine/BacktestEngine.hpp"
#include "Config/BacktestRunConfig.hpp"
#include "Engine/BacktestEngineSetup.hpp"
#include "Engine/BacktestEngineView.hpp"
#include "Accounting/BacktestResultAggregation.hpp"
#include "Strategy/BacktestStrategyRuntime.hpp"
#include "Strategy/Inputs/BacktestStrategyInputFreshness.hpp"
#include "Strategy/Diagnostic/BacktestStrategyTrace.hpp"
#include "Strategy/Execution/BacktestStrategyIntentLowering.hpp"
#include "Strategy/Internal/BacktestStrategyRuntimeHelpers.hpp"
#include "Execution/Internal/BacktestExecutionHelpers.hpp"
#include "Execution/BacktestOrderEvidence.hpp"
#include "trading_core/Strategy/Events/UserEventView.hpp"
#include "strategy/trader/DeliverySumSignal/Descriptor.hpp"
#include "Input/Session/Internal/SessionLoaderHelpers.hpp"
#include "Input/Binary/BinaryCorpusLoader.hpp"
#include "Input/Binary/BinaryCorpusStream.hpp"
#include "Input/Binary/Internal/BinaryCorpusHelpers.hpp"
#include "Run/BacktestUniverse.hpp"
#include "Run/Internal/BacktestRunContext.hpp"
#include "Simulation/SimulationInput.hpp"
#include "cxet/Runtime/Market/DepthFeedComposer.hpp"
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
            (part == 1u ? corpus::BinaryMarketRecordDepthTransactionEnd : 0u) |
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
      record.header.flags = corpus::BinaryMarketRecordDepthState | corpus::BinaryMarketRecordDepthTransactionEnd;
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

hftrec::corpus::BinaryMarketSelection coverageDepthSelection() {
  namespace corpus = hftrec::corpus;
  corpus::BinaryMarketSelection selection{};
  selection.manifest.producerEpoch = 13u;
  selection.source.priceScale = selection.source.quantityScale = 8u;
  selection.source.quantityConversion.canonicalBaseMultiplier =
      selection.source.quantityConversion.nativeBaseMultiplier = 1u;
  selection.source.quantityConversion.kinds[2] = corpus::BinaryMarketQuantityKind::CanonicalBase;
  selection.records.resize(3u);
  auto& root = selection.records[0];
  auto& header = root.header;
  header.sourceId = 1u; header.sourceGeneration = 11u; header.sessionEpoch = 17u;
  header.eventSequence = 7u; header.frameSequence = 9u; header.shardSequence = 1u;
  header.exchangeTimestampNs = 100; header.receiveRealtimeNs = 500;
  header.receiveMonotonicNs = 600u; header.channel = corpus::BinaryMarketChannel::Depth;
  header.flags = corpus::BinaryMarketRecordTraderReplayCompatible | corpus::BinaryMarketRecordDepthSnapshot |
      corpus::BinaryMarketRecordDepthRebase | corpus::BinaryMarketRecordDepthHasCoverage |
      corpus::BinaryMarketRecordDepthSourcePosition;
  header.payloadBytes = sizeof(corpus::BinaryMarketDepthChunkPayload);
  auto& depth = *corpus::binaryMarketPayload<corpus::BinaryMarketDepthChunkPayload>(&root);
  depth.totalLevelCount = depth.levelCount = 2u;
  depth.partCount = depth.transactionPartCount = 1u;
  depth.frameKind = corpus::BinaryMarketDepthFrameKind::Snapshot;
  depth.state = corpus::BinaryMarketDepthState::RebaseInFlight;
  depth.sequence.firstUpdateId = depth.sequence.lastUpdateId = 42u;
  depth.sourceReceiveRealtimeNs = 450u; depth.sourceFreshnessMonotonicNs = 550u;
  using Field = cxet::api::market::MarketNativeField;
  depth.nativeEvidence.set(Field::EventTime, 101u);
  depth.nativeEvidence.set(Field::TransactionTime, 102u);
  depth.nativeEvidence.set(Field::MatchingEngineTime, 103u);
  depth.nativeEvidence.set(Field::SendTime, 104u);
  depth.nativeEvidence.set(Field::UpdateId, 0u);
  depth.nativeEvidence.updateGuarantee = cxet::api::market::MarketNativeCounterGuarantee::OrderOnly;
  depth.nativeEvidence.set(Field::CrossSequence, 12u);
  depth.levels[0] = {99u, 2u, 1u, 1u, 0u, 0u};
  depth.levels[1] = {101u, 3u, 2u, 1u, 0u, 0u};
  for (std::uint16_t part = 0u; part < 2u; ++part) {
    auto& record = selection.records[part + 1u];
    record.header = header;
    record.header.shardSequence = part + 2u;
    record.header.eventOrdinal = part;
    record.header.flags = corpus::BinaryMarketRecordTraderReplayCompatible |
        corpus::BinaryMarketRecordDepthCoverage | corpus::BinaryMarketRecordDepthHasCoverage |
        corpus::BinaryMarketRecordDepthSourcePosition |
        (part == 1u ? corpus::BinaryMarketRecordDepthTransactionEnd : 0u);
    record.header.payloadBytes = sizeof(corpus::BinaryMarketDepthCoveragePayload);
    auto& payload = *corpus::binaryMarketPayload<corpus::BinaryMarketDepthCoveragePayload>(&record);
    payload.totalCoverageCount = 6u; payload.coverageOffset = part * 5u;
    payload.coverageCount = part == 0u ? 5u : 1u;
    payload.partIndex = part; payload.partCount = 2u; payload.transactionPartCount = 1u;
    for (std::uint16_t index = 0u; index < payload.coverageCount; ++index) {
      auto& feed = payload.coverage[index];
      feed.sourceId = payload.coverageOffset + index + 10u; feed.sequenceDomain = 3u;
      feed.maximumLevelsPerSide = 5u; feed.sequenceFlags = cxet::market::DepthEventSourcePosition;
      feed.sourceGeneration = 20u; feed.sessionEpoch = 21u; feed.lastUpdateId = 22u;
      feed.freshnessMonotonicNs = 540u; feed.lowestBidPriceRaw = 90; feed.highestAskPriceRaw = 110;
      feed.lowestProtectedBidPriceRaw = 80; feed.highestProtectedAskPriceRaw = 120;
      feed.state = corpus::BinaryMarketDepthState::Healthy; feed.snapshotAligned = 1u;
      feed.nativeEvidence = depth.nativeEvidence;
    }
  }
  return selection;
}

struct CoreComposedReplayFixture {
  cxet::runtime::market::LocalOrderBookStore books;
  cxet::runtime::market::DepthFeedComposer composer;
  std::vector<hftrec::corpus::BinaryMarketSelection> cuts;
  std::uint64_t nextShardSequence{1u};
  CoreComposedReplayFixture() {
    const std::array<std::uint32_t, 1u> capacities{8u};
    const std::array<cxet::runtime::market::DepthFeedComposition, 1u> feeds{{{1u, 1u, 0u, 8u}}};
    CXET_CHECK(books.initialize(capacities));
    CXET_CHECK(composer.initialize(capacities, feeds));
  }
  cxet::runtime::market::DepthCommitSink sink() {
    return {.context = this, .commit = +[](void* context,
        const cxet::runtime::market::DepthCommitTransaction& tx) noexcept {
      namespace corpus = hftrec::corpus;
      try {
        auto& self = *static_cast<CoreComposedReplayFixture*>(context);
        if (tx.parts.size() != 1u || tx.coverage.empty() || tx.coverage.size() > 5u ||
            tx.parts[0].metadata.mutationCount > 32u)
          return cxet::runtime::market::DepthCommitStatus::InvalidInput;
        auto selection = coverageDepthSelection(); selection.records.resize(2u);
        const auto& metadata = tx.parts[0].metadata;
        auto& record = selection.records[0]; record = {};
        auto& h = record.header;
        h.sourceId = metadata.sourceId; h.sourceGeneration = metadata.sourceGeneration;
        h.sessionEpoch = metadata.sessionEpoch; h.eventSequence = metadata.publicationSequence;
        h.frameSequence = metadata.publicationSequence; h.shardSequence = self.nextShardSequence++;
        h.exchangeTimestampNs = static_cast<std::int64_t>(metadata.exchangeTimestampNs);
        h.receiveRealtimeNs = static_cast<std::int64_t>(1'000u + metadata.publicationSequence);
        h.receiveMonotonicNs = 2'000u + metadata.publicationSequence; h.channel = corpus::BinaryMarketChannel::Depth;
        h.flags = corpus::BinaryMarketRecordTraderReplayCompatible | corpus::BinaryMarketRecordDepthSnapshot |
            corpus::BinaryMarketRecordDepthSourcePosition | corpus::BinaryMarketRecordDepthHasCoverage;
        if (metadata.state == cxet::market::DepthSourceState::Partial) h.flags |= corpus::BinaryMarketRecordDepthPartial;
        if (!h.exchangeTimestampNs) {
          h.flags |= corpus::BinaryMarketRecordExchangeTimestampMissing;
          h.sourceFlags |= corpus::BinaryMarketSourceExchangeTimestampMissing;
        }
        h.payloadBytes = sizeof(corpus::BinaryMarketDepthChunkPayload);
        auto& p = *corpus::binaryMarketPayload<corpus::BinaryMarketDepthChunkPayload>(&record);
        p.sequence = {metadata.sequence.firstUpdateId, metadata.sequence.lastUpdateId, metadata.sequence.previousUpdateId};
        p.totalLevelCount = metadata.mutationCount; p.levelCount = static_cast<std::uint16_t>(metadata.mutationCount);
        p.partCount = p.transactionPartCount = 1u; p.frameKind = corpus::BinaryMarketDepthFrameKind::Snapshot;
        p.state = static_cast<corpus::BinaryMarketDepthState>(metadata.state);
        p.failure = static_cast<std::uint8_t>(metadata.failure);
        p.sourceReceiveRealtimeNs = metadata.receiveTimestampNs;
        p.sourceFreshnessMonotonicNs = metadata.freshnessMonotonicNs; p.nativeEvidence = metadata.nativeEvidence;
        std::size_t level = 0u;
        for (const auto& chunk : tx.parts[0].mutationChunks) for (std::uint32_t i=0u; i<chunk.count; ++i) {
          if (level == p.levels.size()) return cxet::runtime::market::DepthCommitStatus::InvalidInput;
          const auto& value = chunk.data[i];
          p.levels[level++] = {static_cast<std::uint64_t>(value.priceRaw), static_cast<std::uint64_t>(value.quantityRaw),
              static_cast<std::uint8_t>(value.side), static_cast<std::uint8_t>(value.action), 0u, 0u};
        }
        if (level != metadata.mutationCount) return cxet::runtime::market::DepthCommitStatus::InvalidInput;
        auto& tail = selection.records[1]; tail = {}; tail.header = h;
        tail.header.shardSequence = self.nextShardSequence++;
        tail.header.flags &= ~static_cast<std::uint32_t>(corpus::BinaryMarketRecordDepthSnapshot | corpus::BinaryMarketRecordDepthPartial);
        tail.header.flags |= corpus::BinaryMarketRecordDepthCoverage | corpus::BinaryMarketRecordDepthTransactionEnd;
        tail.header.payloadBytes = sizeof(corpus::BinaryMarketDepthCoveragePayload);
        auto& coverage = *corpus::binaryMarketPayload<corpus::BinaryMarketDepthCoveragePayload>(&tail);
        coverage.totalCoverageCount = static_cast<std::uint32_t>(tx.coverage.size());
        coverage.coverageCount = static_cast<std::uint16_t>(tx.coverage.size());
        coverage.partCount = coverage.transactionPartCount = 1u;
        for (std::size_t i=0u; i<tx.coverage.size(); ++i)
          coverage.coverage[i] = corpus::encodeBinaryMarketDepthCoverage(tx.coverage[i]);
        self.cuts.push_back(std::move(selection));
        return cxet::runtime::market::DepthCommitStatus::Committed;
      } catch (...) { return cxet::runtime::market::DepthCommitStatus::CapacityExceeded; }
    }};
  }
  void publish(std::uint64_t sequence, bool snapshot) {
    using namespace cxet::runtime::market;
    const std::array<cxet::market::DepthMutation, 1u> mutations{{
        {.priceRaw=99, .quantityRaw=static_cast<std::int64_t>(sequence + 1u), .side=cxet::market::DepthSide::Bid}}};
    auto* book = books.book(1u);
    const cxet::market::DepthSequence nativeSequence{sequence, sequence, snapshot ? 0u : sequence - 1u};
    if (snapshot) {
      CXET_CHECK(book->stageSnapshot(nativeSequence, mutations) == LocalOrderBookStatus::RebaseStaged);
      CXET_CHECK(book->commitRebase() == LocalOrderBookStatus::RebaseCommitted);
    } else CXET_CHECK(book->applyPartialDelta(nativeSequence, mutations) == LocalOrderBookStatus::Partial);
    cxet::api::market::MarketNativeEvidence native;
    native.set(cxet::api::market::MarketNativeField::EventTime, 100u + sequence);
    native.set(cxet::api::market::MarketNativeField::UpdateId, sequence);
    native.updateGuarantee = cxet::api::market::MarketNativeCounterGuarantee::OrderOnly;
    DepthBookProjection projection{11u, 17u, sequence, 500u + sequence, 100u + sequence};
    projection.nativeEvidence = native; book->commitProjection(projection);
    const DepthCommitPart::MutationChunk chunk{mutations.data(), 1u};
    const DepthCommitPart part{.metadata={.sourceId=1u, .mutationCount=1u, .sourceGeneration=11u,
        .sessionEpoch=17u, .publicationSequence=sequence, .exchangeTimestampNs=100u + sequence,
        .receiveTimestampNs=500u + sequence, .freshnessMonotonicNs=600u + sequence,
        .sequence=nativeSequence, .flags=snapshot ? cxet::market::DepthEventSnapshot | cxet::market::DepthEventRebase
            : cxet::market::DepthEventDelta | cxet::market::DepthEventPartial,
        .state=book->state(), .nativeEvidence=native}, .mutationChunks={&chunk, 1u}};
    const cxet::market::DepthSourceStateRecord state{.sourceId=1u, .sourceGeneration=11u, .sessionEpoch=17u,
        .publicationSequence=sequence, .lastUpdateId=sequence, .lastExchangeTimestampNs=100u + sequence,
        .lastFreshnessMonotonicNs=600u + sequence, .state=book->state(), .nativeEvidence=native};
    composer.commit(books, {sequence, {&part, 1u}, state}, sink());
  }
  void gap(std::uint64_t sequence) {
    books.book(1u)->markGap();
    const cxet::market::DepthSourceStateRecord state{.sourceId=1u, .sourceGeneration=11u, .sessionEpoch=17u,
        .publicationSequence=sequence, .lastUpdateId=sequence - 1u,
        .lastExchangeTimestampNs=100u + sequence, .lastFreshnessMonotonicNs=600u + sequence,
        .state=cxet::market::DepthSourceState::Gap, .failure=cxet::market::DepthFailure::ExchangeSequenceGap};
    composer.observeState(books, state, sink());
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
