#include "hft_backtest/backtest_sweep.hpp"

#include "backtest_prepared.hpp"
#include "backtest_sweep_internal.hpp"

#include <chrono>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <utility>

namespace hft_backtest {
namespace {

std::string makeSweepId() {
  const auto nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  return "sweep-" + std::to_string(nowNs);
}

bool reportSweepProgress(std::uint64_t done,
                         std::uint64_t total,
                         BacktestProgressCallback cb,
                         void* userData) noexcept {
  if (!cb) return true;
  BacktestProgress progress{};
  progress.eventsDone = done;
  progress.eventsTotal = total;
  progress.percent = total == 0u ? 0u : static_cast<std::uint32_t>((done * 100u) / total);
  if (progress.percent > 100u) progress.percent = 100u;
  progress.stage = "sweeping";
  return cb(progress, userData);
}

bool readBaseConfig(const std::filesystem::path& path, std::string& out) noexcept {
  std::ifstream in(path, std::ios::in | std::ios::binary);
  if (!in.is_open()) return false;
  out.assign(std::istreambuf_iterator<char>{in}, std::istreambuf_iterator<char>{});
  return in.good() || in.eof();
}

bool writePointConfig(const std::string& baseConfig,
                      const BacktestSweepRequest& request,
                      const detail::BacktestSweepPoint& point,
                      const std::filesystem::path& path) noexcept {
  std::ofstream out(path, std::ios::out | std::ios::trunc | std::ios::binary);
  if (!out.is_open()) return false;
  out << baseConfig;
  out << "\n[strategy]\n";
  for (std::size_t i = 0u; i < request.ranges.size(); ++i) {
    out << request.ranges[i].key << '=' << point.values[i] << '\n';
  }
  return out.good();
}

bool validateSweepRequest(const BacktestSweepRequest& request, std::string& error) {
  if ((request.baseRun.sessionPath.empty() && request.baseRun.sessions.empty()) || request.baseRun.configPath.empty()) {
    error = "session_path and config_path are required";
    return false;
  }
  if (request.ranges.empty()) {
    error = "at least one sweep range is required";
    return false;
  }
  if (request.runBudget == 0u) {
    error = "run_budget must be greater than zero";
    return false;
  }
  for (const BacktestSweepParamRange& range : request.ranges) {
    if (range.key.empty()) {
      error = "sweep range key is required";
      return false;
    }
    if (range.stepRaw <= 0 || range.maxRaw < range.minRaw) {
      error = "invalid sweep range bounds";
      return false;
    }
    if (range.mode != BacktestSweepParamMode::Grid) {
      error = "invalid sweep range mode";
      return false;
    }
  }
  return true;
}

}  // namespace

BacktestSweepResult runBacktestSweep(const BacktestSweepRequest& request,
                                     BacktestProgressCallback progressCallback,
                                     void* userData) noexcept {
  BacktestSweepResult result{};
  result.sweepId = request.sweepId.empty() ? makeSweepId() : request.sweepId;
  result.resultPath = detail::sweepResultPathFor(request, result.sweepId);
  result.rowsPath = result.resultPath / "sweep_results.jsonl";
  result.curvesPath = result.resultPath / "sweep_curves.jsonl";
  result.manifestPath = result.resultPath / "manifest.json";

  std::string error{};
  if (!validateSweepRequest(request, error)) {
    result.status = Status::InvalidArgument;
    result.error = error;
    return result;
  }
  std::error_code ec;
  std::filesystem::create_directories(result.resultPath / "point_configs", ec);
  if (ec) {
    result.status = Status::IoError;
    result.error = "failed to create sweep output directory";
    return result;
  }

  const std::vector<detail::BacktestSweepPoint> plan = detail::buildSweepSearchPlan(request);
  if (plan.empty()) {
    result.status = Status::InvalidArgument;
    result.error = "sweep search plan is empty";
    return result;
  }
  std::vector<detail::BacktestSweepPointResult> points{};
  points.reserve(plan.size());
  detail::PreparedBacktestInput preparedInput{};
  detail::PreparedBacktestScope preparedScope(&preparedInput);
  if (!reportSweepProgress(0u, plan.size(), progressCallback, userData)) {
    result.status = Status::Cancelled;
    result.error = "cancelled";
    return result;
  }

  std::string baseConfig{};
  if (!readBaseConfig(request.baseRun.configPath, baseConfig)) {
    result.status = Status::IoError;
    result.error = "failed to read sweep base config";
    return result;
  }

  for (const detail::BacktestSweepPoint& point : plan) {
    const std::filesystem::path configPath = result.resultPath / "point_configs" / ("point-" + std::to_string(point.pointId) + ".ini");
    if (!writePointConfig(baseConfig, request, point, configPath)) {
      result.status = Status::IoError;
      result.error = "failed to write sweep point config";
      return result;
    }
    BacktestRunRequest run = request.baseRun;
    run.configPath = configPath;
    run.outputPath.clear();
    run.runId = result.sweepId + "-point-" + std::to_string(point.pointId);
    run.requestId = run.runId;
    run.writeArtifacts = false;
    const BacktestRunResult runResult = runBacktest(run, nullptr, nullptr);
    detail::BacktestSweepPointResult pointResult{};
    pointResult.point = point;
    pointResult.run = runResult;
    points.push_back(std::move(pointResult));
    result.pointsEvaluated = points.size();
    if (!reportSweepProgress(result.pointsEvaluated, plan.size(), progressCallback, userData)) {
      result.status = Status::Cancelled;
      result.error = "cancelled";
      break;
    }
  }

  if (result.status == Status::Unknown) result.status = Status::Ok;
  result.zonesFound = 0u;
  const Status rowsStatus = detail::writeSweepRows(request, result, points);
  const Status curvesStatus = detail::writeSweepCurves(request, result, points);
  const Status manifestStatus = detail::writeSweepManifest(request, result, points);
  if (!isOk(rowsStatus) || !isOk(curvesStatus) || !isOk(manifestStatus)) {
    result.status = Status::IoError;
    result.error = "failed to write sweep artifacts";
  }
  return result;
}

}  // namespace hft_backtest
