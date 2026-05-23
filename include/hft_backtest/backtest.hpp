#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include "hft_backtest/api.hpp"
#include "hft_backtest/status.hpp"

namespace hft_backtest {

struct BacktestRunRequest {
  std::filesystem::path sessionPath{};
  std::filesystem::path configPath{};
  std::filesystem::path outputPath{};
  std::string requestId{};
  std::string runId{};
  std::string strategy{};
  std::uint64_t orderLatencyUs{1000};
  std::uint64_t amendLatencyUs{1000};
  std::uint64_t cancelLatencyUs{1000};
};

struct BacktestProgress {
  std::uint64_t eventsDone{0};
  std::uint64_t eventsTotal{0};
  std::uint32_t percent{0};
  std::string stage{};
};

struct BacktestRunResult {
  Status status{Status::Unknown};
  std::string requestId{};
  std::string runId{};
  std::string strategy{};
  std::filesystem::path sessionPath{};
  std::filesystem::path configPath{};
  std::filesystem::path resultPath{};
  std::string error{};
  std::uint64_t orderLatencyUs{1000};
  std::uint64_t amendLatencyUs{1000};
  std::uint64_t cancelLatencyUs{1000};
  std::uint64_t trades{0};
  std::uint64_t bookTickers{0};
  std::uint64_t depths{0};
  std::uint64_t orders{0};
  std::uint64_t fills{0};
  std::uint64_t events{0};
  std::int64_t firstTsNs{0};
  std::int64_t lastTsNs{0};
  std::int64_t realizedPnlE8{0};
  std::int64_t unrealizedPnlE8{0};
  std::int64_t totalPnlE8{0};
};

using BacktestProgressCallback = bool (*)(const BacktestProgress& progress, void* userData) noexcept;

HFT_BACKTEST_API BacktestRunResult runBacktest(const BacktestRunRequest& request,
                                               BacktestProgressCallback progressCallback = nullptr,
                                               void* userData = nullptr) noexcept;

}  // namespace hft_backtest
