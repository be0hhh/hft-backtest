#include "hft_backtest/Backtest.hpp"

#include "../Engine/BacktestEngine.hpp"
#include "Internal/BacktestRunContext.hpp"
#include "Internal/BacktestRunHelpers.hpp"
#include "../Engine/BacktestEngineSetup.hpp"
#include "../Execution/Model/BacktestExecutionDepth.hpp"
#include "../Accounting/BacktestResultAggregation.hpp"
#include "../Config/BacktestRunConfig.hpp"
#include "../Config/Strategy/BacktestStrategyConfig.hpp"
#include "../Strategy/Inputs/BacktestStrategyWarmup.hpp"
#include "../Strategy/BacktestStrategyRuntime.hpp"
#include "../Strategy/Diagnostics/BacktestStrategyTrace.hpp"
#include "../Input/Binary/BinaryCorpusLoader.hpp"
#include "../Input/Session/SessionLoader.hpp"
#include "BacktestRunError.hpp"

#include "trading_core/Contracts/Control/ControlState.hpp"
#include "trading_core/Algorithm/Market/LocalOrderBook.hpp"
#include "trading_core/Contracts/Strategy/StrategyContract.hpp"
#include "trading_core/Execution/PortfolioRisk.hpp"
#include "trading_core/Strategy/GeneratedStrategyRegistry.hpp"
#include "trading_core/Strategy/StrategyMarketData.hpp"

