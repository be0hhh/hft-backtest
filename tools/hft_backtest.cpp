#include "hft_backtest/backtest.hpp"
#include "hft_backtest/backtest_sweep.hpp"

#include <charconv>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

void printUsage() {
  std::cerr << "Usage: hft-backtest --session DIR --config INI [--leg-session DIR] [--strategy NAME] [--indicator-profile NAME] [--strategy-spread] [--no-rate-limits] [--two-phase-reprice] [--output DIR] [--run-id ID] [--sweep-id ID] [--sweep-budget N] [--sweep-seed N] [--sweep-param key:min:max:step[:grid|sweep]] [--latency-seed N] [--market-data-latency-us N] [--market-data-jitter-us N] [--market-order-latency-us N] [--market-order-jitter-us N] [--limit-order-latency-us N] [--limit-order-jitter-us N] [--cancel-order-latency-us N] [--cancel-order-jitter-us N] [--user-data-latency-us N] [--user-data-jitter-us N] [--order-latency-us N] [--cancel-latency-us N] [--maker-fill-min-cross-ticks N] [--initial-balance-usdt N] [--maker-fee-bps N] [--taker-fee-bps N] [--fee exchange:market:maker_bps:taker_bps] [--latency exchange:market:md_base:md_jitter:mkt_base:mkt_jitter:limit_base:limit_jitter[:cancel_base:cancel_jitter:user_base:user_jitter]]\n";
}

bool progress(const hft_backtest::BacktestProgress& value, void*) noexcept {
  std::cerr << value.stage << ": " << value.eventsDone << "/" << value.eventsTotal << " (" << value.percent << "%)\n";
  return true;
}

bool parseU64(const char* text, std::uint64_t& out) noexcept {
  if (!text) return false;
  const char* last = text + std::strlen(text);
  const auto parsed = std::from_chars(text, last, out);
  return parsed.ec == std::errc{} && parsed.ptr == last;
}

bool parseU32(const char* text, std::uint32_t& out) noexcept {
  std::uint64_t value = 0u;
  if (!parseU64(text, value)) return false;
  if (value > std::numeric_limits<std::uint32_t>::max()) return false;
  out = static_cast<std::uint32_t>(value);
  return true;
}

bool parseI64Text(const char* first, const char* last, std::int64_t& out) noexcept {
  if (!first || first == last) return false;
  const auto parsed = std::from_chars(first, last, out);
  return parsed.ec == std::errc{} && parsed.ptr == last;
}

bool parseDecimalE8(const char* text, std::int64_t& out) noexcept;

bool parseSweepParam(const char* text, hft_backtest::BacktestSweepParamRange& out) {
  if (!text || *text == '\0') return false;
  const char* keyEnd = std::strchr(text, ':');
  if (!keyEnd || keyEnd == text) return false;
  const char* minFirst = keyEnd + 1;
  const char* minEnd = std::strchr(minFirst, ':');
  if (!minEnd) return false;
  const char* maxFirst = minEnd + 1;
  const char* maxEnd = std::strchr(maxFirst, ':');
  if (!maxEnd) return false;
  const char* stepFirst = maxEnd + 1;
  const char* textEnd = text + std::strlen(text);
  const char* stepEnd = std::strchr(stepFirst, ':');
  const char* modeFirst = nullptr;
  if (stepEnd) modeFirst = stepEnd + 1;
  else stepEnd = textEnd;
  out.key.assign(text, static_cast<std::size_t>(keyEnd - text));
  if (!parseI64Text(minFirst, minEnd, out.minRaw) ||
      !parseI64Text(maxFirst, maxEnd, out.maxRaw) ||
      !parseI64Text(stepFirst, stepEnd, out.stepRaw)) return false;
  if (modeFirst) {
    const std::string mode(modeFirst, static_cast<std::size_t>(textEnd - modeFirst));
    if (mode == "grid" || mode == "sweep") out.mode = hft_backtest::BacktestSweepParamMode::Grid;
    else return false;
  }
  return true;
}

