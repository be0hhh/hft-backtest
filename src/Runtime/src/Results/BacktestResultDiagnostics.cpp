#include "trading_core/Contracts/Numeric/Integer/Saturating.hpp"
#include "../Engine/BacktestEngine.hpp"

#include <algorithm>
#include <array>
#include <chrono>
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

std::string_view trim(std::string_view value) noexcept {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r' || value.front() == '\n')) {
    value.remove_prefix(1u);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t' || value.back() == '\r' || value.back() == '\n')) {
    value.remove_suffix(1u);
  }
  return value;
}

std::string_view stripIniComment(std::string_view value) noexcept {
  const std::size_t hash = value.find('#');
  const std::size_t semicolon = value.find(';');
  std::size_t cut = std::string_view::npos;
  if (hash != std::string_view::npos) cut = hash;
  if (semicolon != std::string_view::npos && (cut == std::string_view::npos || semicolon < cut)) cut = semicolon;
  if (cut != std::string_view::npos) value = value.substr(0u, cut);
  return trim(value);
}

bool parseDecimalBpsE8(std::string_view value, std::int64_t& out) noexcept {
  value = stripIniComment(value);
  if (value.empty()) return false;
  bool negative = false;
  if (value.front() == '+' || value.front() == '-') {
    negative = value.front() == '-';
    value.remove_prefix(1u);
  }
  if (value.empty()) return false;

  using WideMagnitude = unsigned __int128;
  constexpr WideMagnitude kPositiveLimit =
      static_cast<WideMagnitude>(std::numeric_limits<std::int64_t>::max());
  const WideMagnitude magnitudeLimit =
      negative ? kPositiveLimit + 1u : kPositiveLimit;
  const WideMagnitude wholeLimit =
      magnitudeLimit / static_cast<WideMagnitude>(kBpsE8Scale);

  WideMagnitude whole = 0;
  WideMagnitude fraction = 0;
  WideMagnitude fractionScale =
      static_cast<WideMagnitude>(kBpsE8Scale / 10);
  bool sawDigit = false;
  bool fractional = false;
  for (const char ch : value) {
    if (ch == '.') {
      if (fractional) return false;
      fractional = true;
      continue;
    }
    if (ch < '0' || ch > '9') return false;
    sawDigit = true;
    const WideMagnitude digit = static_cast<unsigned>(ch - '0');
    if (fractional) {
      if (fractionScale > 0) {
        fraction += digit * fractionScale;
        fractionScale /= 10;
      }
    } else {
      if (whole > wholeLimit / 10u ||
          (whole == wholeLimit / 10u && digit > wholeLimit % 10u)) {
        return false;
      }
      whole = whole * 10 + digit;
    }
  }
  if (!sawDigit) return false;
  const WideMagnitude magnitude =
      whole * static_cast<WideMagnitude>(kBpsE8Scale) + fraction;
  if (magnitude > magnitudeLimit) return false;
  if (!negative) {
    out = static_cast<std::int64_t>(magnitude);
  } else if (magnitude == kPositiveLimit + 1u) {
    out = std::numeric_limits<std::int64_t>::min();
  } else {
    out = -static_cast<std::int64_t>(magnitude);
  }
  return true;
}

bool readConfigBpsE8(const std::filesystem::path& path,
                     std::string_view key,
                     std::int64_t& out) noexcept {
  if (path.empty()) return false;
  std::ifstream input(path);
  if (!input.is_open()) return false;
  std::string line;
  while (std::getline(input, line)) {
    std::string_view view = trim(line);
    if (view.empty() || view.front() == '#' || view.front() == ';' || view.front() == '[') continue;
    const std::size_t equals = view.find('=');
    if (equals == std::string_view::npos) continue;
    const std::string_view lineKey = trim(view.substr(0u, equals));
    if (lineKey != key) continue;
    return parseDecimalBpsE8(view.substr(equals + 1u), out);
  }
  return false;
}

std::int64_t absI64(std::int64_t value) noexcept {
  if (value >= 0) return value;
  if (value == std::numeric_limits<std::int64_t>::min()) return std::numeric_limits<std::int64_t>::max();
  return -value;
}

