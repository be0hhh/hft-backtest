#include "Internal/BacktestStrategyConfigHelpers.hpp"

namespace hft_backtest::strategy_config {

[[nodiscard]] bool parseRateLimitBucketKind(
    const std::string& value,
    trading_core::RateLimitBucketKind& out) {
  const std::string normalized = lower(value);
  if (normalized == "request_weight" || normalized == "request-weight" ||
      normalized == "weight") {
    out = trading_core::RateLimitBucketKind::RequestWeight;
    return true;
  }
  if (normalized == "orders" || normalized == "order") {
    out = trading_core::RateLimitBucketKind::Orders;
    return true;
  }
  if (normalized == "requests" || normalized == "request") {
    out = trading_core::RateLimitBucketKind::Requests;
    return true;
  }
  if (normalized == "cancel_orders" || normalized == "cancel-orders" ||
      normalized == "cancels") {
    out = trading_core::RateLimitBucketKind::CancelOrders;
    return true;
  }
  if (normalized == "reduce_only_orders" ||
      normalized == "reduce-only-orders" || normalized == "reduce_only") {
    out = trading_core::RateLimitBucketKind::ReduceOnlyOrders;
    return true;
  }
  return false;
}

[[nodiscard]] bool parseRateLimitActionKind(
    const std::string& value,
    trading_core::RateLimitActionKind& out) {
  const std::string normalized = lower(value);
  if (normalized == "limit_order" || normalized == "limit-order") {
    out = trading_core::RateLimitActionKind::LimitOrder;
    return true;
  }
  if (normalized == "market_order" || normalized == "market-order") {
    out = trading_core::RateLimitActionKind::MarketOrder;
    return true;
  }
  if (normalized == "cancel_order" || normalized == "cancel-order" ||
      normalized == "cancel") {
    out = trading_core::RateLimitActionKind::CancelOrder;
    return true;
  }
  if (normalized == "replace_order" || normalized == "replace-order" ||
      normalized == "replace") {
    out = trading_core::RateLimitActionKind::ReplaceOrder;
    return true;
  }
  if (normalized == "reduce_only_limit_order" ||
      normalized == "reduce-only-limit-order") {
    out = trading_core::RateLimitActionKind::ReduceOnlyLimitOrder;
    return true;
  }
  if (normalized == "reduce_only_market_order" ||
      normalized == "reduce-only-market-order" ||
      normalized == "reduce_only") {
    out = trading_core::RateLimitActionKind::ReduceOnlyMarketOrder;
    return true;
  }
  if (normalized == "ws_subscribe" || normalized == "ws-subscribe") {
    out = trading_core::RateLimitActionKind::WsSubscribe;
    return true;
  }
  if (normalized == "rest_order_query" ||
      normalized == "rest-order-query") {
    out = trading_core::RateLimitActionKind::RestOrderQuery;
    return true;
  }
  return false;
}

[[nodiscard]] bool parseRateLimitScopeKind(
    const std::string& value,
    trading_core::RateLimitScopeKind& out) {
  const std::string normalized = lower(value);
  if (normalized == "account" || normalized == "uid") {
    out = trading_core::RateLimitScopeKind::Account;
    return true;
  }
  if (normalized == "ip") {
    out = trading_core::RateLimitScopeKind::Ip;
    return true;
  }
  if (normalized == "instrument" || normalized == "symbol") {
    out = trading_core::RateLimitScopeKind::Instrument;
    return true;
  }
  if (normalized == "connection" || normalized == "session") {
    out = trading_core::RateLimitScopeKind::Connection;
    return true;
  }
  return false;
}

[[nodiscard]] bool parseRateLimitWindowMode(
    const std::string& value,
    trading_core::RateLimitWindowMode& out) {
  const std::string normalized = lower(value);
  if (normalized == "fixed") {
    out = trading_core::RateLimitWindowMode::Fixed;
    return true;
  }
  if (normalized == "sliding" ||
      normalized == "sliding_from_first_use" ||
      normalized == "sliding-from-first-use") {
    out = trading_core::RateLimitWindowMode::SlidingFromFirstUse;
    return true;
  }
  return false;
}

[[nodiscard]] trading_core::RateLimitBucketConfig* upsertRateLimitBucket(
    BacktestStrategyVenueConfig& venue,
    trading_core::RateLimitBucketKind kind,
    std::uint8_t configSlot) {
  for (trading_core::RateLimitBucketConfig& bucket : venue.rateLimitBuckets) {
    if (bucket.kind == kind && bucket.configSlot == configSlot) return &bucket;
  }
  if (venue.rateLimitBuckets.size() >= trading_core::kMaxRateLimitBuckets) {
    return nullptr;
  }
  venue.rateLimitBuckets.push_back(trading_core::RateLimitBucketConfig{});
  trading_core::RateLimitBucketConfig& out = venue.rateLimitBuckets.back();
  out.kind = kind;
  out.configSlot = configSlot;
  out.enabled = true;
  return &out;
}

[[nodiscard]] trading_core::RateLimitActionConfig* upsertRateLimitAction(
    BacktestStrategyVenueConfig& venue,
    trading_core::RateLimitActionKind action) {
  for (trading_core::RateLimitActionConfig& row : venue.rateLimitActions) {
    if (row.action == action) return &row;
  }
  if (venue.rateLimitActions.size() >= trading_core::kMaxRateLimitActions) {
    return nullptr;
  }
  venue.rateLimitActions.push_back(trading_core::RateLimitActionConfig{});
  trading_core::RateLimitActionConfig& out = venue.rateLimitActions.back();
  out.action = action;
  out.enabled = true;
  return &out;
}

[[nodiscard]] bool setRateLimitActionCost(
    trading_core::RateLimitActionConfig& action,
    trading_core::RateLimitBucketKind bucket,
    std::int64_t cost) noexcept {
  for (std::uint8_t i = 0u; i < action.costCount; ++i) {
    if (action.costs[i].bucket != bucket) continue;
    action.costs[i].cost = cost;
    return true;
  }
  if (cost <= 0) return true;
  if (action.costCount >= action.costs.size()) return false;
  action.costs[action.costCount].bucket = bucket;
  action.costs[action.costCount].cost = cost;
  ++action.costCount;
  return true;
}

[[nodiscard]] bool parseVenueRateLimitKey(
    BacktestStrategyVenueConfig& venue,
    const std::string& key,
    const std::string& value,
    std::string& error) {
  static constexpr std::size_t kBucketPrefixSize = 18u;
  if (key.starts_with("rate_limit_bucket_")) {
    const std::string rest = key.substr(kBucketPrefixSize);
    enum class BucketField : std::uint8_t {
      Limit,
      IntervalMs,
      Scope,
      WindowMode,
    };
    BucketField field{};
    std::size_t suffixLength = 0u;
    if (rest.ends_with("_interval_ms")) {
      field = BucketField::IntervalMs;
      suffixLength = 12u;
    } else if (rest.ends_with("_window_mode")) {
      field = BucketField::WindowMode;
      suffixLength = 12u;
    } else if (rest.ends_with("_scope")) {
      field = BucketField::Scope;
      suffixLength = 6u;
    } else if (rest.ends_with("_limit")) {
      field = BucketField::Limit;
      suffixLength = 6u;
    } else {
      error = "invalid venue rate-limit bucket key: " + venue.name;
      return true;
    }

    std::string bucketText = rest.substr(0u, rest.size() - suffixLength);
    std::uint8_t configSlot = 0u;
    const std::size_t slotSeparator = bucketText.rfind('_');
    if (slotSeparator != std::string::npos &&
        slotSeparator + 1u < bucketText.size()) {
      const std::string slotText = bucketText.substr(slotSeparator + 1u);
      bool numeric = true;
      for (char c : slotText) {
        if (c < '0' || c > '9') {
          numeric = false;
          break;
        }
      }
      if (numeric) {
        unsigned parsedSlot = 0u;
        if (!parseUint(slotText, parsedSlot) ||
            parsedSlot >= trading_core::kMaxRateLimitBuckets) {
          error = "invalid venue rate-limit bucket slot: " + venue.name;
          return true;
        }
        configSlot = static_cast<std::uint8_t>(parsedSlot);
        bucketText.resize(slotSeparator);
      }
    }

    trading_core::RateLimitBucketKind kind{};
    if (!parseRateLimitBucketKind(bucketText, kind)) {
      error = "invalid venue rate-limit bucket kind: " + venue.name;
      return true;
    }
    trading_core::RateLimitBucketConfig* bucket =
        upsertRateLimitBucket(venue, kind, configSlot);
    if (!bucket) {
      error = "too many venue rate-limit buckets: " + venue.name;
      return true;
    }
    if (field == BucketField::Limit) {
      std::int64_t parsed = 0;
      if (!parseInt64(value, parsed) || parsed <= 0) {
        error = "invalid venue rate-limit bucket limit: " + venue.name;
        return true;
      }
      bucket->limit = parsed;
    } else if (field == BucketField::IntervalMs) {
      unsigned parsed = 0u;
      if (!parseUint(value, parsed) || parsed == 0u) {
        error = "invalid venue rate-limit bucket interval_ms: " + venue.name;
        return true;
      }
      bucket->intervalNs = static_cast<std::uint64_t>(parsed) * 1000000ull;
    } else if (field == BucketField::Scope) {
      if (!parseRateLimitScopeKind(value, bucket->scope)) {
        error = "invalid venue rate-limit bucket scope: " + venue.name;
        return true;
      }
    } else if (!parseRateLimitWindowMode(value, bucket->windowMode)) {
      error = "invalid venue rate-limit bucket window_mode: " + venue.name;
      return true;
    }
    bucket->enabled = true;
    return true;
  }

  static constexpr std::size_t kActionPrefixSize = 18u;
  if (!key.starts_with("rate_limit_action_")) return false;
  const std::string rest = key.substr(kActionPrefixSize);
  static constexpr cxet::StringView kBucketTokens[]{
      "_request_weight",
      "_reduce_only_orders",
      "_cancel_orders",
      "_requests",
      "_orders",
  };
  const cxet::StringView restView{rest.data(), rest.size()};
  for (cxet::StringView suffix : kBucketTokens) {
    if (rest.size() <= suffix.size() || !restView.endsWith(suffix)) continue;
    const std::string actionText =
        rest.substr(0u, rest.size() - suffix.size());
    const std::string bucketText{suffix.data() + 1u, suffix.size() - 1u};
    trading_core::RateLimitActionKind actionKind{};
    trading_core::RateLimitBucketKind bucketKind{};
    if (!parseRateLimitActionKind(actionText, actionKind) ||
        !parseRateLimitBucketKind(bucketText, bucketKind)) {
      error = "invalid venue rate-limit action key: " + venue.name;
      return true;
    }
    std::int64_t cost = 0;
    if (!parseInt64(value, cost) || cost <= 0) {
      error = "invalid venue rate-limit action cost: " + venue.name;
      return true;
    }
    trading_core::RateLimitActionConfig* action =
        upsertRateLimitAction(venue, actionKind);
    if (!action) {
      error = "too many rate-limit actions: " + venue.name;
      return true;
    }
    if (!setRateLimitActionCost(*action, bucketKind, cost)) {
      error = "too many rate-limit buckets for action: " + venue.name;
    }
    return true;
  }
  error = "invalid venue rate-limit action key: " + venue.name;
  return true;
}

[[nodiscard]] bool resolveVenueRateLimitPolicy(
    BacktestStrategyVenueConfig& venue,
    std::string& error) noexcept {
  trading_core::RateLimitPreset preset =
      trading_core::defaultExchangeRateLimitPreset(venue.exchange, venue.market);
  const bool hasPolicyInput = !preset.empty() ||
      !venue.rateLimitBuckets.empty() || !venue.rateLimitActions.empty();
  if (!venue.hasRateLimitsEnabled && !hasPolicyInput) {
    venue.rateLimitsEnabled = false;
    return true;
  }
  if (venue.hasRateLimitsEnabled && !venue.rateLimitsEnabled) return true;

  for (const trading_core::RateLimitBucketConfig& bucket :
       venue.rateLimitBuckets) {
    if (!trading_core::upsertRateLimitBucket(preset, bucket)) {
      error = "invalid venue rate-limit bucket policy: " + venue.name;
      return false;
    }
  }
  for (const trading_core::RateLimitActionConfig& action :
       venue.rateLimitActions) {
    if (!trading_core::upsertRateLimitAction(preset, action)) {
      error = "invalid venue rate-limit action policy: " + venue.name;
      return false;
    }
  }
  if (!trading_core::addDefaultRateLimitActions(preset) ||
      preset.bucketCount == 0u || preset.actionCount == 0u) {
    error =
        "rate_limits_enabled requires a valid preset or explicit buckets/actions: " +
        venue.name;
    return false;
  }
  for (std::uint8_t i = 0u; i < preset.bucketCount; ++i) {
    const trading_core::RateLimitBucketConfig& bucket = preset.buckets[i];
    if (!bucket.enabled ||
        bucket.kind == trading_core::RateLimitBucketKind::Unknown ||
        bucket.limit <= 0 || bucket.intervalNs == 0u) {
      error = "invalid venue rate-limit bucket policy: " + venue.name;
      return false;
    }
  }
  for (std::uint8_t i = 0u; i < preset.actionCount; ++i) {
    const trading_core::RateLimitActionConfig& action = preset.actions[i];
    if (!action.enabled ||
        action.action == trading_core::RateLimitActionKind::Unknown ||
        action.costCount == 0u || action.costCount > action.costs.size()) {
      error = "invalid venue rate-limit action policy: " + venue.name;
      return false;
    }
    for (std::uint8_t costIndex = 0u; costIndex < action.costCount;
         ++costIndex) {
      const trading_core::RateLimitCost& cost = action.costs[costIndex];
      if (cost.bucket == trading_core::RateLimitBucketKind::Unknown ||
          cost.cost <= 0 ||
          !trading_core::rateLimitPresetHasBucket(preset, cost.bucket)) {
        error = "rate-limit action references missing bucket: " + venue.name;
        return false;
      }
    }
  }
  venue.rateLimitsEnabled = true;
  return true;
}

}  // namespace hft_backtest::strategy_config
