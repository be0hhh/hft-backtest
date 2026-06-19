#include "backtest_latency.hpp"

#include <limits>

namespace hft_backtest::detail {
namespace {

[[nodiscard]] std::uint64_t mixSeed(std::uint64_t seed, std::uint64_t salt) noexcept {
  std::uint64_t x = seed == 0u ? 0x9e3779b97f4a7c15ull : seed;
  x ^= salt + 0x9e3779b97f4a7c15ull + (x << 6u) + (x >> 2u);
  x ^= x >> 30u;
  x *= 0xbf58476d1ce4e5b9ull;
  x ^= x >> 27u;
  x *= 0x94d049bb133111ebull;
  x ^= x >> 31u;
  return x == 0u ? 0x2545f4914f6cdd1dull : x;
}

[[nodiscard]] std::uint64_t nextRandom(std::uint64_t& state) noexcept {
  std::uint64_t x = state;
  x ^= x << 13u;
  x ^= x >> 7u;
  x ^= x << 17u;
  state = x == 0u ? 0x2545f4914f6cdd1dull : x;
  return state;
}

[[nodiscard]] std::uint64_t boundedRandom(std::uint64_t& state, std::uint64_t bound) noexcept {
  if (bound == 0u) return 0u;
  return nextRandom(state) % bound;
}

[[nodiscard]] std::uint64_t saturatingAdd(std::uint64_t a, std::uint64_t b) noexcept {
  if (a > std::numeric_limits<std::uint64_t>::max() - b) return std::numeric_limits<std::uint64_t>::max();
  return a + b;
}

[[nodiscard]] BacktestLatencyProfile legacyProfile(std::uint64_t latencyUs) noexcept {
  BacktestLatencyProfile out{};
  out.baseUs = latencyUs;
  return out;
}

[[nodiscard]] std::uint64_t sampleProfile(BacktestLatencyProfile profile, std::uint64_t& state) noexcept {
  if (profile.jitterUs == 0u) return profile.baseUs;
  const std::uint64_t roll = boundedRandom(state, 10000u);
  const std::uint64_t jitter = profile.jitterUs;
  if (roll < 9700u) {
    const std::uint64_t half = jitter / 5u;
    const std::uint64_t span = half == 0u ? 1u : half;
    const std::uint64_t delta = boundedRandom(state, span + 1u);
    const bool negative = (nextRandom(state) & 1u) != 0u;
    if (negative) return profile.baseUs > delta ? profile.baseUs - delta : 0u;
    return saturatingAdd(profile.baseUs, delta);
  }
  if (roll < 9990u) {
    const std::uint64_t lowDelta = jitter >> 1u;
    const std::uint64_t low = profile.baseUs > lowDelta ? profile.baseUs - lowDelta : 0u;
    const std::uint64_t high = saturatingAdd(profile.baseUs, jitter + lowDelta);
    const std::uint64_t span = high >= low ? high - low : 0u;
    return saturatingAdd(low, boundedRandom(state, span + 1u));
  }
  const std::uint64_t spike = jitter + (jitter << 1u) + (jitter << 2u);
  const std::uint64_t high = saturatingAdd(profile.baseUs, spike + (jitter >> 1u));
  const std::uint64_t low = saturatingAdd(profile.baseUs, jitter + jitter);
  const std::uint64_t span = high >= low ? high - low : 0u;
  return saturatingAdd(low, boundedRandom(state, span + 1u));
}

}  // namespace

BacktestLatencyConfig latencyConfigFromRequest(const BacktestRunRequest& request) noexcept {
  BacktestLatencyConfig out{};
  out.marketData = request.marketDataLatency;
  out.marketOrder = request.marketOrderLatency.baseUs != 0u || request.marketOrderLatency.jitterUs != 0u
      ? request.marketOrderLatency
      : legacyProfile(request.orderLatencyUs);
  out.limitOrder = request.limitOrderLatency.baseUs != 0u || request.limitOrderLatency.jitterUs != 0u
      ? request.limitOrderLatency
      : legacyProfile(request.orderLatencyUs);
  out.cancelOrder = request.cancelOrderLatency.baseUs != 0u || request.cancelOrderLatency.jitterUs != 0u
      ? request.cancelOrderLatency
      : (request.cancelLatencyUs != 1000u ? legacyProfile(request.cancelLatencyUs) : out.limitOrder);
  out.userData = request.userDataLatency;
  out.seed = request.latencySeed;
  return out;
}

void initLatencyRuntime(BacktestLatencyRuntime& runtime, const BacktestLatencyConfig& config) noexcept {
  runtime.config = config;
  runtime.marketDataState = mixSeed(config.seed, 0x6d61726b65745f64ull);
  runtime.marketOrderState = mixSeed(config.seed, 0x6d61726b65745f6full);
  runtime.limitOrderState = mixSeed(config.seed, 0x6c696d69745f6full);
  runtime.cancelOrderState = mixSeed(config.seed, 0x63616e63656c5f6full);
  runtime.userDataState = mixSeed(config.seed, 0x757365725f646174ull);
}

std::uint64_t latencyProfileMaxUs(BacktestLatencyProfile profile) noexcept {
  if (profile.jitterUs == 0u) return profile.baseUs;
  const std::uint64_t spike = profile.jitterUs + (profile.jitterUs << 1u) + (profile.jitterUs << 2u);
  return saturatingAdd(profile.baseUs, spike + (profile.jitterUs >> 1u));
}

std::uint64_t sampleLatencyUs(BacktestLatencyRuntime& runtime, BacktestLatencyChannel channel) noexcept {
  switch (channel) {
    case BacktestLatencyChannel::MarketData: return sampleProfile(runtime.config.marketData, runtime.marketDataState);
    case BacktestLatencyChannel::MarketOrder: return sampleProfile(runtime.config.marketOrder, runtime.marketOrderState);
    case BacktestLatencyChannel::LimitOrder: return sampleProfile(runtime.config.limitOrder, runtime.limitOrderState);
    case BacktestLatencyChannel::CancelOrder: return sampleProfile(runtime.config.cancelOrder, runtime.cancelOrderState);
    case BacktestLatencyChannel::UserData: return sampleProfile(runtime.config.userData, runtime.userDataState);
    default: return 0u;
  }
}

}  // namespace hft_backtest::detail
