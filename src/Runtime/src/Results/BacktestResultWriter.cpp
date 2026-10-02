#include "trading_core/Algorithm/Math/Integer/Saturating.hpp"
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

}  // namespace hft_backtest::detail::result_writer

namespace hft_backtest::detail {
using result_writer::OrderLifetimeRow;
using result_writer::materializeOrderLifetimes;
using result_writer::writeEquity;
using result_writer::writeExecutionQuality;
using result_writer::writeFills;
using result_writer::writeLegEquityStreams;
using result_writer::writeManifest;
using result_writer::writeOrderLifetimes;
using result_writer::writeRateLimitUsage;
using result_writer::writeRiskSnapshots;
using result_writer::writeRunConfig;
using result_writer::writeStrategyIndicator;
using result_writer::writeStrategyRange;
using result_writer::writeStrategySpread;

Status writeResult(BacktestRunResult& result,
                   const EngineState& engine,
                   const std::vector<EngineState>* legEngines) noexcept {
  const auto writeStart = std::chrono::steady_clock::now();
  std::error_code ec;
  std::filesystem::create_directories(result.resultPath, ec);
  if (ec) return Status::IoError;
  const std::vector<OrderLifetimeRow> orderLifetimes = materializeOrderLifetimes(engine);
  if (!writeRunConfig(result.resultPath, result.configPath)) return Status::IoError;
  if (!writeOrderLifetimes(result.resultPath / "order_lifetimes.jsonl", orderLifetimes)) return Status::IoError;
  std::filesystem::remove(result.resultPath / "orders.jsonl", ec);
  if (ec) return Status::IoError;
  if (!writeFills(result.resultPath / "fills.jsonl", engine)) return Status::IoError;
  if (!writeEquity(result.resultPath / "equity.jsonl", engine)) return Status::IoError;
  if (!writeLegEquityStreams(result.resultPath, result, legEngines)) return Status::IoError;
  if (!writeRiskSnapshots(result.resultPath / "risk_snapshots.jsonl", engine)) return Status::IoError;
  std::filesystem::remove(result.resultPath / "probe_metrics.jsonl", ec);
  if (ec) return Status::IoError;
  if (!writeStrategyIndicator(result.resultPath / "strategy_indicator.jsonl", engine)) return Status::IoError;
  if (!writeStrategyRange(result.resultPath / "strategy_range.jsonl", engine)) return Status::IoError;
  if (!writeStrategySpread(result.resultPath / "strategy_spread.jsonl", engine)) return Status::IoError;
  if (!writeExecutionQuality(result.resultPath / "execution_quality.jsonl", engine)) return Status::IoError;
  if (!writeRateLimitUsage(result.resultPath / "rate_limit_usage.jsonl", engine)) return Status::IoError;
  const auto writeElapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now() - writeStart).count();
  if (writeElapsed > 0) {
    result.performance.artifactWriteNs += static_cast<std::uint64_t>(writeElapsed);
    result.performance.totalNs += static_cast<std::uint64_t>(writeElapsed);
  }
  if (!writeManifest(result.resultPath / "manifest.json", result, engine, legEngines, orderLifetimes.size())) return Status::IoError;
  return Status::Ok;
}

}  // namespace hft_backtest::detail
