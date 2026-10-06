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

bool writeFills(const std::filesystem::path& path, const EngineState& engine) noexcept {
  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out.is_open()) return false;
  for (const JsonFill& row : engine.fills) {
    out << '[' << row.orderId
        << ',' << row.entryTsNs
        << ',' << row.exitTsNs
        << ',' << static_cast<unsigned>(row.side)
        << ',' << row.priceE8
        << ',' << row.qtyE8
        << ',' << row.realizedPnlE8
        << ',' << (row.reduceOnly ? 1 : 0)
        << ',' << row.legIndex
        << ',' << static_cast<unsigned>(row.fillReason)
        << ',' << static_cast<unsigned>(row.liquidity)
        << ',' << row.orderQtyE8
        << ',' << row.cumulativeFilledQtyE8
        << ',' << row.remainingQtyE8
        << ',' << row.avgPriceE8
        << ',' << row.bookLevelQtyE8
        << ',' << row.bookVisibleExecutableQtyE8
        << ',' << row.bookConsumedPctE8
        << ',' << row.queueAheadBeforeE8
        << ',' << row.queueAheadAfterE8
        << ',' << row.chunkIndex
        << ',' << row.chunkCount
        << ',' << row.executionQtyE8
        << ',' << row.executionAvgPriceE8
        << ',' << row.referencePriceE8
        << ',' << row.slippageE8
        << ',' << row.slippageBpsE8
        << ',' << row.executionBookConsumedPctE8
        << ',' << row.contextIndex
        << "]\n";
  }
  return out.good();
}

void writeEquityRow(std::ostream& out, const JsonEquityPoint& row) {
  out << '[' << row.tsNs
      << ',' << row.grossRealizedPnlE8
      << ',' << row.realizedPnlE8
      << ',' << row.unrealizedPnlE8
      << ',' << row.grossTotalPnlE8
      << ',' << row.netTotalPnlE8
      << ',' << row.totalPnlE8
      << ',' << row.feesPaidE8
      << ',' << row.walletBalanceE8
      << ',' << row.positionQtyE8
      << ',' << row.fillCount
      << "]\n";
}

bool writeEquity(const std::filesystem::path& path, const EngineState& engine) noexcept {
  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out.is_open()) return false;
  for (const JsonEquityPoint& row : engine.equityPoints) writeEquityRow(out, row);
  return out.good();
}

JsonEquityPoint syntheticLegEquityPoint(const BacktestLegSummary& leg) noexcept {
  JsonEquityPoint out{};
  out.grossRealizedPnlE8 = leg.grossRealizedPnlE8;
  out.realizedPnlE8 = leg.netRealizedPnlE8;
  out.unrealizedPnlE8 = leg.unrealizedPnlE8;
  out.grossTotalPnlE8 = trading_core::saturatingAddI64(
      leg.grossRealizedPnlE8, leg.unrealizedPnlE8);
  out.netTotalPnlE8 = trading_core::saturatingAddI64(
      leg.netRealizedPnlE8, leg.unrealizedPnlE8);
  out.totalPnlE8 = leg.totalPnlE8;
  out.feesPaidE8 = leg.feesPaidE8;
  out.walletBalanceE8 = leg.walletBalanceE8;
  out.positionQtyE8 = leg.openPositionQtyE8;
  out.fillCount = leg.fills;
  return out;
}

bool writeSyntheticLegEquity(const std::filesystem::path& path, const BacktestLegSummary& leg) noexcept {
  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out.is_open()) return false;
  writeEquityRow(out, syntheticLegEquityPoint(leg));
  return out.good();
}

bool writeRiskSnapshots(const std::filesystem::path& path, const EngineState& engine) noexcept {
  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out.is_open()) return false;
  for (const JsonRiskSnapshot& row : engine.riskSnapshots) {
    out << '[' << row.tsNs
        << ',' << row.grossRealizedPnlE8
        << ',' << row.realizedPnlE8
        << ',' << row.unrealizedPnlE8
        << ',' << row.grossTotalPnlE8
        << ',' << row.netTotalPnlE8
        << ',' << row.feesPaidE8
        << ',' << row.walletBalanceE8
        << ',' << row.positionQtyE8
        << ',' << row.entryPriceE8
        << ',' << row.bidPriceE8
        << ',' << row.askPriceE8
        << ',' << row.fillCount
        << ',' << static_cast<unsigned>(row.reason)
        << "]\n";
  }
  return out.good();
}

bool writeStrategyIndicator(const std::filesystem::path& path, const EngineState& engine) noexcept {
  if (engine.strategyIndicator.empty()) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return true;
  }
  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out.is_open()) return false;
  const std::size_t featureCount = strategyIndicatorFeatureCount(engine.strategyIndicatorProfile);
  for (const JsonStrategyIndicatorPoint& row : engine.strategyIndicator) {
    out << '[' << row.tsNs
        << ',' << row.valueRaw
        << ',' << row.auxRaw
        << ',' << static_cast<unsigned>(row.eventCode)
        << ',' << static_cast<unsigned>(row.reasonRaw)
        << ',' << static_cast<unsigned>(row.decisionKind)
        << ',' << static_cast<unsigned>(row.sideRaw);
    for (std::size_t i = 0u; i < featureCount && i < row.featureRaw.size(); ++i) {
      out << ',' << row.featureRaw[i];
    }
    out << "]\n";
  }
  return out.good();
}

bool writeStrategyRange(const std::filesystem::path& path, const EngineState& engine) noexcept {
  if (engine.strategyRange.empty()) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return true;
  }
  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out.is_open()) return false;
  for (const JsonStrategyRangePoint& row : engine.strategyRange) {
    out << '[' << row.tsNs
        << ',' << row.lowRaw
        << ',' << row.midRaw
        << ',' << row.highRaw
        << "]\n";
  }
  return out.good();
}

