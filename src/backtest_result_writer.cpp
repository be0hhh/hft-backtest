#include "backtest_engine.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>
#include <unordered_map>
#include <vector>

namespace hft_backtest::detail {
namespace {

const char* resultStatusName(Status status) noexcept {
  switch (status) {
    case Status::Ok: return "complete";
    case Status::Cancelled: return "cancelled";
    default: return "error";
  }
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

struct OrderLifetimeRow {
  std::int64_t tsStartNs{0};
  std::int64_t tsEndNs{0};
  std::int64_t priceE8{0};
  std::int64_t qtyE8{0};
  std::uint8_t side{kRecordSideNone};
  bool openEnded{false};
  std::uint32_t legIndex{0u};
};

bool isEffectiveOrderStatus(std::uint8_t status) noexcept {
  return status == kRecordStatusAcked || status == kRecordStatusFilled || status == kRecordStatusCancelled;
}

bool isOpenOrderStatus(std::uint8_t status) noexcept {
  return status == kRecordStatusAcked;
}

struct OrderLifetimeIndexes {
  std::unordered_map<std::uint64_t, const JsonFill*> fillByOrderId{};
  std::unordered_map<std::uint64_t, std::vector<const JsonOrder*>> commandsByTargetId{};
};

void buildOrderLifetimeIndexes(const EngineState& engine, OrderLifetimeIndexes& out) {
  out.fillByOrderId.clear();
  out.commandsByTargetId.clear();
  out.fillByOrderId.reserve(engine.fills.size());
  out.commandsByTargetId.reserve(engine.orders.size() / 2u + 1u);

  for (const JsonFill& fill : engine.fills) {
    if (fill.exitTsNs <= 0 || fill.priceE8 <= 0) continue;
    auto [it, inserted] = out.fillByOrderId.emplace(fill.orderId, &fill);
    if (!inserted && fill.exitTsNs < it->second->exitTsNs) it->second = &fill;
  }

  for (const JsonOrder& order : engine.orders) {
    if (order.targetId == 0 || order.activeTsNs <= 0) continue;
    if (order.action != kRecordActionCancel) continue;
    if (!isEffectiveOrderStatus(order.status)) continue;
    out.commandsByTargetId[order.targetId].push_back(&order);
  }

  for (auto& row : out.commandsByTargetId) {
    std::stable_sort(row.second.begin(), row.second.end(), [](const JsonOrder* lhs, const JsonOrder* rhs) noexcept {
      if (lhs->activeTsNs != rhs->activeTsNs) return lhs->activeTsNs < rhs->activeTsNs;
      return lhs->id < rhs->id;
    });
  }
}

const JsonFill* lifetimeFill(const OrderLifetimeIndexes& indexes, std::uint64_t orderId) noexcept {
  const auto it = indexes.fillByOrderId.find(orderId);
  return it == indexes.fillByOrderId.end() ? nullptr : it->second;
}

std::int64_t lifetimeCommandEndTs(const OrderLifetimeIndexes& indexes,
                                  std::uint64_t orderId,
                                  std::int64_t startTsNs) noexcept {
  const auto it = indexes.commandsByTargetId.find(orderId);
  if (it == indexes.commandsByTargetId.end()) return 0;
  const auto& commands = it->second;
  const auto command = std::upper_bound(commands.begin(), commands.end(), startTsNs, [](std::int64_t tsNs, const JsonOrder* row) noexcept {
    return tsNs < row->activeTsNs;
  });
  return command == commands.end() ? 0 : (*command)->activeTsNs;
}

bool hasSameTimestampLifetimeReplaceEnd(const OrderLifetimeIndexes& indexes,
                                        std::uint64_t orderId,
                                        std::int64_t startTsNs) noexcept {
  const auto it = indexes.commandsByTargetId.find(orderId);
  if (it == indexes.commandsByTargetId.end()) return false;
  const auto& commands = it->second;
  const auto command = std::lower_bound(commands.begin(), commands.end(), startTsNs, [](const JsonOrder* row, std::int64_t tsNs) noexcept {
    return row->activeTsNs < tsNs;
  });
  return command != commands.end() && (*command)->activeTsNs == startTsNs;
}

std::vector<OrderLifetimeRow> materializeOrderLifetimes(const EngineState& engine) {
  std::vector<OrderLifetimeRow> out;
  out.reserve(engine.fills.size() + 16u);
  OrderLifetimeIndexes indexes{};
  buildOrderLifetimeIndexes(engine, indexes);

  for (const JsonOrder& order : engine.orders) {
    if (order.action != kRecordActionOrder || order.type != kRecordTypeLimit) continue;
    if (!isEffectiveOrderStatus(order.status)) continue;
    if (order.activeTsNs <= 0 || order.priceE8 <= 0 || order.qtyE8 <= 0) continue;
    const bool replacedAtStart = hasSameTimestampLifetimeReplaceEnd(indexes, order.id, order.activeTsNs);
    if (replacedAtStart) continue;

    const JsonFill* fill = lifetimeFill(indexes, order.id);
    std::int64_t endTs = lifetimeCommandEndTs(indexes, order.id, order.activeTsNs);
    if (fill != nullptr && (endTs == 0 || fill->exitTsNs < endTs)) endTs = fill->exitTsNs;

    bool openEnded = false;
    if (endTs <= order.activeTsNs) {
      if (fill != nullptr) {
        endTs = order.activeTsNs;
      } else if (isOpenOrderStatus(order.status)) {
        endTs = engine.lastRiskSnapshotTsNs > order.activeTsNs ? engine.lastRiskSnapshotTsNs : order.activeTsNs;
        openEnded = endTs > order.activeTsNs;
      }
    }
    if (endTs <= order.activeTsNs) continue;
    out.push_back(OrderLifetimeRow{
        order.activeTsNs,
        endTs,
        order.priceE8,
        order.qtyE8,
        order.side,
        openEnded,
        order.legIndex,
    });
  }

  std::stable_sort(out.begin(), out.end(), [](const OrderLifetimeRow& lhs, const OrderLifetimeRow& rhs) noexcept {
    if (lhs.tsStartNs != rhs.tsStartNs) return lhs.tsStartNs < rhs.tsStartNs;
    return lhs.priceE8 < rhs.priceE8;
  });
  return out;
}

bool writeOrderLifetimes(const std::filesystem::path& path,
                         const std::vector<OrderLifetimeRow>& lifetimes) noexcept {
  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out.is_open()) return false;
  for (const OrderLifetimeRow& row : lifetimes) {
    out << '[' << row.tsStartNs
        << ',' << row.tsEndNs
        << ',' << row.priceE8
        << ',' << row.qtyE8
        << ',' << static_cast<unsigned>(row.side)
        << ',' << (row.openEnded ? 1 : 0)
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

bool writeStrategyRange(const std::filesystem::path& path, const EngineState& engine) noexcept {
  if (engine.strategyRange.empty()) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return true;
  }
  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out.is_open()) return false;
  for (const JsonStrategyRangePoint& row : engine.strategyRange) {
    out << '[' << row.tsNs
        << ',' << row.lowRaw
        << ',' << row.midRaw
        << ',' << row.highRaw
        << "]\n";
  }
  return out.good();
}

bool writeStrategySpread(const std::filesystem::path& path, const EngineState& engine) noexcept {
  if (engine.strategySpread.empty()) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return true;
  }
  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out.is_open()) return false;
  for (const JsonStrategySpreadPoint& row : engine.strategySpread) {
    out << '[' << row.tsNs
        << ',' << row.spreadBpsE8
        << ',' << row.emaBpsE8
        << ',' << row.deviationBpsE8
        << ',' << row.costBandBpsE8
        << ',' << row.edgeAfterCostBpsE8
        << ',' << static_cast<unsigned>(row.direction)
        << ',' << static_cast<unsigned>(row.decisionKind)
        << ',' << static_cast<unsigned>(row.reasonRaw)
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
  out << indent << q << "liquidations" << q << ": " << result.liquidations << ",\n";
  out << indent << q << "mark_prices" << q << ": " << result.markPrices << ",\n";
  out << indent << q << "index_prices" << q << ": " << result.indexPrices << ",\n";
  out << indent << q << "funding" << q << ": " << result.funding << ",\n";
  out << indent << q << "open_interest" << q << ": " << result.openInterest << ",\n";
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
        << q << "market_data_latency" << q << ": {" << q << "base_us" << q << ": " << leg.marketDataLatency.baseUs
        << ", " << q << "jitter_us" << q << ": " << leg.marketDataLatency.jitterUs << "}, "
        << q << "market_order_latency" << q << ": {" << q << "base_us" << q << ": " << leg.marketOrderLatency.baseUs
        << ", " << q << "jitter_us" << q << ": " << leg.marketOrderLatency.jitterUs << "}, "
        << q << "limit_order_latency" << q << ": {" << q << "base_us" << q << ": " << leg.limitOrderLatency.baseUs
        << ", " << q << "jitter_us" << q << ": " << leg.limitOrderLatency.jitterUs << "}, "
        << q << "cancel_order_latency" << q << ": {" << q << "base_us" << q << ": " << leg.cancelOrderLatency.baseUs
        << ", " << q << "jitter_us" << q << ": " << leg.cancelOrderLatency.jitterUs << "}, "
        << q << "user_data_latency" << q << ": {" << q << "base_us" << q << ": " << leg.userDataLatency.baseUs
        << ", " << q << "jitter_us" << q << ": " << leg.userDataLatency.jitterUs << "}, "
        << q << "orders" << q << ": " << leg.orders << ", "
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

bool writeRunConfig(const std::filesystem::path& resultDir, const std::filesystem::path& configPath) noexcept {
  if (configPath.empty()) return true;
  const std::filesystem::path target = resultDir / "config.ini";
  std::error_code ec;
  if (std::filesystem::exists(target, ec) && std::filesystem::equivalent(configPath, target, ec)) return true;
  ec.clear();
  std::filesystem::copy_file(configPath, target, std::filesystem::copy_options::overwrite_existing, ec);
  return !ec;
}

bool writeLegEquityStreams(const std::filesystem::path& resultDir,
                           const std::vector<EngineState>* legEngines) noexcept {
  if (legEngines == nullptr) return true;
  for (std::size_t i = 0u; i < legEngines->size(); ++i) {
    const std::filesystem::path dir = resultDir / "legs" / std::to_string(i);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return false;
    if (!writeEquity(dir / "equity.jsonl", (*legEngines)[i])) return false;
  }
  return true;
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
      << ", " << q << "cancel_order" << q << ": {" << q << "base_us" << q << ": " << result.cancelOrderLatency.baseUs
      << ", " << q << "jitter_us" << q << ": " << result.cancelOrderLatency.jitterUs << "}"
      << ", " << q << "user_data" << q << ": {" << q << "base_us" << q << ": " << result.userDataLatency.baseUs
      << ", " << q << "jitter_us" << q << ": " << result.userDataLatency.jitterUs << "}"
      << ", " << q << "order_latency_us" << q << ": " << result.orderLatencyUs
      << ", " << q << "cancel_latency_us" << q << ": " << result.cancelLatencyUs << "},\n";
  if (result.liveProbe.configured) {
    out << "  " << q << "live_probe" << q << ": {"
        << q << "configured" << q << ": true"
        << ", " << q << "enabled" << q << ": " << (result.liveProbe.enabled ? "true" : "false")
        << ", " << q << "mode" << q << ": " << q << "ignored_backtest" << q
        << ", " << q << "notional_usdt_e8" << q << ": " << result.liveProbe.notionalUsdtE8
        << ", " << q << "timeout_ms" << q << ": " << result.liveProbe.timeoutMs
        << ", " << q << "limit_offset_spread_pct" << q << ": " << result.liveProbe.limitOffsetSpreadPct
        << "},\n";
  }
  out << "  " << q << "streams" << q << ": {\n";
  out << "    " << q << "order_lifetimes" << q << ": {" << q << "path" << q << ": " << q << "order_lifetimes.jsonl" << q
      << ", " << q << "row_schema" << q << ": " << q << "[ts_start_ns,ts_end_ns,price_e8,qty_e8,side,open_ended,leg_index]" << q
      << ", " << q << "rows" << q << ": " << orderLifetimeRows << "},\n";
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
  if (!engine.strategyRange.empty()) {
    out << ",\n";
    out << "    " << q << "strategy_range" << q << ": {" << q << "path" << q << ": " << q << "strategy_range.jsonl" << q
        << ", " << q << "row_schema" << q << ": " << q << "[ts_ns,low_raw,mid_raw,high_raw]" << q
        << ", " << q << "rows" << q << ": " << engine.strategyRange.size() << "}";
  }
  if (!engine.strategySpread.empty()) {
    out << ",\n";
    out << "    " << q << "strategy_spread" << q << ": {" << q << "path" << q << ": " << q << "strategy_spread.jsonl" << q
        << ", " << q << "unit" << q << ": " << q << "bps_e8" << q
        << ", " << q << "row_schema" << q << ": " << q << "[ts_ns,spread_bps_e8,ema_bps_e8,deviation_bps_e8,cost_band_bps_e8,edge_after_cost_bps_e8,direction,decision_kind,reason_raw]" << q
        << ", " << q << "rows" << q << ": " << engine.strategySpread.size() << "}";
  }
  out << "\n";
  out << "  },\n";
  out << "  " << q << "enums" << q << ": {\n";
  out << "    " << q << "side" << q << ": {" << q << "0" << q << ": " << q << "sell" << q << ", " << q << "1" << q << ": " << q << "buy" << q << "},\n";
  out << "    " << q << "action" << q << ": {" << q << "1" << q << ": " << q << "order" << q << ", " << q << "3" << q << ": " << q << "cancel" << q << "},\n";
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

}  // namespace

Status writeResult(const BacktestRunResult& result,
                   const EngineState& engine,
                   const std::vector<EngineState>* legEngines) noexcept {
  std::error_code ec;
  std::filesystem::create_directories(result.resultPath, ec);
  if (ec) return Status::IoError;
  const std::vector<OrderLifetimeRow> orderLifetimes = materializeOrderLifetimes(engine);
  if (!writeRunConfig(result.resultPath, result.configPath)) return Status::IoError;
  if (!writeOrderLifetimes(result.resultPath / "order_lifetimes.jsonl", orderLifetimes)) return Status::IoError;
  std::filesystem::remove(result.resultPath / "orders.jsonl", ec);
  if (ec) return Status::IoError;
  if (!writeFills(result.resultPath / "fills.jsonl", engine)) return Status::IoError;
  if (!writeEquity(result.resultPath / "equity.jsonl", engine)) return Status::IoError;
  if (!writeLegEquityStreams(result.resultPath, legEngines)) return Status::IoError;
  if (!writeRiskSnapshots(result.resultPath / "risk_snapshots.jsonl", engine)) return Status::IoError;
  std::filesystem::remove(result.resultPath / "probe_metrics.jsonl", ec);
  if (ec) return Status::IoError;
  if (!writeStrategyIndicator(result.resultPath / "strategy_indicator.jsonl", engine)) return Status::IoError;
  if (!writeStrategyRange(result.resultPath / "strategy_range.jsonl", engine)) return Status::IoError;
  if (!writeStrategySpread(result.resultPath / "strategy_spread.jsonl", engine)) return Status::IoError;
  if (!writeManifest(result.resultPath / "manifest.json", result, engine, legEngines, orderLifetimes.size())) return Status::IoError;
  return Status::Ok;
}

}  // namespace hft_backtest::detail
