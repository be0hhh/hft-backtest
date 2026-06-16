#include "hft_backtest/backtest.hpp"

#include "backtest_engine.hpp"
#include "backtest_metrics.hpp"
#include "backtest_prepared.hpp"
#include "session_loader.hpp"

#include "hft_trader/core/control/ControlState.hpp"
#include "hft_trader/core/market/LocalOrderBook.hpp"
#include "hft_trader/core/strategy/StrategyContract.hpp"
#include "hft_trader/runtime/GeneratedStrategyRegistry.hpp"
#include "hft_trader/runtime/config/RuntimeConfig.hpp"
#include "hft_trader/runtime/risk/PortfolioRiskRuntime.hpp"
#include "hft_trader/runtime/runner/StrategyMarketDataRuntime.hpp"
#include "primitives/composite/OrderBookTapeRuntimeV1.hpp"

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

namespace hft_backtest::detail {
namespace {

thread_local PreparedBacktestInput* gPreparedBacktestInput = nullptr;

}  // namespace

PreparedBacktestScope::PreparedBacktestScope(PreparedBacktestInput* input) noexcept
    : previous_(gPreparedBacktestInput) {
  gPreparedBacktestInput = input;
}

PreparedBacktestScope::~PreparedBacktestScope() {
  gPreparedBacktestInput = previous_;
}

PreparedBacktestInput* activePreparedBacktestInput() noexcept {
  return gPreparedBacktestInput;
}

}  // namespace hft_backtest::detail

