#include "strategy/backtest_strategy_runtime.hpp"

#include "config/backtest_run_config.hpp"
#include "strategy/backtest_strategy_trace.hpp"

#include "hft_trader/api/order/OrderPathWarmup.hpp"
#include "hft_trader/runtime/runner/StrategyMarketDataRuntime.hpp"
#include "hft_trader/runtime/runner/StrategyInputFreshness.hpp"
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

std::uint32_t backtestPrimaryLegIndex(const BacktestRunRequest& request,
                                      std::size_t legCount) noexcept {
  if (legCount == 0u) return 0u;
  if (!request.hasPrimaryLegIndex) return 0u;
  return request.primaryLegIndex < legCount ? request.primaryLegIndex : 0u;
}

std::uint64_t backtestTradableLegMask(const BacktestRunRequest& request,
                                      std::size_t legCount) noexcept {
  if (request.tradeMode == BacktestTradeMode::PrimaryOnly) {
    const std::uint32_t primary = backtestPrimaryLegIndex(request, legCount);
    return primary < 64u ? (std::uint64_t{1} << primary) : 0u;
  }
  if (legCount >= 64u) return hft_trader::core::kAllStrategyLegsTradableMask;
  return legCount == 0u ? 0u : ((std::uint64_t{1} << legCount) - 1u);
}

TimeNs backtestMonoFromTs(std::int64_t tsNs) noexcept {
  TimeNs out{};
  if (tsNs > 0) out.raw = static_cast<std::uint64_t>(tsNs);
  return out;
}

std::uint64_t backtestSourceFreshLegMask(const detail::EngineState* engines,
                                         std::size_t legCount,
                                         std::int64_t tsNs) noexcept {
  if (!engines || legCount == 0u) return 0u;
  const TimeNs nowMono = backtestMonoFromTs(tsNs);
  const std::uint64_t maxAgeNs = hft_trader::runtime::strategyInputFreshnessMaxAgeNs(
      hft_trader::runtime::kDefaultStrategyInputFreshnessMaxAgeMs);
  std::uint64_t out = 0u;
  for (std::size_t i = 0u; i < legCount && i < 64u; ++i) {
    const TimeNs recvMono = backtestMonoFromTs(engines[i].lastStrategySourceMarketDataTsNs);
    if (hft_trader::runtime::strategyInputSourceTimestampFresh(recvMono, nowMono, maxAgeNs)) {
      out |= (std::uint64_t{1} << i);
    }
  }
  return out;
}

hft_trader::core::ShardControlState backtestControlWithInputFreshness(
    const hft_trader::core::ShardControlState& control,
    const hft_trader::runtime::StrategyInputSourceMaskState& freshness) noexcept {
  hft_trader::core::ShardControlState out = control;
  if (!freshness.globallyDegraded) return out;
  out.inputDegraded = hft_trader::core::kAlgoFlagTrue;
  if (out.haltReason.raw == hft_trader::core::kHaltReasonNone.raw ||
      out.haltReason.raw == hft_trader::core::kHaltReasonStaleInput.raw) {
    out.haltReason = hft_trader::core::kHaltReasonStaleInput;
  }
  return out;
}

bool backtestRouteTradable(const BacktestRunRequest& request,
                           std::size_t legCount,
                           std::size_t leg) noexcept {
  if (leg >= legCount) return false;
  if (request.tradeMode == BacktestTradeMode::PrimaryOnly) {
    return leg == static_cast<std::size_t>(backtestPrimaryLegIndex(request, legCount));
  }
  return true;
}

bool backtestOrderIntentTradable(const BacktestRunRequest& request,
                                 std::size_t legCount,
                                 const hft_trader::core::OrderIntent& intent,
                                 std::uint64_t effectiveTradableLegMask) noexcept {
  const std::size_t leg = backtestRouteLegIndex(legCount, intent.routeHandle);
  if (!backtestRouteTradable(request, legCount, leg)) return false;
  return leg < 64u && (effectiveTradableLegMask & (std::uint64_t{1} << leg)) != 0u;
}

bool backtestCancelIntentTradable(const BacktestRunRequest& request,
                                  std::size_t legCount,
                                  const hft_trader::core::CancelIntent& intent) noexcept {
  const std::size_t leg = backtestRouteLegIndex(legCount, intent.routeHandle);
  return backtestRouteTradable(request, legCount, leg);
}

