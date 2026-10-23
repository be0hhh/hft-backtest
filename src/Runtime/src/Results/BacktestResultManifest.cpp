#include "trading_core/Contracts/Numeric/Integer/Saturating.hpp"
#include "../Engine/BacktestEngine.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "absl/container/flat_hash_map.h"

#include "Internal/BacktestResultWriterInternal.hpp"

namespace hft_backtest::detail::result_writer {

std::size_t strategyIndicatorFeatureCount(std::string_view profile) noexcept {
  if (profile == "trend_score") return 6u;
  if (profile == "volatility_bps") return 7u;
  if (profile == "toxic_flow") return 10u;
  if (profile == "toxic_flow_pair") return 0u;
  return 0u;
}

const char* strategyIndicatorRowSchema(std::string_view profile) noexcept {
  if (profile == "trend_score") return kTrendStrategyIndicatorRowSchema;
  if (profile == "volatility_bps") return kVolatilityStrategyIndicatorRowSchema;
  if (profile == "toxic_flow") return kToxicFlowStrategyIndicatorRowSchema;
  if (profile == "toxic_flow_pair") return kStrategyIndicatorBaseRowSchema;
  return kStrategyIndicatorBaseRowSchema;
}

const char* resultStatusName(Status status) noexcept {
  switch (status) {
    case Status::Ok: return "complete";
    case Status::Cancelled: return "cancelled";
    default: return "error";
  }
}

const char* rateLimitModeName(const BacktestRunResult& result) noexcept {
  if (!result.rateLimitsEnabled) return "off";
  return result.strictRateLimitRejects ? "strict" : "model";
}

const char* tradeModeName(BacktestTradeMode mode) noexcept {
  switch (mode) {
    case BacktestTradeMode::AllLegs: return "all_legs";
    case BacktestTradeMode::PrimaryOnly: return "primary_only";
  }
  return "all_legs";
}

const char* depthExecutionModeName(BacktestDepthExecutionMode mode) noexcept {
  switch (mode) {
    case BacktestDepthExecutionMode::BboNoDepth: return "bbo_no_depth";
    case BacktestDepthExecutionMode::Depth: return "depth";
    case BacktestDepthExecutionMode::BboFallback: return "bbo_fallback";
  }
  return "bbo_no_depth";
}

void writeDepthExecutionObject(std::ostream& out,
                               const BacktestDepthExecutionSummary& depth) {
  const char q = '"';
  out << '{'
      << q << "mode" << q << ": " << quote(depthExecutionModeName(depth.mode)) << ", "
      << q << "rows_read" << q << ": " << depth.rowsRead << ", "
      << q << "bbo_fallback_fills" << q << ": " << depth.bboFallbackFills << ", "
      << q << "first_bbo_fallback_ts_ns" << q << ": " << depth.firstBboFallbackTsNs
      << '}';
}

void writeSummaryObject(std::ostream& out, const BacktestRunResult& result, const EngineState& engine, const char* indent) {
  const char q = '"';
  out << indent << q << "mode" << q << ": " << q << "deterministic_bbo_strategy_close" << q << ",\n";
  out << indent << q << "events" << q << ": " << result.events << ",\n";
  out << indent << q << "trades" << q << ": " << result.trades << ",\n";
  out << indent << q << "booktickers" << q << ": " << result.bookTickers << ",\n";
  out << indent << q << "depths" << q << ": " << result.depths << ",\n";
  out << indent << q << "orders" << q << ": " << result.orders << ",\n";
  out << indent << q << "fills" << q << ": " << result.fills << ",\n";
  out << indent << q << "user_events" << q << ": " << result.userEvents << ",\n";
  out << indent << q << "timer_wake_events" << q << ": " << result.timerWakeEvents << ",\n";
  out << indent << q << "reduce_only_orders" << q << ": " << result.reduceOnlyOrders << ",\n";
  out << indent << q << "reduce_only_fills" << q << ": " << result.reduceOnlyFills << ",\n";
  out << indent << q << "strategy_closed" << q << ": " << (result.strategyClosed ? "true" : "false") << ",\n";
  out << indent << q << "liquidated" << q << ": " << (result.liquidated ? "true" : "false") << ",\n";
  out << indent << q << "liquidation_ts_ns" << q << ": " << result.liquidationTsNs << ",\n";
  out << indent << q << "liquidation_equity_e8" << q << ": " << result.liquidationEquityE8 << ",\n";
  out << indent << q << "risk_stopped" << q << ": " << (result.riskStopped ? "true" : "false") << ",\n";
  out << indent << q << "risk_stop_ts_ns" << q << ": " << result.riskStopTsNs << ",\n";
  out << indent << q << "risk_stop_equity_e8" << q << ": " << result.riskStopEquityE8 << ",\n";
  out << indent << q << "risk_stop_threshold_equity_e8" << q << ": " << result.riskStopThresholdEquityE8 << ",\n";
  out << indent << q << "input_coverage" << q << ": {"
      << q << "strategy_cycles" << q << ": " << result.inputCoverage.strategyCycles << ", "
      << q << "cycles_with_trade" << q << ": " << result.inputCoverage.cyclesWithTrade << ", "
      << q << "cycles_with_bookticker" << q << ": " << result.inputCoverage.cyclesWithBookTicker << ", "
      << q << "cycles_with_depth" << q << ": " << result.inputCoverage.cyclesWithDepth << ", "
      << q << "cycles_with_orderbook" << q << ": " << result.inputCoverage.cyclesWithOrderBook << ", "
      << q << "seed_trade_rows" << q << ": " << result.inputCoverage.seedTradeRows << ", "
      << q << "seed_m1_candles" << q << ": " << result.inputCoverage.seedM1Candles << ", "
      << q << "seed_m15_candles" << q << ": " << result.inputCoverage.seedM15Candles << ", "
      << q << "seed_d1_candles" << q << ": " << result.inputCoverage.seedD1Candles << "},\n";
  out << indent << q << "net_qty_e8" << q << ": " << engine.netQtyRaw << ",\n";
  out << indent << q << "open_position_qty_e8" << q << ": " << result.openPositionQtyE8 << ",\n";
  out << indent << q << "initial_balance_e8" << q << ": " << result.initialBalanceE8 << ",\n";
  out << indent << q << "wallet_balance_e8" << q << ": " << result.walletBalanceE8 << ",\n";
  out << indent << q << "gross_realized_pnl_e8" << q << ": " << result.grossRealizedPnlE8 << ",\n";
  out << indent << q << "fees_paid_e8" << q << ": " << result.feesPaidE8 << ",\n";
  out << indent << q << "net_realized_pnl_e8" << q << ": " << result.netRealizedPnlE8 << ",\n";
  out << indent << q << "realized_pnl_e8" << q << ": " << result.realizedPnlE8 << ",\n";
  out << indent << q << "unrealized_pnl_e8" << q << ": " << result.unrealizedPnlE8 << ",\n";
  out << indent << q << "total_pnl_e8" << q << ": " << result.totalPnlE8 << ",\n";
  out << indent << q << "max_drawdown_e8" << q << ": " << result.maxDrawdownE8 << ",\n";
  out << indent << q << "mfe_e8" << q << ": " << result.mfeE8 << ",\n";
  out << indent << q << "mae_e8" << q << ": " << result.maeE8 << "\n";
}

void writeReplayClockObject(std::ostream& out,
                            const BacktestReplayClockEvidence& clock) {
  const char q = '"';
  out << '{'
      << q << "arrival_boundary" << q << ": " << quote(clock.arrivalBoundary) << ", "
      << q << "market_data_delivery" << q << ": " << quote(clock.marketDataDelivery) << ", "
      << q << "venue_schedule" << q << ": " << quote(clock.venueSchedule) << ", "
      << q << "first_receive_monotonic_ns" << q << ": " << clock.firstReceiveMonotonicNs << ", "
      << q << "last_receive_monotonic_ns" << q << ": " << clock.lastReceiveMonotonicNs << ", "
      << q << "first_receive_realtime_ns" << q << ": " << clock.firstReceiveRealtimeNs << ", "
      << q << "last_receive_realtime_ns" << q << ": " << clock.lastReceiveRealtimeNs << ", "
      << q << "first_replay_delivery_ns" << q << ": " << clock.firstReplayDeliveryNs << ", "
      << q << "last_replay_delivery_ns" << q << ": " << clock.lastReplayDeliveryNs << ", "
      << q << "first_venue_schedule_ns" << q << ": " << clock.firstVenueScheduleNs << ", "
      << q << "last_venue_schedule_ns" << q << ": " << clock.lastVenueScheduleNs << ", "
      << q << "realtime_regressions" << q << ": " << clock.realtimeRegressions << ", "
      << q << "monotonic_non_increasing" << q << ": " << clock.monotonicNonIncreasing << ", "
      << q << "exchange_ahead_of_receive" << q << ": " << clock.exchangeAheadOfReceive << ", "
      << q << "exchange_timestamp_missing" << q << ": " << clock.exchangeTimestampMissing << ", "
      << q << "venue_schedule_adjusted_events" << q << ": " << clock.venueScheduleAdjustedEvents
      << '}';
}

void writeLegsArray(std::ostream& out, const BacktestRunResult& result, const char* indent) {
  const char q = '"';
  out << indent << q << "legs" << q << ": [";
  for (std::size_t i = 0u; i < result.legs.size(); ++i) {
    const BacktestLegSummary& leg = result.legs[i];
    if (i != 0u) out << ',';
    out << "\n" << indent << "  {"
        << q << "leg_index" << q << ": " << i << ", "
        << q << "session_path" << q << ": " << quote(leg.sessionPath.generic_string()) << ", "
        << q << "exchange" << q << ": " << quote(leg.exchange) << ", "
        << q << "market" << q << ": " << quote(leg.market) << ", "
        << q << "symbol" << q << ": " << quote(leg.symbol) << ", "
        << q << "maker_fee_bps_e8" << q << ": " << leg.makerFeeBpsE8 << ", "
        << q << "taker_fee_bps_e8" << q << ": " << leg.takerFeeBpsE8 << ", "
        << q << "market_order_latency" << q << ": {" << q << "base_us" << q << ": " << leg.marketOrderLatency.baseUs
        << ", " << q << "jitter_us" << q << ": " << leg.marketOrderLatency.jitterUs << "}, "
        << q << "limit_order_latency" << q << ": {" << q << "base_us" << q << ": " << leg.limitOrderLatency.baseUs
        << ", " << q << "jitter_us" << q << ": " << leg.limitOrderLatency.jitterUs << "}, "
        << q << "cancel_order_latency" << q << ": {" << q << "base_us" << q << ": " << leg.cancelOrderLatency.baseUs
        << ", " << q << "jitter_us" << q << ": " << leg.cancelOrderLatency.jitterUs << "}, "
        << q << "user_data_latency" << q << ": {" << q << "base_us" << q << ": " << leg.userDataLatency.baseUs
        << ", " << q << "jitter_us" << q << ": " << leg.userDataLatency.jitterUs << "}, "
        << q << "replay_clock" << q << ": ";
    writeReplayClockObject(out, leg.replayClock);
    out << ", " << q << "orders" << q << ": " << leg.orders << ", "
        << q << "fills" << q << ": " << leg.fills << ", "
        << q << "user_events" << q << ": " << leg.userEvents << ", "
        << q << "timer_wake_events" << q << ": " << leg.timerWakeEvents << ", "
        << q << "reduce_only_orders" << q << ": " << leg.reduceOnlyOrders << ", "
        << q << "reduce_only_fills" << q << ": " << leg.reduceOnlyFills << ", "
        << q << "initial_balance_e8" << q << ": " << leg.initialBalanceE8 << ", "
        << q << "wallet_balance_e8" << q << ": " << leg.walletBalanceE8 << ", "
        << q << "gross_realized_pnl_e8" << q << ": " << leg.grossRealizedPnlE8 << ", "
        << q << "fees_paid_e8" << q << ": " << leg.feesPaidE8 << ", "
        << q << "net_realized_pnl_e8" << q << ": " << leg.netRealizedPnlE8 << ", "
        << q << "unrealized_pnl_e8" << q << ": " << leg.unrealizedPnlE8 << ", "
        << q << "total_pnl_e8" << q << ": " << leg.totalPnlE8 << ", "
        << q << "wallet_delta_e8" << q << ": " << leg.walletDeltaE8 << ", "
        << q << "open_position_qty_e8" << q << ": " << leg.openPositionQtyE8 << ", "
        << q << "depth_execution" << q << ": ";
    writeDepthExecutionObject(out, leg.depthExecution);
    out << ", "
        << q << "strategy_closed" << q << ": " << (leg.strategyClosed ? "true" : "false") << ", "
        << q << "equity" << q << ": {" << q << "path" << q << ": " << quote("legs/" + std::to_string(i) + "/equity.jsonl")
        << ", " << q << "row_schema" << q << ": "
        << q << "[ts_ns,gross_realized_pnl_e8,realized_pnl_e8,unrealized_pnl_e8,gross_total_pnl_e8,net_total_pnl_e8,total_pnl_e8,fees_paid_e8,wallet_balance_e8,position_qty_e8,fill_count]" << q
        << "}"
        << '}';
  }
  if (!result.legs.empty()) out << '\n' << indent;
  out << ']';
}

void writeMessageArray(std::ostream& out, const std::vector<std::string>& messages) {
  const char q = '"';
  out << '[';
  for (std::size_t i = 0u; i < messages.size(); ++i) {
    if (i != 0u) out << ", ";
    out << '{' << q << "message" << q << ": " << quote(messages[i]) << '}';
  }
  out << ']';
}

bool writeManifest(const std::filesystem::path& path,
                   const BacktestRunResult& result,
                   const EngineState& engine,
                   const std::vector<EngineState>* legEngines,
                   std::size_t orderLifetimeRows) noexcept {
  (void)legEngines;
  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out.is_open()) return false;
  const char q = '"';
  const RateLimitDiagnostics manifestRateLimits = collectRateLimitDiagnostics(engine, legEngines);
  const std::uint64_t manifestRejectedCommands = result.rateLimitRejectedCommands != 0u
      ? result.rateLimitRejectedCommands
      : rejectedCommandCount(manifestRateLimits);
  out << "{\n";
  out << "  " << q << "type" << q << ": " << q << "run.result" << q << ",\n";
  out << "  " << q << "schema_version" << q << ": 5,\n";
  out << "  " << q << "universe" << q << ": {" << q << "sources" << q << ": "
      << result.universeSources << ", " << q << "contexts" << q << ": "
      << result.universeContexts << ", " << q << "virtual_accounts" << q << ": "
      << result.virtualAccounts << "},\n";
  out << "  " << q << "request_id" << q << ": " << quote(result.requestId) << ",\n";
  out << "  " << q << "run_id" << q << ": " << quote(result.runId) << ",\n";
  out << "  " << q << "status" << q << ": " << quote(resultStatusName(result.status)) << ",\n";
  out << "  " << q << "strategy" << q << ": " << quote(result.strategy) << ",\n";
  out << "  " << q << "indicator_profile" << q << ": " << quote(result.indicatorProfile) << ",\n";
  out << "  " << q << "session_path" << q << ": " << quote(result.sessionPath.generic_string()) << ",\n";
  out << "  " << q << "config_path" << q << ": " << quote("config.ini") << ",\n";
  out << "  " << q << "execution" << q << ": {"
      << q << "pipeline" << q << ": " << q << "inline" << q
      << ", " << q << "trade_mode" << q << ": " << q << tradeModeName(result.tradeMode) << q
      << ", " << q << "primary_leg_index" << q << ": " << result.primaryLegIndex
      << ", " << q << "has_primary_leg_index" << q << ": " << (result.hasPrimaryLegIndex ? "true" : "false")
      << ", " << q << "latency_model" << q << ": " << q << "captured_market_data_plus_synthetic_execution" << q
      << ", " << q << "latency_seed" << q << ": " << result.latencySeed
      << ", " << q << "market_data_delivery" << q << ": " << q << "captured_application_frame_arrival" << q
      << ", " << q << "market_order" << q << ": {" << q << "base_us" << q << ": " << result.marketOrderLatency.baseUs
      << ", " << q << "jitter_us" << q << ": " << result.marketOrderLatency.jitterUs << "}"
      << ", " << q << "limit_order" << q << ": {" << q << "base_us" << q << ": " << result.limitOrderLatency.baseUs
      << ", " << q << "jitter_us" << q << ": " << result.limitOrderLatency.jitterUs << "}"
      << ", " << q << "cancel_order" << q << ": {" << q << "base_us" << q << ": " << result.cancelOrderLatency.baseUs
      << ", " << q << "jitter_us" << q << ": " << result.cancelOrderLatency.jitterUs << "}"
      << ", " << q << "user_data" << q << ": {" << q << "base_us" << q << ": " << result.userDataLatency.baseUs
      << ", " << q << "jitter_us" << q << ": " << result.userDataLatency.jitterUs << "}"
      << ", " << q << "order_latency_us" << q << ": " << result.orderLatencyUs
      << ", " << q << "cancel_latency_us" << q << ": " << result.cancelLatencyUs << "},\n";
  out << "  " << q << "replay_clock" << q << ": ";
  writeReplayClockObject(out, result.replayClock);
  out << ",\n";
  out << "  " << q << "rate_limits" << q << ": {"
      << q << "enabled" << q << ": " << (result.rateLimitsEnabled ? "true" : "false")
      << ", " << q << "strict_rejects" << q << ": " << (result.strictRateLimitRejects ? "true" : "false")
      << ", " << q << "strict_failure" << q << ": " << (result.rateLimitStrictFailure ? "true" : "false")
      << ", " << q << "mode" << q << ": " << q << rateLimitModeName(result) << q
      << ", " << q << "rejected_commands" << q << ": " << manifestRejectedCommands
      << ", " << q << "venues" << q << ": [";
  for(std::size_t i=0u;i<result.rateLimitSchedules.size();++i) {
    const auto& policy=result.rateLimitSchedules[i];
    if(i!=0u) out<<", ";
    out<<"{"<<q<<"exchange"<<q<<": "<<quote(policy.exchange)
       <<", "<<q<<"market"<<q<<": "<<quote(policy.market)
       <<", "<<q<<"enabled"<<q<<": "<<(policy.enabled?"true":"false")
       <<", "<<q<<"strict_rejects"<<q<<": "<<((policy.enabled && result.strictRateLimitRejects)?"true":"false")<<"}";
  }
  out<<"]},\n";
  out << "  " << q << "depth_execution" << q << ": ";
  writeDepthExecutionObject(out, result.depthExecution);
  out << ",\n";
  if (result.liveProbe.configured) {
    out << "  " << q << "live_probe" << q << ": {"
        << q << "configured" << q << ": true"
        << ", " << q << "enabled" << q << ": " << (result.liveProbe.enabled ? "true" : "false")
        << ", " << q << "mode" << q << ": " << q << "ignored_backtest" << q
        << "},\n";
  }
  out << "  " << q << "streams" << q << ": {\n";
  out << "    " << q << "order_lifetimes" << q << ": {" << q << "path" << q << ": " << q << "order_lifetimes.jsonl" << q
      << ", " << q << "row_schema" << q << ": " << q << "[ts_start_ns,ts_end_ns,price_e8,qty_e8,side,open_ended,leg_index,order_type]" << q
      << ", " << q << "rows" << q << ": " << orderLifetimeRows << "},\n";
  out << "    " << q << "fills" << q << ": {" << q << "path" << q << ": " << q << "fills.jsonl" << q
      << ", " << q << "row_schema" << q << ": " << q << kFillsRowSchema << q
      << ", " << q << "rows" << q << ": " << (engine.emittedFills+engine.fills.size()) << "},\n";
  out << "    " << q << "equity" << q << ": {" << q << "path" << q << ": " << q << "equity.jsonl" << q
      << ", " << q << "row_schema" << q << ": " << q << "[ts_ns,gross_realized_pnl_e8,realized_pnl_e8,unrealized_pnl_e8,gross_total_pnl_e8,net_total_pnl_e8,total_pnl_e8,fees_paid_e8,wallet_balance_e8,position_qty_e8,fill_count]" << q
      << ", " << q << "rows" << q << ": " << (engine.emittedEquityPoints+engine.equityPoints.size()) << "},\n";
  out << "    " << q << "risk_snapshots" << q << ": {" << q << "path" << q << ": " << q << "risk_snapshots.jsonl" << q
      << ", " << q << "row_schema" << q << ": " << q << "[ts_ns,gross_realized_pnl_e8,realized_pnl_e8,unrealized_pnl_e8,gross_total_pnl_e8,net_total_pnl_e8,fees_paid_e8,wallet_balance_e8,position_qty_e8,entry_price_e8,bid_price_e8,ask_price_e8,fill_count,reason]" << q
      << ", " << q << "rows" << q << ": " << (engine.emittedRiskSnapshots+engine.riskSnapshots.size()) << "}";
  if ((engine.emittedStrategyIndicator || !engine.strategyIndicator.empty())) {
    out << ",\n";
    out << "    " << q << "strategy_indicator" << q << ": {" << q << "path" << q << ": " << q << "strategy_indicator.jsonl" << q
        << ", " << q << "profile" << q << ": " << quote(engine.strategyIndicatorProfile)
        << ", " << q << "title" << q << ": " << quote(engine.strategyIndicatorTitle)
        << ", " << q << "value_label" << q << ": " << quote(engine.strategyIndicatorValueLabel)
        << ", " << q << "aux_label" << q << ": " << quote(engine.strategyIndicatorAuxLabel)
        << ", " << q << "unit" << q << ": " << quote(engine.strategyIndicatorUnit)
        << ", " << q << "row_schema" << q << ": " << q << strategyIndicatorRowSchema(engine.strategyIndicatorProfile) << q
        << ", " << q << "rows" << q << ": " << (engine.emittedStrategyIndicator+engine.strategyIndicator.size()) << "}";
  }
  if ((engine.emittedStrategyRange || !engine.strategyRange.empty())) {
    out << ",\n";
    out << "    " << q << "strategy_range" << q << ": {" << q << "path" << q << ": " << q << "strategy_range.jsonl" << q
        << ", " << q << "row_schema" << q << ": " << q << "[ts_ns,low_raw,mid_raw,high_raw]" << q
        << ", " << q << "rows" << q << ": " << (engine.emittedStrategyRange+engine.strategyRange.size()) << "}";
  }
  if ((engine.emittedStrategySpread || !engine.strategySpread.empty())) {
    out << ",\n";
    out << "    " << q << "strategy_spread" << q << ": {" << q << "path" << q << ": " << q << "strategy_spread.jsonl" << q
        << ", " << q << "unit" << q << ": " << q << "bps_e8" << q
        << ", " << q << "row_schema" << q << ": " << q << "[ts_ns,spread_bps_e8,ema_bps_e8,deviation_bps_e8,cost_band_bps_e8,edge_after_cost_bps_e8,direction,decision_kind,reason_raw]" << q
        << ", " << q << "rows" << q << ": " << (engine.emittedStrategySpread+engine.strategySpread.size()) << "}";
  }
  if ((engine.emittedExecutionQuality || !engine.executionQuality.empty())) {
    std::size_t completedRows = 0u;
    std::size_t incompleteRows = 0u;
    std::size_t rejectedRows = 0u;
    for (const JsonExecutionQualityAction& row : engine.executionQuality) {
      if (row.anchorRaw == static_cast<std::uint8_t>(trading_core::StrategyExecutionAnchor::MakerQuote)) continue;
      if (row.statusRaw == static_cast<std::uint8_t>(trading_core::StrategyExecutionStatus::Completed)) ++completedRows;
      else if (row.statusRaw == static_cast<std::uint8_t>(trading_core::StrategyExecutionStatus::Rejected)) ++rejectedRows;
      else ++incompleteRows;
    }
    out << ",\n    " << q << "execution_quality" << q << ": {" << q << "path" << q
        << ": " << q << "execution_quality.jsonl" << q
        << ", " << q << "row_schema" << q << ": " << quote(kExecutionQualityRowSchema)
        << ", " << q << "rows" << q << ": " << (engine.emittedExecutionQuality+engine.executionQuality.size())
        << ", " << q << "completed_rows" << q << ": " << completedRows
        << ", " << q << "incomplete_rows" << q << ": " << incompleteRows
        << ", " << q << "rejected_rows" << q << ": " << rejectedRows << "}";
  }
  if ((engine.emittedRateLimitUsage || !engine.rateLimitUsage.empty())) {
    out << ",\n";
    out << "    " << q << "rate_limit_usage" << q << ": {" << q << "path" << q << ": " << q << "rate_limit_usage.jsonl" << q
        << ", " << q << "unit" << q << ": " << q << "pct_e4" << q
        << ", " << q << "row_schema" << q << ": " << q << kRateLimitUsageRowSchema << q
        << ", " << q << "rows" << q << ": " << (engine.emittedRateLimitUsage+engine.rateLimitUsage.size()) << "}";
  }
  out << "\n";
  out << "  },\n";
  out << "  " << q << "enums" << q << ": {\n";
  out << "    " << q << "side" << q << ": {" << q << "0" << q << ": " << q << "sell" << q << ", " << q << "1" << q << ": " << q << "buy" << q << "},\n";
  out << "    " << q << "action" << q << ": {" << q << "1" << q << ": " << q << "order" << q << ", " << q << "3" << q << ": " << q << "cancel" << q << "},\n";
  out << "    " << q << "type" << q << ": {" << q << "1" << q << ": " << q << "limit" << q << ", "
      << q << "2" << q << ": " << q << "market" << q << ", "
      << q << "3" << q << ": " << q << "stop_market" << q << "},\n";
  out << "    " << q << "status" << q << ": {" << q << "1" << q << ": " << q << "pending" << q << ", " << q << "2" << q << ": " << q << "acked" << q << ", " << q << "3" << q << ": " << q << "filled" << q << ", " << q << "4" << q << ": " << q << "cancelled" << q << ", " << q << "5" << q << ": " << q << "missing" << q << ", " << q << "6" << q << ": " << q << "rejected" << q << ", " << q << "7" << q << ": " << q << "partial_unfilled" << q << "},\n";
  out << "    " << q << "fill_reason" << q << ": {" << q << "0" << q << ": " << q << "unknown" << q << ", " << q << "1" << q << ": " << q << "top" << q << ", " << q << "2" << q << ": " << q << "trade" << q << ", " << q << "3" << q << ": " << q << "aggressive_book" << q << ", " << q << "4" << q << ": " << q << "passive_queue" << q << ", " << q << "5" << q << ": " << q << "passive_bbo_touch" << q << "},\n";
  out << "    " << q << "liquidity" << q << ": {" << q << "0" << q << ": " << q << "maker" << q << ", " << q << "1" << q << ": " << q << "taker" << q << "},\n";
  out << "    " << q << "rate_limit_bucket" << q << ": {"
      << q << "1" << q << ": " << q << "request_weight" << q << ", "
      << q << "2" << q << ": " << q << "orders" << q << ", "
      << q << "3" << q << ": " << q << "requests" << q << ", "
      << q << "4" << q << ": " << q << "cancel_orders" << q << ", "
      << q << "5" << q << ": " << q << "reduce_only_orders" << q << "}\n";
  out << "  },\n";
  out << "  " << q << "summary" << q << ": {\n";
  writeSummaryObject(out, result, engine, "    ");
  out << "  }";
  out << ",\n";
  writeDiagnosticsObject(out, result, engine, legEngines, orderLifetimeRows, "  ");
  if (!result.legs.empty()) {
    out << ",\n";
    writeLegsArray(out, result, "  ");
  }
  out << ",\n";
  out << "  " << q << "warnings" << q << ": ";
  writeMessageArray(out, result.warnings);
  out << ",\n";
  out << "  " << q << "errors" << q << ": ";
  if (result.error.empty()) {
    out << "[]\n";
  } else {
    const std::vector<std::string> errors{result.error};
    writeMessageArray(out, errors);
    out << '\n';
  }
  out << "}\n";
  return out.good();
}

}  // namespace hft_backtest::detail::result_writer

