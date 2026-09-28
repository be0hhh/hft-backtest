#include "trading_core/Algorithm/Math/Integer/Saturating.hpp"
#include "BacktestStrategyRuntime.hpp"

#include "../Config/BacktestRunConfig.hpp"
#include "BacktestStrategyIntentLowering.hpp"
#include "BacktestStrategyTrace.hpp"

#include "trading_core/Strategy/Context/StrategyContextView.hpp"
#include "trading_core/Execution/Order/OrderPathWarmup.hpp"
#include "trading_core/Strategy/StrategyInputFreshness.hpp"
#include "trading_core/Strategy/StrategyMarketData.hpp"
#include "cxet/Primitives/Composite/OrderBookTapeRuntimeV1.hpp"

#include <algorithm>
#include <limits>
#include <string>

namespace hft_backtest {
namespace {

using strategy_intent_lowering::applyToLegs;
using strategy_intent_lowering::applyToSingleLeg;
using strategy_intent_lowering::applyTransactionCycle;
using strategy_intent_lowering::bindTransactionLegs;
using strategy_intent_lowering::configuredDefaultExecutionRoute;
using strategy_intent_lowering::configuredPrimaryLegIndex;
using strategy_intent_lowering::footprintSupported;
using strategy_intent_lowering::lowerDesiredOrders;
using strategy_intent_lowering::routeLegIndex;
using strategy_intent_lowering::routeTradable;
using strategy_intent_lowering::transactionAdmission;

std::int64_t toInt64Time(TimeNs value) noexcept {
  if (value.raw > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return std::numeric_limits<std::int64_t>::max();
  return static_cast<std::int64_t>(value.raw);
}

[[nodiscard]] bool hasExactExchangeTimestamp(
    const hftrec::replay::EventArrival& arrival,
    std::int64_t exchangeTimestampNs) noexcept {
  return exchangeTimestampNs > 0 &&
      (arrival.flags &
       hftrec::replay::EventArrivalExchangeTimestampMissing) == 0u;
}

[[nodiscard]] trading_core::BboReconstructionTimestampOrigin
reconstructionTimestampOrigin(
    const hftrec::replay::EventArrival& arrival,
    std::int64_t exchangeTimestampNs) noexcept {
  return hasExactExchangeTimestamp(arrival, exchangeTimestampNs)
      ? trading_core::BboReconstructionTimestampOrigin::Exchange
      : trading_core::BboReconstructionTimestampOrigin::Unknown;
}

std::int64_t nextPendingTs(const detail::EngineState& engine) noexcept {
  return engine.pending.empty()
      ? std::numeric_limits<std::int64_t>::max()
      : engine.pending.front().activeTsNs;
}

std::int64_t userEventFallbackTs(const cxet::composite::UserDataEvent& event) noexcept {
  if (cxet::composite::userDataFieldPresent(
          event, cxet::composite::UserDataField::UpdateTime)) {
    return toInt64Time(event.updateTime);
  }
  return cxet::composite::userDataFieldPresent(
             event, cxet::composite::UserDataField::EventTime)
      ? toInt64Time(event.eventTime)
      : 0;
}

[[nodiscard]] cxet::composite::UserDataSemanticMask
backtestSimulatedUserEventSemanticMask(
    const cxet::composite::UserDataEvent* event) noexcept {
  if (!event) return 0u;
  using Kind = cxet::composite::UserDataEventKind;
  using Semantic = cxet::composite::UserDataSemantic;
  switch (event->kind) {
    case Kind::Order: {
      cxet::composite::UserDataSemanticMask mask =
          cxet::composite::userDataSemanticBit(Semantic::OrderTransition);
      if (cxet::composite::userDataFieldPresent(
              *event, cxet::composite::UserDataField::LastFilledQty) &&
          event->lastFilledQty.raw > 0) {
        mask |= cxet::composite::userDataSemanticBit(
            Semantic::FillExecution);
      }
      return mask;
    }
    case Kind::Trade:
      return cxet::composite::userDataSemanticBit(Semantic::FillExecution);
    case Kind::Balance:
      return cxet::composite::userDataSemanticBit(Semantic::BalanceState);
    case Kind::Position:
      return cxet::composite::userDataSemanticBit(Semantic::PositionState);
    case Kind::Unknown:
      return 0u;
  }
  return 0u;
}

std::int64_t nextUserEventDeliveryTs(const detail::EngineState& engine) noexcept {
  if (engine.deliveredUserEvents >= engine.userEvents.size()) return std::numeric_limits<std::int64_t>::max();
  if (engine.deliveredUserEvents < engine.userEventDeliveryTsNs.size()) {
    return engine.userEventDeliveryTsNs[engine.deliveredUserEvents];
  }
  return userEventFallbackTs(engine.userEvents[engine.deliveredUserEvents]);
}

void overlayBacktestReconstructedBbo(
    trading_core::StrategyMarketSnapshot& snapshot,
    trading_core::LocalOrderBookState& orderBook,
    const trading_core::BboReconstructionView& view) noexcept {
  if (trading_core::validBboReconstruction(view) &&
      orderBook.initialized.raw == trading_core::kAlgoFlagTrue.raw) {
    (void)trading_core::applyBboOverlay(
        orderBook, view.bookTicker,
        trading_core::AlgoFlag{view.bidQtyKnown.raw},
        trading_core::AlgoFlag{view.askQtyKnown.raw});
    trading_core::bindOrderBookSnapshot(snapshot, orderBook);
  }
}

void publishBacktestReconstructedBbo(
    trading_core::StrategyMarketSnapshot& snapshot,
    trading_core::LocalOrderBookState& orderBook,
    const trading_core::BboReconstructionState& bbo,
    std::uint64_t receiveMonotonicNs) noexcept {
  const trading_core::BboReconstructionView view = trading_core::bboReconstructionView(bbo);
  TimeNs receiveMonotonic{};
  receiveMonotonic.raw = receiveMonotonicNs;
  trading_core::copyReconstructedBboSnapshot(
      snapshot, view, receiveMonotonic);
  overlayBacktestReconstructedBbo(snapshot, orderBook, view);
}

void refreshCapturedBookTicker(
    trading_core::StrategyMarketSnapshot& snapshot,
    const detail::TimelineEvent& event) noexcept {
  TimeNs receiveMonotonic{};
  receiveMonotonic.raw = static_cast<std::uint64_t>(
      event.deliveryTsNs > 0 ? event.deliveryTsNs : 0);
  TimeNs receiveRealtime{};
  receiveRealtime.raw = static_cast<std::uint64_t>(
      event.deliveryRealtimeNs > 0 ? event.deliveryRealtimeNs : 0);
  if (snapshot.hasBookTicker.raw ==
      trading_core::kAlgoFlagTrue.raw) {
    snapshot.lastBookTickerRecvMono = receiveMonotonic;
    snapshot.lastBookTickerRecvUtc = receiveRealtime;
  }
  if (snapshot.hasReconstructedBbo.raw ==
      trading_core::kAlgoFlagTrue.raw) {
    snapshot.reconstructedBookTickerRecvMono = receiveMonotonic;
  }
}

[[nodiscard]] bool applyCapturedBookTicker(
    trading_core::StrategyMarketSnapshot& snapshot,
    trading_core::BboReconstructionState& bbo,
    const detail::BookTickerRow& row,
    const detail::TimelineEvent& event,
    std::uint64_t replayReceiveMonotonicNs,
    const cxet::composite::BookTickerRuntimeV1& ticker) noexcept {
  TimeNs receiveMonotonic{};
  receiveMonotonic.raw = replayReceiveMonotonicNs;
  TimeNs receiveRealtime{};
  receiveRealtime.raw = static_cast<std::uint64_t>(
      event.deliveryRealtimeNs > 0 ? event.deliveryRealtimeNs : 0);
  const bool exactExchangeTimestamp =
      hasExactExchangeTimestamp(row.arrival, row.tsNs);
  const auto timestampOrigin = exactExchangeTimestamp
      ? trading_core::MarketTimestampOrigin::Exchange
      : trading_core::MarketTimestampOrigin::Receive;
  const auto reconstructionOrigin =
      reconstructionTimestampOrigin(row.arrival, row.tsNs);
  if (row.bidPresent && row.askPresent) {
    if (!trading_core::applyBboReconstructionBookTicker(
            bbo, ticker, reconstructionOrigin)) {
      return false;
    }
    trading_core::copyBookTickerSnapshot(
        snapshot, ticker, receiveMonotonic, receiveRealtime,
        timestampOrigin);
    return true;
  }

  if (!row.bidPresent && !row.askPresent) return false;
  cxet::composite::BookTickerSideUpdateRuntimeV1 update{};
  update.ts = ticker.ts;
  bool applied = false;
  if (row.bidPresent) {
    update.side = Side::Buy();
    update.action = cxet::composite::BookTickerSideAction::Upsert;
    update.level = ticker.bid;
    applied = trading_core::applyBboReconstructionBookTickerSide(
        bbo, update, reconstructionOrigin);
    if (!applied) return false;
  }
  if (row.askPresent) {
    update.side = Side::Sell();
    update.action = cxet::composite::BookTickerSideAction::Upsert;
    update.level = ticker.ask;
    applied = trading_core::applyBboReconstructionBookTickerSide(
        bbo, update, reconstructionOrigin);
    if (!applied) return false;
  }
  const auto capturedView = trading_core::bboReconstructionView(bbo);
  if (trading_core::validBboReconstruction(capturedView)) {
    trading_core::copyBookTickerSnapshot(
        snapshot, capturedView.bookTicker, receiveMonotonic, receiveRealtime,
        timestampOrigin);
  } else {
    trading_core::copyPartialBookTickerSnapshot(
        snapshot, capturedView, receiveMonotonic, receiveRealtime,
        timestampOrigin);
  }
  return applied;
}

std::int64_t nextWakeTs(const trading_core::StrategyDescriptor& descriptor, const void* state) noexcept {
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

std::uint64_t backtestTradableLegMask(const BacktestRunRequest& request,
                                      std::size_t legCount) noexcept {
  if (request.tradeMode == BacktestTradeMode::PrimaryOnly) {
    const std::uint32_t primary = configuredPrimaryLegIndex(request, legCount);
    return primary < 64u ? (std::uint64_t{1} << primary) : 0u;
  }
  if (legCount >= 64u) return trading_core::kAllStrategyLegsTradableMask;
  return legCount == 0u ? 0u : ((std::uint64_t{1} << legCount) - 1u);
}

trading_core::ShardControlState backtestControlWithInputFreshness(
    const trading_core::ShardControlState& control,
    const trading_core::strategy::StrategyInputSourceMaskState& freshness) noexcept {
  trading_core::ShardControlState out = control;
  if (!freshness.globallyDegraded) return out;
  out.inputDegraded = trading_core::kAlgoFlagTrue;
  if (out.haltReason.raw == trading_core::kHaltReasonNone.raw ||
      out.haltReason.raw == trading_core::kHaltReasonStaleInput.raw) {
    out.haltReason = trading_core::kHaltReasonStaleInput;
  }
  return out;
}

template<typename Context>
void setBacktestTradePolicy(Context& ctx,
                            const BacktestRunRequest& request,
                            std::size_t legCount,
                            std::uint64_t effectiveTradableLegMask) noexcept {
  const std::uint32_t primary = configuredPrimaryLegIndex(request, legCount);
  const std::uint8_t modeRaw = static_cast<std::uint8_t>(request.tradeMode);
  if constexpr (requires(Context& candidate, bool value) { candidate.hasPrimaryLegIndex = value; }) {
    ctx.hasPrimaryLegIndex = request.hasPrimaryLegIndex;
  }
  if constexpr (requires(Context& candidate, std::uint32_t value) { candidate.primaryLegIndex = value; }) {
    ctx.primaryLegIndex = primary;
  } else if constexpr (requires(Context& candidate) { candidate.primaryLegIndex; }) {
    ctx.primaryLegIndex = static_cast<decltype(ctx.primaryLegIndex)>(primary);
  }
  if constexpr (requires(Context& candidate, std::uint64_t value) { candidate.tradableLegMask = value; }) {
    ctx.tradableLegMask = effectiveTradableLegMask;
  } else if constexpr (requires(Context& candidate) { candidate.tradableLegMask; }) {
    ctx.tradableLegMask = static_cast<decltype(ctx.tradableLegMask)>(effectiveTradableLegMask);
  }
  if constexpr (requires(Context& candidate,
                         trading_core::ExecutionRouteHandle value) {
                  candidate.defaultExecutionRoute = value;
                }) {
    ctx.defaultExecutionRoute =
        configuredDefaultExecutionRoute(request, legCount);
  }
  if constexpr (requires(Context& candidate, std::uint8_t value) { candidate.tradeMode = value; }) {
    ctx.tradeMode = modeRaw;
  } else if constexpr (requires(Context& candidate) { candidate.tradeMode; }) {
    ctx.tradeMode = static_cast<decltype(ctx.tradeMode)>(modeRaw);
  }
}

void retainCancelOutputsWhenNewOrdersBlocked(
    trading_core::StrategyIo& io) noexcept {
  io.orderCount = 0u;
  const std::size_t readableCommands =
      io.commandCount < io.commandOut.size()
          ? io.commandCount
          : io.commandOut.size();
  std::size_t writeIndex = 0u;
  for (std::size_t readIndex = 0u;
       readIndex < readableCommands;
       ++readIndex) {
    if (io.commandOut[readIndex].kind !=
        trading_core::StrategyCommandIntentKind::Cancel) {
      continue;
    }
    if (writeIndex != readIndex) {
      io.commandOut[writeIndex] = io.commandOut[readIndex];
    }
    ++writeIndex;
  }
  io.commandCount = writeIndex;
}

void bindBacktestStrategyStateView(
    trading_core::StrategyContext& ctx) noexcept {
  ctx.state.orders.primary = ctx.orderState;
  ctx.state.orders.legs = ctx.orderStates;
  ctx.state.orders.primaryWarmup = ctx.orderPathWarmup;
  ctx.state.orders.warmups = ctx.orderPathWarmups;
  ctx.state.positions.primary = ctx.positionState;
  ctx.state.positions.legs = ctx.positionStates;
  ctx.state.account.primary = ctx.accountState;
  ctx.state.account.legs = ctx.accountStates;
  ctx.state.account.rateLimits = ctx.rateLimits;
  trading_core::bindStrategyContextStateAliases(ctx);
}

bool orderPathWarmupCandidate(const trading_core::OrderIntent& intent) noexcept {
  return !intent.service &&
         !trading_core::positionReductionRequested(intent.positionReduction) &&
         intent.clientIntentId.raw != 0u &&
         intent.qty.raw > 0 &&
         (intent.orderType == canon::OrderType::Market ||
          intent.orderType == canon::OrderType::Limit ||
          intent.orderType == canon::OrderType::Stop);
}

bool warmupLegSeen(const std::size_t* legs,
                   std::size_t count,
                   std::size_t leg) noexcept {
  for (std::size_t i = 0u; i < count; ++i) {
    if (legs[i] == leg) return true;
  }
  return false;
}

bool warmupReasonFailOpen(trading_core::strategy::OrderPathWarmupSkipReason reason) noexcept {
  return reason == trading_core::strategy::OrderPathWarmupSkipReason::BadTemplate ||
         reason == trading_core::strategy::OrderPathWarmupSkipReason::MissingMarketData ||
         reason == trading_core::strategy::OrderPathWarmupSkipReason::MissingInstrument ||
         reason == trading_core::strategy::OrderPathWarmupSkipReason::BadQtyGrid ||
         reason == trading_core::strategy::OrderPathWarmupSkipReason::QtyTooSmall ||
         reason == trading_core::strategy::OrderPathWarmupSkipReason::BadPrice ||
         reason == trading_core::strategy::OrderPathWarmupSkipReason::BadClientId;
}

trading_core::StrategyOrderPathWarmupView backtestWarmupView(const detail::EngineState& engine) noexcept {
  trading_core::StrategyOrderPathWarmupView view{};
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
  const trading_core::StrategyOrderPathWarmupView view = backtestWarmupView(engine);
  return trading_core::strategy::orderPathWarmupHot(
      view,
      nowMono,
      trading_core::strategy::kDefaultOrderPathWarmupPolicy.heatTtlNs);
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
                               trading_core::strategy::OrderPathWarmupSkipReason reason) noexcept {
  engine.lastWarmupSkipReason = static_cast<std::uint8_t>(reason);
}

bool applyBacktestWarmupGate(trading_core::StrategyContext& ctx,
                             detail::EngineState* engines,
                             std::size_t engineCount,
                             const BacktestRunRequest& request) noexcept {
  if (!engines || engineCount == 0u || ctx.io.commandOut.size() == 0u) return false;
  std::size_t warmupLegs[trading_core::kStrategyRuntimeMaxLegs]{};
  trading_core::OrderIntent warmupTemplates[trading_core::kStrategyRuntimeMaxLegs]{};
  std::size_t warmupRouteCount = 0u;
  auto addWarmupTemplate = [&](const trading_core::OrderIntent& intent) noexcept {
    if (!orderPathWarmupCandidate(intent)) return;
    const std::size_t leg = routeLegIndex(ctx, intent.routeHandle);
    if (leg >= engineCount) return;
    if (!routeTradable(request, engineCount, leg)) return;
    if (backtestOrderPathHot(engines[leg], ctx.nowMono)) return;
    if (warmupLegSeen(warmupLegs, warmupRouteCount, leg)) return;
    if (warmupRouteCount >= trading_core::kStrategyRuntimeMaxLegs) return;
    warmupLegs[warmupRouteCount] = leg;
    warmupTemplates[warmupRouteCount] = intent;
    ++warmupRouteCount;
  };
  for (std::size_t i = 0u; i < ctx.io.orderCount; ++i) addWarmupTemplate(ctx.io.orderOut[i]);
  for (std::size_t i = 0u; i < ctx.io.commandCount; ++i) {
    const trading_core::StrategyCommandIntent& command = ctx.io.commandOut[i];
    if (command.kind == trading_core::StrategyCommandIntentKind::Order) addWarmupTemplate(command.order);
  }
  if (warmupRouteCount == 0u) return false;

  trading_core::StrategyCommandIntent warmupCommands[trading_core::kStrategyRuntimeMaxLegs]{};
  std::size_t warmupCommandCount = 0u;
  bool failOpen = false;
  bool holdWithoutNewWarmup = false;
  for (std::size_t i = 0u; i < warmupRouteCount; ++i) {
    const std::size_t leg = warmupLegs[i];
    if (leg >= engineCount) {
      failOpen = true;
      break;
    }
    const Id serviceId = allocateBacktestServiceClientIntentId(engines[leg]);
    const trading_core::strategy::OrderPathWarmupBuildResult build =
        trading_core::strategy::buildOrderPathWarmup(ctx, warmupTemplates[i], serviceId);
    if (build.built) {
      warmupCommands[warmupCommandCount] = trading_core::StrategyCommandIntent{};
      warmupCommands[warmupCommandCount].kind = trading_core::StrategyCommandIntentKind::WarmupOrder;
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

bool drainUserEvents(const trading_core::StrategyDescriptor& descriptor,
                     const BacktestStrategyInputPolicy& inputPolicy,
                     void* state,
                     detail::EngineState& engine,
                     const StrategyInstrumentSpec& instrument,
                     SingleLegCycleScratch& scratch,
                     const trading_core::SymbolShardConfig& shard,
                     const trading_core::ShardControlState& control,
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
    if (!runCycle(descriptor, inputPolicy, state, engine, instrument,
                  scratch, shard, control, tsNs, request, &event)) return false;
  }
  return true;
}

bool drainUserEventsMulti(const trading_core::StrategyDescriptor& descriptor,
                          const BacktestStrategyInputPolicy& inputPolicy,
                          void* state,
                          std::vector<detail::EngineState>& engines,
                          const std::vector<StrategyInstrumentSpec>& legInstruments,
                          MultiLegCycleScratch& scratch,
                          const trading_core::SymbolShardConfig& shard,
                          const trading_core::ShardControlState& control,
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
      if (!runCycleMulti(descriptor, inputPolicy, state, engines,
                         legInstruments, scratch, shard, control,
                         static_cast<std::uint32_t>(leg), tsNs, request,
                         &event)) return false;
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

const BacktestStrategyVenueConfig* venueByName(
    const BacktestStrategyConfig& cfg,
    const std::string& name) noexcept {
  for (const BacktestStrategyVenueConfig& candidate : cfg.venues) {
    if (candidate.name == name) return &candidate;
  }
  return nullptr;
}

bool venueMatchesSession(const BacktestStrategyVenueConfig& venue,
                         const detail::SessionData& session) noexcept {
  if (venue.exchange.raw != detail::exchangeFromText(session.exchange).raw) return false;
  if (venue.market.raw != detail::marketFromText(session.market).raw) return false;
  for (const Symbol& symbol : venue.symbols) {
    if (session.symbol == symbol.data) return true;
  }
  return false;
}

const BacktestStrategyVenueConfig* venueForSession(
    const BacktestStrategyConfig& cfg,
    const detail::SessionData& session) noexcept {
  for (const BacktestStrategyVenueConfig& candidate : cfg.venues) {
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
  markets[leg] = engine.market;
  revisions[leg] = engine.contextRevision;
}

void MultiLegCycleScratch::refreshDirty(const std::vector<detail::EngineState>& engines,
                                        const std::vector<StrategyInstrumentSpec>& legInstruments) noexcept {
  for (std::size_t leg = 0; leg < legCount; ++leg) {
    if (leg >= engines.size()) break;
    if (revisions[leg] != engines[leg].contextRevision) refreshLeg(leg, engines, legInstruments);
  }
}

trading_core::StrategyAccountState accountStateFromEngine(const detail::EngineState& engine) noexcept {
  trading_core::StrategyAccountState out{};
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
  return count < trading_core::kStrategyRuntimeMaxLegs
      ? count
      : trading_core::kStrategyRuntimeMaxLegs;
}

void refreshRiskAccountsFromEngines(
    const std::vector<detail::EngineState>& engines,
    std::array<trading_core::StrategyAccountState, trading_core::kStrategyRuntimeMaxLegs>& out) noexcept {
  out.fill(trading_core::StrategyAccountState{});
  const std::size_t count = boundedRiskLegCount(engines.size());
  for (std::size_t i = 0u; i < count; ++i) out[i] = accountStateFromEngine(engines[i]);
}

void resolveRiskVenuesForSessions(
    const BacktestStrategyConfig& cfg,
    const std::vector<detail::SessionData>& sessions,
    std::array<const BacktestStrategyVenueConfig*, trading_core::kStrategyRuntimeMaxLegs>& out) noexcept {
  out.fill(nullptr);
  const std::size_t count = boundedRiskLegCount(sessions.size());
  for (const BacktestPortfolioConfig& portfolio : cfg.portfolios) {
    if (portfolio.legs.size() != count) continue;
    bool matches = true;
    for (std::size_t i = 0u; i < count; ++i) {
      const BacktestStrategyVenueConfig* venue = venueByName(cfg, portfolio.legs[i].venue);
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
    out = trading_core::saturatingAddI64(
        out,
        trading_core::saturatingAddI64(
            engine.netRealizedPnlE8, detail::unrealizedPnlE8(engine)));
  }
  return out;
}

TimeNs timeNsFromI64(std::int64_t value) noexcept {
  TimeNs out{};
  if (value > 0) out.raw = static_cast<std::uint64_t>(value);
  return out;
}

TimeNs replayUtcFromMonotonic(std::int64_t monotonicNs,
                              std::int64_t utcOffsetNs) noexcept {
  TimeNs out{};
  const __int128 value = static_cast<__int128>(monotonicNs) +
      static_cast<__int128>(utcOffsetNs);
  if (value <= 0) return out;
  out.raw = value > std::numeric_limits<std::uint64_t>::max()
      ? std::numeric_limits<std::uint64_t>::max()
      : static_cast<std::uint64_t>(value);
  return out;
}

void applyMarketEventToSnapshots(detail::EngineState& engine,
                                 const detail::SessionData& session,
                                 const detail::TimelineEvent& event,
                                 const Symbol& selectedSymbol,
                                 const StrategyInstrumentSpec& instrument,
                                 bool strategyVisible) {
  trading_core::StrategyMarketSnapshot& snapshot = strategyVisible ? engine.snapshot : engine.venueSnapshot;
  trading_core::StrategyCandleSnapshot& candle = strategyVisible ? engine.candle : engine.venueCandle;
  trading_core::LocalOrderBookState& orderBook = strategyVisible ? engine.orderBook : engine.venueOrderBook;
  trading_core::BboReconstructionState& bbo = strategyVisible ? engine.bboReconstruction : engine.venueBboReconstruction;
  detail::ReconstructionSourceIdentity* identity = nullptr;
  detail::ReconstructionSourceIdentity* counterpart = nullptr;
  if (event.kind == detail::kTimelineKindBookTicker) {
    identity = strategyVisible
        ? &engine.reconstructionBookTickerIdentity
        : &engine.venueReconstructionBookTickerIdentity;
    counterpart = strategyVisible
        ? &engine.reconstructionTradeIdentity
        : &engine.venueReconstructionTradeIdentity;
  } else if (event.kind == detail::kTimelineKindTrade) {
    identity = strategyVisible
        ? &engine.reconstructionTradeIdentity
        : &engine.venueReconstructionTradeIdentity;
    counterpart = strategyVisible
        ? &engine.reconstructionBookTickerIdentity
        : &engine.venueReconstructionBookTickerIdentity;
  }
  if (identity) {
    const detail::ReconstructionSourceIdentity nextIdentity{
        .producerEpoch = event.producerEpoch,
        .sourceGeneration = event.sourceGeneration,
        .sessionEpoch = event.sessionEpoch,
        .sourceId = event.sourceId,
        .initialized = true,
    };
    const auto sameIdentity = [](const detail::ReconstructionSourceIdentity& left,
                                 const detail::ReconstructionSourceIdentity& right) noexcept {
      return left.initialized == right.initialized &&
          left.producerEpoch == right.producerEpoch &&
          left.sourceGeneration == right.sourceGeneration &&
          left.sessionEpoch == right.sessionEpoch &&
          left.sourceId == right.sourceId;
    };
    const bool changed = identity->initialized &&
        !sameIdentity(*identity, nextIdentity);
    const bool counterpartAlreadyCurrent =
        counterpart && counterpart->initialized &&
        sameIdentity(*counterpart, nextIdentity);
    *identity = nextIdentity;
    if (changed && !counterpartAlreadyCurrent) {
      trading_core::resetBboReconstruction(bbo);
      trading_core::clearBookTickerSnapshot(snapshot);
      trading_core::copyReconstructedBboSnapshot(
          snapshot, trading_core::BboReconstructionView{});
      const trading_core::LocalOrderBookConfig config = orderBook.config;
      const Symbol symbol = orderBook.symbol;
      orderBook.reset();
      orderBook.config = config;
      orderBook.symbol = symbol;
      trading_core::clearOrderBookSnapshot(snapshot);
    }
  }
  const std::int64_t replayReceiveMonotonic =
      strategyVisible ? event.deliveryTsNs : event.tsNs;
  // Synthetic execution is sequenced on replay delivery, the same causal
  // clock used to submit and activate simulated orders. Exchange timestamps
  // are market payload evidence and may be in a completely different epoch.
  const std::int64_t executionTsNs =
      event.deliveryTsNs > 0 ? event.deliveryTsNs : event.tsNs;
  const std::uint64_t replayReceiveMonotonicNs =
      static_cast<std::uint64_t>(
          replayReceiveMonotonic > 0 ? replayReceiveMonotonic : 0);
  bool changed = false;
  if (event.kind == detail::kTimelineKindDepth) {
    const detail::DepthRow& depth = session.depths[event.index];
    if (depth.resetBookBeforeApply) {
      const trading_core::LocalOrderBookConfig config = orderBook.config;
      const Symbol symbol = orderBook.symbol;
      orderBook.reset();
      orderBook.config = config;
      orderBook.symbol = symbol;
    }
    cxet::composite::OrderBookTapeRuntimeV1 tape{};
    cxet::composite::OrderBookTapeSidesRuntimeV1 sides{};
    detail::toRuntime(depth, tape, sides);
    trading_core::copyDepthTimestamp(
        snapshot, cxet::composite::orderBookTapeTimestamp(tape));
    if (orderBook.initialized.raw != trading_core::kAlgoFlagTrue.raw) {
      orderBook.symbol = selectedSymbol;
      if (orderBook.config.maxStoredLevelsPerSide.raw == 0u) {
        orderBook.config.maxStoredLevelsPerSide =
            orderBook.capacityLevelsPerSide();
      }
      orderBook.config.tickSizeRaw = instrument.tickSizeRaw;
    }
    (void)trading_core::applyTapeDelta(
        orderBook, tape, sides);
    overlayBacktestReconstructedBbo(
        snapshot, orderBook,
        trading_core::bboReconstructionView(bbo));
    trading_core::bindOrderBookSnapshot(snapshot, orderBook);
    changed = true;
  } else if (event.kind == detail::kTimelineKindTrade) {
    const auto& row = session.trades[event.index];
    const auto trade = detail::toRuntime(row);
    trading_core::copyTradeSnapshot(snapshot, trade);
    const bool reconstructionApplied =
        trading_core::applyBboReconstructionKnownInitiatorTrade(
        bbo, trade,
        reconstructionTimestampOrigin(row.arrival, row.tsNs));
    if (reconstructionApplied) {
      publishBacktestReconstructedBbo(
          snapshot, orderBook, bbo, replayReceiveMonotonicNs);
    }
    if (!strategyVisible) detail::tryFillOnTrade(engine, trade, executionTsNs);
    changed = true;
  } else if (event.kind == detail::kTimelineKindBookTicker) {
    const auto& row = session.bookTickers[event.index];
    if (row.freshnessOnly) {
      if (strategyVisible) {
        refreshCapturedBookTicker(snapshot, event);
        changed = true;
      }
    } else {
      const auto ticker = detail::toRuntime(row);
      const bool reconstructionApplied = applyCapturedBookTicker(
          snapshot, bbo, row, event,
          replayReceiveMonotonicNs, ticker);
      if (reconstructionApplied) {
        publishBacktestReconstructedBbo(
            snapshot, orderBook, bbo, replayReceiveMonotonicNs);
      }
      trading_core::bindOrderBookSnapshot(snapshot, orderBook);
      if (!strategyVisible) {
        detail::tryFillOnTop(engine, executionTsNs, true);
      }
      changed = true;
    }
  } else if (event.kind == detail::kTimelineKindPriceLimit) {
    const auto priceLimit = detail::toRuntime(session.priceLimits[event.index]);
    trading_core::copyPriceLimitSnapshot(snapshot, priceLimit);
    changed = true;
  } else if (event.kind == detail::kTimelineKindLiquidation) {
    const auto liquidation = detail::toRuntime(session.liquidations[event.index], engine.exchange, engine.symbol);
    trading_core::copyLiquidationSnapshot(snapshot, liquidation);
    changed = true;
  } else if (event.kind == detail::kTimelineKindMarkPrice) {
    const auto markPrice = detail::toRuntime(session.markPrices[event.index]);
    trading_core::copyMarkPriceSnapshot(snapshot, markPrice);
    if (!strategyVisible) {
      detail::tryTriggerStopMarketsOnMark(engine, markPrice, executionTsNs);
    }
    changed = true;
  } else if (event.kind == detail::kTimelineKindIndexPrice) {
    const auto indexPrice = detail::toRuntime(session.indexPrices[event.index]);
    trading_core::copyIndexPriceSnapshot(snapshot, indexPrice);
    if (!strategyVisible) {
      detail::tryTriggerStopMarketsOnIndex(engine, indexPrice, executionTsNs);
    }
    changed = true;
  } else if (event.kind == detail::kTimelineKindFunding) {
    const auto funding = detail::toRuntime(session.funding[event.index]);
    trading_core::copyFundingSnapshot(snapshot, funding);
    changed = true;
  } else if (event.kind == detail::kTimelineKindOpenInterest) {
    const auto openInterest = detail::toRuntime(session.openInterest[event.index]);
    trading_core::copyOpenInterestSnapshot(snapshot, openInterest);
    changed = true;
  } else if (event.kind == detail::kTimelineKindCandle) {
    candle = detail::toRuntime(session.candles[event.index]);
    changed = true;
  }
  if (changed) {
    if (strategyVisible) {
      recordBacktestStrategySourceDelivery(engine, event);
    }
    detail::bumpContextRevision(engine);
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

bool runCycle(const trading_core::StrategyDescriptor& descriptor,
              const BacktestStrategyInputPolicy& inputPolicy,
              void* state,
              detail::EngineState& engine,
              const StrategyInstrumentSpec& instrument,
              SingleLegCycleScratch& scratch,
              const trading_core::SymbolShardConfig& shard,
              const trading_core::ShardControlState& control,
              std::int64_t tsNs,
              const BacktestRunRequest& request,
              const cxet::composite::UserDataEvent* userEvent) {
  if (!footprintSupported(descriptor)) return false;
  if (engine.liquidated || engine.riskStopped) return true;
  if (!backtestStrategySnapshotReady(inputPolicy, engine.snapshot)) return true;
  const std::uint64_t configuredTradableMask = backtestTradableLegMask(request, 1u);
  const std::uint64_t sourceFreshMask =
      backtestSourceFreshLegMask(inputPolicy, &engine, 1u, tsNs);
  const trading_core::strategy::StrategyInputSourceMaskState freshness =
      trading_core::strategy::strategyInputSourceMaskState(1u, configuredTradableMask, sourceFreshMask);
  const trading_core::ShardControlState effectiveControl =
      backtestControlWithInputFreshness(control, freshness);
  const std::size_t orderViewCount = detail::visibleOrders(engine, scratch.orderStorage.data(), scratch.orderStorage.size());
  trading_core::StrategyContext ctx{};
  ctx.shard = &shard;
  ctx.control = &effectiveControl;
  ctx.snapshot = &engine.snapshot;
  ctx.marketData.snapshots = &engine.snapshot;
  ctx.marketData.count = 1;
  ctx.candle = engine.candle;
  ctx.candles.candles = &engine.candle;
  ctx.candles.count = 1;
  ctx.instrumentSpec = instrument;
  ctx.instrumentSpecs.legs = &instrument;
  ctx.instrumentSpecs.count = 1u;
  ctx.legMarkets.legs = &engine.market;
  ctx.legMarkets.count = 1u;
  ctx.orderState.orders = scratch.orderStorage.data();
  ctx.orderState.count = orderViewCount;
  ctx.orderStates.legs = &ctx.orderState;
  ctx.orderStates.count = 1u;
  ctx.positionState = engine.position;
  ctx.positionStates.legs = &ctx.positionState;
  ctx.positionStates.count = 1u;
  ctx.accountState = accountStateFromEngine(engine);
  ctx.accountStates.legs = &ctx.accountState;
  ctx.accountStates.count = 1u;
  setBacktestTradePolicy(ctx, request, 1u, freshness.tradableLegMask);
  ctx.userEvent = userEvent;
  // Admission is owned by the simulated Backtest runtime; this is not a
  // generic classifier for exchange wire events.
  ctx.userEventAdmittedSemanticMask =
      backtestSimulatedUserEventSemanticMask(userEvent);
  ctx.hasUserEvent = ctx.userEventAdmittedSemanticMask != 0u;
  ctx.nowMono.raw = static_cast<std::uint64_t>(tsNs > 0 ? tsNs : 0);
  ctx.nowUtc = replayUtcFromMonotonic(tsNs, engine.replayUtcOffsetNs);
  if (tsNs > engine.lastReplayMonotonicNs) engine.lastReplayMonotonicNs = tsNs;
  scratch.warmupViews[0] = backtestWarmupView(engine);
  ctx.orderPathWarmup = scratch.warmupViews[0];
  ctx.orderPathWarmups.routes = scratch.warmupViews.data();
  ctx.orderPathWarmups.count = 1u;
  if (userEvent) {
    recordExecutionQualityFill(engine, *userEvent, 0u, tsNs, instrument);
  }
  if (userEvent && userEvent->orderProblem != canon::OrderProblem::None) {
    engine.pendingProblem = trading_core::StrategyProblemEvent{};
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
    ctx.rateLimits = engine.rateLimits.snapshotBook(trading_core::kDefaultExecutionRouteHandle,
                                                    engine.exchange,
                                                    engine.market,
                                                    1u,
                                                    ctx.nowMono);
    engine.rateLimitSnapshotRequested = false;
  }
  bindBacktestStrategyStateView(ctx);
  ctx.userEventRecvMono = ctx.nowMono;
  ctx.userEventRecvUtc = ctx.nowUtc;
  ctx.initialOrderSeedAllowed = true;
  const trading_core::StrategyRuntimeFootprint& footprint =
      descriptor.footprint;
  ctx.io.orderOut = Span<trading_core::OrderIntent>(
      scratch.orderBuf.data(), footprint.maxOrders);
  ctx.io.cancelOut = Span<trading_core::CancelIntent>(
      scratch.cancelBuf.data(), footprint.maxCancels);
  ctx.io.desiredOrderOut = Span<trading_core::StrategyDesiredOrderSlot>(
      scratch.desiredOrderBuf.data(), footprint.maxDesiredOrders);
  ctx.io.commandOut = Span<trading_core::StrategyCommandIntent>(
      scratch.commandBuf.data(),
      trading_core::strategyRuntimeCommandCapacity(footprint));
  ctx.io.transactionOut = Span<trading_core::StrategyTransactionIntent>(
      scratch.transactionBuf.data(), footprint.maxTransactions);
  ctx.io.clear();
  recordInputCoverage(engine);
  trading_core::StrategyDecisionTrace trace{};
  trading_core::StrategyExecutionTraceBatch executionTrace{};
  const bool captureIndicatorTrace = strategyIndicatorEnabled(engine);
  const bool captureRangeTrace = emitsPriceRangeTrace(descriptor);
  const bool captureSpreadDiagnostics = emitsArbSpreadTrace(descriptor);
  const bool captureSpreadRows = request.captureStrategySpread && captureSpreadDiagnostics;
  if (captureIndicatorTrace || captureRangeTrace || captureSpreadDiagnostics) {
    trading_core::initStrategyDecisionTrace(trace, ctx.nowMono.raw);
    ctx.decisionTrace = &trace;
  }
  if (captureSpreadDiagnostics) {
    executionTrace.clear();
    ctx.executionTrace = &executionTrace;
  }
  const bool canOrderFailureRollback =
      descriptor.captureState && descriptor.restoreAfterOrderFailure;
  const bool canCapacityRollback =
      descriptor.captureState && descriptor.rollbackCapacityCycle;
  trading_core::StrategyStateSnapshot before{};
  if (canOrderFailureRollback || canCapacityRollback) {
    descriptor.captureState(state, before);
  }
  const bool ok = descriptor.runCycle(state, ctx);
  if (ctx.io.capacityExceeded) {
    if (canCapacityRollback) {
      descriptor.rollbackCapacityCycle(state, before);
    }
    ctx.io.clear();
    return false;
  }
  if (!ok) {
    ctx.io.clear();
    return false;
  }
  if (!lowerDesiredOrders(ctx)) {
    if (ctx.io.capacityExceeded && canCapacityRollback) {
      descriptor.rollbackCapacityCycle(state, before);
    } else if (canOrderFailureRollback) {
      descriptor.restoreAfterOrderFailure(state, before);
    } else if (canCapacityRollback) {
      descriptor.rollbackCapacityCycle(state, before);
    }
    ctx.io.clear();
    return false;
  }
  const bool transactionCycle = ctx.io.transactionCount != 0u;
  if (!bindTransactionLegs(ctx)) {
    if (canOrderFailureRollback) {
      descriptor.restoreAfterOrderFailure(state, before);
    } else if (canCapacityRollback) {
      descriptor.rollbackCapacityCycle(state, before);
    }
    ctx.io.clear();
    return false;
  }
  if (ctx.io.requestRateLimitSnapshot) engine.rateLimitSnapshotRequested = true;
  const bool newOrderEmissionBlocked =
      !trading_core::canEmitNewOrder(effectiveControl);
  const strategy_intent_lowering::StrategyTransactionAdmission admission =
      transactionCycle
      ? transactionAdmission(ctx, 1u, request, newOrderEmissionBlocked)
      : strategy_intent_lowering::StrategyTransactionAdmission{};
  if (newOrderEmissionBlocked && !transactionCycle) {
    retainCancelOutputsWhenNewOrdersBlocked(ctx.io);
  }
  const bool warmupGateHeldRealOrders = !newOrderEmissionBlocked &&
      canOrderFailureRollback &&
      applyBacktestWarmupGate(ctx, &engine, 1u, request);
  if (warmupGateHeldRealOrders && canOrderFailureRollback) {
    descriptor.restoreAfterOrderFailure(state, before);
    if (transactionCycle) ctx.io.transactionCount = 0u;
  }
  if (captureIndicatorTrace || captureRangeTrace || captureSpreadDiagnostics) {
    std::uint32_t orderedOrderCount = 0u;
    std::uint32_t orderedCancelCount = 0u;
    for (std::size_t i = 0u; i < ctx.io.commandCount; ++i) {
      if (scratch.commandBuf[i].kind == trading_core::StrategyCommandIntentKind::Order ||
          scratch.commandBuf[i].kind == trading_core::StrategyCommandIntentKind::WarmupOrder) ++orderedOrderCount;
      else if (scratch.commandBuf[i].kind == trading_core::StrategyCommandIntentKind::Cancel) ++orderedCancelCount;
    }
    trace.orderCount = static_cast<std::uint32_t>(ctx.io.orderCount) + orderedOrderCount;
    trace.cancelCount = static_cast<std::uint32_t>(ctx.io.cancelCount) + orderedCancelCount;
    if (captureIndicatorTrace) appendStrategyIndicator(engine, tsNs, trace);
    if (captureRangeTrace) appendStrategyRange(engine, tsNs, trace);
    if (captureSpreadDiagnostics) recordStrategySpread(engine, tsNs, trace, captureSpreadRows);
  }
  if (ctx.executionTrace) recordExecutionQualityActions(engine, tsNs, executionTrace);
  if (ctx.io.transactionCount != 0u) {
    if (!applyTransactionCycle(
            descriptor, state, ctx,
            Span<detail::EngineState>(&engine, 1u),
            tsNs, admission)) return false;
  } else {
    applyToSingleLeg(engine, ctx, tsNs, engine.latency, request);
  }
  return true;
}

bool runCycleMulti(const trading_core::StrategyDescriptor& descriptor,
                   const BacktestStrategyInputPolicy& inputPolicy,
                   void* state,
                   std::vector<detail::EngineState>& engines,
                   const std::vector<StrategyInstrumentSpec>& legInstruments,
                   MultiLegCycleScratch& scratch,
                   const trading_core::SymbolShardConfig& shard,
                   const trading_core::ShardControlState& control,
                   std::uint32_t eventLegIndex,
                   std::int64_t tsNs,
                   const BacktestRunRequest& request,
                   const cxet::composite::UserDataEvent* userEvent) {
  if (!footprintSupported(descriptor)) return false;
  if (engines.empty()) return true;
  for (const detail::EngineState& engine : engines) {
    if (engine.liquidated || engine.riskStopped) return true;
  }
  const std::size_t legCount = engines.size() < trading_core::kStrategyRuntimeMaxLegs ? engines.size() : trading_core::kStrategyRuntimeMaxLegs;
  const std::uint64_t configuredTradableMask = backtestTradableLegMask(request, legCount);
  const std::uint64_t sourceFreshMask = backtestSourceFreshLegMask(
      inputPolicy, engines.data(), legCount, tsNs);
  const trading_core::strategy::StrategyInputSourceMaskState freshness =
      trading_core::strategy::strategyInputSourceMaskState(legCount, configuredTradableMask, sourceFreshMask);
  std::uint64_t readinessMask = freshness.sourceFreshLegMask;
  if (!freshness.globallyDegraded && legCount <= 2u) {
    readinessMask = trading_core::strategy::strategyInputLegMaskForCount(legCount);
  } else if (freshness.globallyDegraded && readinessMask == 0u) {
    readinessMask = trading_core::strategy::strategyInputLegMaskForCount(legCount);
  }
  for (std::size_t i = 0u; i < legCount; ++i) {
    if ((readinessMask & (std::uint64_t{1} << i)) == 0u) continue;
    if (!backtestStrategySnapshotReady(inputPolicy, engines[i].snapshot)) return true;
  }
  const trading_core::ShardControlState effectiveControl =
      backtestControlWithInputFreshness(control, freshness);
  if (scratch.legCount != legCount) scratch.reset(legCount);
  scratch.refreshDirty(engines, legInstruments);
  trading_core::StrategyContext ctx{};
  ctx.shard = &shard;
  ctx.control = &effectiveControl;
  ctx.marketData.snapshots = scratch.snapshots.data();
  ctx.marketData.count = legCount;
  ctx.candles.candles = scratch.candles.data();
  ctx.candles.count = legCount;
  ctx.instrumentSpecs.legs = scratch.instruments.data();
  ctx.instrumentSpecs.count = legCount;
  ctx.legMarkets.legs = scratch.markets.data();
  ctx.legMarkets.count = legCount;
  ctx.orderStates.legs = scratch.orderViews.data();
  ctx.orderStates.count = legCount;
  ctx.positionStates.legs = scratch.positions.data();
  ctx.positionStates.count = legCount;
  ctx.accountStates.legs = scratch.accounts.data();
  ctx.accountStates.count = legCount;
  setBacktestTradePolicy(ctx, request, legCount, freshness.tradableLegMask);
  const std::size_t defaultExecutionLeg =
      routeLegIndex(ctx, trading_core::kDefaultExecutionRouteHandle);
  if (defaultExecutionLeg >= legCount ||
      !trading_core::strategy::bindDefaultExecutionMarketAliases(ctx)) {
    return false;
  }
  ctx.instrumentSpec = scratch.instruments[defaultExecutionLeg];
  ctx.orderState = scratch.orderViews[defaultExecutionLeg];
  ctx.positionState = scratch.positions[defaultExecutionLeg];
  ctx.accountState = scratch.accounts[defaultExecutionLeg];
  ctx.userEvent = userEvent;
  // Admission is owned by the simulated Backtest runtime; this is not a
  // generic classifier for exchange wire events.
  ctx.userEventAdmittedSemanticMask =
      backtestSimulatedUserEventSemanticMask(userEvent);
  ctx.hasUserEvent = ctx.userEventAdmittedSemanticMask != 0u;
  ctx.eventLegIndex = eventLegIndex;
  ctx.hasEventLeg = eventLegIndex < legCount;
  ctx.nowMono.raw = static_cast<std::uint64_t>(tsNs > 0 ? tsNs : 0);
  ctx.nowUtc = replayUtcFromMonotonic(tsNs, engines[0].replayUtcOffsetNs);
  for (std::size_t i = 0u; i < legCount; ++i) {
    if (tsNs > engines[i].lastReplayMonotonicNs)
      engines[i].lastReplayMonotonicNs = tsNs;
  }
  for (std::size_t i = 0u; i < legCount; ++i) {
    scratch.warmupViews[i] = backtestWarmupView(engines[i]);
  }
  ctx.orderPathWarmup = scratch.warmupViews[defaultExecutionLeg];
  ctx.orderPathWarmups.routes = scratch.warmupViews.data();
  ctx.orderPathWarmups.count = legCount;
  if (userEvent && eventLegIndex < legInstruments.size()) {
    recordExecutionQualityFill(engines[0], *userEvent, eventLegIndex, tsNs, legInstruments[eventLegIndex]);
  }
  if (userEvent && userEvent->orderProblem != canon::OrderProblem::None && eventLegIndex < engines.size()) {
    engines[eventLegIndex].pendingProblem = trading_core::StrategyProblemEvent{};
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
    ctx.rateLimits = trading_core::StrategyRateLimitBook{};
    for (std::size_t i = 0u; i < legCount; ++i) {
      trading_core::StrategyRateLimitBook legLimits = engines[i].rateLimits.snapshotBook(
          trading_core::ExecutionRouteHandle{static_cast<std::uint32_t>(i + 1u)},
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
  bindBacktestStrategyStateView(ctx);
  ctx.userEventRecvMono = ctx.nowMono;
  ctx.userEventRecvUtc = ctx.nowUtc;
  ctx.initialOrderSeedAllowed = true;
  const trading_core::StrategyRuntimeFootprint& footprint =
      descriptor.footprint;
  ctx.io.orderOut = Span<trading_core::OrderIntent>(
      scratch.orderBuf.data(), footprint.maxOrders);
  ctx.io.cancelOut = Span<trading_core::CancelIntent>(
      scratch.cancelBuf.data(), footprint.maxCancels);
  ctx.io.desiredOrderOut = Span<trading_core::StrategyDesiredOrderSlot>(
      scratch.desiredOrderBuf.data(), footprint.maxDesiredOrders);
  ctx.io.commandOut = Span<trading_core::StrategyCommandIntent>(
      scratch.commandBuf.data(),
      trading_core::strategyRuntimeCommandCapacity(footprint));
  ctx.io.transactionOut = Span<trading_core::StrategyTransactionIntent>(
      scratch.transactionBuf.data(), footprint.maxTransactions);
  ctx.io.clear();
  recordInputCoverage(engines[0]);
  trading_core::StrategyDecisionTrace trace{};
  trading_core::StrategyExecutionTraceBatch executionTrace{};
  const bool captureIndicatorTrace = strategyIndicatorEnabled(engines[0]);
  const bool captureRangeTrace = emitsPriceRangeTrace(descriptor);
  const bool captureSpreadDiagnostics = emitsArbSpreadTrace(descriptor);
  const bool captureSpreadRows = request.captureStrategySpread && captureSpreadDiagnostics;
  if (captureIndicatorTrace || captureRangeTrace || captureSpreadDiagnostics) {
    trading_core::initStrategyDecisionTrace(trace, ctx.nowMono.raw);
    ctx.decisionTrace = &trace;
  }
  if (captureSpreadDiagnostics) {
    executionTrace.clear();
    ctx.executionTrace = &executionTrace;
  }
  const bool canOrderFailureRollback =
      descriptor.captureState && descriptor.restoreAfterOrderFailure;
  const bool canCapacityRollback =
      descriptor.captureState && descriptor.rollbackCapacityCycle;
  trading_core::StrategyStateSnapshot before{};
  if (canOrderFailureRollback || canCapacityRollback) {
    descriptor.captureState(state, before);
  }
  const bool ok = descriptor.runCycle(state, ctx);
  if (ctx.io.capacityExceeded) {
    if (canCapacityRollback) {
      descriptor.rollbackCapacityCycle(state, before);
    }
    ctx.io.clear();
    return false;
  }
  if (!ok) {
    ctx.io.clear();
    return false;
  }
  if (!lowerDesiredOrders(ctx)) {
    if (ctx.io.capacityExceeded && canCapacityRollback) {
      descriptor.rollbackCapacityCycle(state, before);
    } else if (canOrderFailureRollback) {
      descriptor.restoreAfterOrderFailure(state, before);
    } else if (canCapacityRollback) {
      descriptor.rollbackCapacityCycle(state, before);
    }
    ctx.io.clear();
    return false;
  }
  const bool transactionCycle = ctx.io.transactionCount != 0u;
  if (!bindTransactionLegs(ctx)) {
    if (canOrderFailureRollback) {
      descriptor.restoreAfterOrderFailure(state, before);
    } else if (canCapacityRollback) {
      descriptor.rollbackCapacityCycle(state, before);
    }
    ctx.io.clear();
    return false;
  }
  if (ctx.io.requestRateLimitSnapshot) {
    for (detail::EngineState& engine : engines) engine.rateLimitSnapshotRequested = true;
  }
  const bool newOrderEmissionBlocked =
      !trading_core::canEmitNewOrder(effectiveControl);
  const strategy_intent_lowering::StrategyTransactionAdmission admission =
      transactionCycle
      ? transactionAdmission(
            ctx, legCount, request, newOrderEmissionBlocked)
      : strategy_intent_lowering::StrategyTransactionAdmission{};
  if (newOrderEmissionBlocked && !transactionCycle) {
    retainCancelOutputsWhenNewOrdersBlocked(ctx.io);
  }
  const bool warmupGateHeldRealOrders = !newOrderEmissionBlocked &&
      canOrderFailureRollback &&
      applyBacktestWarmupGate(ctx, engines.data(), legCount, request);
  if (warmupGateHeldRealOrders && canOrderFailureRollback) {
    descriptor.restoreAfterOrderFailure(state, before);
    if (transactionCycle) ctx.io.transactionCount = 0u;
  }
  if (captureIndicatorTrace || captureRangeTrace || captureSpreadDiagnostics) {
    std::uint32_t orderedOrderCount = 0u;
    std::uint32_t orderedCancelCount = 0u;
    for (std::size_t i = 0u; i < ctx.io.commandCount; ++i) {
      if (scratch.commandBuf[i].kind == trading_core::StrategyCommandIntentKind::Order ||
          scratch.commandBuf[i].kind == trading_core::StrategyCommandIntentKind::WarmupOrder) ++orderedOrderCount;
      else if (scratch.commandBuf[i].kind == trading_core::StrategyCommandIntentKind::Cancel) ++orderedCancelCount;
    }
    trace.orderCount = static_cast<std::uint32_t>(ctx.io.orderCount) + orderedOrderCount;
    trace.cancelCount = static_cast<std::uint32_t>(ctx.io.cancelCount) + orderedCancelCount;
    if (captureIndicatorTrace) appendStrategyIndicator(engines[0], tsNs, trace);
    if (captureRangeTrace) appendStrategyRange(engines[0], tsNs, trace);
    if (captureSpreadDiagnostics) recordStrategySpread(engines[0], tsNs, trace, captureSpreadRows);
  }
  if (ctx.executionTrace) recordExecutionQualityActions(engines[0], tsNs, executionTrace);
  if (ctx.io.transactionCount != 0u) {
    if (!applyTransactionCycle(
            descriptor, state, ctx,
            Span<detail::EngineState>(engines.data(), legCount),
            tsNs, admission)) return false;
  } else {
    applyToLegs(
        Span<detail::EngineState>(engines.data(), legCount),
        ctx, tsNs, request);
  }
  return true;
}

bool runUntil(const trading_core::StrategyDescriptor& descriptor,
              const BacktestStrategyInputPolicy& inputPolicy,
              void* state,
              detail::EngineState& engine,
              const StrategyInstrumentSpec& instrument,
              SingleLegCycleScratch& scratch,
              const trading_core::SymbolShardConfig& shard,
              const trading_core::ShardControlState& control,
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
      if (!drainUserEvents(descriptor, inputPolicy, state, engine,
                           instrument, scratch, shard, control, request,
                           userTs)) return false;
    } else if (pendingTs <= wakeTs && pendingTs <= cooldownTs) {
      detail::applyDueActions(engine, pendingTs);
    } else if (wakeTs <= cooldownTs) {
      ++engine.timerWakeEvents;
      if (!runCycle(descriptor, inputPolicy, state, engine, instrument,
                    scratch, shard, control, wakeTs, request, nullptr)) return false;
    } else {
      clearRateLimitCooldown(engine, cooldownTs);
      ++engine.timerWakeEvents;
      if (!runCycle(descriptor, inputPolicy, state, engine, instrument,
                    scratch, shard, control, cooldownTs, request,
                    nullptr)) return false;
    }
  }
  return drainUserEvents(descriptor, inputPolicy, state, engine, instrument,
                         scratch, shard, control, request, untilTs, boundary);
}

bool runUntilMulti(const trading_core::StrategyDescriptor& descriptor,
                   const BacktestStrategyInputPolicy& inputPolicy,
                   void* state,
                   std::vector<detail::EngineState>& engines,
                   const std::vector<StrategyInstrumentSpec>& legInstruments,
                   MultiLegCycleScratch& scratch,
                   const trading_core::SymbolShardConfig& shard,
                   const trading_core::ShardControlState& control,
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
      if (!drainUserEventsMulti(descriptor, inputPolicy, state, engines,
                                legInstruments, scratch, shard, control,
                                request, userTs)) return false;
    } else if (pendingTs <= wakeTs && pendingTs <= cooldownTs) {
      for (detail::EngineState& engine : engines) detail::applyDueActions(engine, pendingTs);
    } else if (wakeTs <= cooldownTs) {
      if (!engines.empty()) ++engines[0].timerWakeEvents;
      if (!runCycleMulti(descriptor, inputPolicy, state, engines,
                         legInstruments, scratch, shard, control, 0u,
                         wakeTs, request, nullptr)) return false;
    } else {
      clearRateLimitCooldown(engines, cooldownTs);
      if (!engines.empty()) ++engines[0].timerWakeEvents;
      if (!runCycleMulti(descriptor, inputPolicy, state, engines,
                         legInstruments, scratch, shard, control, 0u,
                         cooldownTs, request, nullptr)) return false;
    }
  }
  return drainUserEventsMulti(descriptor, inputPolicy, state, engines,
                              legInstruments, scratch, shard, control,
                              request, untilTs, boundary);
}

bool settleAfterReplay(const trading_core::StrategyDescriptor& descriptor,
                       const BacktestStrategyInputPolicy& inputPolicy,
                       void* state,
                       detail::EngineState& engine,
                       const StrategyInstrumentSpec& instrument,
                       SingleLegCycleScratch& scratch,
                       const trading_core::SymbolShardConfig& shard,
                       const trading_core::ShardControlState& control,
                       std::int64_t lastTsNs,
                       const BacktestRunRequest& request) {
  std::int64_t untilTs = lastTsNs > 0 ? detail::delayedTs(lastTsNs, maxLatencyUs(request)) : lastTsNs;
  if (!runUntil(descriptor, inputPolicy, state, engine, instrument, scratch,
                shard, control, untilTs, request)) return false;
  for (std::uint32_t i = 0u; i < 32u; ++i) {
    const std::int64_t wakeTs = nextWakeTs(descriptor, state);
    if (wakeTs == std::numeric_limits<std::int64_t>::max()) break;
    untilTs = wakeTs;
    if (!runUntil(descriptor, inputPolicy, state, engine, instrument,
                  scratch, shard, control, untilTs, request)) return false;
    untilTs = detail::delayedTs(untilTs, maxLatencyUs(request));
    if (!runUntil(descriptor, inputPolicy, state, engine, instrument,
                  scratch, shard, control, untilTs, request)) return false;
    if (engine.netQtyRaw == 0 && engine.pending.empty() && nextWakeTs(descriptor, state) == std::numeric_limits<std::int64_t>::max()) break;
  }
  trading_core::ShardControlState teardownControl = control;
  teardownControl.killActive = trading_core::kAlgoFlagTrue;
  for (std::uint32_t i = 0u; i < 8u && (engine.netQtyRaw != 0 || !engine.pending.empty()); ++i) {
    untilTs = detail::delayedTs(untilTs, maxLatencyUs(request));
    if (!runUntil(descriptor, inputPolicy, state, engine, instrument,
                  scratch, shard, teardownControl, untilTs, request)) return false;
    if (engine.netQtyRaw != 0) {
      if (!detail::forceClosePosition(engine, untilTs, engine.latency)) return false;
      untilTs = detail::delayedTs(untilTs, maxLatencyUs(request));
      if (!runUntil(descriptor, inputPolicy, state, engine, instrument,
                    scratch, shard, teardownControl, untilTs,
                    request)) return false;
    }
    detail::cancelOpenOrders(engine, untilTs, engine.latency);
  }
  detail::cancelOpenOrders(engine, untilTs, engine.latency);
  untilTs = detail::delayedTs(untilTs, maxLatencyUs(request));
  if (!runUntil(descriptor, inputPolicy, state, engine, instrument, scratch,
                shard, teardownControl, untilTs, request)) return false;
  return engine.netQtyRaw == 0 && engine.pending.empty();
}

bool settleAfterReplayMulti(const trading_core::StrategyDescriptor& descriptor,
                            const BacktestStrategyInputPolicy& inputPolicy,
                            void* state,
                            std::vector<detail::EngineState>& engines,
                            const std::vector<StrategyInstrumentSpec>& legInstruments,
                            MultiLegCycleScratch& scratch,
                            const trading_core::SymbolShardConfig& shard,
                            const trading_core::ShardControlState& control,
                            std::int64_t lastTsNs,
                            const BacktestRunRequest& request) {
  std::int64_t untilTs = lastTsNs > 0 ? detail::delayedTs(lastTsNs, maxLatencyUs(request)) : lastTsNs;
  if (!runUntilMulti(descriptor, inputPolicy, state, engines,
                     legInstruments, scratch, shard, control, untilTs,
                     request)) return false;
  for (std::uint32_t i = 0u; i < 32u; ++i) {
    const std::int64_t wakeTs = nextWakeTs(descriptor, state);
    if (wakeTs == std::numeric_limits<std::int64_t>::max()) break;
    untilTs = wakeTs;
    if (!runUntilMulti(descriptor, inputPolicy, state, engines,
                       legInstruments, scratch, shard, control, untilTs,
                       request)) return false;
    untilTs = detail::delayedTs(untilTs, maxLatencyUs(request));
    if (!runUntilMulti(descriptor, inputPolicy, state, engines,
                       legInstruments, scratch, shard, control, untilTs,
                       request)) return false;
    if (!portfolioHasOpenState(engines) && nextWakeTs(descriptor, state) == std::numeric_limits<std::int64_t>::max()) break;
  }
  trading_core::ShardControlState teardownControl = control;
  teardownControl.killActive = trading_core::kAlgoFlagTrue;
  for (std::uint32_t i = 0u; i < 8u && portfolioHasOpenState(engines); ++i) {
    untilTs = detail::delayedTs(untilTs, maxLatencyUs(request));
    if (!runUntilMulti(descriptor, inputPolicy, state, engines,
                       legInstruments, scratch, shard, teardownControl,
                       untilTs, request)) return false;
    for (detail::EngineState& engine : engines) {
      if (engine.netQtyRaw != 0 && !detail::forceClosePosition(engine, untilTs, engine.latency)) return false;
    }
    untilTs = detail::delayedTs(untilTs, maxLatencyUs(request));
    if (!runUntilMulti(descriptor, inputPolicy, state, engines,
                       legInstruments, scratch, shard, teardownControl,
                       untilTs, request)) return false;
    for (detail::EngineState& engine : engines) detail::cancelOpenOrders(engine, untilTs, engine.latency);
  }
  for (detail::EngineState& engine : engines) detail::cancelOpenOrders(engine, untilTs, engine.latency);
  untilTs = detail::delayedTs(untilTs, maxLatencyUs(request));
  if (!runUntilMulti(descriptor, inputPolicy, state, engines,
                     legInstruments, scratch, shard, teardownControl,
                     untilTs, request)) return false;
  return !portfolioHasOpenState(engines);
}

}  // namespace hft_backtest