bool backtestCommandIntentTradable(const BacktestRunRequest& request,
                                   std::size_t legCount,
                                   const hft_trader::core::StrategyCommandIntent& command,
                                   std::uint64_t effectiveTradableLegMask) noexcept {
  if (command.kind == hft_trader::core::StrategyCommandIntentKind::Cancel) {
    return backtestCancelIntentTradable(request, legCount, command.cancel);
  }
  if (command.kind == hft_trader::core::StrategyCommandIntentKind::Order) {
    return backtestOrderIntentTradable(request, legCount, command.order, effectiveTradableLegMask);
  }
  if (command.kind == hft_trader::core::StrategyCommandIntentKind::WarmupOrder) {
    return backtestOrderIntentTradable(request, legCount, command.warmup.order, effectiveTradableLegMask);
  }
  return true;
}

bool backtestCommandSequencesTradable(const BacktestRunRequest& request,
                                      std::size_t legCount,
                                      const hft_trader::core::StrategyIo& io,
                                      std::uint64_t effectiveTradableLegMask) noexcept {
  for (std::size_t i = 0u; i < io.commandCount;) {
    const hft_trader::core::StrategyCommandIntent& first = io.commandOut[i];
    if (first.sequenceKind == hft_trader::core::StrategyCommandSequenceKind::None) {
      ++i;
      continue;
    }
    if (!hft_trader::core::strategyCommandAmendPairValid(io.commandOut.data(), io.commandCount, i)) {
      return false;
    }
    const hft_trader::core::StrategyCommandIntent& second = io.commandOut[i + 1u];
    if (!backtestCommandIntentTradable(request, legCount, first, effectiveTradableLegMask) ||
        !backtestCommandIntentTradable(request, legCount, second, effectiveTradableLegMask)) {
      return false;
    }
    i += 2u;
  }
  return true;
}

template<typename Context>
void setBacktestTradePolicy(Context& ctx,
                            const BacktestRunRequest& request,
                            std::size_t legCount,
                            std::uint64_t effectiveTradableLegMask) noexcept {
  const std::uint32_t primary = backtestPrimaryLegIndex(request, legCount);
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
  if constexpr (requires(Context& candidate, std::uint8_t value) { candidate.tradeMode = value; }) {
    ctx.tradeMode = modeRaw;
  } else if constexpr (requires(Context& candidate) { candidate.tradeMode; }) {
    ctx.tradeMode = static_cast<decltype(ctx.tradeMode)>(modeRaw);
  }
}