std::int64_t ratioPctE8(std::uint64_t numerator, std::uint64_t denominator) noexcept {
  if (denominator == 0u) return 0;
#if defined(__SIZEOF_INT128__)
  const unsigned __int128 scaled = static_cast<unsigned __int128>(numerator) * 100000000u;
  const unsigned __int128 value = scaled / denominator;
  return value > static_cast<unsigned __int128>(std::numeric_limits<std::int64_t>::max())
      ? std::numeric_limits<std::int64_t>::max()
      : static_cast<std::int64_t>(value);
#else
  if (numerator > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() / 100000000ll)) {
    return std::numeric_limits<std::int64_t>::max();
  }
  return static_cast<std::int64_t>((numerator * 100000000ull) / denominator);
#endif
}

std::int64_t ratioPctE8(std::int64_t numerator, std::int64_t denominator) noexcept {
  const std::uint64_t num = static_cast<std::uint64_t>(absI64(numerator));
  const std::uint64_t den = static_cast<std::uint64_t>(absI64(denominator));
  return ratioPctE8(num, den);
}

void sortAndTrimDecisionCounts(std::vector<DecisionCount>& counts) {
  std::stable_sort(counts.begin(), counts.end(), [](const DecisionCount& lhs, const DecisionCount& rhs) noexcept {
    if (lhs.count != rhs.count) return lhs.count > rhs.count;
    if (lhs.decisionKind != rhs.decisionKind) return lhs.decisionKind < rhs.decisionKind;
    return lhs.reasonRaw < rhs.reasonRaw;
  });
  if (counts.size() > kMaxDecisionDiagnostics) counts.resize(kMaxDecisionDiagnostics);
}

SignalDiagnostics collectSignalDiagnostics(const EngineState& engine,
                                           const std::filesystem::path& configPath) {
  SignalDiagnostics out{};
  out.hasEntryEdgeBps = readConfigBpsE8(configPath, "entry_edge_bps", out.entryEdgeBpsE8);
  if (engine.strategySpreadDiagnostics.rows > 0u) {
    out.strategySpreadRows = engine.strategySpreadDiagnostics.rows;
    out.minEdgeAfterCostBpsE8 = engine.strategySpreadDiagnostics.minEdgeAfterCostBpsE8;
    out.maxEdgeAfterCostBpsE8 = engine.strategySpreadDiagnostics.maxEdgeAfterCostBpsE8;
    out.maxAbsDeviationBpsE8 = engine.strategySpreadDiagnostics.maxAbsDeviationBpsE8;
    out.decisionCounts.reserve(16u);
    for (std::size_t kind = 0u; kind < engine.strategySpreadDiagnostics.decisionCounts.size(); ++kind) {
      for (std::size_t reason = 0u; reason < engine.strategySpreadDiagnostics.decisionCounts[kind].size(); ++reason) {
        const std::uint64_t count = engine.strategySpreadDiagnostics.decisionCounts[kind][reason];
        if (count == 0u) continue;
        out.decisionCounts.push_back(DecisionCount{
            static_cast<std::uint8_t>(kind),
            static_cast<std::uint8_t>(reason),
            count,
        });
      }
    }
    sortAndTrimDecisionCounts(out.decisionCounts);
    return out;
  }

  out.strategySpreadRows = engine.strategySpread.size();
  absl::flat_hash_map<std::uint16_t, std::uint64_t> decisionCounts;
  decisionCounts.reserve(engine.strategySpread.size() > 0u ? 16u : 0u);
  bool haveEdge = false;
  for (const JsonStrategySpreadPoint& row : engine.strategySpread) {
    if (!haveEdge) {
      out.minEdgeAfterCostBpsE8 = row.edgeAfterCostBpsE8;
      out.maxEdgeAfterCostBpsE8 = row.edgeAfterCostBpsE8;
      haveEdge = true;
    } else {
      out.minEdgeAfterCostBpsE8 = std::min(out.minEdgeAfterCostBpsE8, row.edgeAfterCostBpsE8);
      out.maxEdgeAfterCostBpsE8 = std::max(out.maxEdgeAfterCostBpsE8, row.edgeAfterCostBpsE8);
    }
    out.maxAbsDeviationBpsE8 = std::max(out.maxAbsDeviationBpsE8, absI64(row.deviationBpsE8));
    const std::uint16_t key = static_cast<std::uint16_t>((static_cast<std::uint16_t>(row.decisionKind) << 8u) |
                                                         static_cast<std::uint16_t>(row.reasonRaw));
    ++decisionCounts[key];
  }
  out.decisionCounts.reserve(decisionCounts.size());
  for (const auto& entry : decisionCounts) {
    out.decisionCounts.push_back(DecisionCount{
        static_cast<std::uint8_t>((entry.first >> 8u) & 0xffu),
        static_cast<std::uint8_t>(entry.first & 0xffu),
        entry.second,
    });
  }
  sortAndTrimDecisionCounts(out.decisionCounts);
  return out;
}

