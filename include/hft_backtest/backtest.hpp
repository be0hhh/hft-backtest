#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "hft_backtest/api.hpp"
#include "hft_backtest/status.hpp"
#include "hft_backtest/strategy_metadata.hpp"
#include "hft_trader/core/rate_limit/RateLimit.hpp"

namespace hft_backtest {

struct BacktestLatencyProfile {
  std::uint64_t baseUs{0};
  std::uint64_t jitterUs{0};
};

struct BacktestSessionRequest {
  std::filesystem::path path{};
  std::string venue{};
  std::string symbol{};
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
  BacktestLatencyProfile marketData{};
  BacktestLatencyProfile marketOrder{};
  BacktestLatencyProfile limitOrder{};
  BacktestLatencyProfile cancelOrder{};
  BacktestLatencyProfile userData{};
};

struct BacktestRateLimitSchedule {
  std::string exchange{};
  std::string market{};
  std::vector<hft_trader::core::RateLimitBucketConfig> buckets{};
  std::vector<hft_trader::core::RateLimitActionConfig> actions{};
};

enum class BacktestExecutionPipeline : std::uint8_t {
  Inline = 0,
  TwoPhaseReprice = 1,
};

enum class BacktestTradeMode : std::uint8_t {
  AllLegs = 0,
  PrimaryOnly = 1,
};

struct BacktestRunRequest {
  std::filesystem::path sessionPath{};
  std::vector<BacktestSessionRequest> sessions{};
  std::filesystem::path configPath{};
  std::filesystem::path outputPath{};
  std::string requestId{};
  std::string runId{};
  std::string strategy{};
  std::string indicatorProfile{};
  std::uint64_t latencySeed{0};
  BacktestLatencyProfile marketDataLatency{};
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
  BacktestExecutionPipeline executionPipeline{BacktestExecutionPipeline::Inline};
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
  std::uint64_t cyclesWithPriceLimit{0};
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

struct BacktestLegSummary {
  std::filesystem::path sessionPath{};
  std::string exchange{};
  std::string market{};
  std::string symbol{};
  std::int64_t makerFeeBpsE8{0};
  std::int64_t takerFeeBpsE8{0};
  BacktestLatencyProfile marketDataLatency{};
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
  BacktestLatencyProfile marketDataLatency{};
  BacktestLatencyProfile marketOrderLatency{};
  BacktestLatencyProfile limitOrderLatency{};
  BacktestLatencyProfile cancelOrderLatency{};
  BacktestLatencyProfile userDataLatency{};
  std::uint64_t orderLatencyUs{1000};
  std::uint64_t cancelLatencyUs{1000};
  std::uint64_t trades{0};
  std::uint64_t bookTickers{0};
  std::uint64_t priceLimits{0};
  std::uint64_t depths{0};
  std::uint64_t liquidations{0};
  std::uint64_t markPrices{0};
  std::uint64_t indexPrices{0};
  std::uint64_t funding{0};
  std::uint64_t openInterest{0};
  std::uint64_t orders{0};
  std::uint64_t fills{0};
  std::uint64_t strategySpreadRows{0};
  std::uint64_t events{0};
  std::uint64_t userEvents{0};
  std::uint64_t timerWakeEvents{0};
  std::uint64_t reduceOnlyOrders{0};
  std::uint64_t reduceOnlyFills{0};
  std::uint64_t phase2DepthRows{0};
  std::uint64_t phase2FillsRepriced{0};
  std::uint64_t phase2FillsMissingDepth{0};
  std::uint64_t phase2BboRepriced{0};
  std::uint64_t phase2DepthVwapRepriced{0};
  std::uint64_t phase2DepthRejectedUnseeded{0};
  std::uint64_t phase2DepthRejectedCrossed{0};
  std::uint64_t phase2DepthRejectedStale{0};
  std::uint64_t phase2MissingBbo{0};
  bool twoPhaseRepriceApplied{false};
  bool twoPhaseRepriceFallback{false};
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
  BacktestInputCoverage inputCoverage{};
  BacktestLiveProbeSummary liveProbe{};
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
