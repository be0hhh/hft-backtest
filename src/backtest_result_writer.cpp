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

std::uint8_t actionCode(const std::string& action) noexcept {
  if (action == "order") return 1u;
  if (action == "amend") return 2u;
  if (action == "cancel") return 3u;
  return 0u;
}

std::uint8_t sideCode(const std::string& side) noexcept {
  return side == "buy" ? 1u : 0u;
}

std::uint8_t typeCode(const std::string& type) noexcept {
  if (type == "limit") return 1u;
  if (type == "market") return 2u;
  return 0u;
}

std::uint8_t statusCode(const std::string& status) noexcept {
  if (status == "pending") return 1u;
  if (status == "active" || status == "acked" || status == "amended") return 2u;
  if (status == "filled") return 3u;
  if (status == "cancelled") return 4u;
  if (status == "missing") return 5u;
  if (status == "rejected") return 6u;
  return 0u;
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
        << ',' << static_cast<unsigned>(actionCode(row.action))
        << ',' << static_cast<unsigned>(sideCode(row.side))
        << ',' << static_cast<unsigned>(typeCode(row.type))
        << ',' << static_cast<unsigned>(statusCode(row.status))
        << ',' << row.priceE8
        << ',' << row.qtyE8
        << ',' << (row.reduceOnly ? 1 : 0)
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
        << ',' << static_cast<unsigned>(sideCode(row.side))
        << ',' << row.priceE8
        << ',' << row.qtyE8
        << ',' << row.realizedPnlE8
        << ',' << (row.reduceOnly ? 1 : 0)
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

void writeSummaryObject(std::ostream& out, const BacktestRunResult& result, const EngineState& engine, const char* indent) {
  const char q = '"';
  out << indent << q << "mode" << q << ": " << q << "deterministic_bbo_v2_strategy_close" << q << ",\n";
  out << indent << q << "events" << q << ": " << result.events << ",\n";
  out << indent << q << "trades" << q << ": " << result.trades << ",\n";
  out << indent << q << "booktickers" << q << ": " << result.bookTickers << ",\n";
  out << indent << q << "depths" << q << ": " << result.depths << ",\n";
  out << indent << q << "orders" << q << ": " << engine.orders.size() << ",\n";
  out << indent << q << "fills" << q << ": " << engine.fills.size() << ",\n";
  out << indent << q << "user_events" << q << ": " << result.userEvents << ",\n";
  out << indent << q << "timer_wake_events" << q << ": " << result.timerWakeEvents << ",\n";
  out << indent << q << "reduce_only_orders" << q << ": " << result.reduceOnlyOrders << ",\n";
  out << indent << q << "reduce_only_fills" << q << ": " << result.reduceOnlyFills << ",\n";
  out << indent << q << "strategy_closed" << q << ": " << (result.strategyClosed ? "true" : "false") << ",\n";
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
  out << indent << q << "total_pnl_e8" << q << ": " << result.totalPnlE8 << "\n";
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
  out << "  " << q << "session_path" << q << ": " << quote(result.sessionPath.generic_string()) << ",\n";
  out << "  " << q << "config_path" << q << ": " << quote("config.ini") << ",\n";
  out << "  " << q << "execution" << q << ": {" << q << "order_latency_us" << q << ": " << result.orderLatencyUs
      << ", " << q << "amend_latency_us" << q << ": " << result.amendLatencyUs
      << ", " << q << "cancel_latency_us" << q << ": " << result.cancelLatencyUs << "},\n";
  out << "  " << q << "streams" << q << ": {\n";
  out << "    " << q << "orders" << q << ": {" << q << "path" << q << ": " << q << "orders.jsonl" << q
      << ", " << q << "row_schema" << q << ": " << q << "[id,target_id,submit_ts_ns,active_ts_ns,ts_ns,action,side,type,status,price_e8,qty_e8,reduce_only]" << q
      << ", " << q << "rows" << q << ": " << engine.orders.size() << "},\n";
  out << "    " << q << "fills" << q << ": {" << q << "path" << q << ": " << q << "fills.jsonl" << q
      << ", " << q << "row_schema" << q << ": " << q << "[order_id,entry_ts_ns,exit_ts_ns,side,price_e8,qty_e8,realized_pnl_e8,reduce_only]" << q
      << ", " << q << "rows" << q << ": " << engine.fills.size() << "},\n";
  out << "    " << q << "equity" << q << ": {" << q << "path" << q << ": " << q << "equity.jsonl" << q
      << ", " << q << "row_schema" << q << ": " << q << "[ts_ns,gross_realized_pnl_e8,realized_pnl_e8,unrealized_pnl_e8,gross_total_pnl_e8,net_total_pnl_e8,total_pnl_e8,fees_paid_e8,wallet_balance_e8,position_qty_e8,fill_count]" << q
      << ", " << q << "rows" << q << ": " << engine.equityPoints.size() << "},\n";
  out << "    " << q << "risk_snapshots" << q << ": {" << q << "path" << q << ": " << q << "risk_snapshots.jsonl" << q
      << ", " << q << "row_schema" << q << ": " << q << "[ts_ns,gross_realized_pnl_e8,realized_pnl_e8,unrealized_pnl_e8,gross_total_pnl_e8,net_total_pnl_e8,fees_paid_e8,wallet_balance_e8,position_qty_e8,entry_price_e8,bid_price_e8,ask_price_e8,fill_count,reason]" << q
      << ", " << q << "rows" << q << ": " << engine.riskSnapshots.size() << "}\n";
  out << "  },\n";
  out << "  " << q << "enums" << q << ": {\n";
  out << "    " << q << "side" << q << ": {" << q << "0" << q << ": " << q << "sell" << q << ", " << q << "1" << q << ": " << q << "buy" << q << "},\n";
  out << "    " << q << "action" << q << ": {" << q << "1" << q << ": " << q << "order" << q << ", " << q << "2" << q << ": " << q << "amend" << q << ", " << q << "3" << q << ": " << q << "cancel" << q << "},\n";
  out << "    " << q << "type" << q << ": {" << q << "1" << q << ": " << q << "limit" << q << ", " << q << "2" << q << ": " << q << "market" << q << "},\n";
  out << "    " << q << "status" << q << ": {" << q << "1" << q << ": " << q << "pending" << q << ", " << q << "2" << q << ": " << q << "acked" << q << ", " << q << "3" << q << ": " << q << "filled" << q << ", " << q << "4" << q << ": " << q << "cancelled" << q << ", " << q << "5" << q << ": " << q << "missing" << q << ", " << q << "6" << q << ": " << q << "rejected" << q << "}\n";
  out << "  },\n";
  out << "  " << q << "summary" << q << ": {\n";
  writeSummaryObject(out, result, engine, "    ");
  out << "  },\n";
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
  if (!writeManifest(result.resultPath / "manifest.json", result, engine)) return Status::IoError;
  return Status::Ok;
}

}  // namespace hft_backtest::detail
