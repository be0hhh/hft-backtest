#include "trading_core/Algorithm/Math/Integer/Saturating.hpp"
#include "../BacktestStrategyRuntime.hpp"

#include "../../Config/BacktestRunConfig.hpp"
#include "../Execution/BacktestStrategyIntentLowering.hpp"
#include "../Diagnostics/BacktestStrategyTrace.hpp"

#include "trading_core/Strategy/Context/StrategyContextView.hpp"
#include "trading_core/Strategy/Orders/WarmupIntents.hpp"
#include "trading_core/Strategy/StrategyInputFreshness.hpp"
#include "trading_core/Strategy/StrategyMarketData.hpp"

#include <algorithm>
#include <limits>
#include <string>

#include "../Internal/BacktestStrategyRuntimeHelpers.hpp"

namespace hft_backtest::strategy_runtime_detail {

}  // namespace hft_backtest::strategy_runtime_detail

namespace hft_backtest {
using strategy_intent_lowering::applyToLegs;
using strategy_intent_lowering::applyToSingleLeg;
using strategy_intent_lowering::applyTransactionCycle;
using strategy_intent_lowering::bindTransactionLegs;
using strategy_intent_lowering::configuredDefaultExecutionRoute;
using strategy_intent_lowering::configuredPrimaryLegIndex;
using strategy_intent_lowering::footprintSupported;
using strategy_intent_lowering::validateOutput;
using strategy_intent_lowering::routeLegIndex;
using strategy_intent_lowering::routeTradable;
using strategy_intent_lowering::transactionAdmission;
using strategy_runtime_detail::backtestWarmupView;
using strategy_runtime_detail::applyBacktestWarmupGate;
using strategy_runtime_detail::setBacktestTradePolicy;
using strategy_runtime_detail::backtestControlWithInputFreshness;
using strategy_runtime_detail::backtestSimulatedUserEventSemanticMask;
using strategy_runtime_detail::backtestTradableLegMask;
using strategy_runtime_detail::bindBacktestStrategyStateView;
using strategy_runtime_detail::retainCancelOutputsWhenNewOrdersBlocked;

bool runCycle(const trading_core::StrategyDescriptor& descriptor,
              const BacktestStrategyInputPolicy& inputPolicy,
              void* state,
              detail::EngineState& engine,
              const StrategyInstrumentSpec& instrument,
              SingleLegCycleScratch& scratch,
              const trading_core::SymbolShardConfig& shard,
              const trading_core::ShardControlState& control,
              std::int64_t tsNs,
              const BacktestRunRequest& request,
              const cxet::composite::UserDataEvent* userEvent) {
  if (!footprintSupported(descriptor)) return false;
  if (engine.liquidated || engine.riskStopped) return true;
  if (!backtestStrategySnapshotReady(inputPolicy, engine.snapshot)) return true;
  const std::uint64_t configuredTradableMask = backtestTradableLegMask(request, 1u);
  const std::uint64_t sourceFreshMask =
      backtestSourceFreshLegMask(inputPolicy, &engine, 1u, tsNs);
  const trading_core::strategy::StrategyInputSourceMaskState freshness =
      trading_core::strategy::strategyInputSourceMaskState(1u, configuredTradableMask, sourceFreshMask);
  const trading_core::ShardControlState effectiveControl =
      backtestControlWithInputFreshness(control, freshness);
  const std::size_t orderViewCount = detail::visibleOrders(engine, scratch.orderStorage.data(), scratch.orderStorage.size());
  trading_core::StrategyContext ctx{};
  ctx.shard = &shard;
  ctx.control = &effectiveControl;
  ctx.snapshot = &engine.snapshot;
  ctx.marketData.snapshots = &engine.snapshot;
  ctx.marketData.count = 1;
  ctx.candle = engine.candle;
  ctx.candles.candles = &engine.candle;
  ctx.candles.count = 1;
  ctx.instrumentSpec = instrument;
  ctx.instrumentSpecs.legs = &instrument;
  ctx.instrumentSpecs.count = 1u;
  ctx.legMarkets.legs = &engine.market;
  ctx.legMarkets.count = 1u;
  ctx.orderState.orders = scratch.orderStorage.data();
  ctx.orderState.count = orderViewCount;
  ctx.orderStates.legs = &ctx.orderState;
  ctx.orderStates.count = 1u;
  ctx.positionState = engine.position;
  ctx.positionStates.legs = &ctx.positionState;
  ctx.positionStates.count = 1u;
  ctx.accountState = accountStateFromEngine(engine);
  ctx.accountStates.legs = &ctx.accountState;
  ctx.accountStates.count = 1u;
  setBacktestTradePolicy(ctx, request, 1u, freshness.tradableLegMask);
  ctx.userEvent = userEvent;
  // Admission is owned by the simulated Backtest runtime; this is not a
  // generic classifier for exchange wire events.
  ctx.userEventAdmittedSemanticMask =
      backtestSimulatedUserEventSemanticMask(userEvent);
  ctx.hasUserEvent = ctx.userEventAdmittedSemanticMask != 0u;
  ctx.nowMono.raw = static_cast<std::uint64_t>(tsNs > 0 ? tsNs : 0);
  ctx.nowUtc = replayUtcFromMonotonic(tsNs, engine.replayUtcOffsetNs);
  if (tsNs > engine.lastReplayMonotonicNs) engine.lastReplayMonotonicNs = tsNs;
  scratch.warmupViews[0] = backtestWarmupView(engine);
  ctx.orderPathWarmup = scratch.warmupViews[0];
  ctx.orderPathWarmups.routes = scratch.warmupViews.data();
  ctx.orderPathWarmups.count = 1u;
  if (userEvent) {
    recordExecutionQualityFill(engine, *userEvent, 0u, tsNs, instrument);
  }
  if (userEvent && userEvent->orderProblem != canon::OrderProblem::None) {
    engine.pendingProblem = trading_core::StrategyProblemEvent{};
    engine.pendingProblem.problem = userEvent->orderProblem;
    engine.pendingProblem.valid = true;
    engine.hasPendingProblem = true;
  }
  if (engine.hasPendingProblem) {
    ctx.problemEvent = &engine.pendingProblem;
    ctx.hasProblemEvent = true;
    engine.hasPendingProblem = false;
  }
  if (engine.rateLimitSnapshotRequested || ctx.hasUserEvent || ctx.hasProblemEvent) {
    ctx.rateLimits = engine.quotas.observationBook(trading_core::kDefaultExecutionRouteHandle,
                                                    engine.exchange,
                                                    engine.market,
                                                    1u,
                                                    ctx.nowMono);
    engine.rateLimitSnapshotRequested = false;
  }
  bindBacktestStrategyStateView(ctx);
  ctx.userEventRecvMono = ctx.nowMono;
  ctx.userEventRecvUtc = ctx.nowUtc;
  ctx.initialOrderSeedAllowed = true;
  const trading_core::StrategyRuntimeFootprint& footprint =
      descriptor.footprint;
  ctx.io.orderOut = Span<trading_core::OrderIntent>(
      scratch.orderBuf.data(), footprint.maxOrders);
  ctx.io.cancelOut = Span<trading_core::CancelIntent>(
      scratch.cancelBuf.data(), footprint.maxCancels);
  ctx.io.commandOut = Span<trading_core::StrategyCommandIntent>(
      scratch.commandBuf.data(),
      trading_core::strategyRuntimeCommandCapacity(footprint));
  ctx.io.transactionOut = Span<trading_core::StrategyTransactionIntent>(
      scratch.transactionBuf.data(), footprint.maxTransactions);
  ctx.io.clear();
  recordInputCoverage(engine);
  trading_core::StrategyDecisionTrace trace{};
  trading_core::StrategyExecutionTraceBatch executionTrace{};
  const bool captureIndicatorTrace = strategyIndicatorEnabled(engine);
  const bool captureRangeTrace = emitsPriceRangeTrace(descriptor);
  const bool captureSpreadDiagnostics = emitsArbSpreadTrace(descriptor);
  const bool captureSpreadRows = request.captureStrategySpread && captureSpreadDiagnostics;
  if (captureIndicatorTrace || captureRangeTrace || captureSpreadDiagnostics) {
    trading_core::initStrategyDecisionTrace(trace, ctx.nowMono.raw);
    ctx.decisionTrace = &trace;
  }
  if (captureSpreadDiagnostics) {
    executionTrace.clear();
    ctx.executionTrace = &executionTrace;
  }
  const bool canOrderFailureRollback =
      descriptor.captureState && descriptor.restoreAfterOrderFailure;
  const bool canCapacityRollback =
      descriptor.captureState && descriptor.rollbackCapacityCycle;
  trading_core::StrategyStateSnapshot before{};
  if (canOrderFailureRollback || canCapacityRollback) {
    descriptor.captureState(state, before);
  }
  const bool ok = descriptor.runCycle(state, ctx);
  if (ctx.io.capacityExceeded) {
    if (canCapacityRollback) {
      descriptor.rollbackCapacityCycle(state, before);
    }
    ctx.io.clear();
    return false;
  }
  if (!ok) {
    ctx.io.clear();
    return false;
  }
  if (!validateOutput(ctx)) {
    if (ctx.io.capacityExceeded && canCapacityRollback) {
      descriptor.rollbackCapacityCycle(state, before);
    } else if (canOrderFailureRollback) {
      descriptor.restoreAfterOrderFailure(state, before);
    } else if (canCapacityRollback) {
      descriptor.rollbackCapacityCycle(state, before);
    }
    ctx.io.clear();
    return false;
  }
  const bool transactionCycle = ctx.io.transactionCount != 0u;
  if (!bindTransactionLegs(ctx)) {
    if (canOrderFailureRollback) {
      descriptor.restoreAfterOrderFailure(state, before);
    } else if (canCapacityRollback) {
      descriptor.rollbackCapacityCycle(state, before);
    }
    ctx.io.clear();
    return false;
  }
  if (ctx.io.requestRateLimitSnapshot) engine.rateLimitSnapshotRequested = true;
  const bool newOrderEmissionBlocked =
      !trading_core::canEmitNewOrder(effectiveControl);
  const strategy_intent_lowering::StrategyTransactionAdmission admission =
      transactionCycle
      ? transactionAdmission(ctx, 1u, request, newOrderEmissionBlocked)
      : strategy_intent_lowering::StrategyTransactionAdmission{};
  if (newOrderEmissionBlocked && !transactionCycle) {
    retainCancelOutputsWhenNewOrdersBlocked(ctx.io);
  }
  const bool warmupGateHeldRealOrders = !newOrderEmissionBlocked &&
      canOrderFailureRollback &&
      applyBacktestWarmupGate(ctx, &engine, 1u, request);
  if (warmupGateHeldRealOrders && canOrderFailureRollback) {
    descriptor.restoreAfterOrderFailure(state, before);
    if (transactionCycle) ctx.io.transactionCount = 0u;
  }
  if (captureIndicatorTrace || captureRangeTrace || captureSpreadDiagnostics) {
    std::uint32_t orderedOrderCount = 0u;
    std::uint32_t orderedCancelCount = 0u;
    for (std::size_t i = 0u; i < ctx.io.commandCount; ++i) {
      if (scratch.commandBuf[i].kind == trading_core::StrategyCommandIntentKind::Order ||
          scratch.commandBuf[i].kind == trading_core::StrategyCommandIntentKind::WarmupOrder) ++orderedOrderCount;
      else if (scratch.commandBuf[i].kind == trading_core::StrategyCommandIntentKind::Cancel) ++orderedCancelCount;
    }
    trace.orderCount = static_cast<std::uint32_t>(ctx.io.orderCount) + orderedOrderCount;
    trace.cancelCount = static_cast<std::uint32_t>(ctx.io.cancelCount) + orderedCancelCount;
    if (captureIndicatorTrace) appendStrategyIndicator(engine, tsNs, trace);
    if (captureRangeTrace) appendStrategyRange(engine, tsNs, trace);
    if (captureSpreadDiagnostics) recordStrategySpread(engine, tsNs, trace, captureSpreadRows);
  }
  if (ctx.executionTrace) recordExecutionQualityActions(engine, tsNs, executionTrace);
  if (ctx.io.transactionCount != 0u) {
    if (!applyTransactionCycle(
            descriptor, state, ctx,
            Span<detail::EngineState>(&engine, 1u),
            tsNs, admission)) return false;
  } else {
    applyToSingleLeg(engine, ctx, tsNs, engine.latency, request);
  }
  return true;
}


}  // namespace hft_backtest
