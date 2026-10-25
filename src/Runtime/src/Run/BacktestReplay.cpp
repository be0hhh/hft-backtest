#include "hft_backtest/Backtest.hpp"

#include "../Engine/BacktestEngine.hpp"
#include "../Engine/BacktestEngineSetup.hpp"
#include "../Execution/Model/BacktestExecutionDepth.hpp"
#include "../Accounting/BacktestResultAggregation.hpp"
#include "../Config/BacktestRunConfig.hpp"
#include "../Config/Strategy/BacktestStrategyConfig.hpp"
#include "../Strategy/Inputs/BacktestStrategyWarmup.hpp"
#include "../Strategy/BacktestStrategyRuntime.hpp"
#include "../Strategy/Diagnostic/BacktestStrategyTrace.hpp"
#include "../Input/Binary/BinaryCorpusLoader.hpp"
#include "../Input/Session/SessionLoader.hpp"
#include "BacktestRunError.hpp"

#include "trading_core/Contracts/Control/ControlState.hpp"
#include "trading_core/Algorithm/Market/LocalOrderBook.hpp"
#include "trading_core/Contracts/Strategy/StrategyContract.hpp"
#include "capital/Risk/PortfolioRisk.hpp"
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

std::int64_t currentReplayTsNs(BacktestRunContext& context) noexcept {
  auto& hasExplicitSessions = context.hasExplicitSessions;
  auto& engine = context.engine;
  auto& legEngines = context.legEngines;
  auto& replayStartTsNs = context.replayStartTsNs;
  auto& greatestProcessedTsNs = context.greatestProcessedTsNs;

  std::int64_t out = greatestProcessedTsNs;
  if (hasExplicitSessions) {
    for (const detail::EngineState& legEngine : legEngines)
      if (legEngine.lastReplayMonotonicNs > out) out = legEngine.lastReplayMonotonicNs;
  } else if (engine.lastReplayMonotonicNs > out) {
    out = engine.lastReplayMonotonicNs;
  }
  if (out > 0) return out;
  return replayStartTsNs;
}

bool reportReplayProgress(BacktestRunContext& context, std::uint64_t done, std::uint64_t total) noexcept {
  auto& progressCallback = context.progressCallback;
  auto& userData = context.userData;

  return reportProgress(done, total, "running", progressCallback, userData);
}

bool depthReplayRejected(BacktestRunContext& context) {
  auto& result = context.result;
  auto& engine = context.engine;
  auto& legEngines = context.legEngines;

  bool failed = engine.depthReplayFailed;
  for (const auto& leg : legEngines) failed = failed || leg.depthReplayFailed;
  if (!failed) return false;
  result.status = Status::CorruptData;
  result.error = "replay depth rejected by canonical CXET book";
  (void)writeCurrentArtifacts(context);
  return true;
}

