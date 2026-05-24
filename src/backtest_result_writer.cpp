#include "backtest_engine.hpp"

#include <algorithm>
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

constexpr std::size_t kMaxCompactEquityPoints = 4096;

std::filesystem::path compactResultPath(const std::filesystem::path& fullPath) {
  std::filesystem::path out = fullPath;
  out.replace_filename(fullPath.stem().string() + ".summary.json");
  return out;
}

bool includeCompactEquityPoint(std::size_t index, std::size_t count) noexcept {
  if (count <= kMaxCompactEquityPoints) return true;
  if (index == 0 || index + 1 == count) return true;
  const std::size_t stride = (count + kMaxCompactEquityPoints - 1u) / kMaxCompactEquityPoints;
  return stride == 0u || (index % stride) == 0u;
}

void writeEquityPoint(std::ostream& out, const JsonEquityPoint& row) {
  const char q = '"';
  out << "    {" << q << "ts_ns" << q << ": " << row.tsNs
      << ", " << q << "gross_realized_pnl_e8" << q << ": " << row.grossRealizedPnlE8
      << ", " << q << "realized_pnl_e8" << q << ": " << row.realizedPnlE8
      << ", " << q << "unrealized_pnl_e8" << q << ": " << row.unrealizedPnlE8
      << ", " << q << "gross_total_pnl_e8" << q << ": " << row.grossTotalPnlE8
      << ", " << q << "net_total_pnl_e8" << q << ": " << row.netTotalPnlE8
      << ", " << q << "total_pnl_e8" << q << ": " << row.totalPnlE8
      << ", " << q << "fees_paid_e8" << q << ": " << row.feesPaidE8
      << ", " << q << "wallet_balance_e8" << q << ": " << row.walletBalanceE8
      << ", " << q << "position_qty_e8" << q << ": " << row.positionQtyE8
      << ", " << q << "fill_count" << q << ": " << row.fillCount << "}";
}

void writeSummaryObject(std::ostream& out, const BacktestRunResult& result, const EngineState& engine) {
  const char q = '"';
  out << "    " << q << "mode" << q << ": " << q << "deterministic_bbo_v2_strategy_close" << q << ",\n";
  out << "    " << q << "events" << q << ": " << result.events << ",\n";
  out << "    " << q << "trades" << q << ": " << result.trades << ",\n";
  out << "    " << q << "booktickers" << q << ": " << result.bookTickers << ",\n";
  out << "    " << q << "depths" << q << ": " << result.depths << ",\n";
  out << "    " << q << "orders" << q << ": " << engine.orders.size() << ",\n";
  out << "    " << q << "fills" << q << ": " << engine.fills.size() << ",\n";
  out << "    " << q << "user_events" << q << ": " << result.userEvents << ",\n";
  out << "    " << q << "timer_wake_events" << q << ": " << result.timerWakeEvents << ",\n";
  out << "    " << q << "reduce_only_orders" << q << ": " << result.reduceOnlyOrders << ",\n";
  out << "    " << q << "reduce_only_fills" << q << ": " << result.reduceOnlyFills << ",\n";
  out << "    " << q << "strategy_closed" << q << ": " << (result.strategyClosed ? "true" : "false") << ",\n";
  out << "    " << q << "net_qty_e8" << q << ": " << engine.netQtyRaw << ",\n";
  out << "    " << q << "open_position_qty_e8" << q << ": " << result.openPositionQtyE8 << ",\n";
  out << "    " << q << "initial_balance_e8" << q << ": " << result.initialBalanceE8 << ",\n";
  out << "    " << q << "wallet_balance_e8" << q << ": " << result.walletBalanceE8 << ",\n";
  out << "    " << q << "gross_realized_pnl_e8" << q << ": " << result.grossRealizedPnlE8 << ",\n";
  out << "    " << q << "fees_paid_e8" << q << ": " << result.feesPaidE8 << ",\n";
  out << "    " << q << "net_realized_pnl_e8" << q << ": " << result.netRealizedPnlE8 << ",\n";
  out << "    " << q << "realized_pnl_e8" << q << ": " << result.realizedPnlE8 << ",\n";
  out << "    " << q << "unrealized_pnl_e8" << q << ": " << result.unrealizedPnlE8 << ",\n";
  out << "    " << q << "total_pnl_e8" << q << ": " << result.totalPnlE8 << "\n";
}