ExecutionDiagnostics collectExecutionDiagnostics(const EngineState& engine) noexcept {
  ExecutionDiagnostics out{};
  for (const JsonOrder& order : engine.orders) {
    if (order.action == kRecordActionOrder) ++out.orderCommands;
    else if (order.action == kRecordActionCancel) ++out.cancelCommands;
    if (order.postOnly && order.action == kRecordActionOrder) ++out.postOnlyOrders;
    if (order.service) ++out.serviceOrders;
    if (order.warmup) ++out.warmupOrders;
    if (order.status < out.statusCounts.size()) ++out.statusCounts[order.status];
    if (order.type < out.typeCounts.size()) ++out.typeCounts[order.type];
  }
  for (const JsonFill& fill : engine.fills) {
    if (fill.fillReason < out.fillReasonCounts.size()) ++out.fillReasonCounts[fill.fillReason];
    if (fill.liquidity < out.fillLiquidityCounts.size()) ++out.fillLiquidityCounts[fill.liquidity];
  }
  return out;
}

void mergeRateLimitRejects(RateLimitDiagnostics& out, const EngineState& engine) noexcept {
  out.rejectedOrders += engine.rateLimitRejectedOrders;
  out.rejectedCancels += engine.rateLimitRejectedCancels;
  if (engine.firstRateLimitRejectTsNs > 0 &&
      (out.firstRejectTsNs == 0 || engine.firstRateLimitRejectTsNs < out.firstRejectTsNs)) {
    out.firstRejectTsNs = engine.firstRateLimitRejectTsNs;
  }
  if (engine.lastRateLimitRejectTsNs > out.lastRejectTsNs) out.lastRejectTsNs = engine.lastRateLimitRejectTsNs;
}

RateLimitDiagnostics collectRateLimitDiagnostics(const EngineState& engine,
                                                 const std::vector<EngineState>* legEngines) noexcept {
  RateLimitDiagnostics out{};
  if (legEngines != nullptr) {
    for (const EngineState& legEngine : *legEngines) mergeRateLimitRejects(out, legEngine);
  } else {
    mergeRateLimitRejects(out, engine);
  }
  out.usageRows = engine.rateLimitUsage.size();
  for (const JsonRateLimitUsagePoint& row : engine.rateLimitUsage) {
    if (row.exhausted) ++out.exhaustedRows;
  }
  return out;
}

std::uint64_t rejectedCommandCount(const RateLimitDiagnostics& rateLimits) noexcept {
  return rateLimits.rejectedOrders + rateLimits.rejectedCancels;
}

std::string zeroActivityReason(const BacktestRunResult& result,
                               const SignalDiagnostics& signal,
                               const RateLimitDiagnostics& rateLimits) {
  if (result.fills > 0u) return "fills_present";
  if (result.orders == 0u) {
    if (signal.strategySpreadRows > 0u &&
        signal.hasEntryEdgeBps &&
        signal.maxEdgeAfterCostBpsE8 < signal.entryEdgeBpsE8) {
      return "no_signal_below_threshold";
    }
    if (signal.strategySpreadRows > 0u) return "no_orders_from_strategy";
    return result.events == 0u ? "no_replay_events" : "no_orders";
  }
  if (rejectedCommandCount(rateLimits) > 0u) return "orders_without_fills_rate_limited";
  return "orders_without_fills";
}

