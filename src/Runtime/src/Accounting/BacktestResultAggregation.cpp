#include "trading_core/Algorithm/Math/Integer/Saturating.hpp"
#include "BacktestResultAggregation.hpp"

#include "BacktestMetrics.hpp"
#include "../Execution/BacktestExecutionDepth.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

namespace hft_backtest {
namespace {

std::uint64_t rateLimitRejectCount(const detail::EngineState& engine) noexcept {
  return engine.rateLimitRejectedOrders + engine.rateLimitRejectedCancels;
}

bool warningAlreadyPresent(const BacktestRunResult& result, const std::string& warning) {
  for (const std::string& row : result.warnings) {
    if (row == warning) return true;
  }
  return false;
}

std::string rateLimitWarningForEngine(const detail::EngineState& engine, std::size_t legIndex) {
  const std::uint64_t total = rateLimitRejectCount(engine);
  std::string out{};
  if (legIndex != static_cast<std::size_t>(-1)) {
    out += "leg ";
    out += std::to_string(legIndex);
    out += ": ";
  }
  out += "rate limit reached during backtest: ";
  out += std::to_string(total);
  out += total == 1u ? " command rejected" : " commands rejected";
  out += " (orders=";
  out += std::to_string(engine.rateLimitRejectedOrders);
  out += ", cancels=";
  out += std::to_string(engine.rateLimitRejectedCancels);
  out += ", first_ts_ns=";
  out += std::to_string(engine.firstRateLimitRejectTsNs);
  out += ", last_ts_ns=";
  out += std::to_string(engine.lastRateLimitRejectTsNs);
  out += ")";
  return out;
}

void appendRateLimitWarningForEngine(BacktestRunResult& result,
                                     const detail::EngineState& engine,
                                     std::size_t legIndex) noexcept {
  if (rateLimitRejectCount(engine) == 0u) return;
  try {
    const std::string warning = rateLimitWarningForEngine(engine, legIndex);
    if (!warningAlreadyPresent(result, warning)) result.warnings.push_back(warning);
  } catch (...) {
    // Warning emission is best-effort; it must not change replay status.
  }
}

BacktestDepthExecutionSummary depthSummary(const detail::EngineState& engine) noexcept {
  BacktestDepthExecutionSummary out{};
  out.mode = engine.depthExecutionMode;
  out.bboFallbackFills = engine.depthFallbackFills;
  out.firstBboFallbackTsNs = engine.firstDepthFallbackTsNs;
  if (engine.executionDepthProvider != nullptr) {
    out.rowsRead = engine.executionDepthProvider->rowsRead();
    out.readNs = engine.executionDepthProvider->readNs();
  }
  return out;
}

void addDepthSummary(BacktestDepthExecutionSummary& out,
                     const BacktestDepthExecutionSummary& in) noexcept {
  out.rowsRead += in.rowsRead;
  out.bboFallbackFills += in.bboFallbackFills;
  out.readNs += in.readNs;
  if (in.firstBboFallbackTsNs > 0 &&
      (out.firstBboFallbackTsNs == 0 || in.firstBboFallbackTsNs < out.firstBboFallbackTsNs)) {
    out.firstBboFallbackTsNs = in.firstBboFallbackTsNs;
  }
  if (in.mode == BacktestDepthExecutionMode::BboFallback) {
    out.mode = BacktestDepthExecutionMode::BboFallback;
  } else if (in.mode == BacktestDepthExecutionMode::Depth &&
             out.mode == BacktestDepthExecutionMode::BboNoDepth) {
    out.mode = BacktestDepthExecutionMode::Depth;
  }
}

void appendDepthWarningForEngine(BacktestRunResult& result,
                                 const detail::EngineState& engine,
                                 std::size_t legIndex) noexcept {
  if (engine.depthExecutionMode != BacktestDepthExecutionMode::BboFallback) return;
  try {
    std::string warning{};
    if (legIndex != static_cast<std::size_t>(-1)) {
      warning += "leg ";
      warning += std::to_string(legIndex);
      warning += ": ";
    }
    warning += "declared execution depth failed; continuing with BBO fills";
    if (!engine.depthFallbackReason.empty()) {
      warning += " (reason=";
      warning += engine.depthFallbackReason;
      warning += ")";
    }
    if (!warningAlreadyPresent(result, warning)) result.warnings.push_back(std::move(warning));
  } catch (...) {
  }
}

void mergeStrategySpreadDiagnostics(detail::JsonStrategySpreadDiagnostics& out,
                                    const detail::JsonStrategySpreadDiagnostics& in) noexcept {
  if (in.rows == 0u) return;
  if (out.rows == 0u) {
    out.minEdgeAfterCostBpsE8 = in.minEdgeAfterCostBpsE8;
    out.maxEdgeAfterCostBpsE8 = in.maxEdgeAfterCostBpsE8;
  } else {
    out.minEdgeAfterCostBpsE8 = std::min(out.minEdgeAfterCostBpsE8, in.minEdgeAfterCostBpsE8);
    out.maxEdgeAfterCostBpsE8 = std::max(out.maxEdgeAfterCostBpsE8, in.maxEdgeAfterCostBpsE8);
  }
  out.maxAbsDeviationBpsE8 = std::max(out.maxAbsDeviationBpsE8, in.maxAbsDeviationBpsE8);
  for (std::size_t kind = 0u; kind < out.decisionCounts.size(); ++kind) {
    for (std::size_t reason = 0u; reason < out.decisionCounts[kind].size(); ++reason) {
      out.decisionCounts[kind][reason] += in.decisionCounts[kind][reason];
    }
  }
  out.rows += in.rows;
}

}  // namespace

BacktestInputCoverage addCoverage(BacktestInputCoverage lhs, const BacktestInputCoverage& rhs) noexcept {
  lhs.strategyCycles += rhs.strategyCycles;
  lhs.cyclesWithTrade += rhs.cyclesWithTrade;
  lhs.cyclesWithBookTicker += rhs.cyclesWithBookTicker;
  lhs.cyclesWithPriceLimit += rhs.cyclesWithPriceLimit;
  lhs.cyclesWithDepth += rhs.cyclesWithDepth;
  lhs.cyclesWithOrderBook += rhs.cyclesWithOrderBook;
  lhs.seedTradeRows += rhs.seedTradeRows;
  lhs.seedM1Candles += rhs.seedM1Candles;
  lhs.seedM15Candles += rhs.seedM15Candles;
  lhs.seedD1Candles += rhs.seedD1Candles;
  return lhs;
}

BacktestLegSummary legSummaryFromEngine(const detail::SessionData& session,
                                        const detail::EngineState& engine) noexcept {
  BacktestLegSummary out{};
  out.sessionPath = session.sessionPath;
  out.exchange = session.exchange;
  out.market = session.market;
  out.symbol = session.symbol;
  out.makerFeeBpsE8 = engine.makerFeeBpsE8;
  out.takerFeeBpsE8 = engine.takerFeeBpsE8;
  out.marketOrderLatency = engine.latency.config.marketOrder;
  out.limitOrderLatency = engine.latency.config.limitOrder;
  out.cancelOrderLatency = engine.latency.config.cancelOrder;
  out.userDataLatency = engine.latency.config.userData;
  out.orders = engine.orders.size();
  out.fills = engine.fills.size();
  out.userEvents = engine.userEvents.size();
  out.timerWakeEvents = engine.timerWakeEvents;
  out.reduceOnlyOrders = engine.reduceOnlyOrders;
  out.reduceOnlyFills = engine.reduceOnlyFills;
  out.initialBalanceE8 = engine.initialBalanceE8;
  out.walletBalanceE8 = engine.walletBalanceE8;
  out.grossRealizedPnlE8 = engine.grossRealizedPnlE8;
  out.feesPaidE8 = engine.feesPaidE8;
  out.netRealizedPnlE8 = engine.netRealizedPnlE8;
  out.unrealizedPnlE8 = detail::unrealizedPnlE8(engine);
  out.totalPnlE8 = trading_core::saturatingAddI64(
      out.netRealizedPnlE8, out.unrealizedPnlE8);
  out.walletDeltaE8 = engine.netRealizedPnlE8;
  out.openPositionQtyE8 = engine.netQtyRaw;
  out.totalPnlCurveE8.reserve(engine.equityPoints.size());
  for (const detail::JsonEquityPoint& point : engine.equityPoints) out.totalPnlCurveE8.push_back(point.totalPnlE8);
  out.depthExecution = depthSummary(engine);
  out.replayClock.firstReceiveMonotonicNs = session.firstReceiveMonotonicNs;
  out.replayClock.lastReceiveMonotonicNs = session.lastReceiveMonotonicNs;
  out.replayClock.firstReceiveRealtimeNs = session.firstReceiveRealtimeNs;
  out.replayClock.lastReceiveRealtimeNs = session.lastReceiveRealtimeNs;
  out.replayClock.firstReplayDeliveryNs = engine.firstReplayDeliveryNs;
  out.replayClock.lastReplayDeliveryNs = engine.lastReplayDeliveryNs;
  out.replayClock.firstVenueScheduleNs = engine.firstVenueScheduleNs;
  out.replayClock.lastVenueScheduleNs = engine.lastVenueScheduleNs;
  out.replayClock.realtimeRegressions = session.realtimeRegressionCount;
  out.replayClock.monotonicNonIncreasing = session.monotonicNonIncreasingCount;
  out.replayClock.exchangeAheadOfReceive = session.exchangeAheadOfReceiveCount;
  out.replayClock.exchangeTimestampMissing = session.exchangeTimestampMissingCount;
  out.replayClock.venueScheduleAdjustedEvents = engine.venueScheduleAdjustedEvents;
  out.strategyClosed = engine.netQtyRaw == 0 && engine.pending.empty();
  return out;
}

void populateAggregateArtifactEngine(const std::vector<detail::EngineState>& engines,
                                     std::int64_t initialBalanceE8,
                                     std::int64_t finalTsNs,
                                     detail::EngineState& out) {
  out.initialBalanceE8 = initialBalanceE8;
  out.walletBalanceE8 = initialBalanceE8;
  for (const detail::EngineState& engine : engines) {
    out.orders.insert(out.orders.end(), engine.orders.begin(), engine.orders.end());
    out.fills.insert(out.fills.end(), engine.fills.begin(), engine.fills.end());
    out.strategyIndicator.insert(out.strategyIndicator.end(), engine.strategyIndicator.begin(), engine.strategyIndicator.end());
    out.strategyRange.insert(out.strategyRange.end(), engine.strategyRange.begin(), engine.strategyRange.end());
    out.strategySpread.insert(out.strategySpread.end(), engine.strategySpread.begin(), engine.strategySpread.end());
    out.executionQuality.insert(out.executionQuality.end(), engine.executionQuality.begin(), engine.executionQuality.end());
    mergeStrategySpreadDiagnostics(out.strategySpreadDiagnostics, engine.strategySpreadDiagnostics);
    out.rateLimitUsage.insert(out.rateLimitUsage.end(), engine.rateLimitUsage.begin(), engine.rateLimitUsage.end());
    out.userEvents.insert(out.userEvents.end(), engine.userEvents.begin(), engine.userEvents.end());
    out.userEventDeliveryTsNs.insert(out.userEventDeliveryTsNs.end(), engine.userEventDeliveryTsNs.begin(), engine.userEventDeliveryTsNs.end());
    out.inputCoverage = addCoverage(out.inputCoverage, engine.inputCoverage);
    out.timerWakeEvents += engine.timerWakeEvents;
    out.reduceOnlyOrders += engine.reduceOnlyOrders;
    out.reduceOnlyFills += engine.reduceOnlyFills;
    out.netQtyRaw = trading_core::saturatingAddI64(
        out.netQtyRaw, engine.netQtyRaw);
    out.grossRealizedPnlE8 = trading_core::saturatingAddI64(
        out.grossRealizedPnlE8, engine.grossRealizedPnlE8);
    out.netRealizedPnlE8 = trading_core::saturatingAddI64(
        out.netRealizedPnlE8, engine.netRealizedPnlE8);
    out.feesPaidE8 = trading_core::saturatingAddI64(
        out.feesPaidE8, engine.feesPaidE8);
    if (engine.liquidated) {
      out.liquidated = true;
      out.liquidationTsNs = engine.liquidationTsNs;
      out.liquidationEquityE8 = engine.liquidationEquityE8;
    }
    if (engine.riskStopped) {
      out.riskStopped = true;
      out.riskStopTsNs = engine.riskStopTsNs;
      out.riskStopEquityE8 = engine.riskStopEquityE8;
      out.riskStopThresholdEquityE8 = engine.riskStopThresholdEquityE8;
    }
  }
  if (out.liquidated && initialBalanceE8 > 0) {
    out.netQtyRaw = 0;
    out.netRealizedPnlE8 = -initialBalanceE8;
    out.grossRealizedPnlE8 = trading_core::saturatingAddI64(
        out.netRealizedPnlE8, out.feesPaidE8);
  }
  out.walletBalanceE8 = trading_core::saturatingAddI64(
      initialBalanceE8, out.netRealizedPnlE8);
  if (out.liquidated) out.walletBalanceE8 = 0;
  if (!engines.empty()) {
    out.strategyIndicatorProfile = engines[0].strategyIndicatorProfile;
    out.strategyIndicatorTitle = engines[0].strategyIndicatorTitle;
    out.strategyIndicatorValueLabel = engines[0].strategyIndicatorValueLabel;
    out.strategyIndicatorAuxLabel = engines[0].strategyIndicatorAuxLabel;
    out.strategyIndicatorUnit = engines[0].strategyIndicatorUnit;
  }
  detail::appendEquityPoint(out, finalTsNs);
  detail::appendRiskSnapshot(out, finalTsNs, detail::kRiskSnapshotReasonFinal);
}

void populateResultPnl(BacktestRunResult& result, const detail::EngineState& engine) noexcept {
  result.initialBalanceE8 = engine.initialBalanceE8;
  result.walletBalanceE8 = engine.walletBalanceE8;
  result.grossRealizedPnlE8 = engine.grossRealizedPnlE8;
  result.feesPaidE8 = engine.feesPaidE8;
  result.netRealizedPnlE8 = engine.netRealizedPnlE8;
  const detail::BacktestExcursionMetrics metrics = detail::calculateExcursionMetrics(engine);
  result.maxDrawdownE8 = metrics.maxDrawdownE8;
  result.mfeE8 = metrics.mfeE8;
  result.maeE8 = metrics.maeE8;
  result.realizedPnlE8 = engine.netRealizedPnlE8;
  result.unrealizedPnlE8 = detail::unrealizedPnlE8(engine);
  result.totalPnlE8 = trading_core::saturatingAddI64(
      result.realizedPnlE8, result.unrealizedPnlE8);
  result.openPositionQtyE8 = engine.netQtyRaw;
  result.strategyClosed = engine.netQtyRaw == 0;
  result.liquidated = engine.liquidated;
  result.liquidationTsNs = engine.liquidationTsNs;
  result.liquidationEquityE8 = engine.liquidationEquityE8;
  result.riskStopped = engine.riskStopped;
  result.riskStopTsNs = engine.riskStopTsNs;
  result.riskStopEquityE8 = engine.riskStopEquityE8;
  result.riskStopThresholdEquityE8 = engine.riskStopThresholdEquityE8;
  result.userEvents = engine.userEvents.size();
  result.timerWakeEvents = engine.timerWakeEvents;
  result.reduceOnlyOrders = engine.reduceOnlyOrders;
  result.reduceOnlyFills = engine.reduceOnlyFills;
  result.inputCoverage = engine.inputCoverage;
  result.depthExecution = depthSummary(engine);
  result.totalPnlCurveE8.clear();
  result.totalPnlCurveE8.reserve(engine.equityPoints.size());
  for (const detail::JsonEquityPoint& point : engine.equityPoints) {
    result.totalPnlCurveE8.push_back(point.totalPnlE8);
  }
}

void populatePortfolioResultPnl(BacktestRunResult& result,
                                const std::vector<detail::SessionData>& sessions,
                                const std::vector<detail::EngineState>& engines,
                                std::int64_t initialBalanceE8) noexcept {
  result.initialBalanceE8 = initialBalanceE8;
  result.walletBalanceE8 = initialBalanceE8;
  result.grossRealizedPnlE8 = 0;
  result.feesPaidE8 = 0;
  result.netRealizedPnlE8 = 0;
  result.realizedPnlE8 = 0;
  result.unrealizedPnlE8 = 0;
  result.totalPnlE8 = 0;
  result.openPositionQtyE8 = 0;
  result.orders = 0;
  result.fills = 0;
  result.userEvents = 0;
  result.timerWakeEvents = 0;
  result.reduceOnlyOrders = 0;
  result.reduceOnlyFills = 0;
  result.inputCoverage = BacktestInputCoverage{};
  result.depthExecution = BacktestDepthExecutionSummary{};
  result.legs.clear();
  bool closed = true;
  bool liquidated = false;
  bool riskStopped = false;
  std::int64_t liquidationTsNs = 0;
  std::int64_t liquidationEquityE8 = 0;
  std::int64_t riskStopTsNs = 0;
  std::int64_t riskStopEquityE8 = 0;
  std::int64_t riskStopThresholdEquityE8 = 0;
  for (std::size_t i = 0u; i < engines.size(); ++i) {
    const detail::EngineState& engine = engines[i];
    result.grossRealizedPnlE8 = trading_core::saturatingAddI64(
        result.grossRealizedPnlE8, engine.grossRealizedPnlE8);
    result.feesPaidE8 = trading_core::saturatingAddI64(
        result.feesPaidE8, engine.feesPaidE8);
    result.netRealizedPnlE8 = trading_core::saturatingAddI64(
        result.netRealizedPnlE8, engine.netRealizedPnlE8);
    result.unrealizedPnlE8 = trading_core::saturatingAddI64(
        result.unrealizedPnlE8, detail::unrealizedPnlE8(engine));
    result.openPositionQtyE8 = trading_core::saturatingAddI64(
        result.openPositionQtyE8, engine.netQtyRaw);
    result.orders += engine.orders.size();
    result.fills += engine.fills.size();
    result.userEvents += engine.userEvents.size();
    result.timerWakeEvents += engine.timerWakeEvents;
    result.reduceOnlyOrders += engine.reduceOnlyOrders;
    result.reduceOnlyFills += engine.reduceOnlyFills;
    result.inputCoverage = addCoverage(result.inputCoverage, engine.inputCoverage);
    addDepthSummary(result.depthExecution, depthSummary(engine));
    if (engine.netQtyRaw != 0 || !engine.pending.empty()) closed = false;
    if (engine.liquidated) {
      liquidated = true;
      liquidationTsNs = engine.liquidationTsNs;
      liquidationEquityE8 = engine.liquidationEquityE8;
    }
    if (engine.riskStopped) {
      riskStopped = true;
      riskStopTsNs = engine.riskStopTsNs;
      riskStopEquityE8 = engine.riskStopEquityE8;
      riskStopThresholdEquityE8 = engine.riskStopThresholdEquityE8;
    }
    if (i < sessions.size()) result.legs.push_back(legSummaryFromEngine(sessions[i], engine));
  }
  result.realizedPnlE8 = result.netRealizedPnlE8;
  result.totalPnlE8 = trading_core::saturatingAddI64(
      result.realizedPnlE8, result.unrealizedPnlE8);
  result.walletBalanceE8 = trading_core::saturatingAddI64(
      initialBalanceE8, result.netRealizedPnlE8);
  const detail::BacktestExcursionMetrics metrics = detail::calculatePortfolioExcursionMetrics(engines);
  result.maxDrawdownE8 = metrics.maxDrawdownE8;
  result.mfeE8 = metrics.mfeE8;
  result.maeE8 = metrics.maeE8;
  result.totalPnlCurveE8 = detail::buildPortfolioTotalPnlCurveE8(engines);
  if (liquidated && initialBalanceE8 > 0) {
    result.walletBalanceE8 = 0;
    result.grossRealizedPnlE8 = trading_core::saturatingAddI64(
        -initialBalanceE8, result.feesPaidE8);
    result.netRealizedPnlE8 = -initialBalanceE8;
    result.realizedPnlE8 = result.netRealizedPnlE8;
    result.unrealizedPnlE8 = 0;
    result.totalPnlE8 = result.netRealizedPnlE8;
    result.openPositionQtyE8 = 0;
    closed = true;
  }
  result.liquidated = liquidated;
  result.liquidationTsNs = liquidationTsNs;
  result.liquidationEquityE8 = liquidationEquityE8;
  result.riskStopped = riskStopped;
  result.riskStopTsNs = riskStopTsNs;
  result.riskStopEquityE8 = riskStopEquityE8;
  result.riskStopThresholdEquityE8 = riskStopThresholdEquityE8;
  result.strategyClosed = closed;
}

void appendRateLimitWarnings(BacktestRunResult& result,
                             const detail::EngineState& engine,
                             const std::vector<detail::EngineState>* legEngines) noexcept {
  if (legEngines && !legEngines->empty()) {
    for (std::size_t i = 0u; i < legEngines->size(); ++i) {
      appendRateLimitWarningForEngine(result, (*legEngines)[i], i);
    }
    return;
  }
  appendRateLimitWarningForEngine(result, engine, static_cast<std::size_t>(-1));
}

void appendDepthExecutionWarnings(BacktestRunResult& result,
                                  const detail::EngineState& engine,
                                  const std::vector<detail::EngineState>* legEngines) noexcept {
  if (legEngines && !legEngines->empty()) {
    for (std::size_t i = 0u; i < legEngines->size(); ++i) {
      appendDepthWarningForEngine(result, (*legEngines)[i], i);
    }
    return;
  }
  appendDepthWarningForEngine(result, engine, static_cast<std::size_t>(-1));
}

std::uint64_t rateLimitRejectedCommands(const detail::EngineState& engine,
                                        const std::vector<detail::EngineState>* legEngines) noexcept {
  std::uint64_t out = 0u;
  if (legEngines && !legEngines->empty()) {
    for (const detail::EngineState& legEngine : *legEngines) out += rateLimitRejectCount(legEngine);
    return out;
  }
  return rateLimitRejectCount(engine);
}

bool applyStrictRateLimitStatus(BacktestRunResult& result,
                                const detail::EngineState& engine,
                                const std::vector<detail::EngineState>* legEngines) noexcept {
  result.rateLimitRejectedCommands = rateLimitRejectedCommands(engine, legEngines);
  result.rateLimitStrictFailure = false;
  if (!result.rateLimitsEnabled || !result.strictRateLimitRejects || result.rateLimitRejectedCommands == 0u) {
    return false;
  }
  result.rateLimitStrictFailure = true;
  result.status = Status::InvalidArgument;
  try {
    result.error = "strict rate-limit mode rejected backtest: ";
    result.error += std::to_string(result.rateLimitRejectedCommands);
    result.error += result.rateLimitRejectedCommands == 1u ? " command was locally rate-limited"
                                                           : " commands were locally rate-limited";
  } catch (...) {
    result.error = "strict rate-limit mode rejected backtest";
  }
  return true;
}

}  // namespace hft_backtest
