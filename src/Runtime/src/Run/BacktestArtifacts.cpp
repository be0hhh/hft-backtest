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
#include "trading_core/Strategy/Risk/PortfolioRisk.hpp"
#include "trading_core/Strategy/GeneratedStrategyRegistry.hpp"
#include "trading_core/Strategy/StrategyMarketData.hpp"

#include <array>
#include <algorithm>
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



void refreshArtifactCounters(BacktestArtifactContext& context, const detail::EngineState& engine) noexcept {
  auto& result = context.result;

  result.strategySpreadRows = engine.strategySpreadDiagnostics.rows != 0u
      ? engine.strategySpreadDiagnostics.rows
      : engine.strategySpread.size();
}

Status writeArtifacts(BacktestArtifactContext& context, const detail::EngineState& engine) noexcept {
  auto& result = context.result;
  auto& request = context.request;

  refreshArtifactCounters(context, engine);
  if (!request.writeArtifacts) return Status::Ok;
  return detail::writeResult(result, engine);
}

Status writeEmptyArtifacts(BacktestArtifactContext& context) noexcept {
  auto& request = context.request;

  if (!request.writeArtifacts) return Status::Ok;
  try {
    const auto empty = std::make_unique<detail::EngineState>();
    return writeArtifacts(context, *empty);
  } catch (...) {
    return Status::IoError;
  }
}

Status writeCurrentArtifacts(BacktestRunContext& context) noexcept {
  auto& result = context.result;
  auto& request = context.request;
  auto& hasExplicitSessions = context.hasExplicitSessions;
  auto& portfolioInitialBalance = context.portfolioInitialBalance;
  auto& sessions = context.sessions;
  auto& engine = context.engine;
  auto& legEngines = context.legEngines;
  if (hasExplicitSessions) {
    const std::int64_t tsNs = currentReplayTsNs(context);
    try {
      for (detail::EngineState& legEngine : legEngines) detail::appendRateLimitUsageSnapshot(legEngine, tsNs);
    } catch (...) {
    }
    populatePortfolioResultPnl(result, sessions, legEngines, portfolioInitialBalance);
    appendRateLimitWarnings(result, engine, &legEngines);
    appendDepthExecutionWarnings(result, engine, &legEngines);
    result.rateLimitRejectedCommands = rateLimitRejectedCommands(engine, &legEngines);
    try {
      auto aggregate = std::make_unique<detail::EngineState>();
      populateAggregateArtifactEngine(legEngines, portfolioInitialBalance, tsNs, *aggregate);
      refreshArtifactCounters(context.artifacts, *aggregate);
      return request.writeArtifacts ? detail::writeResult(result, *aggregate, &legEngines) : Status::Ok;
    } catch (...) {
      return Status::IoError;
    }
  }
  try {
    detail::appendRateLimitUsageSnapshot(engine, currentReplayTsNs(context));
  } catch (...) {
  }
  populateResultPnl(result, engine);
  appendRateLimitWarnings(result, engine);
  appendDepthExecutionWarnings(result, engine);
  result.rateLimitRejectedCommands = rateLimitRejectedCommands(engine);
  result.orders = engine.orders.size();
  result.fills = engine.fills.size();
  return writeArtifacts(context.artifacts, engine);
}


}  // namespace hft_backtest::run_detail