Status writeCompactResult(const BacktestRunResult& result, const EngineState& engine) noexcept {
  std::ofstream out(compactResultPath(result.resultPath), std::ios::out | std::ios::trunc);
  if (!out.is_open()) return Status::IoError;
  const char q = '"';
  out << "{\n";
  out << "  " << q << "type" << q << ": " << q << "run.summary" << q << ",\n";
  out << "  " << q << "request_id" << q << ": " << quote(result.requestId) << ",\n";
  out << "  " << q << "run_id" << q << ": " << quote(result.runId) << ",\n";
  out << "  " << q << "status" << q << ": " << quote(resultStatusName(result.status)) << ",\n";
  out << "  " << q << "strategy" << q << ": " << quote(result.strategy) << ",\n";
  out << "  " << q << "session_path" << q << ": " << quote(result.sessionPath.generic_string()) << ",\n";
  out << "  " << q << "config_path" << q << ": " << quote(result.configPath.generic_string()) << ",\n";
  out << "  " << q << "execution" << q << ": {" << q << "order_latency_us" << q << ": " << result.orderLatencyUs
      << ", " << q << "amend_latency_us" << q << ": " << result.amendLatencyUs
      << ", " << q << "cancel_latency_us" << q << ": " << result.cancelLatencyUs << "},\n";
  out << "  " << q << "equity_points" << q << ": [\n";
  bool first = true;
  for (std::size_t i = 0; i < engine.equityPoints.size(); ++i) {
    if (!includeCompactEquityPoint(i, engine.equityPoints.size())) continue;
    if (!first) out << ",\n";
    writeEquityPoint(out, engine.equityPoints[i]);
    first = false;
  }
  out << "\n  ],\n  " << q << "summary" << q << ": {\n";
  writeSummaryObject(out, result, engine);
  out << "  },\n  " << q << "errors" << q << ": ";
  if (result.error.empty()) out << "[]\n";
  else out << "[{" << q << "message" << q << ": " << quote(result.error) << "}]\n";
  out << "}\n";
  return out.good() ? Status::Ok : Status::IoError;
}
}  // namespace

