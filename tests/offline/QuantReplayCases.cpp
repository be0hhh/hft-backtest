#include "OfflineCase.hpp"
#include "Engine/BacktestEngine.hpp"
#include "Engine/BacktestEngineSetup.hpp"
#include "Execution/Internal/BacktestExecutionHelpers.hpp"
#include "Execution/BacktestOrderEvidence.hpp"
#include "Strategy/BacktestStrategyRuntime.hpp"
#include "Run/BacktestUniverse.hpp"
#include "Accounting/BacktestResultAggregation.hpp"
#include "Accounting/BacktestMetrics.hpp"
#include "Strategy/Inputs/BacktestStrategyWarmup.hpp"
#include "Strategy/Internal/BacktestStrategyRuntimeHelpers.hpp"
#include "trading_core/Strategy/Orders/Evidence/OrderDecisionCursor.hpp"
#include "Input/Binary/BinaryCorpusStream.hpp"
#include "Input/Binary/BinaryCorpusLoader.hpp"
#include "Strategy/Inputs/BacktestStrategyInstrument.hpp"
#include "Input/Binary/Internal/BinaryCorpusHelpers.hpp"
#include "Strategy/Execution/BacktestStrategyIntentLowering.hpp"
#include "hftrec/CorpusContract/BinaryMarketCorpusWriter.hpp"
#include "hftrec/CorpusContract/BinaryMarketCorpusReader.hpp"
#include "cxet/Canon/ExchangeRegistry.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <limits>
#include <string>
#include <unistd.h>

