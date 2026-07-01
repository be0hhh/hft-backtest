#include "accounting/backtest_result_aggregation.hpp"
#include "config/backtest_run_config.hpp"
#include "engine/backtest_engine.hpp"

#include "hft_trader/core/rate_limit/RateLimit.hpp"

#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <string_view>

namespace {

using namespace hft_backtest;
using namespace hft_backtest::detail;

bool expect(bool value, std::string_view message) {
  if (value) return true;
  std::cerr << "FAIL: " << message << "\n";
  return false;
}

const hft_trader::core::RateLimitBucketConfig* findRateLimitBucket(
    const BacktestRateLimitSchedule& schedule,
    hft_trader::core::RateLimitBucketKind kind,
    std::uint64_t intervalNs) noexcept {
  for (const hft_trader::core::RateLimitBucketConfig& bucket : schedule.buckets) {
    if (bucket.enabled && bucket.kind == kind && bucket.intervalNs == intervalNs) return &bucket;
  }
  return nullptr;
}

bool testBacktestRateLimitScheduleOverlaysMatchingPreset() {
  BacktestRunRequest request{};
  BacktestRateLimitSchedule overrideSchedule{};
  overrideSchedule.exchange = "binance";
  overrideSchedule.market = "futures";
  hft_trader::core::RateLimitBucketConfig bucket{};
  bucket.enabled = true;
  bucket.kind = hft_trader::core::RateLimitBucketKind::Orders;
  bucket.intervalNs = 10000000000ull;
  bucket.limit = 7;
  overrideSchedule.buckets.push_back(bucket);
  request.rateLimitSchedules.push_back(overrideSchedule);

  const BacktestRateLimitSchedule schedule = resolveRateLimitSchedule(request, "binance", "futures");
  const hft_trader::core::RateLimitBucketConfig* tenSecond = findRateLimitBucket(
      schedule, hft_trader::core::RateLimitBucketKind::Orders, 10000000000ull);
  const hft_trader::core::RateLimitBucketConfig* minute = findRateLimitBucket(
      schedule, hft_trader::core::RateLimitBucketKind::Orders, 60000000000ull);
  return expect(tenSecond && tenSecond->limit == 7,
                "explicit backtest rate-limit bucket overrides matching preset window") &&
         expect(minute && minute->limit == 1200,
                "explicit backtest rate-limit schedule preserves untouched preset windows");
}

bool testStrictRateLimitRejectFailsRunResult() {
  EngineState engine{};
  engine.rateLimitRejectedOrders = 1;
  engine.firstRateLimitRejectTsNs = 1000;
  engine.lastRateLimitRejectTsNs = 1000;

  BacktestRunResult result{};
  result.status = Status::Ok;
  result.rateLimitsEnabled = true;
  result.strictRateLimitRejects = true;

  const bool failed = applyStrictRateLimitStatus(result, engine);

  return expect(failed, "strict rate-limit mode reports a release-gate failure") &&
         expect(result.status == Status::InvalidArgument,
                "strict rate-limit reject fails the run result") &&
         expect(result.rateLimitStrictFailure, "strict rate-limit failure is exposed on the result") &&
         expect(result.rateLimitRejectedCommands == 1u, "strict rate-limit failure records rejected commands") &&
         expect(result.error.find("strict rate-limit mode") != std::string::npos,
                "strict rate-limit failure explains the release gate");
}

}  // namespace

int main() {
  bool ok = true;
  ok = testBacktestRateLimitScheduleOverlaysMatchingPreset() && ok;
  ok = testStrictRateLimitRejectFailsRunResult() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
