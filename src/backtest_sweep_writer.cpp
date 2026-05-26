#include "backtest_sweep_internal.hpp"

#include "backtest_engine.hpp"

#include <fstream>
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

}  // namespace

std::filesystem::path sweepResultPathFor(const BacktestSweepRequest& request, std::string_view sweepId) {
  if (!request.outputPath.empty()) return request.outputPath;
  return request.baseRun.sessionPath / "backtests" / "sweeps" / std::string{sweepId};
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
    out << q << "total_pnl_e8" << q << ':' << point.run.totalPnlE8;
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
    out << q << "curve_e8" << q << ":[";
    for (std::size_t i = 0u; i < point.run.totalPnlCurveE8.size(); ++i) {
      if (i != 0u) out << ',';
      out << point.run.totalPnlCurveE8[i];
    }
    out << ']';
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
  out << "  " << q << "session_path" << q << ": " << quote(request.baseRun.sessionPath.generic_string()) << ",\n";
  out << "  " << q << "config_path" << q << ": " << quote(request.baseRun.configPath.generic_string()) << ",\n";
  out << "  " << q << "strategy" << q << ": " << quote(request.baseRun.strategy) << ",\n";
  out << "  " << q << "budget" << q << ": " << request.runBudget << ",\n";
  out << "  " << q << "search_seed" << q << ": " << request.searchSeed << ",\n";
  out << "  " << q << "points_evaluated" << q << ": " << points.size() << ",\n";
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
