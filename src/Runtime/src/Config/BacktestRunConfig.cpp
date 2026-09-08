#include "trading_core/Algorithm/Math/Integer/Saturating.hpp"
#include "BacktestRunConfig.hpp"

#include "trading_core/Algorithm/Fees/FeePresets.hpp"
#include "trading_core/Algorithm/Math/PriceQuantity/NotionalQty.hpp"
#include "trading_core/Algorithm/RateLimit/RateLimitPresets.hpp"
#include "trading_core/Strategy/StrategyMarketData.hpp"

#include <chrono>
#include <string>
#include <utility>

namespace hft_backtest {
namespace {

BacktestRateLimitSchedule scheduleFromPreset(ExchangeId exchange,
                                             canon::MarketType market,
                                             const std::string& exchangeText,
                                             const std::string& marketText) {
  BacktestRateLimitSchedule out{};
  out.exchange = exchangeText;
  out.market = marketText;
  trading_core::RateLimitPreset preset = trading_core::defaultExchangeRateLimitPreset(exchange, market);
  (void)trading_core::addDefaultRateLimitActions(preset);
  out.buckets.reserve(preset.bucketCount);
  out.actions.reserve(preset.actionCount);
  for (std::uint8_t i = 0u; i < preset.bucketCount; ++i) out.buckets.push_back(preset.buckets[i]);
  for (std::uint8_t i = 0u; i < preset.actionCount; ++i) out.actions.push_back(preset.actions[i]);
  return out;
}

void overlayRateLimitSchedule(BacktestRateLimitSchedule& base,
                              const BacktestRateLimitSchedule& overlay) {
  for (const trading_core::RateLimitBucketConfig& bucket : overlay.buckets) {
    bool replaced = false;
    for (trading_core::RateLimitBucketConfig& existing : base.buckets) {
      if (existing.kind != bucket.kind ||
          existing.intervalNs != bucket.intervalNs ||
          existing.windowMode != bucket.windowMode) {
        continue;
      }
      existing = bucket;
      replaced = true;
      break;
    }
    if (!replaced) base.buckets.push_back(bucket);
  }
  for (const trading_core::RateLimitActionConfig& action : overlay.actions) {
    bool replaced = false;
    for (trading_core::RateLimitActionConfig& existing : base.actions) {
      if (existing.action != action.action) continue;
      existing = action;
      replaced = true;
      break;
    }
    if (!replaced) base.actions.push_back(action);
  }
}

}  // namespace

bool sameLoadPlan(const detail::SessionLoadPlan& lhs, const detail::SessionLoadPlan& rhs) noexcept {
  return lhs.loadTrades == rhs.loadTrades &&
         lhs.loadBookTickers == rhs.loadBookTickers &&
         lhs.loadPriceLimits == rhs.loadPriceLimits &&
         lhs.loadDepths == rhs.loadDepths &&
         lhs.loadCandles == rhs.loadCandles &&
         lhs.loadLiquidations == rhs.loadLiquidations &&
         lhs.loadMarkPrices == rhs.loadMarkPrices &&
         lhs.loadIndexPrices == rhs.loadIndexPrices &&
         lhs.loadFunding == rhs.loadFunding &&
         lhs.loadOpenInterest == rhs.loadOpenInterest &&
         lhs.requireTrades == rhs.requireTrades &&
         lhs.requireBookTickers == rhs.requireBookTickers &&
         lhs.requirePriceLimits == rhs.requirePriceLimits &&
         lhs.requireDepths == rhs.requireDepths &&
         lhs.requireCandles == rhs.requireCandles &&
         lhs.requireLiquidations == rhs.requireLiquidations &&
         lhs.requireMarkPrices == rhs.requireMarkPrices &&
         lhs.requireIndexPrices == rhs.requireIndexPrices &&
         lhs.requireFunding == rhs.requireFunding &&
         lhs.requireOpenInterest == rhs.requireOpenInterest;
}

std::vector<std::filesystem::path> sessionPathsFor(const BacktestRunRequest& request,
                                                   bool hasExplicitSessions) {
  std::vector<std::filesystem::path> out{};
  if (hasExplicitSessions) {
    out.reserve(request.sessions.size() + (request.sessionPath.empty() ? 0u : 1u));
    if (!request.sessionPath.empty()) out.push_back(request.sessionPath);
    for (const BacktestSessionRequest& session : request.sessions) out.push_back(session.path);
  } else {
    out.push_back(request.sessionPath);
  }
  return out;
}

std::size_t explicitSessionPathCount(const BacktestRunRequest& request, bool hasExplicitSessions) {
  return sessionPathsFor(request, hasExplicitSessions).size();
}

bool preparedMatches(const detail::PreparedBacktestInput& prepared,
                     const BacktestRunRequest& request,
                     bool hasExplicitSessions,
                     const detail::SessionLoadPlan& loadPlan) {
  if (!prepared.valid || prepared.hasExplicitSessions != hasExplicitSessions || !sameLoadPlan(prepared.loadPlan, loadPlan)) return false;
  return prepared.sessionPaths == sessionPathsFor(request, hasExplicitSessions);
}

void storePreparedSessions(detail::PreparedBacktestInput& prepared,
                           const BacktestRunRequest& request,
                           bool hasExplicitSessions,
                           const detail::SessionLoadPlan& loadPlan,
                           std::vector<detail::SessionData>& sessions) {
  prepared.valid = true;
  prepared.timelineValid = false;
  prepared.hasExplicitSessions = hasExplicitSessions;
  prepared.loadPlan = loadPlan;
  prepared.sessionPaths = sessionPathsFor(request, hasExplicitSessions);
  prepared.sessions = std::move(sessions);
  prepared.timeline.clear();
  prepared.depthCaches.clear();
  prepared.depthCaches.reserve(prepared.sessions.size());
  for (std::size_t i = 0u; i < prepared.sessions.size(); ++i) {
    prepared.depthCaches.push_back(detail::makeExecutionDepthCheckpointCache());
  }
}

void appendSessionWarnings(BacktestRunResult& result, const std::vector<detail::SessionData>& sessions) {
  for (std::size_t i = 0u; i < sessions.size(); ++i) {
    const detail::SessionData& session = sessions[i];
    for (const std::string& warning : session.warnings) {
      if (sessions.size() <= 1u) {
        result.warnings.push_back(warning);
      } else {
        result.warnings.push_back("leg " + std::to_string(i) + ": " + warning);
      }
    }
  }
}

std::string makeRunId() {
  const auto nowNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  return "run-" + std::to_string(nowNs);
}

std::filesystem::path resultPathFor(const BacktestRunRequest& request, const std::string& runId) {
  if (!request.outputPath.empty()) return request.outputPath;
  if (!request.corpusPath.empty())
    return request.corpusPath / "backtests" / runId;
  if (!request.sessionPath.empty()) return request.sessionPath / "backtests" / runId;
  if (!request.sessions.empty() && !request.sessions[0].path.empty()) return request.sessions[0].path / "backtests" / runId;
  return request.sessionPath / "backtests" / runId;
}

bool buildSessionLoadPlan(const trading_core::StrategyDescriptor& descriptor,
                          detail::SessionLoadPlan& out,
                          std::string& error) noexcept {
  out = detail::SessionLoadPlan{};
  out.loadTrades = false;
  out.loadBookTickers = false;
  out.loadPriceLimits = false;
  out.loadDepths = false;
  out.loadCandles = descriptor.seedHistory != nullptr || descriptor.loadCandleHistory;
  out.requireCandles = descriptor.seedHistory != nullptr || descriptor.requireCandleHistory;
  if (descriptor.resolveTradeHistory && descriptor.seedTrades) out.loadTrades = true;
  const std::size_t streamCount = trading_core::strategy::descriptorStreamCount(descriptor);
  for (std::size_t i = 0u; i < streamCount; ++i) {
    const cxet::api::market::PublicMarketDataStream stream = trading_core::strategy::descriptorStreamAt(descriptor, i);
    const bool required = trading_core::strategy::descriptorStreamRequired(descriptor, i);
    if (stream == cxet::api::market::PublicMarketDataStream::Trades) {
      out.loadTrades = true;
      out.requireTrades = out.requireTrades || required;
    } else if (stream == cxet::api::market::PublicMarketDataStream::BookTicker) {
      out.loadBookTickers = true;
      out.requireBookTickers = out.requireBookTickers || required;
    } else if (stream == cxet::api::market::PublicMarketDataStream::PriceLimit) {
      out.loadPriceLimits = true;
      out.requirePriceLimits = out.requirePriceLimits || required;
    } else if (stream == cxet::api::market::PublicMarketDataStream::Orderbook) {
      out.loadDepths = true;
      out.requireDepths = out.requireDepths || required;
    } else if (stream == cxet::api::market::PublicMarketDataStream::Liquidations) {
      out.loadLiquidations = true;
      out.requireLiquidations = out.requireLiquidations || required;
    } else if (stream == cxet::api::market::PublicMarketDataStream::MarkPrice) {
      out.loadMarkPrices = true;
      out.requireMarkPrices = out.requireMarkPrices || required;
    } else if (stream == cxet::api::market::PublicMarketDataStream::IndexPrice) {
      out.loadIndexPrices = true;
      out.requireIndexPrices = out.requireIndexPrices || required;
    } else if (stream == cxet::api::market::PublicMarketDataStream::Funding) {
      out.loadFunding = true;
      out.requireFunding = out.requireFunding || required;
    } else if (stream == cxet::api::market::PublicMarketDataStream::OpenInterest) {
      out.loadOpenInterest = true;
      out.requireOpenInterest = out.requireOpenInterest || required;
    } else {
      error = "strategy requires unsupported market data stream for backtest";
      return false;
    }
  }
  return true;
}

std::uint64_t hashRunSeed(const std::string& text) noexcept {
  std::uint64_t out = 1469598103934665603ull;
  for (char c : text) {
    out ^= static_cast<unsigned char>(c);
    out *= 1099511628211ull;
  }
  return out == 0u ? 88172645463325252ull : out;
}

BacktestFeeSchedule resolveFeeSchedule(const BacktestRunRequest& request,
                                       const std::string& exchangeText,
                                       const std::string& marketText) noexcept {
  const ExchangeId exchange = detail::exchangeFromText(exchangeText);
  const canon::MarketType market = detail::marketFromText(marketText);
  for (const BacktestFeeSchedule& schedule : request.feeSchedules) {
    if (detail::exchangeFromText(schedule.exchange).raw != exchange.raw) continue;
    if (detail::marketFromText(schedule.market).raw != market.raw) continue;
    return schedule;
  }
  BacktestFeeSchedule out{};
  out.exchange = exchangeText;
  out.market = marketText;
  const trading_core::ExchangeFeePreset preset = trading_core::defaultExchangeFeePreset(exchange, market);
  if (preset.available) {
    out.makerFeeBpsE8 = preset.makerFeeBpsE8;
    out.takerFeeBpsE8 = preset.takerFeeBpsE8;
  }
  if (request.hasMakerFeeBps) out.makerFeeBpsE8 = request.makerFeeBpsE8;
  if (request.hasTakerFeeBps) out.takerFeeBpsE8 = request.takerFeeBpsE8;
  return out;
}

BacktestRateLimitSchedule resolveRateLimitSchedule(const BacktestRunRequest& request,
                                                   const std::string& exchangeText,
                                                   const std::string& marketText) noexcept {
  const ExchangeId exchange = detail::exchangeFromText(exchangeText);
  const canon::MarketType market = detail::marketFromText(marketText);
  if (!request.rateLimitsEnabled) {
    BacktestRateLimitSchedule disabled{};
    disabled.exchange = exchangeText;
    disabled.market = marketText;
    return disabled;
  }
  BacktestRateLimitSchedule out = scheduleFromPreset(exchange, market, exchangeText, marketText);
  for (const BacktestRateLimitSchedule& schedule : request.rateLimitSchedules) {
    if (detail::exchangeFromText(schedule.exchange).raw != exchange.raw) continue;
    if (detail::marketFromText(schedule.market).raw != market.raw) continue;
    overlayRateLimitSchedule(out, schedule);
  }
  return out;
}

std::int64_t legInitialBalanceE8(const BacktestRunRequest& request, std::size_t legIndex) noexcept {
  if (request.legInitialBalancesE8.empty()) return 0;
  if (legIndex < request.legInitialBalancesE8.size()) return request.legInitialBalancesE8[legIndex];
  return request.initialBalanceE8;
}

std::int64_t portfolioInitialBalanceE8(const BacktestRunRequest& request, std::size_t legCount) noexcept {
  if (request.legInitialBalancesE8.empty()) return request.initialBalanceE8;
  std::int64_t out = 0;
  for (std::size_t i = 0u; i < legCount; ++i) {
    out = trading_core::saturatingAddI64(
        out, legInitialBalanceE8(request, i));
  }
  return out;
}

detail::BacktestLatencyConfig resolveLatencyConfig(const BacktestRunRequest& request,
                                                   const std::string& exchangeText,
                                                   const std::string& marketText) noexcept {
  detail::BacktestLatencyConfig fallback = detail::latencyConfigFromRequest(request);
  const ExchangeId exchange = detail::exchangeFromText(exchangeText);
  const canon::MarketType market = detail::marketFromText(marketText);
  for (const BacktestLatencySchedule& schedule : request.latencySchedules) {
    if (detail::exchangeFromText(schedule.exchange).raw != exchange.raw) continue;
    if (detail::marketFromText(schedule.market).raw != market.raw) continue;
    fallback.marketOrder = schedule.marketOrder;
    fallback.limitOrder = schedule.limitOrder;
    fallback.cancelOrder = schedule.cancelOrder.baseUs != 0u || schedule.cancelOrder.jitterUs != 0u ? schedule.cancelOrder : fallback.cancelOrder;
    if (schedule.userData.baseUs != 0u || schedule.userData.jitterUs != 0u) fallback.userData = schedule.userData;
    return fallback;
  }
  return fallback;
}

detail::BacktestLatencyConfig latencyConfigForLeg(const BacktestRunRequest& request,
                                                  const std::string& exchangeText,
                                                  const std::string& marketText,
                                                  std::size_t legIndex) noexcept {
  detail::BacktestLatencyConfig out = resolveLatencyConfig(request, exchangeText, marketText);
  out.seed ^= hashRunSeed(exchangeText);
  out.seed ^= (hashRunSeed(marketText) << 1u);
  out.seed ^= (static_cast<std::uint64_t>(legIndex + 1u) * 0x9e3779b97f4a7c15ull);
  return out;
}

std::uint64_t maxLatencyUs(const BacktestRunRequest& request) noexcept {
  const detail::BacktestLatencyConfig config = detail::latencyConfigFromRequest(request);
  std::uint64_t out = detail::latencyProfileMaxUs(config.marketOrder);
  const std::uint64_t marketOrder = detail::latencyProfileMaxUs(config.marketOrder);
  const std::uint64_t limitOrder = detail::latencyProfileMaxUs(config.limitOrder);
  const std::uint64_t cancelOrder = detail::latencyProfileMaxUs(config.cancelOrder);
  const std::uint64_t userData = detail::latencyProfileMaxUs(config.userData);
  if (marketOrder > out) out = marketOrder;
  if (limitOrder > out) out = limitOrder;
  if (cancelOrder > out) out = cancelOrder;
  if (userData > out) out = userData;
  for (const BacktestLatencySchedule& schedule : request.latencySchedules) {
    const std::uint64_t mo = detail::latencyProfileMaxUs(schedule.marketOrder);
    const std::uint64_t lo = detail::latencyProfileMaxUs(schedule.limitOrder);
    const std::uint64_t co = detail::latencyProfileMaxUs(schedule.cancelOrder);
    const std::uint64_t ud = detail::latencyProfileMaxUs(schedule.userData);
    if (mo > out) out = mo;
    if (lo > out) out = lo;
    if (co > out) out = co;
    if (ud > out) out = ud;
  }
  return out;
}

}  // namespace hft_backtest
