#include "hft_backtest/backtest.hpp"

#include "engine/backtest_engine.hpp"
#include "engine/backtest_engine_setup.hpp"
#include "execution/backtest_execution_depth.hpp"
#include "execution/backtest_execution_reprice.hpp"
#include "accounting/backtest_result_aggregation.hpp"
#include "config/backtest_run_config.hpp"
#include "strategy/backtest_strategy_warmup.hpp"
#include "strategy/backtest_strategy_runtime.hpp"
#include "strategy/backtest_strategy_trace.hpp"
#include "input/session_loader.hpp"

#include "hft_trader/core/control/ControlState.hpp"
#include "hft_trader/core/market/LocalOrderBook.hpp"
#include "hft_trader/core/strategy/StrategyContract.hpp"
#include "hft_trader/runtime/GeneratedStrategyRegistry.hpp"
#include "hft_trader/runtime/config/RuntimeConfig.hpp"
#include "hft_trader/runtime/risk/PortfolioRiskRuntime.hpp"
#include "hft_trader/runtime/runner/StrategyMarketDataRuntime.hpp"
#include "primitives/composite/OrderBookTapeRuntimeV1.hpp"

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
namespace {

using hft_trader::runtime::StrategyDescriptor;
using hft_trader::runtime::StrategyParamStorage;

bool reportProgress(std::uint64_t done, std::uint64_t total, std::string stage, BacktestProgressCallback cb, void* userData) noexcept {
  if (!cb) return true;
  BacktestProgress progress{};
  progress.eventsDone = done;
  progress.eventsTotal = total;
  progress.percent = total == 0 ? 0 : static_cast<std::uint32_t>((done * 100u) / total);
  if (progress.percent > 100u) progress.percent = 100u;
  progress.stage = std::move(stage);
  return cb(progress, userData);
}

bool reportProgress(std::uint64_t done,
                    std::uint64_t total,
                    std::string stage,
                    std::uint32_t basePercent,
                    std::uint32_t spanPercent,
                    BacktestProgressCallback cb,
                    void* userData) noexcept {
  if (!cb) return true;
  BacktestProgress progress{};
  progress.eventsDone = done;
  progress.eventsTotal = total;
  const std::uint32_t rawPercent = total == 0 ? 0 : static_cast<std::uint32_t>((done * 100u) / total);
  const std::uint32_t clampedRaw = rawPercent > 100u ? 100u : rawPercent;
  progress.percent = basePercent + static_cast<std::uint32_t>((static_cast<std::uint64_t>(clampedRaw) * spanPercent) / 100u);
  if (progress.percent > 100u) progress.percent = 100u;
  progress.stage = std::move(stage);
  return cb(progress, userData);
}

void prefetchRead(const void* ptr) noexcept {
#if defined(__GNUC__) || defined(__clang__)
  __builtin_prefetch(ptr, 0, 1);
#else
  (void)ptr;
#endif
}

std::size_t requestedLegCount(const BacktestRunRequest& request) noexcept {
  return request.sessions.size() + (request.sessionPath.empty() ? 0u : 1u);
}

void applyLegSummaryLatency(BacktestLegSummary& leg,
                            const detail::BacktestLatencyConfig& latency) noexcept {
  leg.marketDataLatency = latency.marketData;
  leg.marketOrderLatency = latency.marketOrder;
  leg.limitOrderLatency = latency.limitOrder;
  leg.cancelOrderLatency = latency.cancelOrder;
  leg.userDataLatency = latency.userData;
}

void seedRequestedLegSummaries(BacktestRunResult& result,
                               const BacktestRunRequest& request,
                               bool hasExplicitSessions) noexcept {
  if (!hasExplicitSessions) return;
  try {
    result.legs.clear();
    result.legs.reserve(requestedLegCount(request));
    auto appendLeg = [&](const std::filesystem::path& path, const BacktestSessionRequest* requested) {
      const std::size_t i = result.legs.size();
      BacktestLegSummary leg{};
      leg.sessionPath = path;
      if (requested != nullptr) leg.symbol = requested->symbol;
      applyLegSummaryLatency(leg, latencyConfigForLeg(request, leg.exchange, leg.market, i));
      leg.initialBalanceE8 = legInitialBalanceE8(request, i);
      leg.walletBalanceE8 = leg.initialBalanceE8;
      leg.strategyClosed = true;
      result.legs.push_back(std::move(leg));
    };
    if (!request.sessionPath.empty()) appendLeg(request.sessionPath, nullptr);
    for (const BacktestSessionRequest& session : request.sessions) {
      appendLeg(session.path, &session);
    }
  } catch (...) {
    result.legs.clear();
  }
}

void seedLoadedLegSummaries(BacktestRunResult& result,
                            const BacktestRunRequest& request,
                            const std::vector<detail::SessionData>& sessions) noexcept {
  try {
    result.legs.clear();
    result.legs.reserve(sessions.size());
    for (std::size_t i = 0u; i < sessions.size(); ++i) {
      const detail::SessionData& session = sessions[i];
      BacktestLegSummary leg{};
      leg.sessionPath = session.sessionPath;
      leg.exchange = session.exchange;
      leg.market = session.market;
      leg.symbol = session.symbol;
      const BacktestFeeSchedule fee = resolveFeeSchedule(request, session.exchange, session.market);
      leg.makerFeeBpsE8 = fee.makerFeeBpsE8;
      leg.takerFeeBpsE8 = fee.takerFeeBpsE8;
      applyLegSummaryLatency(leg, latencyConfigForLeg(request, session.exchange, session.market, i));
      leg.initialBalanceE8 = legInitialBalanceE8(request, i);
      leg.walletBalanceE8 = leg.initialBalanceE8;
      leg.strategyClosed = true;
      result.legs.push_back(std::move(leg));
    }
  } catch (...) {
    result.legs.clear();
  }
}

}  // namespace

