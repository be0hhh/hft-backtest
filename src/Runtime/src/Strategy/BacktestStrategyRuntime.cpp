#include "trading_core/Algorithm/Math/Integer/Saturating.hpp"
#include "BacktestStrategyRuntime.hpp"

#include "../Config/BacktestRunConfig.hpp"
#include "Execution/BacktestStrategyIntentLowering.hpp"
#include "Diagnostics/BacktestStrategyTrace.hpp"

#include "trading_core/Strategy/Context/StrategyContextView.hpp"
#include "trading_core/Execution/Order/OrderPathWarmup.hpp"
#include "trading_core/Strategy/StrategyInputFreshness.hpp"
#include "trading_core/Strategy/StrategyMarketData.hpp"

#include <algorithm>
#include <limits>
#include <string>

#include "Internal/BacktestStrategyRuntimeHelpers.hpp"

namespace hft_backtest::strategy_runtime_detail {

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

}  // namespace hft_backtest::strategy_runtime_detail

namespace hft_backtest {
using strategy_intent_lowering::applyToLegs;
using strategy_intent_lowering::applyToSingleLeg;
using strategy_intent_lowering::applyTransactionCycle;
using strategy_intent_lowering::bindTransactionLegs;
using strategy_intent_lowering::configuredDefaultExecutionRoute;
using strategy_intent_lowering::configuredPrimaryLegIndex;
using strategy_intent_lowering::footprintSupported;
using strategy_intent_lowering::validateOutput;
using strategy_intent_lowering::routeLegIndex;
using strategy_intent_lowering::routeTradable;
using strategy_intent_lowering::transactionAdmission;
using strategy_runtime_detail::clearRateLimitCooldown;
using strategy_runtime_detail::drainUserEvents;
using strategy_runtime_detail::drainUserEventsMulti;
using strategy_runtime_detail::dueWithinBoundary;
using strategy_runtime_detail::hasUndeliveredUserEvents;
using strategy_runtime_detail::nextPendingTs;
using strategy_runtime_detail::nextRateLimitCooldownTs;
using strategy_runtime_detail::nextUserEventDeliveryTs;
using strategy_runtime_detail::nextWakeTs;

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


}  // namespace hft_backtest
