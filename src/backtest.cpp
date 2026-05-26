#include "hft_backtest/backtest.hpp"

#include "backtest_engine.hpp"
#include "session_loader.hpp"

#include "hft_trader/core/ControlState.hpp"
#include "hft_trader/core/LocalOrderBook.hpp"
#include "hft_trader/core/StrategyContract.hpp"
#include "hft_trader/runtime/GeneratedStrategyRegistry.hpp"
#include "hft_trader/runtime/RuntimeConfig.hpp"
#include "hft_trader/runtime/StrategyMarketDataRuntime.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace hft_backtest {
namespace {

using hft_trader::runtime::StrategyDescriptor;
using hft_trader::runtime::StrategyInstrumentSpec;
using hft_trader::runtime::StrategyParamStorage;

std::string makeRunId() {
  const auto nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  return "run-" + std::to_string(nowNs);
}

std::filesystem::path resultPathFor(const BacktestRunRequest& request, const std::string& runId) {
  if (!request.outputPath.empty()) return request.outputPath;
  return request.sessionPath / "backtests" / runId;
}

bool buildSessionLoadPlan(const StrategyDescriptor& descriptor, detail::SessionLoadPlan& out, std::string& error) noexcept {
  out = detail::SessionLoadPlan{};
  out.loadTrades = false;
  out.loadBookTickers = false;
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
    } else if (stream == cxet::api::market::PublicMarketDataStream::Orderbook) {
      out.loadDepths = true;
      out.requireDepths = true;
    } else {
      error = "strategy requires unsupported market data stream for backtest";
      return false;
    }
  }
  return true;
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
    if (!hft_trader::runtime::snapshotHasRequiredStream(snapshot, stream, &descriptor)) return false;
  }
  return true;
}

bool isBacktestProbe(const StrategyDescriptor& descriptor) noexcept {
  return descriptor.name && std::string_view{descriptor.name} == "backtest_probe";
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
  }
  row.eventCode = indicatorEventCode(trace);
  row.reasonRaw = trace.reasonRaw;
  row.decisionKind = trace.decisionKind;
  row.sideRaw = trace.sideRaw;
  engine.strategyIndicator.push_back(row);
}
void recordInputCoverage(detail::EngineState& engine) noexcept {
  ++engine.inputCoverage.strategyCycles;
  if (engine.snapshot.hasTrade.raw == hft_trader::core::kAlgoFlagTrue.raw) ++engine.inputCoverage.cyclesWithTrade;
  if (engine.snapshot.hasBookTicker.raw == hft_trader::core::kAlgoFlagTrue.raw) ++engine.inputCoverage.cyclesWithBookTicker;
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
  if (!strategySnapshotReady(descriptor, engine.snapshot)) return true;
  std::array<hft_trader::core::OrderIntent, detail::kIntentCapacity> orderBuf{};
  std::array<hft_trader::core::AmendIntent, detail::kIntentCapacity> amendBuf{};
  std::array<hft_trader::core::CancelIntent, detail::kIntentCapacity> cancelBuf{};
  std::vector<hft_trader::core::StrategyOrderState> orderViewStorage = detail::visibleOrders(engine);
  hft_trader::core::StrategyContext ctx{};
  ctx.shard = &shard;
  ctx.control = &control;
  ctx.snapshot = &engine.snapshot;
  ctx.marketData.snapshots = &engine.snapshot;
  ctx.marketData.count = 1;
  ctx.orderState.orders = orderViewStorage.data();
  ctx.orderState.count = orderViewStorage.size();
  ctx.positionState = engine.position;
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
  if (captureProbeTrace || captureIndicatorTrace) {
    hft_trader::core::initStrategyDecisionTrace(trace, ctx.nowMono.raw);
    ctx.decisionTrace = &trace;
  }
  const bool ok = descriptor.runCycle(state, ctx);
  if (captureProbeTrace || captureIndicatorTrace) {
    trace.orderCount = static_cast<std::uint32_t>(ctx.io.orderCount);
    trace.amendCount = static_cast<std::uint32_t>(ctx.io.amendCount);
    trace.cancelCount = static_cast<std::uint32_t>(ctx.io.cancelCount);
    if (captureProbeTrace) appendProbeMetric(engine, tsNs, trace);
    if (captureIndicatorTrace) appendStrategyIndicator(engine, tsNs, trace);
  }
  if (captureProbeTrace && !ok) return false;
  detail::applyIntents(engine, ctx.io, tsNs, request.orderLatencyUs, request.amendLatencyUs, request.cancelLatencyUs);
  return true;
}