BacktestRunResult runBacktest(const BacktestRunRequest& request,
                              BacktestProgressCallback progressCallback,
                              void* userData) noexcept {
  BacktestRunResult result{};
  result.runId = request.runId.empty() ? makeRunId() : request.runId;
  result.requestId = request.requestId.empty() ? result.runId : request.requestId;
  result.sessionPath = request.sessionPath;
  result.configPath = request.configPath;
  result.resultPath = resultPathFor(request, result.runId);
  result.rateLimitsEnabled = request.rateLimitsEnabled;
  result.strictRateLimitRejects = request.strictRateLimitRejects;
  const bool hasExplicitSessions = !request.sessions.empty();
  const std::size_t requestedSessionCount = explicitSessionPathCount(request, hasExplicitSessions);
  const std::int64_t portfolioInitialBalance = portfolioInitialBalanceE8(request, requestedSessionCount);
  BacktestRunRequest effectiveRequest = request;
  effectiveRequest.initialBalanceE8 = portfolioInitialBalance;
  if (effectiveRequest.latencySeed == 0u) effectiveRequest.latencySeed = hashRunSeed(result.runId);
  const detail::BacktestLatencyConfig latencyConfig = detail::latencyConfigFromRequest(effectiveRequest);
  result.latencySeed = latencyConfig.seed;
  result.marketDataLatency = latencyConfig.marketData;
  result.marketOrderLatency = latencyConfig.marketOrder;
  result.limitOrderLatency = latencyConfig.limitOrder;
  result.cancelOrderLatency = latencyConfig.cancelOrder;
  result.userDataLatency = latencyConfig.userData;
  result.orderLatencyUs = request.orderLatencyUs;
  result.cancelLatencyUs = latencyConfig.cancelOrder.baseUs;
  result.initialBalanceE8 = portfolioInitialBalance;
  result.walletBalanceE8 = portfolioInitialBalance;
  auto refreshArtifactCounters = [&](const detail::EngineState& engine) noexcept {
    result.strategySpreadRows = engine.strategySpread.size();
  };
  auto writeArtifacts = [&](const detail::EngineState& engine) noexcept -> Status {
    refreshArtifactCounters(engine);
    if (!request.writeArtifacts) return Status::Ok;
    return detail::writeResult(result, engine);
  };
  auto writeEmptyArtifacts = [&]() noexcept -> Status {
    if (!request.writeArtifacts) return Status::Ok;
    try {
      const auto empty = std::make_unique<detail::EngineState>();
      return writeArtifacts(*empty);
    } catch (...) {
      return Status::IoError;
    }
  };

  if ((!hasExplicitSessions && request.sessionPath.empty()) || request.configPath.empty()) {
    result.status = Status::InvalidArgument;
    result.error = "session_path and config_path are required";
    return result;
  }
  if (hasExplicitSessions && explicitSessionPathCount(request, hasExplicitSessions) > hft_trader::core::kStrategyRuntimeMaxLegs) {
    result.status = Status::InvalidArgument;
    result.error = "too many backtest sessions (max " + std::to_string(hft_trader::core::kStrategyRuntimeMaxLegs) + ")";
    return result;
  }
  if (!request.outputPath.empty() && request.outputPath.extension() == ".json") {
    result.status = Status::InvalidArgument;
    result.error = "output_path must be a run directory, not a .json file";
    return result;
  }
  seedRequestedLegSummaries(result, request, hasExplicitSessions);

  std::string error;
  if (hft_trader::runtime::hftRuntimeConfigAbiFingerprint() !=
      hft_trader::runtime::kHftRuntimeConfigAbiFingerprint) {
    result.status = Status::ConfigMismatch;
    result.error = "hft-trader runtime ABI mismatch: rebuild apps/hft-trader and apps/hft-backtest before running backtests";
    (void)writeEmptyArtifacts();
    return result;
  }

  hft_trader::runtime::HftRuntimeConfig cfg{};
  if (!hft_trader::runtime::loadRuntimeConfig(request.configPath.string().c_str(), cfg, error)) {
    result.status = Status::InvalidArgument;
    result.error = error;
    (void)writeEmptyArtifacts();
    return result;
  }
  result.liveProbe.configured = cfg.liveProbe.configured;
  result.liveProbe.enabled = cfg.liveProbe.enabled;
  result.liveProbe.ignored = cfg.liveProbe.configured;
  result.liveProbe.notionalUsdtE8 = cfg.liveProbe.notionalUsdtE8;
  result.liveProbe.timeoutMs = cfg.liveProbe.timeoutMs;
  result.liveProbe.limitOffsetSpreadPct = cfg.liveProbe.limitOffsetSpreadPct;
  result.strategy = request.strategy.empty() ? cfg.strategyType : request.strategy;
  StrategyIndicatorProfileInfo indicatorProfile{};
  if (!resolveStrategyIndicatorProfile(request.indicatorProfile, indicatorProfile)) {
    result.status = Status::InvalidArgument;
    result.error = "unknown indicator profile";
    (void)writeEmptyArtifacts();
    return result;
  }
  result.indicatorProfile = std::string{indicatorProfile.id};
  if (!strategyDeclaresIndicator(result.strategy, result.indicatorProfile)) {
    result.status = Status::InvalidArgument;
    result.error = "indicator profile is not supported by strategy";
    (void)writeEmptyArtifacts();
    return result;
  }
  if (result.strategy != cfg.strategyType) {
    result.status = Status::ConfigMismatch;
    result.error = "requested strategy does not match config [strategy] type";
    (void)writeEmptyArtifacts();
    return result;
  }
  const StrategyDescriptor* descriptor = hft_trader::runtime::findStrategyDescriptor(cfg.strategyType);
  if (!descriptor || !descriptor->resolveParams || !descriptor->initState || !descriptor->runCycle) {
    result.status = Status::Unsupported;
    result.error = "strategy descriptor is missing required callbacks";
    (void)writeEmptyArtifacts();
    return result;
  }

  detail::SessionLoadPlan loadPlan{};
  if (!buildSessionLoadPlan(*descriptor, loadPlan, error)) {
    result.status = Status::Unsupported;
    result.error = error;
    (void)writeEmptyArtifacts();
    return result;
  }
  const bool requestedTwoPhaseReprice = request.executionPipeline == BacktestExecutionPipeline::TwoPhaseReprice;
  const bool twoPhaseRepriceActive = requestedTwoPhaseReprice && !loadPlan.requireDepths;
  if (requestedTwoPhaseReprice && loadPlan.requireDepths) {
    result.twoPhaseRepriceFallback = true;
    result.warnings.push_back("two-phase reprice disabled: strategy requires orderbook stream");
  }
  if (twoPhaseRepriceActive) loadPlan.loadDepths = false;

  detail::PreparedBacktestInput* preparedInput = detail::activePreparedBacktestInput();
  const bool usePreparedSessions = preparedInput != nullptr &&
                                   preparedMatches(*preparedInput, request, hasExplicitSessions, loadPlan);
  std::vector<detail::SessionData> loadedSessions{};
  const std::vector<detail::SessionData>* sessionsView = nullptr;
  if (usePreparedSessions) {
    sessionsView = &preparedInput->sessions;
    result.status = Status::Ok;
  } else if (hasExplicitSessions) {
    const std::vector<std::filesystem::path> explicitPaths = sessionPathsFor(request, hasExplicitSessions);
    loadedSessions.resize(explicitPaths.size());
    for (std::size_t i = 0; i < explicitPaths.size(); ++i) {
      result.status = detail::loadSession(explicitPaths[i], loadPlan, loadedSessions[i], error);
      if (!isOk(result.status)) break;
    }
    if (isOk(result.status) && preparedInput != nullptr) {
      storePreparedSessions(*preparedInput, request, hasExplicitSessions, loadPlan, loadedSessions);
      sessionsView = &preparedInput->sessions;
    } else {
      sessionsView = &loadedSessions;
    }
  } else {
    loadedSessions.resize(1u);
    result.status = detail::loadSession(request.sessionPath, loadPlan, loadedSessions[0], error);
    if (isOk(result.status) && preparedInput != nullptr) {
      storePreparedSessions(*preparedInput, request, hasExplicitSessions, loadPlan, loadedSessions);
      sessionsView = &preparedInput->sessions;
    } else {
      sessionsView = &loadedSessions;
    }
  }
  if (sessionsView != nullptr) appendSessionWarnings(result, *sessionsView);
  if (!isOk(result.status)) {
    result.error = error;
    (void)writeEmptyArtifacts();
    return result;
  }
  if (sessionsView == nullptr || sessionsView->empty()) {
    result.status = Status::CorruptData;
    result.error = "backtest session cache is empty";
    (void)writeEmptyArtifacts();
    return result;
  }
  const std::vector<detail::SessionData>& sessions = *sessionsView;
  const detail::SessionData& session = sessions[0];
  result.sessionPath = session.sessionPath;
  if (hasExplicitSessions) seedLoadedLegSummaries(result, request, sessions);

  const ExchangeId sessionExchange = detail::exchangeFromText(session.exchange);
  const canon::MarketType sessionMarket = detail::marketFromText(session.market);
  const hft_trader::runtime::VenueRuntimeConfig* venue = nullptr;
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
    result.error = "config venue/symbol does not match recorder session";
    (void)writeEmptyArtifacts();
    return result;
  }

  StrategyParamStorage params{};
  if (!descriptor->resolveParams(hft_trader::runtime::makeStrategyParamContext(cfg, *venue), params, error)) {
    result.status = Status::InvalidArgument;
    result.error = error;
    (void)writeEmptyArtifacts();
    return result;
  }
  StrategyInstrumentSpec instrument{};
  instrument.tickSizeRaw = session.instrument.tickSizeE8;
  instrument.stepSizeRaw = session.instrument.lotSizeE8;
  instrument.contractBaseQtyRaw = session.instrument.contractBaseQtyE8;
  instrument.priceBasisQtyRaw = session.instrument.priceBasisQtyE8;
  instrument.expiryUtcNs = static_cast<std::uint64_t>(session.instrument.expiryUtcNs > 0 ? session.instrument.expiryUtcNs : 0);
  instrument.tickGrid = hft_trader::core::prepareFastStepGrid(instrument.tickSizeRaw);
  instrument.qtyStepGrid = hft_trader::core::prepareFastStepGrid(instrument.stepSizeRaw);
  instrument.contractBaseQtyGrid = hft_trader::core::prepareFastStepGrid(instrument.contractBaseQtyRaw);
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
      legInstruments[leg].tickGrid = hft_trader::core::prepareFastStepGrid(legInstruments[leg].tickSizeRaw);
      legInstruments[leg].qtyStepGrid = hft_trader::core::prepareFastStepGrid(legInstruments[leg].stepSizeRaw);
      legInstruments[leg].contractBaseQtyGrid = hft_trader::core::prepareFastStepGrid(legInstruments[leg].contractBaseQtyRaw);
      legSymbols[leg].copyFrom(sessions[leg].symbol.c_str());
    }
  }

  alignas(hft_trader::runtime::kStrategyRuntimeStateAlign) std::array<unsigned char, hft_trader::runtime::kStrategyRuntimeStateBytes> state{};
  if (!descriptor->initState(state.data(), params, instrument)) {
    result.status = Status::InvalidArgument;
    result.error = "strategy initState failed";
    (void)writeEmptyArtifacts();
    return result;
  }
  BacktestInputCoverage seedCoverage{};
  if (descriptor->seedHistory) {
    if (session.candles.empty()) {
      result.status = Status::CorruptData;
      result.error = "strategy requires candle history but candles channel is missing or empty";
      (void)writeEmptyArtifacts();
      return result;
    }
    cxet::composite::TieredCandleHistory history{};
    if (!detail::buildTieredCandleHistory(session.candles, history)) {
      result.status = Status::CorruptData;
      result.error = "failed to build strategy candle history from recorder session";
      (void)writeEmptyArtifacts();
      return result;
    }
    seedCoverage.seedM1Candles = history.m1Count.raw;
    seedCoverage.seedM15Candles = history.m15Count.raw;
    seedCoverage.seedD1Candles = history.d1Count.raw;
    hft_trader::runtime::StrategyHistorySeedStats stats{};
    if (!descriptor->seedHistory(state.data(), history, &stats)) {
      result.status = Status::InvalidArgument;
      result.error = "strategy seedHistory failed";
      (void)writeEmptyArtifacts();
      return result;
    }
  }
  if (!seedTradeWarmupFromSession(*descriptor, state.data(), params, session, error)) {
    result.status = Status::CorruptData;
    result.error = error;
    (void)writeEmptyArtifacts();
    return result;
  }
  seedCoverage.seedTradeRows = session.tradeWarmup.size();

  std::unique_ptr<detail::EngineState> engineStorage;
  try {
    engineStorage = std::make_unique<detail::EngineState>();
  } catch (...) {
    result.status = Status::IoError;
    result.error = "failed to allocate backtest engine state";
    (void)writeEmptyArtifacts();
    return result;
  }
  detail::EngineState& engine = *engineStorage;
  configureStrategyIndicator(engine, indicatorProfile);
  detail::initLatencyRuntime(engine.latency, resolveLatencyConfig(effectiveRequest, session.exchange, session.market));
  engine.inputCoverage = seedCoverage;
  engine.exchange = sessionExchange;
  engine.market = sessionMarket;
  engine.symbol = selectedSymbol;
  engine.tickSizeE8 = instrument.tickSizeRaw;
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
      (void)writeEmptyArtifacts();
      return result;
    }
  }
  std::vector<detail::EngineState> legEngines;
  if (hasExplicitSessions) {
    legEngines.resize(sessions.size());
    for (std::size_t leg = 0; leg < sessions.size(); ++leg) {
      detail::EngineState& legEngine = legEngines[leg];
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
        (void)writeEmptyArtifacts();
        return result;
      }
    }
    if (!legEngines.empty()) engine = legEngines[0];
  }
  if (loadPlan.loadDepths) {
    if (hasExplicitSessions) {
      for (std::size_t leg = 0; leg < sessions.size() && leg < legEngines.size(); ++leg) {
        legEngines[leg].venueOrderBookExecutionEnabled = !sessions[leg].depths.empty();
      }
      if (!legEngines.empty()) engine = legEngines[0];
    } else {
      engine.venueOrderBookExecutionEnabled = !session.depths.empty();
    }
  }

  std::vector<std::unique_ptr<detail::ExecutionDepthProvider>> executionDepthProviders{};
  if (!loadPlan.loadDepths && !twoPhaseRepriceActive) {
    if (hasExplicitSessions) {
      executionDepthProviders.reserve(legEngines.size());
      for (std::size_t leg = 0; leg < sessions.size() && leg < legEngines.size(); ++leg) {
        hft_trader::core::LocalOrderBookConfig bookConfig{};
        bookConfig.maxStoredLevelsPerSide.raw = rawdata::kMaxOrderbookLevels;
        bookConfig.tickSizeRaw = sessions[leg].instrument.tickSizeE8;
        Symbol legSymbol{};
        legSymbol.copyFrom(sessions[leg].symbol.c_str());
        auto provider = std::make_unique<detail::ExecutionDepthProvider>();
        if (provider->open(sessions[leg].sessionPath, legSymbol, bookConfig, legEngines[leg].venueOrderBook)) {
          legEngines[leg].executionDepthProvider = provider.get();
          executionDepthProviders.push_back(std::move(provider));
        }
      }
      if (!legEngines.empty()) engine = legEngines[0];
    } else {
      hft_trader::core::LocalOrderBookConfig bookConfig{};
      bookConfig.maxStoredLevelsPerSide.raw = rawdata::kMaxOrderbookLevels;
      bookConfig.tickSizeRaw = instrument.tickSizeRaw;
      auto provider = std::make_unique<detail::ExecutionDepthProvider>();
      if (provider->open(session.sessionPath, selectedSymbol, bookConfig, engine.venueOrderBook)) {
        engine.executionDepthProvider = provider.get();
        executionDepthProviders.push_back(std::move(provider));
      }
    }
  }
  hft_trader::core::SymbolShardConfig shard{};
  shard.shardId.raw = 1;
  shard.strategyId.raw = 1;
  shard.symbolId.raw = 1;
  shard.symbol = selectedSymbol;
  hft_trader::core::ShardControlState control{};
  control.shardId = shard.shardId;

  std::vector<detail::TimelineEvent> loadedTimeline;
  const std::vector<detail::TimelineEvent>* timelineView = nullptr;
  if (usePreparedSessions && preparedInput != nullptr && preparedInput->timelineValid) {
    timelineView = &preparedInput->timeline;
  } else if (preparedInput != nullptr && preparedMatches(*preparedInput, request, hasExplicitSessions, loadPlan)) {
    if (hasExplicitSessions) detail::buildTimeline(preparedInput->sessions, preparedInput->timeline);
    else detail::buildTimeline(preparedInput->sessions[0], preparedInput->timeline);
    preparedInput->timelineValid = true;
    timelineView = &preparedInput->timeline;
  } else {
    if (hasExplicitSessions) detail::buildTimeline(sessions, loadedTimeline);
    else detail::buildTimeline(session, loadedTimeline);
    timelineView = &loadedTimeline;
  }
  const std::vector<detail::TimelineEvent>& timeline = *timelineView;
  std::vector<detail::TimelineEvent> loadedDeliveryTimeline;
  const std::vector<detail::TimelineEvent>* deliveryTimelineView = nullptr;
  if (!hasMarketDataLatency(effectiveRequest, latencyConfig)) {
    deliveryTimelineView = &timeline;
  } else {
    loadedDeliveryTimeline.clear();
    loadedDeliveryTimeline.reserve(timeline.size());
    for (const detail::TimelineEvent& rawEvent : timeline) {
      loadedDeliveryTimeline.push_back(rawEvent);
    }
    for (detail::TimelineEvent& event : loadedDeliveryTimeline) {
      if (hasExplicitSessions && event.legIndex < legEngines.size()) {
        event.deliveryTsNs = detail::delayedTs(
            event.tsNs,
            detail::sampleLatencyUs(legEngines[event.legIndex].latency, detail::BacktestLatencyChannel::MarketData));
      } else {
        event.deliveryTsNs = detail::delayedTs(
            event.tsNs,
            detail::sampleLatencyUs(engine.latency, detail::BacktestLatencyChannel::MarketData));
      }
    }
    if (!marketDataLatencyKeepsOrder(effectiveRequest, latencyConfig)) {
      std::stable_sort(loadedDeliveryTimeline.begin(), loadedDeliveryTimeline.end(), [](const detail::TimelineEvent& lhs, const detail::TimelineEvent& rhs) noexcept {
        if (lhs.deliveryTsNs != rhs.deliveryTsNs) return lhs.deliveryTsNs < rhs.deliveryTsNs;
        if (lhs.tsNs != rhs.tsNs) return lhs.tsNs < rhs.tsNs;
        if (lhs.ingestSeq != rhs.ingestSeq) return lhs.ingestSeq < rhs.ingestSeq;
        return lhs.kind < rhs.kind;
      });
    }
    deliveryTimelineView = &loadedDeliveryTimeline;
  }
  const std::vector<detail::TimelineEvent>& deliveryTimeline = *deliveryTimelineView;
  result.trades = 0;
  result.bookTickers = 0;
  result.priceLimits = 0;
  result.depths = 0;
  result.liquidations = 0;
  result.markPrices = 0;
  result.indexPrices = 0;
  result.funding = 0;
  result.openInterest = 0;
  result.firstTsNs = std::numeric_limits<std::int64_t>::max();
  result.lastTsNs = 0;
  for (const detail::SessionData& loadedSession : sessions) {
    result.trades += loadedSession.trades.size();
    result.bookTickers += loadedSession.bookTickers.size();
    result.priceLimits += loadedSession.priceLimits.size();
    result.depths += loadedSession.depths.size();
    result.liquidations += loadedSession.liquidations.size();
    result.markPrices += loadedSession.markPrices.size();
    result.indexPrices += loadedSession.indexPrices.size();
    result.funding += loadedSession.funding.size();
    result.openInterest += loadedSession.openInterest.size();
    if (loadedSession.firstTsNs > 0 && loadedSession.firstTsNs < result.firstTsNs) result.firstTsNs = loadedSession.firstTsNs;
    if (loadedSession.lastTsNs > result.lastTsNs) result.lastTsNs = loadedSession.lastTsNs;
  }
  if (result.firstTsNs == std::numeric_limits<std::int64_t>::max()) result.firstTsNs = session.firstTsNs;
  result.events = timeline.size();
  const bool captureIndicatorTrace = !result.indicatorProfile.empty();
  const bool captureRangeTrace = false;
  const bool captureSpreadTrace = request.captureStrategySpread && emitsArbSpreadTrace(*descriptor);
  if (hasExplicitSessions) {
    for (detail::EngineState& legEngine : legEngines) {
      reserveEngineStorage(legEngine, timeline.size(), captureIndicatorTrace, captureRangeTrace, captureSpreadTrace);
    }
  } else {
    reserveEngineStorage(engine, timeline.size(), captureIndicatorTrace, captureRangeTrace, captureSpreadTrace);
  }
  if (hasExplicitSessions) {
    for (detail::EngineState& legEngine : legEngines) {
      detail::appendEquityPoint(legEngine, result.firstTsNs);
      detail::appendRiskSnapshot(legEngine, result.firstTsNs, detail::kRiskSnapshotReasonStart);
    }
  } else {
    detail::appendEquityPoint(engine, result.firstTsNs);
    detail::appendRiskSnapshot(engine, result.firstTsNs, detail::kRiskSnapshotReasonStart);
  }
  std::array<hft_trader::core::StrategyAccountState, hft_trader::core::kStrategyRuntimeMaxLegs> riskAccounts{};
  std::array<const hft_trader::runtime::VenueRuntimeConfig*, hft_trader::core::kStrategyRuntimeMaxLegs> riskVenues{};
  const std::size_t riskAccountCount = hasExplicitSessions ? boundedRiskLegCount(legEngines.size()) : 1u;
  if (hasExplicitSessions) {
    refreshRiskAccountsFromEngines(legEngines, riskAccounts);
    resolveRiskVenuesForSessions(cfg, sessions, riskVenues);
  } else {
    riskAccounts[0] = accountStateFromEngine(engine);
    riskVenues[0] = venue;
  }
  hft_trader::runtime::PortfolioRiskRuntimeState traderRiskState =
      hft_trader::runtime::makePortfolioRiskRuntimeState(cfg.risk, riskAccounts.data(), riskAccountCount, riskVenues.data());
  auto writeCurrentArtifacts = [&]() noexcept -> Status {
    if (hasExplicitSessions) {
      const std::int64_t tsNs = result.lastTsNs > 0 ? result.lastTsNs : result.firstTsNs;
      try {
        for (detail::EngineState& legEngine : legEngines) detail::appendRateLimitUsageSnapshot(legEngine, tsNs);
      } catch (...) {
      }
      populatePortfolioResultPnl(result, sessions, legEngines, portfolioInitialBalance);
      appendRateLimitWarnings(result, engine, &legEngines);
      result.rateLimitRejectedCommands = rateLimitRejectedCommands(engine, &legEngines);
      try {
        auto aggregate = std::make_unique<detail::EngineState>();
        populateAggregateArtifactEngine(legEngines, portfolioInitialBalance, tsNs, *aggregate);
        refreshArtifactCounters(*aggregate);
        return request.writeArtifacts ? detail::writeResult(result, *aggregate, &legEngines) : Status::Ok;
      } catch (...) {
        return Status::IoError;
      }
    }
    try {
      detail::appendRateLimitUsageSnapshot(engine, result.lastTsNs > 0 ? result.lastTsNs : result.firstTsNs);
    } catch (...) {
    }
    populateResultPnl(result, engine);
    appendRateLimitWarnings(result, engine);
    result.rateLimitRejectedCommands = rateLimitRejectedCommands(engine);
    result.orders = engine.orders.size();
    result.fills = engine.fills.size();
    return writeArtifacts(engine);
  };
  auto stopOnStrictRateLimitReject = [&]() noexcept -> bool {
    if (!result.rateLimitsEnabled || !result.strictRateLimitRejects) return false;
    const std::uint64_t rejected = rateLimitRejectedCommands(engine, hasExplicitSessions ? &legEngines : nullptr);
    if (rejected == 0u) return false;
    result.rateLimitRejectedCommands = rejected;
    (void)applyStrictRateLimitStatus(result, engine, hasExplicitSessions ? &legEngines : nullptr);
    (void)writeCurrentArtifacts();
    return true;
  };
  bool traderRiskCloseProblem = false;
  bool legLiquidationProblem = false;
  auto tripHardLiquidation = [&](std::int64_t tsNs) noexcept -> bool {
    if (portfolioInitialBalance <= 0) return false;
    if (hasExplicitSessions) {
      for (std::size_t leg = 0u; leg < legEngines.size(); ++leg) {
        detail::EngineState& legEngine = legEngines[leg];
        if (legEngine.liquidated) {
          legLiquidationProblem = true;
          return true;
        }
        if (legEngine.initialBalanceE8 > 0 && detail::equityE8(legEngine) <= 0) {
          const std::int64_t legEquity = detail::equityE8(legEngine);
          detail::hardLiquidate(legEngine, tsNs, legEquity);
          control.killActive = hft_trader::core::kAlgoFlagTrue;
          control.haltReason = hft_trader::core::kHaltReasonManualKill;
          result.liquidated = true;
          result.liquidationTsNs = tsNs;
          result.liquidationEquityE8 = legEquity;
          result.error = "leg " + std::to_string(leg) + " liquidated";
          legLiquidationProblem = true;
          return true;
        }
      }
      const std::int64_t equity = portfolioEquityE8(legEngines, portfolioInitialBalance);
      if (equity > 0) return false;
      for (detail::EngineState& legEngine : legEngines) detail::hardLiquidate(legEngine, tsNs, equity);
      control.killActive = hft_trader::core::kAlgoFlagTrue;
      control.haltReason = hft_trader::core::kHaltReasonManualKill;
      result.liquidated = true;
      result.liquidationTsNs = tsNs;
      result.liquidationEquityE8 = equity;
      return true;
    }
    if (engine.liquidated) return true;
    const std::int64_t equity = detail::equityE8(engine);
    if (equity > 0) return false;
    detail::hardLiquidate(engine, tsNs, equity);
    control.killActive = hft_trader::core::kAlgoFlagTrue;
    control.haltReason = hft_trader::core::kHaltReasonManualKill;
    result.liquidated = true;
    result.liquidationTsNs = tsNs;
    result.liquidationEquityE8 = equity;
    return true;
  };
  auto tripTraderRiskStop = [&](std::int64_t tsNs, bool forceRiskCheck = false) -> bool {
    const TimeNs riskNow = timeNsFromI64(tsNs);
    if (hasExplicitSessions) {
      for (const detail::EngineState& legEngine : legEngines) {
        if (legEngine.liquidated) return false;
        if (legEngine.riskStopped) return true;
      }
      refreshRiskAccountsFromEngines(legEngines, riskAccounts);
    } else {
      if (engine.liquidated) return false;
      if (engine.riskStopped) return true;
      riskAccounts[0] = accountStateFromEngine(engine);
    }
    if (!hft_trader::runtime::acquirePortfolioRiskEvaluationSlot(traderRiskState, riskNow, forceRiskCheck)) return false;
    const hft_trader::runtime::PortfolioRiskEvaluation eval =
        hft_trader::runtime::evaluatePortfolioRisk(traderRiskState, riskAccounts.data(), riskAccountCount, riskNow);
    if (!eval.breached) return false;
    if (hasExplicitSessions) {
      for (detail::EngineState& legEngine : legEngines) {
        if (!forceCloseForTraderRisk(legEngine, tsNs)) traderRiskCloseProblem = true;
        detail::stopForTraderRisk(legEngine, tsNs, eval.equityE8, eval.thresholdEquityE8);
      }
    } else {
      if (!forceCloseForTraderRisk(engine, tsNs)) traderRiskCloseProblem = true;
      detail::stopForTraderRisk(engine, tsNs, eval.equityE8, eval.thresholdEquityE8);
    }
    hft_trader::runtime::applyPortfolioRiskToControl(traderRiskState, control);
    result.riskStopped = true;
    result.riskStopTsNs = tsNs;
    result.riskStopEquityE8 = eval.equityE8;
    result.riskStopThresholdEquityE8 = eval.thresholdEquityE8;
    return true;
  };

  auto reportReplayProgress = [&](std::uint64_t done, std::uint64_t total) noexcept -> bool {
    return twoPhaseRepriceActive
        ? reportProgress(done, total, "phase1 replay", 0u, 50u, progressCallback, userData)
        : reportProgress(done, total, "running", progressCallback, userData);
  };
  auto reportRepriceProgress = [&](std::uint64_t done, std::uint64_t total) noexcept -> bool {
    return reportProgress(done, total, "phase2 reprice", 50u, 50u, progressCallback, userData);
  };

  if (!reportReplayProgress(0, timeline.size())) {
    result.status = Status::Cancelled;
    result.error = "cancelled";
    (void)writeCurrentArtifacts();
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
      (void)writeCurrentArtifacts();
      return result;
    }
    singleScratch = singleScratchStorage.get();
  } else {
    try {
      multiScratchStorage = std::make_unique<MultiLegCycleScratch>();
    } catch (...) {
      result.status = Status::IoError;
      result.error = "failed to allocate multi-leg cycle scratch";
      (void)writeCurrentArtifacts();
      return result;
    }
    multiScratch = multiScratchStorage.get();
    multiScratch->reset(legEngines.size() < hft_trader::core::kStrategyRuntimeMaxLegs ? legEngines.size() : hft_trader::core::kStrategyRuntimeMaxLegs);
  }
  bool replayStopped = false;
  std::size_t i = 0u;
  while (i < timeline.size()) {
    if (i + 1u < timeline.size()) prefetchRead(&timeline[i + 1u]);
    if (deliveryCursor < deliveryTimeline.size()) prefetchRead(&deliveryTimeline[deliveryCursor]);
    const std::int64_t eventTsNs = timeline[i].tsNs;
    // Same-timestamp venue updates win the tie: orders due at raw ts activate after the raw BBO is applied.
    const bool beforeOk = hasExplicitSessions
        ? runUntilMulti(*descriptor, state.data(), legEngines, legInstruments, *multiScratch, shard, control, eventTsNs, request, RunUntilBoundary::Exclusive)
        : runUntil(*descriptor, state.data(), engine, instrument, *singleScratch, shard, control, eventTsNs, request, RunUntilBoundary::Exclusive);
    if (!beforeOk) {
      result.status = Status::InvalidArgument;
      result.error = "strategy event loop did not settle before market event";
      (void)writeCurrentArtifacts();
      return result;
    }
    if (stopOnStrictRateLimitReject()) return result;
    if (tripHardLiquidation(eventTsNs) || tripTraderRiskStop(eventTsNs)) {
      replayStopped = true;
      break;
    }
    while (deliveryCursor < deliveryTimeline.size() && deliveryTimeline[deliveryCursor].deliveryTsNs < eventTsNs) {
      if (deliveryCursor + 1u < deliveryTimeline.size()) prefetchRead(&deliveryTimeline[deliveryCursor + 1u]);
      const detail::TimelineEvent& delivered = deliveryTimeline[deliveryCursor++];
      if (hasExplicitSessions) applyMarketEventToSnapshots(legEngines, sessions, delivered, legSymbols, legInstruments, true);
      else applyMarketEventToSnapshots(engine, session, delivered, selectedSymbol, instrument, true);
      const bool deliveredOk = hasExplicitSessions
          ? runCycleMulti(*descriptor, state.data(), legEngines, legInstruments, *multiScratch, shard, control, delivered.legIndex, delivered.deliveryTsNs, request, nullptr)
          : runCycle(*descriptor, state.data(), engine, instrument, *singleScratch, shard, control, delivered.deliveryTsNs, request, nullptr);
      if (!deliveredOk) {
        result.status = Status::InvalidArgument;
        result.error = "strategy runCycle failed on delivered market event";
        (void)writeCurrentArtifacts();
        return result;
      }
      if (stopOnStrictRateLimitReject()) return result;
      if (hasExplicitSessions) {
        for (detail::EngineState& legEngine : legEngines) (void)detail::appendRiskSnapshotIfDue(legEngine, delivered.deliveryTsNs);
      } else {
        (void)detail::appendRiskSnapshotIfDue(engine, delivered.deliveryTsNs);
      }
      if (tripHardLiquidation(delivered.deliveryTsNs) || tripTraderRiskStop(delivered.deliveryTsNs)) {
        replayStopped = true;
        break;
      }
    }
    if (replayStopped) break;
    const bool beforeRawOk = hasExplicitSessions
        ? runUntilMulti(*descriptor, state.data(), legEngines, legInstruments, *multiScratch, shard, control, eventTsNs, request, RunUntilBoundary::Exclusive)
        : runUntil(*descriptor, state.data(), engine, instrument, *singleScratch, shard, control, eventTsNs, request, RunUntilBoundary::Exclusive);
    if (!beforeRawOk) {
      result.status = Status::InvalidArgument;
      result.error = "strategy event loop did not settle before raw market event";
      (void)writeCurrentArtifacts();
      return result;
    }
    if (stopOnStrictRateLimitReject()) return result;
    while (i < timeline.size() && timeline[i].tsNs == eventTsNs) {
      if (i + 1u < timeline.size()) prefetchRead(&timeline[i + 1u]);
      const detail::TimelineEvent& event = timeline[i++];
      if (hasExplicitSessions) applyMarketEventToSnapshots(legEngines, sessions, event, legSymbols, legInstruments, false);
      else applyMarketEventToSnapshots(engine, session, event, selectedSymbol, instrument, false);
      if (hasExplicitSessions) {
        for (detail::EngineState& legEngine : legEngines) (void)detail::appendRiskSnapshotIfDue(legEngine, event.tsNs);
      } else {
        (void)detail::appendRiskSnapshotIfDue(engine, event.tsNs);
      }
      if (tripHardLiquidation(event.tsNs) || tripTraderRiskStop(event.tsNs)) {
        replayStopped = true;
        break;
      }
    }
    if (replayStopped) break;
    while (deliveryCursor < deliveryTimeline.size() && deliveryTimeline[deliveryCursor].deliveryTsNs <= eventTsNs) {
      if (deliveryCursor + 1u < deliveryTimeline.size()) prefetchRead(&deliveryTimeline[deliveryCursor + 1u]);
      const detail::TimelineEvent& delivered = deliveryTimeline[deliveryCursor++];
      if (hasExplicitSessions) applyMarketEventToSnapshots(legEngines, sessions, delivered, legSymbols, legInstruments, true);
      else applyMarketEventToSnapshots(engine, session, delivered, selectedSymbol, instrument, true);
      const bool deliveredOk = hasExplicitSessions
          ? runCycleMulti(*descriptor, state.data(), legEngines, legInstruments, *multiScratch, shard, control, delivered.legIndex, delivered.deliveryTsNs, request, nullptr)
          : runCycle(*descriptor, state.data(), engine, instrument, *singleScratch, shard, control, delivered.deliveryTsNs, request, nullptr);
      if (!deliveredOk) {
        result.status = Status::InvalidArgument;
        result.error = "strategy runCycle failed on delivered market event";
        (void)writeCurrentArtifacts();
        return result;
      }
      if (stopOnStrictRateLimitReject()) return result;
      if (hasExplicitSessions) {
        for (detail::EngineState& legEngine : legEngines) (void)detail::appendRiskSnapshotIfDue(legEngine, delivered.deliveryTsNs);
      } else {
        (void)detail::appendRiskSnapshotIfDue(engine, delivered.deliveryTsNs);
      }
      if (tripHardLiquidation(delivered.deliveryTsNs) || tripTraderRiskStop(delivered.deliveryTsNs)) {
        replayStopped = true;
        break;
      }
    }
    if (replayStopped) break;
    const bool afterOk = hasExplicitSessions
        ? runUntilMulti(*descriptor, state.data(), legEngines, legInstruments, *multiScratch, shard, control, eventTsNs, request)
        : runUntil(*descriptor, state.data(), engine, instrument, *singleScratch, shard, control, eventTsNs, request);
    if (!afterOk) {
      result.status = Status::InvalidArgument;
      result.error = "strategy event loop did not settle after market event";
      (void)writeCurrentArtifacts();
      return result;
    }
    if (stopOnStrictRateLimitReject()) return result;
    if (tripHardLiquidation(eventTsNs) || tripTraderRiskStop(eventTsNs)) {
      replayStopped = true;
      break;
    }
    if ((i % replayProgressStep) == 0u && !reportReplayProgress(i, timeline.size())) {
      result.status = Status::Cancelled;
      result.error = "cancelled";
      (void)writeCurrentArtifacts();
      return result;
    }
  }
  while (!replayStopped && deliveryCursor < deliveryTimeline.size()) {
    if (deliveryCursor + 1u < deliveryTimeline.size()) prefetchRead(&deliveryTimeline[deliveryCursor + 1u]);
    const detail::TimelineEvent& delivered = deliveryTimeline[deliveryCursor++];
    const bool beforeDelayedOk = hasExplicitSessions
        ? runUntilMulti(*descriptor, state.data(), legEngines, legInstruments, *multiScratch, shard, control, delivered.deliveryTsNs, request)
        : runUntil(*descriptor, state.data(), engine, instrument, *singleScratch, shard, control, delivered.deliveryTsNs, request);
    if (!beforeDelayedOk) {
      result.status = Status::InvalidArgument;
      result.error = "strategy event loop did not settle before delayed market event";
      (void)writeCurrentArtifacts();
      return result;
    }
    if (stopOnStrictRateLimitReject()) return result;
    if (hasExplicitSessions) applyMarketEventToSnapshots(legEngines, sessions, delivered, legSymbols, legInstruments, true);
    else applyMarketEventToSnapshots(engine, session, delivered, selectedSymbol, instrument, true);
    const bool delayedOk = hasExplicitSessions
        ? runCycleMulti(*descriptor, state.data(), legEngines, legInstruments, *multiScratch, shard, control, delivered.legIndex, delivered.deliveryTsNs, request, nullptr)
        : runCycle(*descriptor, state.data(), engine, instrument, *singleScratch, shard, control, delivered.deliveryTsNs, request, nullptr);
    if (!delayedOk) {
      result.status = Status::InvalidArgument;
      result.error = "strategy runCycle failed on delayed market event";
      (void)writeCurrentArtifacts();
      return result;
    }
    if (stopOnStrictRateLimitReject()) return result;
    if (hasExplicitSessions) {
      for (detail::EngineState& legEngine : legEngines) (void)detail::appendRiskSnapshotIfDue(legEngine, delivered.deliveryTsNs);
    } else {
      (void)detail::appendRiskSnapshotIfDue(engine, delivered.deliveryTsNs);
    }
    if (tripHardLiquidation(delivered.deliveryTsNs) || tripTraderRiskStop(delivered.deliveryTsNs)) {
      replayStopped = true;
      break;
    }
  }
  if (!replayStopped) {
    const bool settled = hasExplicitSessions
        ? settleAfterReplayMulti(*descriptor, state.data(), legEngines, legInstruments, *multiScratch, shard, control, result.lastTsNs, effectiveRequest)
        : settleAfterReplay(*descriptor, state.data(), engine, instrument, *singleScratch, shard, control, result.lastTsNs, effectiveRequest);
    if (!settled) {
      result.status = Status::InvalidArgument;
      result.error = "strategy event loop did not settle after replay";
      (void)writeCurrentArtifacts();
      return result;
    }
    if (stopOnStrictRateLimitReject()) return result;
  }
  if (!replayStopped) {
    const std::int64_t stopCheckTsNs = result.lastTsNs > 0 ? result.lastTsNs : result.firstTsNs;
    (void)(tripHardLiquidation(stopCheckTsNs) || tripTraderRiskStop(stopCheckTsNs, true));
  }
  if (twoPhaseRepriceActive) {
    if (hasExplicitSessions) {
      for (std::size_t leg = 0; leg < legEngines.size() && leg < sessions.size(); ++leg) {
        detail::Phase2MakerQueueStats makerStats{};
        const bool makerOk = detail::repriceMakerLimitQueueWithBboTrades(sessions[leg].sessionPath,
                                                                         legEngines[leg],
                                                                         result.lastTsNs,
                                                                         makerStats);
        if (!makerOk && makerStats.ordersVisited > 0u) {
          result.warnings.push_back("two-phase maker queue skipped leg " + std::to_string(leg) + ": BBO/trade stream unavailable");
        }
      }
      if (!legEngines.empty()) engine = legEngines[0];
    } else {
      detail::Phase2MakerQueueStats makerStats{};
      const bool makerOk = detail::repriceMakerLimitQueueWithBboTrades(session.sessionPath,
                                                                       engine,
                                                                       result.lastTsNs,
                                                                       makerStats);
      if (!makerOk && makerStats.ordersVisited > 0u) {
        result.warnings.push_back("two-phase maker queue skipped: BBO/trade stream unavailable");
      }
    }
    std::uint64_t totalFillsForReprice = 0;
    if (hasExplicitSessions) {
      for (const detail::EngineState& legEngine : legEngines) totalFillsForReprice += legEngine.fills.size();
    } else {
      totalFillsForReprice = engine.fills.size();
    }
    if (!reportRepriceProgress(0, totalFillsForReprice)) {
      result.status = Status::Cancelled;
      result.error = "cancelled";
      (void)writeCurrentArtifacts();
      return result;
    }
    std::uint64_t repriceDone = 0;
    if (hasExplicitSessions) {
      for (std::size_t leg = 0; leg < legEngines.size() && leg < sessions.size() && leg < legSymbols.size() && leg < legInstruments.size(); ++leg) {
        detail::ExecutionRepriceStats stats{};
        const bool ok = detail::repriceFillsWithDepth(sessions[leg].sessionPath,
                                                      legSymbols[leg],
                                                      legInstruments[leg].tickSizeRaw,
                                                      legEngines[leg],
                                                      stats);
        result.phase2DepthRows += stats.depthRowsRead;
        result.phase2FillsRepriced += stats.fillsRepriced;
        result.phase2FillsMissingDepth += stats.fillsMissingDepth;
        result.phase2BboRepriced += stats.fillsBboRepriced;
        result.phase2DepthVwapRepriced += stats.fillsDepthVwapRepriced;
        result.phase2DepthRejectedUnseeded += stats.fillsDepthRejectedUnseeded;
        result.phase2DepthRejectedCrossed += stats.fillsDepthRejectedCrossed;
        result.phase2DepthRejectedStale += stats.fillsDepthRejectedStale;
        result.phase2MissingBbo += stats.fillsMissingBbo;
        detail::rebuildAccountingFromFills(legEngines[leg], result.firstTsNs, 0);
        if (!ok && !legEngines[leg].fills.empty()) {
          result.warnings.push_back("two-phase reprice skipped leg " + std::to_string(leg) + ": BBO stream unavailable");
        }
        repriceDone += legEngines[leg].fills.size();
        if (!reportRepriceProgress(repriceDone, totalFillsForReprice)) {
          result.status = Status::Cancelled;
          result.error = "cancelled";
          (void)writeCurrentArtifacts();
          return result;
        }
      }
      if (!legEngines.empty()) engine = legEngines[0];
    } else {
      detail::ExecutionRepriceStats stats{};
      const bool ok = detail::repriceFillsWithDepth(session.sessionPath,
                                                    selectedSymbol,
                                                    instrument.tickSizeRaw,
                                                    engine,
                                                    stats);
      result.phase2DepthRows = stats.depthRowsRead;
      result.phase2FillsRepriced = stats.fillsRepriced;
      result.phase2FillsMissingDepth = stats.fillsMissingDepth;
      result.phase2BboRepriced = stats.fillsBboRepriced;
      result.phase2DepthVwapRepriced = stats.fillsDepthVwapRepriced;
      result.phase2DepthRejectedUnseeded = stats.fillsDepthRejectedUnseeded;
      result.phase2DepthRejectedCrossed = stats.fillsDepthRejectedCrossed;
      result.phase2DepthRejectedStale = stats.fillsDepthRejectedStale;
      result.phase2MissingBbo = stats.fillsMissingBbo;
      detail::rebuildAccountingFromFills(engine, result.firstTsNs, 0);
      if (!ok && !engine.fills.empty()) result.warnings.push_back("two-phase reprice skipped: BBO stream unavailable");
      if (!reportRepriceProgress(totalFillsForReprice, totalFillsForReprice)) {
        result.status = Status::Cancelled;
        result.error = "cancelled";
        (void)writeCurrentArtifacts();
        return result;
      }
    }
    if (!replayStopped) {
      const std::int64_t postRepriceCheckTsNs = result.lastTsNs > 0 ? result.lastTsNs : result.firstTsNs;
      (void)(tripHardLiquidation(postRepriceCheckTsNs) ||
             tripTraderRiskStop(postRepriceCheckTsNs, true));
    }
    result.warnings.push_back("two-phase reprice is diagnostic: fills are updated post-hoc after phase1 strategy decisions");
    result.twoPhaseRepriceApplied = true;
  }
  const std::int64_t finalTsNs = result.lastTsNs > 0 ? result.lastTsNs + 1 : result.lastTsNs;
  if (hasExplicitSessions) {
    for (detail::EngineState& legEngine : legEngines) {
      detail::appendRateLimitUsageSnapshot(legEngine, finalTsNs);
      detail::appendEquityPoint(legEngine, finalTsNs);
      detail::appendRiskSnapshot(legEngine, finalTsNs, detail::kRiskSnapshotReasonFinal);
    }
    populatePortfolioResultPnl(result, sessions, legEngines, portfolioInitialBalance);
    try {
      auto aggregate = std::make_unique<detail::EngineState>();
      populateAggregateArtifactEngine(legEngines, portfolioInitialBalance, finalTsNs, *aggregate);
      engine = std::move(*aggregate);
    } catch (...) {
      result.status = Status::IoError;
      result.error = "failed to allocate aggregate backtest engine state";
      (void)writeEmptyArtifacts();
      return result;
    }
  } else {
    detail::appendRateLimitUsageSnapshot(engine, finalTsNs);
    detail::appendEquityPoint(engine, finalTsNs);
    detail::appendRiskSnapshot(engine, finalTsNs, detail::kRiskSnapshotReasonFinal);
    populateResultPnl(result, engine);
    result.orders = engine.orders.size();
    result.fills = engine.fills.size();
  }
  if (result.twoPhaseRepriceApplied && !result.strategyClosed) {
    std::string warning = "two-phase reprice left open exposure after post-hoc fill updates; "
                          "downstream strategy hedge/close decisions were generated from phase1 fills";
    warning += " (open_position_qty_e8=";
    warning += std::to_string(result.openPositionQtyE8);
    if (!result.legs.empty()) {
      warning += ", leg_open_position_qty_e8=[";
      for (std::size_t i = 0u; i < result.legs.size(); ++i) {
        if (i != 0u) warning += ",";
        warning += std::to_string(result.legs[i].openPositionQtyE8);
      }
      warning += "]";
    }
    warning += ")";
    result.warnings.push_back(std::move(warning));
  }
  appendRateLimitWarnings(result, engine, hasExplicitSessions ? &legEngines : nullptr);
  result.rateLimitRejectedCommands = rateLimitRejectedCommands(engine, hasExplicitSessions ? &legEngines : nullptr);
  if (legLiquidationProblem) {
    result.status = Status::Ok;
    result.warnings.push_back(result.error.empty() ? std::string{"leg liquidated"} : result.error);
    result.error.clear();
    (void)applyStrictRateLimitStatus(result, engine, hasExplicitSessions ? &legEngines : nullptr);
    refreshArtifactCounters(engine);
    Status writeStatus = hasExplicitSessions && request.writeArtifacts ? detail::writeResult(result, engine, &legEngines) : writeArtifacts(engine);
    if (!isOk(writeStatus)) result.status = writeStatus;
    return result;
  }
  if (traderRiskCloseProblem) {
    result.status = Status::InvalidArgument;
    result.error = "trader risk stop could not force-close all open positions";
    refreshArtifactCounters(engine);
    Status writeStatus = hasExplicitSessions && request.writeArtifacts ? detail::writeResult(result, engine, &legEngines) : writeArtifacts(engine);
    if (!isOk(writeStatus)) result.status = writeStatus;
    return result;
  }
  result.status = Status::Ok;
  (void)applyStrictRateLimitStatus(result, engine, hasExplicitSessions ? &legEngines : nullptr);
  refreshArtifactCounters(engine);
  Status writeStatus = hasExplicitSessions && request.writeArtifacts ? detail::writeResult(result, engine, &legEngines) : writeArtifacts(engine);
  if (!isOk(writeStatus)) result.status = writeStatus;
  (void)reportProgress(timeline.size(), timeline.size(), "complete", progressCallback, userData);
  return result;
}

}  // namespace hft_backtest
