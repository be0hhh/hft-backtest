#pragma once

#include "../../Engine/BacktestEngine.hpp"
#include "../../Preparation/BacktestPrepared.hpp"
#include "../../Strategy/BacktestStrategyRuntime.hpp"
#include "trading_core/Execution/PortfolioRisk.hpp"

#include <array>
#include <chrono>
#include <memory>
#include <vector>

namespace hft_backtest::run_detail {

struct DeliveryEventRef {
  std::size_t rawIndex{0u};
};

struct BacktestArtifactContext {
  BacktestRunResult& result;
  const BacktestRunRequest& request;
  const std::chrono::steady_clock::time_point& totalStart;
};

// These contexts borrow the original staged owners in runBacktest.
struct BacktestRunContext {
  BacktestArtifactContext& artifacts;
  BacktestRunResult& result;
  const BacktestRunRequest& request;
  const BacktestRunRequest& effectiveRequest;
  BacktestProgressCallback& progressCallback;
  void*& userData;
  const bool& hasExplicitSessions;
  const std::int64_t& portfolioInitialBalance;
  const std::vector<detail::SessionData>& sessions;
  const detail::SessionData& session;
  const trading_core::StrategyDescriptor*& descriptor;
  const BacktestStrategyInputPolicy& inputPolicy;
  StrategyInstrumentSpec& instrument;
  const Symbol& selectedSymbol;
  std::vector<StrategyInstrumentSpec>& legInstruments;
  std::vector<Symbol>& legSymbols;
  std::array<unsigned char, trading_core::kStrategyRuntimeStateBytes>& state;
  detail::EngineState& engine;
  std::vector<detail::EngineState>& legEngines;
  std::unique_ptr<detail::EngineState>& aggregateEngineStorage;
  const detail::EngineState*& artifactEngine;
  trading_core::SymbolShardConfig& shard;
  trading_core::ShardControlState& control;
  const std::vector<detail::TimelineEvent>& timeline;
  std::int64_t& replayStartTsNs;
  std::int64_t& greatestProcessedTsNs;
  std::array<trading_core::StrategyAccountState, trading_core::kStrategyRuntimeMaxLegs>& riskAccounts;
  const std::size_t& riskAccountCount;
  trading_core::PortfolioRiskState& traderRiskState;
  bool& traderRiskCloseProblem;
  bool& legLiquidationProblem;
};

struct BacktestReplayContext {
  BacktestRunContext& run;
  const std::vector<DeliveryEventRef>& deliveryRefs;
  const std::size_t& deliveryEventCount;
  std::size_t& deliveryCursor;
  SingleLegCycleScratch*& singleScratch;
  MultiLegCycleScratch*& multiScratch;
  bool& replayStopped;
  const std::size_t& replayProgressStep;
};

void markTotalTime(BacktestArtifactContext& context) noexcept;
void refreshArtifactCounters(BacktestArtifactContext& context, const detail::EngineState& engine) noexcept;
Status writeArtifacts(BacktestArtifactContext& context, const detail::EngineState& engine) noexcept;
Status writeEmptyArtifacts(BacktestArtifactContext& context) noexcept;
Status writeCurrentArtifacts(BacktestRunContext& context) noexcept;
std::int64_t currentReplayTsNs(BacktestRunContext& context) noexcept;
bool stopOnStrictRateLimitReject(BacktestRunContext& context) noexcept;
bool tripHardLiquidation(BacktestRunContext& context, std::int64_t tsNs) noexcept;
bool tripTraderRiskStop(BacktestRunContext& context, std::int64_t tsNs, bool forceRiskCheck = false);
bool reportReplayProgress(BacktestRunContext& context, std::uint64_t done, std::uint64_t total) noexcept;
bool depthReplayRejected(BacktestRunContext& context);
bool deliverNextApplicationTransaction(BacktestReplayContext& context, const char* phase);
bool replayBacktestRun(BacktestReplayContext& context);
void finalizeBacktestRun(BacktestRunContext& context);
const detail::TimelineEvent& deliveryEventAt(
    const std::vector<detail::TimelineEvent>& timeline,
    const std::vector<DeliveryEventRef>& deliveryRefs, std::size_t index) noexcept;
void loadBacktestRunSessions(const BacktestRunRequest& request, bool binaryCorpus,
    bool hasExplicitSessions, bool usePreparedSessions, detail::PreparedBacktestInput* preparedInput,
    const detail::SessionLoadPlan& loadPlan, std::vector<detail::SessionData>& loadedSessions,
    const std::vector<detail::SessionData>*& sessionsView, BacktestRunResult& result, std::string& error);
bool prepareEngineOrderBookStorage(detail::EngineState& target,
    const trading_core::StrategyDescriptor* descriptor, const detail::SessionLoadPlan& loadPlan) noexcept;
void prepareBacktestDepthProviders(std::vector<std::unique_ptr<detail::ExecutionDepthProvider>>& executionDepthProviders,
    detail::PreparedBacktestInput* preparedInput, const detail::SessionLoadPlan& loadPlan,
    bool binaryCorpus, bool hasExplicitSessions, const std::vector<detail::SessionData>& sessions,
    const detail::SessionData& session, std::vector<detail::EngineState>& legEngines,
    detail::EngineState& engine, const StrategyInstrumentSpec& instrument, const Symbol& selectedSymbol);
void prepareBacktestRunTimeline(bool usePreparedSessions, detail::PreparedBacktestInput* preparedInput,
    const BacktestRunRequest& request, bool hasExplicitSessions, const detail::SessionLoadPlan& loadPlan,
    const std::vector<detail::SessionData>& sessions, const detail::SessionData& session,
    std::vector<detail::TimelineEvent>& loadedTimeline, const std::vector<detail::TimelineEvent>*& timelineView);
void prepareBacktestDeliveryReferences(std::vector<DeliveryEventRef>& deliveryRefs,
    const std::vector<detail::TimelineEvent>& timeline);

}  // namespace hft_backtest::run_detail
