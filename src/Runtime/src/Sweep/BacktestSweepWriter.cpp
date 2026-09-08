#include "BacktestSweepInternal.hpp"

#include "../BacktestEngine.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

namespace hft_backtest::detail {
namespace {

const char* paramModeName(BacktestSweepParamMode mode) noexcept {
  switch (mode) {
    case BacktestSweepParamMode::Grid: return "grid";
  }
  return "grid";
}

const char* depthModeName(BacktestDepthExecutionMode mode) noexcept {
  switch (mode) {
    case BacktestDepthExecutionMode::BboNoDepth: return "bbo_no_depth";
    case BacktestDepthExecutionMode::Depth: return "depth";
    case BacktestDepthExecutionMode::BboFallback: return "bbo_fallback";
  }
  return "bbo_no_depth";
}

void writeDepthSummary(std::ofstream& out, const BacktestDepthExecutionSummary& depth) {
  const char q = '"';
  out << '{'
      << q << "mode" << q << ':' << quote(depthModeName(depth.mode)) << ','
      << q << "rows_read" << q << ':' << depth.rowsRead << ','
      << q << "bbo_fallback_fills" << q << ':' << depth.bboFallbackFills << ','
      << q << "first_bbo_fallback_ts_ns" << q << ':' << depth.firstBboFallbackTsNs << ','
      << q << "read_ns" << q << ':' << depth.readNs
      << '}';
}

void writeRunPerformance(std::ofstream& out, const BacktestStageTimings& performance) {
  const char q = '"';
  out << '{'
      << q << "load_ns" << q << ':' << performance.loadNs << ','
      << q << "timeline_ns" << q << ':' << performance.timelineNs << ','
      << q << "delivery_ns" << q << ':' << performance.deliveryNs << ','
      << q << "replay_ns" << q << ':' << performance.replayNs << ','
      << q << "finalize_ns" << q << ':' << performance.finalizeNs << ','
      << q << "artifact_write_ns" << q << ':' << performance.artifactWriteNs << ','
      << q << "total_ns" << q << ':' << performance.totalNs
      << '}';
}

std::filesystem::path baseSessionPath(const BacktestSweepRequest& request) {
  if (!request.baseRun.corpusPath.empty()) return request.baseRun.corpusPath;
  if (!request.baseRun.sessionPath.empty()) return request.baseRun.sessionPath;
  if (!request.baseRun.sessions.empty()) return request.baseRun.sessions[0].path;
  return {};
}

void writeSweepLegs(std::ofstream& out, const BacktestRunResult& run, bool includeCurves) {
  const char q = '"';
  out << q << "legs" << q << ":[";
  for (std::size_t i = 0u; i < run.legs.size(); ++i) {
    const BacktestLegSummary& leg = run.legs[i];
    if (i != 0u) out << ',';
    out << '{';
    out << q << "leg_index" << q << ':' << i << ',';
    out << q << "session_path" << q << ':' << quote(leg.sessionPath.generic_string()) << ',';
    out << q << "exchange" << q << ':' << quote(leg.exchange) << ',';
    out << q << "market" << q << ':' << quote(leg.market) << ',';
    out << q << "symbol" << q << ':' << quote(leg.symbol) << ',';
    out << q << "initial_balance_e8" << q << ':' << leg.initialBalanceE8 << ',';
    out << q << "wallet_balance_e8" << q << ':' << leg.walletBalanceE8 << ',';
    out << q << "orders" << q << ':' << leg.orders << ',';
    out << q << "fills" << q << ':' << leg.fills << ',';
    out << q << "reduce_only_orders" << q << ':' << leg.reduceOnlyOrders << ',';
    out << q << "reduce_only_fills" << q << ':' << leg.reduceOnlyFills << ',';
    out << q << "total_pnl_e8" << q << ':' << leg.totalPnlE8;
    out << ',' << q << "depth_execution" << q << ':';
    writeDepthSummary(out, leg.depthExecution);
    if (includeCurves) {
      out << ',' << q << "curve_e8" << q << ":[";
      for (std::size_t c = 0u; c < leg.totalPnlCurveE8.size(); ++c) {
        if (c != 0u) out << ',';
        out << leg.totalPnlCurveE8[c];
      }
      out << ']';
    }
    out << '}';
  }
  out << ']';
}

void writeSweepManifestLegs(std::ofstream& out,
                            const BacktestSweepRequest& request,
                            const std::vector<BacktestSweepPointResult>& points) {
  const char q = '"';
  for (const BacktestSweepPointResult& point : points) {
    if (point.run.legs.empty()) continue;
    out << "  " << q << "legs" << q << ": [";
    for (std::size_t i = 0u; i < point.run.legs.size(); ++i) {
      const BacktestLegSummary& leg = point.run.legs[i];
      std::uint64_t fallbackPoints = 0u;
      std::uint64_t rowsRead = 0u;
      std::uint64_t fallbackFills = 0u;
      std::uint64_t readNs = 0u;
      std::int64_t firstFallbackTsNs = 0;
      for (const BacktestSweepPointResult& aggregatePoint : points) {
        if (i >= aggregatePoint.run.legs.size()) continue;
        const BacktestDepthExecutionSummary& depth = aggregatePoint.run.legs[i].depthExecution;
        if (depth.mode == BacktestDepthExecutionMode::BboFallback) ++fallbackPoints;
        rowsRead += depth.rowsRead;
        fallbackFills += depth.bboFallbackFills;
        readNs += depth.readNs;
        if (depth.firstBboFallbackTsNs != 0 &&
            (firstFallbackTsNs == 0 || depth.firstBboFallbackTsNs < firstFallbackTsNs)) {
          firstFallbackTsNs = depth.firstBboFallbackTsNs;
        }
      }
      if (i != 0u) out << ", ";
      out << '{'
          << q << "leg_index" << q << ": " << i << ", "
          << q << "session_path" << q << ": " << quote(leg.sessionPath.generic_string()) << ", "
          << q << "exchange" << q << ": " << quote(leg.exchange) << ", "
          << q << "market" << q << ": " << quote(leg.market) << ", "
          << q << "symbol" << q << ": " << quote(leg.symbol) << ", "
          << q << "depth_execution" << q << ": {"
          << q << "fallback_points" << q << ": " << fallbackPoints << ", "
          << q << "rows_read_sum" << q << ": " << rowsRead << ", "
          << q << "bbo_fallback_fills_sum" << q << ": " << fallbackFills << ", "
          << q << "first_bbo_fallback_ts_ns" << q << ": " << firstFallbackTsNs << ", "
          << q << "read_ns_sum" << q << ": " << readNs << '}'
          << '}';
    }
    out << "],\n";
    return;
  }

  out << "  " << q << "legs" << q << ": [";
  bool wrote = false;
  auto writeFallbackLeg = [&](std::size_t index,
                              const std::filesystem::path& path,
                              const std::string& exchange,
                              const std::string& market,
                              const std::string& symbol) {
    if (path.empty()) return;
    if (wrote) out << ", ";
    out << '{'
        << q << "leg_index" << q << ": " << index << ", "
        << q << "session_path" << q << ": " << quote(path.generic_string()) << ", "
        << q << "exchange" << q << ": " << quote(exchange) << ", "
        << q << "market" << q << ": " << quote(market) << ", "
        << q << "symbol" << q << ": " << quote(symbol)
        << '}';
    wrote = true;
  };
  std::size_t index = 0u;
  for (const BacktestCorpusSourceRequest& source :
       request.baseRun.corpusSources) {
    writeFallbackLeg(index++, request.baseRun.corpusPath,
                     source.exchange, source.market,
                     source.canonicalSymbol);
  }
  if (!request.baseRun.sessionPath.empty())
    writeFallbackLeg(index++, request.baseRun.sessionPath, {}, {}, {});
  for (const BacktestSessionRequest& session : request.baseRun.sessions) {
    writeFallbackLeg(index++, session.path, session.venue, {}, session.symbol);
  }
  out << "],\n";
}

}  // namespace

std::filesystem::path sweepResultPathFor(const BacktestSweepRequest& request, std::string_view sweepId) {
  if (!request.outputPath.empty()) return request.outputPath;
  return baseSessionPath(request) / "backtests" / "sweeps" / std::string{sweepId};
}

Status writeSweepRows(const BacktestSweepRequest& request,
                      const BacktestSweepResult& result,
                      const std::vector<BacktestSweepPointResult>& points) noexcept {
  std::ofstream out(result.rowsPath, std::ios::out | std::ios::trunc);
  if (!out.is_open()) return Status::IoError;
  const char q = '"';
  for (const BacktestSweepPointResult& point : points) {
    out << '{';
    out << q << "point_id" << q << ':' << point.point.pointId << ',';
    out << q << "params" << q << ":{";
    for (std::size_t i = 0u; i < request.ranges.size(); ++i) {
      if (i != 0u) out << ',';
      out << q << request.ranges[i].key << q << ':' << point.point.values[i];
    }
    out << "},";
    out << q << "status" << q << ':' << quote(std::string{statusToString(point.run.status)}) << ',';
    out << q << "total_pnl_e8" << q << ':' << point.run.totalPnlE8 << ',';
    out << q << "max_drawdown_e8" << q << ':' << point.run.maxDrawdownE8 << ',';
    out << q << "mfe_e8" << q << ':' << point.run.mfeE8 << ',';
    out << q << "mae_e8" << q << ':' << point.run.maeE8 << ',';
    out << q << "orders" << q << ':' << point.run.orders << ',';
    out << q << "fills" << q << ':' << point.run.fills << ',';
    out << q << "risk_stopped" << q << ':' << (point.run.riskStopped ? "true" : "false") << ',';
    out << q << "liquidated" << q << ':' << (point.run.liquidated ? "true" : "false") << ',';
    out << q << "performance" << q << ':';
    writeRunPerformance(out, point.run.performance);
    out << ',' << q << "depth_execution" << q << ':';
    writeDepthSummary(out, point.run.depthExecution);
    out << ',';
    writeSweepLegs(out, point.run, false);
    if (!point.run.error.empty()) out << ',' << q << "error" << q << ':' << quote(point.run.error);
    out << "}\n";
  }
  return out.good() ? Status::Ok : Status::IoError;
}

Status writeSweepCurves(const BacktestSweepRequest& request,
                        const BacktestSweepResult& result,
                        const std::vector<BacktestSweepPointResult>& points) noexcept {
  std::ofstream out(result.curvesPath, std::ios::out | std::ios::trunc);
  if (!out.is_open()) return Status::IoError;
  const char q = '"';
  for (const BacktestSweepPointResult& point : points) {
    out << '{';
    out << q << "point_id" << q << ':' << point.point.pointId << ',';
    out << q << "params" << q << ":{";
    for (std::size_t i = 0u; i < request.ranges.size(); ++i) {
      if (i != 0u) out << ',';
      out << q << request.ranges[i].key << q << ':' << point.point.values[i];
    }
    out << "},";
    out << q << "status" << q << ':' << quote(std::string{statusToString(point.run.status)}) << ',';
    out << q << "initial_balance_e8" << q << ':' << point.run.initialBalanceE8 << ',';
    out << q << "total_pnl_e8" << q << ':' << point.run.totalPnlE8 << ',';
    out << q << "max_drawdown_e8" << q << ':' << point.run.maxDrawdownE8 << ',';
    out << q << "mfe_e8" << q << ':' << point.run.mfeE8 << ',';
    out << q << "mae_e8" << q << ':' << point.run.maeE8 << ',';
    out << q << "orders" << q << ':' << point.run.orders << ',';
    out << q << "fills" << q << ':' << point.run.fills << ',';
    out << q << "risk_stopped" << q << ':' << (point.run.riskStopped ? "true" : "false") << ',';
    out << q << "liquidated" << q << ':' << (point.run.liquidated ? "true" : "false") << ',';
    out << q << "performance" << q << ':';
    writeRunPerformance(out, point.run.performance);
    out << ',' << q << "depth_execution" << q << ':';
    writeDepthSummary(out, point.run.depthExecution);
    out << ',';
    out << q << "curve_e8" << q << ":[";
    for (std::size_t i = 0u; i < point.run.totalPnlCurveE8.size(); ++i) {
      if (i != 0u) out << ',';
      out << point.run.totalPnlCurveE8[i];
    }
    out << "],";
    writeSweepLegs(out, point.run, true);
    if (!point.run.error.empty()) out << ',' << q << "error" << q << ':' << quote(point.run.error);
    out << "}\n";
  }
  return out.good() ? Status::Ok : Status::IoError;
}

Status writeSweepManifest(const BacktestSweepRequest& request,
                          const BacktestSweepResult& result,
                          const std::vector<BacktestSweepPointResult>& points) noexcept {
  std::ofstream out(result.manifestPath, std::ios::out | std::ios::trunc);
  if (!out.is_open()) return Status::IoError;
  const char q = '"';
  out << "{\n";
  out << "  " << q << "type" << q << ": " << q << "sweep.result.v1" << q << ",\n";
  out << "  " << q << "schema_version" << q << ": 1,\n";
  out << "  " << q << "mode" << q << ": " << q << "whole_session_exploration" << q << ",\n";
  out << "  " << q << "sweep_id" << q << ": " << quote(result.sweepId) << ",\n";
  out << "  " << q << "session_path" << q << ": " << quote(baseSessionPath(request).generic_string()) << ",\n";
  out << "  " << q << "config_path" << q << ": " << quote(request.baseRun.configPath.generic_string()) << ",\n";
  out << "  " << q << "strategy" << q << ": " << quote(request.baseRun.strategy) << ",\n";
  writeSweepManifestLegs(out, request, points);
  out << "  " << q << "budget" << q << ": " << request.runBudget << ",\n";
  out << "  " << q << "search_seed" << q << ": " << request.searchSeed << ",\n";
  out << "  " << q << "points_evaluated" << q << ": " << points.size() << ",\n";
  std::uint64_t fallbackPoints = 0u;
  for (const BacktestSweepPointResult& point : points) {
    if (point.run.depthExecution.mode == BacktestDepthExecutionMode::BboFallback) ++fallbackPoints;
  }
  out << "  " << q << "performance" << q << ": {"
      << q << "worker_count" << q << ": " << result.workerCount << ", "
      << q << "prepare_ns" << q << ": " << result.prepareNs << ", "
      << q << "points_wall_ns" << q << ": " << result.pointsWallNs << ", "
      << q << "points_cpu_sum_ns" << q << ": " << result.pointsCpuSumNs << ", "
      << q << "artifact_write_ns" << q << ": " << result.artifactWriteNs << ", "
      << q << "total_ns" << q << ": " << result.totalNs << "},\n";
  out << "  " << q << "depth_execution" << q << ": {"
      << q << "fallback_points" << q << ": " << fallbackPoints << "},\n";
  out << "  " << q << "rows" << q << ": {" << q << "path" << q << ": " << q << "sweep_results.jsonl" << q << ", "
      << q << "row_schema" << q << ": " << q << "object.v1" << q << "},\n";
  out << "  " << q << "curves" << q << ": {" << q << "path" << q << ": " << q << "sweep_curves.jsonl" << q << ", "
      << q << "row_schema" << q << ": " << q << "object.v1" << q << "},\n";
  out << "  " << q << "ranges" << q << ": [";
  for (std::size_t i = 0u; i < request.ranges.size(); ++i) {
    const BacktestSweepParamRange& range = request.ranges[i];
    if (i != 0u) out << ", ";
    out << '{' << q << "key" << q << ": " << quote(range.key)
        << ", " << q << "mode" << q << ": " << quote(paramModeName(range.mode))
        << ", " << q << "min_raw" << q << ": " << range.minRaw
        << ", " << q << "max_raw" << q << ": " << range.maxRaw
        << ", " << q << "step_raw" << q << ": " << range.stepRaw << '}';
  }
  out << "],\n";
  out << "  " << q << "errors" << q << ": ";
  if (result.error.empty()) out << "[]\n";
  else out << "[{" << q << "message" << q << ": " << quote(result.error) << "}]\n";
  out << "}\n";
  return out.good() ? Status::Ok : Status::IoError;
}

}  // namespace hft_backtest::detail