#include <array>
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hft_backtest {
using trading_core::StrategyDescriptor;
using trading_core::StrategyParamStorage;
using run_detail::BacktestArtifactContext;
using run_detail::BacktestReplayContext;
using run_detail::BacktestRunContext;
using run_detail::DeliveryEventRef;
using run_detail::deliveryEventAt;
using run_detail::elapsedNs;
using run_detail::finalizeBacktestRun;
using run_detail::includeTimelineClock;
using run_detail::loadBacktestRunSessions;
using run_detail::mergeReplayClockEvidence;
using run_detail::prepareBacktestDeliveryReferences;
using run_detail::prepareBacktestDepthProviders;
using run_detail::prepareBacktestRunTimeline;
using run_detail::prepareEngineOrderBookStorage;
using run_detail::replayBacktestRun;
using run_detail::reportReplayProgress;
using run_detail::seedLoadedLegSummaries;
using run_detail::seedRequestedLegSummaries;
using run_detail::summarizeLiveProbeConfig;
using run_detail::usesBinaryCorpus;
using run_detail::writeCurrentArtifacts;
using run_detail::writeEmptyArtifacts;


extern "C" HFT_BACKTEST_API std::uint32_t hft_backtest_api_version() noexcept {
  return kBacktestApiVersion;
}
BacktestRunResult runBacktest(const BacktestRunRequest& request,
                              BacktestProgressCallback progressCallback,
                              void* userData) noexcept {
  BacktestRunResult result{};
  const auto totalStart = std::chrono::steady_clock::now();
  BacktestArtifactContext artifacts{result, request, totalStart};

  result.runId = request.runId.empty() ? makeRunId() : request.runId;
  result.requestId = request.requestId.empty() ? result.runId : request.requestId;
  result.sessionPath = usesBinaryCorpus(request)
      ? request.corpusPath
      : request.sessionPath;
  result.configPath = request.configPath;
  result.resultPath = resultPathFor(request, result.runId);
  result.tradeMode = request.tradeMode;
  result.primaryLegIndex = request.hasPrimaryLegIndex ? request.primaryLegIndex : 0u;
  result.hasPrimaryLegIndex = request.hasPrimaryLegIndex;
  result.rateLimitsEnabled = request.rateLimitsEnabled;
  result.strictRateLimitRejects = request.strictRateLimitRejects;
  const bool binaryCorpus = usesBinaryCorpus(request);
  const bool recorderSessions =
      !request.sessionPath.empty() || !request.sessions.empty();
  const bool hasExplicitSessions = binaryCorpus
      ? request.corpusSources.size() > 1u
      : !request.sessions.empty();
  const std::size_t requestedSessionCount = binaryCorpus
      ? request.corpusSources.size()
      : explicitSessionPathCount(request, hasExplicitSessions);
  const std::int64_t portfolioInitialBalance = portfolioInitialBalanceE8(request, requestedSessionCount);
  BacktestRunRequest effectiveRequest = request;
  effectiveRequest.initialBalanceE8 = portfolioInitialBalance;
  if (effectiveRequest.latencySeed == 0u) effectiveRequest.latencySeed = hashRunSeed(result.runId);
  const detail::BacktestLatencyConfig latencyConfig = detail::latencyConfigFromRequest(effectiveRequest);
  result.latencySeed = latencyConfig.seed;
  result.marketOrderLatency = latencyConfig.marketOrder;
  result.limitOrderLatency = latencyConfig.limitOrder;
  result.cancelOrderLatency = latencyConfig.cancelOrder;
  result.userDataLatency = latencyConfig.userData;
  result.orderLatencyUs = request.orderLatencyUs;
  result.cancelLatencyUs = latencyConfig.cancelOrder.baseUs;
  result.initialBalanceE8 = portfolioInitialBalance;
  result.walletBalanceE8 = portfolioInitialBalance;




  if ((binaryCorpus && recorderSessions) ||
      (binaryCorpus &&
       (request.corpusPath.empty() || request.corpusSources.empty())) ||
      (!binaryCorpus && !recorderSessions) || request.configPath.empty()) {
    result.status = Status::InvalidArgument;
    result.error = "select either recorder sessions or a corpus path with source selections, plus config_path";
    return result;
  }
  if (requestedSessionCount > trading_core::kStrategyRuntimeMaxLegs) {
    result.status = Status::InvalidArgument;
    result.error = "too many backtest market legs (max " +
        std::to_string(trading_core::kStrategyRuntimeMaxLegs) + ")";
    return result;
  }
  if (request.hasPrimaryLegIndex && request.primaryLegIndex >= requestedSessionCount) {
    result.status = Status::InvalidArgument;
    result.error = "primary leg index " + std::to_string(request.primaryLegIndex) +
                   " is outside selected leg count " + std::to_string(requestedSessionCount);
    return result;
  }
  if (!request.outputPath.empty() && request.outputPath.extension() == ".json") {
    result.status = Status::InvalidArgument;
    result.error = "output_path must be a run directory, not a .json file";
    return result;
  }
  seedRequestedLegSummaries(result, request, hasExplicitSessions);

  std::string error;
  BacktestStrategyConfig cfg{};
  if (!loadBacktestStrategyConfig(
          request.configPath.string().c_str(), cfg, error)) {
    result.status = Status::InvalidArgument;
    result.error = error;
    (void)writeEmptyArtifacts(artifacts);
    return result;
  }
  result.liveProbe = summarizeLiveProbeConfig(cfg);
  result.strategy = request.strategy.empty() ? cfg.strategyType : request.strategy;
  StrategyIndicatorProfileInfo indicatorProfile{};
  if (!resolveStrategyIndicatorProfile(request.indicatorProfile, indicatorProfile)) {
    result.status = Status::InvalidArgument;
    result.error = "unknown indicator profile";
    (void)writeEmptyArtifacts(artifacts);
    return result;
  }
  result.indicatorProfile = std::string{indicatorProfile.id};
  if (!strategyDeclaresIndicator(result.strategy, result.indicatorProfile)) {
    result.status = Status::InvalidArgument;
    result.error = "indicator profile is not supported by strategy";
    (void)writeEmptyArtifacts(artifacts);
    return result;
  }
  if (result.strategy != cfg.strategyType) {
    result.status = Status::ConfigMismatch;
    result.error = "requested strategy does not match config [strategy] type";
    (void)writeEmptyArtifacts(artifacts);
    return result;
  }
  const StrategyDescriptor* descriptor = trading_core::findStrategyDescriptor(cfg.strategyType);
  if (!descriptor || !descriptor->resolveParams || !descriptor->initState || !descriptor->runCycle) {
    result.status = Status::Unsupported;
    result.error = "strategy descriptor is missing required callbacks";
    (void)writeEmptyArtifacts(artifacts);
    return result;
  }

  detail::SessionLoadPlan loadPlan{};
  if (!buildSessionLoadPlan(*descriptor, loadPlan, error)) {
    result.status = Status::Unsupported;
    result.error = error;
    (void)writeEmptyArtifacts(artifacts);
    return result;
  }
  // Prepared JSON-session caches are path keyed and intentionally do not own
  // corpus source/time selections. Corpus runs load their exact interval for
  // every point until that cache grows a selection-aware contract.
  detail::PreparedBacktestInput* preparedInput = binaryCorpus
      ? nullptr
      : detail::activePreparedBacktestInput();
  const auto loadStart = std::chrono::steady_clock::now();
  const bool usePreparedSessions = preparedInput != nullptr &&
                                   preparedMatches(*preparedInput, request, hasExplicitSessions, loadPlan);
  std::vector<detail::SessionData> loadedSessions{};
  const std::vector<detail::SessionData>* sessionsView = nullptr;
  loadBacktestRunSessions(request, binaryCorpus, hasExplicitSessions, usePreparedSessions,
      preparedInput, loadPlan, loadedSessions, sessionsView, result, error);
  if (sessionsView != nullptr) appendSessionWarnings(result, *sessionsView);
  result.performance.loadNs = elapsedNs(loadStart);
  if (!isOk(result.status)) {
    result.error = error;
    (void)writeEmptyArtifacts(artifacts);
    return result;
  }
  if (sessionsView == nullptr || sessionsView->empty()) {
    result.status = Status::CorruptData;
    result.error = "backtest session cache is empty";
    (void)writeEmptyArtifacts(artifacts);
    return result;
  }
  const std::vector<detail::SessionData>& sessions = *sessionsView;
  const detail::SessionData& session = sessions[0];
  result.sessionPath = session.sessionPath;
  if (hasExplicitSessions) seedLoadedLegSummaries(result, request, sessions);

  const ExchangeId sessionExchange = detail::exchangeFromText(session.exchange);
  const canon::MarketType sessionMarket = detail::marketFromText(session.market);
  const BacktestStrategyVenueConfig* venue = nullptr;
  Symbol selectedSymbol{};
  bool symbolMatched = false;
  for (const auto& candidate : cfg.venues) {
    if (candidate.exchange.raw != sessionExchange.raw || candidate.market.raw != sessionMarket.raw) continue;
    for (const Symbol& symbol : candidate.symbols) {
      if (session.symbol != symbol.data) continue;
      venue = &candidate;
      selectedSymbol = symbol;
      symbolMatched = true;
      break;
    }
    if (symbolMatched) break;
  }
  if (!venue) {
    result.status = Status::ConfigMismatch;
    result.error = "config venue/symbol does not match selected market source";
    (void)writeEmptyArtifacts(artifacts);
    return result;
  }

  StrategyParamStorage params{};
  if (!descriptor->resolveParams(
          makeBacktestStrategyParamContext(cfg, *venue), params, error)) {
    result.status = Status::InvalidArgument;
    result.error = error;
    (void)writeEmptyArtifacts(artifacts);
    return result;
  }
  const BacktestStrategyInputPolicy inputPolicy =
      resolveBacktestStrategyInputPolicy(*descriptor, params);
  StrategyInstrumentSpec instrument{};
  instrument.tickSizeRaw = session.instrument.tickSizeE8;
  instrument.stepSizeRaw = session.instrument.lotSizeE8;
  instrument.contractBaseQtyRaw = session.instrument.contractBaseQtyE8;
  instrument.priceBasisQtyRaw = session.instrument.priceBasisQtyE8;
  instrument.expiryUtcNs = static_cast<std::uint64_t>(session.instrument.expiryUtcNs > 0 ? session.instrument.expiryUtcNs : 0);
  (void)trading_core::prepareStrategyInstrumentSpecDerived(instrument);
  std::vector<StrategyInstrumentSpec> legInstruments;
  std::vector<Symbol> legSymbols;
  if (hasExplicitSessions) {
    legInstruments.resize(sessions.size());
    legSymbols.resize(sessions.size());
    for (std::size_t leg = 0; leg < sessions.size(); ++leg) {
      legInstruments[leg].tickSizeRaw = sessions[leg].instrument.tickSizeE8;
      legInstruments[leg].stepSizeRaw = sessions[leg].instrument.lotSizeE8;
      legInstruments[leg].contractBaseQtyRaw = sessions[leg].instrument.contractBaseQtyE8;
      legInstruments[leg].priceBasisQtyRaw = sessions[leg].instrument.priceBasisQtyE8;
      legInstruments[leg].expiryUtcNs = static_cast<std::uint64_t>(sessions[leg].instrument.expiryUtcNs > 0 ? sessions[leg].instrument.expiryUtcNs : 0);
      (void)trading_core::prepareStrategyInstrumentSpecDerived(
          legInstruments[leg]);
      legSymbols[leg].copyFrom(sessions[leg].symbol.c_str());
    }
  }

  alignas(trading_core::kStrategyRuntimeStateAlign) std::array<unsigned char, trading_core::kStrategyRuntimeStateBytes> state{};
  if (!descriptor->initState(state.data(), params, instrument)) {
    result.status = Status::InvalidArgument;
    result.error = "strategy initState failed";
    (void)writeEmptyArtifacts(artifacts);
    return result;
  }
  BacktestInputCoverage seedCoverage{};
  if (descriptor->seedHistory) {
    if (session.candles.empty()) {
      result.status = Status::CorruptData;
      result.error = "strategy requires candle history but candles channel is missing or empty";
      (void)writeEmptyArtifacts(artifacts);
      return result;
    }
    cxet::composite::TieredCandleHistory history{};
    if (!detail::buildTieredCandleHistory(session.candles, history)) {
      result.status = Status::CorruptData;
      result.error = "failed to build strategy candle history from recorder session";
      (void)writeEmptyArtifacts(artifacts);
      return result;
    }
    seedCoverage.seedM1Candles = history.m1Count.raw;
    seedCoverage.seedM15Candles = history.m15Count.raw;
    seedCoverage.seedD1Candles = history.d1Count.raw;
    trading_core::StrategyHistorySeedStats stats{};
    if (!descriptor->seedHistory(state.data(), history, &stats)) {
      result.status = Status::InvalidArgument;
      result.error = "strategy seedHistory failed";
      (void)writeEmptyArtifacts(artifacts);
      return result;
    }
  }
  if (!seedTradeWarmupFromSession(*descriptor, state.data(), params, session, error)) {
    result.status = Status::CorruptData;
    result.error = error;
    (void)writeEmptyArtifacts(artifacts);
    return result;
  }
  seedCoverage.seedTradeRows = session.tradeWarmup.size();

  std::unique_ptr<detail::EngineState> engineStorage;
  try {
    engineStorage = std::make_unique<detail::EngineState>();
  } catch (...) {
    result.status = Status::IoError;
    result.error = "failed to allocate backtest engine state";
    (void)writeEmptyArtifacts(artifacts);
    return result;
  }
  detail::EngineState& engine = *engineStorage;
  std::unique_ptr<detail::EngineState> aggregateEngineStorage;
  const detail::EngineState* artifactEngine = &engine;

  if (!prepareEngineOrderBookStorage(engine, descriptor, loadPlan)) {
    result.status = Status::IoError;
    result.error = "failed to allocate exact backtest order-book storage";
    (void)writeEmptyArtifacts(artifacts);
    return result;
  }
  configureStrategyIndicator(engine, indicatorProfile);
  detail::initLatencyRuntime(engine.latency, resolveLatencyConfig(effectiveRequest, session.exchange, session.market));
  engine.inputCoverage = seedCoverage;
  engine.exchange = sessionExchange;
  engine.market = sessionMarket;
  engine.symbol = selectedSymbol;
  engine.tickSizeE8 = instrument.tickSizeRaw;
  Price reconstructionTick{};
  reconstructionTick.raw = instrument.tickSizeRaw;
  (void)trading_core::configureBboReconstruction(
      engine.bboReconstruction, reconstructionTick);
  (void)trading_core::configureBboReconstruction(
      engine.venueBboReconstruction, reconstructionTick);
  engine.makerFillMinCrossTicks = effectiveRequest.makerFillMinCrossTicks;
  detail::initializeAccounting(engine, request);
  const BacktestFeeSchedule primaryFee = resolveFeeSchedule(request, session.exchange, session.market);
  engine.makerFeeBpsE8 = primaryFee.makerFeeBpsE8;
  engine.takerFeeBpsE8 = primaryFee.takerFeeBpsE8;
  {
    std::string rateLimitError;
    if (!applyRateLimitSchedule(engine,
                                resolveRateLimitSchedule(effectiveRequest, session.exchange, session.market),
                                rateLimitError)) {
      result.status = Status::InvalidArgument;
      result.error = std::move(rateLimitError);
      (void)writeEmptyArtifacts(artifacts);
      return result;
    }
  }
  std::vector<detail::EngineState> legEngines(
      hasExplicitSessions ? sessions.size() : 0u);
  if (hasExplicitSessions) {
    for (std::size_t leg = 0; leg < sessions.size(); ++leg) {
      detail::EngineState& legEngine = legEngines[leg];
      if (!prepareEngineOrderBookStorage(legEngine, descriptor, loadPlan)) {
        result.status = Status::IoError;
        result.error =
            "failed to allocate exact leg order-book storage";
        (void)writeEmptyArtifacts(artifacts);
        return result;
      }
      configureStrategyIndicator(legEngine, indicatorProfile);
      const detail::BacktestLatencyConfig legLatency =
          latencyConfigForLeg(effectiveRequest, sessions[leg].exchange, sessions[leg].market, leg);
      detail::initLatencyRuntime(legEngine.latency, legLatency);
      legEngine.inputCoverage = seedCoverage;
      legEngine.legIndex = static_cast<std::uint32_t>(leg);
      legEngine.exchange = detail::exchangeFromText(sessions[leg].exchange);
      legEngine.market = detail::marketFromText(sessions[leg].market);
      legEngine.symbol.copyFrom(sessions[leg].symbol.c_str());
      legEngine.tickSizeE8 = legInstruments[leg].tickSizeRaw;
      Price legReconstructionTick{};
      legReconstructionTick.raw = legInstruments[leg].tickSizeRaw;
      (void)trading_core::configureBboReconstruction(
          legEngine.bboReconstruction, legReconstructionTick);
      (void)trading_core::configureBboReconstruction(
          legEngine.venueBboReconstruction, legReconstructionTick);
      legEngine.makerFillMinCrossTicks = effectiveRequest.makerFillMinCrossTicks;
      detail::initializeAccounting(legEngine, request);
      const std::int64_t legBalance = legInitialBalanceE8(request, leg);
      legEngine.initialBalanceE8 = legBalance;
      legEngine.walletBalanceE8 = legBalance;
      const BacktestFeeSchedule fee = resolveFeeSchedule(request, sessions[leg].exchange, sessions[leg].market);
      legEngine.makerFeeBpsE8 = fee.makerFeeBpsE8;
      legEngine.takerFeeBpsE8 = fee.takerFeeBpsE8;
      std::string rateLimitError;
      if (!applyRateLimitSchedule(legEngine,
                                  resolveRateLimitSchedule(effectiveRequest, sessions[leg].exchange, sessions[leg].market),
                                  rateLimitError)) {
        result.status = Status::InvalidArgument;
        result.error = std::move(rateLimitError);
        (void)writeEmptyArtifacts(artifacts);
        return result;
      }
    }
  }
  if (loadPlan.loadDepths) {
    if (hasExplicitSessions) {
      for (std::size_t leg = 0; leg < sessions.size() && leg < legEngines.size(); ++leg) {
        legEngines[leg].venueOrderBookExecutionEnabled = !sessions[leg].depths.empty();
        if (legEngines[leg].venueOrderBookExecutionEnabled) {
          legEngines[leg].depthExecutionMode = BacktestDepthExecutionMode::Depth;
        }
      }
    } else {
      engine.venueOrderBookExecutionEnabled = !session.depths.empty();
      if (engine.venueOrderBookExecutionEnabled) engine.depthExecutionMode = BacktestDepthExecutionMode::Depth;
    }
  }

  std::vector<std::unique_ptr<detail::ExecutionDepthProvider>> executionDepthProviders{};
  prepareBacktestDepthProviders(executionDepthProviders, preparedInput, loadPlan,
      binaryCorpus, hasExplicitSessions, sessions, session, legEngines, engine, instrument, selectedSymbol);
  trading_core::SymbolShardConfig shard{};
  shard.shardId.raw = 1;
  shard.strategyId.raw = 1;
  shard.symbolId.raw = 1;
  shard.symbol = selectedSymbol;
  trading_core::ShardControlState control{};
  control.shardId = shard.shardId;

  std::vector<detail::TimelineEvent> loadedTimeline;
  const auto timelineStart = std::chrono::steady_clock::now();
  const std::vector<detail::TimelineEvent>* timelineView = nullptr;
  prepareBacktestRunTimeline(usePreparedSessions, preparedInput, request, hasExplicitSessions,
      loadPlan, sessions, session, loadedTimeline, timelineView);
  const std::vector<detail::TimelineEvent>& timeline = *timelineView;
  result.performance.timelineNs = elapsedNs(timelineStart);
  const auto deliveryStart = std::chrono::steady_clock::now();
  std::vector<DeliveryEventRef> deliveryRefs{};
  prepareBacktestDeliveryReferences(deliveryRefs, timeline);
  const std::size_t deliveryEventCount = deliveryRefs.size();

  result.performance.deliveryNs = elapsedNs(deliveryStart);
  result.trades = 0;
  result.bookTickers = 0;
  result.depths = 0;
  result.firstTsNs = std::numeric_limits<std::int64_t>::max();
  result.lastTsNs = 0;
  for (const detail::SessionData& loadedSession : sessions) {
    result.trades += loadedSession.trades.size();
    result.bookTickers += loadedSession.bookTickers.size();
    result.depths += loadedSession.depths.size();
    if (loadedSession.firstTsNs > 0 && loadedSession.firstTsNs < result.firstTsNs) result.firstTsNs = loadedSession.firstTsNs;
    if (loadedSession.lastTsNs > result.lastTsNs) result.lastTsNs = loadedSession.lastTsNs;
    mergeReplayClockEvidence(result.replayClock, loadedSession);
  }
  if (result.firstTsNs == std::numeric_limits<std::int64_t>::max()) result.firstTsNs = session.firstTsNs;
  result.events = timeline.size();
  for (const detail::TimelineEvent& event : timeline) {
    includeTimelineClock(result.replayClock, event);
    detail::EngineState* eventEngine = &engine;
    if (hasExplicitSessions && event.legIndex < legEngines.size())
      eventEngine = &legEngines[event.legIndex];
    includeTimelineClock(*eventEngine, event);
    if (event.venueScheduleAdjusted) {
      ++result.replayClock.venueScheduleAdjustedEvents;
      ++eventEngine->venueScheduleAdjustedEvents;
    }
  }
  std::int64_t replayStartTsNs = result.firstTsNs;
  std::int64_t replayEndEstimateNs = result.lastTsNs;
  if (!timeline.empty()) {
    replayStartTsNs = timeline.front().tsNs;
    replayEndEstimateNs = timeline.back().tsNs;
  }
  if (deliveryEventCount != 0u) {
    const std::int64_t firstDeliveryNs = deliveryEventAt(timeline, deliveryRefs, 0u).deliveryTsNs;
    const std::int64_t lastDeliveryNs =
        deliveryEventAt(timeline, deliveryRefs, deliveryEventCount - 1u).deliveryTsNs;
    if (replayStartTsNs <= 0 ||
        (firstDeliveryNs > 0 && firstDeliveryNs < replayStartTsNs))
      replayStartTsNs = firstDeliveryNs;
    if (lastDeliveryNs > replayEndEstimateNs)
      replayEndEstimateNs = lastDeliveryNs;
  }
  std::int64_t greatestProcessedTsNs = replayStartTsNs > 0 ? replayStartTsNs : 0;

  const bool captureIndicatorTrace = !result.indicatorProfile.empty();
  const bool captureRangeTrace = emitsPriceRangeTrace(*descriptor);
  const bool captureSpreadDiagnostics = emitsArbSpreadTrace(*descriptor);
  const bool captureSpreadRows = request.captureStrategySpread && captureSpreadDiagnostics;
  const std::uint64_t replayDurationNs = replayEndEstimateNs > replayStartTsNs && replayStartTsNs >= 0
      ? static_cast<std::uint64_t>(replayEndEstimateNs) - static_cast<std::uint64_t>(replayStartTsNs)
      : 0u;
  if (hasExplicitSessions) {
    for (std::size_t leg = 0u; leg < legEngines.size(); ++leg) {
      const bool traceOwner = leg == 0u;
      reserveEngineStorage(legEngines[leg],
                           timeline.size(),
                           replayDurationNs,
                           traceOwner && captureIndicatorTrace,
                           traceOwner && captureRangeTrace,
                           traceOwner && captureSpreadDiagnostics);
    }
  } else {
    reserveEngineStorage(engine,
                         timeline.size(),
                         replayDurationNs,
                         captureIndicatorTrace,
                         captureRangeTrace,
                         captureSpreadDiagnostics);
  }
  if (hasExplicitSessions) {
    for (detail::EngineState& legEngine : legEngines) {
      detail::appendEquityPoint(legEngine, replayStartTsNs);
      detail::appendRiskSnapshot(legEngine, replayStartTsNs, detail::kRiskSnapshotReasonStart);
    }
  } else {
    detail::appendEquityPoint(engine, replayStartTsNs);
    detail::appendRiskSnapshot(engine, replayStartTsNs, detail::kRiskSnapshotReasonStart);
  }
  std::array<trading_core::StrategyAccountState, trading_core::kStrategyRuntimeMaxLegs> riskAccounts{};
  std::array<const BacktestStrategyVenueConfig*, trading_core::kStrategyRuntimeMaxLegs> riskVenues{};
  std::array<trading_core::PortfolioRiskLegConfig,
             trading_core::kStrategyRuntimeMaxLegs> riskLegConfigs{};
  const std::size_t riskAccountCount = hasExplicitSessions ? boundedRiskLegCount(legEngines.size()) : 1u;
  if (hasExplicitSessions) {
    refreshRiskAccountsFromEngines(legEngines, riskAccounts);
    resolveRiskVenuesForSessions(cfg, sessions, riskVenues);
  } else {
    riskAccounts[0] = accountStateFromEngine(engine);
    riskVenues[0] = venue;
  }
  for (std::size_t i = 0u; i < riskAccountCount; ++i) {
    riskLegConfigs[i] = makeBacktestPortfolioRiskLegConfig(riskVenues[i]);
  }
  trading_core::PortfolioRiskState traderRiskState =
      trading_core::makePortfolioRiskState(
          makeBacktestPortfolioRiskConfig(cfg.risk), riskAccounts.data(),
          riskAccountCount, riskLegConfigs.data());


  bool traderRiskCloseProblem = false;
  bool legLiquidationProblem = false;
  BacktestRunContext runContext{
      artifacts,
      result,
      request,
      effectiveRequest,
      progressCallback,
      userData,
      hasExplicitSessions,
      portfolioInitialBalance,
      sessions,
      session,
      descriptor,
      inputPolicy,
      instrument,
      selectedSymbol,
      legInstruments,
      legSymbols,
      state,
      engine,
      legEngines,
      aggregateEngineStorage,
      artifactEngine,
      shard,
      control,
      timeline,
      replayStartTsNs,
      greatestProcessedTsNs,
      riskAccounts,
      riskAccountCount,
      traderRiskState,
      traderRiskCloseProblem,
      legLiquidationProblem
  };





  const auto replayStart = std::chrono::steady_clock::now();
  if (!reportReplayProgress(runContext, 0, timeline.size())) {
    result.status = Status::Cancelled;
    result.error = "cancelled";
    (void)writeCurrentArtifacts(runContext);
    return result;
  }
  const std::size_t replayProgressStep = timeline.size() > 102400u
      ? std::max<std::size_t>(1024u, timeline.size() / 100u)
      : 1024u;
  std::size_t deliveryCursor = 0u;
  std::unique_ptr<SingleLegCycleScratch> singleScratchStorage{};
  SingleLegCycleScratch* singleScratch = nullptr;
  std::unique_ptr<MultiLegCycleScratch> multiScratchStorage{};
  MultiLegCycleScratch* multiScratch = nullptr;
  if (!hasExplicitSessions) {
    try {
      singleScratchStorage = std::make_unique<SingleLegCycleScratch>();
    } catch (...) {
      result.status = Status::IoError;
      result.error = "failed to allocate single-leg cycle scratch";
      (void)writeCurrentArtifacts(runContext);
      return result;
    }
    singleScratch = singleScratchStorage.get();
  } else {
    try {
      multiScratchStorage = std::make_unique<MultiLegCycleScratch>();
    } catch (...) {
      result.status = Status::IoError;
      result.error = "failed to allocate multi-leg cycle scratch";
      (void)writeCurrentArtifacts(runContext);
      return result;
    }
    multiScratch = multiScratchStorage.get();
    multiScratch->reset(legEngines.size() < trading_core::kStrategyRuntimeMaxLegs ? legEngines.size() : trading_core::kStrategyRuntimeMaxLegs);
  }

  bool replayStopped = false;
  BacktestReplayContext replayContext{
      runContext,
      deliveryRefs,
      deliveryEventCount,
      deliveryCursor,
      singleScratch,
      multiScratch,
      replayStopped,
      replayProgressStep
  };

  if (!replayBacktestRun(replayContext)) return result;
  result.performance.replayNs = elapsedNs(replayStart);
  finalizeBacktestRun(runContext);
  return result;
}

}  // namespace hft_backtest
