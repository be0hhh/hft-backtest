#include "backtest_engine.hpp"

#include <fstream>
#include <system_error>

namespace hft_backtest::detail {
namespace {

const char* resultStatusName(Status status) noexcept {
  switch (status) {
    case Status::Ok: return "complete";
    case Status::Cancelled: return "cancelled";
    default: return "error";
  }
}

bool writeOrders(const std::filesystem::path& path, const EngineState& engine) noexcept {
  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out.is_open()) return false;
  for (const JsonOrder& row : engine.orders) {
    out << '[' << row.id
        << ',' << row.targetId
        << ',' << row.submitTsNs
        << ',' << row.activeTsNs
        << ',' << row.tsNs
        << ',' << static_cast<unsigned>(row.action)
        << ',' << static_cast<unsigned>(row.side)
        << ',' << static_cast<unsigned>(row.type)
        << ',' << static_cast<unsigned>(row.status)
        << ',' << row.priceE8
        << ',' << row.qtyE8
        << ',' << (row.reduceOnly ? 1 : 0)
        << ',' << row.legIndex
        << "]\n";
  }
  return out.good();
}

bool writeFills(const std::filesystem::path& path, const EngineState& engine) noexcept {
  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out.is_open()) return false;
  for (const JsonFill& row : engine.fills) {
    out << '[' << row.orderId
        << ',' << row.entryTsNs
        << ',' << row.exitTsNs
        << ',' << static_cast<unsigned>(row.side)
        << ',' << row.priceE8
        << ',' << row.qtyE8
        << ',' << row.realizedPnlE8
        << ',' << (row.reduceOnly ? 1 : 0)
        << ',' << row.legIndex
        << "]\n";
  }
  return out.good();
}

bool writeEquity(const std::filesystem::path& path, const EngineState& engine) noexcept {
  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out.is_open()) return false;
  for (const JsonEquityPoint& row : engine.equityPoints) {
    out << '[' << row.tsNs
        << ',' << row.grossRealizedPnlE8
        << ',' << row.realizedPnlE8
        << ',' << row.unrealizedPnlE8
        << ',' << row.grossTotalPnlE8
        << ',' << row.netTotalPnlE8
        << ',' << row.totalPnlE8
        << ',' << row.feesPaidE8
        << ',' << row.walletBalanceE8
        << ',' << row.positionQtyE8
        << ',' << row.fillCount
        << "]\n";
  }
  return out.good();
}

bool writeRiskSnapshots(const std::filesystem::path& path, const EngineState& engine) noexcept {
  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out.is_open()) return false;
  for (const JsonRiskSnapshot& row : engine.riskSnapshots) {
    out << '[' << row.tsNs
        << ',' << row.grossRealizedPnlE8
        << ',' << row.realizedPnlE8
        << ',' << row.unrealizedPnlE8
        << ',' << row.grossTotalPnlE8
        << ',' << row.netTotalPnlE8
        << ',' << row.feesPaidE8
        << ',' << row.walletBalanceE8
        << ',' << row.positionQtyE8
        << ',' << row.entryPriceE8
        << ',' << row.bidPriceE8
        << ',' << row.askPriceE8
        << ',' << row.fillCount
        << ',' << static_cast<unsigned>(row.reason)
        << "]\n";
  }
  return out.good();
}

bool writeProbeMetrics(const std::filesystem::path& path, const EngineState& engine) noexcept {
  if (engine.probeMetrics.empty()) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return true;
  }
  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out.is_open()) return false;
  for (const JsonProbeMetric& row : engine.probeMetrics) {
    out << '[' << row.tsNs
        << ',' << static_cast<unsigned>(row.decisionKind)
        << ',' << static_cast<unsigned>(row.reasonRaw)
        << ',' << static_cast<unsigned>(row.sideRaw)
        << ',' << row.orderCount
        << ',' << row.amendCount
        << ',' << row.cancelCount
        << ',' << row.bestBidRaw
        << ',' << row.bestAskRaw
        << ',' << row.tradeRangeSampleBps
        << ',' << row.tradeRangeEmaBps
        << ',' << row.tradeRangeObservedSeconds
        << ',' << row.rollingPerMinuteRaw
        << ',' << row.volumeWindowObservedSeconds
        << ',' << static_cast<unsigned>(row.tradeRangeReady)
        << ',' << static_cast<unsigned>(row.volumeWindowReady)
        << ',' << static_cast<unsigned>(row.hasTrade)
        << ',' << static_cast<unsigned>(row.hasBookTicker)
        << ',' << static_cast<unsigned>(row.hasPriceLimit)
        << ',' << static_cast<unsigned>(row.hasDepth)
        << ',' << static_cast<unsigned>(row.hasOrderBook)
        << "]\n";
  }
  return out.good();
}