bool writeStrategySpread(const std::filesystem::path& path, const EngineState& engine) noexcept {
  if (engine.strategySpread.empty()) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return true;
  }
  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out.is_open()) return false;
  for (const JsonStrategySpreadPoint& row : engine.strategySpread) {
    out << '[' << row.tsNs
        << ',' << row.spreadBpsE8
        << ',' << row.emaBpsE8
        << ',' << row.deviationBpsE8
        << ',' << row.costBandBpsE8
        << ',' << row.edgeAfterCostBpsE8
        << ',' << static_cast<unsigned>(row.direction)
        << ',' << static_cast<unsigned>(row.decisionKind)
        << ',' << static_cast<unsigned>(row.reasonRaw)
        << "]\n";
  }
  return out.good();
}

bool writeExecutionQuality(const std::filesystem::path& path, const EngineState& engine) noexcept {
  if (engine.executionQuality.empty()) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return true;
  }
  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out.is_open()) return false;
  for (const JsonExecutionQualityAction& row : engine.executionQuality) {
    std::uint8_t status = row.statusRaw;
    if (row.anchorRaw != static_cast<std::uint8_t>(trading_core::StrategyExecutionAnchor::MakerQuote) &&
        (status == static_cast<std::uint8_t>(trading_core::StrategyExecutionStatus::Proposed) ||
        status == static_cast<std::uint8_t>(trading_core::StrategyExecutionStatus::Accepted))) {
      const bool anyFill = row.legs[0].filledQtyE8 > 0 || row.legs[1].filledQtyE8 > 0;
      status = static_cast<std::uint8_t>(anyFill
          ? trading_core::StrategyExecutionStatus::Partial
          : trading_core::StrategyExecutionStatus::Uncertain);
    }
    out << '[' << row.actionId
        << ',' << row.positionCycleId
        << ',' << row.transactionId
        << ',' << row.pairKey
        << ',' << row.decisionTsNs
        << ',' << row.completedTsNs
        << ',' << static_cast<unsigned>(row.phaseRaw)
        << ',' << static_cast<unsigned>(row.anchorRaw)
        << ',' << static_cast<unsigned>(status)
        << ',' << static_cast<unsigned>(row.directionRaw)
        << ',' << static_cast<unsigned>(row.bandSideRaw)
        << ',' << row.decisionEdgeAfterCostBpsE8
        << ',' << row.fillEdgeAfterCostBpsE8
        << ',' << row.edgeLossBpsE8
        << ',' << row.requiredEdgeBpsE8
        << ',' << row.fairBpsE8
        << ',' << row.costBandBpsE8
        << ',' << row.quoteDecisionEdgeAfterCostBpsE8
        << ',' << row.decisionToFirstFillNs
        << ',' << row.decisionToLastFillNs
        << ',' << row.fillLegSkewNs
        << ',' << row.thresholdDeadlineNs
        << ',' << row.zeroEdgeDeadlineNs
        << ',' << row.pairIndex
        << ',' << row.pairSlot;
    for (const JsonExecutionQualityLeg& leg : row.legs) {
      out << ',' << leg.legIndex
          << ',' << leg.clientIntentId
          << ',' << static_cast<unsigned>(leg.sideRaw)
          << ',' << leg.expectedQtyE8
          << ',' << leg.filledQtyE8
          << ',' << leg.decisionPriceE8
          << ',' << leg.fillVwapE8
          << ',' << leg.firstFillTsNs
          << ',' << leg.lastFillTsNs;
    }
    out << "]\n";
  }
  return out.good();
}

bool writeRateLimitUsage(const std::filesystem::path& path, const EngineState& engine) noexcept {
  if (engine.rateLimitUsage.empty()) {
    std::error_code ec;
    std::filesystem::remove(path, ec);
    return true;
  }
  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out.is_open()) return false;
  for (const JsonRateLimitUsagePoint& row : engine.rateLimitUsage) {
    out << '[' << row.tsNs
        << ',' << row.legIndex
        << ',' << row.minRemainingPctE4
        << ',' << row.remaining
        << ',' << row.limit
        << ',' << static_cast<unsigned>(row.bucketKind)
        << ',' << row.intervalNs
        << ',' << (row.exhausted ? 1 : 0)
        << "]\n";
  }
  return out.good();
}

bool writeRunConfig(const std::filesystem::path& resultDir, const std::filesystem::path& configPath) noexcept {
  if (configPath.empty()) return true;
  const std::filesystem::path target = resultDir / "config.ini";
  std::error_code ec;
  if (std::filesystem::exists(target, ec) && std::filesystem::equivalent(configPath, target, ec)) return true;
  ec.clear();
  std::filesystem::copy_file(configPath, target, std::filesystem::copy_options::overwrite_existing, ec);
  return !ec;
}

bool writeLegEquityStreams(const std::filesystem::path& resultDir,
                           const BacktestRunResult& result,
                           const std::vector<EngineState>* legEngines) noexcept {
  const std::size_t count = legEngines != nullptr ? legEngines->size() : result.legs.size();
  for (std::size_t i = 0u; i < count; ++i) {
    const std::filesystem::path dir = resultDir / "legs" / std::to_string(i);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) return false;
    if (legEngines != nullptr) {
      if (!writeEquity(dir / "equity.jsonl", (*legEngines)[i])) return false;
    } else if (!writeSyntheticLegEquity(dir / "equity.jsonl", result.legs[i])) {
      return false;
    }
  }
  return true;
}

}  // namespace hft_backtest::detail::result_writer

