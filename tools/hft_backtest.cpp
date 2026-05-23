#include "hft_backtest/backtest.hpp"

#include <charconv>
#include <cstring>
#include <cstdint>
#include <iostream>
#include <string>
#include <system_error>

namespace {

void printUsage() {
  std::cerr << "Usage: hft-backtest --session DIR --config INI [--strategy NAME] [--output JSON] [--run-id ID] [--order-latency-us N] [--amend-latency-us N] [--cancel-latency-us N]\n";
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

}  // namespace

int main(int argc, char** argv) {
  hft_backtest::BacktestRunRequest request{};
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
    } else if (std::strcmp(arg, "--config") == 0) {
      if (const char* value = needValue(arg)) request.configPath = value;
    } else if (std::strcmp(arg, "--strategy") == 0) {
      if (const char* value = needValue(arg)) request.strategy = value;
    } else if (std::strcmp(arg, "--output") == 0) {
      if (const char* value = needValue(arg)) request.outputPath = value;
    } else if (std::strcmp(arg, "--run-id") == 0) {
      if (const char* value = needValue(arg)) request.runId = value;
    } else if (std::strcmp(arg, "--order-latency-us") == 0) {
      if (const char* value = needValue(arg); value && !parseU64(value, request.orderLatencyUs)) return 2;
    } else if (std::strcmp(arg, "--amend-latency-us") == 0) {
      if (const char* value = needValue(arg); value && !parseU64(value, request.amendLatencyUs)) return 2;
    } else if (std::strcmp(arg, "--cancel-latency-us") == 0) {
      if (const char* value = needValue(arg); value && !parseU64(value, request.cancelLatencyUs)) return 2;
    } else if (std::strcmp(arg, "-h") == 0 || std::strcmp(arg, "--help") == 0) {
      printUsage();
      return 0;
    } else {
      std::cerr << "unknown argument: " << arg << "\n";
      printUsage();
      return 2;
    }
  }
  if (request.sessionPath.empty() || request.configPath.empty()) {
    printUsage();
    return 2;
  }
  const auto result = hft_backtest::runBacktest(request, progress, nullptr);
  if (!hft_backtest::isOk(result.status)) {
    std::cerr << "backtest failed: " << hft_backtest::statusToString(result.status) << ": " << result.error << "\n";
    return 1;
  }
  std::cout << result.resultPath.generic_string() << "\n";
  return 0;
}
