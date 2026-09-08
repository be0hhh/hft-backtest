#include "BacktestStrategyConfig.hpp"

#include "trading_core/Algorithm/RateLimit/RateLimitPresets.hpp"

#include "cxet/Api/Resolve/LocalSymbolValidation.hpp"
#include "cxet/Canon/CanonLookup.hpp"
#include "cxet/Primitives/Buf/StringView.hpp"

#include <charconv>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace hft_backtest {
namespace {

[[nodiscard]] std::string trim(cxet::StringView value) {
  std::size_t first = 0u;
  while (first < value.size() &&
         std::isspace(static_cast<unsigned char>(value[first]))) {
    ++first;
  }
  std::size_t last = value.size();
  while (last > first &&
         std::isspace(static_cast<unsigned char>(value[last - 1u]))) {
    --last;
  }
  const cxet::StringView trimmed = value.substr(first, last - first);
  return std::string{trimmed.data(), trimmed.size()};
}

[[nodiscard]] std::string lower(std::string value) {
  for (char& c : value) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return value;
}

[[nodiscard]] std::vector<std::string> splitCsv(const std::string& value) {
  std::vector<std::string> out{};
  std::size_t start = 0u;
  while (start <= value.size()) {
    const std::size_t comma = value.find(',', start);
    const std::size_t end =
        comma == std::string::npos ? value.size() : comma;
    std::string part = trim(
        cxet::StringView{value.data(), value.size()}.substr(
            start, end - start));
    if (!part.empty()) out.push_back(std::move(part));
    if (comma == std::string::npos) break;
    start = comma + 1u;
  }
  return out;
}

[[nodiscard]] bool parseUint(const std::string& value,
                             unsigned& out) noexcept {
  unsigned parsed = 0u;
  const auto [ptr, ec] =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (ec != std::errc{} || ptr != value.data() + value.size()) return false;
  out = parsed;
  return true;
}

[[nodiscard]] bool parseInt64(const std::string& value,
                              std::int64_t& out) noexcept {
  std::int64_t parsed = 0;
  const auto [ptr, ec] =
      std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (ec != std::errc{} || ptr != value.data() + value.size()) return false;
  out = parsed;
  return true;
}

[[nodiscard]] bool parseBoolStrict(const std::string& value,
                                   bool& out) {
  const std::string normalized = lower(value);
  if (normalized == "1" || normalized == "true" ||
      normalized == "yes" || normalized == "on" ||
      normalized == "enabled") {
    out = true;
    return true;
  }
  if (normalized == "0" || normalized == "false" ||
      normalized == "no" || normalized == "off" ||
      normalized == "disabled") {
    out = false;
    return true;
  }
  return false;
}

[[nodiscard]] bool parseDecimalE8(const std::string& value,
                                  std::int64_t& out) noexcept {
  if (value.empty()) return false;
  const char* cursor = value.c_str();
  bool negative = false;
  if (*cursor == '-') {
    negative = true;
    ++cursor;
  }
  std::int64_t whole = 0;
  bool anyWhole = false;
  while (*cursor >= '0' && *cursor <= '9') {
    anyWhole = true;
    if (whole > std::numeric_limits<std::int64_t>::max() / 10) return false;
    whole = whole * 10 + static_cast<std::int64_t>(*cursor - '0');
    ++cursor;
  }
  std::int64_t fraction = 0;
  std::int64_t scale = 10000000ll;
  if (*cursor == '.') {
    ++cursor;
    while (*cursor >= '0' && *cursor <= '9') {
      if (scale > 0) {
        fraction += static_cast<std::int64_t>(*cursor - '0') * scale;
        scale /= 10;
      }
      ++cursor;
    }
  }
  if (!anyWhole || *cursor != '\0') return false;
  constexpr std::int64_t kScaleE8 = 100000000ll;
  if (whole >
      (std::numeric_limits<std::int64_t>::max() - fraction) / kScaleE8) {
    return false;
  }
  out = whole * kScaleE8 + fraction;
  if (negative) out = -out;
  return true;
}

[[nodiscard]] bool cryptoExchangeRequiresLocalSymbols(
    ExchangeId exchange) noexcept {
  switch (exchange.raw) {
    case canon::kExchangeIdAster.raw:
    case canon::kExchangeIdBinance.raw:
    case canon::kExchangeIdBingx.raw:
    case canon::kExchangeIdBitget.raw:
    case canon::kExchangeIdBybit.raw:
    case canon::kExchangeIdGate.raw:
    case canon::kExchangeIdHtx.raw:
    case canon::kExchangeIdHyperliquid.raw:
    case canon::kExchangeIdKucoin.raw:
    case canon::kExchangeIdMexc.raw:
    case canon::kExchangeIdOkx.raw:
    case canon::kExchangeIdPhemex.raw:
    case canon::kExchangeIdPoloniex.raw:
    case canon::kExchangeIdToobit.raw:
    case canon::kExchangeIdXt.raw:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] bool validLocalCryptoSymbol(const char* symbol) noexcept {
  return cxet::api::isLocalCryptoSymbolText(symbol) ||
      cxet::api::isPotentialRouteExceptionLocalCryptoSymbolText(symbol);
}

[[nodiscard]] bool venueFromSection(
    const std::string& section,
    BacktestStrategyVenueConfig& out) {
  static constexpr std::size_t kPrefixSize = 6u;
  if (!section.starts_with("venue.")) return false;
  out = BacktestStrategyVenueConfig{};
  out.name = lower(section.substr(kPrefixSize));
  const std::size_t separator = out.name.rfind('_');
  if (separator == std::string::npos || separator == 0u ||
      separator + 1u >= out.name.size()) {
    return false;
  }
  const std::string exchange = out.name.substr(0u, separator);
  const std::string market = out.name.substr(separator + 1u);
  out.exchange = canon::exchangeIdFromName(exchange.c_str());
  out.market = canon::marketTypeFromName(market.c_str());
  if (out.exchange.raw == canon::kExchangeIdUnknown.raw ||
      out.market.raw == canon::kMarketTypeUnknown.raw) {
    return false;
  }
  return !(out.exchange.raw == canon::kExchangeIdOkx.raw &&
           out.market.raw == canon::kMarketTypeSwap.raw);
}

[[nodiscard]] bool parseLiveProbeMode(const std::string& value,
                                      BacktestLiveProbeMode& out) {
  const std::string normalized = lower(value);
  if (normalized == "default" || normalized == "inherit" ||
      normalized == "auto") {
    out = BacktestLiveProbeMode::Default;
    return true;
  }
  if (normalized == "required" || normalized == "require" ||
      normalized == "on" || normalized == "true" || normalized == "1") {
    out = BacktestLiveProbeMode::Required;
    return true;
  }
  if (normalized == "disabled" || normalized == "disable" ||
      normalized == "off" || normalized == "false" || normalized == "0") {
    out = BacktestLiveProbeMode::Disabled;
    return true;
  }
  return false;
}

[[nodiscard]] bool parsePortfolioTradeMode(
    const std::string& value,
    BacktestPortfolioTradeMode& out) {
  const std::string normalized = lower(value);
  if (normalized == "all") {
    out = BacktestPortfolioTradeMode::All;
    return true;
  }
  if (normalized == "primary") {
    out = BacktestPortfolioTradeMode::Primary;
    return true;
  }
  return false;
}

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

[[nodiscard]] bool parsePortfolioLeg(const std::string& value,
                                     BacktestPortfolioLegConfig& out) {
  const std::size_t venueEnd = value.find(':');
  if (venueEnd == std::string::npos) return false;
  out = BacktestPortfolioLegConfig{};
  const cxet::StringView view{value.data(), value.size()};
  out.venue = lower(trim(view.substr(0u, venueEnd)));
  const std::string symbolText =
      trim(view.substr(venueEnd + 1u));
  return !out.venue.empty() && out.symbol.copyFrom(symbolText.c_str());
}

[[nodiscard]] bool ignoredVenueOnlyKey(const std::string& key) noexcept {
  return key == "api_slot" || key == "market_enabled" ||
      key == "user_enabled" || key == "order_enabled" ||
      key == "control_enabled" || key == "initial_balance_usdt" ||
      key == "maker_fee_bps" || key == "taker_fee_bps" ||
      key == "recovery_account_exclusive" ||
      key == "market_ws_lanes" || key == "user_ws_lanes" ||
      key == "order_ws_lanes" || key == "transport_tuple_mode" ||
      key == "ecmp_mode" || key == "ecmp_enabled" ||
      key.starts_with("transport_tuple_") || key.starts_with("ecmp_") ||
      key == "api_key" || key == "api_secret" || key == "secret_key" ||
      key == "passphrase" || key == "password" ||
      key.starts_with("credential_") || key.starts_with("credentials_") ||
      key.ends_with("_key_env") || key.ends_with("_secret_env") ||
      key.ends_with("_passphrase_env") || key.starts_with("private_key");
}

[[nodiscard]] const BacktestStrategyVenueConfig* findVenue(
    const BacktestStrategyConfig& config,
    const std::string& name) noexcept {
  for (const BacktestStrategyVenueConfig& venue : config.venues) {
    if (venue.name == name) return &venue;
  }
  return nullptr;
}

[[nodiscard]] bool venueHasSymbol(const BacktestStrategyVenueConfig& venue,
                                  const Symbol& symbol) noexcept {
  const cxet::StringView wanted = cxet::StringView::fromCString(symbol.data);
  for (const Symbol& configured : venue.symbols) {
    if (cxet::StringView::fromCString(configured.data) == wanted) return true;
  }
  return false;
}

[[nodiscard]] bool validateConfig(BacktestStrategyConfig& config,
                                  std::string& error) {
  if (config.strategyType.empty()) {
    error = "config has no [strategy] type";
    return false;
  }
  if (config.venues.empty()) {
    error = "config has no [venue.*] sections";
    return false;
  }
  for (BacktestStrategyVenueConfig& venue : config.venues) {
    if (venue.symbols.empty()) {
      error = "venue has no symbols: " + venue.name;
      return false;
    }
    if (!resolveVenueRateLimitPolicy(venue, error)) return false;
    const bool explicitUnsafeOverride =
        venue.hasRateLimitsEnabled && !venue.rateLimitsEnabled;
    if (config.risk.rateLimitGuardEnabled && !venue.rateLimitsEnabled &&
        !explicitUnsafeOverride) {
      error =
          "risk rate_limit_guard_enabled requires a valid venue rate-limit policy: " +
          venue.name;
      return false;
    }
  }
  for (const BacktestPortfolioConfig& portfolio : config.portfolios) {
    if (portfolio.legs.empty()) {
      error = "portfolio has no legs: " + portfolio.name;
      return false;
    }
    if (portfolio.primaryLegIndex >= portfolio.legs.size()) {
      error = "portfolio primary_leg_index out of range: " + portfolio.name;
      return false;
    }
    for (const BacktestPortfolioLegConfig& leg : portfolio.legs) {
      const BacktestStrategyVenueConfig* venue = findVenue(config, leg.venue);
      if (!venue) {
        error = "portfolio leg references unknown venue: " + portfolio.name +
            ":" + leg.venue;
        return false;
      }
      if (!venueHasSymbol(*venue, leg.symbol)) {
        error = "portfolio leg references symbol not configured on venue: " +
            portfolio.name + ":" + leg.venue + ":" + leg.symbol.data;
        return false;
      }
      if (cryptoExchangeRequiresLocalSymbols(venue->exchange) &&
          !validLocalCryptoSymbol(leg.symbol.data)) {
        error =
            "portfolio crypto leg symbol must use local format BASE_QUOTE or N_BASE_QUOTE: " +
            portfolio.name + " " + leg.venue + ":" + leg.symbol.data;
        return false;
      }
    }
  }
  return true;
}

}  // namespace

bool loadBacktestStrategyConfig(const char* path,
                                BacktestStrategyConfig& config,
                                std::string& error) {
  config = BacktestStrategyConfig{};
  error.clear();
  std::ifstream input(path);
  if (!input) {
    error = "cannot open config: ";
    error += path ? path : "";
    return false;
  }

  std::string section{};
  BacktestStrategyVenueConfig currentVenue{};
  bool hasCurrentVenue = false;
  BacktestPortfolioConfig currentPortfolio{};
  bool hasCurrentPortfolio = false;
  const auto flushVenue = [&]() {
    if (hasCurrentVenue) config.venues.push_back(std::move(currentVenue));
    currentVenue = BacktestStrategyVenueConfig{};
    hasCurrentVenue = false;
  };
  const auto flushPortfolio = [&]() {
    if (hasCurrentPortfolio) {
      config.portfolios.push_back(std::move(currentPortfolio));
    }
    currentPortfolio = BacktestPortfolioConfig{};
    hasCurrentPortfolio = false;
  };

  std::string line{};
  while (std::getline(input, line)) {
    const std::size_t comment = line.find('#');
    if (comment != std::string::npos) line.resize(comment);
    line = trim(cxet::StringView{line.data(), line.size()});
    if (line.empty()) continue;
    if (line.front() == '[' && line.back() == ']') {
      flushVenue();
      flushPortfolio();
      section = lower(trim(
          cxet::StringView{line.data(), line.size()}.substr(
              1u, line.size() - 2u)));
      hasCurrentVenue = venueFromSection(section, currentVenue);
      if (section.starts_with("venue.") && !hasCurrentVenue) {
        error = "invalid venue section: " + section;
        return false;
      }
      static constexpr std::size_t kPortfolioPrefixSize = 10u;
      if (section.starts_with("portfolio.")) {
        currentPortfolio = BacktestPortfolioConfig{};
        currentPortfolio.name = section.substr(kPortfolioPrefixSize);
        if (currentPortfolio.name.empty()) {
          error = "invalid portfolio section: " + section;
          return false;
        }
        hasCurrentPortfolio = true;
      }
      continue;
    }

    const std::size_t equals = line.find('=');
    if (equals == std::string::npos) continue;
    const cxet::StringView lineView{line.data(), line.size()};
    const std::string key = lower(trim(lineView.substr(0u, equals)));
    const std::string value = trim(lineView.substr(equals + 1u));

    if (section == "strategy") {
      if (key == "type") {
        config.strategyType = lower(value);
      } else if (key == "inputs") {
        error =
            "strategy inputs override is not supported; streams come from trading_core::StrategyDescriptor";
        return false;
      } else if (key == "enabled") {
        bool enabled = false;
        if (!parseBoolStrict(value, enabled)) {
          error = "invalid strategy enabled";
          return false;
        }
        config.strategyDefaults.enabled = enabled;
      } else {
        trading_core::setStrategyParam(config.strategyDefaults, key, value);
      }
      continue;
    }

    if (section == "risk") {
      if (key == "enabled") {
        if (!parseBoolStrict(value, config.risk.enabled)) {
          error = "invalid risk enabled";
          return false;
        }
      } else if (key == "min_equity_pct") {
        if (!parseDecimalE8(value, config.risk.minEquityPctE8) ||
            config.risk.minEquityPctE8 < 0 ||
            config.risk.minEquityPctE8 > 10000000000ll) {
          error = "invalid risk min_equity_pct";
          return false;
        }
        config.risk.hasMinEquityPct = true;
      } else if (key == "min_leg_equity_pct") {
        if (!parseDecimalE8(value, config.risk.minLegEquityPctE8) ||
            config.risk.minLegEquityPctE8 < 0 ||
            config.risk.minLegEquityPctE8 > 10000000000ll) {
          error = "invalid risk min_leg_equity_pct";
          return false;
        }
        config.risk.hasMinLegEquityPct = true;
      } else if (key == "min_leg_equity_usdt") {
        if (!parseDecimalE8(value, config.risk.minLegEquityE8) ||
            config.risk.minLegEquityE8 < 0) {
          error = "invalid risk min_leg_equity_usdt";
          return false;
        }
        config.risk.hasMinLegEquity = true;
      } else if (key == "rate_limit_guard_enabled") {
        if (!parseBoolStrict(value, config.risk.rateLimitGuardEnabled)) {
          error = "invalid risk rate_limit_guard_enabled";
          return false;
        }
      } else if (key == "rate_limit_guard_min_remaining") {
        unsigned parsed = 0u;
        if (!parseUint(value, parsed)) {
          error = "invalid risk rate_limit_guard_min_remaining";
          return false;
        }
        config.risk.rateLimitGuardMinRemaining =
            static_cast<std::int64_t>(parsed);
      }
      continue;
    }

    if (hasCurrentVenue) {
      if (key == "symbols") {
        currentVenue.symbols.clear();
        for (const std::string& symbolText : splitCsv(value)) {
          if (cryptoExchangeRequiresLocalSymbols(currentVenue.exchange) &&
              !validLocalCryptoSymbol(symbolText.c_str())) {
            error =
                "crypto venue symbol must use local format BASE_QUOTE or N_BASE_QUOTE: " +
                currentVenue.name + " " + symbolText;
            return false;
          }
          Symbol symbol{};
          if (!symbol.copyFrom(symbolText.c_str())) {
            error = "venue symbol is empty or exceeds Symbol capacity: " +
                currentVenue.name + " " + symbolText;
            return false;
          }
          currentVenue.symbols.push_back(symbol);
        }
      } else if (key == "live_probe") {
        if (!parseLiveProbeMode(value, currentVenue.liveProbe)) {
          error = "invalid venue live_probe: " + currentVenue.name;
          return false;
        }
      } else if (key == "rate_limits_enabled") {
        if (!parseBoolStrict(value, currentVenue.rateLimitsEnabled)) {
          error = "invalid venue rate_limits_enabled: " + currentVenue.name;
          return false;
        }
        currentVenue.hasRateLimitsEnabled = true;
      } else if (key == "risk_min_equity_pct") {
        if (!parseDecimalE8(value, currentVenue.riskMinEquityPctE8) ||
            currentVenue.riskMinEquityPctE8 < 0 ||
            currentVenue.riskMinEquityPctE8 > 10000000000ll) {
          error = "invalid venue risk_min_equity_pct: " + currentVenue.name;
          return false;
        }
        currentVenue.hasRiskMinEquityPct = true;
      } else if (key == "risk_min_equity_usdt") {
        if (!parseDecimalE8(value, currentVenue.riskMinEquityE8) ||
            currentVenue.riskMinEquityE8 < 0) {
          error = "invalid venue risk_min_equity_usdt: " + currentVenue.name;
          return false;
        }
        currentVenue.hasRiskMinEquity = true;
      } else if (key == "replace_policy" ||
                 key == "order_replace_policy") {
        error =
            "replace policy is fixed: cancel -> terminal cancel outcome -> new; configurable replace_policy is unsupported: " +
            currentVenue.name;
        return false;
      } else if (parseVenueRateLimitKey(
                     currentVenue, key, value, error)) {
        if (!error.empty()) return false;
      } else if (!ignoredVenueOnlyKey(key)) {
        if (key == "enabled") {
          bool enabled = false;
          if (!parseBoolStrict(value, enabled)) {
            error = "invalid venue strategy enabled: " + currentVenue.name;
            return false;
          }
          currentVenue.params.enabled = enabled;
        } else {
          trading_core::setStrategyParam(currentVenue.params, key, value);
        }
      }
      continue;
    }

    if (hasCurrentPortfolio) {
      if (key == "legs") {
        currentPortfolio.legs.clear();
        std::uint32_t nextRoute = 1u;
        for (const std::string& legText : splitCsv(value)) {
          BacktestPortfolioLegConfig leg{};
          if (!parsePortfolioLeg(legText, leg)) {
            error = "invalid portfolio leg: " + currentPortfolio.name +
                ":" + legText;
            return false;
          }
          leg.routeHandle = trading_core::ExecutionRouteHandle{nextRoute++};
          currentPortfolio.legs.push_back(std::move(leg));
        }
      } else if (key == "primary_leg_index") {
        unsigned parsed = 0u;
        if (!parseUint(value, parsed)) {
          error = "invalid portfolio primary_leg_index: " +
              currentPortfolio.name;
          return false;
        }
        currentPortfolio.primaryLegIndex = parsed;
      } else if (key == "trade_mode") {
        if (!parsePortfolioTradeMode(value, currentPortfolio.tradeMode)) {
          error = "invalid portfolio trade_mode: " + currentPortfolio.name;
          return false;
        }
      } else if (key == "primary") {
        error = "portfolio primary leg is no longer supported: " +
            currentPortfolio.name;
        return false;
      }
    }
  }

  flushVenue();
  flushPortfolio();
  return validateConfig(config, error);
}

}  // namespace hft_backtest
