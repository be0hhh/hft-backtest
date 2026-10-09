#include "OfflineCase.hpp"
#include "Engine/BacktestEngine.hpp"
#include "Engine/BacktestEngineView.hpp"
#include "Accounting/BacktestResultAggregation.hpp"
#include "Strategy/BacktestStrategyRuntime.hpp"
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
#include "strategy/hft-trader/SpreadMaker1and2/Descriptor.hpp"
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

struct UniverseAuditFixture {
  std::filesystem::path root=std::filesystem::current_path()/
      ("backtest-universe-audit-"+std::to_string(::getpid())+"-"+
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  explicit UniverseAuditFixture(bool changeQuote,bool splitVenues=false) {
    namespace corpus=hftrec::corpus;
    std::array<corpus::BinaryMarketSource,2u> sources{};
    for(std::size_t i=0u;i<sources.size();++i) {
      auto& source=sources[i];source.sourceId=i+1u;source.canonicalSymbolId=i+7u;
      source.initialSourceGeneration=11u;source.venueId=(changeQuote || splitVenues) && i?detail::exchangeFromText("bybit").raw:1u;source.marketRaw=2u;
      source.marketKindRaw=1u;source.assetDomainRaw=1u;
      source.economicBaseAssetId=i+1u;source.quoteAssetId=2u;
      source.priceScale=source.quantityScale=8u;source.tickSizeRaw=source.stepSizeRaw=1;
      source.canonicalStepSizeRaw=source.executionQuantityStepRaw=1;
      source.quantityConversion.canonicalBaseMultiplier=source.quantityConversion.nativeBaseMultiplier=1u;
      source.quantityConversion.kinds[0]=corpus::BinaryMarketQuantityKind::CanonicalBase;
      source.directoryFlags=corpus::BinaryMarketDirectoryBookTicker|corpus::BinaryMarketDirectoryTickSize;
      source.instrumentMetadataFlags=corpus::BinaryMarketInstrumentStepSize|
          corpus::BinaryMarketInstrumentCanonicalStepSize|corpus::BinaryMarketInstrumentExecutionQuantityStep;
      source.configuredChannelMask=source.availableChannelMask=source.traderReplayChannelMask=1u;
      source.compatibility[0]=corpus::BinaryMarketCompatibility::ExactTraderReplay;
      const auto text=[](auto& dst,std::string_view value) {std::copy(value.begin(),value.end(),dst.begin());};
      text(source.venue,(changeQuote || splitVenues) && i?"bybit":"binance");source.venueBytes=(changeQuote || splitVenues) && i?5u:7u;text(source.market,"spot");source.marketBytes=4u;
      text(source.canonicalSymbol,i?"ETH_USDT":"BTC_USDT");source.canonicalSymbolBytes=8u;
      text(source.nativeSymbol,i?"ETHUSDT":"BTCUSDT");source.nativeSymbolBytes=7u;
    }
    corpus::BinaryMarketWriterConfig cfg{};cfg.root=root;cfg.sources=sources;cfg.producerEpoch=13u;
    cfg.maximumBytes=128u*1024u;cfg.segmentTargetBytes=4096u;cfg.targetDurationNs=10'000u;
    cfg.startedReceiveNs=cfg.startedMonotonicNs=100u;cfg.ringCapacity=2u;cfg.shardCount=1u;
    corpus::BinaryMarketCorpusWriter writer;CXET_CHECK(writer.start(cfg)==hftrec::Status::Ok);
    std::uint64_t sequence=1u;
    if(changeQuote) {
      corpus::BinaryMarketRecord row{};auto& h=row.header;
      h.sourceId=2u;h.sourceGeneration=12u;h.sessionEpoch=17u;h.eventSequence=h.shardSequence=sequence++;
      h.receiveRealtimeNs=h.receiveMonotonicNs=200u;h.channel=corpus::BinaryMarketChannel::SourceLifecycle;
      h.flags=corpus::BinaryMarketRecordSourceLifecycle|corpus::BinaryMarketRecordTraderReplayCompatible|corpus::BinaryMarketRecordExchangeTimestampMissing;
      h.payloadBytes=sizeof(corpus::BinaryMarketSourceLifecyclePayload);
      auto& life=*corpus::binaryMarketPayload<corpus::BinaryMarketSourceLifecyclePayload>(&row);
      life.source=sources[1];life.source.quoteAssetId=3u;life.source.initialSourceGeneration=12u;
      life.directoryRevision=life.requestSequence=1u;life.previousSourceGeneration=11u;
      life.channelMaskBefore=life.channelMaskAfter=1u;life.kind=corpus::BinaryMarketSourceLifecycleKind::DirectoryChanged;
      life.affectedChannel=1u;CXET_CHECK(writer.append(row)==hftrec::Status::Ok);
    }
    for(std::size_t i=0u;i<sources.size();++i) {
      corpus::BinaryMarketRecord row{};auto& h=row.header;
      h.sourceId=i+1u;h.sourceGeneration=changeQuote && i?12u:11u;h.sessionEpoch=17u;
      h.eventSequence=h.shardSequence=sequence++;h.frameSequence=i+1u;
      h.receiveRealtimeNs=h.receiveMonotonicNs=400u+i;h.exchangeTimestampNs=399u+i;
      h.flags=corpus::BinaryMarketRecordTraderReplayCompatible;h.payloadBytes=sizeof(corpus::BinaryMarketBookTickerPayload);
      h.sourceFlags=corpus::BinaryMarketSourceBookTickerBidPresent|corpus::BinaryMarketSourceBookTickerAskPresent;
      *corpus::binaryMarketPayload<corpus::BinaryMarketBookTickerPayload>(&row)={100'000'000,100'000'000,101'000'000,100'000'000};
      CXET_CHECK(writer.append(row)==hftrec::Status::Ok);
    }
    CXET_CHECK(writer.finalize(corpus::BinaryMarketStopReason::Requested,10'000)==hftrec::Status::Ok);
    std::ofstream config(root/"strategy.ini");
    config<<"[strategy]\ntype=spread_maker1and2\ndistance_bps=10\ntrigger_bps=10\norder_qty=0.00000001\nrefresh_ms=1\n"
          "[venue.binance_spot]\nsymbols="<<((changeQuote || splitVenues)?"BTC_USDT":"BTC_USDT,ETH_USDT")<<"\nrate_limits_enabled=false\n";
    if(changeQuote || splitVenues) config<<"[venue.bybit_spot]\nsymbols=ETH_USDT\nrate_limits_enabled=false\n";
  }
  ~UniverseAuditFixture(){std::error_code error;std::filesystem::remove_all(root,error);}
  hft_backtest::BacktestRunResult run(bool enableVenue=false,bool globalEnabled=true) const {
    hft_backtest::BacktestRunRequest request{};request.corpusPath=root;request.configPath=root/"strategy.ini";
    request.corpusBeginReceiveNs=300;request.corpusEndReceiveNs=10'000;request.writeArtifacts=true;
    request.outputPath=root/"result";request.initialBalanceE8=100'000'000;
    request.rateLimitsEnabled=globalEnabled;request.strictRateLimitRejects=true;
    if(enableVenue) {
      hft_backtest::BacktestRateLimitSchedule policy{};policy.exchange="binance";policy.market="spot";
      hft_backtest::BacktestQuotaBucketConfig bucket{};bucket.kind=trading_core::RateLimitBucketKind::Orders;
      bucket.limit=1000;bucket.intervalNs=1'000'000;policy.buckets.push_back(bucket);
      for(const auto kind:{trading_core::RateLimitActionKind::LimitOrder,trading_core::RateLimitActionKind::CancelOrder}) {
        trading_core::RateLimitActionConfig action{};action.action=kind;action.costCount=1u;
        action.costs[0]={bucket.kind,1};policy.actions.push_back(action);
      }
      request.rateLimitSchedules.push_back(std::move(policy));
    }
    return hft_backtest::runBacktest(request);
  }
};

const cxet::testing::Case cases[]{
    cxet::testing::Case{"backtest_api_guard_rejects_old_result_layout",+[] {
      CXET_CHECK(hft_backtest::kBacktestApiVersion==7u);
      CXET_CHECK(hft_backtest::hft_backtest_api_version()==hft_backtest::kBacktestApiVersion);
      CXET_CHECK(hft_backtest::hft_backtest_api_version()!=6u);
    }},
    cxet::testing::Case{"universe_hydrated_quote_currency_refuses_before_accounting", +[] {
      UniverseAuditFixture fixture(true);const auto result=fixture.run();
      CXET_CHECK(result.status==hft_backtest::Status::ConfigMismatch);
      CXET_CHECK(result.error.find("quote")!=std::string::npos);
      CXET_CHECK(result.legs.empty() && result.virtualAccounts==0u && result.universeSources==0u && result.orders==0u);
    }},
    cxet::testing::Case{"result_effective_disabled_quotas_do_not_claim_strict_enforcement", +[] {
      for(const auto mode:{0u,1u,2u,3u}) {
        const bool enabled=mode==1u || mode==2u;
        UniverseAuditFixture fixture(false,mode==2u);const auto result=fixture.run(mode!=0u,mode!=3u);
        CXET_CHECK(result.status==hft_backtest::Status::Ok);
        CXET_CHECK(result.rateLimitsEnabled==enabled && result.strictRateLimitRejects==enabled);
        CXET_CHECK(result.rateLimitSchedules.size()==(mode==2u?2u:1u));
        CXET_CHECK(result.rateLimitSchedules[0].enabled==enabled);
        if(mode==2u) CXET_CHECK(!result.rateLimitSchedules[1].enabled);
        std::ifstream input(fixture.root/"result"/"manifest.json");
        const std::string manifest((std::istreambuf_iterator<char>(input)),{});
        CXET_CHECK(manifest.find("\"venues\"")!=std::string::npos);
        CXET_CHECK(manifest.find(enabled?"\"enabled\": true":"\"enabled\": false")!=std::string::npos);
        CXET_CHECK(manifest.find(enabled?"\"strict_rejects\": true":"\"strict_rejects\": false")!=std::string::npos);
        if(mode==2u) CXET_CHECK(manifest.find("\"market\": \"spot\", \"enabled\": false, \"strict_rejects\": false")!=std::string::npos);
      }
    }},

    cxet::testing::Case{"universe_refuses_mixed_or_unknown_quote_currency_before_strategy", +[] {
      std::vector<detail::SessionData> sources(2u);
      for(std::size_t i=0u;i<sources.size();++i) {
        sources[i].exchange=i==0u?"Binance":"Bybit";sources[i].market="Futures";
        sources[i].symbol=i==0u?"BTC_USDT":"ETH_USDC";sources[i].quoteAssetId=i+1u;
      }
      hft_backtest::BacktestStrategyConfig config;
      std::vector<hft_backtest::run_detail::UniverseContextSelection> contexts;std::string error;
      CXET_CHECK(!hft_backtest::run_detail::discoverUniverseContexts(trading_core::kSpreadMaker1And2StrategyDescriptor,
          config,sources,contexts,error) && contexts.empty() && !error.empty());
      sources[1].quoteAssetId=0u;error.clear();
      CXET_CHECK(!hft_backtest::run_detail::discoverUniverseContexts(trading_core::kSpreadMaker1And2StrategyDescriptor,
          config,sources,contexts,error));
      sources[1].quoteAssetId=sources[0].quoteAssetId;error.clear();
      CXET_CHECK(hft_backtest::run_detail::discoverUniverseContexts(trading_core::kSpreadMaker1And2StrategyDescriptor,
          config,sources,contexts,error) && contexts.size()==2u);
    }},
    cxet::testing::Case{"native_channel_close_preserves_membership_and_fences_execution", +[] {
      namespace corpus=hftrec::corpus;
      auto engine=std::make_unique<detail::EngineState>();
      const auto bbo=corpus::binaryMarketChannelBit(corpus::BinaryMarketChannel::BookTicker);
      engine->capturedSourceMember=true;engine->capturedSourceGeneration=11u;
      engine->capturedExecutionRequiredChannels=engine->capturedActiveChannels=bbo;
      engine->snapshot.hasBookTicker=trading_core::kAlgoFlagTrue;
      engine->pending.push_back({});engine->pending[0].sourceGeneration=11u;
      engine->open[0].state.valid=true;engine->netQtyRaw=7;
      detail::SourceLifecycleRow row;
      row.arrival.sourceId=row.payload.source.sourceId=1u;
      row.arrival.sourceGeneration=row.payload.source.initialSourceGeneration=row.payload.previousSourceGeneration=11u;
      row.payload.channelMaskBefore=row.payload.channelMaskAfter=bbo;row.payload.directoryRevision=1u;
      row.payload.kind=corpus::BinaryMarketSourceLifecycleKind::DirectoryChanged;
      row.payload.affectedChannel=1u;row.payload.cause=corpus::BinaryMarketSourceTransitionCause::NativeClosed;
      std::string error;
      CXET_CHECK(hft_backtest::run_detail::applyCapturedSourceLifecycle(*engine,row,200,error));
      CXET_CHECK(engine->capturedSourceMember && engine->capturedActiveChannels==bbo && engine->capturedUnhealthyChannels==bbo);
      detail::applyDueActions(*engine,250);
      CXET_CHECK(engine->pending.size()==1u && engine->open[0].state.valid && engine->netQtyRaw==7);
      row.payload.directoryRevision=2u;row.payload.cause=corpus::BinaryMarketSourceTransitionCause::NativeRecovered;
      CXET_CHECK(hft_backtest::run_detail::applyCapturedSourceLifecycle(*engine,row,300,error));
      CXET_CHECK(engine->capturedUnhealthyChannels==0u && engine->snapshot.hasBookTicker.raw==0u && engine->replayNotBeforeNs==300);
    }},
    cxet::testing::Case{"native_depth_gap_without_replay_compatibility_is_exact_health_state", +[] {
      namespace corpus = hftrec::corpus;
      BinaryTradeFixture fixture(0u, 0u, 0u, 1u, true, false, false, false, true);
      detail::SessionLoadPlan plan{}; plan.loadTrades = plan.loadBookTickers = false;
      plan.loadDepths = plan.requireDepths = true;
      detail::BinaryCorpusStream stream; std::string error;
      const std::array requests{fixture.request()};
      CXET_CHECK(stream.open(fixture.root, requests, plan, error) == hft_backtest::Status::Ok);
      std::vector<detail::TimelineEvent> events; bool available = false;
      CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Ok && available && events.size() == 2u);
      CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Ok && available && events.size() == 1u);
      CXET_CHECK(events[0].kind == detail::kTimelineKindHealth && events[0].applicationOnly && stream.sessions()[0].depths.empty());
      const auto& row = stream.sessions()[0].sourceHealth[0];
      const auto& payload = *corpus::binaryMarketPayload<corpus::BinaryMarketDepthChunkPayload>(&row.record);
      CXET_CHECK(row.unhealthy && payload.state == corpus::BinaryMarketDepthState::Gap && payload.failure == 7u);
      CXET_CHECK((row.record.header.flags & corpus::BinaryMarketRecordTraderReplayCompatible) == 0u);
      CXET_CHECK(row.record.header.receiveMonotonicNs == 250u && row.record.header.sourceGeneration == 11u);
      auto interval = fixture.request(); interval.beginReceiveNs = 225;
      const std::array intervalRequests{interval};
      CXET_CHECK(stream.open(fixture.root, intervalRequests, plan, error) == hft_backtest::Status::Ok);
      CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Ok && available && events.size() == 1u);
      CXET_CHECK(events[0].kind == detail::kTimelineKindHealth && stream.sessions()[0].depths.empty());
    }},
    cxet::testing::Case{"observed_health_pauses_required_lane_and_keeps_existing_execution_state", +[] {
      namespace corpus = hftrec::corpus;
      auto engine = std::make_unique<detail::EngineState>();
      auto unaffected = std::make_unique<detail::EngineState>();
      engine->capturedSourceManaged = true; engine->capturedSourceGeneration = 11u;
      const auto trade = corpus::binaryMarketChannelBit(corpus::BinaryMarketChannel::Trade);
      engine->capturedExecutionRequiredChannels = engine->capturedActiveChannels = trade;
      engine->snapshot.hasTrade = trading_core::kAlgoFlagTrue; engine->snapshot.lastTrade.ts.raw = 100u;
      engine->pending.push_back({}); engine->pending[0].sourceGeneration = 11u;
      engine->open[0].state.valid = true; engine->open[0].sourceGeneration = 11u;
      engine->netQtyRaw = 7;
      unaffected->snapshot.hasTrade = trading_core::kAlgoFlagTrue;
      detail::SourceHealthRow row;
      row.record.header.channel = corpus::BinaryMarketChannel::Trade;
      row.record.header.sourceGeneration = row.arrival.sourceGeneration = 11u;
      row.record.header.sourceFlags = corpus::BinaryMarketSourceSequenceGap;
      row.unhealthy = true;
      std::string error;
      CXET_CHECK(hft_backtest::run_detail::applyCapturedSourceHealth(*engine, row, 200, error));
      CXET_CHECK(engine->capturedUnhealthyChannels == trade && engine->snapshot.hasTrade.raw == 0u);
      CXET_CHECK(engine->capturedDirectoryRevision == 0u && engine->executionBoundaryEpoch == 1u);
      CXET_CHECK(hft_backtest::strategy_runtime_detail::nextPendingTs(*engine) == std::numeric_limits<std::int64_t>::max());
      detail::applyDueActions(*engine, 200);
      CXET_CHECK(engine->pending.size() == 1u && engine->open[0].state.valid && engine->netQtyRaw == 7);
      row.unhealthy = false; row.record.header.sourceFlags = 0u;
      CXET_CHECK(hft_backtest::run_detail::applyCapturedSourceHealth(*engine, row, 300, error));
      CXET_CHECK(engine->capturedUnhealthyChannels == 0u && engine->snapshot.hasTrade.raw == 0u);
      CXET_CHECK(engine->executionBoundaryEpoch == 2u && engine->replayNotBeforeNs == 300);
      CXET_CHECK(engine->pending.size() == 1u && engine->open[0].state.valid && engine->netQtyRaw == 7);
      CXET_CHECK(unaffected->snapshot.hasTrade.raw == 1u && unaffected->executionBoundaryEpoch == 0u);
    }},
    cxet::testing::Case{"binary_observed_health_is_typed_pause_and_recovery_uses_real_payload", +[] {
      BinaryTradeFixture fixture(2u, 199u, 7u, 1u, false, false, true);
      detail::BinaryCorpusStream stream; std::string error;
      const std::array requests{fixture.request()};
      CXET_CHECK(stream.open(fixture.root, requests, fixture.plan(), error) == hft_backtest::Status::Ok);
      std::vector<detail::TimelineEvent> events; bool available = false;
      CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Ok && available && events.size() == 1u);
      CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Ok && available && events.size() == 1u);
      CXET_CHECK(events[0].kind == detail::kTimelineKindHealth && events[0].applicationOnly && stream.sessions()[0].trades.empty());
      CXET_CHECK(stream.sessions()[0].sourceHealth[0].record.header.sourceFlags == hftrec::corpus::BinaryMarketSourceStale);
      CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Ok && available && events.size() == 2u);
      CXET_CHECK(events[0].kind == detail::kTimelineKindHealth && !stream.sessions()[0].sourceHealth[0].unhealthy);
      CXET_CHECK(events[1].kind == detail::kTimelineKindTrade && stream.sessions()[0].trades[0].priceE8 == 1);
      auto interval = fixture.request(); interval.beginReceiveNs = 275;
      const std::array intervalRequests{interval};
      CXET_CHECK(stream.open(fixture.root, intervalRequests, fixture.plan(), error) == hft_backtest::Status::Ok);
      CXET_CHECK(stream.sessions()[0].capturedUnhealthyChannels == hftrec::corpus::binaryMarketChannelBit(hftrec::corpus::BinaryMarketChannel::Trade));
    }},
    cxet::testing::Case{"missing_capture_gap_boundary_still_refuses_complete_replay_interval", +[] {
      BinaryTradeFixture fixture(2u, 199u, 7u, 1u, false, false, false, true);
      detail::BinaryCorpusStream stream; std::string error;
      const std::array requests{fixture.request()};
      CXET_CHECK(stream.open(fixture.root, requests, fixture.plan(), error) == hft_backtest::Status::CorruptData);
    }},
    cxet::testing::Case{"binary_stream_replays_real_membership_without_synthetic_market_frames", +[] {
      BinaryTradeFixture fixture(2u, 199u, 7u, 1u, false, true);
      detail::BinaryCorpusStream stream; std::string error;
      const std::array requests{fixture.request()};
      CXET_CHECK(stream.open(fixture.root, requests, fixture.plan(), error) == hft_backtest::Status::Ok);
      std::vector<detail::TimelineEvent> events; bool available = false;
      for (std::size_t index = 0u; index < 4u; ++index) {
        CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Ok && available && events.size() == 1u);
        const auto& event = events[0];
        if (index == 1u || index == 2u) {
          CXET_CHECK(event.kind == detail::kTimelineKindLifecycle && event.applicationOnly);
          const auto& row = stream.sessions()[0].sourceLifecycle[0];
          CXET_CHECK(row.arrival.frameSequence == 0u && row.arrival.sourceGeneration == 11u);
          CXET_CHECK(row.arrival.sessionEpoch == (index == 1u ? 17u : 18u));
        } else CXET_CHECK(event.kind == detail::kTimelineKindTrade && !event.applicationOnly);
        CXET_CHECK(event.deliveryTsNs == static_cast<std::int64_t>(200u + index * 50u));
      }
      CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Ok && !available);
      auto interval = fixture.request(); interval.beginReceiveNs = 275;
      const std::array intervalRequests{interval};
      CXET_CHECK(stream.open(fixture.root, intervalRequests, fixture.plan(), error) == hft_backtest::Status::Ok);
      CXET_CHECK(!stream.sessions()[0].capturedInitiallyPresent &&
          stream.sessions()[0].capturedDirectoryRevision == 2u);
      CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Ok && available && events.size() == 1u);
      CXET_CHECK(events[0].kind == detail::kTimelineKindLifecycle && events[0].deliveryTsNs == 300);
    }},
    cxet::testing::Case{"administrative_lifecycle_arrival_is_not_a_market_application_frame", +[] {
      hftrec::replay::EventArrival arrival;
      arrival.receiveRealtimeNs = 100; arrival.receiveMonotonicNs = 10;
      arrival.producerEpoch = arrival.sourceGeneration = arrival.shardSequence = 1u;
      arrival.sourceId = 1u;
      arrival.flags = hftrec::replay::EventArrivalSourceLifecycle;
      CXET_CHECK(hftrec::replay::hasCapturedSourceLifecycleArrival(arrival));
      CXET_CHECK(!hftrec::replay::hasCapturedApplicationArrival(arrival));
      arrival.flags |= hftrec::replay::EventArrivalApplicationFrame;
      arrival.frameSequence = arrival.sessionEpoch = 1u;
      CXET_CHECK(!hftrec::replay::hasCapturedSourceLifecycleArrival(arrival));
      CXET_CHECK(!hftrec::replay::hasCapturedApplicationArrival(arrival));
    }},
    cxet::testing::Case{"same_generation_partial_bbo_readd_preserves_trade_and_fences_old_actions", +[] {
      namespace corpus = hftrec::corpus;
      auto engine = std::make_unique<detail::EngineState>();
      engine->capturedSourceManaged = true;
      engine->capturedSourceGeneration = 11u;
      const auto bbo = corpus::binaryMarketChannelBit(corpus::BinaryMarketChannel::BookTicker);
      const auto trade = corpus::binaryMarketChannelBit(corpus::BinaryMarketChannel::Trade);
      engine->capturedActiveChannels = bbo | trade;
      engine->capturedExecutionRequiredChannels = bbo;
      engine->snapshot.hasTrade = trading_core::kAlgoFlagTrue;
      engine->snapshot.lastTrade.ts.raw = 123u;
      engine->snapshot.hasBookTicker = trading_core::kAlgoFlagTrue;
      detail::PendingAction old; old.activeTsNs = 150; old.sourceGeneration = 11u;
      engine->pending.push_back(old);
      engine->open[0].state.valid = true; engine->open[0].sourceGeneration = 11u;
      engine->netQtyRaw = 7;
      detail::SourceLifecycleRow row;
      row.arrival.sourceId = row.payload.source.sourceId = 1u;
      row.arrival.sourceGeneration = row.payload.source.initialSourceGeneration = 11u;
      row.payload.previousSourceGeneration = 11u;
      row.payload.directoryRevision = 1u;
      row.payload.channelMaskBefore = bbo | trade; row.payload.channelMaskAfter = trade;
      row.payload.affectedChannel = static_cast<std::uint8_t>(corpus::BinaryMarketChannel::BookTicker);
      row.payload.kind = corpus::BinaryMarketSourceLifecycleKind::Removed;
      std::string error;
      CXET_CHECK(hft_backtest::run_detail::applyCapturedSourceLifecycle(*engine, row, 200, error));
      CXET_CHECK(engine->snapshot.hasTrade.raw == 1u && engine->snapshot.lastTrade.ts.raw == 123u);
      CXET_CHECK(engine->snapshot.hasBookTicker.raw == 0u && engine->capturedSourceMember);
      row.payload.directoryRevision = 2u;
      row.payload.channelMaskBefore = trade; row.payload.channelMaskAfter = bbo | trade;
      row.payload.kind = corpus::BinaryMarketSourceLifecycleKind::Readded;
      row.arrival.sessionEpoch = 19u;
      CXET_CHECK(hft_backtest::run_detail::applyCapturedSourceLifecycle(*engine, row, 300, error));
      CXET_CHECK(engine->capturedSourceGeneration == 11u && engine->snapshot.lastTrade.ts.raw == 123u);
      CXET_CHECK(hft_backtest::strategy_runtime_detail::nextPendingTs(*engine) == std::numeric_limits<std::int64_t>::max());
      detail::applyDueActions(*engine, 400);
      CXET_CHECK(engine->pending.size() == 1u && engine->open[0].state.valid && engine->netQtyRaw == 7);
      detail::PendingAction current; current.activeTsNs = 410; current.sourceGeneration = 11u;
      current.executionBoundaryEpoch = engine->executionBoundaryEpoch;
      engine->pending.push_back(current);
      CXET_CHECK(hft_backtest::strategy_runtime_detail::nextPendingTs(*engine) == 410);
    }},
    cxet::testing::Case{"venue_wallet_configuration_seeds_account_once_and_explicit_override_wins", +[] {
      BinaryTradeFixture fixture(2u, 199u, 7u);
      const auto configPath = fixture.root / "strategy.ini";
      {
        std::ofstream config(configPath);
        config << "[strategy]\ntype=spread_maker1and2\n"
            "[venue.binance_spot]\nsymbols=BTC_USDT\nrate_limits_enabled=false\ninitial_balance_usdt=12.5\n";
      }
      hft_backtest::BacktestStrategyConfig config; std::string error;
      CXET_CHECK(hft_backtest::loadBacktestStrategyConfig(configPath.c_str(), config, error));
      CXET_CHECK(config.venues.size() == 1u && config.venues[0].hasInitialBalance);
      hft_backtest::BacktestRunRequest request; request.initialBalanceE8 = 999;
      CXET_CHECK(hft_backtest::run_detail::virtualAccountInitialBalance(request, config.venues[0], 0u) == 1'250'000'000);
      request.legInitialBalancesE8 = {5};
      CXET_CHECK(hft_backtest::run_detail::virtualAccountInitialBalance(request, config.venues[0], 0u) == 5);
    }},
    cxet::testing::Case{"source_removal_preserves_orders_and_fences_old_generation_pending", +[] {
      auto engine = std::make_unique<detail::EngineState>();
      engine->capturedSourceManaged = true;
      engine->capturedSourceGeneration = 1u;
      detail::PendingAction old;
      old.activeTsNs = 100; old.sourceGeneration = 1u;
      engine->pending.push_back(old);
      engine->open[0].state.valid = true;
      engine->open[0].sourceGeneration = 1u;
      engine->netQtyRaw = 7;
      engine->capturedSourceMember = false;
      CXET_CHECK(hft_backtest::strategy_runtime_detail::nextPendingTs(*engine) == std::numeric_limits<std::int64_t>::max());
      detail::applyDueActions(*engine, 200);
      CXET_CHECK(engine->pending.size() == 1u && engine->open[0].state.valid && engine->netQtyRaw == 7);
      engine->capturedSourceMember = true; engine->capturedSourceGeneration = 2u;
      engine->replayNotBeforeNs = 300;
      CXET_CHECK(hft_backtest::strategy_runtime_detail::nextPendingTs(*engine) == std::numeric_limits<std::int64_t>::max());
      detail::PendingAction current; current.activeTsNs = 310; current.sourceGeneration = 2u;
      engine->pending.push_back(current);
      CXET_CHECK(hft_backtest::strategy_runtime_detail::nextPendingTs(*engine) == 310);
    }},
    cxet::testing::Case{"binary_stream_commits_full_rebase_before_exposing_one_transaction", +[] {
      BinaryTradeFixture fixture(0u, 0u, 0u, 1u, true);
      detail::SessionLoadPlan plan{};
      plan.loadTrades = plan.loadBookTickers = false;
      plan.loadDepths = plan.requireDepths = true;
      detail::BinaryCorpusStream stream; std::string error;
      const std::array requests{fixture.request()};
      CXET_CHECK(stream.open(fixture.root, requests, plan, error) == hft_backtest::Status::Ok);
      std::vector<detail::TimelineEvent> events; bool available = false;
      CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Ok && available);
      CXET_CHECK(events.size() == 2u && stream.sessions()[0].depths.size() == 2u);
      CXET_CHECK(events[0].exchangeTsNs == 199 && events[1].exchangeTsNs == 150 &&
          events[0].deliveryTsNs == events[1].deliveryTsNs &&
          events[0].shardSequence == 1u && events[1].shardSequence == 2u);
      auto engine = std::make_unique<detail::EngineState>();
      CXET_CHECK(engine->strategyDepthBook.prepare());
      CXET_CHECK(engine->orderBook.reserveLevelsPerSide(CountVal{8u}));
      hft_backtest::StrategyInstrumentSpec instrument; instrument.tickSizeRaw = 1;
      for (const auto& event : events)
        hft_backtest::applyMarketEventToSnapshots(*engine, stream.sessions()[0], event, Symbol{}, instrument, true);
      CXET_CHECK(!engine->depthReplayFailed);
      trading_core::LocalOrderBookLevel bid;
      CXET_CHECK(trading_core::orderBook(engine->orderBook).readStoredLevel(
          trading_core::BookSide::Bid, Index{0u}, bid));
      CXET_CHECK(bid.px.raw == 100 && bid.qty.raw == 3);
    }},
    cxet::testing::Case{"overlapping_context_local_ids_do_not_adopt_orders_or_stale_actions", +[] {
      auto engine = std::make_unique<detail::EngineState>();
      trading_core::OrderIntent intent{};
      intent.clientIntentId.raw = 7u; intent.orderType = canon::OrderType::Limit;
      intent.side = Side::Buy(); intent.qty.raw = 4; intent.price.raw = 100;
      engine->activeContextIndex = 1u;
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*engine, intent, 10, 20));
      engine->activeContextIndex = 2u;
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*engine, intent, 10, 20));
      detail::applyDueActions(*engine, 20);
      std::array<trading_core::StrategyOrderState, 2> output;
      CXET_CHECK(detail::visibleOrders(*engine, output.data(), output.size()) == 1u);
      CXET_CHECK(detail::execution_detail::findOpenIndex(*engine, Id{7u}) == 1u);
      detail::PendingAction stale;
      stale.contextIndex = 1u; stale.targetClientIntentId.raw = 7u;
      CXET_CHECK(!detail::execution_detail::matchesExpectedOrder(engine->open[1], stale));
      hft_backtest::BacktestRunResult result;
      hft_backtest::populateResultPnl(result, *engine);
      CXET_CHECK(!result.strategyClosed && result.openPositionQtyE8 == 0);
    }},
    cxet::testing::Case{"universe_single_source_context_bound_does_not_cap_catalog_at_fifty", +[] {
      std::vector<detail::SessionData> sources(60u);
      for (std::size_t i = 0; i < sources.size(); ++i) {
        sources[i].exchange = "Binance"; sources[i].market = "Futures";
        sources[i].symbol = "S" + std::to_string(i);
        sources[i].quoteAssetId=1u;
      }
      hft_backtest::BacktestStrategyConfig config;
      std::vector<hft_backtest::run_detail::UniverseContextSelection> contexts;
      std::string error;
      CXET_CHECK(hft_backtest::run_detail::discoverUniverseContexts(
          trading_core::kSpreadMaker1And2StrategyDescriptor, config, sources, contexts, error));
      CXET_CHECK(contexts.size() == 60u);
      for (std::size_t i = 0; i < contexts.size(); ++i)
        CXET_CHECK(contexts[i].sourceIndices.size() == 1u && contexts[i].sourceIndices[0] == i);
    }},
    cxet::testing::Case{"whole_market_materializes_new_symbols_from_proven_seed_quote", +[] {
      using Source=hftrec::corpus::BinaryMarketSource;
      const auto row=[](const char* symbol,std::uint32_t quote) {
        Source source{};source.venueId=1u;source.marketRaw=1u;source.quoteAssetId=quote;
        std::string text=symbol;source.canonicalSymbolBytes=text.size();
        std::copy(text.begin(),text.end(),source.canonicalSymbol.begin());return source;
      };
      const std::array catalog{row("AAA_USDT",2u),row("NEW_USDT",2u),row("OTHER_USDC",3u)};
      hft_backtest::BacktestStrategyConfig config;
      config.strategyType="SpreadMaker1And2";
      hft_backtest::BacktestStrategyVenueConfig venue;
      venue.name="Binance_Futures";venue.exchange.raw=1u;venue.market.raw=1u;
      venue.initialBalanceE8=123;venue.hasInitialBalance=true;
      Symbol seed{};CXET_CHECK(seed.copyFrom("AAA_USDT"));venue.symbols.push_back(seed);
      config.venues.push_back(venue);std::string error;
      CXET_CHECK(hft_backtest::run_detail::materializeUniverseSymbolsFromCorpus(config,catalog,error));
      CXET_CHECK(config.venues[0].symbols.size()==2u);
      CXET_CHECK(std::string_view(config.venues[0].symbols[1].data)=="NEW_USDT");
      CXET_CHECK(config.venues[0].initialBalanceE8==123 && config.strategyType=="SpreadMaker1And2");
    }},
    cxet::testing::Case{"whole_market_ambiguous_quote_refuses_without_partial_config_change", +[] {
      std::array<hftrec::corpus::BinaryMarketSource,2u> catalog{};
      for (std::size_t i=0;i<catalog.size();++i) {
        catalog[i].venueId=1u;catalog[i].marketRaw=1u;catalog[i].quoteAssetId=i+2u;
        const std::string symbol=i==0u?"AAA_USDT":"BBB_USDC";
        catalog[i].canonicalSymbolBytes=symbol.size();
        std::copy(symbol.begin(),symbol.end(),catalog[i].canonicalSymbol.begin());
      }
      hft_backtest::BacktestStrategyConfig config;hft_backtest::BacktestStrategyVenueConfig venue;
      venue.name="Binance_Futures";venue.exchange.raw=1u;venue.market.raw=1u;
      Symbol seed{};CXET_CHECK(seed.copyFrom("MISSING_USDT"));venue.symbols.push_back(seed);config.venues.push_back(venue);
      std::string error;
      CXET_CHECK(!hft_backtest::run_detail::materializeUniverseSymbolsFromCorpus(config,catalog,error));
      CXET_CHECK(config.venues[0].symbols.size()==1u && std::string_view(config.venues[0].symbols[0].data)=="MISSING_USDT");
      config.venues[0].symbols.clear();
      for (const auto& row:catalog) {Symbol symbol{};CXET_CHECK(symbol.copyFrom(row.canonicalSymbol.data()));config.venues[0].symbols.push_back(symbol);}
      CXET_CHECK(!hft_backtest::run_detail::materializeUniverseSymbolsFromCorpus(config,catalog,error));
      config.venues[0].symbols.resize(1u);
      catalog[1].quoteAssetId=catalog[0].quoteAssetId;
      catalog[1].canonicalSymbol[3]='\0';
      CXET_CHECK(!hft_backtest::run_detail::materializeUniverseSymbolsFromCorpus(config,catalog,error));
      CXET_CHECK(config.venues[0].symbols.size()==1u && std::string_view(config.venues[0].symbols[0].data)=="AAA_USDT");
      catalog[1].canonicalSymbol.fill('\0');
      const std::string malformed="BROKEN";
      catalog[1].canonicalSymbolBytes=malformed.size();
      std::copy(malformed.begin(),malformed.end(),catalog[1].canonicalSymbol.begin());
      CXET_CHECK(!hft_backtest::run_detail::materializeUniverseSymbolsFromCorpus(config,catalog,error));
      CXET_CHECK(config.venues[0].symbols.size()==1u && std::string_view(config.venues[0].symbols[0].data)=="AAA_USDT");
    }},
    cxet::testing::Case{"universe_contexts_borrow_same_source_and_share_wallet", +[] {
      auto first = std::make_unique<detail::EngineState>();
      auto second = std::make_unique<detail::EngineState>();
      detail::SharedAccountState account{};
      account.initialBalanceE8 = account.walletBalanceE8 = 1000;
      account.observedWalletBalanceE8=1000;account.observedInitialized=true;
      first->sharedAccount = second->sharedAccount = &account;
      first->initialBalanceE8 = first->walletBalanceE8 = 1000;
      std::array<detail::EngineState*, 2> routes{second.get(), first.get()};
      detail::EngineView view(routes.data(), routes.size());
      view[0].netQtyRaw = 7;
      CXET_CHECK(second->netQtyRaw == 7 && &view[1] == first.get());
      const auto filled = detail::updatePosition(*first, Side::Buy(), 100, 100'000'000, 1, 100'000'000);
      CXET_CHECK(filled.applied && account.walletBalanceE8 == first->walletBalanceE8);
      const auto sameAccount = hft_backtest::accountStateFromEngine(*second);
      CXET_CHECK(sameAccount.initialBalanceE8 == 1000 && sameAccount.walletBalanceE8 == 1000);
    }},
    cxet::testing::Case{"binary_stream_long_interval_does_not_accumulate_rows_or_sort_exchange_time", +[] {
      BinaryTradeFixture fixture(2u, 199u, 7u, 16u);
      detail::BinaryCorpusStream stream;
      std::string error;
      const std::array requests{fixture.request()};
      CXET_CHECK(stream.open(fixture.root, requests, fixture.plan(), error) == hft_backtest::Status::Ok);
      std::vector<detail::TimelineEvent> events;
      bool available = false;
      for (std::uint32_t row = 0u; row < 16u; ++row) {
        CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Ok && available);
        CXET_CHECK(events.size() == 1u && events[0].exchangeTsNs == 199 - row &&
            events[0].receiveMonotonicNs == 200u + row &&
            events[0].tsNs == events[0].deliveryTsNs);
        CXET_CHECK(stream.sessions()[0].trades.size() == 1u);
      }
      CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Ok && !available);
    }},
    cxet::testing::Case{"simulated_event_identity_survives_terminal_slot_and_native_text", +[] {
      auto engine = std::make_unique<detail::EngineState>();
      trading_core::OrderIntent order;
      order.clientIntentId.raw = 1'000'000'000'007u;
      order.orderType=canon::OrderType::Limit;order.side=Side::Buy();order.qty.raw=1;order.price.raw=100;
      CXET_CHECK(detail::prepareSimulationOrderEvidence(*engine));
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*engine,order,10,20));
      detail::applyDueActions(*engine,20);
      CXET_CHECK(engine->userEvents.size()==1u);
      CXET_CHECK(detail::deliverSimulationOrderEvent(*engine,0u,engine->userEventDeliveryTsNs[0]));
      trading_core::CancelIntent cancel;cancel.clientIntentId.raw=2u;cancel.targetClientIntentId=order.clientIntentId;
      CXET_CHECK(detail::execution_detail::submitCancelIntent(*engine,cancel,30,40));
      detail::applyDueActions(*engine,40);
      CXET_CHECK(engine->userEvents.size()==2u);
      CXET_CHECK(detail::deliverSimulationOrderEvent(*engine,1u,engine->userEventDeliveryTsNs[1]));
      const auto event=engine->userEvents[1];
      trading_core::StrategyContext context{};
      context.userEvent = &engine->userEvents[1];
      context.hasUserEvent = true;
      context.userEventAdmittedSemanticMask = cxet::composite::userDataSemanticBit(
          cxet::composite::UserDataSemantic::OrderTransition);
      hft_backtest::bindBacktestUserEventIdentity(context, *engine);
      trading_core::strategy::RuntimeClientOrderId admitted{};
      CXET_CHECK(trading_core::strategy::admittedStrategyEventIdentity(context, admitted));
      CXET_CHECK(admitted.clientIntentId.raw == order.clientIntentId.raw && admitted.generation == 1u);
      auto foreign = event;
      foreign.eventId.raw += 1u;
      context.userEvent = &foreign;
      hft_backtest::bindBacktestUserEventIdentity(context, *engine);
      CXET_CHECK(!trading_core::strategy::admittedStrategyEventIdentity(context, admitted));
    }},
    cxet::testing::Case{"binary_stream_retains_only_current_transaction_and_exchange_time", +[] {
      BinaryTradeFixture fixture(2u, 199u, 7u);
      detail::BinaryCorpusStream stream;
      std::string error;
      const std::array requests{fixture.request()};
      CXET_CHECK(stream.open(fixture.root, requests, fixture.plan(), error) == hft_backtest::Status::Ok);
      CXET_CHECK(stream.sessions().size() == 1u && stream.sessions()[0].trades.empty());
      std::vector<detail::TimelineEvent> events;
      bool available = false;
      CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Ok && available);
      CXET_CHECK(events.size() == 1u && events[0].exchangeTsNs == 199 &&
          events[0].tsNs == events[0].deliveryTsNs && events[0].receiveMonotonicNs == 200u);
      const auto& rows = stream.sessions()[0].trades;
      CXET_CHECK(rows.size() == 1u && rows[0].tsNs == 199 && rows[0].nativeIdentity.nativeSecond == 7u);
      CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Ok && !available);
      CXET_CHECK(events.empty() && stream.sessions()[0].trades.empty());
    }},
    cxet::testing::Case{"replay_refuses_canonical_order_evidence_state", +[] {
      const auto& descriptor = trading_core::kDeliverySumSignalStrategyDescriptor;
      CXET_CHECK(trading_core::strategyDescriptorContractValid(descriptor));
      CXET_CHECK(!hft_backtest::strategy_intent_lowering::footprintSupported(descriptor));
    }},
    cxet::testing::Case{"replay_depth_refresh_does_not_reapply_older_reconstructed_bbo", +[] {
      for (const bool strategyVisible : {false, true}) {
        for (const std::int64_t tick : {0ll, 1ll}) {
          auto engine = std::make_unique<detail::EngineState>();
          auto& book = strategyVisible ? engine->orderBook : engine->venueOrderBook;
          auto& depthBook = strategyVisible ? engine->strategyDepthBook : engine->venueDepthBook;
          auto& snapshot = strategyVisible ? engine->snapshot : engine->venueSnapshot;
          auto& bbo = strategyVisible ? engine->bboReconstruction : engine->venueBboReconstruction;
          CXET_CHECK(trading_core::configureBboReconstruction(bbo, Price{1}));
          CXET_CHECK(book.reserveLevelsPerSide(CountVal{8u}));
          CXET_CHECK(depthBook.prepare());
          detail::SessionData session{};
          detail::BookTickerRow ticker{};
          ticker.bidPriceE8 = 100; ticker.bidQtyE8 = 2;
          ticker.askPriceE8 = 101; ticker.askQtyE8 = 3; ticker.tsNs = 100;
          session.bookTickers.push_back(ticker);
          detail::DepthRow depth{};
          depth.tsNs = 200; depth.resetBookBeforeApply = true;
          depth.levels = {{99, 4, 0}, {102, 5, 1}};
          depth.arrival.receiveMonotonicNs = 200u;
          session.depths.push_back(depth);
          hft_backtest::StrategyInstrumentSpec instrument{};
          instrument.tickSizeRaw = tick;
          detail::TimelineEvent event{};
          event.kind = detail::kTimelineKindBookTicker;
          event.tsNs = event.deliveryTsNs = 100;
          hft_backtest::applyMarketEventToSnapshots(
              *engine, session, event, Symbol{}, instrument, strategyVisible);
          CXET_CHECK(snapshot.hasReconstructedBbo.raw == trading_core::kAlgoFlagTrue.raw);
          CXET_CHECK(snapshot.reconstructedBookTicker.bid.px.raw == 100 &&
              snapshot.reconstructedBookTicker.ask.px.raw == 101);
          event.kind = detail::kTimelineKindDepth;
          event.tsNs = event.deliveryTsNs = 200;
          hft_backtest::applyMarketEventToSnapshots(
              *engine, session, event, Symbol{}, instrument, strategyVisible);
          CXET_CHECK(!engine->depthReplayFailed && book.ts.raw == 200u);
          CXET_CHECK(book.bids.storedCount.raw == 1u && book.asks.storedCount.raw == 1u);
          trading_core::LocalOrderBookLevel bid{}, ask{};
          const auto view = trading_core::orderBook(book);
          CXET_CHECK(view.readStoredLevel(trading_core::BookSide::Bid, Index{0u}, bid));
          CXET_CHECK(view.readStoredLevel(trading_core::BookSide::Ask, Index{0u}, ask));
          CXET_CHECK(bid.px.raw == 99 && bid.qty.raw == 4 && ask.px.raw == 102 && ask.qty.raw == 5);
          for (const auto* side : {&book.bids, &book.asks}) {
            CXET_CHECK(side->origins[0].sourceId == 0u &&
                side->origins[0].exchangeTimestampNs == 0u &&
                side->origins[0].freshnessMonotonicNs == 0u);
          }
          if (tick > 0) {
            CXET_CHECK(book.config.tickSizeRaw == tick &&
                book.config.tickGrid.step.divisor == static_cast<std::uint64_t>(tick));
          } else {
            CXET_CHECK(book.config.tickSizeRaw == 0 && book.config.tickGrid.step.divisor == 0u);
            CXET_CHECK(book.bids.format == trading_core::LocalOrderBookStorageFormat::Sparse &&
                book.asks.format == trading_core::LocalOrderBookStorageFormat::Sparse);
          }
        }
      }
    }},
    cxet::testing::Case{"trade_replay_captured_zero_tail_stays_known_and_malformed_key_refuses", +[] {
      namespace m = cxet::api::market;
      // The actual reader admits shape2 structurally; replay admission must
      // reject its malformed first limb before any TradeRow is installed.
      for (const auto shape : {std::uint8_t{2u}, std::uint8_t{4u}}) {
        const auto second = shape == 2u ? 7u : 0u;
        BinaryTradeFixture fixture(shape, 0u, second);
        hftrec::corpus::BinaryMarketSelectionRequest selected{};
        selected.root = fixture.root; selected.exchange = "fixture";
        selected.market = "futures"; selected.canonicalSymbol = "BTCUSDT";
        selected.beginReceiveNs = 100; selected.endReceiveNs = 10'000;
        selected.channelMask = selected.requiredChannelMask =
            hftrec::corpus::binaryMarketChannelBit(hftrec::corpus::BinaryMarketChannel::Trade);
        hftrec::corpus::BinaryMarketSelection records;
        std::string error;
        CXET_CHECK(hftrec::corpus::BinaryMarketCorpusReader{}.select(selected, records, error) == hftrec::Status::Ok);
        CXET_CHECK(records.records.size() == 1u && records.records[0].header.nativeIdentityFirst == 0u);
        detail::SessionData loaded;
        const auto status = detail::loadBinaryMarketCorpus(fixture.root, fixture.request(), fixture.plan(), loaded, error);
        if (shape == 2u) {
          CXET_CHECK(status == hft_backtest::Status::CorruptData && loaded.trades.empty());
          CXET_CHECK(error == "invalid binary corpus native trade identity");
        } else {
          CXET_CHECK(status == hft_backtest::Status::Ok && loaded.trades.size() == 1u);
          cxet::composite::Trade converted{};
          CXET_CHECK(detail::toRuntime(loaded.trades[0], converted) && converted.eventId.raw == 0u);
        }
      }
      {
        // Hyperliquid's exact pair projects the second limb (tid).
        BinaryTradeFixture fixture(2u, 199u, 7u);
        detail::SessionData loaded; std::string error;
        CXET_CHECK(detail::loadBinaryMarketCorpus(fixture.root, fixture.request(), fixture.plan(), loaded, error) == hft_backtest::Status::Ok);
        CXET_CHECK(loaded.trades.size() == 1u);
        cxet::composite::Trade converted{};
        CXET_CHECK(detail::toRuntime(loaded.trades[0], converted) && converted.eventId.raw == 7u);
      }
      detail::TradeRow row{}; row.priceE8 = 1; row.qtyE8 = 2; row.tsNs = 100;
      row.nativeIdentity = {.origin=m::PublicMarketIdentityOrigin::NativeTail64,
          .nativeShape=m::PublicMarketNativeIdentityShape::UuidTail64};
      cxet::composite::Trade trade{};
      CXET_CHECK(detail::toRuntime(row, trade));
      trading_core::StrategyMarketSnapshot snapshot{};
      CXET_CHECK(trading_core::copyTradeSnapshot(snapshot, trade, row.nativeIdentity));
      CXET_CHECK(snapshot.lastTradeIdentityShape == m::PublicMarketNativeIdentityShape::UuidTail64 &&
          trade.eventId.raw == 0);
      row.nativeIdentity.nativeSecond = 1; row.priceE8 = 9;
      CXET_CHECK(!detail::toRuntime(row, trade) && trade.price().raw == 1);
    }},
    cxet::testing::Case{"trade_replay_negative_json_refused_without_row_mutation", +[] {
      detail::TradeRow row{};
      row.priceE8 = 11; row.qtyE8 = 12; row.tsNs = 13;
      for (const auto input : {negativePrice, negativeQuantity}) {
        hftrec::replay::TradeRow corpus{};
        CXET_CHECK(hftrec::isOk(hftrec::replay::parseTradeLine(input, corpus)));
        CXET_CHECK(!detail::session_loader::parseContractTradeLine(input, row));
        CXET_CHECK(row.priceE8 == 11 && row.qtyE8 == 12 && row.tsNs == 13);
      }
    }},
    cxet::testing::Case{"trade_replay_zero_json_preserves_existing_admission", +[] {
      detail::TradeRow row{};
      CXET_CHECK(detail::session_loader::parseContractTradeLine(zeroMagnitudes, row));
      cxet::composite::Trade trade{};
      CXET_CHECK(detail::toRuntime(row, trade));
      CXET_CHECK(trade.price().raw == 0 && trade.qty().raw == 0 &&
          trade.initiatorSide() == Initiator::Seller && trade.ts.raw == 100);
    }},
    cxet::testing::Case{"trade_replay_conversion_range_side_and_atomic_refusal", +[] {
      detail::TradeRow row{};
      row.priceE8 = std::numeric_limits<std::int64_t>::max();
      row.qtyE8 = row.priceE8; row.tsNs = 100;
      cxet::composite::Trade trade{};
      for (const auto side : {-1, 0, 1}) {
        row.side = side;
        CXET_CHECK(detail::toRuntime(row, trade));
        CXET_CHECK(trade.price().raw == row.priceE8 && trade.qty().raw == row.qtyE8);
        CXET_CHECK(trade.initiatorSide() == (side == -1 ? Initiator::Unknown :
            (side == 0 ? Initiator::Seller : Initiator::Buyer)));
      }
      row.qtyE8 = -1;
      CXET_CHECK(!detail::toRuntime(row, trade));
      CXET_CHECK(trade.qty().raw == std::numeric_limits<std::int64_t>::max() &&
          trade.initiatorSide() == Initiator::Buyer && trade.ts.raw == 100);
      row.qtyE8 = 1; row.side = 2; row.tsNs = 200;
      CXET_CHECK(!detail::toRuntime(row, trade));
      CXET_CHECK(trade.qty().raw == std::numeric_limits<std::int64_t>::max() &&
          trade.initiatorSide() == Initiator::Buyer && trade.ts.raw == 100);
    }},
};
}

int main(int argc, char** argv) { return cxet::testing::runCases(argc, argv, cases); }
