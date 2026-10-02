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
#include "../Run/BacktestRunError.hpp"

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

#include "../Run/Internal/BacktestRunContext.hpp"
#include "../Run/Internal/BacktestRunHelpers.hpp"

namespace hft_backtest::run_detail {

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


std::int64_t capturedUtcOffset(const detail::TimelineEvent& event) noexcept {
  const __int128 offset = static_cast<__int128>(event.deliveryRealtimeNs) -
      static_cast<__int128>(event.deliveryTsNs);
  if (offset < std::numeric_limits<std::int64_t>::min())
    return std::numeric_limits<std::int64_t>::min();
  if (offset > std::numeric_limits<std::int64_t>::max())
    return std::numeric_limits<std::int64_t>::max();
  return static_cast<std::int64_t>(offset);
}

void applyCapturedDeliveryClock(detail::EngineState& engine,
                                const detail::TimelineEvent& event) noexcept {
  engine.replayUtcOffsetNs = capturedUtcOffset(event);
}

void applyCapturedDeliveryClock(std::vector<detail::EngineState>& engines,
                                const detail::TimelineEvent& event) noexcept {
  const std::int64_t offset = capturedUtcOffset(event);
  for (detail::EngineState& engine : engines) engine.replayUtcOffsetNs = offset;
}


void mergeReplayClockEvidence(BacktestReplayClockEvidence& out,
                              const detail::SessionData& session) noexcept {
  if (out.firstReceiveMonotonicNs == 0u ||
      (session.firstReceiveMonotonicNs != 0u &&
       session.firstReceiveMonotonicNs < out.firstReceiveMonotonicNs))
    out.firstReceiveMonotonicNs = session.firstReceiveMonotonicNs;
  if (session.lastReceiveMonotonicNs > out.lastReceiveMonotonicNs)
    out.lastReceiveMonotonicNs = session.lastReceiveMonotonicNs;
  if (out.firstReceiveRealtimeNs == 0 ||
      (session.firstReceiveRealtimeNs > 0 &&
       session.firstReceiveRealtimeNs < out.firstReceiveRealtimeNs))
    out.firstReceiveRealtimeNs = session.firstReceiveRealtimeNs;
  if (session.lastReceiveRealtimeNs > out.lastReceiveRealtimeNs)
    out.lastReceiveRealtimeNs = session.lastReceiveRealtimeNs;
  out.realtimeRegressions += session.realtimeRegressionCount;
  out.monotonicNonIncreasing += session.monotonicNonIncreasingCount;
  out.exchangeAheadOfReceive += session.exchangeAheadOfReceiveCount;
  out.exchangeTimestampMissing += session.exchangeTimestampMissingCount;
}

void includeReplayCoordinate(std::int64_t value,
                             std::int64_t& first,
                             std::int64_t& last) noexcept {
  if (value <= 0) return;
  if (first == 0 || value < first) first = value;
  if (value > last) last = value;
}

void includeTimelineClock(BacktestReplayClockEvidence& out,
                          const detail::TimelineEvent& event) noexcept {
  includeReplayCoordinate(event.deliveryTsNs, out.firstReplayDeliveryNs,
                          out.lastReplayDeliveryNs);
  if (!event.applicationOnly) {
    includeReplayCoordinate(event.tsNs, out.firstVenueScheduleNs,
                            out.lastVenueScheduleNs);
  }
}

void includeTimelineClock(detail::EngineState& out,
                          const detail::TimelineEvent& event) noexcept {
  includeReplayCoordinate(event.deliveryTsNs, out.firstReplayDeliveryNs,
                          out.lastReplayDeliveryNs);
  if (!event.applicationOnly) {
    includeReplayCoordinate(event.tsNs, out.firstVenueScheduleNs,
                            out.lastVenueScheduleNs);
  }
}


}  // namespace hft_backtest::run_detail