bool deliverNextApplicationTransaction(BacktestReplayContext& context, const char* phase) {
  auto& result = context.run.result;
  auto& request = context.run.request;
  auto& hasExplicitSessions = context.run.hasExplicitSessions;
  auto& sessions = context.run.sessions;
  auto& session = context.run.session;
  auto& descriptor = context.run.descriptor;
  auto& inputPolicy = context.run.inputPolicy;
  auto& instrument = context.run.instrument;
  const auto& selectedSymbol = context.run.selectedSymbol;
  auto& legInstruments = context.run.legInstruments;
  auto& legSymbols = context.run.legSymbols;
  auto& state = context.run.state;
  auto& engine = context.run.engine;
  auto& legEngines = context.run.legEngines;
  auto& shard = context.run.shard;
  auto& control = context.run.control;
  auto& timeline = context.run.timeline;
  auto& greatestProcessedTsNs = context.run.greatestProcessedTsNs;
  auto& deliveryRefs = context.deliveryRefs;
  auto& deliveryEventCount = context.deliveryEventCount;
  auto& deliveryCursor = context.deliveryCursor;
  auto& singleScratch = context.singleScratch;
  auto& multiScratch = context.multiScratch;
  auto& replayStopped = context.replayStopped;

  const std::int64_t deliveryTsNs =
      deliveryEventAt(timeline, deliveryRefs, deliveryCursor).deliveryTsNs;
  const bool beforeDeliveryOk = hasExplicitSessions
      ? runUntilMulti(*descriptor, inputPolicy, state.data(), legEngines,
                      legInstruments, *multiScratch, shard, control,
                      deliveryTsNs, request, RunUntilBoundary::Exclusive)
      : runUntil(*descriptor, inputPolicy, state.data(), engine, instrument,
                 *singleScratch, shard, control, deliveryTsNs, request,
                 RunUntilBoundary::Exclusive);
  if (!beforeDeliveryOk) {
    result.status = Status::InvalidArgument;
    result.error =
        "strategy event loop did not settle before market delivery";
    (void)writeCurrentArtifacts(context.run);
    return false;
  }
  if (stopOnStrictRateLimitReject(context.run)) return false;
  if (tripHardLiquidation(context.run, deliveryTsNs) ||
      tripTraderRiskStop(context.run, deliveryTsNs)) {
    replayStopped = true;
    return true;
  }

  const detail::TimelineEvent& first =
      deliveryEventAt(timeline, deliveryRefs, deliveryCursor++);
  const detail::TimelineEvent* cycleEvent = &first;
  if (first.deliveryTsNs > greatestProcessedTsNs)
    greatestProcessedTsNs = first.deliveryTsNs;
  if (hasExplicitSessions) applyCapturedDeliveryClock(legEngines, first);
  else applyCapturedDeliveryClock(engine, first);
  if (hasExplicitSessions) {
    applyMarketEventToSnapshots(legEngines, sessions, first, legSymbols,
                                legInstruments, true);
  } else {
    applyMarketEventToSnapshots(engine, session, first, selectedSymbol,
                                instrument, true);
  }
  while (deliveryCursor < deliveryEventCount &&
         sameDepthApplicationTransaction(
             first, deliveryEventAt(timeline, deliveryRefs, deliveryCursor))) {
    const detail::TimelineEvent& next =
        deliveryEventAt(timeline, deliveryRefs, deliveryCursor++);
    cycleEvent = &next;
    if (hasExplicitSessions) {
      applyMarketEventToSnapshots(legEngines, sessions, next, legSymbols,
                                  legInstruments, true);
    } else {
      applyMarketEventToSnapshots(engine, session, next, selectedSymbol,
                                  instrument, true);
    }
  }
  if (depthReplayRejected(context.run)) return false;
  const bool deliveredOk = cycleEvent->applicationOnly ||
      (hasExplicitSessions
          ? runCycleMulti(*descriptor, inputPolicy, state.data(), legEngines,
                          legInstruments, *multiScratch, shard, control,
                          cycleEvent->legIndex, cycleEvent->deliveryTsNs,
                          request, nullptr)
          : runCycle(*descriptor, inputPolicy, state.data(), engine,
                     instrument,
                     *singleScratch, shard, control,
                     cycleEvent->deliveryTsNs, request, nullptr));
  if (!deliveredOk) {
    result.status = Status::InvalidArgument;
    result.error = formatStrategyRunCycleError(
        *descriptor, state.data(), phase, cycleEvent->deliveryTsNs,
        cycleEvent->legIndex);
    (void)writeCurrentArtifacts(context.run);
    return false;
  }
  if (stopOnStrictRateLimitReject(context.run)) return false;
  const bool afterDeliveryOk = hasExplicitSessions
      ? runUntilMulti(*descriptor, inputPolicy, state.data(), legEngines,
                      legInstruments, *multiScratch, shard, control,
                      cycleEvent->deliveryTsNs, request)
      : runUntil(*descriptor, inputPolicy, state.data(), engine, instrument,
                 *singleScratch, shard, control,
                 cycleEvent->deliveryTsNs, request);
  if (!afterDeliveryOk) {
    result.status = Status::InvalidArgument;
    result.error =
        "strategy event loop did not settle after market delivery";
    (void)writeCurrentArtifacts(context.run);
    return false;
  }
  if (stopOnStrictRateLimitReject(context.run)) return false;
  if (hasExplicitSessions) {
    for (detail::EngineState& legEngine : legEngines) {
      (void)detail::appendRiskSnapshotIfDue(
          legEngine, cycleEvent->deliveryTsNs);
    }
  } else {
    (void)detail::appendRiskSnapshotIfDue(
        engine, cycleEvent->deliveryTsNs);
  }
  if (tripHardLiquidation(context.run, cycleEvent->deliveryTsNs) ||
      tripTraderRiskStop(context.run, cycleEvent->deliveryTsNs)) {
    replayStopped = true;
  }
  return true;
}

