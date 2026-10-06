#pragma once

#include "trading_core/Strategy/Params/StrategyParams.hpp"
#include "trading_core/Algorithm/Primitives/AlgorithmPrimitives.hpp"
#include "hft_backtest/Backtest.hpp"
#include "trading_core/Strategy/Risk/PortfolioRisk.hpp"

#include "cxet/Canon/PositionAndExchange.hpp"
#include "cxet/Canon/Subtypes.hpp"
#include "cxet/Primitives/Buf/Symbol.hpp"
#include "cxet/Primitives/Raw/Counts/ExchangeId.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace hft_backtest {

enum class BacktestLiveProbeMode : std::uint8_t {
  Default = 0,
  Required = 1,
  Disabled = 2,
};

enum class BacktestPortfolioTradeMode : std::uint8_t {
  All = 0,
  Primary = 1,
};

struct BacktestStrategyVenueConfig {
  std::string name{};
  std::int64_t initialBalanceE8{};
  bool hasInitialBalance{false};
  ExchangeId exchange{canon::kExchangeIdUnknown};
  canon::MarketType market{canon::kMarketTypeUnknown};
  bool rateLimitsEnabled{false};
  bool hasRateLimitsEnabled{false};
  std::int64_t riskMinEquityPctE8{0};
  std::int64_t riskMinEquityE8{0};
  bool hasRiskMinEquityPct{false};
  bool hasRiskMinEquity{false};
  std::vector<Symbol> symbols{};
  std::vector<BacktestQuotaBucketConfig> rateLimitBuckets{};
  std::vector<trading_core::RateLimitActionConfig> rateLimitActions{};
  BacktestLiveProbeMode liveProbe{BacktestLiveProbeMode::Default};
  trading_core::StrategyRuntimeParams params{};
};

struct BacktestPortfolioLegConfig {
  std::string venue{};
  Symbol symbol{};
  trading_core::ExecutionRouteHandle routeHandle{};
};

struct BacktestPortfolioConfig {
  std::string name{};
  std::vector<BacktestPortfolioLegConfig> legs{};
  std::uint32_t primaryLegIndex{0u};
  BacktestPortfolioTradeMode tradeMode{BacktestPortfolioTradeMode::All};
};

struct BacktestRiskConfig {
  bool enabled{false};
  std::int64_t minEquityPctE8{10000000000ll};
  std::int64_t minLegEquityPctE8{0};
  std::int64_t minLegEquityE8{0};
  std::int64_t rateLimitGuardMinRemaining{0};
  bool rateLimitGuardEnabled{false};
  bool hasMinEquityPct{false};
  bool hasMinLegEquityPct{false};
  bool hasMinLegEquity{false};
};

struct BacktestStrategyConfig {
  std::string strategyType{};
  trading_core::StrategyRuntimeParams strategyDefaults{};
  BacktestRiskConfig risk{};
  std::vector<BacktestStrategyVenueConfig> venues{};
  std::vector<BacktestPortfolioConfig> portfolios{};
};

[[nodiscard]] bool loadBacktestStrategyConfig(
    const char* path,
    BacktestStrategyConfig& config,
    std::string& error);

[[nodiscard]] inline trading_core::StrategyParamContext
makeBacktestStrategyParamContext(
    const BacktestStrategyConfig& config,
    const BacktestStrategyVenueConfig& venue) noexcept {
  return trading_core::StrategyParamContext{
      config.strategyDefaults,
      venue.params,
      venue.name,
      config.risk.rateLimitGuardEnabled && venue.rateLimitsEnabled,
      config.risk.rateLimitGuardMinRemaining};
}

[[nodiscard]] inline trading_core::PortfolioRiskConfig
makeBacktestPortfolioRiskConfig(const BacktestRiskConfig& config) noexcept {
  trading_core::PortfolioRiskConfig out{};
  out.enabled = config.enabled;
  out.minEquityPctE8 = config.minEquityPctE8;
  out.minLegEquityPctE8 = config.minLegEquityPctE8;
  out.minLegEquityE8 = config.minLegEquityE8;
  out.hasMinEquityPct = config.hasMinEquityPct;
  out.hasMinLegEquityPct = config.hasMinLegEquityPct;
  out.hasMinLegEquity = config.hasMinLegEquity;
  return out;
}

[[nodiscard]] inline trading_core::PortfolioRiskLegConfig
makeBacktestPortfolioRiskLegConfig(
    const BacktestStrategyVenueConfig* venue) noexcept {
  trading_core::PortfolioRiskLegConfig out{};
  if (!venue) return out;
  out.minEquityPctE8 = venue->riskMinEquityPctE8;
  out.minEquityE8 = venue->riskMinEquityE8;
  out.hasMinEquityPct = venue->hasRiskMinEquityPct;
  out.hasMinEquity = venue->hasRiskMinEquity;
  return out;
}

}  // namespace hft_backtest
