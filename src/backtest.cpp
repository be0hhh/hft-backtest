#include "hft_backtest/backtest.hpp"

#include "backtest_engine.hpp"
#include "session_loader.hpp"

#include "hft_trader/core/ControlState.hpp"
#include "hft_trader/core/LocalOrderBook.hpp"
#include "hft_trader/core/StrategyContract.hpp"
#include "hft_trader/runtime/GeneratedStrategyRegistry.hpp"
#include "hft_trader/runtime/RuntimeConfig.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
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
  return request.sessionPath / "backtests" / (runId + ".json");
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

bool runCycle(const StrategyDescriptor& descriptor,
              void* state,
              detail::EngineState& engine,
              const hft_trader::core::SymbolShardConfig& shard,
              const hft_trader::core::ShardControlState& control,
              std::int64_t tsNs,
              const BacktestRunRequest& request,
              const cxet::composite::UserDataEvent* userEvent) {
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
  if (!descriptor.runCycle(state, ctx)) return false;
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
    if (wakeTs == std::numeric_limits<std::int64_t>::max()) return true;
    untilTs = wakeTs;
    if (!runUntil(descriptor, state, engine, shard, control, untilTs, request)) return false;
    untilTs = detail::delayedTs(untilTs, maxLatencyUs(request));
    if (!runUntil(descriptor, state, engine, shard, control, untilTs, request)) return false;
  }
  return false;
}

void populateResultPnl(BacktestRunResult& result, const detail::EngineState& engine) noexcept {
  result.initialBalanceE8 = engine.initialBalanceE8;
  result.walletBalanceE8 = engine.walletBalanceE8;
  result.grossRealizedPnlE8 = engine.grossRealizedPnlE8;
  result.feesPaidE8 = engine.feesPaidE8;
  result.netRealizedPnlE8 = engine.netRealizedPnlE8;
  result.realizedPnlE8 = engine.netRealizedPnlE8;
  result.unrealizedPnlE8 = detail::unrealizedPnlE8(engine);
  result.totalPnlE8 = result.realizedPnlE8 + result.unrealizedPnlE8;
  result.openPositionQtyE8 = engine.netQtyRaw;
  result.strategyClosed = engine.netQtyRaw == 0;
  result.userEvents = engine.userEvents.size();
  result.timerWakeEvents = engine.timerWakeEvents;
  result.reduceOnlyOrders = engine.reduceOnlyOrders;
  result.reduceOnlyFills = engine.reduceOnlyFills;
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

  detail::SessionData session{};
  std::string error;
  result.status = detail::loadSession(request.sessionPath, session, error);
  if (!isOk(result.status)) {
    result.error = error;
    (void)detail::writeResult(result, detail::EngineState{});
    return result;
  }

  hft_trader::runtime::HftRuntimeConfig cfg{};
  if (!hft_trader::runtime::loadRuntimeConfig(request.configPath.string().c_str(), cfg, error)) {
    result.status = Status::InvalidArgument;
    result.error = error;
    (void)detail::writeResult(result, detail::EngineState{});
    return result;
  }
  result.strategy = request.strategy.empty() ? cfg.strategyType : request.strategy;
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

  const ExchangeId sessionExchange = detail::exchangeFromText(session.exchange);
  const canon::MarketType sessionMarket = detail::marketFromText(session.market);
  const hft_trader::runtime::VenueRuntimeConfig* venue = nullptr;
  for (const auto& candidate : cfg.venues) {
    if (candidate.exchange.raw != sessionExchange.raw || candidate.market.raw != sessionMarket.raw) continue;
    for (const Symbol& symbol : candidate.symbols) {
      if (session.symbol == symbol.data) venue = &candidate;
    }
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

  detail::EngineState engine{};
  engine.exchange = sessionExchange;
  engine.symbol = venue->symbols.front();
  detail::initializeAccounting(engine, request);
  if (session.hasSnapshot) {
    hft_trader::core::LocalOrderBookConfig bookConfig{};
    bookConfig.maxStoredLevelsPerSide.raw = rawdata::kMaxOrderbookLevels;
    bookConfig.tickSizeRaw = instrument.tickSizeRaw;
    hft_trader::core::applySnapshot(engine.orderBook, detail::toRuntimeSnapshot(session.snapshot), venue->symbols.front(), bookConfig);
    hft_trader::core::bindOrderBookSnapshot(engine.snapshot, engine.orderBook);
  }
  hft_trader::core::SymbolShardConfig shard{};
  shard.shardId.raw = 1;
  shard.strategyId.raw = 1;
  shard.symbolId.raw = 1;
  shard.symbol = venue->symbols.front();
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
      (void)hft_trader::core::applyDelta(engine.orderBook, delta);
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

  populateResultPnl(result, engine);
  result.orders = engine.orders.size();
  result.fills = engine.fills.size();
  result.status = Status::Ok;
  Status writeStatus = detail::writeResult(result, engine);
  if (!isOk(writeStatus)) result.status = writeStatus;
  (void)reportProgress(timeline.size(), timeline.size(), "complete", progressCallback, userData);
  return result;
}

}  // namespace hft_backtest