Status writeResult(const BacktestRunResult& result, const EngineState& engine) noexcept {
  std::error_code ec;
  std::filesystem::create_directories(result.resultPath.parent_path(), ec);
  if (ec) return Status::IoError;
  std::ofstream out(result.resultPath, std::ios::out | std::ios::trunc);
  if (!out.is_open()) return Status::IoError;
  const char q = '"';
  out << "{\n";
  out << "  " << q << "type" << q << ": " << q << "run.result" << q << ",\n";
  out << "  " << q << "request_id" << q << ": " << quote(result.requestId) << ",\n";
  out << "  " << q << "run_id" << q << ": " << quote(result.runId) << ",\n";
  out << "  " << q << "status" << q << ": " << quote(resultStatusName(result.status)) << ",\n";
  out << "  " << q << "strategy" << q << ": " << quote(result.strategy) << ",\n";
  out << "  " << q << "session_path" << q << ": " << quote(result.sessionPath.generic_string()) << ",\n";
  out << "  " << q << "config_path" << q << ": " << quote(result.configPath.generic_string()) << ",\n";
  out << "  " << q << "execution" << q << ": {" << q << "order_latency_us" << q << ": " << result.orderLatencyUs
      << ", " << q << "amend_latency_us" << q << ": " << result.amendLatencyUs
      << ", " << q << "cancel_latency_us" << q << ": " << result.cancelLatencyUs << "},\n";
  out << "  " << q << "orders" << q << ": [\n";
  for (std::size_t i = 0; i < engine.orders.size(); ++i) {
    const JsonOrder& row = engine.orders[i];
    out << "    {" << q << "id" << q << ": " << row.id
        << ", " << q << "target_id" << q << ": " << row.targetId
        << ", " << q << "submit_ts_ns" << q << ": " << row.submitTsNs
        << ", " << q << "active_ts_ns" << q << ": " << row.activeTsNs
        << ", " << q << "ts_ns" << q << ": " << row.tsNs
        << ", " << q << "action" << q << ": " << quote(row.action)
        << ", " << q << "side" << q << ": " << quote(row.side)
        << ", " << q << "type" << q << ": " << quote(row.type)
        << ", " << q << "status" << q << ": " << quote(row.status)
        << ", " << q << "price_e8" << q << ": " << row.priceE8
        << ", " << q << "qty_e8" << q << ": " << row.qtyE8
        << ", " << q << "reduce_only" << q << ": " << (row.reduceOnly ? "true" : "false") << "}"
        << (i + 1 == engine.orders.size() ? "\n" : ",\n");
  }
  out << "  ],\n  " << q << "fills" << q << ": [\n";
  for (std::size_t i = 0; i < engine.fills.size(); ++i) {
    const JsonFill& row = engine.fills[i];
    out << "    {" << q << "order_id" << q << ": " << row.orderId
        << ", " << q << "entry_ts_ns" << q << ": " << row.entryTsNs
        << ", " << q << "exit_ts_ns" << q << ": " << row.exitTsNs
        << ", " << q << "side" << q << ": " << quote(row.side)
        << ", " << q << "price_e8" << q << ": " << row.priceE8
        << ", " << q << "qty_e8" << q << ": " << row.qtyE8
        << ", " << q << "realized_pnl_e8" << q << ": " << row.realizedPnlE8
        << ", " << q << "reduce_only" << q << ": " << (row.reduceOnly ? "true" : "false") << "}"
        << (i + 1 == engine.fills.size() ? "\n" : ",\n");
  }
  out << "  ],\n  " << q << "equity_points" << q << ": [\n";
  for (std::size_t i = 0; i < engine.equityPoints.size(); ++i) {
    const JsonEquityPoint& row = engine.equityPoints[i];
    out << "    {" << q << "ts_ns" << q << ": " << row.tsNs
        << ", " << q << "gross_realized_pnl_e8" << q << ": " << row.grossRealizedPnlE8
        << ", " << q << "realized_pnl_e8" << q << ": " << row.realizedPnlE8
        << ", " << q << "unrealized_pnl_e8" << q << ": " << row.unrealizedPnlE8
        << ", " << q << "gross_total_pnl_e8" << q << ": " << row.grossTotalPnlE8
        << ", " << q << "net_total_pnl_e8" << q << ": " << row.netTotalPnlE8
        << ", " << q << "total_pnl_e8" << q << ": " << row.totalPnlE8
        << ", " << q << "fees_paid_e8" << q << ": " << row.feesPaidE8
        << ", " << q << "wallet_balance_e8" << q << ": " << row.walletBalanceE8
        << ", " << q << "position_qty_e8" << q << ": " << row.positionQtyE8
        << ", " << q << "fill_count" << q << ": " << row.fillCount << "}"
        << (i + 1 == engine.equityPoints.size() ? "\n" : ",\n");
  }
  out << "  ],\n  " << q << "summary" << q << ": {\n";
  writeSummaryObject(out, result, engine);

  out << "  },\n  " << q << "errors" << q << ": ";
  if (result.error.empty()) out << "[]\n";
  else out << "[{" << q << "message" << q << ": " << quote(result.error) << "}]\n";
  out << "}\n";
  if (!out.good()) return Status::IoError;
  return writeCompactResult(result, engine);
}

}  // namespace hft_backtest::detail
