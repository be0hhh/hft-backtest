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
  std::int64_t initialBalanceE8{0};
  std::int64_t makerFeeBpsE8{0};
  std::int64_t takerFeeBpsE8{0};
};

struct BacktestProgress {
  std::uint64_t eventsDone{0};
  std::uint64_t eventsTotal{0};
  std::uint32_t percent{0};
  std::string stage{};
};

struct BacktestInputCoverage {
  std::uint64_t strategyCycles{0};
  std::uint64_t cyclesWithTrade{0};
  std::uint64_t cyclesWithBookTicker{0};
  std::uint64_t cyclesWithDepth{0};
  std::uint64_t cyclesWithOrderBook{0};
  std::uint64_t seedTradeRows{0};
  std::uint64_t seedM1Candles{0};
  std::uint64_t seedM15Candles{0};
  std::uint64_t seedD1Candles{0};
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
  std::uint64_t userEvents{0};
  std::uint64_t timerWakeEvents{0};
  std::uint64_t reduceOnlyOrders{0};
  std::uint64_t reduceOnlyFills{0};
  std::int64_t firstTsNs{0};
  std::int64_t lastTsNs{0};
  std::int64_t realizedPnlE8{0};
  std::int64_t unrealizedPnlE8{0};
  std::int64_t totalPnlE8{0};
  std::int64_t initialBalanceE8{0};
  std::int64_t walletBalanceE8{0};
  std::int64_t grossRealizedPnlE8{0};
  std::int64_t feesPaidE8{0};
  std::int64_t netRealizedPnlE8{0};
  std::int64_t openPositionQtyE8{0};
  BacktestInputCoverage inputCoverage{};
  bool strategyClosed{false};
};

using BacktestProgressCallback = bool (*)(const BacktestProgress& progress, void* userData) noexcept;

HFT_BACKTEST_API BacktestRunResult runBacktest(const BacktestRunRequest& request,
                                               BacktestProgressCallback progressCallback = nullptr,
                                               void* userData = nullptr) noexcept;

}  // namespace hft_backtest
