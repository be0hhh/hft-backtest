#include "hft_backtest/backtest_sweep.hpp"

#include "engine/backtest_prepared.hpp"
#include "sweep/backtest_sweep_internal.hpp"

#include <chrono>
#include <atomic>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

namespace hft_backtest {
namespace {

std::uint64_t elapsedNs(std::chrono::steady_clock::time_point start) noexcept {
  const auto value = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - start).count();
  return value > 0 ? static_cast<std::uint64_t>(value) : 0u;
}

struct SweepCancelContext {
  std::atomic_bool* cancelled{nullptr};
};

bool sweepRunCancelProgress(const BacktestProgress&, void* raw) noexcept {
  const auto* context = static_cast<const SweepCancelContext*>(raw);
  return context == nullptr || context->cancelled == nullptr || !context->cancelled->load(std::memory_order_relaxed);
}

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
    if (!request.ranges[i].key.empty() && request.ranges[i].key.front() == '@') continue;
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
  const auto totalStart = std::chrono::steady_clock::now();
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
  const auto prepareStart = std::chrono::steady_clock::now();
  detail::PreparedBacktestInput preparedInput{};
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

  std::vector<std::filesystem::path> pointConfigPaths(plan.size());
  for (std::size_t index = 0u; index < plan.size(); ++index) {
    const detail::BacktestSweepPoint& point = plan[index];
    const std::filesystem::path configPath = result.resultPath / "point_configs" / ("point-" + std::to_string(point.pointId) + ".ini");
    if (!writePointConfig(baseConfig, request, point, configPath)) {
      result.status = Status::IoError;
      result.error = "failed to write sweep point config";
      return result;
    }
    pointConfigPaths[index] = configPath;
  }
  result.prepareNs = elapsedNs(prepareStart);

  std::vector<detail::BacktestSweepPointResult> pointResults(plan.size());
  std::vector<std::uint8_t> completed(plan.size(), 0u);
  std::atomic_bool cancelled{false};
  std::atomic<std::size_t> nextIndex{1u};
  std::atomic<std::uint64_t> completedCount{0u};
  std::atomic<std::uint64_t> pointCpuSumNs{0u};
  std::mutex progressMutex{};
  SweepCancelContext cancelContext{&cancelled};

  auto executePoint = [&](std::size_t index) {
    const detail::BacktestSweepPoint& point = plan[index];
    BacktestRunRequest run = request.baseRun;
    run.configPath = pointConfigPaths[index];
    run.outputPath.clear();
    run.runId = result.sweepId + "-point-" + std::to_string(point.pointId);
    run.requestId = run.runId;
    run.writeArtifacts = false;
    for (std::size_t rangeIndex = 0u; rangeIndex < request.ranges.size(); ++rangeIndex) {
      const std::string& key = request.ranges[rangeIndex].key;
      const std::uint64_t value = point.values[rangeIndex] > 0
          ? static_cast<std::uint64_t>(point.values[rangeIndex])
          : 0u;
      const auto applyLatency = [&](BacktestLatencyProfile& distribution) noexcept {
        distribution.baseUs = value;
      };
      if (key == "@market_order_latency_us") {
        applyLatency(run.marketOrderLatency);
        run.orderLatencyUs = value;
        for (BacktestLatencySchedule& schedule : run.latencySchedules) applyLatency(schedule.marketOrder);
      } else if (key == "@limit_order_latency_us") {
        applyLatency(run.limitOrderLatency);
        for (BacktestLatencySchedule& schedule : run.latencySchedules) applyLatency(schedule.limitOrder);
      } else if (key == "@user_data_latency_us") {
        applyLatency(run.userDataLatency);
        for (BacktestLatencySchedule& schedule : run.latencySchedules) applyLatency(schedule.userData);
      }
    }
    detail::PreparedBacktestScope preparedScope(&preparedInput);
    BacktestRunResult runResult = runBacktest(run, sweepRunCancelProgress, &cancelContext);
    pointCpuSumNs.fetch_add(runResult.performance.totalNs, std::memory_order_relaxed);
    pointResults[index].point = point;
    pointResults[index].run = std::move(runResult);
    completed[index] = 1u;
    const std::uint64_t done = completedCount.fetch_add(1u, std::memory_order_relaxed) + 1u;
    std::lock_guard lock{progressMutex};
    if (!reportSweepProgress(done, plan.size(), progressCallback, userData)) {
      cancelled.store(true, std::memory_order_relaxed);
    }
  };

  const auto pointsStart = std::chrono::steady_clock::now();
  executePoint(0u);
  result.prepareNs += pointResults[0].run.performance.loadNs + pointResults[0].run.performance.timelineNs;

  const unsigned hardware = std::thread::hardware_concurrency();
  const std::size_t cpuWorkers = std::max<std::size_t>(1u, hardware == 0u ? 1u : hardware / 2u);
  result.workerCount = static_cast<std::uint32_t>(
      std::min<std::size_t>(plan.size(), std::min<std::size_t>(4u, cpuWorkers)));
  std::vector<std::thread> workers{};
  if (!cancelled.load(std::memory_order_relaxed) && plan.size() > 1u) {
    workers.reserve(result.workerCount);
    for (std::uint32_t worker = 0u; worker < result.workerCount; ++worker) {
      workers.emplace_back([&] {
        while (!cancelled.load(std::memory_order_relaxed)) {
          const std::size_t index = nextIndex.fetch_add(1u, std::memory_order_relaxed);
          if (index >= plan.size()) break;
          executePoint(index);
        }
      });
    }
  }
  for (std::thread& worker : workers) worker.join();
  result.pointsWallNs = elapsedNs(pointsStart);
  result.pointsCpuSumNs = pointCpuSumNs.load(std::memory_order_relaxed);

  std::vector<detail::BacktestSweepPointResult> points{};
  points.reserve(static_cast<std::size_t>(completedCount.load(std::memory_order_relaxed)));
  for (std::size_t index = 0u; index < pointResults.size(); ++index) {
    if (completed[index] != 0u) points.push_back(std::move(pointResults[index]));
  }
  result.pointsEvaluated = points.size();
  if (cancelled.load(std::memory_order_relaxed)) {
    result.status = Status::Cancelled;
    result.error = "cancelled";
  }

  if (result.status == Status::Unknown) result.status = Status::Ok;
  result.zonesFound = 0u;
  const auto artifactStart = std::chrono::steady_clock::now();
  const Status rowsStatus = detail::writeSweepRows(request, result, points);
  const Status curvesStatus = detail::writeSweepCurves(request, result, points);
  result.artifactWriteNs = elapsedNs(artifactStart);
  result.totalNs = elapsedNs(totalStart);
  const Status manifestStatus = detail::writeSweepManifest(request, result, points);
  if (!isOk(rowsStatus) || !isOk(curvesStatus) || !isOk(manifestStatus)) {
    result.status = Status::IoError;
    result.error = "failed to write sweep artifacts";
  }
  return result;
}

}  // namespace hft_backtest