namespace hft_backtest {
namespace {

using hft_trader::runtime::StrategyDescriptor;
using hft_trader::runtime::StrategyInstrumentSpec;
using hft_trader::runtime::StrategyParamStorage;

BacktestFeeSchedule resolveFeeSchedule(const BacktestRunRequest& request,
                                       const std::string& exchangeText,
                                       const std::string& marketText) noexcept;
hft_trader::core::StrategyAccountState accountStateFromEngine(const detail::EngineState& engine) noexcept;
BacktestInputCoverage addCoverage(BacktestInputCoverage lhs, const BacktestInputCoverage& rhs) noexcept;
BacktestLegSummary legSummaryFromEngine(const detail::SessionData& session,
                                        const detail::EngineState& engine) noexcept;
void populateAggregateArtifactEngine(const std::vector<detail::EngineState>& engines,
                                     std::int64_t initialBalanceE8,
                                     std::int64_t finalTsNs,
                                     detail::EngineState& out);

bool sameLoadPlan(const detail::SessionLoadPlan& lhs, const detail::SessionLoadPlan& rhs) noexcept {
  return lhs.loadTrades == rhs.loadTrades &&
         lhs.loadBookTickers == rhs.loadBookTickers &&
         lhs.loadPriceLimits == rhs.loadPriceLimits &&
         lhs.loadDepths == rhs.loadDepths &&
         lhs.loadCandles == rhs.loadCandles &&
         lhs.loadLiquidations == rhs.loadLiquidations &&
         lhs.loadMarkPrices == rhs.loadMarkPrices &&
         lhs.loadIndexPrices == rhs.loadIndexPrices &&
         lhs.loadFunding == rhs.loadFunding &&
         lhs.loadOpenInterest == rhs.loadOpenInterest &&
         lhs.requireTrades == rhs.requireTrades &&
         lhs.requireBookTickers == rhs.requireBookTickers &&
         lhs.requirePriceLimits == rhs.requirePriceLimits &&
         lhs.requireDepths == rhs.requireDepths &&
         lhs.requireCandles == rhs.requireCandles &&
         lhs.requireLiquidations == rhs.requireLiquidations &&
         lhs.requireMarkPrices == rhs.requireMarkPrices &&
         lhs.requireIndexPrices == rhs.requireIndexPrices &&
         lhs.requireFunding == rhs.requireFunding &&
         lhs.requireOpenInterest == rhs.requireOpenInterest;
}

std::vector<std::filesystem::path> sessionPathsFor(const BacktestRunRequest& request,
                                                   bool hasExplicitSessions) {
  std::vector<std::filesystem::path> out{};
  if (hasExplicitSessions) {
    out.reserve(request.sessions.size());
    for (const BacktestSessionRequest& session : request.sessions) out.push_back(session.path);
  } else {
    out.push_back(request.sessionPath);
  }
  return out;
}

bool preparedMatches(const detail::PreparedBacktestInput& prepared,
                     const BacktestRunRequest& request,
                     bool hasExplicitSessions,
                     const detail::SessionLoadPlan& loadPlan) {
  if (!prepared.valid || prepared.hasExplicitSessions != hasExplicitSessions || !sameLoadPlan(prepared.loadPlan, loadPlan)) return false;
  return prepared.sessionPaths == sessionPathsFor(request, hasExplicitSessions);
}

void storePreparedSessions(detail::PreparedBacktestInput& prepared,
                           const BacktestRunRequest& request,
                           bool hasExplicitSessions,
                           const detail::SessionLoadPlan& loadPlan,
                           std::vector<detail::SessionData>& sessions) {
  prepared.valid = true;
  prepared.timelineValid = false;
  prepared.hasExplicitSessions = hasExplicitSessions;
  prepared.loadPlan = loadPlan;
  prepared.sessionPaths = sessionPathsFor(request, hasExplicitSessions);
  prepared.sessions = std::move(sessions);
  prepared.timeline.clear();
}

bool marketDataLatencyKeepsOrder(const detail::BacktestLatencyConfig& config) noexcept {
  return config.marketData.jitterUs == 0u;
}

bool hasMarketDataLatency(const BacktestRunRequest& request,
                          const detail::BacktestLatencyConfig& fallback) noexcept {
  if (fallback.marketData.baseUs != 0u || fallback.marketData.jitterUs != 0u) return true;
  for (const BacktestLatencySchedule& schedule : request.latencySchedules) {
    if (schedule.marketData.baseUs != 0u || schedule.marketData.jitterUs != 0u) return true;
  }
  return false;
}

bool marketDataLatencyKeepsOrder(const BacktestRunRequest& request,
                                 const detail::BacktestLatencyConfig& fallback) noexcept {
  if (!marketDataLatencyKeepsOrder(fallback)) return false;
  const std::uint64_t baseUs = fallback.marketData.baseUs;
  for (const BacktestLatencySchedule& schedule : request.latencySchedules) {
    if (schedule.marketData.jitterUs != 0u) return false;
    if (schedule.marketData.baseUs != baseUs) return false;
  }
  return true;
}

std::string makeRunId() {
  const auto nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  return "run-" + std::to_string(nowNs);
}

std::filesystem::path resultPathFor(const BacktestRunRequest& request, const std::string& runId) {
  if (!request.outputPath.empty()) return request.outputPath;
  if (!request.sessionPath.empty()) return request.sessionPath / "backtests" / runId;
  if (!request.sessions.empty() && !request.sessions[0].path.empty()) return request.sessions[0].path / "backtests" / runId;
  return request.sessionPath / "backtests" / runId;
}

bool buildSessionLoadPlan(const StrategyDescriptor& descriptor, detail::SessionLoadPlan& out, std::string& error) noexcept {
  out = detail::SessionLoadPlan{};
  out.loadTrades = false;
  out.loadBookTickers = false;
  out.loadPriceLimits = false;
  out.loadDepths = false;
  out.loadCandles = descriptor.seedHistory != nullptr;
  out.requireCandles = descriptor.seedHistory != nullptr;
  if (descriptor.resolveTradeHistory && descriptor.seedTrades) out.loadTrades = true;
  const std::size_t streamCount = hft_trader::runtime::descriptorStreamCount(descriptor);
  for (std::size_t i = 0u; i < streamCount; ++i) {
    const cxet::api::market::PublicMarketDataStream stream = hft_trader::runtime::descriptorStreamAt(descriptor, i);
    if (stream == cxet::api::market::PublicMarketDataStream::Trades) {
      out.loadTrades = true;
      out.requireTrades = true;
    } else if (stream == cxet::api::market::PublicMarketDataStream::BookTicker) {
      out.loadBookTickers = true;
      out.requireBookTickers = true;
    } else if (stream == cxet::api::market::PublicMarketDataStream::PriceLimit) {
      out.loadPriceLimits = true;
      out.requirePriceLimits = true;
    } else if (stream == cxet::api::market::PublicMarketDataStream::Orderbook) {
      out.loadDepths = true;
      out.requireDepths = true;
    } else if (stream == cxet::api::market::PublicMarketDataStream::Liquidations) {
      out.loadLiquidations = true;
    } else if (stream == cxet::api::market::PublicMarketDataStream::MarkPrice) {
      out.loadMarkPrices = true;
    } else if (stream == cxet::api::market::PublicMarketDataStream::IndexPrice) {
      out.loadIndexPrices = true;
    } else if (stream == cxet::api::market::PublicMarketDataStream::Funding) {
      out.loadFunding = true;
    } else if (stream == cxet::api::market::PublicMarketDataStream::OpenInterest) {
      out.loadOpenInterest = true;
    } else {
      error = "strategy requires unsupported market data stream for backtest";
      return false;
    }
  }
  return true;
}

bool backtestStreamRequiredForReadiness(cxet::api::market::PublicMarketDataStream stream) noexcept {
  return stream == cxet::api::market::PublicMarketDataStream::Trades ||
         stream == cxet::api::market::PublicMarketDataStream::BookTicker ||
         stream == cxet::api::market::PublicMarketDataStream::PriceLimit ||
         stream == cxet::api::market::PublicMarketDataStream::Orderbook;
}

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

bool strategySnapshotReady(const StrategyDescriptor& descriptor,
                           const hft_trader::core::StrategyMarketSnapshot& snapshot) noexcept {
  const std::size_t streamCount = hft_trader::runtime::descriptorStreamCount(descriptor);
  for (std::size_t i = 0u; i < streamCount; ++i) {
    const cxet::api::market::PublicMarketDataStream stream = hft_trader::runtime::descriptorStreamAt(descriptor, i);
    if (!backtestStreamRequiredForReadiness(stream)) continue;
    if (!hft_trader::runtime::snapshotHasRequiredStream(snapshot, stream, &descriptor)) return false;
  }
  return true;
}

bool isBacktestProbe(const StrategyDescriptor& descriptor) noexcept {
  return descriptor.name && std::string_view{descriptor.name} == "backtest_probe";
}

bool isRangeReversion(const StrategyDescriptor& descriptor) noexcept {
  return descriptor.name && std::string_view{descriptor.name} == "range_reversion";
}

bool isStatArbBestside(const StrategyDescriptor& descriptor) noexcept {
  return descriptor.name && std::string_view{descriptor.name} == "stat_arb_bestside";
}

void prefetchRead(const void* ptr) noexcept {
#if defined(__GNUC__) || defined(__clang__)
  __builtin_prefetch(ptr, 0, 1);
#else
  (void)ptr;
#endif
}

std::size_t boundedReserve(std::size_t events, std::size_t divisor, std::size_t base) noexcept {
  return events == 0u ? base : events / divisor + base;
}

void reserveEngineStorage(detail::EngineState& engine,
                          std::size_t events,
                          bool captureProbeTrace,
                          bool captureIndicatorTrace,
                          bool captureRangeTrace,
                          bool captureSpreadTrace) {
  const std::size_t tradeRows = boundedReserve(events, 16u, 64u);
  engine.pending.reserve(detail::kIntentCapacity);
  engine.orders.reserve(tradeRows);
  engine.fills.reserve(tradeRows);
  engine.userEvents.reserve(tradeRows);
  engine.equityPoints.reserve(boundedReserve(events, 64u, 16u));
  engine.riskSnapshots.reserve(boundedReserve(events, 1024u, 8u));
  if (captureProbeTrace) engine.probeMetrics.reserve(events + 8u);
  if (captureIndicatorTrace) engine.strategyIndicator.reserve(events + 8u);
  if (captureRangeTrace) engine.strategyRange.reserve(events + 8u);
  if (captureSpreadTrace) engine.strategySpread.reserve(events + 8u);
}

struct StrategyIndicatorProfileInfo {
  std::string_view id{};
  std::string_view title{};
  std::string_view valueLabel{};
  std::string_view auxLabel{};
  std::string_view unit{};
};

bool resolveStrategyIndicatorProfile(std::string_view id, StrategyIndicatorProfileInfo& out) noexcept {
  out = StrategyIndicatorProfileInfo{};
  if (id.empty() || id == "none") return true;
  if (id == "rolling_volume") {
    out = StrategyIndicatorProfileInfo{"rolling_volume", "Rolling volume", "Rolling/min", "EMA/min", "raw"};
    return true;
  }
  if (id == "trade_range") {
    out = StrategyIndicatorProfileInfo{"trade_range", "Trade range", "EMA", "Sample", "bps"};
    return true;
  }
  if (id == "wall_density") {
    out = StrategyIndicatorProfileInfo{"wall_density", "Wall density", "Current", "Max seen", "notional"};
    return true;
  }
  if (id == "range_position") {
    out = StrategyIndicatorProfileInfo{"range_position", "Range position", "Position", "Trend", "score"};
    return true;
  }
  return false;
}


bool strategyDeclaresIndicator(std::string_view strategy, std::string_view profile) noexcept {
  if (profile.empty()) return true;
  const hft_backtest::StrategyMetadata* metadata = hft_backtest::findStrategyMetadata(strategy);
  if (metadata == nullptr) return false;
  for (std::size_t i = 0; i < metadata->indicatorCount && i < hft_backtest::kStrategyMetadataMaxIndicators; ++i) {
    const hft_backtest::StrategyIndicatorMetadata& indicator = metadata->indicators[i];
    if (indicator.id != nullptr && profile == std::string_view{indicator.id}) return true;
  }
  return false;
}

bool strategyIndicatorEnabled(const detail::EngineState& engine) noexcept {
  return !engine.strategyIndicatorProfile.empty();
}

void configureStrategyIndicator(detail::EngineState& engine, const StrategyIndicatorProfileInfo& profile) {
  engine.strategyIndicatorProfile = std::string{profile.id};
  engine.strategyIndicatorTitle = std::string{profile.title};
  engine.strategyIndicatorValueLabel = std::string{profile.valueLabel};
  engine.strategyIndicatorAuxLabel = std::string{profile.auxLabel};
  engine.strategyIndicatorUnit = std::string{profile.unit};
}

std::int64_t maxWallValue(const hft_trader::core::StrategyDecisionTrace& trace, bool maxSeen) noexcept {
  std::int64_t out = 0;
  for (std::size_t side = 0; side < hft_trader::core::kStrategyDecisionWallSideCount; ++side) {
    for (std::size_t rank = 0; rank < hft_trader::core::kStrategyDecisionWallRankCount; ++rank) {
      if (trace.wallActive[side][rank] == 0u && trace.wallConfirmed[side][rank] == 0u && trace.wallDropped[side][rank] == 0u) continue;
      const std::int64_t value = maxSeen ? trace.wallMaxSeenNotionalRaw[side][rank] : trace.wallCurrentNotionalRaw[side][rank];
      if (value > out) out = value;
    }
  }
  return out;
}

bool hasWallDrop(const hft_trader::core::StrategyDecisionTrace& trace) noexcept {
  for (std::size_t side = 0; side < hft_trader::core::kStrategyDecisionWallSideCount; ++side) {
    for (std::size_t rank = 0; rank < hft_trader::core::kStrategyDecisionWallRankCount; ++rank) {
      if (trace.wallDropped[side][rank] != 0u) return true;
    }
  }
  return false;
}

std::uint8_t indicatorEventCode(const hft_trader::core::StrategyDecisionTrace& trace) noexcept {
  if (hasWallDrop(trace)) return 2u;
  if (trace.decisionKind == static_cast<std::uint8_t>(hft_trader::core::StrategyDecisionKind::Skip) && trace.reasonRaw != 0u) return 4u;
  if (trace.decisionKind == static_cast<std::uint8_t>(hft_trader::core::StrategyDecisionKind::Enter) ||
      trace.decisionKind == static_cast<std::uint8_t>(hft_trader::core::StrategyDecisionKind::Close) ||
      trace.bidTriggered != 0u || trace.askTriggered != 0u) return 1u;
  return 0u;
}

std::int64_t clampU64ToI64(std::uint64_t value) noexcept {
  const std::uint64_t max = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  return static_cast<std::int64_t>(value > max ? max : value);
}

void appendStrategyIndicator(detail::EngineState& engine,
                             std::int64_t tsNs,
                             const hft_trader::core::StrategyDecisionTrace& trace) {
  if (!strategyIndicatorEnabled(engine)) return;
  detail::JsonStrategyIndicatorPoint row{};
  row.tsNs = tsNs;
  if (engine.strategyIndicatorProfile == "rolling_volume") {
    row.valueRaw = clampU64ToI64(trace.rollingPerMinuteRaw);
    row.auxRaw = clampU64ToI64(trace.volumeEmaPerMinuteRaw);
  } else if (engine.strategyIndicatorProfile == "trade_range") {
    row.valueRaw = static_cast<std::int64_t>(trace.tradeRangeEmaBps);
    row.auxRaw = static_cast<std::int64_t>(trace.tradeRangeSampleBps);
  } else if (engine.strategyIndicatorProfile == "wall_density") {
    row.valueRaw = maxWallValue(trace, false);
    row.auxRaw = maxWallValue(trace, true);
  } else if (engine.strategyIndicatorProfile == "range_position") {
    row.valueRaw = static_cast<std::int64_t>(trace.rangePositionBps / 100u);
    row.auxRaw = static_cast<std::int64_t>(trace.trendScoreRaw);
  }
  row.eventCode = indicatorEventCode(trace);
  row.reasonRaw = trace.reasonRaw;
  row.decisionKind = trace.decisionKind;
  row.sideRaw = trace.sideRaw;
  engine.strategyIndicator.push_back(row);
}

void appendStrategyRange(detail::EngineState& engine,
                         std::int64_t tsNs,
                         const hft_trader::core::StrategyDecisionTrace& trace) {
  if (trace.rangeReady == 0u || trace.rangeLowRaw <= 0 || trace.rangeHighRaw <= trace.rangeLowRaw) return;
  detail::JsonStrategyRangePoint row{};
  row.tsNs = tsNs;
  row.lowRaw = trace.rangeLowRaw;
  row.midRaw = trace.rangeMidRaw;
  row.highRaw = trace.rangeHighRaw;
  engine.strategyRange.push_back(row);
}

void appendStrategySpread(detail::EngineState& engine,
                          std::int64_t tsNs,
                          const hft_trader::core::StrategyDecisionTrace& trace) {
  if (trace.arbReady == 0u) return;
  detail::JsonStrategySpreadPoint row{};
  row.tsNs = tsNs;
  row.spreadBpsE8 = trace.arbSpreadBpsE8;
  row.emaBpsE8 = trace.arbEmaBpsE8;
  row.deviationBpsE8 = trace.arbDeviationBpsE8;
  row.costBandBpsE8 = trace.arbCostBandBpsE8;
  row.edgeAfterCostBpsE8 = trace.arbEdgeAfterCostBpsE8;
  row.direction = trace.arbDirection;
  row.decisionKind = trace.decisionKind;
  row.reasonRaw = trace.reasonRaw;
  engine.strategySpread.push_back(row);
}

void recordInputCoverage(detail::EngineState& engine) noexcept {
  ++engine.inputCoverage.strategyCycles;
  if (engine.snapshot.hasTrade.raw == hft_trader::core::kAlgoFlagTrue.raw) ++engine.inputCoverage.cyclesWithTrade;
  if (engine.snapshot.hasBookTicker.raw == hft_trader::core::kAlgoFlagTrue.raw) ++engine.inputCoverage.cyclesWithBookTicker;
  if (engine.snapshot.hasPriceLimit.raw == hft_trader::core::kAlgoFlagTrue.raw) ++engine.inputCoverage.cyclesWithPriceLimit;
  if (engine.snapshot.hasDepth.raw == hft_trader::core::kAlgoFlagTrue.raw) ++engine.inputCoverage.cyclesWithDepth;
  if (engine.snapshot.hasOrderBook.raw == hft_trader::core::kAlgoFlagTrue.raw && engine.snapshot.orderBook) ++engine.inputCoverage.cyclesWithOrderBook;
}

void appendProbeMetric(detail::EngineState& engine,
                       std::int64_t tsNs,
                       const hft_trader::core::StrategyDecisionTrace& trace) {
  detail::JsonProbeMetric row{};
  row.tsNs = tsNs;
  row.decisionKind = trace.decisionKind;
  row.reasonRaw = trace.reasonRaw;
  row.sideRaw = trace.sideRaw;
  row.orderCount = trace.orderCount;
  row.amendCount = trace.amendCount;
  row.cancelCount = trace.cancelCount;
  row.bestBidRaw = trace.bestBidRaw;
  row.bestAskRaw = trace.bestAskRaw;
  row.tradeRangeSampleBps = trace.tradeRangeSampleBps;
  row.tradeRangeEmaBps = trace.tradeRangeEmaBps;
  row.tradeRangeObservedSeconds = trace.tradeRangeObservedSeconds;
  row.rollingPerMinuteRaw = trace.rollingPerMinuteRaw;
  row.volumeWindowObservedSeconds = trace.volumeWindowObservedSeconds;
  row.tradeRangeReady = trace.tradeRangeReady;
  row.volumeWindowReady = trace.volumeWindowReady;
  row.hasTrade = engine.snapshot.hasTrade.raw == hft_trader::core::kAlgoFlagTrue.raw ? 1u : 0u;
  row.hasBookTicker = engine.snapshot.hasBookTicker.raw == hft_trader::core::kAlgoFlagTrue.raw ? 1u : 0u;
  row.hasPriceLimit = engine.snapshot.hasPriceLimit.raw == hft_trader::core::kAlgoFlagTrue.raw ? 1u : 0u;
  row.hasDepth = engine.snapshot.hasDepth.raw == hft_trader::core::kAlgoFlagTrue.raw ? 1u : 0u;
  row.hasOrderBook = engine.snapshot.hasOrderBook.raw == hft_trader::core::kAlgoFlagTrue.raw && engine.snapshot.orderBook ? 1u : 0u;
  engine.probeMetrics.push_back(row);
}

bool probeMetricsReady(const std::vector<detail::JsonProbeMetric>& metrics) noexcept {
  for (const detail::JsonProbeMetric& metric : metrics) {
    if (metric.tradeRangeReady != 0u && metric.volumeWindowReady != 0u && metric.tradeRangeObservedSeconds != 0u &&
        metric.volumeWindowObservedSeconds != 0u && metric.rollingPerMinuteRaw != 0u) return true;
  }
  return false;
}

bool probeMetricsUpdatedFromReplay(const std::vector<detail::JsonProbeMetric>& metrics) noexcept {
  for (const detail::JsonProbeMetric& metric : metrics) {
    if (metric.hasTrade != 0u && metric.tradeRangeReady != 0u && metric.volumeWindowReady != 0u) return true;
  }
  return false;
}

bool validateBacktestProbeCoverage(const BacktestRunResult& result, const detail::EngineState& engine, std::string& error) {
  error.clear();
  const BacktestInputCoverage& coverage = result.inputCoverage;
  if (coverage.strategyCycles == 0u) error = "backtest_probe did not run strategy cycles";
  else if (coverage.cyclesWithTrade == 0u) error = "backtest_probe did not observe trade input";
  else if (coverage.cyclesWithBookTicker == 0u) error = "backtest_probe did not observe bookticker input";
  else if (coverage.cyclesWithPriceLimit == 0u) error = "backtest_probe did not observe price_limit input";
  else if (coverage.cyclesWithDepth == 0u) error = "backtest_probe did not observe depth input";
  else if (coverage.cyclesWithOrderBook == 0u) error = "backtest_probe did not observe reconstructed orderbook input";
  else if (coverage.seedTradeRows == 0u) error = "backtest_probe did not receive trade warmup rows";
  else if (coverage.seedM1Candles == 0u || coverage.seedM15Candles == 0u || coverage.seedD1Candles == 0u) {
    error = "backtest_probe did not receive full m1/m15/d1 candle seed";
  } else if (engine.probeMetrics.empty()) {
    error = "backtest_probe did not write probe metrics";
  } else if (!probeMetricsReady(engine.probeMetrics)) {
    error = "backtest_probe warmup did not produce ready range and volume metrics";
  } else if (!probeMetricsUpdatedFromReplay(engine.probeMetrics)) {
    error = "backtest_probe did not update range and volume metrics from replay trades";
  } else if (result.orders == 0u) {
    error = "backtest_probe did not emit an order";
  } else if (result.fills == 0u) {
    error = "backtest_probe did not receive simulated fills";
  } else if (result.userEvents == 0u) {
    error = "backtest_probe did not receive user events";
  } else if (result.timerWakeEvents == 0u) {
    error = "backtest_probe did not receive timer wake events";
  } else if (!result.strategyClosed) {
    error = "backtest_probe did not close its position";
  }
  return error.empty();
}

bool runCycle(const StrategyDescriptor& descriptor,
              void* state,
              detail::EngineState& engine,
              const hft_trader::core::SymbolShardConfig& shard,
              const hft_trader::core::ShardControlState& control,
              std::int64_t tsNs,
              const BacktestRunRequest& request,
              const cxet::composite::UserDataEvent* userEvent) {
  if (engine.liquidated || engine.riskStopped) return true;
  if (!strategySnapshotReady(descriptor, engine.snapshot)) return true;
  std::array<hft_trader::core::OrderIntent, detail::kIntentCapacity> orderBuf{};
  std::array<hft_trader::core::AmendIntent, detail::kIntentCapacity> amendBuf{};
  std::array<hft_trader::core::CancelIntent, detail::kIntentCapacity> cancelBuf{};
  std::array<hft_trader::core::StrategyOrderState, detail::kMaxOrderStates> orderViewStorage{};
  const std::size_t orderViewCount = detail::visibleOrders(engine, orderViewStorage.data(), orderViewStorage.size());
  hft_trader::core::StrategyContext ctx{};
  ctx.shard = &shard;
  ctx.control = &control;
  ctx.snapshot = &engine.snapshot;
  ctx.marketData.snapshots = &engine.snapshot;
  ctx.marketData.count = 1;
  ctx.orderState.orders = orderViewStorage.data();
  ctx.orderState.count = orderViewCount;
  ctx.positionState = engine.position;
  ctx.accountState = accountStateFromEngine(engine);
  ctx.accountStates.legs = &ctx.accountState;
  ctx.accountStates.count = 1u;
  ctx.userEvent = userEvent;
  ctx.hasUserEvent = userEvent && userEvent->kind != cxet::composite::UserDataEventKind::Unknown;
  ctx.nowMono.raw = static_cast<std::uint64_t>(tsNs > 0 ? tsNs : 0);
  ctx.nowUtc = ctx.nowMono;
  ctx.userEventRecvMono = ctx.nowMono;
  ctx.userEventRecvUtc = ctx.nowUtc;
  ctx.initialOrderSeedAllowed = true;
  ctx.io.orderOut = Span<hft_trader::core::OrderIntent>(orderBuf.data(), orderBuf.size());
  ctx.io.amendOut = Span<hft_trader::core::AmendIntent>(amendBuf.data(), amendBuf.size());
  ctx.io.cancelOut = Span<hft_trader::core::CancelIntent>(cancelBuf.data(), cancelBuf.size());
  ctx.io.clear();
  recordInputCoverage(engine);
  hft_trader::core::StrategyDecisionTrace trace{};
  const bool captureProbeTrace = isBacktestProbe(descriptor);
  const bool captureIndicatorTrace = strategyIndicatorEnabled(engine);
  const bool captureRangeTrace = isRangeReversion(descriptor);
  const bool captureSpreadTrace = isStatArbBestside(descriptor);
  if (captureProbeTrace || captureIndicatorTrace || captureRangeTrace || captureSpreadTrace) {
    hft_trader::core::initStrategyDecisionTrace(trace, ctx.nowMono.raw);
    ctx.decisionTrace = &trace;
  }
  const bool ok = descriptor.runCycle(state, ctx);
  if (!hft_trader::core::canEmitNewOrder(control)) {
    ctx.io.orderCount = 0u;
    ctx.io.amendCount = 0u;
  }
  if (captureProbeTrace || captureIndicatorTrace || captureRangeTrace || captureSpreadTrace) {
    trace.orderCount = static_cast<std::uint32_t>(ctx.io.orderCount);
    trace.amendCount = static_cast<std::uint32_t>(ctx.io.amendCount);
    trace.cancelCount = static_cast<std::uint32_t>(ctx.io.cancelCount);
    if (captureProbeTrace) appendProbeMetric(engine, tsNs, trace);
    if (captureIndicatorTrace) appendStrategyIndicator(engine, tsNs, trace);
    if (captureRangeTrace) appendStrategyRange(engine, tsNs, trace);
    if (captureSpreadTrace) appendStrategySpread(engine, tsNs, trace);
  }
  if (captureProbeTrace && !ok) return false;
  detail::applyIntents(engine, ctx.io, tsNs, engine.latency);
  return true;
}

std::size_t backtestRouteLegIndex(std::size_t legCount,
                                  std::size_t primaryLegIndex,
                                  hft_trader::core::ExecutionRouteHandle route) noexcept {
  if (legCount == 0u) return static_cast<std::size_t>(-1);
  if (route.raw == hft_trader::core::kDefaultExecutionRouteHandle.raw) return primaryLegIndex < legCount ? primaryLegIndex : 0u;
  const std::size_t index = static_cast<std::size_t>(route.raw - 1u);
  return index < legCount ? index : static_cast<std::size_t>(-1);
}

void applyIntentsToLegs(std::vector<detail::EngineState>& engines,
                        const hft_trader::core::StrategyIo& io,
                        std::size_t primaryLegIndex,
                        std::int64_t tsNs) {
  for (std::size_t i = 0; i < io.orderCount; ++i) {
    const std::size_t leg = backtestRouteLegIndex(engines.size(), primaryLegIndex, io.orderOut[i].routeHandle);
    if (leg >= engines.size()) continue;
    hft_trader::core::StrategyIo one{};
    one.orderOut = Span<hft_trader::core::OrderIntent>(const_cast<hft_trader::core::OrderIntent*>(&io.orderOut[i]), 1u);
    one.orderCount = 1u;
    detail::applyIntents(engines[leg], one, tsNs, engines[leg].latency);
  }
  for (std::size_t i = 0; i < io.amendCount; ++i) {
    const std::size_t leg = backtestRouteLegIndex(engines.size(), primaryLegIndex, io.amendOut[i].routeHandle);
    if (leg >= engines.size()) continue;
    hft_trader::core::StrategyIo one{};
    one.amendOut = Span<hft_trader::core::AmendIntent>(const_cast<hft_trader::core::AmendIntent*>(&io.amendOut[i]), 1u);
    one.amendCount = 1u;
    detail::applyIntents(engines[leg], one, tsNs, engines[leg].latency);
  }
  for (std::size_t i = 0; i < io.cancelCount; ++i) {
    const std::size_t leg = backtestRouteLegIndex(engines.size(), primaryLegIndex, io.cancelOut[i].routeHandle);
    if (leg >= engines.size()) continue;
    hft_trader::core::StrategyIo one{};
    one.cancelOut = Span<hft_trader::core::CancelIntent>(const_cast<hft_trader::core::CancelIntent*>(&io.cancelOut[i]), 1u);
    one.cancelCount = 1u;
    detail::applyIntents(engines[leg], one, tsNs, engines[leg].latency);
  }
}

bool runCycleMulti(const StrategyDescriptor& descriptor,
                   void* state,
                   std::vector<detail::EngineState>& engines,
                   const hft_trader::core::SymbolShardConfig& shard,
                   const hft_trader::core::ShardControlState& control,
                   std::size_t primaryLegIndex,
                   std::uint32_t eventLegIndex,
                   std::int64_t tsNs,
                   const BacktestRunRequest& request,
                   const cxet::composite::UserDataEvent* userEvent) {
  (void)request;
  if (engines.empty()) return true;
  for (const detail::EngineState& engine : engines) {
    if (engine.liquidated || engine.riskStopped) return true;
  }
  if (primaryLegIndex >= engines.size()) primaryLegIndex = 0u;
  for (const detail::EngineState& engine : engines) {
    if (!strategySnapshotReady(descriptor, engine.snapshot)) return true;
  }
  std::array<hft_trader::core::OrderIntent, detail::kIntentCapacity> orderBuf{};
  std::array<hft_trader::core::AmendIntent, detail::kIntentCapacity> amendBuf{};
  std::array<hft_trader::core::CancelIntent, detail::kIntentCapacity> cancelBuf{};
  std::array<hft_trader::core::StrategyMarketSnapshot, hft_trader::core::kStrategyRuntimeMaxLegs> snapshots{};
  std::array<std::array<hft_trader::core::StrategyOrderState, detail::kMaxOrderStates>, hft_trader::core::kStrategyRuntimeMaxLegs> orderStorage{};
  std::array<hft_trader::core::StrategyOrderStateView, hft_trader::core::kStrategyRuntimeMaxLegs> orderViews{};
  std::array<hft_trader::core::StrategyPositionState, hft_trader::core::kStrategyRuntimeMaxLegs> positions{};
  std::array<hft_trader::core::StrategyAccountState, hft_trader::core::kStrategyRuntimeMaxLegs> accounts{};
  const std::size_t legCount = engines.size() < hft_trader::core::kStrategyRuntimeMaxLegs ? engines.size() : hft_trader::core::kStrategyRuntimeMaxLegs;
  for (std::size_t leg = 0; leg < legCount; ++leg) {
    snapshots[leg] = engines[leg].snapshot;
    orderViews[leg].orders = orderStorage[leg].data();
    orderViews[leg].count = detail::visibleOrders(engines[leg], orderStorage[leg].data(), orderStorage[leg].size());
    positions[leg] = engines[leg].position;
    accounts[leg] = accountStateFromEngine(engines[leg]);
  }
  hft_trader::core::StrategyContext ctx{};
  ctx.shard = &shard;
  ctx.control = &control;
  ctx.snapshot = &snapshots[primaryLegIndex];
  ctx.marketData.snapshots = snapshots.data();
  ctx.marketData.count = legCount;
  ctx.orderState = orderViews[primaryLegIndex];
  ctx.orderStates.legs = orderViews.data();
  ctx.orderStates.count = legCount;
  ctx.positionState = positions[primaryLegIndex];
  ctx.positionStates.legs = positions.data();
  ctx.positionStates.count = legCount;
  ctx.accountState = accounts[primaryLegIndex];
  ctx.accountStates.legs = accounts.data();
  ctx.accountStates.count = legCount;
  ctx.userEvent = userEvent;
  ctx.hasUserEvent = userEvent && userEvent->kind != cxet::composite::UserDataEventKind::Unknown;
  ctx.eventLegIndex = eventLegIndex;
  ctx.hasEventLeg = eventLegIndex < legCount;
  ctx.nowMono.raw = static_cast<std::uint64_t>(tsNs > 0 ? tsNs : 0);
  ctx.nowUtc = ctx.nowMono;
  ctx.userEventRecvMono = ctx.nowMono;
  ctx.userEventRecvUtc = ctx.nowUtc;
  ctx.initialOrderSeedAllowed = true;
  ctx.io.orderOut = Span<hft_trader::core::OrderIntent>(orderBuf.data(), orderBuf.size());
  ctx.io.amendOut = Span<hft_trader::core::AmendIntent>(amendBuf.data(), amendBuf.size());
  ctx.io.cancelOut = Span<hft_trader::core::CancelIntent>(cancelBuf.data(), cancelBuf.size());
  ctx.io.clear();
  recordInputCoverage(engines[primaryLegIndex]);
  hft_trader::core::StrategyDecisionTrace trace{};
  const bool captureProbeTrace = isBacktestProbe(descriptor);
  const bool captureIndicatorTrace = strategyIndicatorEnabled(engines[primaryLegIndex]);
  const bool captureRangeTrace = isRangeReversion(descriptor);
  const bool captureSpreadTrace = isStatArbBestside(descriptor);
  if (captureProbeTrace || captureIndicatorTrace || captureRangeTrace || captureSpreadTrace) {
    hft_trader::core::initStrategyDecisionTrace(trace, ctx.nowMono.raw);
    ctx.decisionTrace = &trace;
  }
  const bool ok = descriptor.runCycle(state, ctx);
  if (!hft_trader::core::canEmitNewOrder(control)) {
    ctx.io.orderCount = 0u;
    ctx.io.amendCount = 0u;
  }
  if (captureProbeTrace || captureIndicatorTrace || captureRangeTrace || captureSpreadTrace) {
    trace.orderCount = static_cast<std::uint32_t>(ctx.io.orderCount);
    trace.amendCount = static_cast<std::uint32_t>(ctx.io.amendCount);
    trace.cancelCount = static_cast<std::uint32_t>(ctx.io.cancelCount);
    if (captureProbeTrace) appendProbeMetric(engines[primaryLegIndex], tsNs, trace);
    if (captureIndicatorTrace) appendStrategyIndicator(engines[primaryLegIndex], tsNs, trace);
    if (captureRangeTrace) appendStrategyRange(engines[primaryLegIndex], tsNs, trace);
    if (captureSpreadTrace) appendStrategySpread(engines[primaryLegIndex], tsNs, trace);
  }
  if (!ok && captureProbeTrace) return false;
  applyIntentsToLegs(engines, ctx.io, primaryLegIndex, tsNs);
  return true;
}

std::uint64_t hashRunSeed(const std::string& text) noexcept {
  std::uint64_t out = 1469598103934665603ull;
  for (char c : text) {
    out ^= static_cast<unsigned char>(c);
    out *= 1099511628211ull;
  }
  return out == 0u ? 88172645463325252ull : out;
}

BacktestFeeSchedule resolveFeeSchedule(const BacktestRunRequest& request,
                                       const std::string& exchangeText,
                                       const std::string& marketText) noexcept {
  const ExchangeId exchange = detail::exchangeFromText(exchangeText);
  const canon::MarketType market = detail::marketFromText(marketText);
  for (const BacktestFeeSchedule& schedule : request.feeSchedules) {
    if (detail::exchangeFromText(schedule.exchange).raw != exchange.raw) continue;
    if (detail::marketFromText(schedule.market).raw != market.raw) continue;
    return schedule;
  }
  BacktestFeeSchedule fallback{};
  fallback.exchange = exchangeText;
  fallback.market = marketText;
  fallback.makerFeeBpsE8 = request.makerFeeBpsE8;
  fallback.takerFeeBpsE8 = request.takerFeeBpsE8;
  return fallback;
}

detail::BacktestLatencyConfig resolveLatencyConfig(const BacktestRunRequest& request,
                                                   const std::string& exchangeText,
                                                   const std::string& marketText) noexcept {
  detail::BacktestLatencyConfig fallback = detail::latencyConfigFromRequest(request);
  const ExchangeId exchange = detail::exchangeFromText(exchangeText);
  const canon::MarketType market = detail::marketFromText(marketText);
  for (const BacktestLatencySchedule& schedule : request.latencySchedules) {
    if (detail::exchangeFromText(schedule.exchange).raw != exchange.raw) continue;
    if (detail::marketFromText(schedule.market).raw != market.raw) continue;
    fallback.marketData = schedule.marketData;
    fallback.marketOrder = schedule.marketOrder;
    fallback.limitOrder = schedule.limitOrder;
    return fallback;
  }
  return fallback;
}

detail::BacktestLatencyConfig latencyConfigForLeg(const BacktestRunRequest& request,
                                                  const std::string& exchangeText,
                                                  const std::string& marketText,
                                                  std::size_t legIndex) noexcept {
  detail::BacktestLatencyConfig out = resolveLatencyConfig(request, exchangeText, marketText);
  out.seed ^= hashRunSeed(exchangeText);
  out.seed ^= (hashRunSeed(marketText) << 1u);
  out.seed ^= (static_cast<std::uint64_t>(legIndex + 1u) * 0x9e3779b97f4a7c15ull);
  return out;
}

hft_trader::core::StrategyAccountState accountStateFromEngine(const detail::EngineState& engine) noexcept {
  hft_trader::core::StrategyAccountState out{};
  out.initialBalanceE8 = engine.initialBalanceE8;
  out.walletBalanceE8 = engine.walletBalanceE8;
  out.availableBalanceE8 = engine.walletBalanceE8;
  out.equityE8 = detail::equityE8(engine);
  out.makerFeeBpsE8 = engine.makerFeeBpsE8;
  out.takerFeeBpsE8 = engine.takerFeeBpsE8;
  out.hasBalance = engine.initialBalanceE8 != 0 || engine.walletBalanceE8 != 0;
  out.hasFees = engine.makerFeeBpsE8 != 0 || engine.takerFeeBpsE8 != 0;
  return out;
}

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
  out.marketDataLatency = engine.latency.config.marketData;
  out.marketOrderLatency = engine.latency.config.marketOrder;
  out.limitOrderLatency = engine.latency.config.limitOrder;
  out.orders = engine.orders.size();
  out.fills = engine.fills.size();
  out.userEvents = engine.userEvents.size();
  out.timerWakeEvents = engine.timerWakeEvents;
  out.reduceOnlyOrders = engine.reduceOnlyOrders;
  out.reduceOnlyFills = engine.reduceOnlyFills;
  out.grossRealizedPnlE8 = engine.grossRealizedPnlE8;
  out.feesPaidE8 = engine.feesPaidE8;
  out.netRealizedPnlE8 = engine.netRealizedPnlE8;
  out.unrealizedPnlE8 = detail::unrealizedPnlE8(engine);
  out.totalPnlE8 = out.netRealizedPnlE8 + out.unrealizedPnlE8;
  out.walletDeltaE8 = engine.netRealizedPnlE8;
  out.openPositionQtyE8 = engine.netQtyRaw;
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
    out.probeMetrics.insert(out.probeMetrics.end(), engine.probeMetrics.begin(), engine.probeMetrics.end());
    out.strategyIndicator.insert(out.strategyIndicator.end(), engine.strategyIndicator.begin(), engine.strategyIndicator.end());
    out.strategyRange.insert(out.strategyRange.end(), engine.strategyRange.begin(), engine.strategyRange.end());
    out.strategySpread.insert(out.strategySpread.end(), engine.strategySpread.begin(), engine.strategySpread.end());
    out.userEvents.insert(out.userEvents.end(), engine.userEvents.begin(), engine.userEvents.end());
    out.inputCoverage = addCoverage(out.inputCoverage, engine.inputCoverage);
    out.timerWakeEvents += engine.timerWakeEvents;
    out.reduceOnlyOrders += engine.reduceOnlyOrders;
    out.reduceOnlyFills += engine.reduceOnlyFills;
    out.netQtyRaw += engine.netQtyRaw;
    out.grossRealizedPnlE8 += engine.grossRealizedPnlE8;
    out.netRealizedPnlE8 += engine.netRealizedPnlE8;
    out.feesPaidE8 += engine.feesPaidE8;
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
    out.grossRealizedPnlE8 = out.netRealizedPnlE8 + out.feesPaidE8;
  }
  out.walletBalanceE8 = initialBalanceE8 + out.netRealizedPnlE8;
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

std::uint64_t maxLatencyUs(const BacktestRunRequest& request) noexcept {
  const detail::BacktestLatencyConfig config = detail::latencyConfigFromRequest(request);
  std::uint64_t out = detail::latencyProfileMaxUs(config.marketData);
  const std::uint64_t marketOrder = detail::latencyProfileMaxUs(config.marketOrder);
  const std::uint64_t limitOrder = detail::latencyProfileMaxUs(config.limitOrder);
  if (marketOrder > out) out = marketOrder;
  if (limitOrder > out) out = limitOrder;
  for (const BacktestLatencySchedule& schedule : request.latencySchedules) {
    const std::uint64_t md = detail::latencyProfileMaxUs(schedule.marketData);
    const std::uint64_t mo = detail::latencyProfileMaxUs(schedule.marketOrder);
    const std::uint64_t lo = detail::latencyProfileMaxUs(schedule.limitOrder);
    if (md > out) out = md;
    if (mo > out) out = mo;
    if (lo > out) out = lo;
  }
  return out;
}

std::int64_t toInt64Time(TimeNs value) noexcept {
  if (value.raw > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return std::numeric_limits<std::int64_t>::max();
  return static_cast<std::int64_t>(value.raw);
}

TimeNs timeNsFromI64(std::int64_t value) noexcept {
  TimeNs out{};
  if (value > 0) out.raw = static_cast<std::uint64_t>(value);
  return out;
}

bool buildTradeWarmupRows(const detail::SessionData& session,
                          std::vector<cxet::composite::TradeRuntimeV1>& out,
                          hft_trader::runtime::StrategyTradeHistorySeedStats& stats) {
  out.clear();
  out.reserve(session.tradeWarmup.size());
  stats = hft_trader::runtime::StrategyTradeHistorySeedStats{};
  stats.rowsFetched = static_cast<std::uint64_t>(session.tradeHistoryRows > 0 ? session.tradeHistoryRows : static_cast<std::int64_t>(session.tradeWarmup.size()));
  stats.rowsProvided = static_cast<std::uint64_t>(session.tradeWarmup.size());
  stats.firstTs = timeNsFromI64(session.tradeWarmup.empty() ? 0 : session.tradeWarmup.front().tsNs);
  stats.lastTs = timeNsFromI64(session.tradeHistoryRequestedEndNs > 0 ? session.tradeHistoryRequestedEndNs : (session.tradeWarmup.empty() ? 0 : session.tradeWarmup.back().tsNs));
  for (const detail::TradeRow& row : session.tradeWarmup) out.push_back(detail::toRuntime(row));
  return !out.empty();
}

bool seedTradeWarmupFromSession(const StrategyDescriptor& descriptor,
                                void* state,
                                const StrategyParamStorage& params,
                                const detail::SessionData& session,
                                std::string& error) {
  if (!descriptor.resolveTradeHistory || !descriptor.seedTrades) return true;
  const TimeNs seedEnd = timeNsFromI64(session.tradeHistoryRequestedEndNs > 0 ? session.tradeHistoryRequestedEndNs : session.firstTsNs);
  hft_trader::runtime::StrategyTradeHistoryRequest request{};
  if (!descriptor.resolveTradeHistory(params, seedEnd, request)) {
    error = "strategy trade history request failed";
    return false;
  }
  if (!request.enabled) return true;
  if (session.tradeHistoryRequestedStartNs <= 0 || session.tradeHistoryRequestedEndNs <= 0) {
    error = "strategy requires trade warmup but recorder manifest has no trade history coverage";
    return false;
  }
  if (static_cast<std::uint64_t>(session.tradeHistoryRequestedStartNs) > request.startTime.raw) {
    error = "recorder trade warmup is shorter than strategy requires";
    return false;
  }
  std::vector<cxet::composite::TradeRuntimeV1> rows{};
  hft_trader::runtime::StrategyTradeHistorySeedStats stats{};
  if (!buildTradeWarmupRows(session, rows, stats)) {
    error = "strategy requires trade warmup but warmup trades are empty";
    return false;
  }
  const hft_trader::runtime::StrategySeedTradesStatus seedStatus = descriptor.seedTrades(state, rows.data(), rows.size(), stats);
  if (seedStatus == hft_trader::runtime::StrategySeedTradesStatus::BadData) {
    error = "strategy seedTrades rejected warmup data; calculated warmup metrics are not ready";
    return false;
  }
  return true;
}

std::int64_t nextPendingTs(const detail::EngineState& engine) noexcept {
  return engine.pending.empty()
      ? std::numeric_limits<std::int64_t>::max()
      : engine.pending.front().activeTsNs;
}

void applyBacktestReconstructedBbo(hft_trader::core::StrategyMarketSnapshot& snapshot,
                                   hft_trader::core::LocalOrderBookState& orderBook,
                                   const hft_trader::core::BboReconstructionState& bbo) noexcept {
  const hft_trader::core::BboReconstructionView view = hft_trader::core::reconstructedBbo(bbo);
  hft_trader::core::copyReconstructedBboSnapshot(snapshot, view);
  if (hft_trader::core::validReconstructedBbo(view) &&
      orderBook.initialized.raw == hft_trader::core::kAlgoFlagTrue.raw) {
    (void)hft_trader::core::applyBboOverlay(orderBook, view.bookTicker, view.bidQtyKnown, view.askQtyKnown);
    hft_trader::core::bindOrderBookSnapshot(snapshot, orderBook);
  }
}

std::int64_t nextWakeTs(const StrategyDescriptor& descriptor, const void* state) noexcept {
  if (!descriptor.nextWakeMono) return std::numeric_limits<std::int64_t>::max();
  const TimeNs wake = descriptor.nextWakeMono(state);
  if (wake.raw == 0u) return std::numeric_limits<std::int64_t>::max();
  return toInt64Time(wake);
}

std::int64_t portfolioEquityE8(const std::vector<detail::EngineState>& engines,
                               std::int64_t initialBalanceE8) noexcept {
  std::int64_t out = initialBalanceE8;
  for (const detail::EngineState& engine : engines) {
    out += engine.netRealizedPnlE8 + detail::unrealizedPnlE8(engine);
  }
  return out;
}

hft_trader::core::StrategyAccountState portfolioAccountStateFromEngines(
    const std::vector<detail::EngineState>& engines,
    std::int64_t initialBalanceE8) noexcept {
  hft_trader::core::StrategyAccountState out{};
  out.initialBalanceE8 = initialBalanceE8;
  out.walletBalanceE8 = initialBalanceE8;
  for (const detail::EngineState& engine : engines) {
    out.walletBalanceE8 += engine.netRealizedPnlE8;
  }
  out.availableBalanceE8 = out.walletBalanceE8;
  out.equityE8 = portfolioEquityE8(engines, initialBalanceE8);
  out.hasBalance = initialBalanceE8 != 0 || out.walletBalanceE8 != 0;
  return out;
}

void applyMarketEventToSnapshots(detail::EngineState& engine,
                                 const detail::SessionData& session,
                                 const detail::TimelineEvent& event,
                                 const Symbol& selectedSymbol,
                                 const StrategyInstrumentSpec& instrument,
                                 bool strategyVisible) {
  hft_trader::core::StrategyMarketSnapshot& snapshot = strategyVisible ? engine.snapshot : engine.venueSnapshot;
  hft_trader::core::LocalOrderBookState& orderBook = strategyVisible ? engine.orderBook : engine.venueOrderBook;
  hft_trader::core::BboReconstructionState& bbo = strategyVisible ? engine.bboReconstruction : engine.venueBboReconstruction;
  if (event.kind == detail::kTimelineKindDepth) {
    cxet::composite::OrderBookTapeRuntimeV1 tape{};
    cxet::composite::OrderBookTapeSidesRuntimeV1 sides{};
    detail::toRuntime(session.depths[event.index], tape, sides);
    hft_trader::core::copyDepthSnapshot(snapshot, tape, sides);
    if (orderBook.initialized.raw != hft_trader::core::kAlgoFlagTrue.raw) {
      hft_trader::core::LocalOrderBookConfig bookConfig{};
      bookConfig.maxStoredLevelsPerSide.raw = rawdata::kMaxOrderbookLevels;
      bookConfig.tickSizeRaw = instrument.tickSizeRaw;
      hft_trader::core::applySnapshot(orderBook, detail::toRuntimeSnapshot(session.depths[event.index]), selectedSymbol, bookConfig);
    } else {
      (void)hft_trader::core::applyTapeDelta(orderBook, tape, sides);
    }
    applyBacktestReconstructedBbo(snapshot, orderBook, bbo);
    hft_trader::core::bindOrderBookSnapshot(snapshot, orderBook);
  } else if (event.kind == detail::kTimelineKindTrade) {
    const auto trade = detail::toRuntime(session.trades[event.index]);
    hft_trader::core::copyTradeSnapshot(snapshot, trade);
    (void)hft_trader::core::applyTrade(bbo, trade);
    applyBacktestReconstructedBbo(snapshot, orderBook, bbo);
    if (!strategyVisible) detail::tryFillOnTrade(engine, trade);
  } else if (event.kind == detail::kTimelineKindBookTicker) {
    const auto ticker = detail::toRuntime(session.bookTickers[event.index]);
    hft_trader::core::copyBookTickerSnapshot(snapshot, ticker);
    (void)hft_trader::core::applyBookTicker(bbo, ticker);
    applyBacktestReconstructedBbo(snapshot, orderBook, bbo);
    hft_trader::core::bindOrderBookSnapshot(snapshot, orderBook);
    if (!strategyVisible) detail::tryFillOnTop(engine, event.tsNs);
  } else if (event.kind == detail::kTimelineKindPriceLimit) {
    const auto priceLimit = detail::toRuntime(session.priceLimits[event.index]);
    hft_trader::core::copyPriceLimitSnapshot(snapshot, priceLimit);
  } else if (event.kind == detail::kTimelineKindLiquidation) {
    const auto liquidation = detail::toRuntime(session.liquidations[event.index], engine.exchange, engine.symbol);
    hft_trader::core::copyLiquidationSnapshot(snapshot, liquidation);
  } else if (event.kind == detail::kTimelineKindMarkPrice) {
    const auto markPrice = detail::toRuntime(session.markPrices[event.index]);
    hft_trader::core::copyMarkPriceSnapshot(snapshot, markPrice);
  } else if (event.kind == detail::kTimelineKindIndexPrice) {
    const auto indexPrice = detail::toRuntime(session.indexPrices[event.index]);
    hft_trader::core::copyIndexPriceSnapshot(snapshot, indexPrice);
  } else if (event.kind == detail::kTimelineKindFunding) {
    const auto funding = detail::toRuntime(session.funding[event.index]);
    hft_trader::core::copyFundingSnapshot(snapshot, funding);
  } else if (event.kind == detail::kTimelineKindOpenInterest) {
    const auto openInterest = detail::toRuntime(session.openInterest[event.index]);
    hft_trader::core::copyOpenInterestSnapshot(snapshot, openInterest);
  }
}

void applyMarketEventToSnapshots(detail::EngineState& engine,
                                 const std::vector<detail::SessionData>& sessions,
                                 const detail::TimelineEvent& event,
                                 const Symbol& selectedSymbol,
                                 const StrategyInstrumentSpec& instrument,
                                 bool strategyVisible) {
  const std::size_t legIndex = event.legIndex;
  if (legIndex >= sessions.size()) return;
  applyMarketEventToSnapshots(engine, sessions[legIndex], event, selectedSymbol, instrument, strategyVisible);
}

void applyMarketEventToSnapshots(std::vector<detail::EngineState>& engines,
                                 const std::vector<detail::SessionData>& sessions,
                                 const detail::TimelineEvent& event,
                                 const std::vector<Symbol>& symbols,
                                 const std::vector<StrategyInstrumentSpec>& instruments,
                                 bool strategyVisible) {
  const std::size_t legIndex = event.legIndex;
  if (legIndex >= engines.size() || legIndex >= sessions.size() || legIndex >= symbols.size() || legIndex >= instruments.size()) return;
  applyMarketEventToSnapshots(engines[legIndex], sessions[legIndex], event, symbols[legIndex], instruments[legIndex], strategyVisible);
}

bool drainUserEvents(const StrategyDescriptor& descriptor,
                     void* state,
                     detail::EngineState& engine,
                     const hft_trader::core::SymbolShardConfig& shard,
                     const hft_trader::core::ShardControlState& control,
                     const BacktestRunRequest& request) {
  if (engine.liquidated || engine.riskStopped) return true;
  std::uint32_t guard = 0u;
  while (engine.deliveredUserEvents < engine.userEvents.size()) {
    if (++guard > 4096u) return false;
    const cxet::composite::UserDataEvent event = engine.userEvents[engine.deliveredUserEvents++];
    const std::int64_t tsNs = event.updateTime.raw != 0u ? toInt64Time(event.updateTime) : toInt64Time(event.eventTime);
    if (!runCycle(descriptor, state, engine, shard, control, tsNs, request, &event)) return false;
  }
  return true;
}

bool runUntil(const StrategyDescriptor& descriptor,
              void* state,
              detail::EngineState& engine,
              const hft_trader::core::SymbolShardConfig& shard,
              const hft_trader::core::ShardControlState& control,
              std::int64_t untilTs,
              const BacktestRunRequest& request) {
  if (engine.liquidated || engine.riskStopped) return true;
  std::uint32_t guard = 0u;
  while (true) {
    if (++guard > 8192u) return false;
    if (!drainUserEvents(descriptor, state, engine, shard, control, request)) return false;
    const std::int64_t pendingTs = nextPendingTs(engine);
    const std::int64_t wakeTs = nextWakeTs(descriptor, state);
    std::int64_t nextTs = pendingTs < wakeTs ? pendingTs : wakeTs;
    if (nextTs == std::numeric_limits<std::int64_t>::max() || nextTs > untilTs) break;
    if (pendingTs <= wakeTs) {
      detail::applyDueActions(engine, pendingTs);
    } else {
      ++engine.timerWakeEvents;
      if (!runCycle(descriptor, state, engine, shard, control, wakeTs, request, nullptr)) return false;
    }
  }
  return drainUserEvents(descriptor, state, engine, shard, control, request);
}

std::int64_t nextPendingTs(const std::vector<detail::EngineState>& engines) noexcept {
  std::int64_t out = std::numeric_limits<std::int64_t>::max();
  for (const detail::EngineState& engine : engines) {
    const std::int64_t candidate = nextPendingTs(engine);
    if (candidate < out) out = candidate;
  }
  return out;
}

bool drainUserEventsMulti(const StrategyDescriptor& descriptor,
                          void* state,
                          std::vector<detail::EngineState>& engines,
                          const hft_trader::core::SymbolShardConfig& shard,
                          const hft_trader::core::ShardControlState& control,
                          std::size_t primaryLegIndex,
  const BacktestRunRequest& request) {
  for (const detail::EngineState& engine : engines) {
    if (engine.liquidated || engine.riskStopped) return true;
  }
  std::uint32_t guard = 0u;
  bool progressed = true;
  while (progressed) {
    progressed = false;
    for (std::size_t leg = 0; leg < engines.size(); ++leg) {
      detail::EngineState& engine = engines[leg];
      if (engine.deliveredUserEvents >= engine.userEvents.size()) continue;
      if (++guard > 4096u) return false;
      const cxet::composite::UserDataEvent event = engine.userEvents[engine.deliveredUserEvents++];
      const std::int64_t tsNs = event.updateTime.raw != 0u ? toInt64Time(event.updateTime) : toInt64Time(event.eventTime);
      if (!runCycleMulti(descriptor, state, engines, shard, control, primaryLegIndex, static_cast<std::uint32_t>(leg), tsNs, request, &event)) return false;
      progressed = true;
    }
  }
  return true;
}

bool hasUndeliveredUserEvents(const std::vector<detail::EngineState>& engines) noexcept {
  for (const detail::EngineState& engine : engines) {
    if (engine.deliveredUserEvents < engine.userEvents.size()) return true;
  }
  return false;
}

bool runUntilMulti(const StrategyDescriptor& descriptor,
                   void* state,
                   std::vector<detail::EngineState>& engines,
                   const hft_trader::core::SymbolShardConfig& shard,
                   const hft_trader::core::ShardControlState& control,
                   std::size_t primaryLegIndex,
                   std::int64_t untilTs,
                   const BacktestRunRequest& request) {
  for (const detail::EngineState& engine : engines) {
    if (engine.liquidated || engine.riskStopped) return true;
  }
  const std::int64_t pendingTs = nextPendingTs(engines);
  const std::int64_t wakeTs = nextWakeTs(descriptor, state);
  const std::int64_t nextTs = pendingTs < wakeTs ? pendingTs : wakeTs;
  if (!hasUndeliveredUserEvents(engines) &&
      (nextTs == std::numeric_limits<std::int64_t>::max() || nextTs > untilTs)) {
    return true;
  }
  std::uint32_t guard = 0u;
  while (true) {
    if (++guard > 8192u) return false;
    if (!drainUserEventsMulti(descriptor, state, engines, shard, control, primaryLegIndex, request)) return false;
    const std::int64_t pendingTs = nextPendingTs(engines);
    const std::int64_t wakeTs = nextWakeTs(descriptor, state);
    std::int64_t nextTs = pendingTs < wakeTs ? pendingTs : wakeTs;
    if (nextTs == std::numeric_limits<std::int64_t>::max() || nextTs > untilTs) break;
    if (pendingTs <= wakeTs) {
      for (detail::EngineState& engine : engines) detail::applyDueActions(engine, pendingTs);
    } else {
      if (primaryLegIndex < engines.size()) ++engines[primaryLegIndex].timerWakeEvents;
      if (!runCycleMulti(descriptor, state, engines, shard, control, primaryLegIndex, static_cast<std::uint32_t>(primaryLegIndex), wakeTs, request, nullptr)) return false;
    }
  }
  return drainUserEventsMulti(descriptor, state, engines, shard, control, primaryLegIndex, request);
}

bool settleAfterReplay(const StrategyDescriptor& descriptor,
                       void* state,
                       detail::EngineState& engine,
                       const hft_trader::core::SymbolShardConfig& shard,
                       const hft_trader::core::ShardControlState& control,
                       std::int64_t lastTsNs,
                       const BacktestRunRequest& request) {
  std::int64_t untilTs = lastTsNs > 0 ? detail::delayedTs(lastTsNs, maxLatencyUs(request)) : lastTsNs;
  if (!runUntil(descriptor, state, engine, shard, control, untilTs, request)) return false;
  for (std::uint32_t i = 0u; i < 32u; ++i) {
    const std::int64_t wakeTs = nextWakeTs(descriptor, state);
    if (wakeTs == std::numeric_limits<std::int64_t>::max()) break;
    untilTs = wakeTs;
    if (!runUntil(descriptor, state, engine, shard, control, untilTs, request)) return false;
    untilTs = detail::delayedTs(untilTs, maxLatencyUs(request));
    if (!runUntil(descriptor, state, engine, shard, control, untilTs, request)) return false;
    if (engine.netQtyRaw == 0 && engine.pending.empty() && nextWakeTs(descriptor, state) == std::numeric_limits<std::int64_t>::max()) break;
  }
  hft_trader::core::ShardControlState teardownControl = control;
  teardownControl.killActive = hft_trader::core::kAlgoFlagTrue;
  for (std::uint32_t i = 0u; i < 8u && (engine.netQtyRaw != 0 || !engine.pending.empty()); ++i) {
    untilTs = detail::delayedTs(untilTs, maxLatencyUs(request));
    if (!runUntil(descriptor, state, engine, shard, teardownControl, untilTs, request)) return false;
    if (engine.netQtyRaw != 0) {
      if (!detail::forceClosePosition(engine, untilTs, engine.latency)) return false;
      untilTs = detail::delayedTs(untilTs, maxLatencyUs(request));
      if (!runUntil(descriptor, state, engine, shard, teardownControl, untilTs, request)) return false;
    }
    detail::cancelOpenOrders(engine, untilTs, engine.latency);
  }
  detail::cancelOpenOrders(engine, untilTs, engine.latency);
  untilTs = detail::delayedTs(untilTs, maxLatencyUs(request));
  if (!runUntil(descriptor, state, engine, shard, teardownControl, untilTs, request)) return false;
  return engine.netQtyRaw == 0 && engine.pending.empty();
}

bool portfolioHasOpenState(const std::vector<detail::EngineState>& engines) noexcept {
  for (const detail::EngineState& engine : engines) {
    if (engine.netQtyRaw != 0 || !engine.pending.empty()) return true;
  }
  return false;
}

bool settleAfterReplayMulti(const StrategyDescriptor& descriptor,
                            void* state,
                            std::vector<detail::EngineState>& engines,
                            const hft_trader::core::SymbolShardConfig& shard,
                            const hft_trader::core::ShardControlState& control,
                            std::size_t primaryLegIndex,
                            std::int64_t lastTsNs,
                            const BacktestRunRequest& request) {
  std::int64_t untilTs = lastTsNs > 0 ? detail::delayedTs(lastTsNs, maxLatencyUs(request)) : lastTsNs;
  if (!runUntilMulti(descriptor, state, engines, shard, control, primaryLegIndex, untilTs, request)) return false;
  for (std::uint32_t i = 0u; i < 32u; ++i) {
    const std::int64_t wakeTs = nextWakeTs(descriptor, state);
    if (wakeTs == std::numeric_limits<std::int64_t>::max()) break;
    untilTs = wakeTs;
    if (!runUntilMulti(descriptor, state, engines, shard, control, primaryLegIndex, untilTs, request)) return false;
    untilTs = detail::delayedTs(untilTs, maxLatencyUs(request));
    if (!runUntilMulti(descriptor, state, engines, shard, control, primaryLegIndex, untilTs, request)) return false;
    if (!portfolioHasOpenState(engines) && nextWakeTs(descriptor, state) == std::numeric_limits<std::int64_t>::max()) break;
  }
  hft_trader::core::ShardControlState teardownControl = control;
  teardownControl.killActive = hft_trader::core::kAlgoFlagTrue;
  for (std::uint32_t i = 0u; i < 8u && portfolioHasOpenState(engines); ++i) {
    untilTs = detail::delayedTs(untilTs, maxLatencyUs(request));
    if (!runUntilMulti(descriptor, state, engines, shard, teardownControl, primaryLegIndex, untilTs, request)) return false;
    for (detail::EngineState& engine : engines) {
      if (engine.netQtyRaw != 0 && !detail::forceClosePosition(engine, untilTs, engine.latency)) return false;
    }
    untilTs = detail::delayedTs(untilTs, maxLatencyUs(request));
    if (!runUntilMulti(descriptor, state, engines, shard, teardownControl, primaryLegIndex, untilTs, request)) return false;
    for (detail::EngineState& engine : engines) detail::cancelOpenOrders(engine, untilTs, engine.latency);
  }
  for (detail::EngineState& engine : engines) detail::cancelOpenOrders(engine, untilTs, engine.latency);
  untilTs = detail::delayedTs(untilTs, maxLatencyUs(request));
  if (!runUntilMulti(descriptor, state, engines, shard, teardownControl, primaryLegIndex, untilTs, request)) return false;
  return !portfolioHasOpenState(engines);
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
  result.totalPnlE8 = result.realizedPnlE8 + result.unrealizedPnlE8;
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
    result.grossRealizedPnlE8 += engine.grossRealizedPnlE8;
    result.feesPaidE8 += engine.feesPaidE8;
    result.netRealizedPnlE8 += engine.netRealizedPnlE8;
    result.unrealizedPnlE8 += detail::unrealizedPnlE8(engine);
    result.openPositionQtyE8 += engine.netQtyRaw;
    result.orders += engine.orders.size();
    result.fills += engine.fills.size();
    result.userEvents += engine.userEvents.size();
    result.timerWakeEvents += engine.timerWakeEvents;
    result.reduceOnlyOrders += engine.reduceOnlyOrders;
    result.reduceOnlyFills += engine.reduceOnlyFills;
    result.inputCoverage = addCoverage(result.inputCoverage, engine.inputCoverage);
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
  result.totalPnlE8 = result.realizedPnlE8 + result.unrealizedPnlE8;
  result.walletBalanceE8 = initialBalanceE8 + result.netRealizedPnlE8;
  if (liquidated && initialBalanceE8 > 0) {
    result.walletBalanceE8 = 0;
    result.grossRealizedPnlE8 = -initialBalanceE8 + result.feesPaidE8;
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
  result.totalPnlCurveE8.clear();
}

}  // namespace

BacktestRunResult runBacktest(const BacktestRunRequest& request,
                              BacktestProgressCallback progressCallback,
                              void* userData) noexcept {
  BacktestRunResult result{};
  result.runId = request.runId.empty() ? makeRunId() : request.runId;
  result.requestId = request.requestId.empty() ? result.runId : request.requestId;
  result.sessionPath = request.sessionPath;
  result.configPath = request.configPath;
  result.resultPath = resultPathFor(request, result.runId);
  BacktestRunRequest effectiveRequest = request;
  if (effectiveRequest.latencySeed == 0u) effectiveRequest.latencySeed = hashRunSeed(result.runId);
  const detail::BacktestLatencyConfig latencyConfig = detail::latencyConfigFromRequest(effectiveRequest);
  result.latencySeed = latencyConfig.seed;
  result.marketDataLatency = latencyConfig.marketData;
  result.marketOrderLatency = latencyConfig.marketOrder;
  result.limitOrderLatency = latencyConfig.limitOrder;
  result.orderLatencyUs = request.orderLatencyUs;
  result.amendLatencyUs = request.amendLatencyUs;
  result.cancelLatencyUs = request.cancelLatencyUs;
  result.initialBalanceE8 = request.initialBalanceE8;
  result.walletBalanceE8 = request.initialBalanceE8;
  auto writeArtifacts = [&](const detail::EngineState& engine) noexcept -> Status {
    if (!request.writeArtifacts) return Status::Ok;
    return detail::writeResult(result, engine);
  };
  auto writeEmptyArtifacts = [&]() noexcept -> Status {
    if (!request.writeArtifacts) return Status::Ok;
    try {
      const auto empty = std::make_unique<detail::EngineState>();
      return writeArtifacts(*empty);
    } catch (...) {
      return Status::IoError;
    }
  };

  const bool hasExplicitSessions = !request.sessions.empty();
  if ((!hasExplicitSessions && request.sessionPath.empty()) || request.configPath.empty()) {
    result.status = Status::InvalidArgument;
    result.error = "session_path and config_path are required";
    return result;
  }
  if (hasExplicitSessions && request.sessions.size() > hft_trader::core::kStrategyRuntimeMaxLegs) {
    result.status = Status::InvalidArgument;
    result.error = "too many backtest sessions";
    return result;
  }
  if (!request.outputPath.empty() && request.outputPath.extension() == ".json") {
    result.status = Status::InvalidArgument;
    result.error = "output_path must be a run directory, not a .json file";
    return result;
  }

  std::string error;
  hft_trader::runtime::HftRuntimeConfig cfg{};
  if (!hft_trader::runtime::loadRuntimeConfig(request.configPath.string().c_str(), cfg, error)) {
    result.status = Status::InvalidArgument;
    result.error = error;
    (void)writeEmptyArtifacts();
    return result;
  }
  std::size_t primaryLegIndex = 0u;
  if (hasExplicitSessions && cfg.portfolios.size() == 1u && cfg.portfolios[0].hasPrimary &&
      cfg.portfolios[0].primaryLegIndex < request.sessions.size()) {
    primaryLegIndex = cfg.portfolios[0].primaryLegIndex;
  }
  result.strategy = request.strategy.empty() ? cfg.strategyType : request.strategy;
  StrategyIndicatorProfileInfo indicatorProfile{};
  if (!resolveStrategyIndicatorProfile(request.indicatorProfile, indicatorProfile)) {
    result.status = Status::InvalidArgument;
    result.error = "unknown indicator profile";
    (void)writeEmptyArtifacts();
    return result;
  }
  result.indicatorProfile = std::string{indicatorProfile.id};
  if (!strategyDeclaresIndicator(result.strategy, result.indicatorProfile)) {
    result.status = Status::InvalidArgument;
    result.error = "indicator profile is not supported by strategy";
    (void)writeEmptyArtifacts();
    return result;
  }
  if (result.strategy != cfg.strategyType) {
    result.status = Status::ConfigMismatch;
    result.error = "requested strategy does not match config [strategy] type";
    (void)writeEmptyArtifacts();
    return result;
  }
  const StrategyDescriptor* descriptor = hft_trader::runtime::findStrategyDescriptor(cfg.strategyType);
  if (!descriptor || !descriptor->resolveParams || !descriptor->initState || !descriptor->runCycle) {
    result.status = Status::Unsupported;
    result.error = "strategy descriptor is missing required callbacks";
    (void)writeEmptyArtifacts();
    return result;
  }

  detail::SessionLoadPlan loadPlan{};
  if (!buildSessionLoadPlan(*descriptor, loadPlan, error)) {
    result.status = Status::Unsupported;
    result.error = error;
    (void)writeEmptyArtifacts();
    return result;
  }

  detail::PreparedBacktestInput* preparedInput = detail::activePreparedBacktestInput();
  const bool usePreparedSessions = preparedInput != nullptr &&
                                   preparedMatches(*preparedInput, request, hasExplicitSessions, loadPlan);
  std::vector<detail::SessionData> loadedSessions{};
  const std::vector<detail::SessionData>* sessionsView = nullptr;
  if (usePreparedSessions) {
    sessionsView = &preparedInput->sessions;
    result.status = Status::Ok;
  } else if (hasExplicitSessions) {
    loadedSessions.resize(request.sessions.size());
    for (std::size_t i = 0; i < request.sessions.size(); ++i) {
      result.status = detail::loadSession(request.sessions[i].path, loadPlan, loadedSessions[i], error);
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
  if (!isOk(result.status)) {
    result.error = error;
    (void)writeEmptyArtifacts();
    return result;
  }
  if (sessionsView == nullptr || sessionsView->empty()) {
    result.status = Status::CorruptData;
    result.error = "backtest session cache is empty";
    (void)writeEmptyArtifacts();
    return result;
  }
  const std::vector<detail::SessionData>& sessions = *sessionsView;
  const detail::SessionData& session = sessions[0];
  result.sessionPath = session.sessionPath;

  const ExchangeId sessionExchange = detail::exchangeFromText(session.exchange);
  const canon::MarketType sessionMarket = detail::marketFromText(session.market);
  const hft_trader::runtime::VenueRuntimeConfig* venue = nullptr;
  Symbol selectedSymbol{};
  bool symbolMatched = false;
  for (const auto& candidate : cfg.venues) {
    if (candidate.exchange.raw != sessionExchange.raw || candidate.market.raw != sessionMarket.raw) continue;
    for (const Symbol& symbol : candidate.symbols) {
      if (session.symbol != symbol.data) continue;
      venue = &candidate;
      selectedSymbol = symbol;
      symbolMatched = true;
      break;
    }
    if (symbolMatched) break;
  }
  if (!venue) {
    result.status = Status::ConfigMismatch;
    result.error = "config venue/symbol does not match recorder session";
    (void)writeEmptyArtifacts();
    return result;
  }

  StrategyParamStorage params{};
  if (!descriptor->resolveParams(cfg, *venue, params, error)) {
    result.status = Status::InvalidArgument;
    result.error = error;
    (void)writeEmptyArtifacts();
    return result;
  }
  StrategyInstrumentSpec instrument{};
  instrument.tickSizeRaw = session.instrument.tickSizeE8;
  instrument.stepSizeRaw = session.instrument.lotSizeE8;
  instrument.contractBaseQtyRaw = session.instrument.contractBaseQtyE8;
  instrument.tickGrid = hft_trader::core::prepareFastStepGrid(instrument.tickSizeRaw);
  instrument.qtyStepGrid = hft_trader::core::prepareFastStepGrid(instrument.stepSizeRaw);
  instrument.contractBaseQtyGrid = hft_trader::core::prepareFastStepGrid(instrument.contractBaseQtyRaw);
  std::vector<StrategyInstrumentSpec> legInstruments;
  std::vector<Symbol> legSymbols;
  if (hasExplicitSessions) {
    legInstruments.resize(sessions.size());
    legSymbols.resize(sessions.size());
    for (std::size_t leg = 0; leg < sessions.size(); ++leg) {
      legInstruments[leg].tickSizeRaw = sessions[leg].instrument.tickSizeE8;
      legInstruments[leg].stepSizeRaw = sessions[leg].instrument.lotSizeE8;
      legInstruments[leg].contractBaseQtyRaw = sessions[leg].instrument.contractBaseQtyE8;
      legInstruments[leg].tickGrid = hft_trader::core::prepareFastStepGrid(legInstruments[leg].tickSizeRaw);
      legInstruments[leg].qtyStepGrid = hft_trader::core::prepareFastStepGrid(legInstruments[leg].stepSizeRaw);
      legInstruments[leg].contractBaseQtyGrid = hft_trader::core::prepareFastStepGrid(legInstruments[leg].contractBaseQtyRaw);
      legSymbols[leg].copyFrom(sessions[leg].symbol.c_str());
    }
  }

  alignas(hft_trader::runtime::kStrategyRuntimeStateAlign) std::array<unsigned char, hft_trader::runtime::kStrategyRuntimeStateBytes> state{};
  if (!descriptor->initState(state.data(), params, instrument)) {
    result.status = Status::InvalidArgument;
    result.error = "strategy initState failed";
    (void)writeEmptyArtifacts();
    return result;
  }
  BacktestInputCoverage seedCoverage{};
  if (descriptor->seedHistory) {
    if (session.candles.empty()) {
      result.status = Status::CorruptData;
      result.error = "strategy requires candle history but candles channel is missing or empty";
      (void)writeEmptyArtifacts();
      return result;
    }
    cxet::composite::TieredCandleHistory history{};
    if (!detail::buildTieredCandleHistory(session.candles, history)) {
      result.status = Status::CorruptData;
      result.error = "failed to build strategy candle history from recorder session";
      (void)writeEmptyArtifacts();
      return result;
    }
    seedCoverage.seedM1Candles = history.m1Count.raw;
    seedCoverage.seedM15Candles = history.m15Count.raw;
    seedCoverage.seedD1Candles = history.d1Count.raw;
    hft_trader::runtime::StrategyHistorySeedStats stats{};
    if (!descriptor->seedHistory(state.data(), history, &stats)) {
      result.status = Status::InvalidArgument;
      result.error = "strategy seedHistory failed";
      (void)writeEmptyArtifacts();
      return result;
    }
    if (isBacktestProbe(*descriptor) &&
        (seedCoverage.seedM1Candles == 0u || seedCoverage.seedM15Candles == 0u || seedCoverage.seedD1Candles == 0u)) {
      result.status = Status::CorruptData;
      result.error = "backtest_probe did not receive full m1/m15/d1 candle seed";
      (void)writeEmptyArtifacts();
      return result;
    }
  }
  if (!seedTradeWarmupFromSession(*descriptor, state.data(), params, session, error)) {
    result.status = Status::CorruptData;
    result.error = error;
    (void)writeEmptyArtifacts();
    return result;
  }
  seedCoverage.seedTradeRows = session.tradeWarmup.size();

  std::unique_ptr<detail::EngineState> engineStorage;
  try {
    engineStorage = std::make_unique<detail::EngineState>();
  } catch (...) {
    result.status = Status::IoError;
    result.error = "failed to allocate backtest engine state";
    (void)writeEmptyArtifacts();
    return result;
  }
  detail::EngineState& engine = *engineStorage;
  configureStrategyIndicator(engine, indicatorProfile);
  detail::initLatencyRuntime(engine.latency, resolveLatencyConfig(effectiveRequest, session.exchange, session.market));
  engine.inputCoverage = seedCoverage;
  engine.exchange = sessionExchange;
  engine.market = sessionMarket;
  engine.symbol = selectedSymbol;
  detail::initializeAccounting(engine, request);
  const BacktestFeeSchedule primaryFee = resolveFeeSchedule(request, session.exchange, session.market);
  engine.makerFeeBpsE8 = primaryFee.makerFeeBpsE8;
  engine.takerFeeBpsE8 = primaryFee.takerFeeBpsE8;
  std::vector<detail::EngineState> legEngines;
  if (hasExplicitSessions) {
    legEngines.resize(sessions.size());
    for (std::size_t leg = 0; leg < sessions.size(); ++leg) {
      detail::EngineState& legEngine = legEngines[leg];
      configureStrategyIndicator(legEngine, indicatorProfile);
      const detail::BacktestLatencyConfig legLatency =
          latencyConfigForLeg(effectiveRequest, sessions[leg].exchange, sessions[leg].market, leg);
      detail::initLatencyRuntime(legEngine.latency, legLatency);
      legEngine.inputCoverage = seedCoverage;
      legEngine.legIndex = static_cast<std::uint32_t>(leg);
      legEngine.exchange = detail::exchangeFromText(sessions[leg].exchange);
      legEngine.market = detail::marketFromText(sessions[leg].market);
      legEngine.symbol.copyFrom(sessions[leg].symbol.c_str());
      detail::initializeAccounting(legEngine, request);
      legEngine.initialBalanceE8 = 0;
      legEngine.walletBalanceE8 = 0;
      const BacktestFeeSchedule fee = resolveFeeSchedule(request, sessions[leg].exchange, sessions[leg].market);
      legEngine.makerFeeBpsE8 = fee.makerFeeBpsE8;
      legEngine.takerFeeBpsE8 = fee.takerFeeBpsE8;
    }
    if (primaryLegIndex < legEngines.size()) engine = legEngines[primaryLegIndex];
  }
  if (session.hasSnapshot) {
    hft_trader::core::LocalOrderBookConfig bookConfig{};
    bookConfig.maxStoredLevelsPerSide.raw = rawdata::kMaxOrderbookLevels;
    bookConfig.tickSizeRaw = instrument.tickSizeRaw;
    hft_trader::core::applySnapshot(engine.orderBook, detail::toRuntimeSnapshot(session.snapshot), selectedSymbol, bookConfig);
    hft_trader::core::applySnapshot(engine.venueOrderBook, detail::toRuntimeSnapshot(session.snapshot), selectedSymbol, bookConfig);
    hft_trader::core::bindOrderBookSnapshot(engine.snapshot, engine.orderBook);
    hft_trader::core::bindOrderBookSnapshot(engine.venueSnapshot, engine.venueOrderBook);
  }
  if (hasExplicitSessions) {
    for (std::size_t leg = 0; leg < sessions.size() && leg < legEngines.size(); ++leg) {
      if (!sessions[leg].hasSnapshot) continue;
      hft_trader::core::LocalOrderBookConfig bookConfig{};
      bookConfig.maxStoredLevelsPerSide.raw = rawdata::kMaxOrderbookLevels;
      bookConfig.tickSizeRaw = sessions[leg].instrument.tickSizeE8;
      Symbol legSymbol{};
      legSymbol.copyFrom(sessions[leg].symbol.c_str());
      hft_trader::core::applySnapshot(legEngines[leg].orderBook, detail::toRuntimeSnapshot(sessions[leg].snapshot), legSymbol, bookConfig);
      hft_trader::core::applySnapshot(legEngines[leg].venueOrderBook, detail::toRuntimeSnapshot(sessions[leg].snapshot), legSymbol, bookConfig);
      hft_trader::core::bindOrderBookSnapshot(legEngines[leg].snapshot, legEngines[leg].orderBook);
      hft_trader::core::bindOrderBookSnapshot(legEngines[leg].venueSnapshot, legEngines[leg].venueOrderBook);
    }
    if (primaryLegIndex < legEngines.size()) engine = legEngines[primaryLegIndex];
  }
  hft_trader::core::SymbolShardConfig shard{};
  shard.shardId.raw = 1;
  shard.strategyId.raw = 1;
  shard.symbolId.raw = 1;
  shard.symbol = selectedSymbol;
  hft_trader::core::ShardControlState control{};
  control.shardId = shard.shardId;

  std::vector<detail::TimelineEvent> loadedTimeline;
  const std::vector<detail::TimelineEvent>* timelineView = nullptr;
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
  const std::vector<detail::TimelineEvent>& timeline = *timelineView;
  std::vector<detail::TimelineEvent> loadedDeliveryTimeline;
  const std::vector<detail::TimelineEvent>* deliveryTimelineView = nullptr;
  if (!hasMarketDataLatency(effectiveRequest, latencyConfig)) {
    deliveryTimelineView = &timeline;
  } else {
    loadedDeliveryTimeline = timeline;
    for (detail::TimelineEvent& event : loadedDeliveryTimeline) {
      if (hasExplicitSessions && event.legIndex < legEngines.size()) {
        event.deliveryTsNs = detail::delayedTs(
            event.tsNs,
            detail::sampleLatencyUs(legEngines[event.legIndex].latency, detail::BacktestLatencyChannel::MarketData));
      } else {
        event.deliveryTsNs = detail::delayedTs(
            event.tsNs,
            detail::sampleLatencyUs(engine.latency, detail::BacktestLatencyChannel::MarketData));
      }
    }
    if (!marketDataLatencyKeepsOrder(effectiveRequest, latencyConfig)) {
      std::stable_sort(loadedDeliveryTimeline.begin(), loadedDeliveryTimeline.end(), [](const detail::TimelineEvent& lhs, const detail::TimelineEvent& rhs) noexcept {
        if (lhs.deliveryTsNs != rhs.deliveryTsNs) return lhs.deliveryTsNs < rhs.deliveryTsNs;
        if (lhs.tsNs != rhs.tsNs) return lhs.tsNs < rhs.tsNs;
        if (lhs.ingestSeq != rhs.ingestSeq) return lhs.ingestSeq < rhs.ingestSeq;
        return lhs.kind < rhs.kind;
      });
    }
    deliveryTimelineView = &loadedDeliveryTimeline;
  }
  const std::vector<detail::TimelineEvent>& deliveryTimeline = *deliveryTimelineView;
  result.trades = 0;
  result.bookTickers = 0;
  result.priceLimits = 0;
  result.depths = 0;
  result.liquidations = 0;
  result.markPrices = 0;
  result.indexPrices = 0;
  result.funding = 0;
  result.openInterest = 0;
  result.firstTsNs = std::numeric_limits<std::int64_t>::max();
  result.lastTsNs = 0;
  for (const detail::SessionData& loadedSession : sessions) {
    result.trades += loadedSession.trades.size();
    result.bookTickers += loadedSession.bookTickers.size();
    result.priceLimits += loadedSession.priceLimits.size();
    result.depths += loadedSession.depths.size();
    result.liquidations += loadedSession.liquidations.size();
    result.markPrices += loadedSession.markPrices.size();
    result.indexPrices += loadedSession.indexPrices.size();
    result.funding += loadedSession.funding.size();
    result.openInterest += loadedSession.openInterest.size();
    if (loadedSession.firstTsNs > 0 && loadedSession.firstTsNs < result.firstTsNs) result.firstTsNs = loadedSession.firstTsNs;
    if (loadedSession.lastTsNs > result.lastTsNs) result.lastTsNs = loadedSession.lastTsNs;
  }
  if (result.firstTsNs == std::numeric_limits<std::int64_t>::max()) result.firstTsNs = session.firstTsNs;
  result.events = timeline.size();
  const bool captureProbeTrace = isBacktestProbe(*descriptor);
  const bool captureIndicatorTrace = !result.indicatorProfile.empty();
  const bool captureRangeTrace = isRangeReversion(*descriptor);
  const bool captureSpreadTrace = isStatArbBestside(*descriptor);
  if (hasExplicitSessions) {
    for (detail::EngineState& legEngine : legEngines) {
      reserveEngineStorage(legEngine, timeline.size(), captureProbeTrace, captureIndicatorTrace, captureRangeTrace, captureSpreadTrace);
    }
  } else {
    reserveEngineStorage(engine, timeline.size(), captureProbeTrace, captureIndicatorTrace, captureRangeTrace, captureSpreadTrace);
  }
  if (hasExplicitSessions) {
    for (detail::EngineState& legEngine : legEngines) {
      detail::appendEquityPoint(legEngine, result.firstTsNs);
      detail::appendRiskSnapshot(legEngine, result.firstTsNs, detail::kRiskSnapshotReasonStart);
    }
  } else {
    detail::appendEquityPoint(engine, result.firstTsNs);
    detail::appendRiskSnapshot(engine, result.firstTsNs, detail::kRiskSnapshotReasonStart);
  }
  hft_trader::core::StrategyAccountState riskAccount = hasExplicitSessions
      ? portfolioAccountStateFromEngines(legEngines, request.initialBalanceE8)
      : accountStateFromEngine(engine);
  hft_trader::runtime::PortfolioRiskRuntimeState traderRiskState =
      hft_trader::runtime::makePortfolioRiskRuntimeState(cfg.risk, &riskAccount, 1u);
  auto writeCurrentArtifacts = [&]() noexcept -> Status {
    if (hasExplicitSessions) {
      const std::int64_t tsNs = result.lastTsNs > 0 ? result.lastTsNs : result.firstTsNs;
      populatePortfolioResultPnl(result, sessions, legEngines, request.initialBalanceE8);
      try {
        auto aggregate = std::make_unique<detail::EngineState>();
        populateAggregateArtifactEngine(legEngines, request.initialBalanceE8, tsNs, *aggregate);
        return writeArtifacts(*aggregate);
      } catch (...) {
        return Status::IoError;
      }
    }
    populateResultPnl(result, engine);
    result.orders = engine.orders.size();
    result.fills = engine.fills.size();
    return writeArtifacts(engine);
  };
  auto tripHardLiquidation = [&](std::int64_t tsNs) noexcept -> bool {
    if (request.initialBalanceE8 <= 0) return false;
    if (hasExplicitSessions) {
      for (const detail::EngineState& legEngine : legEngines) {
        if (legEngine.liquidated) return true;
      }
      const std::int64_t equity = portfolioEquityE8(legEngines, request.initialBalanceE8);
      if (equity > 0) return false;
      for (detail::EngineState& legEngine : legEngines) detail::hardLiquidate(legEngine, tsNs, equity);
      control.killActive = hft_trader::core::kAlgoFlagTrue;
      control.haltReason = hft_trader::core::kHaltReasonManualKill;
      result.liquidated = true;
      result.liquidationTsNs = tsNs;
      result.liquidationEquityE8 = equity;
      return true;
    }
    if (engine.liquidated) return true;
    const std::int64_t equity = detail::equityE8(engine);
    if (equity > 0) return false;
    detail::hardLiquidate(engine, tsNs, equity);
    control.killActive = hft_trader::core::kAlgoFlagTrue;
    control.haltReason = hft_trader::core::kHaltReasonManualKill;
    result.liquidated = true;
    result.liquidationTsNs = tsNs;
    result.liquidationEquityE8 = equity;
    return true;
  };
  auto tripTraderRiskStop = [&](std::int64_t tsNs) noexcept -> bool {
    if (hasExplicitSessions) {
      for (const detail::EngineState& legEngine : legEngines) {
        if (legEngine.liquidated) return false;
        if (legEngine.riskStopped) return true;
      }
      riskAccount = portfolioAccountStateFromEngines(legEngines, request.initialBalanceE8);
    } else {
      if (engine.liquidated) return false;
      if (engine.riskStopped) return true;
      riskAccount = accountStateFromEngine(engine);
    }
    const hft_trader::runtime::PortfolioRiskEvaluation eval =
        hft_trader::runtime::evaluatePortfolioRisk(traderRiskState, &riskAccount, 1u, timeNsFromI64(tsNs));
    if (!eval.breached) return false;
    if (hasExplicitSessions) {
      for (detail::EngineState& legEngine : legEngines) {
        detail::stopForTraderRisk(legEngine, tsNs, eval.equityE8, eval.thresholdEquityE8);
      }
    } else {
      detail::stopForTraderRisk(engine, tsNs, eval.equityE8, eval.thresholdEquityE8);
    }
    hft_trader::runtime::applyPortfolioRiskToControl(traderRiskState, control);
    result.riskStopped = true;
    result.riskStopTsNs = tsNs;
    result.riskStopEquityE8 = eval.equityE8;
    result.riskStopThresholdEquityE8 = eval.thresholdEquityE8;
    return true;
  };

  if (!reportProgress(0, timeline.size(), "running", progressCallback, userData)) {
    result.status = Status::Cancelled;
    result.error = "cancelled";
    (void)writeCurrentArtifacts();
    return result;
  }
  std::size_t deliveryCursor = 0u;
  bool replayStopped = false;
  for (std::size_t i = 0; i < timeline.size(); ++i) {
    if (i + 1u < timeline.size()) prefetchRead(&timeline[i + 1u]);
    if (deliveryCursor < deliveryTimeline.size()) prefetchRead(&deliveryTimeline[deliveryCursor]);
    const detail::TimelineEvent& event = timeline[i];
    const bool beforeOk = hasExplicitSessions
        ? runUntilMulti(*descriptor, state.data(), legEngines, shard, control, primaryLegIndex, event.tsNs, request)
        : runUntil(*descriptor, state.data(), engine, shard, control, event.tsNs, request);
    if (!beforeOk) {
      result.status = Status::InvalidArgument;
      result.error = "strategy event loop did not settle before market event";
      (void)writeCurrentArtifacts();
      return result;
    }
    if (tripHardLiquidation(event.tsNs) || tripTraderRiskStop(event.tsNs)) {
      replayStopped = true;
      break;
    }
    while (deliveryCursor < deliveryTimeline.size() && deliveryTimeline[deliveryCursor].deliveryTsNs < event.tsNs) {
      if (deliveryCursor + 1u < deliveryTimeline.size()) prefetchRead(&deliveryTimeline[deliveryCursor + 1u]);
      const detail::TimelineEvent& delivered = deliveryTimeline[deliveryCursor++];
      if (hasExplicitSessions) applyMarketEventToSnapshots(legEngines, sessions, delivered, legSymbols, legInstruments, true);
      else applyMarketEventToSnapshots(engine, session, delivered, selectedSymbol, instrument, true);
      const bool deliveredOk = hasExplicitSessions
          ? runCycleMulti(*descriptor, state.data(), legEngines, shard, control, primaryLegIndex, delivered.legIndex, delivered.deliveryTsNs, request, nullptr)
          : runCycle(*descriptor, state.data(), engine, shard, control, delivered.deliveryTsNs, request, nullptr);
      if (!deliveredOk) {
        result.status = Status::InvalidArgument;
        result.error = "strategy runCycle failed on delivered market event";
        (void)writeCurrentArtifacts();
        return result;
      }
      if (hasExplicitSessions) {
        for (detail::EngineState& legEngine : legEngines) (void)detail::appendRiskSnapshotIfDue(legEngine, delivered.deliveryTsNs);
      } else {
        (void)detail::appendRiskSnapshotIfDue(engine, delivered.deliveryTsNs);
      }
      if (tripHardLiquidation(delivered.deliveryTsNs) || tripTraderRiskStop(delivered.deliveryTsNs)) {
        replayStopped = true;
        break;
      }
    }
    if (replayStopped) break;
    if (hasExplicitSessions) applyMarketEventToSnapshots(legEngines, sessions, event, legSymbols, legInstruments, false);
    else applyMarketEventToSnapshots(engine, session, event, selectedSymbol, instrument, false);
    if (hasExplicitSessions) {
      for (detail::EngineState& legEngine : legEngines) (void)detail::appendRiskSnapshotIfDue(legEngine, event.tsNs);
    } else {
      (void)detail::appendRiskSnapshotIfDue(engine, event.tsNs);
    }
    if (tripHardLiquidation(event.tsNs) || tripTraderRiskStop(event.tsNs)) {
      replayStopped = true;
      break;
    }
    while (deliveryCursor < deliveryTimeline.size() && deliveryTimeline[deliveryCursor].deliveryTsNs <= event.tsNs) {
      if (deliveryCursor + 1u < deliveryTimeline.size()) prefetchRead(&deliveryTimeline[deliveryCursor + 1u]);
      const detail::TimelineEvent& delivered = deliveryTimeline[deliveryCursor++];
      if (hasExplicitSessions) applyMarketEventToSnapshots(legEngines, sessions, delivered, legSymbols, legInstruments, true);
      else applyMarketEventToSnapshots(engine, session, delivered, selectedSymbol, instrument, true);
      const bool deliveredOk = hasExplicitSessions
          ? runCycleMulti(*descriptor, state.data(), legEngines, shard, control, primaryLegIndex, delivered.legIndex, delivered.deliveryTsNs, request, nullptr)
          : runCycle(*descriptor, state.data(), engine, shard, control, delivered.deliveryTsNs, request, nullptr);
      if (!deliveredOk) {
        result.status = Status::InvalidArgument;
        result.error = "strategy runCycle failed on delivered market event";
        (void)writeCurrentArtifacts();
        return result;
      }
      if (hasExplicitSessions) {
        for (detail::EngineState& legEngine : legEngines) (void)detail::appendRiskSnapshotIfDue(legEngine, delivered.deliveryTsNs);
      } else {
        (void)detail::appendRiskSnapshotIfDue(engine, delivered.deliveryTsNs);
      }
      if (tripHardLiquidation(delivered.deliveryTsNs) || tripTraderRiskStop(delivered.deliveryTsNs)) {
        replayStopped = true;
        break;
      }
    }
    if (replayStopped) break;
    const bool afterOk = hasExplicitSessions
        ? runUntilMulti(*descriptor, state.data(), legEngines, shard, control, primaryLegIndex, event.tsNs, request)
        : runUntil(*descriptor, state.data(), engine, shard, control, event.tsNs, request);
    if (!afterOk) {
      result.status = Status::InvalidArgument;
      result.error = "strategy event loop did not settle after market event";
      (void)writeCurrentArtifacts();
      return result;
    }
    if (tripHardLiquidation(event.tsNs) || tripTraderRiskStop(event.tsNs)) {
      replayStopped = true;
      break;
    }
    if ((i % 1024u) == 0u && !reportProgress(i, timeline.size(), "running", progressCallback, userData)) {
      result.status = Status::Cancelled;
      result.error = "cancelled";
      (void)writeCurrentArtifacts();
      return result;
    }
  }
  while (!replayStopped && deliveryCursor < deliveryTimeline.size()) {
    if (deliveryCursor + 1u < deliveryTimeline.size()) prefetchRead(&deliveryTimeline[deliveryCursor + 1u]);
    const detail::TimelineEvent& delivered = deliveryTimeline[deliveryCursor++];
    const bool beforeDelayedOk = hasExplicitSessions
        ? runUntilMulti(*descriptor, state.data(), legEngines, shard, control, primaryLegIndex, delivered.deliveryTsNs, request)
        : runUntil(*descriptor, state.data(), engine, shard, control, delivered.deliveryTsNs, request);
    if (!beforeDelayedOk) {
      result.status = Status::InvalidArgument;
      result.error = "strategy event loop did not settle before delayed market event";
      (void)writeCurrentArtifacts();
      return result;
    }
    if (hasExplicitSessions) applyMarketEventToSnapshots(legEngines, sessions, delivered, legSymbols, legInstruments, true);
    else applyMarketEventToSnapshots(engine, session, delivered, selectedSymbol, instrument, true);
    const bool delayedOk = hasExplicitSessions
        ? runCycleMulti(*descriptor, state.data(), legEngines, shard, control, primaryLegIndex, delivered.legIndex, delivered.deliveryTsNs, request, nullptr)
        : runCycle(*descriptor, state.data(), engine, shard, control, delivered.deliveryTsNs, request, nullptr);
    if (!delayedOk) {
      result.status = Status::InvalidArgument;
      result.error = "strategy runCycle failed on delayed market event";
      (void)writeCurrentArtifacts();
      return result;
    }
    if (hasExplicitSessions) {
      for (detail::EngineState& legEngine : legEngines) (void)detail::appendRiskSnapshotIfDue(legEngine, delivered.deliveryTsNs);
    } else {
      (void)detail::appendRiskSnapshotIfDue(engine, delivered.deliveryTsNs);
    }
    if (tripHardLiquidation(delivered.deliveryTsNs) || tripTraderRiskStop(delivered.deliveryTsNs)) {
      replayStopped = true;
      break;
    }
  }
  if (!replayStopped) {
    const bool settled = hasExplicitSessions
        ? settleAfterReplayMulti(*descriptor, state.data(), legEngines, shard, control, primaryLegIndex, result.lastTsNs, effectiveRequest)
        : settleAfterReplay(*descriptor, state.data(), engine, shard, control, result.lastTsNs, effectiveRequest);
    if (!settled) {
      result.status = Status::InvalidArgument;
      result.error = "strategy event loop did not settle after replay";
      (void)writeCurrentArtifacts();
      return result;
    }
  }
  if (!replayStopped) {
    const std::int64_t stopCheckTsNs = result.lastTsNs > 0 ? result.lastTsNs : result.firstTsNs;
    (void)(tripHardLiquidation(stopCheckTsNs) || tripTraderRiskStop(stopCheckTsNs));
  }
  const std::int64_t finalTsNs = result.lastTsNs > 0 ? result.lastTsNs + 1 : result.lastTsNs;
  if (hasExplicitSessions) {
    for (detail::EngineState& legEngine : legEngines) {
      detail::appendEquityPoint(legEngine, finalTsNs);
      detail::appendRiskSnapshot(legEngine, finalTsNs, detail::kRiskSnapshotReasonFinal);
    }
    populatePortfolioResultPnl(result, sessions, legEngines, request.initialBalanceE8);
    try {
      auto aggregate = std::make_unique<detail::EngineState>();
      populateAggregateArtifactEngine(legEngines, request.initialBalanceE8, finalTsNs, *aggregate);
      engine = std::move(*aggregate);
    } catch (...) {
      result.status = Status::IoError;
      result.error = "failed to allocate aggregate backtest engine state";
      (void)writeEmptyArtifacts();
      return result;
    }
  } else {
    detail::appendEquityPoint(engine, finalTsNs);
    detail::appendRiskSnapshot(engine, finalTsNs, detail::kRiskSnapshotReasonFinal);
    populateResultPnl(result, engine);
    result.orders = engine.orders.size();
    result.fills = engine.fills.size();
  }
  if (result.strategy == "backtest_probe" && !validateBacktestProbeCoverage(result, engine, result.error)) {
    result.status = Status::CorruptData;
    (void)writeArtifacts(engine);
    return result;
  }
  result.status = Status::Ok;
  Status writeStatus = writeArtifacts(engine);
  if (!isOk(writeStatus)) result.status = writeStatus;
  (void)reportProgress(timeline.size(), timeline.size(), "complete", progressCallback, userData);
  return result;
}

}  // namespace hft_backtest