namespace {
namespace detail = hft_backtest::detail;
namespace corpus = hftrec::corpus;

struct QuantityFixtureConfig {
  corpus::BinaryMarketQuantityConversion conversion{};
  std::int64_t qtyRaw{100'000'000};
  std::uint8_t scale{8u};
  std::uint8_t marketKind{1u};
  std::uint8_t marketRaw{};
  std::string_view venue{"binance"};
  bool allChannels{};
  bool changeConversion{};
  bool wholeUniverseSources{};
  bool recordedOnlyRequiredSource{};
  bool noncompatibleRequiredRow{};
  QuantityFixtureConfig() {
    conversion.kinds.fill(corpus::BinaryMarketQuantityKind::CanonicalBase);
    conversion.canonicalBaseMultiplier = conversion.nativeBaseMultiplier = 1u;
  }
};

struct SpotFixture {
  static constexpr std::int64_t q = 100'000'000;
  detail::SharedAccountState account{};
  SpotFixture() { account.initialBalanceE8 = account.walletBalanceE8 = 100 * q; }
  std::unique_ptr<detail::EngineState> engine(std::uint32_t source, canon::MarketType market = canon::kMarketTypeSpot) {
    auto out = std::make_unique<detail::EngineState>(); out->legIndex = source; out->market = market;
    out->activeContextIndex = 1u; out->sharedAccount = &account;
    hft_backtest::BacktestRateLimitSchedule schedule; schedule.enabled = false;
    std::string quotaError;
    CXET_CHECK(hft_backtest::applyRateLimitSchedule(*out, schedule, quotaError));
    out->exchange = detail::exchangeFromText("binance"); out->symbol.copyFrom(source ? "ETH_USDT" : "BTC_USDT");
    hft_backtest::BacktestRunRequest request{}; request.initialBalanceE8 = source ? 0 : account.initialBalanceE8;
    detail::initializeAccounting(*out, request);
    top(*out, 60 * q, 60 * q);
    detail::BacktestLatencyConfig config{}; config.userData = {1u, 0u}; detail::initLatencyRuntime(out->latency, config);
    return out;
  }
  static void top(detail::EngineState& engine, std::int64_t bid, std::int64_t ask) {
    engine.venueSnapshot.hasBookTicker = engine.snapshot.hasBookTicker = trading_core::kAlgoFlagTrue;
    engine.venueSnapshot.lastBookTicker.bid.px.raw = bid; engine.venueSnapshot.lastBookTicker.ask.px.raw = ask;
    engine.venueSnapshot.lastBookTicker.bid.qty.raw = engine.venueSnapshot.lastBookTicker.ask.qty.raw = 10 * q;
    engine.snapshot.lastBookTicker = engine.venueSnapshot.lastBookTicker;
    detail::recordSpotValuationMark(engine, false);
    detail::recordSpotValuationMark(engine, true);
  }
  static trading_core::OrderIntent intent(std::uint64_t id, Side side, std::int64_t qty = q,
      canon::OrderType type = canon::OrderType::Market, std::int64_t price = 0) {
    trading_core::OrderIntent out{}; out.clientIntentId.raw = id; out.side = side; out.qty.raw = qty;
    out.orderType = type; out.price.raw = price; return out;
  }
  static void deliverAll(detail::EngineState& engine) {
    while (engine.deliveredUserEvents < engine.userEvents.size()) {
      const auto index = engine.deliveredUserEvents; engine.activeContextIndex = engine.userEventContextIndices[index];
      CXET_CHECK(detail::deliverSimulationOrderEvent(engine, index, engine.userEventDeliveryTsNs[index]));
    }
  }
};

struct QuantFixture {
  std::filesystem::path root = std::filesystem::current_path() /
      ("backtest-quant-fixture-" + std::to_string(::getpid()) + "-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::string venueName, marketName;
  explicit QuantFixture(std::int64_t priceBasis = 0, bool executionAuthority = false,
                        std::uint8_t assetDomain = 1u, QuantityFixtureConfig quantity = {}) {
    std::vector<corpus::BinaryMarketSource> sources(quantity.wholeUniverseSources ? 3u : 1u);
    auto& source = sources[0];
    source.sourceId = 1u; source.canonicalSymbolId = 7u; source.initialSourceGeneration = 11u;
    source.venueId = detail::exchangeFromText(std::string(quantity.venue)).raw;
    source.marketRaw = quantity.marketRaw ? quantity.marketRaw : quantity.marketKind == 1u ? 2u : 1u;
    source.marketKindRaw = quantity.marketKind; source.assetDomainRaw = assetDomain;
    source.quantityConversion = quantity.conversion;
    source.economicBaseAssetId = 1u; source.quoteAssetId = 2u;
    source.priceScale = 8u; source.quantityScale = quantity.scale;
    source.tickSizeRaw = 1; source.stepSizeRaw = 6;
    source.priceBasisQtyRaw = priceBasis;
    source.directoryFlags = corpus::BinaryMarketDirectoryBookTicker | corpus::BinaryMarketDirectoryTickSize;
    source.instrumentMetadataFlags = corpus::BinaryMarketInstrumentStepSize;
    if (executionAuthority) {
      source.canonicalStepSizeRaw = 2;
      source.executionQuantityStepRaw = 4;
      source.instrumentMetadataFlags |= corpus::BinaryMarketInstrumentCanonicalStepSize |
          corpus::BinaryMarketInstrumentExecutionQuantityStep;
    }
    source.configuredChannelMask = source.availableChannelMask = source.traderReplayChannelMask =
        corpus::binaryMarketChannelBit(corpus::BinaryMarketChannel::BookTicker);
    source.compatibility[corpus::binaryMarketChannelIndex(corpus::BinaryMarketChannel::BookTicker)] =
        corpus::BinaryMarketCompatibility::ExactTraderReplay;
    if (quantity.allChannels) {
      source.configuredChannelMask |= corpus::binaryMarketChannelBit(corpus::BinaryMarketChannel::Trade) |
          corpus::binaryMarketChannelBit(corpus::BinaryMarketChannel::Depth);
      source.availableChannelMask = source.traderReplayChannelMask = source.configuredChannelMask;
      source.directoryFlags |= corpus::BinaryMarketDirectoryTrade | corpus::BinaryMarketDirectoryDepth;
      source.compatibility[1] = source.compatibility[2] = corpus::BinaryMarketCompatibility::ExactTraderReplay;
    } else {
      source.quantityConversion.kinds[1] = source.quantityConversion.kinds[2] = corpus::BinaryMarketQuantityKind::Unknown;
    }
    const auto text = [](auto& destination, std::string_view value) {
      for (std::size_t i = 0u; i < value.size(); ++i) destination[i] = value[i];
    };
    text(source.venue, quantity.venue); source.venueBytes = quantity.venue.size();
    const auto marketText = source.marketRaw == 2u ? std::string_view{"spot"} :
        source.marketRaw == 6u ? std::string_view{"swap"} : source.marketRaw == 5u ? std::string_view{"inverse"} : std::string_view{"futures"};
    text(source.market, marketText); source.marketBytes = marketText.size();
    venueName = quantity.venue; marketName = marketText;
    text(source.canonicalSymbol, "BTC_USDT"); source.canonicalSymbolBytes = 8u;
    text(source.nativeSymbol, "BTCUSDT"); source.nativeSymbolBytes = 7u;
    if (quantity.wholeUniverseSources) {
      for (std::size_t i = 1u; i < sources.size(); ++i) {
        sources[i] = source; sources[i].sourceId = i + 1u; sources[i].canonicalSymbolId = 7u + i;
        sources[i].canonicalSymbol = {}; sources[i].nativeSymbol = {};
        text(sources[i].canonicalSymbol, i == 1u ? "ETH_USDT" : "LTC_USDT");
        text(sources[i].nativeSymbol, i == 1u ? "ETHUSDT" : "LTCUSDT");
      }
      if (quantity.recordedOnlyRequiredSource) {
        sources[2].traderReplayChannelMask = 0u;
        sources[2].compatibility[0] = corpus::BinaryMarketCompatibility::RecordedOnly;
      }
    }
    corpus::BinaryMarketWriterConfig config{};
    config.root = root; config.sources = sources; config.producerEpoch = 13u;
    config.maximumBytes = 128u * 1024u; config.segmentTargetBytes = 4096u;
    config.targetDurationNs = 10'000u; config.startedReceiveNs = 100;
    config.startedMonotonicNs = 100u; config.ringCapacity = 2u; config.shardCount = 1u;
    corpus::BinaryMarketCorpusWriter writer;
    CXET_CHECK(writer.start(config) == hftrec::Status::Ok);
    corpus::BinaryMarketRecord record{};
    auto& header = record.header;
    header.sourceId = 1u; header.sourceGeneration = 11u; header.sessionEpoch = 17u;
    header.eventSequence = header.frameSequence = header.shardSequence = 1u;
    header.receiveRealtimeNs = 200; header.receiveMonotonicNs = 200u; header.exchangeTimestampNs = 199;
    header.channel = corpus::BinaryMarketChannel::BookTicker;
    header.flags = corpus::BinaryMarketRecordTraderReplayCompatible;
    header.sourceFlags = corpus::BinaryMarketSourceBookTickerBidPresent | corpus::BinaryMarketSourceBookTickerAskPresent;
    header.payloadBytes = sizeof(corpus::BinaryMarketBookTickerPayload);
    *corpus::binaryMarketPayload<corpus::BinaryMarketBookTickerPayload>(&record) =
        {10'000'000'000, quantity.qtyRaw, 10'010'000'000, quantity.qtyRaw};
    CXET_CHECK(writer.append(record) == hftrec::Status::Ok);
    if (quantity.wholeUniverseSources) {
      auto addedSymbol = record; addedSymbol.header.sourceId = 2u; addedSymbol.header.shardSequence = 2u;
      addedSymbol.header.receiveMonotonicNs = addedSymbol.header.receiveRealtimeNs = 350u;
      CXET_CHECK(writer.append(addedSymbol) == hftrec::Status::Ok);
      if (quantity.recordedOnlyRequiredSource || quantity.noncompatibleRequiredRow) {
        auto unsupported = addedSymbol; unsupported.header.sourceId = 3u; unsupported.header.shardSequence = 3u;
        unsupported.header.receiveMonotonicNs = unsupported.header.receiveRealtimeNs = 400u;
        unsupported.header.flags = 0u;
        CXET_CHECK(writer.append(unsupported) == hftrec::Status::Ok);
      }
    }
    if (quantity.allChannels) {
      header.eventSequence = header.frameSequence = header.shardSequence = 2u;
      header.receiveMonotonicNs = header.receiveRealtimeNs = 250u;
      header.channel = corpus::BinaryMarketChannel::Trade; header.sourceFlags = 0u;
      header.payloadBytes = sizeof(corpus::BinaryMarketTradePayload);
      record.payload = {};
      *corpus::binaryMarketPayload<corpus::BinaryMarketTradePayload>(&record) = {10'000'000'000, quantity.qtyRaw, 1u, {}};
      CXET_CHECK(writer.append(record) == hftrec::Status::Ok);
      record = {}; auto& depthHeader = record.header;
      depthHeader.sourceId = 1u; depthHeader.sourceGeneration = 11u; depthHeader.sessionEpoch = 17u;
      depthHeader.eventSequence = depthHeader.frameSequence = depthHeader.shardSequence = 3u;
      depthHeader.receiveMonotonicNs = depthHeader.receiveRealtimeNs = 300u; depthHeader.exchangeTimestampNs = 299;
      depthHeader.channel = corpus::BinaryMarketChannel::Depth;
      depthHeader.flags = corpus::BinaryMarketRecordTraderReplayCompatible | corpus::BinaryMarketRecordDepthSnapshot | corpus::BinaryMarketRecordDepthRebase;
      depthHeader.payloadBytes = sizeof(corpus::BinaryMarketDepthChunkPayload);
      auto& depth = *corpus::binaryMarketPayload<corpus::BinaryMarketDepthChunkPayload>(&record); depth = {};
      depth.totalLevelCount = depth.levelCount = 2u; depth.partCount = depth.transactionPartCount = 1u;
      depth.frameKind = corpus::BinaryMarketDepthFrameKind::Snapshot; depth.state = corpus::BinaryMarketDepthState::RebaseInFlight;
      depth.sequence.firstUpdateId = depth.sequence.lastUpdateId = 1u;
      depth.sourceReceiveRealtimeNs = depth.sourceFreshnessMonotonicNs = 300u;
      depth.levels[0] = {10'000'000'000u, static_cast<std::uint64_t>(quantity.qtyRaw), 1u, 1u, {}};
      depth.levels[1] = {10'010'000'000u, static_cast<std::uint64_t>(quantity.qtyRaw), 2u, 1u, {}};
      CXET_CHECK(writer.append(record) == hftrec::Status::Ok);
    }
    if (quantity.changeConversion) {
      record = {}; auto& lifeHeader = record.header;
      lifeHeader.sourceId = 1u; lifeHeader.sourceGeneration = 12u; lifeHeader.sessionEpoch = 17u;
      lifeHeader.eventSequence = lifeHeader.shardSequence = quantity.allChannels ? 4u : 2u;
      lifeHeader.receiveRealtimeNs = lifeHeader.receiveMonotonicNs = 400u;
      lifeHeader.channel = corpus::BinaryMarketChannel::SourceLifecycle;
      lifeHeader.flags = corpus::BinaryMarketRecordSourceLifecycle | corpus::BinaryMarketRecordTraderReplayCompatible | corpus::BinaryMarketRecordExchangeTimestampMissing;
      lifeHeader.payloadBytes = sizeof(corpus::BinaryMarketSourceLifecyclePayload);
      auto& life = *corpus::binaryMarketPayload<corpus::BinaryMarketSourceLifecyclePayload>(&record); life = {};
      life.directoryRevision = life.requestSequence = 1u; life.previousSourceGeneration = 11u;
      life.channelMaskBefore = life.channelMaskAfter = source.availableChannelMask;
      life.kind = corpus::BinaryMarketSourceLifecycleKind::DirectoryChanged;
      life.affectedChannel = static_cast<std::uint8_t>(corpus::BinaryMarketChannel::BookTicker);
      life.source = source; life.source.initialSourceGeneration = 12u;
      ++life.source.quantityConversion.canonicalBaseMultiplier;
      CXET_CHECK(writer.append(record) == hftrec::Status::Ok);
    }
    CXET_CHECK(writer.finalize(corpus::BinaryMarketStopReason::Requested, 10'000) == hftrec::Status::Ok);
  }
  ~QuantFixture() { std::error_code error; std::filesystem::remove_all(root, error); }
  struct Loaded {
    hft_backtest::Status status{};
    std::string error;
    detail::SessionData session;
  };
  Loaded load(std::string symbol = "BTC_USDT") const {
    Loaded result;
    detail::SessionLoadPlan plan; plan.loadTrades = false; plan.requireBookTickers = true;
    result.status = detail::loadBinaryMarketCorpus(root,
        {venueName, marketName, std::move(symbol), 100, 10'000}, plan, result.session, result.error);
    return result;
  }
};

const cxet::testing::Case cases[]{
    cxet::testing::Case{"drawdown_empty_single_and_portfolio_keep_zero_baseline", +[] {
      auto engine = std::make_unique<detail::EngineState>();
      const auto single = detail::calculateExcursionMetrics(*engine);
      CXET_CHECK(single.maxDrawdownE8 == 0 && single.mfeE8 == 0 && single.maeE8 == 0);
      const auto empty = detail::calculatePortfolioExcursionMetrics({});
      CXET_CHECK(empty.maxDrawdownE8 == 0 && empty.mfeE8 == 0 && empty.maeE8 == 0);
      const std::vector<detail::EngineState> engines(2u);
      const auto portfolio = detail::calculatePortfolioExcursionMetrics(engines);
      CXET_CHECK(portfolio.maxDrawdownE8 == 0 && portfolio.mfeE8 == 0 && portfolio.maeE8 == 0);
    }},
    cxet::testing::Case{"drawdown_first_loss_counts_from_zero_on_equity_and_risk_paths", +[] {
      for (const bool risk : {false, true}) {
        auto engine = std::make_unique<detail::EngineState>();
        for (const auto pnl : {-7, -2, 4, -3}) {
          if (risk) {
            detail::JsonRiskSnapshot row{}; row.netTotalPnlE8 = pnl;
            engine->riskSnapshots.push_back(row);
          } else {
            detail::JsonEquityPoint row{}; row.netTotalPnlE8 = pnl;
            engine->equityPoints.push_back(row);
          }
        }
        hft_backtest::BacktestRunResult result{};
        hft_backtest::populateResultPnl(result, *engine);
        CXET_CHECK(result.maxDrawdownE8 == 7 && result.mfeE8 == 4 && result.maeE8 == -7);
      }
    }},
    cxet::testing::Case{"drawdown_extreme_first_loss_saturates_without_signed_negation", +[] {
      auto engine = std::make_unique<detail::EngineState>();
      detail::JsonRiskSnapshot row{}; row.netTotalPnlE8 = std::numeric_limits<std::int64_t>::min();
      engine->riskSnapshots.push_back(row);
      const auto metrics = detail::calculateExcursionMetrics(*engine);
      CXET_CHECK(metrics.maxDrawdownE8 == std::numeric_limits<std::int64_t>::max());
      CXET_CHECK(metrics.mfeE8 == 0 && metrics.maeE8 == std::numeric_limits<std::int64_t>::min());
    }},
    cxet::testing::Case{"portfolio_drawdown_uses_zero_baseline_and_latest_same_timestamp_leg_rows", +[] {
      for (const bool risk : {false, true}) {
        std::vector<detail::EngineState> engines(2u);
        const auto append = [&](std::size_t leg, std::int64_t ts, std::int64_t pnl) {
          if (risk) {
            detail::JsonRiskSnapshot row{}; row.tsNs = ts; row.netTotalPnlE8 = pnl;
            engines[leg].riskSnapshots.push_back(row);
          } else {
            detail::JsonEquityPoint row{}; row.tsNs = ts; row.netTotalPnlE8 = pnl;
            engines[leg].equityPoints.push_back(row);
          }
        };
        append(0u, 10, -100); append(0u, 10, -7); append(1u, 10, 2);
        append(0u, 20, 8); append(1u, 20, -2);
        append(1u, 30, -10);
        hft_backtest::BacktestRunResult result{};
        hft_backtest::populatePortfolioResultPnl(result, {}, engines, 0);
        CXET_CHECK(result.maxDrawdownE8 == 8 && result.mfeE8 == 6 && result.maeE8 == -5);
        // The first synchronized point alone must include the initial loss.
        if (risk) {
          engines[0].riskSnapshots.resize(2u); engines[1].riskSnapshots.resize(1u);
        } else {
          engines[0].equityPoints.resize(2u); engines[1].equityPoints.resize(1u);
        }
        const auto first = detail::calculatePortfolioExcursionMetrics(engines);
        CXET_CHECK(first.maxDrawdownE8 == 5 && first.mfeE8 == 0 && first.maeE8 == -5);
      }
    }},
    cxet::testing::Case{"bbo_optimism_warning_requires_actual_fills_including_no_depth_mode", +[] {
      auto engine = std::make_unique<detail::EngineState>();
      engine->depthExecutionMode = hft_backtest::BacktestDepthExecutionMode::BboNoDepth;
      hft_backtest::BacktestRunResult result{};
      hft_backtest::appendDepthExecutionWarnings(result, *engine);
      CXET_CHECK(result.warnings.empty());
      engine->depthFallbackFills = 1u;
      hft_backtest::appendDepthExecutionWarnings(result, *engine);
      CXET_CHECK(result.warnings.size() == 1u);
      CXET_CHECK(result.warnings[0].find("optimistic BBO") != std::string::npos);
      CXET_CHECK(result.warnings[0].find("without displayed quantity limits") != std::string::npos);
      hft_backtest::appendDepthExecutionWarnings(result, *engine);
      CXET_CHECK(result.warnings.size() == 1u);
    }},
    cxet::testing::Case{"depth_failure_diagnostic_survives_without_fills_and_keeps_reason", +[] {
      auto engine = std::make_unique<detail::EngineState>();
      engine->depthExecutionMode = hft_backtest::BacktestDepthExecutionMode::BboFallback;
      engine->depthFallbackReason = "missing execution row";
      hft_backtest::BacktestRunResult result{};
      hft_backtest::appendDepthExecutionWarnings(result, *engine);
      CXET_CHECK(result.warnings.size() == 1u);
      CXET_CHECK(result.warnings[0].find("declared execution depth failed") != std::string::npos);
      CXET_CHECK(result.warnings[0].find(engine->depthFallbackReason) != std::string::npos);
      CXET_CHECK(result.warnings[0].find("optimistic BBO") == std::string::npos);
      engine->depthFallbackFills = 2u;
      hft_backtest::appendDepthExecutionWarnings(result, *engine);
      CXET_CHECK(result.warnings.size() == 2u && result.warnings[1].find("optimistic BBO") != std::string::npos);
    }},
    cxet::testing::Case{"portfolio_bbo_warning_identifies_only_legs_with_actual_top_fills", +[] {
      auto aggregate = std::make_unique<detail::EngineState>();
      std::vector<detail::EngineState> legs(2u);
      legs[0].depthExecutionMode = hft_backtest::BacktestDepthExecutionMode::BboNoDepth;
      legs[1].depthExecutionMode = hft_backtest::BacktestDepthExecutionMode::Depth;
      legs[1].depthFallbackFills = 1u;
      hft_backtest::BacktestRunResult result{};
      hft_backtest::appendDepthExecutionWarnings(result, *aggregate, &legs);
      CXET_CHECK(result.warnings.size() == 1u && result.warnings[0].find("leg 1: optimistic BBO") == 0u);
    }},
    cxet::testing::Case{"service_unwind_without_genuine_opposite_price_retains_spot_and_futures_exposure", +[] {
      for (const auto market : {canon::kMarketTypeSpot, canon::kMarketTypeFutures}) {
        SpotFixture fixture; auto engine = fixture.engine(0u, market);
        const auto probe = SpotFixture::intent(212u, Side::Buy(), SpotFixture::q, canon::OrderType::Limit, 50 * SpotFixture::q);
        CXET_CHECK(detail::execution_detail::submitOrderIntent(*engine, probe, 10, 20, true, 0u, false)); detail::applyDueActions(*engine, 20);
        engine->venueSnapshot.lastBookTicker.bid.px.raw = 0;
        CXET_CHECK(detail::fillOrderQty(*engine, engine->open[0], 50 * SpotFixture::q, SpotFixture::q, 30, true, {}));
        CXET_CHECK(engine->netQtyRaw == SpotFixture::q && engine->serviceExposureOpen && engine->fills.size() == 1u);
        const auto cash = (market.raw == canon::kMarketTypeSpot.raw ? 50 : 100) * SpotFixture::q;
        CXET_CHECK(fixture.account.walletBalanceE8 == cash);
        SpotFixture::deliverAll(*engine);
        CXET_CHECK(engine->position.netQty.raw == SpotFixture::q && hft_backtest::accountStateFromEngine(*engine).walletBalanceE8 == cash);
      }
    }},
    cxet::testing::Case{"warmup_ack_is_unknown_until_correlated_private_delivery", +[] {
      SpotFixture fixture; auto engine = fixture.engine(0u);
      const auto probe = SpotFixture::intent(213u, Side::Buy(), SpotFixture::q, canon::OrderType::Limit, 50 * SpotFixture::q);
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*engine, probe, 10, 20, true, 0u, false));
      const auto submitted = hft_backtest::strategy_runtime_detail::backtestWarmupView(*engine);
      CXET_CHECK(submitted.warmupInFlight && submitted.lastAckMono.raw == 0u && !submitted.routeHot);
      detail::applyDueActions(*engine, 20);
      const auto activated = hft_backtest::strategy_runtime_detail::backtestWarmupView(*engine);
      CXET_CHECK(activated.warmupInFlight && activated.lastAckMono.raw == 0u && !activated.routeHot);
      const auto* record = trading_core::strategy::canonicalOrderEvidence(detail::simulationOrderEvidenceView(*engine), probe.clientIntentId, 1u);
      CXET_CHECK(record && record->placeSettled && !record->venueObserved);
      CXET_CHECK(engine->userEvents.size() == 1u && engine->userEventDeliveryTsNs[0] > 20);
      CXET_CHECK(!detail::deliverSimulationOrderEvent(*engine, 0u, engine->userEventDeliveryTsNs[0] - 1));
      CXET_CHECK(detail::deliverSimulationOrderEvent(*engine, 0u, engine->userEventDeliveryTsNs[0]));
      const auto received = hft_backtest::strategy_runtime_detail::backtestWarmupView(*engine);
      CXET_CHECK(!received.warmupInFlight && received.routeHot && record->venueObserved);
      CXET_CHECK(received.lastAckMono.raw == static_cast<std::uint64_t>(engine->userEventDeliveryTsNs[0]));
      CXET_CHECK(detail::fillOrderQty(*engine, engine->open[0], 50 * SpotFixture::q, SpotFixture::q, 2000, true, {}));
      CXET_CHECK(engine->netQtyRaw == 0 && engine->userEvents.size() == 4u);
      CXET_CHECK(!hft_backtest::strategy_runtime_detail::backtestWarmupView(*engine).serviceExposureOpen);
      CXET_CHECK(detail::deliverSimulationOrderEvent(*engine, 1u, engine->userEventDeliveryTsNs[1]));
      CXET_CHECK(engine->position.netQty.raw == SpotFixture::q &&
          hft_backtest::strategy_runtime_detail::backtestWarmupView(*engine).serviceExposureOpen);
      CXET_CHECK(detail::deliverSimulationOrderEvent(*engine, 2u, engine->userEventDeliveryTsNs[2]));
      CXET_CHECK(hft_backtest::strategy_runtime_detail::backtestWarmupView(*engine).serviceExposureOpen);
      CXET_CHECK(detail::deliverSimulationOrderEvent(*engine, 3u, engine->userEventDeliveryTsNs[3]));
      CXET_CHECK(engine->position.netQty.raw == 0 &&
          !hft_backtest::strategy_runtime_detail::backtestWarmupView(*engine).serviceExposureOpen);
    }},
    cxet::testing::Case{"whole_market_available_recorded_only_required_source_is_not_a_quiet_pause", +[] {
      QuantityFixtureConfig config; config.wholeUniverseSources = config.recordedOnlyRequiredSource = true;
      QuantFixture fixture(0, true, 1u, config); const auto result = fixture.load("LTC_USDT");
      CXET_CHECK(result.status == hft_backtest::Status::Unsupported && result.error.find("replay") != std::string::npos);
    }},
    cxet::testing::Case{"whole_market_nonhealth_required_row_without_replay_compatibility_refuses", +[] {
      QuantityFixtureConfig config; config.wholeUniverseSources = config.noncompatibleRequiredRow = true;
      QuantFixture fixture(0, true, 1u, config); const auto result = fixture.load("LTC_USDT");
      CXET_CHECK(result.status == hft_backtest::Status::Unsupported && result.error.find("replay") != std::string::npos);
    }},
    cxet::testing::Case{"shared_account_private_credit_precedes_later_debit_across_reversed_source_indices", +[] {
      SpotFixture fixture; auto debit = fixture.engine(0u); auto credit = fixture.engine(1u);
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*credit, SpotFixture::intent(220u, Side::Buy(), SpotFixture::q / 2), 10, 20));
      detail::applyDueActions(*credit, 20); SpotFixture::deliverAll(*credit);
      CXET_CHECK(fixture.account.observedWalletBalanceE8 == 70 * SpotFixture::q);
      SpotFixture::top(*credit, 70 * SpotFixture::q, 71 * SpotFixture::q);
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*credit, SpotFixture::intent(221u, Side::Sell(), SpotFixture::q / 2), 30, 40));
      detail::applyDueActions(*credit, 40);
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*debit, SpotFixture::intent(222u, Side::Buy(), SpotFixture::q / 2), 30, 40));
      detail::applyDueActions(*debit, 40);
      const auto creditHead = credit->deliveredUserEvents;
      CXET_CHECK(credit->userEvents[creditHead].updateTime.raw == debit->userEvents[0].updateTime.raw);
      CXET_CHECK(credit->userEventDeliveryTsNs[creditHead + 1] < debit->userEventDeliveryTsNs[0]);
      CXET_CHECK(fixture.account.walletBalanceE8 == 75 * SpotFixture::q);
      CXET_CHECK(fixture.account.observedWalletBalanceE8 == 70 * SpotFixture::q);
      CXET_CHECK(hft_backtest::accountStateFromEngine(*debit).availableBalanceE8 == 40 * SpotFixture::q);
      SpotFixture::deliverAll(*credit);
      CXET_CHECK(fixture.account.observedWalletBalanceE8 == 105 * SpotFixture::q);
      SpotFixture::deliverAll(*debit);
      CXET_CHECK(fixture.account.observedWalletBalanceE8 == 75 * SpotFixture::q);
    }},
    cxet::testing::Case{"spot_rejects_naked_ask_without_negative_base_inventory", +[] {
      SpotFixture fixture; auto engine = fixture.engine(0u);
      const auto sell = SpotFixture::intent(201u, Side::Sell());
      (void)detail::execution_detail::submitOrderIntent(*engine, sell, 10, 20); detail::applyDueActions(*engine, 20);
      CXET_CHECK(engine->fills.empty() && engine->netQtyRaw == 0 && fixture.account.walletBalanceE8 == 100 * SpotFixture::q);
      SpotFixture::deliverAll(*engine); CXET_CHECK(engine->position.netQty.raw == 0);
    }},
    cxet::testing::Case{"spot_two_symbols_cannot_spend_shared_cash_twice", +[] {
      SpotFixture fixture; auto btc = fixture.engine(0u); auto eth = fixture.engine(1u);
      (void)detail::execution_detail::submitOrderIntent(*btc, SpotFixture::intent(202u, Side::Buy()), 10, 20);
      CXET_CHECK(hft_backtest::accountStateFromEngine(*eth).availableBalanceE8 == 40 * SpotFixture::q);
      (void)detail::execution_detail::submitOrderIntent(*eth, SpotFixture::intent(203u, Side::Buy()), 10, 20);
      detail::applyDueActions(*btc, 20); detail::applyDueActions(*eth, 20);
      CXET_CHECK(btc->fills.size() + eth->fills.size() == 1u && fixture.account.walletBalanceE8 == 40 * SpotFixture::q);
      SpotFixture::deliverAll(*btc); SpotFixture::deliverAll(*eth);
      CXET_CHECK(hft_backtest::accountStateFromEngine(*eth).walletBalanceE8 == 40 * SpotFixture::q);
    }},
    cxet::testing::Case{"spot_partial_fill_and_cancel_release_observed_reserve_only_at_private_terminal", +[] {
      SpotFixture fixture; auto engine = fixture.engine(0u); SpotFixture::top(*engine, 79 * SpotFixture::q, 80 * SpotFixture::q);
      const auto buy = SpotFixture::intent(204u, Side::Buy(), SpotFixture::q, canon::OrderType::Limit, 60 * SpotFixture::q);
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*engine, buy, 10, 20)); detail::applyDueActions(*engine, 20);
      SpotFixture::deliverAll(*engine);
      CXET_CHECK(detail::fillOrderQty(*engine, engine->open[0], 60 * SpotFixture::q, SpotFixture::q / 2, 30, false, {}));
      CXET_CHECK(fixture.account.walletBalanceE8 == 70 * SpotFixture::q);
      CXET_CHECK(hft_backtest::accountStateFromEngine(*engine).walletBalanceE8 == 100 * SpotFixture::q);
      SpotFixture::deliverAll(*engine);
      CXET_CHECK(hft_backtest::accountStateFromEngine(*engine).walletBalanceE8 == 70 * SpotFixture::q);
      CXET_CHECK(hft_backtest::accountStateFromEngine(*engine).availableBalanceE8 == 40 * SpotFixture::q);
      trading_core::CancelIntent cancel{}; cancel.clientIntentId.raw = 205u; cancel.targetClientIntentId = buy.clientIntentId;
      CXET_CHECK(detail::execution_detail::submitCancelIntent(*engine, cancel, 40, 50)); detail::applyDueActions(*engine, 50);
      CXET_CHECK(hft_backtest::accountStateFromEngine(*engine).availableBalanceE8 == 40 * SpotFixture::q);
      SpotFixture::deliverAll(*engine);
      CXET_CHECK(hft_backtest::accountStateFromEngine(*engine).availableBalanceE8 == 70 * SpotFixture::q && engine->position.netQty.raw == SpotFixture::q / 2);
      std::vector<detail::EngineState> sources; sources.push_back(std::move(*engine));
      hft_backtest::BacktestRunResult result{};
      hft_backtest::populatePortfolioResultPnl(result, {}, sources, 100 * SpotFixture::q);
      CXET_CHECK(result.walletBalanceE8 == 70 * SpotFixture::q && result.netRealizedPnlE8 == 0);
      const auto markedPnl = 19 * SpotFixture::q / 2;
      CXET_CHECK(result.totalPnlE8 == markedPnl && result.unrealizedPnlE8 == markedPnl);
      CXET_CHECK(!result.totalPnlCurveE8.empty() && result.totalPnlCurveE8.back() == markedPnl);
      CXET_CHECK(sources[0].equityPoints.back().unrealizedPnlE8 == markedPnl);
      auto aggregate = std::make_unique<detail::EngineState>();
      hft_backtest::populateAggregateArtifactEngine(sources, 100 * SpotFixture::q, 50, *aggregate);
      CXET_CHECK(aggregate->walletBalanceE8 == 70 * SpotFixture::q);
      CXET_CHECK(aggregate->equityPoints.back().totalPnlE8 == markedPnl);
      CXET_CHECK(aggregate->riskSnapshots.back().unrealizedPnlE8 == markedPnl &&
          aggregate->riskSnapshots.back().netTotalPnlE8 == markedPnl);
    }},
    cxet::testing::Case{"spot_buy_sell_conserves_principal_equity_and_realized_pnl", +[] {
      SpotFixture fixture; auto engine = fixture.engine(0u);
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*engine, SpotFixture::intent(206u, Side::Buy()), 10, 20)); detail::applyDueActions(*engine, 20);
      CXET_CHECK(fixture.account.walletBalanceE8 == 40 * SpotFixture::q && detail::equityE8(*engine) == 100 * SpotFixture::q);
      SpotFixture::deliverAll(*engine);
      engine->capturedSourceGeneration = 11u;
      detail::SourceHealthRow pause{}; pause.unhealthy = true;
      pause.record.header.channel = corpus::BinaryMarketChannel::BookTicker;
      pause.record.header.sourceGeneration = 11u;
      std::string error;
      CXET_CHECK(hft_backtest::run_detail::applyCapturedSourceHealth(*engine, pause, 25, error));
      auto sibling = fixture.engine(1u);
      CXET_CHECK(engine->venueSnapshot.hasBookTicker.raw == trading_core::kAlgoFlagFalse.raw &&
          engine->snapshot.hasBookTicker.raw == trading_core::kAlgoFlagFalse.raw);
      CXET_CHECK(engine->netQtyRaw == SpotFixture::q && engine->position.netQty.raw == SpotFixture::q);
      CXET_CHECK(detail::equityE8(*engine) == 100 * SpotFixture::q);
      CXET_CHECK(hft_backtest::accountStateFromEngine(*sibling).equityE8 == 100 * SpotFixture::q);
      SpotFixture::top(*engine, 70 * SpotFixture::q, 71 * SpotFixture::q);
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*engine, SpotFixture::intent(207u, Side::Sell()), 30, 40)); detail::applyDueActions(*engine, 40);
      CXET_CHECK(fixture.account.walletBalanceE8 == 110 * SpotFixture::q && engine->netQtyRaw == 0 && engine->netRealizedPnlE8 == 10 * SpotFixture::q);
      CXET_CHECK(hft_backtest::accountStateFromEngine(*engine).walletBalanceE8 == 40 * SpotFixture::q);
      SpotFixture::deliverAll(*engine);
      CXET_CHECK(hft_backtest::accountStateFromEngine(*engine).walletBalanceE8 == 110 * SpotFixture::q && engine->position.netQty.raw == 0);
    }},
    cxet::testing::Case{"spot_actual_price_overrun_rejects_at_venue_without_future_local_feedback", +[] {
      SpotFixture fixture; auto btc = fixture.engine(0u); auto eth = fixture.engine(1u);
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*btc, SpotFixture::intent(208u, Side::Buy()), 10, 20));
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*eth, SpotFixture::intent(209u, Side::Buy(), SpotFixture::q / 2), 10, 20));
      btc->venueSnapshot.lastBookTicker.ask.px.raw = eth->venueSnapshot.lastBookTicker.ask.px.raw = 80 * SpotFixture::q;
      detail::applyDueActions(*btc, 20); detail::applyDueActions(*eth, 20);
      CXET_CHECK(btc->fills.size() == 1u && eth->fills.empty() && fixture.account.walletBalanceE8 == 20 * SpotFixture::q);
      const auto* record = trading_core::strategy::canonicalOrderEvidence(detail::simulationOrderEvidenceView(*eth), Id{209u}, 1u);
      CXET_CHECK(record && !record->terminal() && !record->venueObserved);
      CXET_CHECK(hft_backtest::accountStateFromEngine(*eth).walletBalanceE8 == 100 * SpotFixture::q);
      SpotFixture::deliverAll(*btc); SpotFixture::deliverAll(*eth);
      CXET_CHECK(record->terminal() && hft_backtest::accountStateFromEngine(*eth).availableBalanceE8 == 20 * SpotFixture::q);
    }},
    cxet::testing::Case{"spot_cash_debits_exact_principal_and_fee_at_private_delivery", +[] {
      SpotFixture fixture; auto engine = fixture.engine(0u); engine->takerFeeBpsE8 = 100'000'000;
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*engine, SpotFixture::intent(210u, Side::Buy()), 10, 20)); detail::applyDueActions(*engine, 20);
      const auto expectedCash = 40 * SpotFixture::q - engine->feesPaidE8;
      CXET_CHECK(engine->feesPaidE8 > 0 && fixture.account.walletBalanceE8 == expectedCash);
      CXET_CHECK(hft_backtest::accountStateFromEngine(*engine).walletBalanceE8 == 100 * SpotFixture::q);
      CXET_CHECK(engine->userEvents.size() == 2u);
      // Admit the ACK as an explicit canonical input so the fill counterexample
      // reaches accounting independently of the simulated producer's markers.
      auto& ack = engine->userEvents[0];
      ack.quantityUnit = cxet::contract::Unit::BaseQuantity;
      ack.quantityDenomination = cxet::contract::QuantityDenomination::Canonical;
      cxet::composite::markUserDataFieldPresent(ack, cxet::composite::UserDataField::QuantityUnit);
      cxet::composite::markUserDataFieldPresent(ack, cxet::composite::UserDataField::QuantityDenomination);
      CXET_CHECK(detail::deliverSimulationOrderEvent(*engine, 0u, engine->userEventDeliveryTsNs[0]));
      auto& fill = engine->userEvents[1];
      const auto producedFill = fill;
      auto canonicalFill = fill;
      canonicalFill.quantityUnit = cxet::contract::Unit::BaseQuantity;
      canonicalFill.quantityDenomination = cxet::contract::QuantityDenomination::Canonical;
      cxet::composite::markUserDataFieldPresent(canonicalFill, cxet::composite::UserDataField::QuantityUnit);
      cxet::composite::markUserDataFieldPresent(canonicalFill, cxet::composite::UserDataField::QuantityDenomination);
      const auto* record = trading_core::strategy::canonicalOrderEvidence(
          detail::simulationOrderEvidenceView(*engine), Id{210u}, 1u);
      CXET_CHECK(record && record->venueObserved && !record->terminal() && record->cumulativeQtyE8 == 0);
      const auto wallet = fixture.account.observedWalletBalanceE8;
      const auto reserved = fixture.account.observedReservedQuoteE8;
      const auto accountRevision = fixture.account.revision;
      const auto contextRevision = engine->contextRevision;
      const auto replayTime = engine->lastReplayMonotonicNs;
      for (unsigned variant = 0u; variant < 8u; ++variant) {
        fill = canonicalFill;
        switch (variant) {
          case 0u: fill.quantityDenomination = cxet::contract::QuantityDenomination::Native; break;
          case 1u: fill.quantityDenomination = cxet::contract::QuantityDenomination::Unknown; break;
          case 2u: fill.quantityUnit = cxet::contract::Unit::ContractQuantity; break;
          case 3u: fill.quantityUnit = cxet::contract::Unit::LotQuantity; break;
          case 4u: fill.fieldPresenceMask &= ~cxet::composite::userDataFieldMaskBit(cxet::composite::UserDataField::QuantityUnit); break;
          case 5u: fill.fieldPresenceMask &= ~cxet::composite::userDataFieldMaskBit(cxet::composite::UserDataField::QuantityDenomination); break;
          case 6u: fill.fieldPresenceMask &= ~cxet::composite::userDataFieldMaskBit(cxet::composite::UserDataField::Qty); break;
          case 7u: fill.quantityUnit = cxet::contract::Unit::None; break;
        }
        CXET_CHECK(!detail::deliverSimulationOrderEvent(*engine, 1u, engine->userEventDeliveryTsNs[1]));
        CXET_CHECK(record->cumulativeQtyE8 == 0 && !record->terminal());
        CXET_CHECK(engine->position.netQty.raw == 0 && engine->deliveredUserEvents == 1u && !engine->userEventConsumed[1]);
        CXET_CHECK(fixture.account.observedWalletBalanceE8 == wallet &&
            fixture.account.observedReservedQuoteE8 == reserved && fixture.account.revision == accountRevision);
        CXET_CHECK(engine->contextRevision == contextRevision && engine->lastReplayMonotonicNs == replayTime);
      }
      fill = producedFill;
      CXET_CHECK(fill.quantityUnit == cxet::contract::Unit::BaseQuantity &&
          fill.quantityDenomination == cxet::contract::QuantityDenomination::Canonical);
      CXET_CHECK(cxet::composite::userDataFieldPresent(fill, cxet::composite::UserDataField::QuantityUnit) &&
          cxet::composite::userDataFieldPresent(fill, cxet::composite::UserDataField::QuantityDenomination));
      SpotFixture::deliverAll(*engine); CXET_CHECK(hft_backtest::accountStateFromEngine(*engine).walletBalanceE8 == expectedCash);
    }},
    cxet::testing::Case{"linear_futures_keep_pnl_wallet_and_allow_short_inventory_model", +[] {
      SpotFixture fixture; auto engine = fixture.engine(0u, canon::kMarketTypeFutures);
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*engine, SpotFixture::intent(211u, Side::Sell()), 10, 20)); detail::applyDueActions(*engine, 20);
      CXET_CHECK(engine->netQtyRaw == -SpotFixture::q && fixture.account.walletBalanceE8 == 100 * SpotFixture::q);
      SpotFixture::deliverAll(*engine); CXET_CHECK(engine->position.netQty.raw == -SpotFixture::q);
    }},
    cxet::testing::Case{"whole_market_corpus_loads_configured_and_added_symbols", +[] {
      QuantityFixtureConfig config; config.wholeUniverseSources = true;
      QuantFixture fixture(0, true, 1u, config);
      const auto first = fixture.load(), added = fixture.load("ETH_USDT");
      CXET_CHECK(first.status == hft_backtest::Status::Ok && added.status == hft_backtest::Status::Ok);
      CXET_CHECK(first.session.symbol == "BTC_USDT" && added.session.symbol == "ETH_USDT");
      CXET_CHECK(first.session.bookTickers.size() == 1u && added.session.bookTickers.size() == 1u);
    }},
    cxet::testing::Case{"legacy_single_session_cannot_infer_market_quantity_unit_from_order_grid", +[] {
      detail::InstrumentMetadata metadata{}; metadata.lotSizeE8 = 100; metadata.tickSizeE8 = 1;
      trading_core::StrategyInstrumentSpec instrument{}; std::string error;
      CXET_CHECK(hft_backtest::prepareBacktestStrategyInstrument(metadata, instrument, error) == hft_backtest::Status::Unsupported);
      CXET_CHECK(error.find("execution quantity") != std::string::npos);
    }},
    cxet::testing::Case{"linear_swap_accepts_exact_recorded_native_base_quantity_authority", +[] {
      for (const auto& exchange : canon::exchangeRegistryEntries())
        CXET_CHECK(detail::exchangeFromText(exchange.canonicalName).raw == exchange.id.raw);
      CXET_CHECK(detail::exchangeFromText("bingx").raw == canon::kExchangeIdBingx.raw);
      CXET_CHECK(detail::exchangeFromText("Bingx").raw == canon::kExchangeIdUnknown.raw);
      CXET_CHECK(detail::exchangeFromText(std::string{"bingx\0suffix", 12u}).raw == canon::kExchangeIdUnknown.raw);
      QuantityFixtureConfig config; config.marketKind = 2u;
      config.marketRaw = canon::kMarketTypeFutures.raw; config.venue = "bingx";
      config.conversion.kinds.fill(corpus::BinaryMarketQuantityKind::NativeBase);
      QuantFixture fixture(100'000'000, true, 1u, config); const auto result = fixture.load();
      CXET_CHECK(result.status == hft_backtest::Status::Ok);
      CXET_CHECK(result.session.bookTickers.size() == 1u && result.session.bookTickers[0].bidQtyE8 == config.qtyRaw);
    }},
    cxet::testing::Case{"quantity_authority_change_refuses_instead_of_reinterpreting_retained_state", +[] {
      QuantityFixtureConfig config; config.conversion.kinds.fill(corpus::BinaryMarketQuantityKind::NativeBase);
      config.changeConversion = true; QuantFixture fixture(0, true, 1u, config);
      detail::SessionLoadPlan plan; plan.loadTrades = false; plan.requireBookTickers = true;
      const std::array<hft_backtest::BacktestCorpusSourceRequest, 1> requests{
          hft_backtest::BacktestCorpusSourceRequest{"binance", "spot", "BTC_USDT", 100, 10'000}};
      detail::BinaryCorpusStream stream; std::string error; std::vector<detail::TimelineEvent> events; bool available = false;
      CXET_CHECK(stream.open(fixture.root, requests, plan, error) == hft_backtest::Status::Ok);
      CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Ok && available);
      CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Unsupported && !available);
      CXET_CHECK(error.find("quantity") != std::string::npos);
    }},
    cxet::testing::Case{"different_venue_base_and_contract_channels_project_equal_canonical_quantities", +[] {
      for (const bool contract : {false, true}) {
        QuantityFixtureConfig config; config.marketKind = 2u; config.allChannels = true;
        config.venue = contract ? "okx" : "binance";
        config.conversion.kinds.fill(contract ? corpus::BinaryMarketQuantityKind::NativeContract : corpus::BinaryMarketQuantityKind::NativeBase);
        config.conversion.canonicalBaseMultiplier = 2u; config.conversion.nativeBaseMultiplier = 1u;
        config.conversion.nativeContractBaseQtyRaw = contract ? 50'000'000 : 0;
        config.qtyRaw = contract ? 2'400'000'000 : 1'200'000'000;
        QuantFixture fixture(0, true, 1u, config);
        detail::SessionLoadPlan plan; plan.loadDepths = true;
        plan.requireBookTickers = plan.requireTrades = plan.requireDepths = true;
        const std::array<hft_backtest::BacktestCorpusSourceRequest, 1> requests{
            hft_backtest::BacktestCorpusSourceRequest{std::string(config.venue), "futures", "BTC_USDT", 100, 10'000}};
        detail::BinaryCorpusStream stream; std::string error; std::vector<detail::TimelineEvent> events; bool available = false;
        const auto openStatus = stream.open(fixture.root, requests, plan, error);
        if (openStatus != hft_backtest::Status::Ok)
          std::fprintf(stderr, "quantity replay open: contract=%d status=%u error=%s\n",
              static_cast<int>(contract), static_cast<unsigned>(openStatus), error.c_str());
        CXET_CHECK(openStatus == hft_backtest::Status::Ok);
        CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Ok && available);
        CXET_CHECK(stream.sessions()[0].bookTickers[0].bidQtyE8 == 600'000'000 && stream.sessions()[0].bookTickers[0].askQtyE8 == 600'000'000);
        CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Ok && available);
        CXET_CHECK(stream.sessions()[0].trades[0].qtyE8 == 600'000'000);
        CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Ok && available);
        CXET_CHECK(stream.sessions()[0].depths[0].levels[0].qtyE8 == 600'000'000 && stream.sessions()[0].depths[0].levels[1].qtyE8 == 600'000'000);
        corpus::BinaryMarketSelectionRequest native; native.root = fixture.root; native.exchange = std::string(config.venue);
        native.market = "futures"; native.canonicalSymbol = "BTC_USDT"; native.beginReceiveNs = 100; native.endReceiveNs = 10'000;
        native.channelMask = corpus::binaryMarketChannelBit(corpus::BinaryMarketChannel::BookTicker);
        corpus::BinaryMarketSelection selected;
        CXET_CHECK(corpus::BinaryMarketCorpusReader{}.select(native, selected, error) == hftrec::Status::Ok);
        CXET_CHECK(corpus::binaryMarketPayload<corpus::BinaryMarketBookTickerPayload>(&selected.records[0])->bidQtyRaw == config.qtyRaw);

      }
    }},
    cxet::testing::Case{"canonical_spot_quantities_are_not_packaged_twice", +[] {
      QuantityFixtureConfig config; config.conversion.canonicalBaseMultiplier = 1000u; config.conversion.nativeBaseMultiplier = 1u;
      QuantFixture fixture(0, true, 1u, config);
      detail::SessionLoadPlan plan; plan.loadTrades = false; plan.requireBookTickers = true;
      const std::array<hft_backtest::BacktestCorpusSourceRequest, 1> requests{
          hft_backtest::BacktestCorpusSourceRequest{"binance", "spot", "BTC_USDT", 100, 10'000}};
      detail::BinaryCorpusStream stream; std::string error; std::vector<detail::TimelineEvent> events; bool available = false;
      CXET_CHECK(stream.open(fixture.root, requests, plan, error) == hft_backtest::Status::Ok);
      CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Ok && available);
      CXET_CHECK(stream.sessions()[0].bookTickers[0].bidQtyE8 == config.qtyRaw);
    }},
    cxet::testing::Case{"native_lot_quantity_scales_then_uses_proven_native_lot_factor", +[] {
      corpus::BinaryMarketSource source{}; source.quantityScale = 6u;
      source.quantityConversion.kinds.fill(corpus::BinaryMarketQuantityKind::NativeLot);
      source.quantityConversion.canonicalBaseMultiplier = 2u; source.quantityConversion.nativeBaseMultiplier = 3u;
      source.quantityConversion.nativeLotBaseQtyRaw = 200'000'000;
      std::int64_t out = -1;
      CXET_CHECK(detail::binary_corpus::canonicalMarketQuantityToE8(source, corpus::BinaryMarketChannel::Trade, std::int64_t{1'000'000}, out));
      CXET_CHECK(out == 300'000'000);
    }},
    cxet::testing::Case{"unknown_selected_lane_quantity_authority_refuses_before_strategy_admission", +[] {
      QuantityFixtureConfig config; config.conversion = {};
      QuantFixture fixture(0, true, 1u, config); const auto result = fixture.load();
      CXET_CHECK(result.status == hft_backtest::Status::Unsupported && result.error.find("quantity") != std::string::npos);
    }},
    cxet::testing::Case{"quantity_projection_refuses_invalid_kind_missing_factors_and_contract_authority", +[] {
      corpus::BinaryMarketSource source{}; source.quantityScale = 8u;
      source.quantityConversion.kinds.fill(corpus::BinaryMarketQuantityKind::NativeContract);
      std::int64_t out = -7;
      CXET_CHECK(!detail::binary_corpus::canonicalMarketQuantityToE8(source, corpus::BinaryMarketChannel::BookTicker, std::int64_t{100'000'000}, out));
      source.quantityConversion.canonicalBaseMultiplier = source.quantityConversion.nativeBaseMultiplier = 1u;
      CXET_CHECK(!detail::binary_corpus::canonicalMarketQuantityToE8(source, corpus::BinaryMarketChannel::BookTicker, std::int64_t{100'000'000}, out));
      source.quantityConversion.nativeContractBaseQtyRaw = 100'000'000;
      source.quantityConversion.kinds[0] = static_cast<corpus::BinaryMarketQuantityKind>(255u);
      CXET_CHECK(!detail::binary_corpus::canonicalMarketQuantityToE8(source, corpus::BinaryMarketChannel::BookTicker, std::int64_t{100'000'000}, out));
      CXET_CHECK(out == -7);
    }},
    cxet::testing::Case{"nonexact_quantity_division_rejects_selected_frame_without_rounded_copy", +[] {
      QuantityFixtureConfig config; config.conversion.kinds.fill(corpus::BinaryMarketQuantityKind::NativeBase);
      config.conversion.canonicalBaseMultiplier = 3u; config.qtyRaw = 1;
      QuantFixture fixture(0, true, 1u, config);
      detail::SessionLoadPlan plan; plan.loadTrades = false; plan.requireBookTickers = true;
      const std::array<hft_backtest::BacktestCorpusSourceRequest, 1> requests{
          hft_backtest::BacktestCorpusSourceRequest{"binance", "spot", "BTC_USDT", 100, 10'000}};
      detail::BinaryCorpusStream stream; std::string error; std::vector<detail::TimelineEvent> events; bool available = true;
      CXET_CHECK(stream.open(fixture.root, requests, plan, error) == hft_backtest::Status::Ok);
      CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::CorruptData && !available && events.empty());
    }},
    cxet::testing::Case{"quantity_projection_overflow_refuses_without_output_mutation", +[] {
      corpus::BinaryMarketSource source{}; source.quantityScale = 8u;
      source.quantityConversion.kinds.fill(corpus::BinaryMarketQuantityKind::NativeBase);
      source.quantityConversion.canonicalBaseMultiplier = 1u; source.quantityConversion.nativeBaseMultiplier = 2u;
      std::int64_t out = -9;
      CXET_CHECK(!detail::binary_corpus::canonicalMarketQuantityToE8(source, corpus::BinaryMarketChannel::Depth,
          std::numeric_limits<std::int64_t>::max(), out));
      CXET_CHECK(out == -9);
    }},
    cxet::testing::Case{"venue_fill_does_not_reach_market_cycle_or_shared_wallet_before_private_delivery", +[] {
      auto engine = std::make_unique<detail::EngineState>(); auto sibling = std::make_unique<detail::EngineState>();
      sibling->legIndex = 1u;
      constexpr std::string_view firstSymbol = "BTC_USDT", secondSymbol = "ETH_USDT";
      for (std::size_t i = 0u; i < firstSymbol.size(); ++i) engine->symbol.data[i] = firstSymbol[i];
      for (std::size_t i = 0u; i < secondSymbol.size(); ++i) sibling->symbol.data[i] = secondSymbol[i];
      detail::SharedAccountState account{}; account.initialBalanceE8 = account.walletBalanceE8 = 1'000'000'000;
      engine->sharedAccount = sibling->sharedAccount = &account;
      hft_backtest::BacktestRateLimitSchedule schedule; schedule.enabled = false;
      std::string quotaError;
      CXET_CHECK(hft_backtest::applyRateLimitSchedule(*engine, schedule, quotaError));
      hft_backtest::BacktestRunRequest request{}; request.initialBalanceE8 = account.initialBalanceE8; request.takerFeeBpsE8 = 100'000'000;
      detail::initializeAccounting(*engine, request); detail::initializeAccounting(*sibling, request);
      engine->activeContextIndex = 1u;
      engine->venueSnapshot.hasBookTicker = engine->snapshot.hasBookTicker = trading_core::kAlgoFlagTrue;
      engine->venueSnapshot.lastBookTicker.bid.px.raw = 100'000'000; engine->venueSnapshot.lastBookTicker.ask.px.raw = 101'000'000;
      engine->venueSnapshot.lastBookTicker.bid.qty.raw = engine->venueSnapshot.lastBookTicker.ask.qty.raw = 100'000'000;
      engine->snapshot.lastBookTicker = engine->venueSnapshot.lastBookTicker;
      detail::BacktestLatencyConfig latency{}; latency.userData = {1u, 0u}; detail::initLatencyRuntime(engine->latency, latency);
      trading_core::OrderIntent intent{}; intent.clientIntentId.raw = 91u; intent.orderType = canon::OrderType::Market;
      intent.side = Side::Buy(); intent.qty.raw = 100'000'000;
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*engine, intent, 10, 20)); detail::applyDueActions(*engine, 20);
      CXET_CHECK(engine->netQtyRaw == intent.qty.raw && account.walletBalanceE8 < account.initialBalanceE8);
      struct Projection { std::int64_t qty{}, wallet{}; } projection;
      trading_core::StrategyDescriptor descriptor;
      descriptor.name = "private_account_projection";
      descriptor.primaryStream = cxet::api::market::PublicMarketDataStream::BookTicker;
      descriptor.streamCount = 1u;
      descriptor.streams[0] = cxet::api::market::PublicMarketDataStream::BookTicker;
      descriptor.domainMask = trading_core::StrategyDomainMask::All;
      descriptor.minLegs = descriptor.maxLegs = 1u;
      descriptor.stateDiscipline = trading_core::StrategyStateDiscipline::Snapshot;
      descriptor.footprint = trading_core::strategyRuntimeFootprint<Projection>(0u, 0u, 0u, 0u, 0u);
      descriptor.runCycle = +[](void* state, trading_core::StrategyContext& ctx) noexcept {
        auto& out = *static_cast<Projection*>(state); out.qty = ctx.positionState.netQty.raw; out.wallet = ctx.accountState.walletBalanceE8; return true;
      };
      descriptor.captureState = +[](const void* state, trading_core::StrategyStateSnapshot& out) noexcept {
        const auto& captured = *static_cast<const Projection*>(state);
        out = {}; out.lastQty.raw = captured.qty; out.auxQty0.raw = captured.wallet;
      };
      CXET_CHECK(hft_backtest::strategy_intent_lowering::footprintSupported(descriptor));
      hft_backtest::SingleLegCycleScratch scratch{}; trading_core::StrategyInstrumentSpec instrument{};
      trading_core::SymbolShardConfig shard{}; trading_core::ShardControlState control{};
      CXET_CHECK(hft_backtest::runCycle(descriptor, {}, &projection, *engine, instrument, scratch, shard, control, 500, request, nullptr));
      CXET_CHECK(projection.qty == 0 && projection.wallet == account.initialBalanceE8);
      CXET_CHECK(hft_backtest::accountStateFromEngine(*sibling).walletBalanceE8 == account.initialBalanceE8);
      CXET_CHECK(detail::deliverSimulationOrderEvent(*engine, 0u, engine->userEventDeliveryTsNs[0]));
      CXET_CHECK(engine->position.netQty.raw == 0);
      CXET_CHECK(detail::deliverSimulationOrderEvent(*engine, 1u, engine->userEventDeliveryTsNs[1]));
      CXET_CHECK(hft_backtest::runCycle(descriptor, {}, &projection, *engine, instrument, scratch, shard, control, 1020, request, nullptr));
      CXET_CHECK(projection.qty == engine->netQtyRaw && projection.wallet == account.walletBalanceE8);
      CXET_CHECK(hft_backtest::accountStateFromEngine(*sibling).walletBalanceE8 == account.walletBalanceE8);
    }},
    cxet::testing::Case{"order_snapshot_keeps_private_pending_ack_and_terminal_boundaries", +[] {
      auto engine = std::make_unique<detail::EngineState>(); engine->activeContextIndex = 1u;
      hft_backtest::BacktestRateLimitSchedule schedule; schedule.enabled = false;
      std::string quotaError;
      CXET_CHECK(hft_backtest::applyRateLimitSchedule(*engine, schedule, quotaError));
      trading_core::OrderIntent intent{}; intent.clientIntentId.raw = 92u; intent.orderType = canon::OrderType::Limit;
      intent.side = Side::Buy(); intent.qty.raw = 4; intent.price.raw = 100;
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*engine, intent, 10, 20)); detail::applyDueActions(*engine, 20);
      std::array<trading_core::StrategyOrderState, detail::kMaxOrderStates> states{};
      CXET_CHECK(detail::visibleOrders(*engine, states.data(), states.size()) == 1u);
      CXET_CHECK(states[0].state == trading_core::StrategyOrderLocalState::PendingNew);
      CXET_CHECK(detail::deliverSimulationOrderEvent(*engine, 0u, engine->userEventDeliveryTsNs[0]));
      CXET_CHECK(detail::visibleOrders(*engine, states.data(), states.size()) == 1u);
      CXET_CHECK(states[0].state == trading_core::StrategyOrderLocalState::LiveLocal);
      CXET_CHECK(detail::fillOrderQty(*engine, engine->open[0], 100, 4, 30, true, {}));
      CXET_CHECK(!engine->open[0].state.valid && detail::visibleOrders(*engine, states.data(), states.size()) == 1u);
      CXET_CHECK(states[0].state == trading_core::StrategyOrderLocalState::LiveLocal);
      CXET_CHECK(detail::deliverSimulationOrderEvent(*engine, 1u, engine->userEventDeliveryTsNs[1]));
      CXET_CHECK(detail::visibleOrders(*engine, states.data(), states.size()) == 0u);
    }},
    cxet::testing::Case{"paused_private_head_advances_without_strategy_callback", +[] {
      auto engine = std::make_unique<detail::EngineState>(); engine->activeContextIndex = 1u;
      hft_backtest::BacktestRateLimitSchedule schedule; schedule.enabled = false;
      std::string quotaError;
      CXET_CHECK(hft_backtest::applyRateLimitSchedule(*engine, schedule, quotaError));
      trading_core::OrderIntent intent{}; intent.clientIntentId.raw = 93u; intent.orderType = canon::OrderType::Limit;
      intent.side = Side::Buy(); intent.qty.raw = 4; intent.price.raw = 100;
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*engine, intent, 10, 20));
      engine->activeContextIndex = 2u; intent.clientIntentId.raw = 94u;
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*engine, intent, 10, 20)); detail::applyDueActions(*engine, 20);
      engine->activeContextIndex = 1u; engine->capturedSourceManaged = true; engine->capturedSourceMember = false;
      struct Probe { std::size_t callbacks{}; } probe;
      trading_core::StrategyDescriptor descriptor;
      descriptor.name = "paused_private_callback_probe";
      descriptor.primaryStream = cxet::api::market::PublicMarketDataStream::BookTicker;
      descriptor.streamCount = 1u;
      descriptor.streams[0] = cxet::api::market::PublicMarketDataStream::BookTicker;
      descriptor.domainMask = trading_core::StrategyDomainMask::All;
      descriptor.minLegs = descriptor.maxLegs = 1u;
      descriptor.stateDiscipline = trading_core::StrategyStateDiscipline::Snapshot;
      descriptor.footprint = trading_core::strategyRuntimeFootprint<Probe>(0u, 0u, 0u, 0u, 0u);
      descriptor.runCycle = +[](void* state, trading_core::StrategyContext&) noexcept { ++static_cast<Probe*>(state)->callbacks; return true; };
      descriptor.captureState = +[](const void* state, trading_core::StrategyStateSnapshot& out) noexcept {
        const auto& captured = *static_cast<const Probe*>(state);
        out = {}; out.cycles = captured.callbacks;
      };
      CXET_CHECK(hft_backtest::strategy_intent_lowering::footprintSupported(descriptor));
      hft_backtest::SingleLegCycleScratch scratch{}; trading_core::StrategyInstrumentSpec instrument{};
      trading_core::SymbolShardConfig shard{}; trading_core::ShardControlState control{}; hft_backtest::BacktestRunRequest request{};
      CXET_CHECK(hft_backtest::runUntil(descriptor, {}, &probe, *engine, instrument, scratch, shard, control, 100, request));
      CXET_CHECK(engine->deliveredUserEvents == 1u && probe.callbacks == 0u);
      engine->activeContextIndex = 2u;
      CXET_CHECK(detail::deliverSimulationOrderEvent(*engine, 1u, engine->userEventDeliveryTsNs[1]));
      CXET_CHECK(engine->deliveredUserEvents == 2u);
    }},
    cxet::testing::Case{"warmup_cancel_retains_order_context_and_service_namespace", +[] {
      auto engine = std::make_unique<detail::EngineState>(); engine->activeContextIndex = 1u;
      hft_backtest::BacktestRateLimitSchedule schedule; schedule.enabled = false;
      std::string quotaError;
      CXET_CHECK(hft_backtest::applyRateLimitSchedule(*engine, schedule, quotaError));
      trading_core::OrderIntent intent{}; intent.clientIntentId.raw = 61u;
      intent.orderType = canon::OrderType::Limit; intent.side = Side::Buy(); intent.qty.raw = 4; intent.price.raw = 100;
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*engine, intent, 10, 20, true, 0u, true));
      engine->activeContextIndex = 2u;
      detail::applyDueActions(*engine, 20);
      CXET_CHECK(engine->activeContextIndex == 2u && engine->orders.size() == 2u);
      CXET_CHECK(engine->orders[1].contextIndex == 1u);
      CXET_CHECK((engine->orders[1].id & 0x8000000000000000ull) != 0u);
      CXET_CHECK(engine->userEvents.size() == 2u && engine->userEventContextIndices[1] == 1u);
      engine->activeContextIndex = 1u;
      CXET_CHECK(detail::deliverSimulationOrderEvent(*engine, 0u, engine->userEventDeliveryTsNs[0]));
      CXET_CHECK(detail::deliverSimulationOrderEvent(*engine, 1u, engine->userEventDeliveryTsNs[1]));
      const auto* record = trading_core::strategy::canonicalOrderEvidence(detail::simulationOrderEvidenceView(*engine), intent.clientIntentId, 1u);
      CXET_CHECK(record && record->terminal());
    }},
    cxet::testing::Case{"private_delivery_refuses_foreign_identity_and_source_head_overtake", +[] {
      auto engine = std::make_unique<detail::EngineState>(); engine->activeContextIndex = 1u;
      hft_backtest::BacktestRateLimitSchedule schedule; schedule.enabled = false;
      std::string quotaError;
      CXET_CHECK(hft_backtest::applyRateLimitSchedule(*engine, schedule, quotaError));
      CXET_CHECK(detail::prepareSimulationOrderEvidence(*engine));
      trading_core::OrderIntent intent{}; intent.clientIntentId.raw = 51u;
      intent.orderType = canon::OrderType::Limit; intent.side = Side::Buy(); intent.qty.raw = 4; intent.price.raw = 100;
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*engine, intent, 10, 20));
      engine->activeContextIndex = 2u; intent.clientIntentId.raw = 52u;
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*engine, intent, 10, 20));
      detail::applyDueActions(*engine, 20);
      CXET_CHECK(engine->userEvents.size() == 2u);
      CXET_CHECK(!detail::deliverSimulationOrderEvent(*engine, 1u, engine->userEventDeliveryTsNs[1]));
      engine->activeContextIndex = 1u;
      engine->userEventGenerations[0] = 2u;
      CXET_CHECK(!detail::deliverSimulationOrderEvent(*engine, 0u, engine->userEventDeliveryTsNs[0]));
      engine->userEventGenerations[0] = 1u;
      const auto expectedClient = engine->userEvents[0].clientOrderId;
      engine->userEvents[0].clientOrderId.data[0] ^= 1;
      CXET_CHECK(!detail::deliverSimulationOrderEvent(*engine, 0u, engine->userEventDeliveryTsNs[0]));
      engine->userEvents[0].clientOrderId = expectedClient;
      CXET_CHECK(detail::deliverSimulationOrderEvent(*engine, 0u, engine->userEventDeliveryTsNs[0]));
      engine->activeContextIndex = 2u;
      CXET_CHECK(detail::deliverSimulationOrderEvent(*engine, 1u, engine->userEventDeliveryTsNs[1]));
      CXET_CHECK(engine->deliveredUserEvents == 2u);
    }},
    cxet::testing::Case{"service_identity_exhaustion_refuses_without_namespace_reset", +[] {
      auto engine = std::make_unique<detail::EngineState>();
      engine->nextServiceClientIntentRaw = std::numeric_limits<std::uint64_t>::max();
      CXET_CHECK(hft_backtest::strategy_runtime_detail::allocateBacktestServiceClientIntentId(*engine).raw == std::numeric_limits<std::uint64_t>::max());
      CXET_CHECK(hft_backtest::strategy_runtime_detail::allocateBacktestServiceClientIntentId(*engine).raw == 0u);
      CXET_CHECK(hft_backtest::strategy_runtime_detail::allocateBacktestServiceClientIntentId(*engine).raw == 0u);
    }},
    cxet::testing::Case{"simulation_evidence_capacity_refuses_without_terminal_eviction", +[] {
      auto engine = std::make_unique<detail::EngineState>(); engine->activeContextIndex = 1u;
      CXET_CHECK(detail::prepareSimulationOrderEvidence(*engine));
      trading_core::OrderIntent intent; intent.orderType = canon::OrderType::Limit;
      intent.limitTimeInForcePolicy = trading_core::LimitTimeInForcePolicy::Ioc;
      intent.side = Side::Buy(); intent.qty.raw = 4; intent.price.raw = 100;
      for (std::size_t i = 1u; i <= detail::kSimulationOrderEvidenceCapacity + 1u; ++i) {
        intent.clientIntentId.raw = i;
        CXET_CHECK(!detail::execution_detail::submitOrderIntent(*engine, intent, i, i + 1));
        if (engine->simulationEvidenceFailed) break;
      }
      CXET_CHECK(engine->simulationEvidenceFailed);
      const auto view = detail::simulationOrderEvidenceView(*engine);
      const auto* first = trading_core::strategy::canonicalOrderEvidence(view, Id{1u}, 1u);
      CXET_CHECK(first && first->terminal() && first->request.identity.owner.strategyInstanceId == 2u);
    }},
    cxet::testing::Case{"service_feedback_updates_canonical_store_without_strategy_delivery", +[] {
      auto engine = std::make_unique<detail::EngineState>(); engine->activeContextIndex = 1u;
      hft_backtest::BacktestRateLimitSchedule schedule; schedule.enabled = false;
      std::string quotaError;
      CXET_CHECK(hft_backtest::applyRateLimitSchedule(*engine, schedule, quotaError));
      CXET_CHECK(detail::prepareSimulationOrderEvidence(*engine));
      trading_core::OrderIntent intent; intent.clientIntentId.raw = 31u; intent.service = true;
      intent.orderType = canon::OrderType::Limit; intent.side = Side::Buy(); intent.qty.raw = 4; intent.price.raw = 100;
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*engine, intent, 10, 20));
      detail::applyDueActions(*engine, 20);
      CXET_CHECK(engine->userEvents.size() == 1u && engine->userEventServiceFlags[0] == 1u);
      const auto view = detail::simulationOrderEvidenceView(*engine);
      const auto* record = trading_core::strategy::canonicalOrderEvidence(view, intent.clientIntentId, 1u);
      CXET_CHECK(record && !record->venueObserved);
      CXET_CHECK(detail::deliverSimulationOrderEvent(*engine, 0u, engine->userEventDeliveryTsNs[0]));
      CXET_CHECK(record->venueObserved && !record->terminal());
      trading_core::StrategyContext ctx; ctx.hasUserEvent = true; ctx.userEvent = &engine->userEvents[0];
      hft_backtest::bindBacktestUserEventIdentity(ctx, *engine);
      CXET_CHECK(ctx.userEventGeneration == 0u && ctx.userEventClientIntentId.raw == 0u);
    }},
    cxet::testing::Case{"simulated_cancel_terminal_evidence_is_final_only_at_private_delivery", +[] {
      auto engine = std::make_unique<detail::EngineState>();
      hft_backtest::BacktestRateLimitSchedule schedule; schedule.enabled = false;
      std::string quotaError;
      CXET_CHECK(hft_backtest::applyRateLimitSchedule(*engine, schedule, quotaError));
      engine->activeContextIndex = 1u;
      CXET_CHECK(detail::prepareSimulationOrderEvidence(*engine));
      detail::BacktestLatencyConfig latency; latency.userData = {1u, 0u};
      detail::initLatencyRuntime(engine->latency, latency);
      trading_core::OrderIntent intent; intent.clientIntentId.raw = 41u;
      intent.orderType = canon::OrderType::Limit; intent.side = Side::Buy();
      intent.qty.raw = 4; intent.price.raw = 100;
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*engine, intent, 10, 20));
      auto view = detail::simulationOrderEvidenceView(*engine);
      const auto* placed = trading_core::strategy::canonicalOrderEvidence(view, intent.clientIntentId, 1u);
      CXET_CHECK(placed && placed->placeSettled && !placed->venueObserved && !placed->terminal());
      detail::applyDueActions(*engine, 20);
      CXET_CHECK(engine->userEvents.size() == 1u);
      CXET_CHECK(!placed->venueObserved);
      CXET_CHECK(detail::deliverSimulationOrderEvent(*engine, 0u, engine->userEventDeliveryTsNs[0]));
      CXET_CHECK(placed->venueObserved && !placed->terminal());
      trading_core::CancelIntent cancel; cancel.clientIntentId.raw = 42u; cancel.targetClientIntentId = intent.clientIntentId;
      CXET_CHECK(detail::execution_detail::submitCancelIntent(*engine, cancel, 1030, 1040));
      detail::applyDueActions(*engine, 1040);
      CXET_CHECK(engine->userEvents.size() == 2u && !placed->terminal());
      CXET_CHECK(!placed->terminal());
      CXET_CHECK(detail::deliverSimulationOrderEvent(*engine, 1u, engine->userEventDeliveryTsNs[1]));
      CXET_CHECK(placed->terminal());
    }},
    cxet::testing::Case{"simulated_evidence_refuses_foreign_context_source_owner_and_generation", +[] {
      auto engine = std::make_unique<detail::EngineState>();
      hft_backtest::BacktestRateLimitSchedule schedule; schedule.enabled = false;
      std::string quotaError;
      CXET_CHECK(hft_backtest::applyRateLimitSchedule(*engine, schedule, quotaError));
      auto foreign = std::make_unique<detail::EngineState>(); foreign->legIndex = 1u;
      engine->activeContextIndex = foreign->activeContextIndex = 1u;
      CXET_CHECK(detail::prepareSimulationOrderEvidence(*engine) && detail::prepareSimulationOrderEvidence(*foreign));
      trading_core::OrderIntent intent; intent.clientIntentId.raw = 77u;
      intent.orderType = canon::OrderType::Limit; intent.side = Side::Buy(); intent.qty.raw = 4; intent.price.raw = 100;
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*engine, intent, 10, 20));
      auto view = detail::simulationOrderEvidenceView(*engine);
      CXET_CHECK(trading_core::strategy::canonicalOrderEvidence(view, intent.clientIntentId, 1u));
      auto otherContext = view; otherContext.contextIndex = 2u;
      CXET_CHECK(!trading_core::strategy::canonicalOrderEvidence(otherContext, intent.clientIntentId, 1u));
      auto otherOwner = view; ++otherOwner.owner.strategyInstanceId;
      CXET_CHECK(!trading_core::strategy::canonicalOrderEvidence(otherOwner, intent.clientIntentId, 1u));
      CXET_CHECK(!trading_core::strategy::canonicalOrderEvidence(view, intent.clientIntentId, 2u));
      auto otherSource = detail::simulationOrderEvidenceView(*foreign);
      otherSource.rows = view.rows; otherSource.binding = view.binding;
      CXET_CHECK(!trading_core::strategy::canonicalOrderEvidence(otherSource, intent.clientIntentId, 1u));
    }},
    cxet::testing::Case{"captured_canonical_and_executable_steps_do_not_use_native_contract_lot", +[] {
      QuantFixture fixture(0, true);
      detail::SessionLoadPlan plan; plan.loadTrades = false; plan.requireBookTickers = true;
      const std::array<hft_backtest::BacktestCorpusSourceRequest, 1> requests{
          hft_backtest::BacktestCorpusSourceRequest{"binance", "spot", "BTC_USDT", 100, 10'000}};
      detail::BinaryCorpusStream stream; std::string error;
      CXET_CHECK(stream.open(fixture.root, requests, plan, error) == hft_backtest::Status::Ok);
      CXET_CHECK(stream.sessions()[0].instrument.lotSizeE8 == 2);
      CXET_CHECK(stream.sessions()[0].instrument.executionQuantityStepE8 == 4);
      trading_core::StrategyInstrumentSpec instrument{};
      CXET_CHECK(hft_backtest::prepareBacktestStrategyInstrument(stream.sessions()[0].instrument, instrument, error) == hft_backtest::Status::Ok);
      CXET_CHECK(instrument.stepSizeRaw == 2 && instrument.executionQuantityStepRaw == 4);
    }},
    cxet::testing::Case{"native_lot_without_execution_quantity_authority_refuses_before_strategy_init", +[] {
      QuantFixture fixture; const auto result = fixture.load();
      CXET_CHECK(result.status == hft_backtest::Status::Ok);
      trading_core::StrategyInstrumentSpec instrument{}; std::string error;
      CXET_CHECK(hft_backtest::prepareBacktestStrategyInstrument(result.session.instrument, instrument, error) == hft_backtest::Status::Unsupported);
      CXET_CHECK(error.find("execution quantity") != std::string::npos);
    }},
    cxet::testing::Case{"nonunit_native_price_basis_refuses_e8_accounting_before_admission", +[] {
      QuantFixture fixture(200'000'000, true); const auto result = fixture.load();
      CXET_CHECK(result.status == hft_backtest::Status::Ok);
      trading_core::StrategyInstrumentSpec instrument{}; std::string error;
      CXET_CHECK(hft_backtest::prepareBacktestStrategyInstrument(result.session.instrument, instrument, error) == hft_backtest::Status::Unsupported);
      CXET_CHECK(error.find("price basis") != std::string::npos);
    }},
    cxet::testing::Case{"user_data_jitter_keeps_source_order_and_deterministic_ties", +[] {
      auto first = std::make_unique<detail::EngineState>();
      auto repeat = std::make_unique<detail::EngineState>();
      detail::BacktestLatencyConfig config; config.seed = 7u; config.userData = {1000u, 1000u};
      detail::initLatencyRuntime(first->latency, config);
      detail::initLatencyRuntime(repeat->latency, config);
      for (std::uint64_t i = 0u; i < 128u; ++i) {
        cxet::composite::UserDataEvent event{}; event.eventId.raw = i + 1u;
        event.eventTime.raw = event.updateTime.raw = 100u + i;
        Id intent{}; intent.raw = i + 11u;
        detail::execution_detail::pushUserEvent(*first, event, 100 + i, intent, 1u);
        detail::execution_detail::pushUserEvent(*repeat, event, 100 + i, intent, 1u);
        CXET_CHECK(first->userEventDeliveryTsNs[i] == repeat->userEventDeliveryTsNs[i]);
        if (i != 0u) CXET_CHECK(first->userEventDeliveryTsNs[i] >= first->userEventDeliveryTsNs[i - 1u]);
        CXET_CHECK(first->userEvents[i].eventId.raw == i + 1u && first->userEvents[i].updateTime.raw == 100u + i);
        CXET_CHECK(first->userEventClientIntentIds[i].raw == i + 11u && first->userEventContextIndices[i] == 1u);
      }
    }},
};
}
int main(int argc, char** argv) { return cxet::testing::runCases(argc, argv, cases); }
