#include "BacktestStrategyWarmup.hpp"

#include "../BacktestStrategyRuntime.hpp"

#include <cstdint>
#include <vector>

namespace hft_backtest {
namespace {

bool buildTradeWarmupRows(const detail::SessionData& session,
                          std::vector<cxet::composite::Trade>& out,
                          trading_core::StrategyTradeHistorySeedStats& stats) {
  out.clear();
  out.reserve(session.tradeWarmup.size());
  stats = trading_core::StrategyTradeHistorySeedStats{};
  stats.rowsFetched = static_cast<std::uint64_t>(session.tradeHistoryRows > 0 ? session.tradeHistoryRows : static_cast<std::int64_t>(session.tradeWarmup.size()));
  stats.rowsProvided = static_cast<std::uint64_t>(session.tradeWarmup.size());
  stats.firstTs = timeNsFromI64(session.tradeWarmup.empty() ? 0 : session.tradeWarmup.front().tsNs);
  stats.lastTs = timeNsFromI64(session.tradeHistoryRequestedEndNs > 0 ? session.tradeHistoryRequestedEndNs : (session.tradeWarmup.empty() ? 0 : session.tradeWarmup.back().tsNs));
  for (const detail::TradeRow& row : session.tradeWarmup) {
    cxet::composite::Trade trade{};
    if (!detail::toRuntime(row, trade)) { out.clear(); return false; }
    out.push_back(trade);
  }
  return !out.empty();
}

}  // namespace

bool seedTradeWarmupFromSession(const trading_core::StrategyDescriptor& descriptor,
                                void* state,
                                const trading_core::StrategyParamStorage& params,
                                const detail::SessionData& session,
                                std::string& error) {
  if (!descriptor.resolveTradeHistory || !descriptor.seedTrades) return true;
  const TimeNs seedEnd = timeNsFromI64(session.tradeHistoryRequestedEndNs > 0 ? session.tradeHistoryRequestedEndNs : session.firstTsNs);
  trading_core::StrategyTradeHistoryRequest request{};
  if (!descriptor.resolveTradeHistory(params, seedEnd, request)) {
    error = "strategy trade history request failed";
    return false;
  }
  if (!request.enabled) return true;
  if (session.tradeHistoryRequestedStartNs <= 0 || session.tradeHistoryRequestedEndNs <= 0) {
    if (!request.required) return true;
    error = "strategy requires trade warmup but recorder manifest has no trade history coverage";
    return false;
  }
  if (static_cast<std::uint64_t>(session.tradeHistoryRequestedStartNs) > request.startTime.raw) {
    if (!request.required) return true;
    error = "recorder trade warmup is shorter than strategy requires";
    return false;
  }
  std::vector<cxet::composite::Trade> rows{};
  trading_core::StrategyTradeHistorySeedStats stats{};
  if (!buildTradeWarmupRows(session, rows, stats)) {
    if (!request.required) return true;
    error = "strategy requires trade warmup but warmup trades are empty";
    return false;
  }
  const trading_core::StrategySeedTradesStatus seedStatus = descriptor.seedTrades(state, rows.data(), rows.size(), stats);
  if (seedStatus == trading_core::StrategySeedTradesStatus::BadData) {
    if (!request.required) return true;
    error = "strategy seedTrades rejected warmup data; calculated warmup metrics are not ready";
    return false;
  }
  if (seedStatus == trading_core::StrategySeedTradesStatus::WarmupNotReady && request.required) {
    error = "strategy seedTrades did not produce ready warmup metrics";
    return false;
  }
  return true;
}

}  // namespace hft_backtest

#include "../Internal/BacktestStrategyRuntimeHelpers.hpp"
#include "../Execution/BacktestStrategyIntentLowering.hpp"
#include "trading_core/Strategy/Orders/Intent/WarmupIntents.hpp"

namespace hft_backtest::strategy_runtime_detail {
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
  view.lastCommandMono = engine.lastOrderPathCommandMono;
  view.lastWarmupMono = engine.lastOrderPathWarmupMono;
  view.lastAckMono = engine.lastOrderPathAckMono;
  view.lastHotMono = engine.lastOrderPathAckMono;
  view.lastSkipReason = engine.lastWarmupSkipReason;
  view.sessionHealthy = true;
  view.warmupInFlight = engine.warmupInFlight;
  view.serviceExposureOpen = engine.observedServiceNetQtyRaw != 0;
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
  if ((engine.nextServiceClientIntentRaw & 0x8000000000000000ull) == 0u)
    return Id{};
  Id out{};
  out.raw = engine.nextServiceClientIntentRaw++;
  return out;
}

void markBacktestWarmupSkipped(detail::EngineState& engine,
                               trading_core::strategy::OrderPathWarmupSkipReason reason) noexcept {
  engine.lastWarmupSkipReason = static_cast<std::uint8_t>(reason);
}

bool applyBacktestWarmupGate(trading_core::StrategyContext& ctx,
                             detail::EngineView engines,
                             std::size_t engineCount,
                             const BacktestRunRequest& request) noexcept {
  if (engines.empty() || engineCount == 0u || ctx.io.commandOut.size() == 0u) return false;
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


}  // namespace hft_backtest::strategy_runtime_detail
