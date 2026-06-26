#include "engine/backtest_engine_setup.hpp"

#include "hft_trader/core/rate_limit/RateLimit.hpp"

namespace hft_backtest {

bool applyRateLimitSchedule(detail::EngineState& engine,
                            const BacktestRateLimitSchedule& schedule,
                            std::string& error) {
  engine.rateLimits = hft_trader::core::RateLimitState{};
  auto hasBucket = [&](hft_trader::core::RateLimitBucketKind kind) noexcept {
    for (const hft_trader::core::RateLimitBucketConfig& bucket : schedule.buckets) {
      if (bucket.enabled && bucket.kind == kind) return true;
    }
    return false;
  };
  auto hasAction = [&](hft_trader::core::RateLimitActionKind action) noexcept {
    for (const hft_trader::core::RateLimitActionConfig& configured : schedule.actions) {
      if (configured.enabled && configured.action == action) return true;
    }
    return false;
  };
  for (const hft_trader::core::RateLimitBucketConfig& bucket : schedule.buckets) {
    if (!engine.rateLimits.configureBucket(bucket)) {
      error = "invalid backtest rate-limit bucket for " + schedule.exchange + "/" + schedule.market;
      return false;
    }
  }
  for (const hft_trader::core::RateLimitActionConfig& action : schedule.actions) {
    if (!engine.rateLimits.configureAction(action)) {
      error = "invalid backtest rate-limit action for " + schedule.exchange + "/" + schedule.market;
      return false;
    }
  }
  hft_trader::core::RateLimitBucketKind orderBucket = hft_trader::core::RateLimitBucketKind::Unknown;
  if (hasBucket(hft_trader::core::RateLimitBucketKind::Orders)) {
    orderBucket = hft_trader::core::RateLimitBucketKind::Orders;
  } else if (hasBucket(hft_trader::core::RateLimitBucketKind::RequestWeight)) {
    orderBucket = hft_trader::core::RateLimitBucketKind::RequestWeight;
  } else if (hasBucket(hft_trader::core::RateLimitBucketKind::Requests)) {
    orderBucket = hft_trader::core::RateLimitBucketKind::Requests;
  }
  const hft_trader::core::RateLimitBucketKind cancelBucket =
      hasBucket(hft_trader::core::RateLimitBucketKind::CancelOrders)
          ? hft_trader::core::RateLimitBucketKind::CancelOrders
          : orderBucket;
  const hft_trader::core::RateLimitBucketKind reduceOnlyBucket =
      hasBucket(hft_trader::core::RateLimitBucketKind::ReduceOnlyOrders)
          ? hft_trader::core::RateLimitBucketKind::ReduceOnlyOrders
          : orderBucket;
  auto configureDefaultAction = [&](hft_trader::core::RateLimitActionKind action,
                                    hft_trader::core::RateLimitBucketKind bucket) {
    if (bucket == hft_trader::core::RateLimitBucketKind::Unknown || hasAction(action)) return true;
    const hft_trader::core::RateLimitActionConfig config = hft_trader::core::singleBucketRateLimitAction(action, bucket, 1);
    if (engine.rateLimits.configureAction(config)) return true;
    error = "invalid default backtest rate-limit action for " + schedule.exchange + "/" + schedule.market;
    return false;
  };
  if (!configureDefaultAction(hft_trader::core::RateLimitActionKind::LimitOrder, orderBucket)) return false;
  if (!configureDefaultAction(hft_trader::core::RateLimitActionKind::MarketOrder, orderBucket)) return false;
  if (!configureDefaultAction(hft_trader::core::RateLimitActionKind::CancelOrder, cancelBucket)) return false;
  if (!configureDefaultAction(hft_trader::core::RateLimitActionKind::ReduceOnlyLimitOrder, reduceOnlyBucket)) return false;
  if (!configureDefaultAction(hft_trader::core::RateLimitActionKind::ReduceOnlyMarketOrder, reduceOnlyBucket)) return false;
  return true;
}

}  // namespace hft_backtest
