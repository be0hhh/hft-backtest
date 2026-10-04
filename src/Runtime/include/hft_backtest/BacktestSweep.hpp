#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "hft_backtest/Api.hpp"
#include "hft_backtest/Backtest.hpp"
#include "hft_backtest/Status.hpp"

namespace hft_backtest {

enum class BacktestSweepParamMode : std::uint8_t {
  Grid = 0,
};

struct BacktestSweepParamRange {
  std::string key{};
  std::int64_t minRaw{0};
  std::int64_t maxRaw{0};
  std::int64_t stepRaw{1};
  BacktestSweepParamMode mode{BacktestSweepParamMode::Grid};
};

struct BacktestSweepRequest {
  BacktestRunRequest baseRun{};
  std::filesystem::path outputPath{};
  std::string sweepId{};
  std::uint64_t runBudget{0};
  std::uint64_t searchSeed{0};
  std::vector<BacktestSweepParamRange> ranges{};
};

struct BacktestSweepResult {
  Status status{Status::Unknown};
  std::string sweepId{};
  std::filesystem::path resultPath{};
  std::filesystem::path rowsPath{};
  std::filesystem::path curvesPath{};
  std::filesystem::path manifestPath{};
  std::uint64_t pointsEvaluated{0};
  std::uint64_t zonesFound{0};
  std::uint32_t workerCount{1u};
  std::string error{};
};

HFT_BACKTEST_API BacktestSweepResult runBacktestSweep(const BacktestSweepRequest& request,
                                                      BacktestProgressCallback progressCallback = nullptr,
                                                      void* userData = nullptr) noexcept;

}  // namespace hft_backtest
