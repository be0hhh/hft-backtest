#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "hft_backtest/Api.hpp"
#include "hft_backtest/Status.hpp"
#include "hft_backtest/StrategyMetadata.hpp"
#include "trading_core/Algorithm/RateLimit/RateLimit.hpp"

namespace hft_backtest {

inline constexpr std::uint32_t kBacktestApiVersion = 3u;

extern "C" HFT_BACKTEST_API std::uint32_t hft_backtest_api_version() noexcept;

struct BacktestLatencyProfile {
  std::uint64_t baseUs{0};
  std::uint64_t jitterUs{0};
};

enum class BacktestDepthExecutionMode : std::uint8_t {
  BboNoDepth = 0,
  Depth = 1,
  BboFallback = 2,
};

struct BacktestDepthExecutionSummary {
  BacktestDepthExecutionMode mode{BacktestDepthExecutionMode::BboNoDepth};
  std::uint64_t rowsRead{0};
  std::uint64_t bboFallbackFills{0};
  std::int64_t firstBboFallbackTsNs{0};
  std::uint64_t readNs{0};
};

struct BacktestStageTimings {
  std::uint64_t loadNs{0};
  std::uint64_t timelineNs{0};
  std::uint64_t deliveryNs{0};
  std::uint64_t replayNs{0};
  std::uint64_t finalizeNs{0};
  std::uint64_t artifactWriteNs{0};
  std::uint64_t totalNs{0};
};

struct BacktestSessionRequest {
  std::filesystem::path path{};
  std::string venue{};
  std::string symbol{};
};

struct BacktestCorpusSourceRequest {
  std::string exchange{};
  std::string market{};
  std::string canonicalSymbol{};
  // Half-open UTC receive interval [beginReceiveNs, endReceiveNs).
  std::int64_t beginReceiveNs{0};
  std::int64_t endReceiveNs{0};
};

struct BacktestFeeSchedule {
  std::string exchange{};
  std::string market{};
  std::int64_t makerFeeBpsE8{0};
  std::int64_t takerFeeBpsE8{0};
};

struct BacktestLatencySchedule {
  std::string exchange{};
  std::string market{};
  BacktestLatencyProfile marketOrder{};
  BacktestLatencyProfile limitOrder{};
  BacktestLatencyProfile cancelOrder{};
  BacktestLatencyProfile userData{};
};

struct BacktestRateLimitSchedule {
  std::string exchange{};
  std::string market{};
  std::vector<trading_core::RateLimitBucketConfig> buckets{};
  std::vector<trading_core::RateLimitActionConfig> actions{};
};

enum class BacktestTradeMode : std::uint8_t {
  AllLegs = 0,
  PrimaryOnly = 1,
};

struct BacktestRunRequest {
  std::filesystem::path sessionPath{};
  std::vector<BacktestSessionRequest> sessions{};
  std::filesystem::path corpusPath{};
  std::vector<BacktestCorpusSourceRequest> corpusSources{};
  std::filesystem::path configPath{};
  std::filesystem::path outputPath{};
  std::string requestId{};
  std::string runId{};
  std::string strategy{};
  std::string indicatorProfile{};
  std::uint64_t latencySeed{0};
  BacktestLatencyProfile marketOrderLatency{};
  BacktestLatencyProfile limitOrderLatency{};
  BacktestLatencyProfile cancelOrderLatency{};
  BacktestLatencyProfile userDataLatency{};
  std::uint64_t orderLatencyUs{1000};
  std::uint64_t cancelLatencyUs{1000};
  std::uint32_t makerFillMinCrossTicks{0};
  std::int64_t initialBalanceE8{0};
  std::vector<std::int64_t> legInitialBalancesE8{};
  std::int64_t makerFeeBpsE8{0};
  std::int64_t takerFeeBpsE8{0};
  std::vector<BacktestFeeSchedule> feeSchedules{};
  std::vector<BacktestLatencySchedule> latencySchedules{};
  std::vector<BacktestRateLimitSchedule> rateLimitSchedules{};
  BacktestTradeMode tradeMode{BacktestTradeMode::AllLegs};
  std::uint32_t primaryLegIndex{0u};
  bool hasMakerFeeBps{false};
  bool hasTakerFeeBps{false};
  bool hasPrimaryLegIndex{false};
  bool rateLimitsEnabled{true};
  bool strictRateLimitRejects{false};
  bool writeArtifacts{true};
  bool captureStrategySpread{false};
};

struct BacktestProgress {
  std::uint64_t eventsDone{0};
  std::uint64_t eventsTotal{0};
  std::uint32_t percent{0};
  std::string stage{};
};

struct BacktestInputCoverage {
  std::uint64_t strategyCycles{0};
  std::uint64_t cyclesWithTrade{0};
  std::uint64_t cyclesWithBookTicker{0};
  std::uint64_t cyclesWithDepth{0};
  std::uint64_t cyclesWithOrderBook{0};
  std::uint64_t seedTradeRows{0};
  std::uint64_t seedM1Candles{0};
  std::uint64_t seedM15Candles{0};
  std::uint64_t seedD1Candles{0};
};

struct BacktestLiveProbeSummary {
  bool configured{false};
  bool enabled{false};
  bool ignored{false};
};

struct BacktestReplayClockEvidence {
  std::string arrivalBoundary{"hft-parser.application-frame-ready"};
  std::string marketDataDelivery{"captured_application_frame_arrival"};
  std::string venueSchedule{"exchange_projected_not_after_delivery"};
  std::uint64_t firstReceiveMonotonicNs{0u};
  std::uint64_t lastReceiveMonotonicNs{0u};
  std::int64_t firstReceiveRealtimeNs{0};
  std::int64_t lastReceiveRealtimeNs{0};
  std::int64_t firstReplayDeliveryNs{0};
  std::int64_t lastReplayDeliveryNs{0};
  std::int64_t firstVenueScheduleNs{0};
  std::int64_t lastVenueScheduleNs{0};
  std::uint64_t realtimeRegressions{0u};
  std::uint64_t monotonicNonIncreasing{0u};
  std::uint64_t exchangeAheadOfReceive{0u};
  std::uint64_t exchangeTimestampMissing{0u};
  std::uint64_t venueScheduleAdjustedEvents{0u};
};

struct BacktestLegSummary {
  std::filesystem::path sessionPath{};
  std::string exchange{};
  std::string market{};
  std::string symbol{};
  std::int64_t makerFeeBpsE8{0};
  std::int64_t takerFeeBpsE8{0};
  BacktestLatencyProfile marketOrderLatency{};
  BacktestLatencyProfile limitOrderLatency{};
  BacktestLatencyProfile cancelOrderLatency{};
  BacktestLatencyProfile userDataLatency{};
  std::uint64_t orders{0};
  std::uint64_t fills{0};
  std::uint64_t userEvents{0};
  std::uint64_t timerWakeEvents{0};
  std::uint64_t reduceOnlyOrders{0};
  std::uint64_t reduceOnlyFills{0};
  std::int64_t initialBalanceE8{0};
  std::int64_t walletBalanceE8{0};
  std::int64_t grossRealizedPnlE8{0};
  std::int64_t feesPaidE8{0};
  std::int64_t netRealizedPnlE8{0};
  std::int64_t unrealizedPnlE8{0};
  std::int64_t totalPnlE8{0};
  std::int64_t walletDeltaE8{0};
  std::int64_t openPositionQtyE8{0};
  std::vector<std::int64_t> totalPnlCurveE8{};
  BacktestDepthExecutionSummary depthExecution{};
  BacktestReplayClockEvidence replayClock{};
  bool strategyClosed{false};
};

struct BacktestRunResult {
  Status status{Status::Unknown};
  std::string requestId{};
  std::string runId{};
  std::string strategy{};
  std::string indicatorProfile{};
  std::filesystem::path sessionPath{};
  std::filesystem::path configPath{};
  std::filesystem::path resultPath{};
  std::string error{};
  std::vector<std::string> warnings{};
  std::uint64_t latencySeed{0};
  BacktestLatencyProfile marketOrderLatency{};
  BacktestLatencyProfile limitOrderLatency{};
  BacktestLatencyProfile cancelOrderLatency{};
  BacktestLatencyProfile userDataLatency{};
  std::uint64_t orderLatencyUs{1000};
  std::uint64_t cancelLatencyUs{1000};
  std::uint64_t trades{0};
  std::uint64_t bookTickers{0};
  std::uint64_t depths{0};
  std::uint64_t orders{0};
  std::uint64_t fills{0};
  std::uint64_t strategySpreadRows{0};
  std::uint64_t events{0};
  std::uint64_t userEvents{0};
  std::uint64_t timerWakeEvents{0};
  std::uint64_t reduceOnlyOrders{0};
  std::uint64_t reduceOnlyFills{0};
  std::int64_t firstTsNs{0};
  std::int64_t lastTsNs{0};
  std::int64_t realizedPnlE8{0};
  std::int64_t unrealizedPnlE8{0};
  std::int64_t totalPnlE8{0};
  std::int64_t initialBalanceE8{0};
  std::int64_t walletBalanceE8{0};
  std::int64_t grossRealizedPnlE8{0};
  std::int64_t feesPaidE8{0};
  std::int64_t netRealizedPnlE8{0};
  std::int64_t maxDrawdownE8{0};
  std::int64_t mfeE8{0};
  std::int64_t maeE8{0};
  std::int64_t openPositionQtyE8{0};
  std::int64_t liquidationTsNs{0};
  std::int64_t liquidationEquityE8{0};
  std::int64_t riskStopTsNs{0};
  std::int64_t riskStopEquityE8{0};
  std::int64_t riskStopThresholdEquityE8{0};
  std::vector<std::int64_t> totalPnlCurveE8{};
  std::vector<BacktestLegSummary> legs{};
  BacktestDepthExecutionSummary depthExecution{};
  BacktestStageTimings performance{};
  BacktestInputCoverage inputCoverage{};
  BacktestLiveProbeSummary liveProbe{};
  BacktestReplayClockEvidence replayClock{};
  std::uint64_t rateLimitRejectedCommands{0};
  BacktestTradeMode tradeMode{BacktestTradeMode::AllLegs};
  std::uint32_t primaryLegIndex{0u};
  bool rateLimitsEnabled{true};
  bool strictRateLimitRejects{false};
  bool rateLimitStrictFailure{false};
  bool hasPrimaryLegIndex{false};
  bool strategyClosed{false};
  bool liquidated{false};
  bool riskStopped{false};
};

using BacktestProgressCallback = bool (*)(const BacktestProgress& progress, void* userData) noexcept;

HFT_BACKTEST_API BacktestRunResult runBacktest(const BacktestRunRequest& request,
                                               BacktestProgressCallback progressCallback = nullptr,
                                               void* userData = nullptr) noexcept;

}  // namespace hft_backtest