bool parseFeeSchedule(const char* text, hft_backtest::BacktestFeeSchedule& out) noexcept {
  if (!text || *text == '\0') return false;
  const char* exchangeEnd = std::strchr(text, ':');
  if (!exchangeEnd || exchangeEnd == text) return false;
  const char* marketFirst = exchangeEnd + 1;
  const char* marketEnd = std::strchr(marketFirst, ':');
  if (!marketEnd || marketEnd == marketFirst) return false;
  const char* makerFirst = marketEnd + 1;
  const char* makerEnd = std::strchr(makerFirst, ':');
  if (!makerEnd || makerEnd == makerFirst) return false;
  const char* takerFirst = makerEnd + 1;
  if (*takerFirst == '\0') return false;
  std::string maker(makerFirst, static_cast<std::size_t>(makerEnd - makerFirst));
  std::int64_t makerE8 = 0;
  std::int64_t takerE8 = 0;
  if (!parseDecimalE8(maker.c_str(), makerE8) || makerE8 < 0) return false;
  if (!parseDecimalE8(takerFirst, takerE8) || takerE8 < 0) return false;
  out.exchange.assign(text, static_cast<std::size_t>(exchangeEnd - text));
  out.market.assign(marketFirst, static_cast<std::size_t>(marketEnd - marketFirst));
  out.makerFeeBpsE8 = makerE8;
  out.takerFeeBpsE8 = takerE8;
  return true;
}

bool parseU64Text(const char* first, const char* last, std::uint64_t& out) noexcept {
  if (!first || first == last) return false;
  const auto parsed = std::from_chars(first, last, out);
  return parsed.ec == std::errc{} && parsed.ptr == last;
}

bool parseLatencySchedule(const char* text, hft_backtest::BacktestLatencySchedule& out) noexcept {
  if (!text || *text == '\0') return false;
  const char* parts[12]{};
  const char* ends[12]{};
  const char* cursor = text;
  const char* textEnd = text + std::strlen(text);
  std::uint32_t partCount = 0u;
  bool consumedAll = false;
  for (; partCount < 12u; ++partCount) {
    if (cursor >= textEnd) return false;
    parts[partCount] = cursor;
    const char* end = std::strchr(cursor, ':');
    if (!end) {
      ends[partCount] = textEnd;
      ++partCount;
      consumedAll = true;
      break;
    }
    if (end == cursor) return false;
    ends[partCount] = end;
    cursor = end + 1;
  }
  if (partCount != 8u && partCount != 12u) return false;
  if (!consumedAll) return false;
  std::uint64_t values[10]{};
  for (std::uint32_t i = 0u; i < partCount - 2u; ++i) {
    if (!parseU64Text(parts[i + 2u], ends[i + 2u], values[i])) return false;
  }
  out.exchange.assign(parts[0], static_cast<std::size_t>(ends[0] - parts[0]));
  out.market.assign(parts[1], static_cast<std::size_t>(ends[1] - parts[1]));
  out.marketData.baseUs = values[0];
  out.marketData.jitterUs = values[1];
  out.marketOrder.baseUs = values[2];
  out.marketOrder.jitterUs = values[3];
  out.limitOrder.baseUs = values[4];
  out.limitOrder.jitterUs = values[5];
  if (partCount == 12u) {
    out.cancelOrder.baseUs = values[6];
    out.cancelOrder.jitterUs = values[7];
    out.userData.baseUs = values[8];
    out.userData.jitterUs = values[9];
  }
  return true;
}