bool writeStrategyIndicator(const std::filesystem::path& path, const EngineState& engine) noexcept {
  if (engine.strategyIndicator.empty()) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return true;
  }
  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out.is_open()) return false;
  for (const JsonStrategyIndicatorPoint& row : engine.strategyIndicator) {
    out << '[' << row.tsNs
        << ',' << row.valueRaw
        << ',' << row.auxRaw
        << ',' << static_cast<unsigned>(row.eventCode)
        << ',' << static_cast<unsigned>(row.reasonRaw)
        << ',' << static_cast<unsigned>(row.decisionKind)
        << ',' << static_cast<unsigned>(row.sideRaw)
        << "]\n";
  }
  return out.good();
}
void writeSummaryObject(std::ostream& out, const BacktestRunResult& result, const EngineState& engine, const char* indent) {
  const char q = '"';
  out << indent << q << "mode" << q << ": " << q << "deterministic_bbo_v2_strategy_close" << q << ",\n";
  out << indent << q << "events" << q << ": " << result.events << ",\n";
  out << indent << q << "trades" << q << ": " << result.trades << ",\n";
  out << indent << q << "booktickers" << q << ": " << result.bookTickers << ",\n";
  out << indent << q << "price_limits" << q << ": " << result.priceLimits << ",\n";
  out << indent << q << "depths" << q << ": " << result.depths << ",\n";
  out << indent << q << "orders" << q << ": " << result.orders << ",\n";
  out << indent << q << "fills" << q << ": " << result.fills << ",\n";
  out << indent << q << "user_events" << q << ": " << result.userEvents << ",\n";
  out << indent << q << "timer_wake_events" << q << ": " << result.timerWakeEvents << ",\n";
  out << indent << q << "reduce_only_orders" << q << ": " << result.reduceOnlyOrders << ",\n";
  out << indent << q << "reduce_only_fills" << q << ": " << result.reduceOnlyFills << ",\n";
  out << indent << q << "strategy_closed" << q << ": " << (result.strategyClosed ? "true" : "false") << ",\n";
  out << indent << q << "input_coverage" << q << ": {"
      << q << "strategy_cycles" << q << ": " << result.inputCoverage.strategyCycles << ", "
      << q << "cycles_with_trade" << q << ": " << result.inputCoverage.cyclesWithTrade << ", "
      << q << "cycles_with_bookticker" << q << ": " << result.inputCoverage.cyclesWithBookTicker << ", "
      << q << "cycles_with_price_limit" << q << ": " << result.inputCoverage.cyclesWithPriceLimit << ", "
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
        << q << "orders" << q << ": " << leg.orders << ", "
        << q << "fills" << q << ": " << leg.fills << ", "
        << q << "user_events" << q << ": " << leg.userEvents << ", "
        << q << "timer_wake_events" << q << ": " << leg.timerWakeEvents << ", "
        << q << "reduce_only_orders" << q << ": " << leg.reduceOnlyOrders << ", "
        << q << "reduce_only_fills" << q << ": " << leg.reduceOnlyFills << ", "
        << q << "gross_realized_pnl_e8" << q << ": " << leg.grossRealizedPnlE8 << ", "
        << q << "fees_paid_e8" << q << ": " << leg.feesPaidE8 << ", "
        << q << "net_realized_pnl_e8" << q << ": " << leg.netRealizedPnlE8 << ", "
        << q << "unrealized_pnl_e8" << q << ": " << leg.unrealizedPnlE8 << ", "
        << q << "total_pnl_e8" << q << ": " << leg.totalPnlE8 << ", "
        << q << "wallet_delta_e8" << q << ": " << leg.walletDeltaE8 << ", "
        << q << "open_position_qty_e8" << q << ": " << leg.openPositionQtyE8 << ", "
        << q << "strategy_closed" << q << ": " << (leg.strategyClosed ? "true" : "false")
        << '}';
  }
  if (!result.legs.empty()) out << '\n' << indent;
  out << ']';
}

bool writeRunConfig(const std::filesystem::path& resultDir, const std::filesystem::path& configPath) noexcept {
  if (configPath.empty()) return true;
  const std::filesystem::path target = resultDir / "config.ini";
  std::error_code ec;
  if (std::filesystem::exists(target, ec) && std::filesystem::equivalent(configPath, target, ec)) return true;
  ec.clear();
  std::filesystem::copy_file(configPath, target, std::filesystem::copy_options::overwrite_existing, ec);
  return !ec;
}

