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

bool runCycleMulti(const trading_core::StrategyDescriptor& descriptor,
                   const BacktestStrategyInputPolicy& inputPolicy,
                   void* state,
                   detail::EngineView engines,
                   std::span<const StrategyInstrumentSpec> legInstruments,
                   MultiLegCycleScratch& scratch,
                   const trading_core::SymbolShardConfig& shard,
                   const trading_core::ShardControlState& control,
                   std::uint32_t eventLegIndex,
                   std::int64_t tsNs,
                   const BacktestRunRequest& request,
                   const cxet::composite::UserDataEvent* userEvent) {
  if (!footprintSupported(descriptor)) return false;
  if (engines.empty()) return true;
  for (const detail::EngineState& engine : engines) if (engine.simulationEvidenceFailed) return false;
  for (const detail::EngineState& engine : engines) {
    if (engine.liquidated || engine.riskStopped ||
        (engine.capturedSourceManaged && !engine.capturedSourceMember)) return true;
  }
  const std::size_t legCount = engines.size() < trading_core::kStrategyRuntimeMaxLegs ? engines.size() : trading_core::kStrategyRuntimeMaxLegs;
  const std::uint64_t configuredTradableMask = backtestTradableLegMask(request, legCount);
  const std::uint64_t sourceFreshMask = backtestSourceFreshLegMask(
      inputPolicy, engines, legCount, tsNs);
  const trading_core::strategy::StrategyInputSourceMaskState freshness =
      trading_core::strategy::strategyInputSourceMaskState(legCount, configuredTradableMask, sourceFreshMask);
  std::uint64_t readinessMask = freshness.sourceFreshLegMask;
  if (!freshness.globallyDegraded && legCount <= 2u) {
    readinessMask = trading_core::strategy::strategyInputLegMaskForCount(legCount);
  } else if (freshness.globallyDegraded && readinessMask == 0u) {
    readinessMask = trading_core::strategy::strategyInputLegMaskForCount(legCount);
  }
  for (std::size_t i = 0u; i < legCount; ++i) {
    if ((readinessMask & (std::uint64_t{1} << i)) == 0u) continue;
    if (!backtestStrategySnapshotReady(inputPolicy, engines[i].snapshot)) return true;
  }
  const trading_core::ShardControlState effectiveControl =
      backtestControlWithInputFreshness(control, freshness);
  if (scratch.legCount != legCount) scratch.reset(legCount);
  scratch.refreshDirty(engines, legInstruments);
  trading_core::StrategyContext ctx{};
  ctx.shard = &shard;
  ctx.control = &effectiveControl;
  ctx.marketData.snapshots = scratch.snapshots.data();
  ctx.marketData.count = legCount;
  ctx.candles.candles = scratch.candles.data();
  ctx.candles.count = legCount;
  ctx.instrumentSpecs.legs = scratch.instruments.data();
  ctx.instrumentSpecs.count = legCount;
  ctx.legMarkets.legs = scratch.markets.data();
  ctx.legMarkets.count = legCount;
  ctx.orderStates.legs = scratch.orderViews.data();
  ctx.orderStates.count = legCount;
  ctx.positionStates.legs = scratch.positions.data();
  ctx.positionStates.count = legCount;
  ctx.accountStates.legs = scratch.accounts.data();
  ctx.accountStates.count = legCount;
  setBacktestTradePolicy(ctx, request, legCount, freshness.tradableLegMask);
  const std::size_t defaultExecutionLeg =
      routeLegIndex(ctx, trading_core::kDefaultExecutionRouteHandle);
  if (defaultExecutionLeg >= legCount ||
      !trading_core::strategy::bindDefaultExecutionMarketAliases(ctx)) {
    return false;
  }
  ctx.instrumentSpec = scratch.instruments[defaultExecutionLeg];
  ctx.orderState = scratch.orderViews[defaultExecutionLeg];
  ctx.positionState = scratch.positions[defaultExecutionLeg];
  ctx.accountState = scratch.accounts[defaultExecutionLeg];
  ctx.userEvent = userEvent;
  // Admission is owned by the simulated Backtest runtime; this is not a
  // generic classifier for exchange wire events.
  ctx.userEventAdmittedSemanticMask =
      backtestSimulatedUserEventSemanticMask(userEvent);
  ctx.hasUserEvent = ctx.userEventAdmittedSemanticMask != 0u;
  if (eventLegIndex < engines.size()) bindBacktestUserEventIdentity(ctx, engines[eventLegIndex]);
  ctx.eventLegIndex = eventLegIndex;
  ctx.hasEventLeg = eventLegIndex < legCount;
  ctx.nowMono.raw = static_cast<std::uint64_t>(tsNs > 0 ? tsNs : 0);
  ctx.nowUtc = replayUtcFromMonotonic(tsNs, engines[0].replayUtcOffsetNs);
  for (std::size_t i = 0u; i < legCount; ++i) {
    if (tsNs > engines[i].lastReplayMonotonicNs)
      engines[i].lastReplayMonotonicNs = tsNs;
  }
  for (std::size_t i = 0u; i < legCount; ++i) {
    scratch.warmupViews[i] = backtestWarmupView(engines[i]);
  }
  ctx.orderPathWarmup = scratch.warmupViews[defaultExecutionLeg];
  ctx.orderPathWarmups.routes = scratch.warmupViews.data();
  ctx.orderPathWarmups.count = legCount;
  if (userEvent && eventLegIndex < legInstruments.size()) {
    recordExecutionQualityFill(engines[0], ctx, eventLegIndex, tsNs, legInstruments[eventLegIndex]);
  }
  if (userEvent && userEvent->orderProblem != canon::OrderProblem::None && eventLegIndex < engines.size()) {
    engines[eventLegIndex].pendingProblem = trading_core::StrategyProblemEvent{};
    engines[eventLegIndex].pendingProblem.problem = userEvent->orderProblem;
    engines[eventLegIndex].pendingProblem.valid = true;
    engines[eventLegIndex].hasPendingProblem = true;
  }
  detail::EngineState* problemEngine = nullptr;
  if (eventLegIndex < engines.size() && engines[eventLegIndex].hasPendingProblem) {
    problemEngine = &engines[eventLegIndex];
  } else {
    for (detail::EngineState& candidate : engines) {
      if (candidate.hasPendingProblem) {
        problemEngine = &candidate;
        break;
      }
    }
  }
  if (problemEngine) {
    ctx.problemEvent = &problemEngine->pendingProblem;
    ctx.hasProblemEvent = true;
    problemEngine->hasPendingProblem = false;
  }
  bool includeRateLimits = ctx.hasUserEvent || ctx.hasProblemEvent;
  for (const detail::EngineState& engine : engines) {
    includeRateLimits = includeRateLimits || engine.rateLimitSnapshotRequested;
  }
  if (includeRateLimits) {
    ctx.rateLimits = trading_core::StrategyRateLimitBook{};
    for (std::size_t i = 0u; i < legCount; ++i) {
      trading_core::StrategyRateLimitBook legLimits = engines[i].quotaOwner().observationBook(
          trading_core::ExecutionRouteHandle{static_cast<std::uint32_t>(i + 1u)},
          engines[i].exchange,
          engines[i].market,
          1u,
          ctx.nowMono);
      for (std::uint8_t j = 0u; j < legLimits.count && ctx.rateLimits.count < ctx.rateLimits.buckets.size(); ++j) {
        ctx.rateLimits.buckets[ctx.rateLimits.count++] = legLimits.buckets[j];
      }
      for (std::uint8_t j = 0u; j < legLimits.actionCount && ctx.rateLimits.actionCount < ctx.rateLimits.actions.size(); ++j) {
        ctx.rateLimits.actionRoutes[ctx.rateLimits.actionCount] = legLimits.actionRoutes[j];
        ctx.rateLimits.actions[ctx.rateLimits.actionCount++] = legLimits.actions[j];
      }
      engines[i].rateLimitSnapshotRequested = false;
    }
  }
  ctx.state.orders.executionLegs = scratch.executionViews.data();
  ctx.state.orders.executionLegCount = legCount;
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
  recordInputCoverage(engines[0]);
  trading_core::StrategyDecisionTrace trace{};
  trading_core::StrategyExecutionTraceBatch executionTrace{};
  const bool captureIndicatorTrace = strategyIndicatorEnabled(engines[0]);
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
  if (ctx.io.requestRateLimitSnapshot) {
    for (detail::EngineState& engine : engines) engine.rateLimitSnapshotRequested = true;
  }
  const bool newOrderEmissionBlocked =
      !trading_core::canEmitNewOrder(effectiveControl);
  const strategy_intent_lowering::StrategyTransactionAdmission admission =
      transactionCycle
      ? transactionAdmission(
            ctx, legCount, request, newOrderEmissionBlocked)
      : strategy_intent_lowering::StrategyTransactionAdmission{};
  if (newOrderEmissionBlocked && !transactionCycle) {
    retainCancelOutputsWhenNewOrdersBlocked(ctx.io);
  }
  const bool warmupGateHeldRealOrders = !newOrderEmissionBlocked &&
      canOrderFailureRollback &&
      applyBacktestWarmupGate(ctx, engines, legCount, request);
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
    if (captureIndicatorTrace) appendStrategyIndicator(engines[0], tsNs, trace);
    if (captureRangeTrace) appendStrategyRange(engines[0], tsNs, trace);
    if (captureSpreadDiagnostics) recordStrategySpread(engines[0], tsNs, trace, captureSpreadRows);
  }
  if (ctx.executionTrace) recordExecutionQualityActions(engines[0], tsNs, executionTrace);
  if (ctx.io.transactionCount != 0u) {
    if (!applyTransactionCycle(
            descriptor, state, ctx,
            engines.first(legCount),
            tsNs, admission)) return false;
  } else {
    applyToLegs(
        engines.first(legCount),
        ctx, tsNs, request);
  }
  for (const auto& engine : engines) if (engine.simulationEvidenceFailed) return false;
  return true;
}


}  // namespace hft_backtest
