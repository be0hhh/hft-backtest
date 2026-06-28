#include "strategy/backtest_strategy_runtime.hpp"

#include "config/backtest_run_config.hpp"
#include "strategy/backtest_strategy_trace.hpp"

#include "hft_trader/api/OrderPathWarmup.hpp"
#include "hft_trader/runtime/runner/StrategyMarketDataRuntime.hpp"
#include "primitives/composite/OrderBookTapeRuntimeV1.hpp"

#include <algorithm>
#include <limits>
#include <string>

namespace hft_backtest {
namespace {

bool backtestStreamRequiredForReadiness(cxet::api::market::PublicMarketDataStream stream) noexcept {
  return stream == cxet::api::market::PublicMarketDataStream::Trades ||
         stream == cxet::api::market::PublicMarketDataStream::BookTicker ||
         stream == cxet::api::market::PublicMarketDataStream::PriceLimit ||
         stream == cxet::api::market::PublicMarketDataStream::Orderbook;
}

bool strategySnapshotReady(const hft_trader::runtime::StrategyDescriptor& descriptor,
                           const hft_trader::core::StrategyMarketSnapshot& snapshot) noexcept {
  const std::size_t streamCount = hft_trader::runtime::descriptorStreamCount(descriptor);
  for (std::size_t i = 0u; i < streamCount; ++i) {
    const cxet::api::market::PublicMarketDataStream stream = hft_trader::runtime::descriptorStreamAt(descriptor, i);
    if (hft_trader::runtime::descriptorStreamOptional(descriptor, i)) continue;
    if (!backtestStreamRequiredForReadiness(stream)) continue;
    if (!hft_trader::runtime::snapshotHasRequiredStream(snapshot, stream, &descriptor)) return false;
  }
  return true;
}

std::int64_t toInt64Time(TimeNs value) noexcept {
  if (value.raw > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return std::numeric_limits<std::int64_t>::max();
  return static_cast<std::int64_t>(value.raw);
}

std::int64_t nextPendingTs(const detail::EngineState& engine) noexcept {
  return engine.pending.empty()
      ? std::numeric_limits<std::int64_t>::max()
      : engine.pending.front().activeTsNs;
}

std::int64_t userEventFallbackTs(const cxet::composite::UserDataEvent& event) noexcept {
  return event.updateTime.raw != 0u ? toInt64Time(event.updateTime) : toInt64Time(event.eventTime);
}

std::int64_t nextUserEventDeliveryTs(const detail::EngineState& engine) noexcept {
  if (engine.deliveredUserEvents >= engine.userEvents.size()) return std::numeric_limits<std::int64_t>::max();
  if (engine.deliveredUserEvents < engine.userEventDeliveryTsNs.size()) {
    return engine.userEventDeliveryTsNs[engine.deliveredUserEvents];
  }
  return userEventFallbackTs(engine.userEvents[engine.deliveredUserEvents]);
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

std::int64_t nextWakeTs(const hft_trader::runtime::StrategyDescriptor& descriptor, const void* state) noexcept {
  if (!descriptor.nextWakeMono) return std::numeric_limits<std::int64_t>::max();
  const TimeNs wake = descriptor.nextWakeMono(state);
  if (wake.raw == 0u) return std::numeric_limits<std::int64_t>::max();
  return toInt64Time(wake);
}

std::int64_t nextRateLimitCooldownTs(const detail::EngineState& engine) noexcept {
  if (engine.rateLimitCooldownUntilTsNs <= 0) return std::numeric_limits<std::int64_t>::max();
  return engine.rateLimitCooldownUntilTsNs;
}

std::int64_t nextRateLimitCooldownTs(const std::vector<detail::EngineState>& engines) noexcept {
  std::int64_t out = std::numeric_limits<std::int64_t>::max();
  for (const detail::EngineState& engine : engines) {
    const std::int64_t candidate = nextRateLimitCooldownTs(engine);
    if (candidate < out) out = candidate;
  }
  return out;
}

void clearRateLimitCooldown(detail::EngineState& engine, std::int64_t tsNs) noexcept {
  if (engine.rateLimitCooldownUntilTsNs > 0 && engine.rateLimitCooldownUntilTsNs <= tsNs) {
    engine.rateLimitCooldownUntilTsNs = 0;
    engine.rateLimitSnapshotRequested = true;
  }
  if (engine.rateLimitProblemUntilTsNs > 0 && engine.rateLimitProblemUntilTsNs <= tsNs) {
    engine.rateLimitProblemUntilTsNs = 0;
  }
}

void clearRateLimitCooldown(std::vector<detail::EngineState>& engines, std::int64_t tsNs) noexcept {
  for (detail::EngineState& engine : engines) clearRateLimitCooldown(engine, tsNs);
}

bool dueWithinBoundary(std::int64_t tsNs, std::int64_t untilTs, RunUntilBoundary boundary) noexcept {
  if (tsNs == std::numeric_limits<std::int64_t>::max()) return false;
  return boundary == RunUntilBoundary::Exclusive ? tsNs < untilTs : tsNs <= untilTs;
}

std::size_t backtestRouteLegIndex(std::size_t legCount,
                                  hft_trader::core::ExecutionRouteHandle route) noexcept {
  if (legCount == 0u) return static_cast<std::size_t>(-1);
  if (route.raw == hft_trader::core::kDefaultExecutionRouteHandle.raw) return 0u;
  const std::size_t index = static_cast<std::size_t>(route.raw - 1u);
  return index < legCount ? index : static_cast<std::size_t>(-1);
}

bool orderPathWarmupCandidate(const hft_trader::core::OrderIntent& intent) noexcept {
  return !intent.service &&
         !intent.reduceOnly &&
         intent.clientIntentId.raw != 0u &&
         intent.qty.raw > 0 &&
         (intent.orderType == canon::OrderType::Market ||
          intent.orderType == canon::OrderType::Limit ||
          intent.orderType == canon::OrderType::Stop);
}

bool warmupRouteSeen(const hft_trader::core::ExecutionRouteHandle* routes,
                     std::size_t count,
                     hft_trader::core::ExecutionRouteHandle route) noexcept {
  for (std::size_t i = 0u; i < count; ++i) {
    if (routes[i].raw == route.raw) return true;
  }
  return false;
}

bool warmupReasonFailOpen(hft_trader::strategy::OrderPathWarmupSkipReason reason) noexcept {
  return reason == hft_trader::strategy::OrderPathWarmupSkipReason::BadTemplate ||
         reason == hft_trader::strategy::OrderPathWarmupSkipReason::MissingMarketData ||
         reason == hft_trader::strategy::OrderPathWarmupSkipReason::MissingInstrument ||
         reason == hft_trader::strategy::OrderPathWarmupSkipReason::BadQtyGrid ||
         reason == hft_trader::strategy::OrderPathWarmupSkipReason::QtyTooSmall ||
         reason == hft_trader::strategy::OrderPathWarmupSkipReason::BadPrice ||
         reason == hft_trader::strategy::OrderPathWarmupSkipReason::BadClientId;
}

hft_trader::core::StrategyOrderPathWarmupView backtestWarmupView(const detail::EngineState& engine) noexcept {
  hft_trader::core::StrategyOrderPathWarmupView view{};
  view.lastCommandMono = engine.lastOrderPathHotMono;
  view.lastWarmupMono = engine.lastOrderPathHotMono;
  view.lastAckMono = engine.lastOrderPathHotMono;
  view.lastHotMono = engine.lastOrderPathHotMono;
  view.lastSkipReason = engine.lastWarmupSkipReason;
  view.sessionHealthy = true;
  view.warmupInFlight = engine.warmupInFlight;
  view.serviceExposureOpen = engine.serviceExposureOpen;
  view.routeHot = view.sessionHealthy &&
                  !view.warmupInFlight &&
                  !view.serviceExposureOpen &&
                  view.lastHotMono.raw != 0u;
  view.warmupStale = !view.routeHot;
  return view;
}

bool backtestOrderPathHot(const detail::EngineState& engine, TimeNs nowMono) noexcept {
  const hft_trader::core::StrategyOrderPathWarmupView view = backtestWarmupView(engine);
  return hft_trader::strategy::orderPathWarmupHot(
      view,
      nowMono,
      hft_trader::strategy::kDefaultOrderPathWarmupPolicy.heatTtlNs);
}

Id allocateBacktestServiceClientIntentId(detail::EngineState& engine) noexcept {
  if ((engine.nextServiceClientIntentRaw & 0x8000000000000000ull) == 0u ||
      engine.nextServiceClientIntentRaw == 0u) {
    engine.nextServiceClientIntentRaw = 0x8000000000000000ull;
  }
  Id out{};
  out.raw = engine.nextServiceClientIntentRaw++;
  if (engine.nextServiceClientIntentRaw == 0u) engine.nextServiceClientIntentRaw = 0x8000000000000000ull;
  return out;
}

void markBacktestWarmupSkipped(detail::EngineState& engine,
                               hft_trader::strategy::OrderPathWarmupSkipReason reason) noexcept {
  engine.lastWarmupSkipReason = static_cast<std::uint8_t>(reason);
}

bool applyBacktestWarmupGate(hft_trader::core::StrategyContext& ctx,
                             detail::EngineState* engines,
                             std::size_t engineCount) noexcept {
  if (!engines || engineCount == 0u || ctx.io.commandOut.size() == 0u) return false;
  hft_trader::core::ExecutionRouteHandle warmupRoutes[hft_trader::core::kStrategyRuntimeMaxLegs]{};
  hft_trader::core::OrderIntent warmupTemplates[hft_trader::core::kStrategyRuntimeMaxLegs]{};
  std::size_t warmupRouteCount = 0u;
  auto addWarmupTemplate = [&](const hft_trader::core::OrderIntent& intent) noexcept {
    if (!orderPathWarmupCandidate(intent)) return;
    if (warmupRouteSeen(warmupRoutes, warmupRouteCount, intent.routeHandle)) return;
    const std::size_t leg = backtestRouteLegIndex(engineCount, intent.routeHandle);
    if (leg >= engineCount) return;
    if (backtestOrderPathHot(engines[leg], ctx.nowMono)) return;
    if (warmupRouteCount >= hft_trader::core::kStrategyRuntimeMaxLegs) return;
    warmupRoutes[warmupRouteCount] = intent.routeHandle;
    warmupTemplates[warmupRouteCount] = intent;
    ++warmupRouteCount;
  };
  for (std::size_t i = 0u; i < ctx.io.orderCount; ++i) addWarmupTemplate(ctx.io.orderOut[i]);
  for (std::size_t i = 0u; i < ctx.io.commandCount; ++i) {
    const hft_trader::core::StrategyCommandIntent& command = ctx.io.commandOut[i];
    if (command.kind == hft_trader::core::StrategyCommandIntentKind::Order) addWarmupTemplate(command.order);
  }
  if (warmupRouteCount == 0u) return false;

  hft_trader::core::StrategyCommandIntent warmupCommands[hft_trader::core::kStrategyRuntimeMaxLegs]{};
  std::size_t warmupCommandCount = 0u;
  bool failOpen = false;
  bool holdWithoutNewWarmup = false;
  for (std::size_t i = 0u; i < warmupRouteCount; ++i) {
    const std::size_t leg = backtestRouteLegIndex(engineCount, warmupRoutes[i]);
    if (leg >= engineCount) {
      failOpen = true;
      break;
    }
    const Id serviceId = allocateBacktestServiceClientIntentId(engines[leg]);
    const hft_trader::strategy::OrderPathWarmupBuildResult build =
        hft_trader::strategy::buildOrderPathWarmup(ctx, warmupTemplates[i], serviceId);
    if (build.built) {
      warmupCommands[warmupCommandCount] = hft_trader::core::StrategyCommandIntent{};
      warmupCommands[warmupCommandCount].kind = hft_trader::core::StrategyCommandIntentKind::WarmupOrder;
      warmupCommands[warmupCommandCount].warmup = build.intent;
      ++warmupCommandCount;
      continue;
    }
    markBacktestWarmupSkipped(engines[leg], build.reason);
    if (warmupReasonFailOpen(build.reason)) {
      failOpen = true;
      break;
    }
    holdWithoutNewWarmup = true;
  }
  if (failOpen || (warmupCommandCount == 0u && !holdWithoutNewWarmup)) return false;
  if (warmupCommandCount > ctx.io.commandOut.size()) return false;
  ctx.io.cancelCount = 0u;
  ctx.io.orderCount = 0u;
  ctx.io.commandCount = warmupCommandCount;
  for (std::size_t i = 0u; i < warmupCommandCount; ++i) ctx.io.commandOut[i] = warmupCommands[i];
  return true;
}

void applyIntentsToLegs(std::vector<detail::EngineState>& engines,
                        const hft_trader::core::StrategyIo& io,
                        std::int64_t tsNs) {
  for (std::size_t i = 0; i < io.commandCount; ++i) {
    const hft_trader::core::StrategyCommandIntent& command = io.commandOut[i];
    if (command.kind == hft_trader::core::StrategyCommandIntentKind::Cancel) {
      const std::size_t leg = backtestRouteLegIndex(engines.size(), command.cancel.routeHandle);
      if (leg >= engines.size()) continue;
      std::array<hft_trader::core::CancelIntent, 1> cancel{command.cancel};
      hft_trader::core::StrategyIo one{};
      one.cancelOut = Span<hft_trader::core::CancelIntent>(cancel.data(), cancel.size());
      one.cancelCount = 1u;
      detail::applyIntents(engines[leg], one, tsNs, engines[leg].latency);
    } else if (command.kind == hft_trader::core::StrategyCommandIntentKind::Order) {
      const std::size_t leg = backtestRouteLegIndex(engines.size(), command.order.routeHandle);
      if (leg >= engines.size()) continue;
      std::array<hft_trader::core::OrderIntent, 1> order{command.order};
      hft_trader::core::StrategyIo one{};
      one.orderOut = Span<hft_trader::core::OrderIntent>(order.data(), order.size());
      one.orderCount = 1u;
      detail::applyIntents(engines[leg], one, tsNs, engines[leg].latency);
    } else if (command.kind == hft_trader::core::StrategyCommandIntentKind::WarmupOrder) {
      const std::size_t leg = backtestRouteLegIndex(engines.size(), command.warmup.order.routeHandle);
      if (leg >= engines.size()) continue;
      std::array<hft_trader::core::StrategyCommandIntent, 1> warmup{command};
      hft_trader::core::StrategyIo one{};
      one.commandOut = Span<hft_trader::core::StrategyCommandIntent>(warmup.data(), warmup.size());
      one.commandCount = 1u;
      detail::applyIntents(engines[leg], one, tsNs, engines[leg].latency);
    }
  }
  for (std::size_t i = 0; i < io.cancelCount; ++i) {
    const std::size_t leg = backtestRouteLegIndex(engines.size(), io.cancelOut[i].routeHandle);
    if (leg >= engines.size()) continue;
    hft_trader::core::StrategyIo one{};
    one.cancelOut = Span<hft_trader::core::CancelIntent>(const_cast<hft_trader::core::CancelIntent*>(&io.cancelOut[i]), 1u);
    one.cancelCount = 1u;
    detail::applyIntents(engines[leg], one, tsNs, engines[leg].latency);
  }
  for (std::size_t i = 0; i < io.orderCount; ++i) {
    const std::size_t leg = backtestRouteLegIndex(engines.size(), io.orderOut[i].routeHandle);
    if (leg >= engines.size()) continue;
    hft_trader::core::StrategyIo one{};
    one.orderOut = Span<hft_trader::core::OrderIntent>(const_cast<hft_trader::core::OrderIntent*>(&io.orderOut[i]), 1u);
    one.orderCount = 1u;
    detail::applyIntents(engines[leg], one, tsNs, engines[leg].latency);
  }
}

std::int64_t nextPendingTs(const std::vector<detail::EngineState>& engines) noexcept {
  std::int64_t out = std::numeric_limits<std::int64_t>::max();
  for (const detail::EngineState& engine : engines) {
    const std::int64_t candidate = nextPendingTs(engine);
    if (candidate < out) out = candidate;
  }
  return out;
}

std::int64_t nextUserEventDeliveryTs(const std::vector<detail::EngineState>& engines) noexcept {
  std::int64_t out = std::numeric_limits<std::int64_t>::max();
  for (const detail::EngineState& engine : engines) {
    const std::int64_t candidate = nextUserEventDeliveryTs(engine);
    if (candidate < out) out = candidate;
  }
  return out;
}

bool drainUserEvents(const hft_trader::runtime::StrategyDescriptor& descriptor,
                     void* state,
                     detail::EngineState& engine,
                     const StrategyInstrumentSpec& instrument,
                     SingleLegCycleScratch& scratch,
                     const hft_trader::core::SymbolShardConfig& shard,
                     const hft_trader::core::ShardControlState& control,
                     const BacktestRunRequest& request,
                     std::int64_t untilTs,
                     RunUntilBoundary boundary = RunUntilBoundary::Inclusive) {
  if (engine.liquidated || engine.riskStopped) return true;
  std::uint32_t guard = 0u;
  while (dueWithinBoundary(nextUserEventDeliveryTs(engine), untilTs, boundary)) {
    if (++guard > 4096u) return false;
    const std::size_t eventIndex = engine.deliveredUserEvents++;
    const cxet::composite::UserDataEvent event = engine.userEvents[eventIndex];
    const std::int64_t tsNs = eventIndex < engine.userEventDeliveryTsNs.size()
        ? engine.userEventDeliveryTsNs[eventIndex]
        : userEventFallbackTs(event);
    if (!runCycle(descriptor, state, engine, instrument, scratch, shard, control, tsNs, request, &event)) return false;
  }
  return true;
}

bool drainUserEventsMulti(const hft_trader::runtime::StrategyDescriptor& descriptor,
                          void* state,
                          std::vector<detail::EngineState>& engines,
                          const std::vector<StrategyInstrumentSpec>& legInstruments,
                          MultiLegCycleScratch& scratch,
                          const hft_trader::core::SymbolShardConfig& shard,
                          const hft_trader::core::ShardControlState& control,
                          const BacktestRunRequest& request,
                          std::int64_t untilTs,
                          RunUntilBoundary boundary = RunUntilBoundary::Inclusive) {
  for (const detail::EngineState& engine : engines) {
    if (engine.liquidated || engine.riskStopped) return true;
  }
  std::uint32_t guard = 0u;
  bool progressed = true;
  while (progressed) {
    progressed = false;
    for (std::size_t leg = 0; leg < engines.size(); ++leg) {
      detail::EngineState& engine = engines[leg];
      if (!dueWithinBoundary(nextUserEventDeliveryTs(engine), untilTs, boundary)) continue;
      if (++guard > 4096u) return false;
      const std::size_t eventIndex = engine.deliveredUserEvents++;
      const cxet::composite::UserDataEvent event = engine.userEvents[eventIndex];
      const std::int64_t tsNs = eventIndex < engine.userEventDeliveryTsNs.size()
          ? engine.userEventDeliveryTsNs[eventIndex]
          : userEventFallbackTs(event);
      if (!runCycleMulti(descriptor, state, engines, legInstruments, scratch, shard, control, static_cast<std::uint32_t>(leg), tsNs, request, &event)) return false;
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

bool portfolioHasOpenState(const std::vector<detail::EngineState>& engines) noexcept {
  for (const detail::EngineState& engine : engines) {
    if (engine.netQtyRaw != 0 || !engine.pending.empty()) return true;
  }
  return false;
}

const hft_trader::runtime::VenueRuntimeConfig* venueByName(
    const hft_trader::runtime::HftRuntimeConfig& cfg,
    const std::string& name) noexcept {
  for (const hft_trader::runtime::VenueRuntimeConfig& candidate : cfg.venues) {
    if (candidate.name == name) return &candidate;
  }
  return nullptr;
}

bool venueMatchesSession(const hft_trader::runtime::VenueRuntimeConfig& venue,
                         const detail::SessionData& session) noexcept {
  if (venue.exchange.raw != detail::exchangeFromText(session.exchange).raw) return false;
  if (venue.market.raw != detail::marketFromText(session.market).raw) return false;
  for (const Symbol& symbol : venue.symbols) {
    if (session.symbol == symbol.data) return true;
  }
  return false;
}

const hft_trader::runtime::VenueRuntimeConfig* venueForSession(
    const hft_trader::runtime::HftRuntimeConfig& cfg,
    const detail::SessionData& session) noexcept {
  for (const hft_trader::runtime::VenueRuntimeConfig& candidate : cfg.venues) {
    if (venueMatchesSession(candidate, session)) return &candidate;
  }
  return nullptr;
}

void cancelOpenOrdersForTraderRiskClose(detail::EngineState& engine, std::int64_t tsNs) noexcept {
  engine.pending.clear();
  for (detail::OpenOrder& order : engine.open) {
    if (order.state.valid && order.recordIndex < engine.orders.size()) {
      engine.orders[order.recordIndex].status = detail::kRecordStatusCancelled;
      engine.orders[order.recordIndex].tsNs = tsNs;
    }
    order = detail::OpenOrder{};
  }
}

}  // namespace

void MultiLegCycleScratch::reset(std::size_t count) noexcept {
  legCount = count;
  revisions.fill(0u);
}

void MultiLegCycleScratch::refreshLeg(std::size_t leg,
                                      const std::vector<detail::EngineState>& engines,
                                      const std::vector<StrategyInstrumentSpec>& legInstruments) noexcept {
  if (leg >= legCount || leg >= engines.size()) return;
  const detail::EngineState& engine = engines[leg];
  snapshots[leg] = engine.snapshot;
  candles[leg] = engine.candle;
  orderViews[leg].orders = orderStorage[leg].data();
  orderViews[leg].count = detail::visibleOrders(engine, orderStorage[leg].data(), orderStorage[leg].size());
  positions[leg] = engine.position;
  accounts[leg] = accountStateFromEngine(engine);
  instruments[leg] = leg < legInstruments.size() ? legInstruments[leg] : StrategyInstrumentSpec{};
  revisions[leg] = engine.contextRevision;
}

void MultiLegCycleScratch::refreshDirty(const std::vector<detail::EngineState>& engines,
                                        const std::vector<StrategyInstrumentSpec>& legInstruments) noexcept {
  for (std::size_t leg = 0; leg < legCount; ++leg) {
    if (leg >= engines.size()) break;
    if (revisions[leg] != engines[leg].contextRevision) refreshLeg(leg, engines, legInstruments);
  }
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

std::size_t boundedRiskLegCount(std::size_t count) noexcept {
  return count < hft_trader::core::kStrategyRuntimeMaxLegs
      ? count
      : hft_trader::core::kStrategyRuntimeMaxLegs;
}

void refreshRiskAccountsFromEngines(
    const std::vector<detail::EngineState>& engines,
    std::array<hft_trader::core::StrategyAccountState, hft_trader::core::kStrategyRuntimeMaxLegs>& out) noexcept {
  out.fill(hft_trader::core::StrategyAccountState{});
  const std::size_t count = boundedRiskLegCount(engines.size());
  for (std::size_t i = 0u; i < count; ++i) out[i] = accountStateFromEngine(engines[i]);
}

void resolveRiskVenuesForSessions(
    const hft_trader::runtime::HftRuntimeConfig& cfg,
    const std::vector<detail::SessionData>& sessions,
    std::array<const hft_trader::runtime::VenueRuntimeConfig*, hft_trader::core::kStrategyRuntimeMaxLegs>& out) noexcept {
  out.fill(nullptr);
  const std::size_t count = boundedRiskLegCount(sessions.size());
  for (const hft_trader::runtime::PortfolioRuntimeConfig& portfolio : cfg.portfolios) {
    if (portfolio.legs.size() != count) continue;
    bool matches = true;
    for (std::size_t i = 0u; i < count; ++i) {
      const hft_trader::runtime::VenueRuntimeConfig* venue = venueByName(cfg, portfolio.legs[i].venue);
      if (!venue || !venueMatchesSession(*venue, sessions[i]) || sessions[i].symbol != portfolio.legs[i].symbol.data) {
        matches = false;
        break;
      }
    }
    if (!matches) continue;
    for (std::size_t i = 0u; i < count; ++i) out[i] = venueByName(cfg, portfolio.legs[i].venue);
    return;
  }
  for (std::size_t i = 0u; i < count; ++i) out[i] = venueForSession(cfg, sessions[i]);
}

bool forceCloseForTraderRisk(detail::EngineState& engine, std::int64_t tsNs) {
  cancelOpenOrdersForTraderRiskClose(engine, tsNs);
  if (engine.netQtyRaw == 0) return true;
  const std::uint64_t closeLatencyUs =
      detail::sampleLatencyUs(engine.latency, detail::BacktestLatencyChannel::MarketOrder);
  return detail::forceClosePosition(engine, tsNs, closeLatencyUs);
}

std::int64_t portfolioEquityE8(const std::vector<detail::EngineState>& engines,
                               std::int64_t initialBalanceE8) noexcept {
  std::int64_t out = initialBalanceE8;
  for (const detail::EngineState& engine : engines) {
    out += engine.netRealizedPnlE8 + detail::unrealizedPnlE8(engine);
  }
  return out;
}

TimeNs timeNsFromI64(std::int64_t value) noexcept {
  TimeNs out{};
  if (value > 0) out.raw = static_cast<std::uint64_t>(value);
  return out;
}

void applyMarketEventToSnapshots(detail::EngineState& engine,
                                 const detail::SessionData& session,
                                 const detail::TimelineEvent& event,
                                 const Symbol& selectedSymbol,
                                 const StrategyInstrumentSpec& instrument,
                                 bool strategyVisible) {
  hft_trader::core::StrategyMarketSnapshot& snapshot = strategyVisible ? engine.snapshot : engine.venueSnapshot;
  hft_trader::core::StrategyCandleSnapshot& candle = strategyVisible ? engine.candle : engine.venueCandle;
  hft_trader::core::LocalOrderBookState& orderBook = strategyVisible ? engine.orderBook : engine.venueOrderBook;
  hft_trader::core::BboReconstructionState& bbo = strategyVisible ? engine.bboReconstruction : engine.venueBboReconstruction;
  bool changed = false;
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
    changed = true;
  } else if (event.kind == detail::kTimelineKindTrade) {
    const auto trade = detail::toRuntime(session.trades[event.index]);
    hft_trader::core::copyTradeSnapshot(snapshot, trade);
    (void)hft_trader::core::applyTrade(bbo, trade);
    applyBacktestReconstructedBbo(snapshot, orderBook, bbo);
    if (!strategyVisible) detail::tryFillOnTrade(engine, trade);
    changed = true;
  } else if (event.kind == detail::kTimelineKindBookTicker) {
    const auto ticker = detail::toRuntime(session.bookTickers[event.index]);
    hft_trader::core::copyBookTickerSnapshot(snapshot, ticker);
    (void)hft_trader::core::applyBookTicker(bbo, ticker);
    applyBacktestReconstructedBbo(snapshot, orderBook, bbo);
    hft_trader::core::bindOrderBookSnapshot(snapshot, orderBook);
    if (!strategyVisible) detail::tryFillOnTop(engine, event.tsNs);
    changed = true;
  } else if (event.kind == detail::kTimelineKindPriceLimit) {
    const auto priceLimit = detail::toRuntime(session.priceLimits[event.index]);
    hft_trader::core::copyPriceLimitSnapshot(snapshot, priceLimit);
    changed = true;
  } else if (event.kind == detail::kTimelineKindLiquidation) {
    const auto liquidation = detail::toRuntime(session.liquidations[event.index], engine.exchange, engine.symbol);
    hft_trader::core::copyLiquidationSnapshot(snapshot, liquidation);
    changed = true;
  } else if (event.kind == detail::kTimelineKindMarkPrice) {
    const auto markPrice = detail::toRuntime(session.markPrices[event.index]);
    hft_trader::core::copyMarkPriceSnapshot(snapshot, markPrice);
    if (!strategyVisible) detail::tryTriggerStopMarketsOnMark(engine, markPrice);
    changed = true;
  } else if (event.kind == detail::kTimelineKindIndexPrice) {
    const auto indexPrice = detail::toRuntime(session.indexPrices[event.index]);
    hft_trader::core::copyIndexPriceSnapshot(snapshot, indexPrice);
    if (!strategyVisible) detail::tryTriggerStopMarketsOnIndex(engine, indexPrice);
    changed = true;
  } else if (event.kind == detail::kTimelineKindFunding) {
    const auto funding = detail::toRuntime(session.funding[event.index]);
    hft_trader::core::copyFundingSnapshot(snapshot, funding);
    changed = true;
  } else if (event.kind == detail::kTimelineKindOpenInterest) {
    const auto openInterest = detail::toRuntime(session.openInterest[event.index]);
    hft_trader::core::copyOpenInterestSnapshot(snapshot, openInterest);
    changed = true;
  } else if (event.kind == detail::kTimelineKindCandle) {
    candle = detail::toRuntime(session.candles[event.index]);
    changed = true;
  }
  if (changed) detail::bumpContextRevision(engine);
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

bool runCycle(const hft_trader::runtime::StrategyDescriptor& descriptor,
              void* state,
              detail::EngineState& engine,
              const StrategyInstrumentSpec& instrument,
              SingleLegCycleScratch& scratch,
              const hft_trader::core::SymbolShardConfig& shard,
              const hft_trader::core::ShardControlState& control,
              std::int64_t tsNs,
              const BacktestRunRequest& request,
              const cxet::composite::UserDataEvent* userEvent) {
  if (engine.liquidated || engine.riskStopped) return true;
  if (!strategySnapshotReady(descriptor, engine.snapshot)) return true;
  const std::size_t orderViewCount = detail::visibleOrders(engine, scratch.orderStorage.data(), scratch.orderStorage.size());
  hft_trader::core::StrategyContext ctx{};
  ctx.shard = &shard;
  ctx.control = &control;
  ctx.snapshot = &engine.snapshot;
  ctx.marketData.snapshots = &engine.snapshot;
  ctx.marketData.count = 1;
  ctx.candle = engine.candle;
  ctx.candles.candles = &engine.candle;
  ctx.candles.count = 1;
  ctx.instrumentSpec = instrument;
  ctx.instrumentSpecs.legs = &instrument;
  ctx.instrumentSpecs.count = 1u;
  ctx.orderState.orders = scratch.orderStorage.data();
  ctx.orderState.count = orderViewCount;
  ctx.positionState = engine.position;
  ctx.accountState = accountStateFromEngine(engine);
  ctx.accountStates.legs = &ctx.accountState;
  ctx.accountStates.count = 1u;
  ctx.userEvent = userEvent;
  ctx.hasUserEvent = userEvent && userEvent->kind != cxet::composite::UserDataEventKind::Unknown;
  ctx.nowMono.raw = static_cast<std::uint64_t>(tsNs > 0 ? tsNs : 0);
  ctx.nowUtc = ctx.nowMono;
  scratch.warmupViews[0] = backtestWarmupView(engine);
  ctx.orderPathWarmup = scratch.warmupViews[0];
  ctx.orderPathWarmups.routes = scratch.warmupViews.data();
  ctx.orderPathWarmups.count = 1u;
  if (userEvent && userEvent->orderProblem != canon::OrderProblem::None) {
    engine.pendingProblem = hft_trader::core::StrategyProblemEvent{};
    engine.pendingProblem.problem = userEvent->orderProblem;
    engine.pendingProblem.valid = true;
    engine.hasPendingProblem = true;
  }
  if (engine.hasPendingProblem) {
    ctx.problemEvent = &engine.pendingProblem;
    ctx.hasProblemEvent = true;
    engine.hasPendingProblem = false;
  }
  if (engine.rateLimitSnapshotRequested || ctx.hasUserEvent || ctx.hasProblemEvent) {
    ctx.rateLimits = engine.rateLimits.snapshotBook(hft_trader::core::kDefaultExecutionRouteHandle,
                                                    engine.exchange,
                                                    engine.market,
                                                    1u,
                                                    ctx.nowMono);
    engine.rateLimitSnapshotRequested = false;
  }
  ctx.userEventRecvMono = ctx.nowMono;
  ctx.userEventRecvUtc = ctx.nowUtc;
  ctx.initialOrderSeedAllowed = true;
  ctx.io.orderOut = Span<hft_trader::core::OrderIntent>(scratch.orderBuf.data(), scratch.orderBuf.size());
  ctx.io.cancelOut = Span<hft_trader::core::CancelIntent>(scratch.cancelBuf.data(), scratch.cancelBuf.size());
  ctx.io.commandOut = Span<hft_trader::core::StrategyCommandIntent>(scratch.commandBuf.data(), scratch.commandBuf.size());
  ctx.io.clear();
  recordInputCoverage(engine);
  hft_trader::core::StrategyDecisionTrace trace{};
  const bool captureIndicatorTrace = strategyIndicatorEnabled(engine);
  const bool captureRangeTrace = false;
  const bool captureSpreadTrace = request.captureStrategySpread && emitsArbSpreadTrace(descriptor);
  if (captureIndicatorTrace || captureRangeTrace || captureSpreadTrace) {
    hft_trader::core::initStrategyDecisionTrace(trace, ctx.nowMono.raw);
    ctx.decisionTrace = &trace;
  }
  const bool canWarmupRollback = descriptor.captureState && descriptor.restoreAfterOrderFailure;
  hft_trader::runtime::StrategyStateSnapshot before{};
  if (canWarmupRollback) descriptor.captureState(state, before);
  const bool ok = descriptor.runCycle(state, ctx);
  if (ctx.io.requestRateLimitSnapshot) engine.rateLimitSnapshotRequested = true;
  if (!hft_trader::core::canEmitNewOrder(control)) {
    ctx.io.orderCount = 0u;
    ctx.io.commandCount = 0u;
  }
  const bool warmupGateHeldRealOrders = canWarmupRollback && applyBacktestWarmupGate(ctx, &engine, 1u);
  if (warmupGateHeldRealOrders) descriptor.restoreAfterOrderFailure(state, before);
  if (captureIndicatorTrace || captureRangeTrace || captureSpreadTrace) {
    std::uint32_t orderedOrderCount = 0u;
    std::uint32_t orderedCancelCount = 0u;
    for (std::size_t i = 0u; i < ctx.io.commandCount; ++i) {
      if (scratch.commandBuf[i].kind == hft_trader::core::StrategyCommandIntentKind::Order ||
          scratch.commandBuf[i].kind == hft_trader::core::StrategyCommandIntentKind::WarmupOrder) ++orderedOrderCount;
      else if (scratch.commandBuf[i].kind == hft_trader::core::StrategyCommandIntentKind::Cancel) ++orderedCancelCount;
    }
    trace.orderCount = static_cast<std::uint32_t>(ctx.io.orderCount) + orderedOrderCount;
    trace.cancelCount = static_cast<std::uint32_t>(ctx.io.cancelCount) + orderedCancelCount;
    if (captureIndicatorTrace) appendStrategyIndicator(engine, tsNs, trace);
    if (captureRangeTrace) appendStrategyRange(engine, tsNs, trace);
    if (captureSpreadTrace) appendStrategySpread(engine, tsNs, trace);
  }
  if (!ok) return false;
  detail::applyIntents(engine, ctx.io, tsNs, engine.latency);
  return true;
}

bool runCycleMulti(const hft_trader::runtime::StrategyDescriptor& descriptor,
                   void* state,
                   std::vector<detail::EngineState>& engines,
                   const std::vector<StrategyInstrumentSpec>& legInstruments,
                   MultiLegCycleScratch& scratch,
                   const hft_trader::core::SymbolShardConfig& shard,
                   const hft_trader::core::ShardControlState& control,
                   std::uint32_t eventLegIndex,
                   std::int64_t tsNs,
                   const BacktestRunRequest& request,
                   const cxet::composite::UserDataEvent* userEvent) {
  if (engines.empty()) return true;
  for (const detail::EngineState& engine : engines) {
    if (engine.liquidated || engine.riskStopped) return true;
  }
  for (const detail::EngineState& engine : engines) {
    if (!strategySnapshotReady(descriptor, engine.snapshot)) return true;
  }
  const std::size_t legCount = engines.size() < hft_trader::core::kStrategyRuntimeMaxLegs ? engines.size() : hft_trader::core::kStrategyRuntimeMaxLegs;
  if (scratch.legCount != legCount) scratch.reset(legCount);
  scratch.refreshDirty(engines, legInstruments);
  hft_trader::core::StrategyContext ctx{};
  ctx.shard = &shard;
  ctx.control = &control;
  ctx.snapshot = &scratch.snapshots[0];
  ctx.marketData.snapshots = scratch.snapshots.data();
  ctx.marketData.count = legCount;
  ctx.candle = scratch.candles[0];
  ctx.candles.candles = scratch.candles.data();
  ctx.candles.count = legCount;
  ctx.instrumentSpec = scratch.instruments[0];
  ctx.instrumentSpecs.legs = scratch.instruments.data();
  ctx.instrumentSpecs.count = legCount;
  ctx.orderState = scratch.orderViews[0];
  ctx.orderStates.legs = scratch.orderViews.data();
  ctx.orderStates.count = legCount;
  ctx.positionState = scratch.positions[0];
  ctx.positionStates.legs = scratch.positions.data();
  ctx.positionStates.count = legCount;
  ctx.accountState = scratch.accounts[0];
  ctx.accountStates.legs = scratch.accounts.data();
  ctx.accountStates.count = legCount;
  ctx.userEvent = userEvent;
  ctx.hasUserEvent = userEvent && userEvent->kind != cxet::composite::UserDataEventKind::Unknown;
  ctx.eventLegIndex = eventLegIndex;
  ctx.hasEventLeg = eventLegIndex < legCount;
  ctx.nowMono.raw = static_cast<std::uint64_t>(tsNs > 0 ? tsNs : 0);
  ctx.nowUtc = ctx.nowMono;
  for (std::size_t i = 0u; i < legCount; ++i) {
    scratch.warmupViews[i] = backtestWarmupView(engines[i]);
  }
  ctx.orderPathWarmup = scratch.warmupViews[0];
  ctx.orderPathWarmups.routes = scratch.warmupViews.data();
  ctx.orderPathWarmups.count = legCount;
  if (userEvent && userEvent->orderProblem != canon::OrderProblem::None && eventLegIndex < engines.size()) {
    engines[eventLegIndex].pendingProblem = hft_trader::core::StrategyProblemEvent{};
    engines[eventLegIndex].pendingProblem.problem = userEvent->orderProblem;
    engines[eventLegIndex].pendingProblem.valid = true;
    engines[eventLegIndex].hasPendingProblem = true;
  }
  detail::EngineState* problemEngine = nullptr;
  if (eventLegIndex < engines.size() && engines[eventLegIndex].hasPendingProblem) {
    problemEngine = &engines[eventLegIndex];
  } else {
    for (detail::EngineState& candidate : engines) {
      if (candidate.hasPendingProblem) {
        problemEngine = &candidate;
        break;
      }
    }
  }
  if (problemEngine) {
    ctx.problemEvent = &problemEngine->pendingProblem;
    ctx.hasProblemEvent = true;
    problemEngine->hasPendingProblem = false;
  }
  bool includeRateLimits = ctx.hasUserEvent || ctx.hasProblemEvent;
  for (const detail::EngineState& engine : engines) {
    includeRateLimits = includeRateLimits || engine.rateLimitSnapshotRequested;
  }
  if (includeRateLimits) {
    ctx.rateLimits = hft_trader::core::StrategyRateLimitBook{};
    for (std::size_t i = 0u; i < legCount; ++i) {
      hft_trader::core::StrategyRateLimitBook legLimits = engines[i].rateLimits.snapshotBook(
          hft_trader::core::ExecutionRouteHandle{static_cast<std::uint32_t>(i + 1u)},
          engines[i].exchange,
          engines[i].market,
          1u,
          ctx.nowMono);
      for (std::uint8_t j = 0u; j < legLimits.count && ctx.rateLimits.count < ctx.rateLimits.buckets.size(); ++j) {
        ctx.rateLimits.buckets[ctx.rateLimits.count++] = legLimits.buckets[j];
      }
      for (std::uint8_t j = 0u; j < legLimits.actionCount && ctx.rateLimits.actionCount < ctx.rateLimits.actions.size(); ++j) {
        ctx.rateLimits.actionRoutes[ctx.rateLimits.actionCount] = legLimits.actionRoutes[j];
        ctx.rateLimits.actions[ctx.rateLimits.actionCount++] = legLimits.actions[j];
      }
      engines[i].rateLimitSnapshotRequested = false;
    }
  }
  ctx.userEventRecvMono = ctx.nowMono;
  ctx.userEventRecvUtc = ctx.nowUtc;
  ctx.initialOrderSeedAllowed = true;
  ctx.io.orderOut = Span<hft_trader::core::OrderIntent>(scratch.orderBuf.data(), scratch.orderBuf.size());
  ctx.io.cancelOut = Span<hft_trader::core::CancelIntent>(scratch.cancelBuf.data(), scratch.cancelBuf.size());
  ctx.io.commandOut = Span<hft_trader::core::StrategyCommandIntent>(scratch.commandBuf.data(), scratch.commandBuf.size());
  ctx.io.clear();
  recordInputCoverage(engines[0]);
  hft_trader::core::StrategyDecisionTrace trace{};
  const bool captureIndicatorTrace = strategyIndicatorEnabled(engines[0]);
  const bool captureRangeTrace = false;
  const bool captureSpreadTrace = request.captureStrategySpread && emitsArbSpreadTrace(descriptor);
  if (captureIndicatorTrace || captureRangeTrace || captureSpreadTrace) {
    hft_trader::core::initStrategyDecisionTrace(trace, ctx.nowMono.raw);
    ctx.decisionTrace = &trace;
  }
  const bool canWarmupRollback = descriptor.captureState && descriptor.restoreAfterOrderFailure;
  hft_trader::runtime::StrategyStateSnapshot before{};
  if (canWarmupRollback) descriptor.captureState(state, before);
  const bool ok = descriptor.runCycle(state, ctx);
  if (ctx.io.requestRateLimitSnapshot) {
    for (detail::EngineState& engine : engines) engine.rateLimitSnapshotRequested = true;
  }
  if (!hft_trader::core::canEmitNewOrder(control)) {
    ctx.io.orderCount = 0u;
    ctx.io.commandCount = 0u;
  }
  const bool warmupGateHeldRealOrders = canWarmupRollback && applyBacktestWarmupGate(ctx, engines.data(), legCount);
  if (warmupGateHeldRealOrders) descriptor.restoreAfterOrderFailure(state, before);
  if (captureIndicatorTrace || captureRangeTrace || captureSpreadTrace) {
    std::uint32_t orderedOrderCount = 0u;
    std::uint32_t orderedCancelCount = 0u;
    for (std::size_t i = 0u; i < ctx.io.commandCount; ++i) {
      if (scratch.commandBuf[i].kind == hft_trader::core::StrategyCommandIntentKind::Order ||
          scratch.commandBuf[i].kind == hft_trader::core::StrategyCommandIntentKind::WarmupOrder) ++orderedOrderCount;
      else if (scratch.commandBuf[i].kind == hft_trader::core::StrategyCommandIntentKind::Cancel) ++orderedCancelCount;
    }
    trace.orderCount = static_cast<std::uint32_t>(ctx.io.orderCount) + orderedOrderCount;
    trace.cancelCount = static_cast<std::uint32_t>(ctx.io.cancelCount) + orderedCancelCount;
    if (captureIndicatorTrace) appendStrategyIndicator(engines[0], tsNs, trace);
    if (captureRangeTrace) appendStrategyRange(engines[0], tsNs, trace);
    if (captureSpreadTrace) appendStrategySpread(engines[0], tsNs, trace);
  }
  if (!ok) return false;
  applyIntentsToLegs(engines, ctx.io, tsNs);
  return true;
}

bool runUntil(const hft_trader::runtime::StrategyDescriptor& descriptor,
              void* state,
              detail::EngineState& engine,
              const StrategyInstrumentSpec& instrument,
              SingleLegCycleScratch& scratch,
              const hft_trader::core::SymbolShardConfig& shard,
              const hft_trader::core::ShardControlState& control,
              std::int64_t untilTs,
              const BacktestRunRequest& request,
              RunUntilBoundary boundary) {
  if (engine.liquidated || engine.riskStopped) return true;
  std::uint32_t guard = 0u;
  while (true) {
    if (++guard > 8192u) return false;
    const std::int64_t pendingTs = nextPendingTs(engine);
    const std::int64_t wakeTs = nextWakeTs(descriptor, state);
    const std::int64_t userTs = nextUserEventDeliveryTs(engine);
    const std::int64_t cooldownTs = nextRateLimitCooldownTs(engine);
    std::int64_t nextTs = std::min(std::min(pendingTs, wakeTs), std::min(userTs, cooldownTs));
    if (nextTs == std::numeric_limits<std::int64_t>::max() || !dueWithinBoundary(nextTs, untilTs, boundary)) break;
    if (userTs <= pendingTs && userTs <= wakeTs && userTs <= cooldownTs) {
      if (!drainUserEvents(descriptor, state, engine, instrument, scratch, shard, control, request, userTs)) return false;
    } else if (pendingTs <= wakeTs && pendingTs <= cooldownTs) {
      detail::applyDueActions(engine, pendingTs);
    } else if (wakeTs <= cooldownTs) {
      ++engine.timerWakeEvents;
      if (!runCycle(descriptor, state, engine, instrument, scratch, shard, control, wakeTs, request, nullptr)) return false;
    } else {
      clearRateLimitCooldown(engine, cooldownTs);
      ++engine.timerWakeEvents;
      if (!runCycle(descriptor, state, engine, instrument, scratch, shard, control, cooldownTs, request, nullptr)) return false;
    }
  }
  return drainUserEvents(descriptor, state, engine, instrument, scratch, shard, control, request, untilTs, boundary);
}

bool runUntilMulti(const hft_trader::runtime::StrategyDescriptor& descriptor,
                   void* state,
                   std::vector<detail::EngineState>& engines,
                   const std::vector<StrategyInstrumentSpec>& legInstruments,
                   MultiLegCycleScratch& scratch,
                   const hft_trader::core::SymbolShardConfig& shard,
                   const hft_trader::core::ShardControlState& control,
                   std::int64_t untilTs,
                   const BacktestRunRequest& request,
                   RunUntilBoundary boundary) {
  for (const detail::EngineState& engine : engines) {
    if (engine.liquidated || engine.riskStopped) return true;
  }
  const std::int64_t pendingTs = nextPendingTs(engines);
  const std::int64_t wakeTs = nextWakeTs(descriptor, state);
  const std::int64_t userTs = nextUserEventDeliveryTs(engines);
  const std::int64_t cooldownTs = nextRateLimitCooldownTs(engines);
  const std::int64_t nextTs = std::min(std::min(pendingTs, wakeTs), std::min(userTs, cooldownTs));
  if (!hasUndeliveredUserEvents(engines) &&
      (nextTs == std::numeric_limits<std::int64_t>::max() || !dueWithinBoundary(nextTs, untilTs, boundary))) {
    return true;
  }
  std::uint32_t guard = 0u;
  while (true) {
    if (++guard > 8192u) return false;
    const std::int64_t pendingTs = nextPendingTs(engines);
    const std::int64_t wakeTs = nextWakeTs(descriptor, state);
    const std::int64_t userTs = nextUserEventDeliveryTs(engines);
    const std::int64_t cooldownTs = nextRateLimitCooldownTs(engines);
    std::int64_t nextTs = std::min(std::min(pendingTs, wakeTs), std::min(userTs, cooldownTs));
    if (nextTs == std::numeric_limits<std::int64_t>::max() || !dueWithinBoundary(nextTs, untilTs, boundary)) break;
    if (userTs <= pendingTs && userTs <= wakeTs && userTs <= cooldownTs) {
      if (!drainUserEventsMulti(descriptor, state, engines, legInstruments, scratch, shard, control, request, userTs)) return false;
    } else if (pendingTs <= wakeTs && pendingTs <= cooldownTs) {
      for (detail::EngineState& engine : engines) detail::applyDueActions(engine, pendingTs);
    } else if (wakeTs <= cooldownTs) {
      if (!engines.empty()) ++engines[0].timerWakeEvents;
      if (!runCycleMulti(descriptor, state, engines, legInstruments, scratch, shard, control, 0u, wakeTs, request, nullptr)) return false;
    } else {
      clearRateLimitCooldown(engines, cooldownTs);
      if (!engines.empty()) ++engines[0].timerWakeEvents;
      if (!runCycleMulti(descriptor, state, engines, legInstruments, scratch, shard, control, 0u, cooldownTs, request, nullptr)) return false;
    }
  }
  return drainUserEventsMulti(descriptor, state, engines, legInstruments, scratch, shard, control, request, untilTs, boundary);
}

bool settleAfterReplay(const hft_trader::runtime::StrategyDescriptor& descriptor,
                       void* state,
                       detail::EngineState& engine,
                       const StrategyInstrumentSpec& instrument,
                       SingleLegCycleScratch& scratch,
                       const hft_trader::core::SymbolShardConfig& shard,
                       const hft_trader::core::ShardControlState& control,
                       std::int64_t lastTsNs,
                       const BacktestRunRequest& request) {
  std::int64_t untilTs = lastTsNs > 0 ? detail::delayedTs(lastTsNs, maxLatencyUs(request)) : lastTsNs;
  if (!runUntil(descriptor, state, engine, instrument, scratch, shard, control, untilTs, request)) return false;
  for (std::uint32_t i = 0u; i < 32u; ++i) {
    const std::int64_t wakeTs = nextWakeTs(descriptor, state);
    if (wakeTs == std::numeric_limits<std::int64_t>::max()) break;
    untilTs = wakeTs;
    if (!runUntil(descriptor, state, engine, instrument, scratch, shard, control, untilTs, request)) return false;
    untilTs = detail::delayedTs(untilTs, maxLatencyUs(request));
    if (!runUntil(descriptor, state, engine, instrument, scratch, shard, control, untilTs, request)) return false;
    if (engine.netQtyRaw == 0 && engine.pending.empty() && nextWakeTs(descriptor, state) == std::numeric_limits<std::int64_t>::max()) break;
  }
  hft_trader::core::ShardControlState teardownControl = control;
  teardownControl.killActive = hft_trader::core::kAlgoFlagTrue;
  for (std::uint32_t i = 0u; i < 8u && (engine.netQtyRaw != 0 || !engine.pending.empty()); ++i) {
    untilTs = detail::delayedTs(untilTs, maxLatencyUs(request));
    if (!runUntil(descriptor, state, engine, instrument, scratch, shard, teardownControl, untilTs, request)) return false;
    if (engine.netQtyRaw != 0) {
      if (!detail::forceClosePosition(engine, untilTs, engine.latency)) return false;
      untilTs = detail::delayedTs(untilTs, maxLatencyUs(request));
      if (!runUntil(descriptor, state, engine, instrument, scratch, shard, teardownControl, untilTs, request)) return false;
    }
    detail::cancelOpenOrders(engine, untilTs, engine.latency);
  }
  detail::cancelOpenOrders(engine, untilTs, engine.latency);
  untilTs = detail::delayedTs(untilTs, maxLatencyUs(request));
  if (!runUntil(descriptor, state, engine, instrument, scratch, shard, teardownControl, untilTs, request)) return false;
  return engine.netQtyRaw == 0 && engine.pending.empty();
}

bool settleAfterReplayMulti(const hft_trader::runtime::StrategyDescriptor& descriptor,
                            void* state,
                            std::vector<detail::EngineState>& engines,
                            const std::vector<StrategyInstrumentSpec>& legInstruments,
                            MultiLegCycleScratch& scratch,
                            const hft_trader::core::SymbolShardConfig& shard,
                            const hft_trader::core::ShardControlState& control,
                            std::int64_t lastTsNs,
                            const BacktestRunRequest& request) {
  std::int64_t untilTs = lastTsNs > 0 ? detail::delayedTs(lastTsNs, maxLatencyUs(request)) : lastTsNs;
  if (!runUntilMulti(descriptor, state, engines, legInstruments, scratch, shard, control, untilTs, request)) return false;
  for (std::uint32_t i = 0u; i < 32u; ++i) {
    const std::int64_t wakeTs = nextWakeTs(descriptor, state);
    if (wakeTs == std::numeric_limits<std::int64_t>::max()) break;
    untilTs = wakeTs;
    if (!runUntilMulti(descriptor, state, engines, legInstruments, scratch, shard, control, untilTs, request)) return false;
    untilTs = detail::delayedTs(untilTs, maxLatencyUs(request));
    if (!runUntilMulti(descriptor, state, engines, legInstruments, scratch, shard, control, untilTs, request)) return false;
    if (!portfolioHasOpenState(engines) && nextWakeTs(descriptor, state) == std::numeric_limits<std::int64_t>::max()) break;
  }
  hft_trader::core::ShardControlState teardownControl = control;
  teardownControl.killActive = hft_trader::core::kAlgoFlagTrue;
  for (std::uint32_t i = 0u; i < 8u && portfolioHasOpenState(engines); ++i) {
    untilTs = detail::delayedTs(untilTs, maxLatencyUs(request));
    if (!runUntilMulti(descriptor, state, engines, legInstruments, scratch, shard, teardownControl, untilTs, request)) return false;
    for (detail::EngineState& engine : engines) {
      if (engine.netQtyRaw != 0 && !detail::forceClosePosition(engine, untilTs, engine.latency)) return false;
    }
    untilTs = detail::delayedTs(untilTs, maxLatencyUs(request));
    if (!runUntilMulti(descriptor, state, engines, legInstruments, scratch, shard, teardownControl, untilTs, request)) return false;
    for (detail::EngineState& engine : engines) detail::cancelOpenOrders(engine, untilTs, engine.latency);
  }
  for (detail::EngineState& engine : engines) detail::cancelOpenOrders(engine, untilTs, engine.latency);
  untilTs = detail::delayedTs(untilTs, maxLatencyUs(request));
  if (!runUntilMulti(descriptor, state, engines, legInstruments, scratch, shard, teardownControl, untilTs, request)) return false;
  return !portfolioHasOpenState(engines);
}

}  // namespace hft_backtest