bool parseDecimalE8(const char* text, std::int64_t& out) noexcept {
  if (!text || *text == '\0') return false;
  bool negative = false;
  const char* p = text;
  if (*p == '-') {
    negative = true;
    ++p;
  }
  std::int64_t whole = 0;
  bool anyWhole = false;
  while (*p >= '0' && *p <= '9') {
    anyWhole = true;
    if (whole > (INT64_MAX / 10)) return false;
    whole = whole * 10 + static_cast<std::int64_t>(*p - '0');
    ++p;
  }
  std::int64_t frac = 0;
  std::int64_t scale = 10000000;
  if (*p == '.') {
    ++p;
    while (*p >= '0' && *p <= '9') {
      if (scale > 0) {
        frac += static_cast<std::int64_t>(*p - '0') * scale;
        scale /= 10;
      }
      ++p;
    }
  }
  if (!anyWhole || *p != '\0') return false;
  if (whole > (INT64_MAX - frac) / 100000000ll) return false;
  out = whole * 100000000ll + frac;
  if (negative) out = -out;
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  hft_backtest::BacktestRunRequest request{};
  std::vector<hft_backtest::BacktestSweepParamRange> sweepRanges{};
  std::uint64_t sweepBudget = 0u;
  std::uint64_t sweepSeed = 0u;
  std::string sweepId{};
  for (int i = 1; i < argc; ++i) {
    const char* arg = argv[i];
    auto needValue = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::cerr << "missing value for " << name << "\n";
        return nullptr;
      }
      return argv[++i];
    };
    if (std::strcmp(arg, "--session") == 0) {
      if (const char* value = needValue(arg)) request.sessionPath = value;
    } else if (std::strcmp(arg, "--leg-session") == 0) {
      if (const char* value = needValue(arg)) {
        hft_backtest::BacktestSessionRequest session{};
        session.path = value;
        request.sessions.push_back(session);
      }
    } else if (std::strcmp(arg, "--config") == 0) {
      if (const char* value = needValue(arg)) request.configPath = value;
    } else if (std::strcmp(arg, "--strategy") == 0) {
      if (const char* value = needValue(arg)) request.strategy = value;
    } else if (std::strcmp(arg, "--indicator-profile") == 0) {
      if (const char* value = needValue(arg)) request.indicatorProfile = value;
    } else if (std::strcmp(arg, "--strategy-spread") == 0) {
      request.captureStrategySpread = true;
    } else if (std::strcmp(arg, "--no-rate-limits") == 0) {
      request.rateLimitsEnabled = false;
    } else if (std::strcmp(arg, "--two-phase-reprice") == 0) {
      request.executionPipeline = hft_backtest::BacktestExecutionPipeline::TwoPhaseReprice;
    } else if (std::strcmp(arg, "--output") == 0) {
      if (const char* value = needValue(arg)) request.outputPath = value;
    } else if (std::strcmp(arg, "--run-id") == 0) {
      if (const char* value = needValue(arg)) request.runId = value;
    } else if (std::strcmp(arg, "--sweep-id") == 0) {
      if (const char* value = needValue(arg)) sweepId = value;
    } else if (std::strcmp(arg, "--sweep-budget") == 0) {
      if (const char* value = needValue(arg); value && !parseU64(value, sweepBudget)) return 2;
    } else if (std::strcmp(arg, "--sweep-seed") == 0) {
      if (const char* value = needValue(arg); value && !parseU64(value, sweepSeed)) return 2;
    } else if (std::strcmp(arg, "--sweep-param") == 0) {
      if (const char* value = needValue(arg)) {
        hft_backtest::BacktestSweepParamRange range{};
        if (!parseSweepParam(value, range)) return 2;
        sweepRanges.push_back(std::move(range));
      }
    } else if (std::strcmp(arg, "--latency-seed") == 0) {
      if (const char* value = needValue(arg); value && !parseU64(value, request.latencySeed)) return 2;
    } else if (std::strcmp(arg, "--market-data-latency-us") == 0) {
      if (const char* value = needValue(arg); value && !parseU64(value, request.marketDataLatency.baseUs)) return 2;
    } else if (std::strcmp(arg, "--market-data-jitter-us") == 0) {
      if (const char* value = needValue(arg); value && !parseU64(value, request.marketDataLatency.jitterUs)) return 2;
    } else if (std::strcmp(arg, "--market-order-latency-us") == 0) {
      if (const char* value = needValue(arg); value && !parseU64(value, request.marketOrderLatency.baseUs)) return 2;
    } else if (std::strcmp(arg, "--market-order-jitter-us") == 0) {
      if (const char* value = needValue(arg); value && !parseU64(value, request.marketOrderLatency.jitterUs)) return 2;
    } else if (std::strcmp(arg, "--limit-order-latency-us") == 0) {
      if (const char* value = needValue(arg); value && !parseU64(value, request.limitOrderLatency.baseUs)) return 2;
    } else if (std::strcmp(arg, "--limit-order-jitter-us") == 0) {
      if (const char* value = needValue(arg); value && !parseU64(value, request.limitOrderLatency.jitterUs)) return 2;
    } else if (std::strcmp(arg, "--cancel-order-latency-us") == 0) {
      if (const char* value = needValue(arg); value && !parseU64(value, request.cancelOrderLatency.baseUs)) return 2;
      request.cancelLatencyUs = request.cancelOrderLatency.baseUs;
    } else if (std::strcmp(arg, "--cancel-order-jitter-us") == 0) {
      if (const char* value = needValue(arg); value && !parseU64(value, request.cancelOrderLatency.jitterUs)) return 2;
    } else if (std::strcmp(arg, "--user-data-latency-us") == 0) {
      if (const char* value = needValue(arg); value && !parseU64(value, request.userDataLatency.baseUs)) return 2;
    } else if (std::strcmp(arg, "--user-data-jitter-us") == 0) {
      if (const char* value = needValue(arg); value && !parseU64(value, request.userDataLatency.jitterUs)) return 2;
    } else if (std::strcmp(arg, "--order-latency-us") == 0) {
      if (const char* value = needValue(arg); value && !parseU64(value, request.orderLatencyUs)) return 2;
      request.marketOrderLatency.baseUs = request.orderLatencyUs;
      request.limitOrderLatency.baseUs = request.orderLatencyUs;
    } else if (std::strcmp(arg, "--cancel-latency-us") == 0) {
      if (const char* value = needValue(arg); value && !parseU64(value, request.cancelLatencyUs)) return 2;
      request.cancelOrderLatency.baseUs = request.cancelLatencyUs;
    } else if (std::strcmp(arg, "--maker-fill-min-cross-ticks") == 0) {
      if (const char* value = needValue(arg); value && !parseU32(value, request.makerFillMinCrossTicks)) return 2;
    } else if (std::strcmp(arg, "--initial-balance-usdt") == 0) {
      if (const char* value = needValue(arg); value && (!parseDecimalE8(value, request.initialBalanceE8) || request.initialBalanceE8 < 0)) return 2;
    } else if (std::strcmp(arg, "--maker-fee-bps") == 0) {
      if (const char* value = needValue(arg); value && (!parseDecimalE8(value, request.makerFeeBpsE8) || request.makerFeeBpsE8 < 0)) return 2;
      request.hasMakerFeeBps = true;
    } else if (std::strcmp(arg, "--taker-fee-bps") == 0) {
      if (const char* value = needValue(arg); value && (!parseDecimalE8(value, request.takerFeeBpsE8) || request.takerFeeBpsE8 < 0)) return 2;
      request.hasTakerFeeBps = true;
    } else if (std::strcmp(arg, "--fee") == 0) {
      if (const char* value = needValue(arg)) {
        hft_backtest::BacktestFeeSchedule schedule{};
        if (!parseFeeSchedule(value, schedule)) return 2;
        request.feeSchedules.push_back(std::move(schedule));
      }
    } else if (std::strcmp(arg, "--latency") == 0) {
      if (const char* value = needValue(arg)) {
        hft_backtest::BacktestLatencySchedule schedule{};
        if (!parseLatencySchedule(value, schedule)) return 2;
        request.latencySchedules.push_back(std::move(schedule));
      }
    } else if (std::strcmp(arg, "-h") == 0 || std::strcmp(arg, "--help") == 0) {
      printUsage();
      return 0;
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      printUsage();
      return 2;
    }
  }
  if ((request.sessionPath.empty() && request.sessions.empty()) || request.configPath.empty()) {
    printUsage();
    return 2;
  }
  if (!sweepRanges.empty() || sweepBudget != 0u) {
    hft_backtest::BacktestSweepRequest sweep{};
    sweep.baseRun = request;
    sweep.outputPath = request.outputPath;
    sweep.sweepId = sweepId;
    sweep.runBudget = sweepBudget;
    sweep.searchSeed = sweepSeed;
    sweep.ranges = std::move(sweepRanges);
    const auto result = hft_backtest::runBacktestSweep(sweep, progress, nullptr);
    if (!hft_backtest::isOk(result.status)) {
      std::cerr << "sweep failed: " << hft_backtest::statusToString(result.status) << ": " << result.error << "\n";
      return 1;
    }
    std::cout << result.resultPath.generic_string() << "\n";
    return 0;
  }
  const auto result = hft_backtest::runBacktest(request, progress, nullptr);
  for (const std::string& warning : result.warnings) {
    std::cerr << "backtest warning: " << warning << "\n";
  }
  if (!hft_backtest::isOk(result.status)) {
    std::cerr << "backtest failed: " << hft_backtest::statusToString(result.status) << ": " << result.error << "\n";
    return 1;
  }
  std::cout << result.resultPath.generic_string() << "\n";
  return 0;
}