std::uint64_t maxLatencyUs(const BacktestRunRequest& request) noexcept {
  std::uint64_t out = request.orderLatencyUs;
  if (request.amendLatencyUs > out) out = request.amendLatencyUs;
  if (request.cancelLatencyUs > out) out = request.cancelLatencyUs;
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
  std::int64_t out = std::numeric_limits<std::int64_t>::max();
  for (const detail::PendingAction& action : engine.pending) {
    if (action.activeTsNs < out) out = action.activeTsNs;
  }
  return out;
}

std::int64_t nextWakeTs(const StrategyDescriptor& descriptor, const void* state) noexcept {
  if (!descriptor.nextWakeMono) return std::numeric_limits<std::int64_t>::max();
  const TimeNs wake = descriptor.nextWakeMono(state);
  if (wake.raw == 0u) return std::numeric_limits<std::int64_t>::max();
  return toInt64Time(wake);
}

bool drainUserEvents(const StrategyDescriptor& descriptor,
                     void* state,
                     detail::EngineState& engine,
                     const hft_trader::core::SymbolShardConfig& shard,
                     const hft_trader::core::ShardControlState& control,
                     const BacktestRunRequest& request) {
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
      if (!detail::forceClosePosition(engine, untilTs, request.orderLatencyUs)) return false;
      untilTs = detail::delayedTs(untilTs, maxLatencyUs(request));
      if (!runUntil(descriptor, state, engine, shard, teardownControl, untilTs, request)) return false;
    }
    detail::cancelOpenOrders(engine, untilTs, request.cancelLatencyUs);
  }
  detail::cancelOpenOrders(engine, untilTs, request.cancelLatencyUs);
  return engine.netQtyRaw == 0 && engine.pending.empty();
}

