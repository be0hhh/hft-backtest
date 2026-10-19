#pragma once

#include "trading_core/Contracts/Numeric/Integer/Saturating.hpp"
#include "../../Engine/BacktestEngine.hpp"

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
#include <utility>

#include "absl/container/flat_hash_map.h"


namespace hft_backtest::detail::result_writer {

constexpr const char* kFillsRowSchema =
    "[order_id,entry_ts_ns,exit_ts_ns,side,price_e8,qty_e8,realized_pnl_e8,reduce_only,leg_index,"
    "fill_reason,liquidity,order_qty_e8,cumulative_filled_qty_e8,remaining_qty_e8,avg_price_e8,"
    "book_level_qty_e8,book_visible_executable_qty_e8,book_consumed_pct_e8,queue_ahead_before_e8,queue_ahead_after_e8,"
    "chunk_index,chunk_count,execution_qty_e8,execution_avg_price_e8,reference_price_e8,slippage_e8,"
    "slippage_bps_e8,execution_book_consumed_pct_e8,context_index]";

constexpr const char* kRateLimitUsageRowSchema =
    "[ts_ns,leg_index,min_remaining_pct_e4,remaining,limit,bucket_kind,interval_ns,exhausted]";

constexpr const char* kExecutionQualityRowSchema =
    "[action_id,position_cycle_id,transaction_id,pair_key,decision_ts_ns,completed_ts_ns,phase,anchor,status,"
    "direction,band_side,decision_edge_after_cost_bps_e8,fill_edge_after_cost_bps_e8,edge_loss_bps_e8,"
    "required_edge_bps_e8,fair_bps_e8,cost_band_bps_e8,quote_decision_edge_after_cost_bps_e8,"
    "decision_to_first_fill_ns,decision_to_last_fill_ns,fill_leg_skew_ns,threshold_deadline_ns,zero_edge_deadline_ns,"
    "pair_index,pair_slot,leg0_index,leg0_client_intent_id,leg0_side,leg0_expected_qty_e8,leg0_filled_qty_e8,"
    "leg0_decision_price_e8,leg0_fill_vwap_e8,leg0_first_fill_ts_ns,leg0_last_fill_ts_ns,"
    "leg1_index,leg1_client_intent_id,leg1_side,leg1_expected_qty_e8,leg1_filled_qty_e8,"
    "leg1_decision_price_e8,leg1_fill_vwap_e8,leg1_first_fill_ts_ns,leg1_last_fill_ts_ns]";

constexpr const char* kStrategyIndicatorBaseRowSchema =
    "[ts_ns,value_raw,aux_raw,event_code,reason_raw,decision_kind,side_raw]";
constexpr const char* kTrendStrategyIndicatorRowSchema =
    "[ts_ns,value_raw,aux_raw,event_code,reason_raw,decision_kind,side_raw,"
    "trend_fast_score,trend_medium_score,trend_slow_score,trend_acceleration_score,"
    "trend_confidence_pct,trend_normal_move_bps]";
constexpr const char* kVolatilityStrategyIndicatorRowSchema =
    "[ts_ns,value_raw,aux_raw,event_code,reason_raw,decision_kind,side_raw,"
    "volatility_fast_bps,volatility_baseline_bps,volatility_shock_bps,"
    "volatility_quote_move_bps,volatility_spread_bps,volatility_confidence_pct,"
    "volatility_quote_confidence_pct]";
constexpr const char* kToxicFlowStrategyIndicatorRowSchema =
    "[ts_ns,value_raw,aux_raw,event_code,reason_raw,decision_kind,side_raw,"
    "toxic_pressure_score,toxic_touch_score,toxic_follow_score,toxic_imbalance_score,"
    "toxic_depth_drop_score,toxic_bid_depth_drop_score,toxic_ask_depth_drop_score,"
    "toxic_buy_pressure_score,toxic_sell_pressure_score,toxic_confidence_pct]";

constexpr std::int64_t kBpsE8Scale = 100000000ll;
constexpr std::size_t kMaxDecisionDiagnostics = 8u;

struct DecisionCount {
  std::uint8_t decisionKind{0u};
  std::uint8_t reasonRaw{0u};
  std::uint64_t count{0u};
};
struct SignalDiagnostics {
  std::uint64_t strategySpreadRows{0u};
  std::int64_t minEdgeAfterCostBpsE8{0};
  std::int64_t maxEdgeAfterCostBpsE8{0};
  std::int64_t maxAbsDeviationBpsE8{0};
  std::int64_t entryEdgeBpsE8{0};
  bool hasEntryEdgeBps{false};
  std::vector<DecisionCount> decisionCounts{};
};
struct ExecutionDiagnostics {
  std::uint64_t orderCommands{0u};
  std::uint64_t cancelCommands{0u};
  std::uint64_t postOnlyOrders{0u};
  std::uint64_t serviceOrders{0u};
  std::uint64_t warmupOrders{0u};
  std::array<std::uint64_t, 8> statusCounts{};
  std::array<std::uint64_t, 4> typeCounts{};
  std::array<std::uint64_t, kFillReasonCount> fillReasonCounts{};
  std::array<std::uint64_t, 2> fillLiquidityCounts{};
};
struct RateLimitDiagnostics {
  std::uint64_t rejectedOrders{0u};
  std::uint64_t rejectedCancels{0u};
  std::uint64_t usageRows{0u};
  std::uint64_t exhaustedRows{0u};
  std::int64_t firstRejectTsNs{0};
  std::int64_t lastRejectTsNs{0};
};
struct OrderLifetimeRow {
  std::int64_t tsStartNs{0};
  std::int64_t tsEndNs{0};
  std::int64_t priceE8{0};
  std::int64_t qtyE8{0};
  std::uint8_t side{kRecordSideNone};
  bool openEnded{false};
  std::uint32_t legIndex{0u};
  std::uint8_t orderType{kRecordTypeLimit};
};
using OrderLifetimeKey = std::pair<std::uint64_t, std::uint64_t>;
struct OrderLifetimeIndexes {
  absl::flat_hash_map<OrderLifetimeKey, const JsonFill*> fillByOrderId{};
  absl::flat_hash_map<OrderLifetimeKey, std::vector<const JsonOrder*>> commandsByTargetId{};
};

std::string_view trim(std::string_view value) noexcept;
std::string_view stripIniComment(std::string_view value) noexcept;
bool parseDecimalBpsE8(std::string_view value, std::int64_t& out) noexcept;
bool readConfigBpsE8(const std::filesystem::path& path,
                     std::string_view key,
                     std::int64_t& out) noexcept;
std::int64_t absI64(std::int64_t value) noexcept;
std::int64_t ratioPctE8(std::uint64_t numerator, std::uint64_t denominator) noexcept;
std::int64_t ratioPctE8(std::int64_t numerator, std::int64_t denominator) noexcept;
void sortAndTrimDecisionCounts(std::vector<DecisionCount>& counts);
SignalDiagnostics collectSignalDiagnostics(const EngineState& engine,
                                           const std::filesystem::path& configPath);
ExecutionDiagnostics collectExecutionDiagnostics(const EngineState& engine) noexcept;
void mergeRateLimitRejects(RateLimitDiagnostics& out, const EngineState& engine) noexcept;
RateLimitDiagnostics collectRateLimitDiagnostics(const EngineState& engine,
                                                 const std::vector<EngineState>* legEngines) noexcept;
std::uint64_t rejectedCommandCount(const RateLimitDiagnostics& rateLimits) noexcept;
std::string zeroActivityReason(const BacktestRunResult& result,
                               const SignalDiagnostics& signal,
                               const RateLimitDiagnostics& rateLimits);
void writeDecisionCounts(std::ostream& out, const std::vector<DecisionCount>& counts);
void writeDiagnosticsObject(std::ostream& out,
                            const BacktestRunResult& result,
                            const EngineState& engine,
                            const std::vector<EngineState>* legEngines,
                            std::size_t orderLifetimeRows,
                            const char* indent);
bool isEffectiveOrderStatus(std::uint8_t status) noexcept;
bool isOpenOrderStatus(std::uint8_t status) noexcept;
void buildOrderLifetimeIndexes(const EngineState& engine, OrderLifetimeIndexes& out);
const JsonFill* lifetimeFill(const OrderLifetimeIndexes& indexes, OrderLifetimeKey orderId) noexcept;
std::int64_t lifetimeCommandEndTs(const OrderLifetimeIndexes& indexes,
                                  OrderLifetimeKey orderId,
                                  std::int64_t startTsNs) noexcept;
bool hasSameTimestampLifetimeReplaceEnd(const OrderLifetimeIndexes& indexes,
                                        OrderLifetimeKey orderId,
                                        std::int64_t startTsNs) noexcept;
std::vector<OrderLifetimeRow> materializeOrderLifetimes(const EngineState& engine);
bool writeOrderLifetimes(const std::filesystem::path& path,
                         const std::vector<OrderLifetimeRow>& lifetimes) noexcept;
bool writeFills(const std::filesystem::path& path, const EngineState& engine) noexcept;
void writeEquityRow(std::ostream& out, const JsonEquityPoint& row);
bool writeEquity(const std::filesystem::path& path, const EngineState& engine) noexcept;
JsonEquityPoint syntheticLegEquityPoint(const BacktestLegSummary& leg) noexcept;
bool writeSyntheticLegEquity(const std::filesystem::path& path, const BacktestLegSummary& leg) noexcept;
bool writeRiskSnapshots(const std::filesystem::path& path, const EngineState& engine) noexcept;
bool writeStrategyIndicator(const std::filesystem::path& path, const EngineState& engine) noexcept;
bool writeStrategyRange(const std::filesystem::path& path, const EngineState& engine) noexcept;
bool writeStrategySpread(const std::filesystem::path& path, const EngineState& engine) noexcept;
bool writeExecutionQuality(const std::filesystem::path& path, const EngineState& engine) noexcept;
bool writeRateLimitUsage(const std::filesystem::path& path, const EngineState& engine) noexcept;
bool writeRunConfig(const std::filesystem::path& resultDir, const std::filesystem::path& configPath) noexcept;
bool writeLegEquityStreams(const std::filesystem::path& resultDir,
                           const BacktestRunResult& result,
                           const std::vector<EngineState>* legEngines) noexcept;
std::size_t strategyIndicatorFeatureCount(std::string_view profile) noexcept;
const char* strategyIndicatorRowSchema(std::string_view profile) noexcept;
const char* resultStatusName(Status status) noexcept;
const char* rateLimitModeName(const BacktestRunResult& result) noexcept;
const char* tradeModeName(BacktestTradeMode mode) noexcept;
const char* depthExecutionModeName(BacktestDepthExecutionMode mode) noexcept;
void writeDepthExecutionObject(std::ostream& out,
                               const BacktestDepthExecutionSummary& depth);
void writeSummaryObject(std::ostream& out, const BacktestRunResult& result, const EngineState& engine, const char* indent);
void writeReplayClockObject(std::ostream& out,
                            const BacktestReplayClockEvidence& clock);
void writeLegsArray(std::ostream& out, const BacktestRunResult& result, const char* indent);
void writeMessageArray(std::ostream& out, const std::vector<std::string>& messages);
bool writeManifest(const std::filesystem::path& path,
                   const BacktestRunResult& result,
                   const EngineState& engine,
                   const std::vector<EngineState>* legEngines,
                   std::size_t orderLifetimeRows) noexcept;

}  // namespace hft_backtest::detail::result_writer