detail::IntentApplyResult applyIntentsToSingleLeg(detail::EngineState& engine,
                                                  const hft_trader::core::StrategyIo& io,
                                                  std::int64_t tsNs,
                                                  detail::BacktestLatencyRuntime& latency,
                                                  const BacktestRunRequest& request,
                                                  std::uint64_t effectiveTradableLegMask) {
  detail::IntentApplyResult result{};
  if (io.commandSequenceFiltered || !hft_trader::core::strategyIoCommandSequencesValid(io)) {
    result.disposition = detail::IntentApplyDisposition::RestoreStrategyState;
    return result;
  }
  for (std::size_t i = 0u; i < io.commandCount;) {
    const hft_trader::core::StrategyCommandIntent& first = io.commandOut[i];
    if (first.sequenceKind == hft_trader::core::StrategyCommandSequenceKind::None) {
      ++i;
      continue;
    }
    const hft_trader::core::StrategyCommandIntent& second = io.commandOut[i + 1u];
    if (!backtestCommandIntentTradable(request, 1u, first, effectiveTradableLegMask) ||
        !backtestCommandIntentTradable(request, 1u, second, effectiveTradableLegMask)) {
      result.disposition = detail::IntentApplyDisposition::RestoreStrategyState;
      return result;
    }
    i += 2u;
  }

  if (request.tradeMode != BacktestTradeMode::PrimaryOnly && (effectiveTradableLegMask & 1u) != 0u) {
    return detail::applyIntents(engine, io, tsNs, latency);
  }

  std::array<hft_trader::core::OrderIntent, detail::kIntentCapacity> orders{};
  std::array<hft_trader::core::CancelIntent, detail::kIntentCapacity> cancels{};
  std::array<hft_trader::core::StrategyCommandIntent, detail::kIntentCapacity> commands{};
  hft_trader::core::StrategyIo filtered{};
  filtered.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  filtered.cancelOut = Span<hft_trader::core::CancelIntent>(cancels.data(), cancels.size());
  filtered.commandOut = Span<hft_trader::core::StrategyCommandIntent>(commands.data(), commands.size());
  filtered.requestRateLimitSnapshot = io.requestRateLimitSnapshot;
  filtered.hasCommandSequence = io.hasCommandSequence;

  for (std::size_t i = 0u; i < io.commandCount && filtered.commandCount < filtered.commandOut.size(); ++i) {
    if (!backtestCommandIntentTradable(request, 1u, io.commandOut[i], effectiveTradableLegMask)) continue;
    filtered.commandOut[filtered.commandCount++] = io.commandOut[i];
  }
  for (std::size_t i = 0u; i < io.cancelCount && filtered.cancelCount < filtered.cancelOut.size(); ++i) {
    if (!backtestCancelIntentTradable(request, 1u, io.cancelOut[i])) continue;
    filtered.cancelOut[filtered.cancelCount++] = io.cancelOut[i];
  }
  for (std::size_t i = 0u; i < io.orderCount && filtered.orderCount < filtered.orderOut.size(); ++i) {
    if (!backtestOrderIntentTradable(request, 1u, io.orderOut[i], effectiveTradableLegMask)) continue;
    filtered.orderOut[filtered.orderCount++] = io.orderOut[i];
  }
  return detail::applyIntents(engine, filtered, tsNs, latency);
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

bool acceptedMarketOrderSince(const detail::EngineState& engine,
                              std::size_t firstRecord) noexcept {
  for (std::size_t i = firstRecord; i < engine.orders.size(); ++i) {
    const detail::JsonOrder& record = engine.orders[i];
    if (record.action != detail::kRecordActionOrder || record.type != detail::kRecordTypeMarket) continue;
    if (record.status != detail::kRecordStatusRejected && record.status != detail::kRecordStatusMissing) {
      return true;
    }
  }
  return false;
}

bool transactionOrderAccepted(Span<const detail::EngineState> engines,
                              const std::array<std::size_t, hft_trader::core::kStrategyRuntimeMaxLegs>& firstRecords,
                              std::size_t leg,
                              hft_trader::core::Id clientIntentId) noexcept {
  if (leg >= engines.size() || clientIntentId.raw == 0u) return false;
  const std::vector<detail::JsonOrder>& orders = engines[leg].orders;
  const std::size_t first = firstRecords[leg] < orders.size() ? firstRecords[leg] : orders.size();
  for (std::size_t index = first; index < orders.size(); ++index) {
    const detail::JsonOrder& order = orders[index];
    if (order.id != clientIntentId.raw || order.action != detail::kRecordActionOrder) continue;
    return order.status != detail::kRecordStatusRejected && order.status != detail::kRecordStatusMissing;
  }
  return false;
}

bool applyBacktestTransactionReports(
    const hft_trader::runtime::StrategyDescriptor& descriptor,
    void* state,
    const hft_trader::core::StrategyIo& io,
    Span<const detail::EngineState> engines,
    const std::array<std::size_t, hft_trader::core::kStrategyRuntimeMaxLegs>& firstRecords) noexcept {
  if (io.transactionCount == 0u || !descriptor.applyEmitReport) return true;
  if (io.transactionCount > io.transactionOut.size()) return false;
  for (std::size_t transactionIndex = 0u; transactionIndex < io.transactionCount; ++transactionIndex) {
    const hft_trader::core::StrategyTransactionIntent& transaction = io.transactionOut[transactionIndex];
    if (transaction.recordKind != hft_trader::core::StrategyTransactionRecordKind::Proposed) continue;
    if (transaction.transactionId.raw == 0u ||
        transaction.legCount > hft_trader::core::kStrategyTransactionMaxLegs) return false;

    hft_trader::core::StrategyEmitReport report{};
    report.transactionId = transaction.transactionId;
    report.semantic = transaction.semantic;
    report.allAccepted = true;
    for (std::uint8_t legIndex = 0u; legIndex < transaction.legCount; ++legIndex) {
      const hft_trader::core::StrategyTransactionLegDelta& transactionLeg = transaction.legs[legIndex];
      const std::size_t routeLeg = backtestRouteLegIndex(engines.size(), transactionLeg.routeHandle);
      hft_trader::core::StrategyEmitLegReport& leg = report.legs[report.legCount++];
      leg.clientIntentId = transactionLeg.clientIntentId;
      leg.routeHandle = transactionLeg.routeHandle;
      leg.requestedQty = transactionLeg.expectedFillQty;
      leg.generation = transactionLeg.generation;
      leg.attempted = routeLeg < engines.size() && leg.clientIntentId.raw != 0u && leg.requestedQty.raw > 0;
      leg.accepted = leg.attempted && transactionOrderAccepted(engines, firstRecords, routeLeg, leg.clientIntentId);
      report.anyAccepted = report.anyAccepted || leg.accepted;
      report.allAccepted = report.allAccepted && leg.accepted;
      if (!leg.accepted) report.localFault = true;
    }
    if (!descriptor.applyEmitReport(state, report)) return false;
  }
  return true;
}

void cleanupFailedCycleOrders(detail::EngineState& engine,
                              std::size_t firstRecord,
                              std::int64_t tsNs) {
  std::array<hft_trader::core::CancelIntent, detail::kIntentCapacity> cancels{};
  std::size_t cancelCount = 0u;
  const std::size_t recordEnd = engine.orders.size();
  for (std::size_t i = firstRecord; i < recordEnd && cancelCount < cancels.size(); ++i) {
    const detail::JsonOrder& record = engine.orders[i];
    if (record.action != detail::kRecordActionOrder || record.type == detail::kRecordTypeMarket ||
        record.status == detail::kRecordStatusRejected || record.status == detail::kRecordStatusMissing ||
        record.status == detail::kRecordStatusFilled || record.status == detail::kRecordStatusCancelled) {
      continue;
    }
    const detail::OpenOrder* accepted = nullptr;
    for (const detail::OpenOrder& order : engine.open) {
      if (order.state.valid && order.state.clientIntentId.raw == record.id) {
        accepted = &order;
        break;
      }
    }
    if (!accepted) continue;
    hft_trader::core::CancelIntent& cancel = cancels[cancelCount++];
    cancel.clientIntentId = allocateBacktestServiceClientIntentId(engine);
    cancel.quoteSlotId = accepted->state.quoteSlotId;
    cancel.targetClientIntentId = accepted->state.clientIntentId;
    cancel.targetOrderType = accepted->state.orderType;
    cancel.routeHandle = accepted->routeHandle;
  }
  if (cancelCount == 0u) return;
  hft_trader::core::StrategyIo cleanup{};
  cleanup.cancelOut = Span<hft_trader::core::CancelIntent>(cancels.data(), cancelCount);
  cleanup.cancelCount = cancelCount;
  (void)detail::applyIntents(engine, cleanup, tsNs, engine.latency);
}

void markBacktestWarmupSkipped(detail::EngineState& engine,
                               hft_trader::strategy::OrderPathWarmupSkipReason reason) noexcept {
  engine.lastWarmupSkipReason = static_cast<std::uint8_t>(reason);
}

bool applyBacktestWarmupGate(hft_trader::core::StrategyContext& ctx,
                             detail::EngineState* engines,
                             std::size_t engineCount,
                             const BacktestRunRequest& request) noexcept {
  if (!engines || engineCount == 0u || ctx.io.commandOut.size() == 0u) return false;
  hft_trader::core::ExecutionRouteHandle warmupRoutes[hft_trader::core::kStrategyRuntimeMaxLegs]{};
  hft_trader::core::OrderIntent warmupTemplates[hft_trader::core::kStrategyRuntimeMaxLegs]{};
  std::size_t warmupRouteCount = 0u;
  auto addWarmupTemplate = [&](const hft_trader::core::OrderIntent& intent) noexcept {
    if (!orderPathWarmupCandidate(intent)) return;
    if (warmupRouteSeen(warmupRoutes, warmupRouteCount, intent.routeHandle)) return;
    const std::size_t leg = backtestRouteLegIndex(engineCount, intent.routeHandle);
    if (leg >= engineCount) return;
    if (!backtestRouteTradable(request, engineCount, leg)) return;
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

detail::IntentApplyResult applyIntentsToLegs(std::vector<detail::EngineState>& engines,
                                             const hft_trader::core::StrategyIo& io,
                                             std::int64_t tsNs,
                                             const BacktestRunRequest& request,
                                             std::uint64_t effectiveTradableLegMask) {
  detail::IntentApplyResult result{};
  if (io.commandSequenceFiltered || !hft_trader::core::strategyIoCommandSequencesValid(io)) {
    result.disposition = detail::IntentApplyDisposition::RestoreStrategyState;
    return result;
  }
  for (std::size_t i = 0u; i < io.commandCount;) {
    const hft_trader::core::StrategyCommandIntent& first = io.commandOut[i];
    if (first.sequenceKind == hft_trader::core::StrategyCommandSequenceKind::None) {
      ++i;
      continue;
    }
    const hft_trader::core::StrategyCommandIntent& second = io.commandOut[i + 1u];
    if (!backtestCommandIntentTradable(request, engines.size(), first, effectiveTradableLegMask) ||
        !backtestCommandIntentTradable(request, engines.size(), second, effectiveTradableLegMask)) {
      result.disposition = detail::IntentApplyDisposition::RestoreStrategyState;
      return result;
    }
    i += 2u;
  }

  for (std::size_t i = 0u; i < io.commandCount;) {
    const hft_trader::core::StrategyCommandIntent& command = io.commandOut[i];
    if (command.sequenceKind != hft_trader::core::StrategyCommandSequenceKind::None) {
      const bool cancelThenNew =
          command.sequenceKind == hft_trader::core::StrategyCommandSequenceKind::LimitAmendCancelThenNew;
      const hft_trader::core::StrategyCommandIntent& orderCommand = cancelThenNew ? io.commandOut[i + 1u] : command;
      const std::size_t leg = backtestRouteLegIndex(engines.size(), orderCommand.order.routeHandle);
      if (leg >= engines.size()) {
        result.disposition = detail::IntentApplyDisposition::RestoreStrategyState;
        return result;
      }
      std::array<hft_trader::core::StrategyCommandIntent, 2> commands{command, io.commandOut[i + 1u]};
      hft_trader::core::StrategyIo pair{};
      pair.commandOut = Span<hft_trader::core::StrategyCommandIntent>(commands.data(), commands.size());
      pair.commandCount = commands.size();
      pair.hasCommandSequence = true;
      result = detail::applyIntents(engines[leg], pair, tsNs, engines[leg].latency);
      if (result.disposition != detail::IntentApplyDisposition::Applied) return result;
      i += 2u;
      continue;
    }
    if (command.kind == hft_trader::core::StrategyCommandIntentKind::Cancel) {
      const std::size_t leg = backtestRouteLegIndex(engines.size(), command.cancel.routeHandle);
      if (leg >= engines.size() || !backtestRouteTradable(request, engines.size(), leg)) {
        ++i;
        continue;
      }
      std::array<hft_trader::core::CancelIntent, 1> cancel{command.cancel};
      hft_trader::core::StrategyIo one{};
      one.cancelOut = Span<hft_trader::core::CancelIntent>(cancel.data(), cancel.size());
      one.cancelCount = 1u;
      (void)detail::applyIntents(engines[leg], one, tsNs, engines[leg].latency);
    } else if (command.kind == hft_trader::core::StrategyCommandIntentKind::Order) {
      const std::size_t leg = backtestRouteLegIndex(engines.size(), command.order.routeHandle);
      if (leg >= engines.size()) {
        ++i;
        continue;
      }
      if (!backtestRouteTradable(request, engines.size(), leg) ||
          leg >= 64u ||
          (effectiveTradableLegMask & (std::uint64_t{1} << leg)) == 0u) {
        ++i;
        continue;
      }
      std::array<hft_trader::core::OrderIntent, 1> order{command.order};
      hft_trader::core::StrategyIo one{};
      one.orderOut = Span<hft_trader::core::OrderIntent>(order.data(), order.size());
      one.orderCount = 1u;
      (void)detail::applyIntents(engines[leg], one, tsNs, engines[leg].latency);
    } else if (command.kind == hft_trader::core::StrategyCommandIntentKind::WarmupOrder) {
      const std::size_t leg = backtestRouteLegIndex(engines.size(), command.warmup.order.routeHandle);
      if (leg >= engines.size()) {
        ++i;
        continue;
      }
      if (!backtestRouteTradable(request, engines.size(), leg) ||
          leg >= 64u ||
          (effectiveTradableLegMask & (std::uint64_t{1} << leg)) == 0u) {
        ++i;
        continue;
      }
      std::array<hft_trader::core::StrategyCommandIntent, 1> warmup{command};
      hft_trader::core::StrategyIo one{};
      one.commandOut = Span<hft_trader::core::StrategyCommandIntent>(warmup.data(), warmup.size());
      one.commandCount = 1u;
      (void)detail::applyIntents(engines[leg], one, tsNs, engines[leg].latency);
    }
    ++i;
  }
  for (std::size_t i = 0; i < io.cancelCount; ++i) {
    const std::size_t leg = backtestRouteLegIndex(engines.size(), io.cancelOut[i].routeHandle);
    if (leg >= engines.size()) continue;
    if (!backtestRouteTradable(request, engines.size(), leg)) continue;
    hft_trader::core::StrategyIo one{};
    one.cancelOut = Span<hft_trader::core::CancelIntent>(const_cast<hft_trader::core::CancelIntent*>(&io.cancelOut[i]), 1u);
    one.cancelCount = 1u;
    (void)detail::applyIntents(engines[leg], one, tsNs, engines[leg].latency);
  }
  for (std::size_t i = 0; i < io.orderCount; ++i) {
    const std::size_t leg = backtestRouteLegIndex(engines.size(), io.orderOut[i].routeHandle);
    if (leg >= engines.size()) continue;
    if (!backtestRouteTradable(request, engines.size(), leg) ||
        leg >= 64u ||
        (effectiveTradableLegMask & (std::uint64_t{1} << leg)) == 0u) continue;
    hft_trader::core::StrategyIo one{};
    one.orderOut = Span<hft_trader::core::OrderIntent>(const_cast<hft_trader::core::OrderIntent*>(&io.orderOut[i]), 1u);
    one.orderCount = 1u;
    (void)detail::applyIntents(engines[leg], one, tsNs, engines[leg].latency);
  }
  return result;
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
  if (changed) {
    if (strategyVisible) {
      engine.lastStrategySourceMarketDataTsNs = event.deliveryTsNs > 0 ? event.deliveryTsNs : event.tsNs;
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
  const std::uint64_t configuredTradableMask = backtestTradableLegMask(request, 1u);
  const std::uint64_t sourceFreshMask = backtestSourceFreshLegMask(&engine, 1u, tsNs);
  const hft_trader::runtime::StrategyInputSourceMaskState freshness =
      hft_trader::runtime::strategyInputSourceMaskState(1u, configuredTradableMask, sourceFreshMask);
  const hft_trader::core::ShardControlState effectiveControl =
      backtestControlWithInputFreshness(control, freshness);
  const std::size_t orderViewCount = detail::visibleOrders(engine, scratch.orderStorage.data(), scratch.orderStorage.size());
  hft_trader::core::StrategyContext ctx{};
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
  ctx.hasUserEvent = userEvent && userEvent->kind != cxet::composite::UserDataEventKind::Unknown;
  ctx.nowMono.raw = static_cast<std::uint64_t>(tsNs > 0 ? tsNs : 0);
  ctx.nowUtc = ctx.nowMono;
  scratch.warmupViews[0] = backtestWarmupView(engine);
  ctx.orderPathWarmup = scratch.warmupViews[0];
  ctx.orderPathWarmups.routes = scratch.warmupViews.data();
  ctx.orderPathWarmups.count = 1u;
  if (userEvent) {
    recordExecutionQualityFill(engine, *userEvent, 0u, tsNs, instrument);
  }
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
  ctx.io.transactionOut = Span<hft_trader::core::StrategyTransactionIntent>(
      scratch.transactionBuf.data(), scratch.transactionBuf.size());
  ctx.io.clear();
  recordInputCoverage(engine);
  hft_trader::core::StrategyDecisionTrace trace{};
  hft_trader::core::StrategyExecutionTraceBatch executionTrace{};
  const bool captureIndicatorTrace = strategyIndicatorEnabled(engine);
  const bool captureRangeTrace = emitsPriceRangeTrace(descriptor);
  const bool captureSpreadDiagnostics = emitsArbSpreadTrace(descriptor);
  const bool captureSpreadRows = request.captureStrategySpread && captureSpreadDiagnostics;
  if (captureIndicatorTrace || captureRangeTrace || captureSpreadDiagnostics) {
    hft_trader::core::initStrategyDecisionTrace(trace, ctx.nowMono.raw);
    ctx.decisionTrace = &trace;
  }
  if (captureSpreadDiagnostics) {
    executionTrace.clear();
    ctx.executionTrace = &executionTrace;
  }
  const bool canOrderFailureRollback = descriptor.captureState && descriptor.restoreAfterOrderFailure;
  hft_trader::runtime::StrategyStateSnapshot before{};
  if (canOrderFailureRollback) descriptor.captureState(state, before);
  const bool ok = descriptor.runCycle(state, ctx);
  if (ctx.io.requestRateLimitSnapshot) engine.rateLimitSnapshotRequested = true;
  bool commandSequenceBlocked =
      ctx.io.commandSequenceFiltered ||
      !hft_trader::core::strategyIoCommandSequencesValid(ctx.io) ||
      !backtestCommandSequencesTradable(request, 1u, ctx.io, freshness.tradableLegMask);
  if (!hft_trader::core::canEmitNewOrder(effectiveControl)) {
    commandSequenceBlocked = commandSequenceBlocked ||
        hft_trader::core::strategyIoHasCommandSequenceMetadata(ctx.io);
    ctx.io.orderCount = 0u;
    ctx.io.commandCount = 0u;
  }
  if (commandSequenceBlocked) {
    ctx.io.cancelCount = 0u;
    ctx.io.orderCount = 0u;
    ctx.io.commandCount = 0u;
  }
  const bool warmupGateHeldRealOrders = !commandSequenceBlocked &&
      canOrderFailureRollback && applyBacktestWarmupGate(ctx, &engine, 1u, request);
  if ((commandSequenceBlocked || warmupGateHeldRealOrders) && canOrderFailureRollback) {
    descriptor.restoreAfterOrderFailure(state, before);
  }
  if (captureIndicatorTrace || captureRangeTrace || captureSpreadDiagnostics) {
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
    if (captureSpreadDiagnostics) recordStrategySpread(engine, tsNs, trace, captureSpreadRows);
  }
  if (ctx.executionTrace) recordExecutionQualityActions(engine, tsNs, executionTrace);
  if (!ok) return false;
  if (commandSequenceBlocked) return true;
  const std::size_t firstCycleOrderRecord = engine.orders.size();
  std::array<std::size_t, hft_trader::core::kStrategyRuntimeMaxLegs> firstCycleOrderRecords{};
  firstCycleOrderRecords[0] = firstCycleOrderRecord;
  const detail::IntentApplyResult applyResult =
      applyIntentsToSingleLeg(engine, ctx.io, tsNs, engine.latency, request, freshness.tradableLegMask);
  if (!applyBacktestTransactionReports(
          descriptor,
          state,
          ctx.io,
          Span<const detail::EngineState>(&engine, 1u),
          firstCycleOrderRecords)) return false;
  if (applyResult.shouldRestoreStrategyState()) {
    const bool acceptedMarketOrder = acceptedMarketOrderSince(engine, firstCycleOrderRecord);
    cleanupFailedCycleOrders(engine, firstCycleOrderRecord, tsNs);
    if (!acceptedMarketOrder && canOrderFailureRollback) descriptor.restoreAfterOrderFailure(state, before);
  }
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
  const std::size_t legCount = engines.size() < hft_trader::core::kStrategyRuntimeMaxLegs ? engines.size() : hft_trader::core::kStrategyRuntimeMaxLegs;
  const std::uint64_t configuredTradableMask = backtestTradableLegMask(request, legCount);
  const std::uint64_t sourceFreshMask = backtestSourceFreshLegMask(engines.data(), legCount, tsNs);
  const hft_trader::runtime::StrategyInputSourceMaskState freshness =
      hft_trader::runtime::strategyInputSourceMaskState(legCount, configuredTradableMask, sourceFreshMask);
  std::uint64_t readinessMask = freshness.sourceFreshLegMask;
  if (!freshness.globallyDegraded && legCount <= 2u) {
    readinessMask = hft_trader::runtime::strategyInputLegMaskForCount(legCount);
  } else if (freshness.globallyDegraded && readinessMask == 0u) {
    readinessMask = hft_trader::runtime::strategyInputLegMaskForCount(legCount);
  }
  for (std::size_t i = 0u; i < legCount; ++i) {
    if ((readinessMask & (std::uint64_t{1} << i)) == 0u) continue;
    if (!strategySnapshotReady(descriptor, engines[i].snapshot)) return true;
  }
  const hft_trader::core::ShardControlState effectiveControl =
      backtestControlWithInputFreshness(control, freshness);
  if (scratch.legCount != legCount) scratch.reset(legCount);
  scratch.refreshDirty(engines, legInstruments);
  hft_trader::core::StrategyContext ctx{};
  ctx.shard = &shard;
  ctx.control = &effectiveControl;
  ctx.snapshot = &scratch.snapshots[0];
  ctx.marketData.snapshots = scratch.snapshots.data();
  ctx.marketData.count = legCount;
  ctx.candle = scratch.candles[0];
  ctx.candles.candles = scratch.candles.data();
  ctx.candles.count = legCount;
  ctx.instrumentSpec = scratch.instruments[0];
  ctx.instrumentSpecs.legs = scratch.instruments.data();
  ctx.instrumentSpecs.count = legCount;
  ctx.legMarkets.legs = scratch.markets.data();
  ctx.legMarkets.count = legCount;
  ctx.orderState = scratch.orderViews[0];
  ctx.orderStates.legs = scratch.orderViews.data();
  ctx.orderStates.count = legCount;
  ctx.positionState = scratch.positions[0];
  ctx.positionStates.legs = scratch.positions.data();
  ctx.positionStates.count = legCount;
  ctx.accountState = scratch.accounts[0];
  ctx.accountStates.legs = scratch.accounts.data();
  ctx.accountStates.count = legCount;
  setBacktestTradePolicy(ctx, request, legCount, freshness.tradableLegMask);
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
  if (userEvent && eventLegIndex < legInstruments.size()) {
    recordExecutionQualityFill(engines[0], *userEvent, eventLegIndex, tsNs, legInstruments[eventLegIndex]);
  }
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
  ctx.io.transactionOut = Span<hft_trader::core::StrategyTransactionIntent>(
      scratch.transactionBuf.data(), scratch.transactionBuf.size());
  ctx.io.clear();
  recordInputCoverage(engines[0]);
  hft_trader::core::StrategyDecisionTrace trace{};
  hft_trader::core::StrategyExecutionTraceBatch executionTrace{};
  const bool captureIndicatorTrace = strategyIndicatorEnabled(engines[0]);
  const bool captureRangeTrace = emitsPriceRangeTrace(descriptor);
  const bool captureSpreadDiagnostics = emitsArbSpreadTrace(descriptor);
  const bool captureSpreadRows = request.captureStrategySpread && captureSpreadDiagnostics;
  if (captureIndicatorTrace || captureRangeTrace || captureSpreadDiagnostics) {
    hft_trader::core::initStrategyDecisionTrace(trace, ctx.nowMono.raw);
    ctx.decisionTrace = &trace;
  }
  if (captureSpreadDiagnostics) {
    executionTrace.clear();
    ctx.executionTrace = &executionTrace;
  }
  const bool canOrderFailureRollback = descriptor.captureState && descriptor.restoreAfterOrderFailure;
  hft_trader::runtime::StrategyStateSnapshot before{};
  if (canOrderFailureRollback) descriptor.captureState(state, before);
  const bool ok = descriptor.runCycle(state, ctx);
  if (ctx.io.requestRateLimitSnapshot) {
    for (detail::EngineState& engine : engines) engine.rateLimitSnapshotRequested = true;
  }
  bool commandSequenceBlocked =
      ctx.io.commandSequenceFiltered ||
      !hft_trader::core::strategyIoCommandSequencesValid(ctx.io) ||
      !backtestCommandSequencesTradable(request, legCount, ctx.io, freshness.tradableLegMask);
  if (!hft_trader::core::canEmitNewOrder(effectiveControl)) {
    commandSequenceBlocked = commandSequenceBlocked ||
        hft_trader::core::strategyIoHasCommandSequenceMetadata(ctx.io);
    ctx.io.orderCount = 0u;
    ctx.io.commandCount = 0u;
  }
  if (commandSequenceBlocked) {
    ctx.io.cancelCount = 0u;
    ctx.io.orderCount = 0u;
    ctx.io.commandCount = 0u;
  }
  const bool warmupGateHeldRealOrders = !commandSequenceBlocked &&
      canOrderFailureRollback && applyBacktestWarmupGate(ctx, engines.data(), legCount, request);
  if ((commandSequenceBlocked || warmupGateHeldRealOrders) && canOrderFailureRollback) {
    descriptor.restoreAfterOrderFailure(state, before);
  }
  if (captureIndicatorTrace || captureRangeTrace || captureSpreadDiagnostics) {
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
    if (captureSpreadDiagnostics) recordStrategySpread(engines[0], tsNs, trace, captureSpreadRows);
  }
  if (ctx.executionTrace) recordExecutionQualityActions(engines[0], tsNs, executionTrace);
  if (!ok) return false;
  if (commandSequenceBlocked) return true;
  std::array<std::size_t, hft_trader::core::kStrategyRuntimeMaxLegs> firstCycleOrderRecords{};
  for (std::size_t i = 0u; i < legCount; ++i) firstCycleOrderRecords[i] = engines[i].orders.size();
  const detail::IntentApplyResult applyResult =
      applyIntentsToLegs(engines, ctx.io, tsNs, request, freshness.tradableLegMask);
  if (!applyBacktestTransactionReports(
          descriptor,
          state,
          ctx.io,
          Span<const detail::EngineState>(engines.data(), engines.size()),
          firstCycleOrderRecords)) return false;
  if (applyResult.shouldRestoreStrategyState()) {
    bool acceptedMarketOrder = false;
    for (std::size_t i = 0u; i < legCount; ++i) {
      acceptedMarketOrder = acceptedMarketOrder ||
          acceptedMarketOrderSince(engines[i], firstCycleOrderRecords[i]);
      cleanupFailedCycleOrders(engines[i], firstCycleOrderRecords[i], tsNs);
    }
    if (!acceptedMarketOrder && canOrderFailureRollback) descriptor.restoreAfterOrderFailure(state, before);
  }
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
