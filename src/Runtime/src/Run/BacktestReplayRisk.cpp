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

bool stopOnStrictRateLimitReject(BacktestRunContext& context) noexcept {
  auto& result = context.result;
  auto& hasExplicitSessions = context.hasExplicitSessions;
  auto& engine = context.engine;
  auto& legEngines = context.legEngines;

  if (!result.rateLimitsEnabled || !result.strictRateLimitRejects) return false;
  const std::uint64_t rejected = rateLimitRejectedCommands(engine, hasExplicitSessions ? &legEngines : nullptr);
  if (rejected == 0u) return false;
  result.rateLimitRejectedCommands = rejected;
  (void)applyStrictRateLimitStatus(result, engine, hasExplicitSessions ? &legEngines : nullptr);
  (void)writeCurrentArtifacts(context);
  return true;
}

bool tripHardLiquidation(BacktestRunContext& context, std::int64_t tsNs) noexcept {
  auto& result = context.result;
  auto& hasExplicitSessions = context.hasExplicitSessions;
  auto& portfolioInitialBalance = context.portfolioInitialBalance;
  auto& engine = context.engine;
  auto& legEngines = context.legEngines;
  auto& control = context.control;
  auto& legLiquidationProblem = context.legLiquidationProblem;

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
        control.killActive = trading_core::kAlgoFlagTrue;
        control.haltReason = trading_core::kHaltReasonManualKill;
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
    control.killActive = trading_core::kAlgoFlagTrue;
    control.haltReason = trading_core::kHaltReasonManualKill;
    result.liquidated = true;
    result.liquidationTsNs = tsNs;
    result.liquidationEquityE8 = equity;
    return true;
  }
  if (engine.liquidated) return true;
  const std::int64_t equity = detail::equityE8(engine);
  if (equity > 0) return false;
  detail::hardLiquidate(engine, tsNs, equity);
  control.killActive = trading_core::kAlgoFlagTrue;
  control.haltReason = trading_core::kHaltReasonManualKill;
  result.liquidated = true;
  result.liquidationTsNs = tsNs;
  result.liquidationEquityE8 = equity;
  return true;
}

bool tripTraderRiskStop(BacktestRunContext& context, std::int64_t tsNs, bool forceRiskCheck) {
  auto& result = context.result;
  auto& hasExplicitSessions = context.hasExplicitSessions;
  auto& engine = context.engine;
  auto& legEngines = context.legEngines;
  auto& control = context.control;
  auto& riskAccounts = context.riskAccounts;
  auto& riskAccountCount = context.riskAccountCount;
  auto& traderRiskState = context.traderRiskState;
  auto& traderRiskCloseProblem = context.traderRiskCloseProblem;

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
  if (!trading_core::acquirePortfolioRiskEvaluationSlot(
          traderRiskState, riskNow, forceRiskCheck)) {
    return false;
  }
  const trading_core::PortfolioRiskEvaluation eval =
      trading_core::evaluatePortfolioRisk(
          traderRiskState, riskAccounts.data(), riskAccountCount, riskNow);
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
  control.killActive = trading_core::kAlgoFlagTrue;
  control.haltReason = trading_core::kHaltReasonRiskLimit;
  result.riskStopped = true;
  result.riskStopTsNs = tsNs;
  result.riskStopEquityE8 = eval.equityE8;
  result.riskStopThresholdEquityE8 = eval.thresholdEquityE8;
  return true;
}


}  // namespace hft_backtest::run_detail
