#include "BacktestEngineSetup.hpp"

#include "trading_core/Algorithm/RateLimit/RateLimit.hpp"

namespace hft_backtest {

bool applyRateLimitSchedule(detail::EngineState& engine,
                            const BacktestRateLimitSchedule& schedule,
                            std::string& error) {
  if (!engine.rateLimits.reset()) {
    error = "backtest rate-limit reset requires completed reservations";
    return false;
  }
  auto hasBucket = [&](trading_core::RateLimitBucketKind kind) noexcept {
    for (const trading_core::RateLimitBucketConfig& bucket : schedule.buckets) {
      if (bucket.enabled && bucket.kind == kind) return true;
    }
    return false;
  };
  auto hasAction = [&](trading_core::RateLimitActionKind action) noexcept {
    for (const trading_core::RateLimitActionConfig& configured : schedule.actions) {
      if (configured.enabled && configured.action == action) return true;
    }
    return false;
  };
  for (const trading_core::RateLimitBucketConfig& bucket : schedule.buckets) {
    if (!engine.rateLimits.configureBucket(bucket)) {
      error = "invalid backtest rate-limit bucket for " + schedule.exchange + "/" + schedule.market;
      return false;
    }
  }
  for (const trading_core::RateLimitActionConfig& action : schedule.actions) {
    if (!engine.rateLimits.configureAction(action)) {
      error = "invalid backtest rate-limit action for " + schedule.exchange + "/" + schedule.market;
      return false;
    }
  }
  trading_core::RateLimitBucketKind orderBucket = trading_core::RateLimitBucketKind::Unknown;
  if (hasBucket(trading_core::RateLimitBucketKind::Orders)) {
    orderBucket = trading_core::RateLimitBucketKind::Orders;
  } else if (hasBucket(trading_core::RateLimitBucketKind::RequestWeight)) {
    orderBucket = trading_core::RateLimitBucketKind::RequestWeight;
  } else if (hasBucket(trading_core::RateLimitBucketKind::Requests)) {
    orderBucket = trading_core::RateLimitBucketKind::Requests;
  }
  const trading_core::RateLimitBucketKind cancelBucket =
      hasBucket(trading_core::RateLimitBucketKind::CancelOrders)
          ? trading_core::RateLimitBucketKind::CancelOrders
          : orderBucket;
  const trading_core::RateLimitBucketKind reduceOnlyBucket =
      hasBucket(trading_core::RateLimitBucketKind::ReduceOnlyOrders)
          ? trading_core::RateLimitBucketKind::ReduceOnlyOrders
          : orderBucket;
  auto configureDefaultAction = [&](trading_core::RateLimitActionKind action,
                                    trading_core::RateLimitBucketKind bucket) {
    if (bucket == trading_core::RateLimitBucketKind::Unknown || hasAction(action)) return true;
    const trading_core::RateLimitActionConfig config = trading_core::singleBucketRateLimitAction(action, bucket, 1);
    if (engine.rateLimits.configureAction(config)) return true;
    error = "invalid default backtest rate-limit action for " + schedule.exchange + "/" + schedule.market;
    return false;
  };
  if (!configureDefaultAction(trading_core::RateLimitActionKind::LimitOrder, orderBucket)) return false;
  if (!configureDefaultAction(trading_core::RateLimitActionKind::MarketOrder, orderBucket)) return false;
  if (!configureDefaultAction(trading_core::RateLimitActionKind::CancelOrder, cancelBucket)) return false;
  if (!configureDefaultAction(trading_core::RateLimitActionKind::ReduceOnlyLimitOrder, reduceOnlyBucket)) return false;
  if (!configureDefaultAction(trading_core::RateLimitActionKind::ReduceOnlyMarketOrder, reduceOnlyBucket)) return false;
  return true;
}

}  // namespace hft_backtest