bool writeManifest(const std::filesystem::path& path, const BacktestRunResult& result, const EngineState& engine) noexcept {
  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out.is_open()) return false;
  const char q = '"';
  out << "{\n";
  out << "  " << q << "type" << q << ": " << q << "run.result.v2" << q << ",\n";
  out << "  " << q << "schema_version" << q << ": 2,\n";
  out << "  " << q << "request_id" << q << ": " << quote(result.requestId) << ",\n";
  out << "  " << q << "run_id" << q << ": " << quote(result.runId) << ",\n";
  out << "  " << q << "status" << q << ": " << quote(resultStatusName(result.status)) << ",\n";
  out << "  " << q << "strategy" << q << ": " << quote(result.strategy) << ",\n";
  out << "  " << q << "indicator_profile" << q << ": " << quote(result.indicatorProfile) << ",\n";
  out << "  " << q << "session_path" << q << ": " << quote(result.sessionPath.generic_string()) << ",\n";
  out << "  " << q << "config_path" << q << ": " << quote("config.ini") << ",\n";
  out << "  " << q << "execution" << q << ": {"
      << q << "latency_model" << q << ": " << q << "realistic_tail_v1" << q
      << ", " << q << "latency_seed" << q << ": " << result.latencySeed
      << ", " << q << "market_data" << q << ": {" << q << "base_us" << q << ": " << result.marketDataLatency.baseUs
      << ", " << q << "jitter_us" << q << ": " << result.marketDataLatency.jitterUs << "}"
      << ", " << q << "market_order" << q << ": {" << q << "base_us" << q << ": " << result.marketOrderLatency.baseUs
      << ", " << q << "jitter_us" << q << ": " << result.marketOrderLatency.jitterUs << "}"
      << ", " << q << "limit_order" << q << ": {" << q << "base_us" << q << ": " << result.limitOrderLatency.baseUs
      << ", " << q << "jitter_us" << q << ": " << result.limitOrderLatency.jitterUs << "}"
      << ", " << q << "order_latency_us" << q << ": " << result.orderLatencyUs
      << ", " << q << "amend_latency_us" << q << ": " << result.amendLatencyUs
      << ", " << q << "cancel_latency_us" << q << ": " << result.cancelLatencyUs << "},\n";
  out << "  " << q << "streams" << q << ": {\n";
  out << "    " << q << "orders" << q << ": {" << q << "path" << q << ": " << q << "orders.jsonl" << q
      << ", " << q << "row_schema" << q << ": " << q << "[id,target_id,submit_ts_ns,active_ts_ns,ts_ns,action,side,type,status,price_e8,qty_e8,reduce_only,leg_index]" << q
      << ", " << q << "rows" << q << ": " << engine.orders.size() << "},\n";
  out << "    " << q << "fills" << q << ": {" << q << "path" << q << ": " << q << "fills.jsonl" << q
      << ", " << q << "row_schema" << q << ": " << q << "[order_id,entry_ts_ns,exit_ts_ns,side,price_e8,qty_e8,realized_pnl_e8,reduce_only,leg_index]" << q
      << ", " << q << "rows" << q << ": " << engine.fills.size() << "},\n";
  out << "    " << q << "equity" << q << ": {" << q << "path" << q << ": " << q << "equity.jsonl" << q
      << ", " << q << "row_schema" << q << ": " << q << "[ts_ns,gross_realized_pnl_e8,realized_pnl_e8,unrealized_pnl_e8,gross_total_pnl_e8,net_total_pnl_e8,total_pnl_e8,fees_paid_e8,wallet_balance_e8,position_qty_e8,fill_count]" << q
      << ", " << q << "rows" << q << ": " << engine.equityPoints.size() << "},\n";
  out << "    " << q << "risk_snapshots" << q << ": {" << q << "path" << q << ": " << q << "risk_snapshots.jsonl" << q
      << ", " << q << "row_schema" << q << ": " << q << "[ts_ns,gross_realized_pnl_e8,realized_pnl_e8,unrealized_pnl_e8,gross_total_pnl_e8,net_total_pnl_e8,fees_paid_e8,wallet_balance_e8,position_qty_e8,entry_price_e8,bid_price_e8,ask_price_e8,fill_count,reason]" << q
      << ", " << q << "rows" << q << ": " << engine.riskSnapshots.size() << "}";
  if (!engine.strategyIndicator.empty()) {
    out << ",\n";
    out << "    " << q << "strategy_indicator" << q << ": {" << q << "path" << q << ": " << q << "strategy_indicator.jsonl" << q
        << ", " << q << "profile" << q << ": " << quote(engine.strategyIndicatorProfile)
        << ", " << q << "title" << q << ": " << quote(engine.strategyIndicatorTitle)
        << ", " << q << "value_label" << q << ": " << quote(engine.strategyIndicatorValueLabel)
        << ", " << q << "aux_label" << q << ": " << quote(engine.strategyIndicatorAuxLabel)
        << ", " << q << "unit" << q << ": " << quote(engine.strategyIndicatorUnit)
        << ", " << q << "row_schema" << q << ": " << q << "[ts_ns,value_raw,aux_raw,event_code,reason_raw,decision_kind,side_raw]" << q
        << ", " << q << "rows" << q << ": " << engine.strategyIndicator.size() << "}";
  }
  if (!engine.probeMetrics.empty()) {
    out << ",\n";
    out << "    " << q << "probe_metrics" << q << ": {" << q << "path" << q << ": " << q << "probe_metrics.jsonl" << q
        << ", " << q << "row_schema" << q << ": " << q << "[ts_ns,decision_kind,reason,side,orders,amends,cancels,best_bid,best_ask,trade_range_sample_bps,trade_range_ema_bps,trade_range_observed_seconds,rolling_per_minute_raw,volume_observed_seconds,trade_range_ready,volume_ready,has_trade,has_bookticker,has_price_limit,has_depth,has_orderbook]" << q
        << ", " << q << "rows" << q << ": " << engine.probeMetrics.size() << "}\n";
  } else {
    out << "\n";
  }
  out << "  },\n";
  out << "  " << q << "enums" << q << ": {\n";
  out << "    " << q << "side" << q << ": {" << q << "0" << q << ": " << q << "sell" << q << ", " << q << "1" << q << ": " << q << "buy" << q << "},\n";
  out << "    " << q << "action" << q << ": {" << q << "1" << q << ": " << q << "order" << q << ", " << q << "2" << q << ": " << q << "amend" << q << ", " << q << "3" << q << ": " << q << "cancel" << q << "},\n";
  out << "    " << q << "type" << q << ": {" << q << "1" << q << ": " << q << "limit" << q << ", " << q << "2" << q << ": " << q << "market" << q << "},\n";
  out << "    " << q << "status" << q << ": {" << q << "1" << q << ": " << q << "pending" << q << ", " << q << "2" << q << ": " << q << "acked" << q << ", " << q << "3" << q << ": " << q << "filled" << q << ", " << q << "4" << q << ": " << q << "cancelled" << q << ", " << q << "5" << q << ": " << q << "missing" << q << ", " << q << "6" << q << ": " << q << "rejected" << q << "}\n";
  out << "  },\n";
  out << "  " << q << "summary" << q << ": {\n";
  writeSummaryObject(out, result, engine, "    ");
  out << "  }";
  if (!result.legs.empty()) {
    out << ",\n";
    writeLegsArray(out, result, "  ");
  }
  out << ",\n";
  out << "  " << q << "errors" << q << ": ";
  if (result.error.empty()) out << "[]\n";
  else out << "[{" << q << "message" << q << ": " << quote(result.error) << "}]\n";
  out << "}\n";
  return out.good();
}

}  // namespace

Status writeResult(const BacktestRunResult& result, const EngineState& engine) noexcept {
  std::error_code ec;
  std::filesystem::create_directories(result.resultPath, ec);
  if (ec) return Status::IoError;
  if (!writeRunConfig(result.resultPath, result.configPath)) return Status::IoError;
  if (!writeOrders(result.resultPath / "orders.jsonl", engine)) return Status::IoError;
  if (!writeFills(result.resultPath / "fills.jsonl", engine)) return Status::IoError;
  if (!writeEquity(result.resultPath / "equity.jsonl", engine)) return Status::IoError;
  if (!writeRiskSnapshots(result.resultPath / "risk_snapshots.jsonl", engine)) return Status::IoError;
  if (!writeProbeMetrics(result.resultPath / "probe_metrics.jsonl", engine)) return Status::IoError;
  if (!writeStrategyIndicator(result.resultPath / "strategy_indicator.jsonl", engine)) return Status::IoError;
  if (!writeManifest(result.resultPath / "manifest.json", result, engine)) return Status::IoError;
  return Status::Ok;
}

}  // namespace hft_backtest::detail