void populateResultPnl(BacktestRunResult& result, const detail::EngineState& engine) noexcept {
  result.initialBalanceE8 = engine.initialBalanceE8;
  result.walletBalanceE8 = engine.walletBalanceE8;
  result.grossRealizedPnlE8 = engine.grossRealizedPnlE8;
  result.feesPaidE8 = engine.feesPaidE8;
  result.netRealizedPnlE8 = engine.netRealizedPnlE8;
  result.realizedPnlE8 = engine.netRealizedPnlE8;
  result.unrealizedPnlE8 = detail::unrealizedPnlE8(engine);
  result.totalPnlE8 = result.realizedPnlE8;
  result.openPositionQtyE8 = engine.netQtyRaw;
  result.strategyClosed = engine.netQtyRaw == 0;
  result.userEvents = engine.userEvents.size();
  result.timerWakeEvents = engine.timerWakeEvents;
  result.reduceOnlyOrders = engine.reduceOnlyOrders;
  result.reduceOnlyFills = engine.reduceOnlyFills;
  result.inputCoverage = engine.inputCoverage;
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
  result.orderLatencyUs = request.orderLatencyUs;
  result.amendLatencyUs = request.amendLatencyUs;
  result.cancelLatencyUs = request.cancelLatencyUs;
  result.initialBalanceE8 = request.initialBalanceE8;
  result.walletBalanceE8 = request.initialBalanceE8;

  if (request.sessionPath.empty() || request.configPath.empty()) {
    result.status = Status::InvalidArgument;
    result.error = "session_path and config_path are required";
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
    (void)detail::writeResult(result, detail::EngineState{});
    return result;
  }
  result.strategy = request.strategy.empty() ? cfg.strategyType : request.strategy;
  StrategyIndicatorProfileInfo indicatorProfile{};
  if (!resolveStrategyIndicatorProfile(request.indicatorProfile, indicatorProfile)) {
    result.status = Status::InvalidArgument;
    result.error = "unknown indicator profile";
    (void)detail::writeResult(result, detail::EngineState{});
    return result;
  }
  result.indicatorProfile = std::string{indicatorProfile.id};
  if (!strategyDeclaresIndicator(result.strategy, result.indicatorProfile)) {
    result.status = Status::InvalidArgument;
    result.error = "indicator profile is not supported by strategy";
    (void)detail::writeResult(result, detail::EngineState{});
    return result;
  }
  if (result.strategy != cfg.strategyType) {
    result.status = Status::ConfigMismatch;
    result.error = "requested strategy does not match config [strategy] type";
    (void)detail::writeResult(result, detail::EngineState{});
    return result;
  }
  const StrategyDescriptor* descriptor = hft_trader::runtime::findStrategyDescriptor(cfg.strategyType);
  if (!descriptor || !descriptor->resolveParams || !descriptor->initState || !descriptor->runCycle) {
    result.status = Status::Unsupported;
    result.error = "strategy descriptor is missing required callbacks";
    (void)detail::writeResult(result, detail::EngineState{});
    return result;
  }

  detail::SessionLoadPlan loadPlan{};
  if (!buildSessionLoadPlan(*descriptor, loadPlan, error)) {
    result.status = Status::Unsupported;
    result.error = error;
    (void)detail::writeResult(result, detail::EngineState{});
    return result;
  }

  detail::SessionData session{};
  result.status = detail::loadSession(request.sessionPath, loadPlan, session, error);
  if (!isOk(result.status)) {
    result.error = error;
    (void)detail::writeResult(result, detail::EngineState{});
    return result;
  }

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
    (void)detail::writeResult(result, detail::EngineState{});
    return result;
  }

  StrategyParamStorage params{};
  if (!descriptor->resolveParams(cfg, *venue, params, error)) {
    result.status = Status::InvalidArgument;
    result.error = error;
    (void)detail::writeResult(result, detail::EngineState{});
    return result;
  }
  StrategyInstrumentSpec instrument{};
  instrument.tickSizeRaw = session.instrument.tickSizeE8;
  instrument.stepSizeRaw = session.instrument.lotSizeE8;
  instrument.contractBaseQtyRaw = session.instrument.contractBaseQtyE8;
  instrument.tickGrid = hft_trader::core::prepareFastStepGrid(instrument.tickSizeRaw);
  instrument.qtyStepGrid = hft_trader::core::prepareFastStepGrid(instrument.stepSizeRaw);
  instrument.contractBaseQtyGrid = hft_trader::core::prepareFastStepGrid(instrument.contractBaseQtyRaw);

  alignas(hft_trader::runtime::kStrategyRuntimeStateAlign) std::array<unsigned char, hft_trader::runtime::kStrategyRuntimeStateBytes> state{};
  if (!descriptor->initState(state.data(), params, instrument)) {
    result.status = Status::InvalidArgument;
    result.error = "strategy initState failed";
    (void)detail::writeResult(result, detail::EngineState{});
    return result;
  }
  BacktestInputCoverage seedCoverage{};
  if (descriptor->seedHistory) {
    if (session.candles.empty()) {
      result.status = Status::CorruptData;
      result.error = "strategy requires candle history but candles channel is missing or empty";
      (void)detail::writeResult(result, detail::EngineState{});
      return result;
    }
    cxet::composite::TieredCandleHistory history{};
    if (!detail::buildTieredCandleHistory(session.candles, history)) {
      result.status = Status::CorruptData;
      result.error = "failed to build strategy candle history from recorder session";
      (void)detail::writeResult(result, detail::EngineState{});
      return result;
    }
    seedCoverage.seedM1Candles = history.m1Count.raw;
    seedCoverage.seedM15Candles = history.m15Count.raw;
    seedCoverage.seedD1Candles = history.d1Count.raw;
    hft_trader::runtime::StrategyHistorySeedStats stats{};
    if (!descriptor->seedHistory(state.data(), history, &stats)) {
      result.status = Status::InvalidArgument;
      result.error = "strategy seedHistory failed";
      (void)detail::writeResult(result, detail::EngineState{});
      return result;
    }
    if (isBacktestProbe(*descriptor) &&
        (seedCoverage.seedM1Candles == 0u || seedCoverage.seedM15Candles == 0u || seedCoverage.seedD1Candles == 0u)) {
      result.status = Status::CorruptData;
      result.error = "backtest_probe did not receive full m1/m15/d1 candle seed";
      (void)detail::writeResult(result, detail::EngineState{});
      return result;
    }
  }
  if (!seedTradeWarmupFromSession(*descriptor, state.data(), params, session, error)) {
    result.status = Status::CorruptData;
    result.error = error;
    (void)detail::writeResult(result, detail::EngineState{});
    return result;
  }
  seedCoverage.seedTradeRows = session.tradeWarmup.size();

  detail::EngineState engine{};
  configureStrategyIndicator(engine, indicatorProfile);
  engine.inputCoverage = seedCoverage;
  engine.exchange = sessionExchange;
  engine.symbol = selectedSymbol;
  detail::initializeAccounting(engine, request);
  if (session.hasSnapshot) {
    hft_trader::core::LocalOrderBookConfig bookConfig{};
    bookConfig.maxStoredLevelsPerSide.raw = rawdata::kMaxOrderbookLevels;
    bookConfig.tickSizeRaw = instrument.tickSizeRaw;
    hft_trader::core::applySnapshot(engine.orderBook, detail::toRuntimeSnapshot(session.snapshot), selectedSymbol, bookConfig);
    hft_trader::core::bindOrderBookSnapshot(engine.snapshot, engine.orderBook);
  }
  hft_trader::core::SymbolShardConfig shard{};
  shard.shardId.raw = 1;
  shard.strategyId.raw = 1;
  shard.symbolId.raw = 1;
  shard.symbol = selectedSymbol;
  hft_trader::core::ShardControlState control{};
  control.shardId = shard.shardId;

  std::vector<detail::TimelineEvent> timeline;
  detail::buildTimeline(session, timeline);
  result.trades = session.trades.size();
  result.bookTickers = session.bookTickers.size();
  result.depths = session.depths.size();
  result.events = timeline.size();
  result.firstTsNs = session.firstTsNs;
  result.lastTsNs = session.lastTsNs;
  detail::appendEquityPoint(engine, result.firstTsNs);
  detail::appendRiskSnapshot(engine, result.firstTsNs, detail::kRiskSnapshotReasonStart);

  if (!reportProgress(0, timeline.size(), "running", progressCallback, userData)) {
    result.status = Status::Cancelled;
    result.error = "cancelled";
    (void)detail::writeResult(result, engine);
    return result;
  }
  for (std::size_t i = 0; i < timeline.size(); ++i) {
    const detail::TimelineEvent& event = timeline[i];
    if (!runUntil(*descriptor, state.data(), engine, shard, control, event.tsNs, request)) {
      result.status = Status::InvalidArgument;
      result.error = "strategy event loop did not settle before market event";
      populateResultPnl(result, engine);
      result.orders = engine.orders.size();
      result.fills = engine.fills.size();
      (void)detail::writeResult(result, engine);
      return result;
    }
    if (event.kind == 0) {
      const auto delta = detail::toRuntime(session.depths[event.index]);
      hft_trader::core::copyDepthSnapshot(engine.snapshot, delta);
      if (engine.orderBook.initialized.raw != hft_trader::core::kAlgoFlagTrue.raw) {
        hft_trader::core::LocalOrderBookConfig bookConfig{};
        bookConfig.maxStoredLevelsPerSide.raw = rawdata::kMaxOrderbookLevels;
        bookConfig.tickSizeRaw = instrument.tickSizeRaw;
        hft_trader::core::applySnapshot(engine.orderBook, detail::toRuntimeSnapshot(session.depths[event.index]), selectedSymbol, bookConfig);
      } else {
        (void)hft_trader::core::applyDelta(engine.orderBook, delta);
      }
      hft_trader::core::bindOrderBookSnapshot(engine.snapshot, engine.orderBook);
    } else if (event.kind == 1) {
      const auto trade = detail::toRuntime(session.trades[event.index]);
      hft_trader::core::copyTradeSnapshot(engine.snapshot, trade);
      detail::tryFillOnTrade(engine, trade);
    } else {
      const auto ticker = detail::toRuntime(session.bookTickers[event.index]);
      hft_trader::core::copyBookTickerSnapshot(engine.snapshot, ticker);
      (void)hft_trader::core::applyBookTicker(engine.orderBook, ticker);
      hft_trader::core::bindOrderBookSnapshot(engine.snapshot, engine.orderBook);
      detail::tryFillOnTop(engine, event.tsNs);
    }
    if (!runCycle(*descriptor, state.data(), engine, shard, control, event.tsNs, request, nullptr)) {
      result.status = Status::InvalidArgument;
      result.error = "strategy runCycle failed on market event";
      populateResultPnl(result, engine);
      result.orders = engine.orders.size();
      result.fills = engine.fills.size();
      (void)detail::writeResult(result, engine);
      return result;
    }
    (void)detail::appendRiskSnapshotIfDue(engine, event.tsNs);
    if (!runUntil(*descriptor, state.data(), engine, shard, control, event.tsNs, request)) {
      result.status = Status::InvalidArgument;
      result.error = "strategy event loop did not settle after market event";
      populateResultPnl(result, engine);
      result.orders = engine.orders.size();
      result.fills = engine.fills.size();
      (void)detail::writeResult(result, engine);
      return result;
    }
    if ((i % 1024u) == 0u && !reportProgress(i, timeline.size(), "running", progressCallback, userData)) {
      result.status = Status::Cancelled;
      result.error = "cancelled";
      (void)detail::writeResult(result, engine);
      return result;
    }
  }
  if (!settleAfterReplay(*descriptor, state.data(), engine, shard, control, result.lastTsNs, request)) {
    result.status = Status::InvalidArgument;
    result.error = "strategy event loop did not settle after replay";
    populateResultPnl(result, engine);
    result.orders = engine.orders.size();
    result.fills = engine.fills.size();
    (void)detail::writeResult(result, engine);
    return result;
  }
  const std::int64_t finalTsNs = result.lastTsNs > 0 ? result.lastTsNs + 1 : result.lastTsNs;
  detail::appendEquityPoint(engine, finalTsNs);
  detail::appendRiskSnapshot(engine, finalTsNs, detail::kRiskSnapshotReasonFinal);

  populateResultPnl(result, engine);
  result.orders = engine.orders.size();
  result.fills = engine.fills.size();
  if (result.strategy == "backtest_probe" && !validateBacktestProbeCoverage(result, engine, result.error)) {
    result.status = Status::CorruptData;
    (void)detail::writeResult(result, engine);
    return result;
  }
  result.status = Status::Ok;
  Status writeStatus = detail::writeResult(result, engine);
  if (!isOk(writeStatus)) result.status = writeStatus;
  (void)reportProgress(timeline.size(), timeline.size(), "complete", progressCallback, userData);
  return result;
}

}  // namespace hft_backtest