void writeDecisionCounts(std::ostream& out, const std::vector<DecisionCount>& counts) {
  out << '[';
  for (std::size_t i = 0u; i < counts.size(); ++i) {
    const DecisionCount& row = counts[i];
    if (i != 0u) out << ", ";
    out << "{\"decision_kind\": " << static_cast<unsigned>(row.decisionKind)
        << ", \"reason_raw\": " << static_cast<unsigned>(row.reasonRaw)
        << ", \"count\": " << row.count << '}';
  }
  out << ']';
}

void writeDiagnosticsObject(std::ostream& out,
                            const BacktestRunResult& result,
                            const EngineState& engine,
                            const std::vector<EngineState>* legEngines,
                            std::size_t orderLifetimeRows,
                            const char* indent) {
  const SignalDiagnostics signal = collectSignalDiagnostics(engine, result.configPath);
  const ExecutionDiagnostics execution = collectExecutionDiagnostics(engine);
  const RateLimitDiagnostics rateLimits = collectRateLimitDiagnostics(engine, legEngines);
  const std::string reason = zeroActivityReason(result, signal, rateLimits);
  const char q = '"';
  out << indent << q << "diagnostics" << q << ": {\n";
  out << indent << "  " << q << "version" << q << ": 1,\n";
  out << indent << "  " << q << "zero_activity_reason" << q << ": " << quote(reason) << ",\n";
  out << indent << "  " << q << "activity" << q << ": {"
      << q << "events" << q << ": " << result.events
      << ", " << q << "orders" << q << ": " << result.orders
      << ", " << q << "fills" << q << ": " << result.fills
      << ", " << q << "order_lifetime_rows" << q << ": " << orderLifetimeRows
      << ", " << q << "strategy_spread_rows" << q << ": " << signal.strategySpreadRows
      << ", " << q << "rate_limit_usage_rows" << q << ": " << rateLimits.usageRows
      << "},\n";
  const std::uint64_t commandCount = execution.orderCommands + execution.cancelCommands;
  out << indent << "  " << q << "quality" << q << ": {"
      << q << "commands_per_fill" << q << ": " << (result.fills == 0u ? 0u : commandCount / result.fills)
      << ", " << q << "fee_drag_pct_e8" << q << ": " << ratioPctE8(result.feesPaidE8, result.grossRealizedPnlE8)
      << ", " << q << "taker_fill_ratio_pct_e8" << q << ": "
      << ratioPctE8(execution.fillLiquidityCounts[kFillLiquidityTaker], result.fills)
      << "},\n";
  out << indent << "  " << q << "signal" << q << ": {"
      << q << "strategy_spread_rows" << q << ": " << signal.strategySpreadRows
      << ", " << q << "min_edge_after_cost_bps_e8" << q << ": " << signal.minEdgeAfterCostBpsE8
      << ", " << q << "max_edge_after_cost_bps_e8" << q << ": " << signal.maxEdgeAfterCostBpsE8
      << ", " << q << "max_abs_deviation_bps_e8" << q << ": " << signal.maxAbsDeviationBpsE8
      << ", " << q << "entry_edge_bps_e8" << q << ": ";
  if (signal.hasEntryEdgeBps) out << signal.entryEdgeBpsE8;
  else out << "null";
  out << ", " << q << "threshold_source" << q << ": " << q
      << (signal.hasEntryEdgeBps ? "config.entry_edge_bps" : "none") << q
      << ", " << q << "decision_counts" << q << ": ";
  writeDecisionCounts(out, signal.decisionCounts);
  out << "},\n";
  out << indent << "  " << q << "execution" << q << ": {"
      << q << "order_commands" << q << ": " << execution.orderCommands
      << ", " << q << "cancel_commands" << q << ": " << execution.cancelCommands
      << ", " << q << "post_only_orders" << q << ": " << execution.postOnlyOrders
      << ", " << q << "service_orders" << q << ": " << execution.serviceOrders
      << ", " << q << "warmup_orders" << q << ": " << execution.warmupOrders
      << ", " << q << "order_status" << q << ": {"
      << q << "none" << q << ": " << execution.statusCounts[kRecordStatusNone]
      << ", " << q << "pending" << q << ": " << execution.statusCounts[kRecordStatusPending]
      << ", " << q << "acked" << q << ": " << execution.statusCounts[kRecordStatusAcked]
      << ", " << q << "filled" << q << ": " << execution.statusCounts[kRecordStatusFilled]
      << ", " << q << "cancelled" << q << ": " << execution.statusCounts[kRecordStatusCancelled]
      << ", " << q << "missing" << q << ": " << execution.statusCounts[kRecordStatusMissing]
      << ", " << q << "rejected" << q << ": " << execution.statusCounts[kRecordStatusRejected]
      << ", " << q << "partial_unfilled" << q << ": " << execution.statusCounts[kRecordStatusPartialUnfilled]
      << "}, " << q << "order_types" << q << ": {"
      << q << "none" << q << ": " << execution.typeCounts[kRecordTypeNone]
      << ", " << q << "limit" << q << ": " << execution.typeCounts[kRecordTypeLimit]
      << ", " << q << "market" << q << ": " << execution.typeCounts[kRecordTypeMarket]
      << ", " << q << "stop_market" << q << ": " << execution.typeCounts[kRecordTypeStopMarket]
      << "}, " << q << "fills_by_reason" << q << ": {"
      << q << "unknown" << q << ": " << execution.fillReasonCounts[kFillReasonUnknown]
      << ", " << q << "top" << q << ": " << execution.fillReasonCounts[kFillReasonTop]
      << ", " << q << "trade" << q << ": " << execution.fillReasonCounts[kFillReasonTrade]
      << ", " << q << "aggressive_book" << q << ": " << execution.fillReasonCounts[kFillReasonAggressiveBook]
      << ", " << q << "passive_queue" << q << ": " << execution.fillReasonCounts[kFillReasonPassiveQueue]
      << ", " << q << "passive_bbo_touch" << q << ": " << execution.fillReasonCounts[kFillReasonPassiveBboTouch]
      << "}, " << q << "fills_by_liquidity" << q << ": {"
      << q << "maker" << q << ": " << execution.fillLiquidityCounts[kFillLiquidityMaker]
      << ", " << q << "taker" << q << ": " << execution.fillLiquidityCounts[kFillLiquidityTaker]
      << "}},\n";
  out << indent << "  " << q << "rate_limits" << q << ": {"
      << q << "rate_limit_rejected_orders" << q << ": " << rateLimits.rejectedOrders
      << ", " << q << "rate_limit_rejected_cancels" << q << ": " << rateLimits.rejectedCancels
      << ", " << q << "rate_limit_rejected_commands" << q << ": " << rejectedCommandCount(rateLimits)
      << ", " << q << "first_reject_ts_ns" << q << ": " << rateLimits.firstRejectTsNs
      << ", " << q << "last_reject_ts_ns" << q << ": " << rateLimits.lastRejectTsNs
      << ", " << q << "usage_rows" << q << ": " << rateLimits.usageRows
      << ", " << q << "exhausted_rows" << q << ": " << rateLimits.exhaustedRows
      << "},\n";
  out << indent << "  " << q << "diagnostic_notes" << q << ": [";
  bool firstNote = true;
  auto appendNote = [&](std::string_view message) {
    if (!firstNote) out << ", ";
    firstNote = false;
    out << '{' << q << "message" << q << ": " << quote(message) << '}';
  };
  if (reason == "no_signal_below_threshold") {
    appendNote("max edge_after_cost_bps_e8 is below configured entry_edge_bps; zero orders are expected for these parameters");
  } else if (reason == "orders_without_fills" || reason == "orders_without_fills_rate_limited") {
    appendNote("orders were placed but no fills were produced by the strict inline fill model");
    appendNote("passive maker fills require a later opposite-side trade crossing the resting limit price");
  } else if (reason == "no_orders_from_strategy") {
    appendNote("strategy emitted spread diagnostics but did not submit order intents");
  } else if (reason == "no_orders") {
    appendNote("replay had events but produced no order records");
  }
  if (rejectedCommandCount(rateLimits) > 0u) {
    appendNote("rate-limit rejects were recorded; inspect rate_limit_usage and strategy decision counts before changing fill policy");
  }
  out << "]\n";
  out << indent << '}';
}

}  // namespace hft_backtest::detail::result_writer

