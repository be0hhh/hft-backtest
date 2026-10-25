#include "trading_core/Contracts/Numeric/Integer/Saturating.hpp"
#include "../BacktestStrategyRuntime.hpp"

#include "../../Config/BacktestRunConfig.hpp"
#include "BacktestStrategyIntentLowering.hpp"
#include "../Diagnostic/BacktestStrategyTrace.hpp"

#include "trading_core/Strategy/Context/StrategyContextView.hpp"
#include "trading_core/Strategy/Orders/Intent/WarmupIntents.hpp"
#include "trading_core/Strategy/StrategyInputFreshness.hpp"
#include "trading_core/Strategy/StrategyMarketData.hpp"

#include <algorithm>
#include <limits>
#include <string>

#include "../Internal/BacktestStrategyRuntimeHelpers.hpp"

namespace hft_backtest::strategy_runtime_detail {

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
using strategy_runtime_detail::nextWakeTs;
using strategy_runtime_detail::portfolioHasOpenState;

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
                            detail::EngineView engines,
                            std::span<const StrategyInstrumentSpec> legInstruments,
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