bool replayBacktestRun(BacktestReplayContext& context) {
  auto& result = context.run.result;
  auto& request = context.run.request;
  auto& effectiveRequest = context.run.effectiveRequest;
  auto& hasExplicitSessions = context.run.hasExplicitSessions;
  auto& sessions = context.run.sessions;
  auto& session = context.run.session;
  auto& descriptor = context.run.descriptor;
  auto& inputPolicy = context.run.inputPolicy;
  auto& instrument = context.run.instrument;
  const auto& selectedSymbol = context.run.selectedSymbol;
  auto& legInstruments = context.run.legInstruments;
  auto& legSymbols = context.run.legSymbols;
  auto& state = context.run.state;
  auto& engine = context.run.engine;
  auto& legEngines = context.run.legEngines;
  auto& shard = context.run.shard;
  auto& control = context.run.control;
  auto& timeline = context.run.timeline;
  auto& greatestProcessedTsNs = context.run.greatestProcessedTsNs;
  auto& deliveryRefs = context.deliveryRefs;
  auto& deliveryEventCount = context.deliveryEventCount;
  auto& deliveryCursor = context.deliveryCursor;
  auto& singleScratch = context.singleScratch;
  auto& multiScratch = context.multiScratch;
  auto& replayStopped = context.replayStopped;
  auto& replayProgressStep = context.replayProgressStep;
  std::size_t i = 0u;
  while (i < timeline.size()) {
    if (i + 1u < timeline.size()) prefetchRead(&timeline[i + 1u]);
    if (deliveryCursor < deliveryEventCount) prefetchRead(&deliveryRefs[deliveryCursor]);
    const std::int64_t eventTsNs = timeline[i].tsNs;
    while (deliveryCursor < deliveryEventCount && deliveryEventAt(timeline, deliveryRefs, deliveryCursor).deliveryTsNs < eventTsNs) {
      if (deliveryCursor + 1u < deliveryEventCount) prefetchRead(&deliveryRefs[deliveryCursor + 1u]);
      if (!deliverNextApplicationTransaction(context, "delivered")) return false;
      if (replayStopped) break;
    }
    if (replayStopped) break;
    if (eventTsNs > greatestProcessedTsNs) greatestProcessedTsNs = eventTsNs;
    // Deliver every application frame that really arrived before this venue
    // boundary before advancing timers/actions to the boundary. For an exact
    // tie the venue update still wins: orders due at the same coordinate are
    // activated only after that venue state has been applied.
    const bool beforeRawOk = hasExplicitSessions
        ? runUntilMulti(*descriptor, inputPolicy, state.data(), legEngines,
                        legInstruments, *multiScratch, shard, control,
                        eventTsNs, request, RunUntilBoundary::Exclusive)
        : runUntil(*descriptor, inputPolicy, state.data(), engine, instrument,
                   *singleScratch, shard, control, eventTsNs, request,
                   RunUntilBoundary::Exclusive);
    if (!beforeRawOk) {
      result.status = Status::InvalidArgument;
      result.error = "strategy event loop did not settle before raw market event";
      (void)writeCurrentArtifacts(context.run);
      return false;
    }
    if (stopOnStrictRateLimitReject(context.run)) return false;
    while (i < timeline.size() && timeline[i].tsNs == eventTsNs) {
      if (i + 1u < timeline.size()) prefetchRead(&timeline[i + 1u]);
      const detail::TimelineEvent& first = timeline[i++];
      const detail::TimelineEvent* riskEvent = &first;
      if (hasExplicitSessions) applyMarketEventToSnapshots(legEngines, sessions, first, legSymbols, legInstruments, false);
      else applyMarketEventToSnapshots(engine, session, first, selectedSymbol, instrument, false);
      while (i < timeline.size() && timeline[i].tsNs == eventTsNs &&
             sameDepthApplicationTransaction(first, timeline[i])) {
        const detail::TimelineEvent& next = timeline[i++];
        riskEvent = &next;
        if (hasExplicitSessions) applyMarketEventToSnapshots(legEngines, sessions, next, legSymbols, legInstruments, false);
        else applyMarketEventToSnapshots(engine, session, next, selectedSymbol, instrument, false);
      }
      if (depthReplayRejected(context.run)) return false;
      if (hasExplicitSessions) {
        for (detail::EngineState& legEngine : legEngines) (void)detail::appendRiskSnapshotIfDue(legEngine, riskEvent->tsNs);
      } else {
        (void)detail::appendRiskSnapshotIfDue(engine, riskEvent->tsNs);
      }
      if (tripHardLiquidation(context.run, riskEvent->tsNs) ||
          tripTraderRiskStop(context.run, riskEvent->tsNs)) {
        replayStopped = true;
        break;
      }
    }
    if (replayStopped) break;
    while (deliveryCursor < deliveryEventCount && deliveryEventAt(timeline, deliveryRefs, deliveryCursor).deliveryTsNs <= eventTsNs) {
      if (deliveryCursor + 1u < deliveryEventCount) prefetchRead(&deliveryRefs[deliveryCursor + 1u]);
      if (!deliverNextApplicationTransaction(context, "delivered")) return false;
      if (replayStopped) break;
    }
    if (replayStopped) break;
    const bool afterOk = hasExplicitSessions
        ? runUntilMulti(*descriptor, inputPolicy, state.data(), legEngines,
                        legInstruments, *multiScratch, shard, control,
                        eventTsNs, request)
        : runUntil(*descriptor, inputPolicy, state.data(), engine, instrument,
                   *singleScratch, shard, control, eventTsNs, request);
    if (!afterOk) {
      result.status = Status::InvalidArgument;
      result.error = "strategy event loop did not settle after market event";
      (void)writeCurrentArtifacts(context.run);
      return false;
    }
    if (stopOnStrictRateLimitReject(context.run)) return false;
    if (tripHardLiquidation(context.run, eventTsNs) || tripTraderRiskStop(context.run, eventTsNs)) {
      replayStopped = true;
      break;
    }
    if ((i % replayProgressStep) == 0u && !reportReplayProgress(context.run, i, timeline.size())) {
      result.status = Status::Cancelled;
      result.error = "cancelled";
      (void)writeCurrentArtifacts(context.run);
      return false;
    }
  }
  while (!replayStopped && deliveryCursor < deliveryEventCount) {
    if (deliveryCursor + 1u < deliveryEventCount) prefetchRead(&deliveryRefs[deliveryCursor + 1u]);
    if (!deliverNextApplicationTransaction(context, "delayed")) return false;
  }
  if (!replayStopped) {
    const std::int64_t settlementBaseTsNs = currentReplayTsNs(context.run);
    const bool settled = hasExplicitSessions
        ? settleAfterReplayMulti(*descriptor, inputPolicy, state.data(),
                                 legEngines, legInstruments, *multiScratch,
                                 shard, control, settlementBaseTsNs,
                                 effectiveRequest)
        : settleAfterReplay(*descriptor, inputPolicy, state.data(), engine,
                            instrument, *singleScratch, shard, control,
                            settlementBaseTsNs, effectiveRequest);
    if (!settled) {
      result.status = Status::InvalidArgument;
      result.error = "strategy event loop did not settle after replay";
      (void)writeCurrentArtifacts(context.run);
      return false;
    }
    if (stopOnStrictRateLimitReject(context.run)) return false;
  }
  if (!replayStopped) {
    const std::int64_t stopCheckTsNs = currentReplayTsNs(context.run);
    (void)(tripHardLiquidation(context.run, stopCheckTsNs) || tripTraderRiskStop(context.run, stopCheckTsNs, true));
  }
  return true;
}


}  // namespace hft_backtest::run_detail
