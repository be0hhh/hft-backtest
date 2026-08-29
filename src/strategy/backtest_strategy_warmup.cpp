#include "strategy/backtest_strategy_warmup.hpp"

#include "strategy/backtest_strategy_runtime.hpp"

#include <cstdint>
#include <vector>

namespace hft_backtest {
namespace {

bool buildTradeWarmupRows(const detail::SessionData& session,
                          std::vector<cxet::composite::TradeRuntimeV1>& out,
                          trading_core::StrategyTradeHistorySeedStats& stats) {
  out.clear();
  out.reserve(session.tradeWarmup.size());
  stats = trading_core::StrategyTradeHistorySeedStats{};
  stats.rowsFetched = static_cast<std::uint64_t>(session.tradeHistoryRows > 0 ? session.tradeHistoryRows : static_cast<std::int64_t>(session.tradeWarmup.size()));
  stats.rowsProvided = static_cast<std::uint64_t>(session.tradeWarmup.size());
  stats.firstTs = timeNsFromI64(session.tradeWarmup.empty() ? 0 : session.tradeWarmup.front().tsNs);
  stats.lastTs = timeNsFromI64(session.tradeHistoryRequestedEndNs > 0 ? session.tradeHistoryRequestedEndNs : (session.tradeWarmup.empty() ? 0 : session.tradeWarmup.back().tsNs));
  for (const detail::TradeRow& row : session.tradeWarmup) out.push_back(detail::toRuntime(row));
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
  std::vector<cxet::composite::TradeRuntimeV1> rows{};
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
