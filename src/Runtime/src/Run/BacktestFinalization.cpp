#include "hft_backtest/Backtest.hpp"

#include "../Engine/BacktestEngine.hpp"
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

#include "Internal/BacktestRunContext.hpp"
#include "Internal/BacktestRunHelpers.hpp"

namespace hft_backtest::run_detail {

void finalizeBacktestRun(BacktestRunContext& context) {
  auto& result = context.result;
  auto& request = context.request;
  auto& progressCallback = context.progressCallback;
  auto& userData = context.userData;
  auto& hasExplicitSessions = context.hasExplicitSessions;
  auto& portfolioInitialBalance = context.portfolioInitialBalance;
  auto& sessions = context.sessions;
  auto& session = context.session;
  auto& instrument = context.instrument;
  auto& legInstruments = context.legInstruments;
  auto& engine = context.engine;
  auto& legEngines = context.legEngines;
  auto& aggregateEngineStorage = context.aggregateEngineStorage;
  auto& artifactEngine = context.artifactEngine;
  auto& timeline = context.timeline;
  auto& traderRiskCloseProblem = context.traderRiskCloseProblem;
  auto& legLiquidationProblem = context.legLiquidationProblem;
  const auto finalizeStart = std::chrono::steady_clock::now();
  const std::int64_t finalTsNs = nextReplayTimestamp(currentReplayTsNs(context));
  if (hasExplicitSessions) {
    if (!legEngines.empty()) {
      calculateExecutionQualityDeadlines(
          legEngines[0], sessions.data(), legInstruments.data(), legInstruments.size());
    }
    for (detail::EngineState& legEngine : legEngines) {
      finalizeExecutionQuality(legEngine);
      detail::appendRateLimitUsageSnapshot(legEngine, finalTsNs);
      detail::appendEquityPoint(legEngine, finalTsNs);
      detail::appendRiskSnapshot(legEngine, finalTsNs, detail::kRiskSnapshotReasonFinal);
    }
    populatePortfolioResultPnl(result, sessions, legEngines, portfolioInitialBalance);
    try {
      aggregateEngineStorage = std::make_unique<detail::EngineState>();
      populateAggregateArtifactEngine(
          legEngines, portfolioInitialBalance, finalTsNs,
          *aggregateEngineStorage);
      artifactEngine = aggregateEngineStorage.get();
    } catch (...) {
      result.status = Status::IoError;
      result.error = "failed to allocate aggregate backtest engine state";
      (void)writeEmptyArtifacts(context.artifacts);
      return;
    }
  } else {
    calculateExecutionQualityDeadlines(engine, &session, &instrument, 1u);
    finalizeExecutionQuality(engine);
    detail::appendRateLimitUsageSnapshot(engine, finalTsNs);
    detail::appendEquityPoint(engine, finalTsNs);
    detail::appendRiskSnapshot(engine, finalTsNs, detail::kRiskSnapshotReasonFinal);
    populateResultPnl(result, engine);
    result.orders = engine.orders.size();
    result.fills = engine.fills.size();
  }
  const detail::EngineState& finalArtifactEngine = *artifactEngine;
  appendRateLimitWarnings(
      result, finalArtifactEngine,
      hasExplicitSessions ? &legEngines : nullptr);
  appendDepthExecutionWarnings(
      result, finalArtifactEngine,
      hasExplicitSessions ? &legEngines : nullptr);
  result.rateLimitRejectedCommands = rateLimitRejectedCommands(
      finalArtifactEngine, hasExplicitSessions ? &legEngines : nullptr);
  result.performance.finalizeNs = elapsedNs(finalizeStart);
  markTotalTime(context.artifacts);
  if (legLiquidationProblem) {
    result.status = Status::Ok;
    result.warnings.push_back(result.error.empty() ? std::string{"leg liquidated"} : result.error);
    result.error.clear();
    (void)applyStrictRateLimitStatus(
        result, finalArtifactEngine,
        hasExplicitSessions ? &legEngines : nullptr);
    refreshArtifactCounters(context.artifacts, finalArtifactEngine);
    Status writeStatus = hasExplicitSessions && request.writeArtifacts
        ? detail::writeResult(result, finalArtifactEngine, &legEngines)
        : writeArtifacts(context.artifacts, finalArtifactEngine);
    if (!isOk(writeStatus)) result.status = writeStatus;
    return;
  }
  if (traderRiskCloseProblem) {
    result.status = Status::InvalidArgument;
    result.error = "trader risk stop could not force-close all open positions";
    refreshArtifactCounters(context.artifacts, finalArtifactEngine);
    Status writeStatus = hasExplicitSessions && request.writeArtifacts
        ? detail::writeResult(result, finalArtifactEngine, &legEngines)
        : writeArtifacts(context.artifacts, finalArtifactEngine);
    if (!isOk(writeStatus)) result.status = writeStatus;
    return;
  }
  result.status = Status::Ok;
  (void)applyStrictRateLimitStatus(
      result, finalArtifactEngine,
      hasExplicitSessions ? &legEngines : nullptr);
  refreshArtifactCounters(context.artifacts, finalArtifactEngine);
  Status writeStatus = hasExplicitSessions && request.writeArtifacts
      ? detail::writeResult(result, finalArtifactEngine, &legEngines)
      : writeArtifacts(context.artifacts, finalArtifactEngine);
  if (!isOk(writeStatus)) result.status = writeStatus;
  (void)reportProgress(timeline.size(), timeline.size(), "complete", progressCallback, userData);
  return;
}


}  // namespace hft_backtest::run_detail
