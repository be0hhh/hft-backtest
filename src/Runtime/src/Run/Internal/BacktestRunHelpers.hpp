#pragma once

#include "hft_backtest/Backtest.hpp"

#include "../../Engine/BacktestEngine.hpp"
#include "../../Engine/BacktestEngineSetup.hpp"
#include "../../Execution/Model/BacktestExecutionDepth.hpp"
#include "../../Accounting/BacktestResultAggregation.hpp"
#include "../../Config/BacktestRunConfig.hpp"
#include "../../Config/Strategy/BacktestStrategyConfig.hpp"
#include "../../Strategy/Inputs/BacktestStrategyWarmup.hpp"
#include "../../Strategy/BacktestStrategyRuntime.hpp"
#include "../../Strategy/Diagnostics/BacktestStrategyTrace.hpp"
#include "../../Input/Binary/BinaryCorpusLoader.hpp"
#include "../../Input/Session/SessionLoader.hpp"
#include "../BacktestRunError.hpp"

#include "trading_core/Contracts/Control/ControlState.hpp"
#include "trading_core/Algorithm/Market/LocalOrderBook.hpp"
#include "trading_core/Contracts/Strategy/StrategyContract.hpp"
#include "capital/Risk/PortfolioRisk.hpp"
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

#include "BacktestRunContext.hpp"

namespace hft_backtest::run_detail {
using trading_core::StrategyDescriptor;
using trading_core::StrategyParamStorage;

BacktestLiveProbeSummary summarizeLiveProbeConfig(const BacktestStrategyConfig& cfg) noexcept;
bool reportProgress(std::uint64_t done, std::uint64_t total, std::string stage, BacktestProgressCallback cb, void* userData) noexcept;
bool reportProgress(std::uint64_t done,
                    std::uint64_t total,
                    std::string stage,
                    std::uint32_t basePercent,
                    std::uint32_t spanPercent,
                    BacktestProgressCallback cb,
                    void* userData) noexcept;
std::int64_t capturedUtcOffset(const detail::TimelineEvent& event) noexcept;
void applyCapturedDeliveryClock(detail::EngineState& engine,
                                const detail::TimelineEvent& event) noexcept;
void applyCapturedDeliveryClock(std::vector<detail::EngineState>& engines,
                                const detail::TimelineEvent& event) noexcept;
void applyLegSummaryLatency(BacktestLegSummary& leg,
                            const detail::BacktestLatencyConfig& latency) noexcept;
BacktestReplayClockEvidence replayClockEvidence(const detail::SessionData& session);
void mergeReplayClockEvidence(BacktestReplayClockEvidence& out,
                              const detail::SessionData& session) noexcept;
void includeReplayCoordinate(std::int64_t value,
                             std::int64_t& first,
                             std::int64_t& last) noexcept;
void includeTimelineClock(BacktestReplayClockEvidence& out,
                          const detail::TimelineEvent& event) noexcept;
void includeTimelineClock(detail::EngineState& out,
                          const detail::TimelineEvent& event) noexcept;
void seedRequestedLegSummaries(BacktestRunResult& result,
                               const BacktestRunRequest& request,
                               bool hasExplicitSessions) noexcept;
void seedLoadedLegSummaries(BacktestRunResult& result,
                            const BacktestRunRequest& request,
                            const std::vector<detail::SessionData>& sessions) noexcept;



inline void prefetchRead(const void* ptr) noexcept {
#if defined(__GNUC__) || defined(__clang__)
  __builtin_prefetch(ptr, 0, 1);
#else
  (void)ptr;
#endif
}


[[nodiscard]] inline bool sameDepthApplicationTransaction(
    const detail::TimelineEvent& first,
    const detail::TimelineEvent& next) noexcept {
  return first.kind == detail::kTimelineKindDepth &&
      next.kind == detail::kTimelineKindDepth &&
      first.applicationAtomicRebase && next.applicationAtomicRebase &&
      first.frameSequence != 0u &&
      next.frameSequence == first.frameSequence &&
      next.deliveryTsNs == first.deliveryTsNs &&
      next.deliveryRealtimeNs == first.deliveryRealtimeNs &&
      next.receiveMonotonicNs == first.receiveMonotonicNs &&
      next.producerEpoch == first.producerEpoch &&
      next.sourceGeneration == first.sourceGeneration &&
      next.sessionEpoch == first.sessionEpoch &&
      next.sourceId == first.sourceId && next.shardId == first.shardId &&
      next.legIndex == first.legIndex;
}

inline std::int64_t nextReplayTimestamp(std::int64_t value) noexcept {
  return value < std::numeric_limits<std::int64_t>::max() ? value + 1 : value;
}

inline std::size_t requestedLegCount(const BacktestRunRequest& request) noexcept {
  if (!request.corpusPath.empty() || !request.corpusSources.empty())
    return request.corpusSources.size();
  return request.sessions.size() + (request.sessionPath.empty() ? 0u : 1u);
}

inline bool usesBinaryCorpus(const BacktestRunRequest& request) noexcept {
  return !request.corpusPath.empty() || !request.corpusSources.empty();
}


}  // namespace hft_backtest::run_detail
