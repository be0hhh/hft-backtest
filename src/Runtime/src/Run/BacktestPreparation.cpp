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

BacktestLiveProbeSummary summarizeLiveProbeConfig(const BacktestStrategyConfig& cfg) noexcept {
  BacktestLiveProbeSummary summary{};
  for (const BacktestStrategyVenueConfig& venue : cfg.venues) {
    if (venue.liveProbe == BacktestLiveProbeMode::Default) continue;
    summary.configured = true;
    if (venue.liveProbe == BacktestLiveProbeMode::Required) summary.enabled = true;
  }
  summary.ignored = summary.configured;
  return summary;
}


void applyLegSummaryLatency(BacktestLegSummary& leg,
                            const detail::BacktestLatencyConfig& latency) noexcept {
  leg.marketOrderLatency = latency.marketOrder;
  leg.limitOrderLatency = latency.limitOrder;
  leg.cancelOrderLatency = latency.cancelOrder;
  leg.userDataLatency = latency.userData;
}

BacktestReplayClockEvidence replayClockEvidence(const detail::SessionData& session) {
  BacktestReplayClockEvidence out{};
  out.firstReceiveMonotonicNs = session.firstReceiveMonotonicNs;
  out.lastReceiveMonotonicNs = session.lastReceiveMonotonicNs;
  out.firstReceiveRealtimeNs = session.firstReceiveRealtimeNs;
  out.lastReceiveRealtimeNs = session.lastReceiveRealtimeNs;
  out.realtimeRegressions = session.realtimeRegressionCount;
  out.monotonicNonIncreasing = session.monotonicNonIncreasingCount;
  out.exchangeAheadOfReceive = session.exchangeAheadOfReceiveCount;
  out.exchangeTimestampMissing = session.exchangeTimestampMissingCount;
  return out;
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
    if (usesBinaryCorpus(request)) {
      for (const BacktestCorpusSourceRequest& source : request.corpusSources) {
        const std::size_t i = result.legs.size();
        BacktestLegSummary leg{};
        leg.sessionPath = request.corpusPath;
        leg.exchange = source.exchange;
        leg.market = source.market;
        leg.symbol = source.canonicalSymbol;
        applyLegSummaryLatency(
            leg, latencyConfigForLeg(request, leg.exchange, leg.market, i));
        leg.initialBalanceE8 = legInitialBalanceE8(request, i);
        leg.walletBalanceE8 = leg.initialBalanceE8;
        leg.strategyClosed = true;
        result.legs.push_back(std::move(leg));
      }
      return;
    }
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
      leg.replayClock = replayClockEvidence(session);
      leg.initialBalanceE8 = legInitialBalanceE8(request, i);
      leg.walletBalanceE8 = leg.initialBalanceE8;
      leg.strategyClosed = true;
      result.legs.push_back(std::move(leg));
    }
  } catch (...) {
    result.legs.clear();
  }
}
void loadBacktestRunSessions(const BacktestRunRequest& request, bool binaryCorpus,
    bool hasExplicitSessions, bool usePreparedSessions, detail::PreparedBacktestInput* preparedInput,
    const detail::SessionLoadPlan& loadPlan, std::vector<detail::SessionData>& loadedSessions,
    const std::vector<detail::SessionData>*& sessionsView, BacktestRunResult& result, std::string& error) {
  if (usePreparedSessions) {
    sessionsView = &preparedInput->sessions;
    result.status = Status::Ok;
  } else if (binaryCorpus) {
    loadedSessions.resize(request.corpusSources.size());
    result.status = Status::Ok;
    for (std::size_t i = 0u; i < request.corpusSources.size(); ++i) {
      result.status = detail::loadBinaryMarketCorpus(
          request.corpusPath, request.corpusSources[i], loadPlan,
          loadedSessions[i], error);
      if (!isOk(result.status)) break;
    }
    sessionsView = &loadedSessions;
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
}

bool prepareEngineOrderBookStorage(detail::EngineState& target,
    const trading_core::StrategyDescriptor* descriptor, const detail::SessionLoadPlan& loadPlan) noexcept {

  if (descriptor->orderBookViewLevelsPerSide != 0u) {
    CountVal strategyDepth{};
    strategyDepth.raw =
        descriptor->orderBookViewLevelsPerSide;
    if (!target.orderBook.reserveLevelsPerSide(
            strategyDepth)) {
      return false;
    }
    target.orderBook.config.maxStoredLevelsPerSide =
        strategyDepth;
  }
  if (loadPlan.loadDepths) {
    if (!target.strategyDepthBook.prepare()) return false;
    CountVal executionDepth{};
    executionDepth.raw = static_cast<std::uint32_t>(
        rawdata::kMaxOrderbookLevels);
    if (!target.venueOrderBook.reserveLevelsPerSide(executionDepth)) {
      return false;
    }
    target.venueOrderBook.config.maxStoredLevelsPerSide =
        executionDepth;
    if (!target.venueDepthBook.prepare() ||
        !target.executionLiquidity.prepare(executionDepth.raw)) return false;
  }
  return true;
}

void prepareBacktestDepthProviders(std::vector<std::unique_ptr<detail::ExecutionDepthProvider>>& executionDepthProviders,
    detail::PreparedBacktestInput* preparedInput, const detail::SessionLoadPlan& loadPlan,
    bool binaryCorpus, bool hasExplicitSessions, const std::vector<detail::SessionData>& sessions,
    const detail::SessionData& session, std::vector<detail::EngineState>& legEngines,
    detail::EngineState& engine, const StrategyInstrumentSpec& instrument, const Symbol& selectedSymbol) {
  auto depthCacheForLeg = [&](std::size_t leg) -> std::shared_ptr<detail::ExecutionDepthCheckpointCache> {
    if (preparedInput == nullptr || leg >= preparedInput->depthCaches.size()) return {};
    return preparedInput->depthCaches[leg];
  };
  if (!loadPlan.loadDepths && !binaryCorpus) {
    if (hasExplicitSessions) {
      executionDepthProviders.reserve(legEngines.size());
      for (std::size_t leg = 0; leg < sessions.size() && leg < legEngines.size(); ++leg) {
        trading_core::LocalOrderBookConfig bookConfig{};
        bookConfig.maxStoredLevelsPerSide.raw = rawdata::kMaxOrderbookLevels;
        bookConfig.tickSizeRaw = sessions[leg].instrument.tickSizeE8;
        Symbol legSymbol{};
        legSymbol.copyFrom(sessions[leg].symbol.c_str());
        auto provider = std::make_unique<detail::ExecutionDepthProvider>();
        const detail::ExecutionDepthOpenStatus depthStatus =
            provider->open(sessions[leg].sessionPath,
                           legSymbol,
                           bookConfig,
                           legEngines[leg].venueOrderBook,
                           depthCacheForLeg(leg));
        if (depthStatus == detail::ExecutionDepthOpenStatus::Ready) {
          legEngines[leg].executionDepthProvider = provider.get();
          legEngines[leg].depthExecutionMode = BacktestDepthExecutionMode::Depth;
          executionDepthProviders.push_back(std::move(provider));
        } else if (depthStatus == detail::ExecutionDepthOpenStatus::Failed) {
          legEngines[leg].depthExecutionMode = BacktestDepthExecutionMode::BboFallback;
          legEngines[leg].depthFallbackReason = provider->failureReason();
        }
      }
    } else {
      trading_core::LocalOrderBookConfig bookConfig{};
      bookConfig.maxStoredLevelsPerSide.raw = rawdata::kMaxOrderbookLevels;
      bookConfig.tickSizeRaw = instrument.tickSizeRaw;
      auto provider = std::make_unique<detail::ExecutionDepthProvider>();
      const detail::ExecutionDepthOpenStatus depthStatus =
          provider->open(session.sessionPath,
                         selectedSymbol,
                         bookConfig,
                         engine.venueOrderBook,
                         depthCacheForLeg(0u));
      if (depthStatus == detail::ExecutionDepthOpenStatus::Ready) {
        engine.executionDepthProvider = provider.get();
        engine.depthExecutionMode = BacktestDepthExecutionMode::Depth;
        executionDepthProviders.push_back(std::move(provider));
      } else if (depthStatus == detail::ExecutionDepthOpenStatus::Failed) {
        engine.depthExecutionMode = BacktestDepthExecutionMode::BboFallback;
        engine.depthFallbackReason = provider->failureReason();
      }
    }
  }
}

void prepareBacktestRunTimeline(bool usePreparedSessions, detail::PreparedBacktestInput* preparedInput,
    const BacktestRunRequest& request, bool hasExplicitSessions, const detail::SessionLoadPlan& loadPlan,
    const std::vector<detail::SessionData>& sessions, const detail::SessionData& session,
    std::vector<detail::TimelineEvent>& loadedTimeline, const std::vector<detail::TimelineEvent>*& timelineView) {
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
}

void prepareBacktestDeliveryReferences(std::vector<DeliveryEventRef>& deliveryRefs,
    const std::vector<detail::TimelineEvent>& timeline) {
  deliveryRefs.reserve(timeline.size());
  for (std::size_t rawIndex = 0u; rawIndex < timeline.size(); ++rawIndex)
    deliveryRefs.push_back(DeliveryEventRef{rawIndex});
  std::stable_sort(deliveryRefs.begin(), deliveryRefs.end(), [&](const DeliveryEventRef& lhs,
                                                                const DeliveryEventRef& rhs) noexcept {
    const detail::TimelineEvent& a = timeline[lhs.rawIndex];
    const detail::TimelineEvent& b = timeline[rhs.rawIndex];
    if (a.deliveryTsNs != b.deliveryTsNs) return a.deliveryTsNs < b.deliveryTsNs;
    if (a.producerEpoch != b.producerEpoch) return a.producerEpoch < b.producerEpoch;
    if (a.shardId != b.shardId) return a.shardId < b.shardId;
    if (a.shardSequence != b.shardSequence) return a.shardSequence < b.shardSequence;
    if (a.eventOrdinal != b.eventOrdinal) return a.eventOrdinal < b.eventOrdinal;
    if (a.sourceId != b.sourceId) return a.sourceId < b.sourceId;
    if (a.sourceGeneration != b.sourceGeneration) return a.sourceGeneration < b.sourceGeneration;
    if (a.sessionEpoch != b.sessionEpoch) return a.sessionEpoch < b.sessionEpoch;
    if (a.frameSequence != b.frameSequence) return a.frameSequence < b.frameSequence;
    if (a.legIndex != b.legIndex) return a.legIndex < b.legIndex;
    if (a.kind != b.kind) return a.kind < b.kind;
    return a.index < b.index;
  });
}

const detail::TimelineEvent& deliveryEventAt(
    const std::vector<detail::TimelineEvent>& timeline,
    const std::vector<DeliveryEventRef>& deliveryRefs, std::size_t index) noexcept {

  return timeline[deliveryRefs[index].rawIndex];
}


}  // namespace hft_backtest::run_detail
