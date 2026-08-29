#include "config/backtest_strategy_config.hpp"
#include "config/backtest_run_config.hpp"

#include "trading_core/api/params/StrategyParams.hpp"
#include "primitives/buf/StringView.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <system_error>

namespace {

using namespace hft_backtest;

[[nodiscard]] bool expect(bool value, const char* message) {
  if (!value) std::cerr << "FAIL: " << message << '\n';
  return value;
}

[[nodiscard]] bool symbolEquals(const Symbol& symbol,
                                cxet::StringView expected) noexcept {
  return cxet::StringView::fromCString(symbol.data) == expected;
}

[[nodiscard]] bool loadConfigText(const char* name,
                                  const char* text,
                                  BacktestStrategyConfig& config,
                                  std::string& error) {
  const std::filesystem::path path =
      std::filesystem::temp_directory_path() / name;
  {
    std::ofstream output(path, std::ios::out | std::ios::trunc);
    if (!output) return false;
    output << text;
  }
  const std::string pathText = path.string();
  const bool loaded =
      loadBacktestStrategyConfig(pathText.c_str(), config, error);
  std::error_code ignored{};
  (void)std::filesystem::remove(path, ignored);
  return loaded;
}

[[nodiscard]] bool testConsumedStrategyConfigIsPreserved() {
  static constexpr const char* kConfig = R"ini(
[runtime]
hot_cpu_id=not-a-backtest-setting
trading_cost_profile=not-loaded-by-backtest.json

[strategy]
type=uptick
enabled=false
max_quote_age_ms=5000

[risk]
enabled=true
min_equity_pct=80
min_leg_equity_pct=70
min_leg_equity_usdt=5
rate_limit_guard_enabled=true
rate_limit_guard_min_remaining=2

[venue.binance_futures]
api_slot=1
symbols=BTC_USDT,ETH_USDT
live_probe=required
rate_limits_enabled=true
rate_limit_bucket_orders_6_limit=7
rate_limit_bucket_orders_6_interval_ms=2000
rate_limit_bucket_orders_6_scope=instrument
rate_limit_bucket_orders_6_window_mode=sliding
rate_limit_action_limit_order_orders=2
risk_min_equity_pct=60
risk_min_equity_usdt=3
amount_qty=10
api_key_env=IGNORED_BY_BACKTEST

[portfolio.cross]
legs=binance_futures:BTC_USDT,binance_futures:ETH_USDT
primary_leg_index=1
trade_mode=primary
)ini";

  BacktestStrategyConfig config{};
  std::string error{};
  if (!expect(loadConfigText(
                  "hft_backtest_strategy_config_valid.ini", kConfig,
                  config, error),
              "valid consumed strategy config must load")) {
    if (!error.empty()) std::cerr << error << '\n';
    return false;
  }
  if (!expect(config.strategyType == "uptick",
              "strategy type must be preserved") ||
      !expect(!config.strategyDefaults.enabled,
              "strategy enabled must be preserved") ||
      !expect(config.risk.enabled && config.risk.hasMinEquityPct &&
                  config.risk.minEquityPctE8 == 8000000000ll,
              "portfolio risk minimum must preserve E8 scaling") ||
      !expect(config.risk.rateLimitGuardEnabled &&
                  config.risk.rateLimitGuardMinRemaining == 2,
              "rate-limit guard fields must be preserved") ||
      !expect(config.venues.size() == 1u,
              "one venue section must remain one venue") ||
      !expect(config.portfolios.size() == 1u,
              "one portfolio section must remain one portfolio")) {
    return false;
  }

  const BacktestStrategyVenueConfig& venue = config.venues[0];
  const std::string* maxQuoteAge = trading_core::findStrategyParam(
      config.strategyDefaults, "max_quote_age_ms");
  const std::string* amountQty =
      trading_core::findStrategyParam(venue.params, "amount_qty");
  if (!expect(venue.exchange.raw == canon::kExchangeIdBinance.raw &&
                  venue.market.raw == canon::kMarketTypeFutures.raw,
              "venue section must resolve canonical exchange and market") ||
      !expect(venue.liveProbe == BacktestLiveProbeMode::Required,
              "venue live-probe summary input must be preserved") ||
      !expect(venue.symbols.size() == 2u &&
                  symbolEquals(venue.symbols[0], "BTC_USDT") &&
                  symbolEquals(venue.symbols[1], "ETH_USDT"),
              "venue symbols must preserve configured order") ||
      !expect(maxQuoteAge && *maxQuoteAge == "5000" &&
                  amountQty && *amountQty == "10",
              "strategy default and venue params must remain distinct") ||
      !expect(venue.rateLimitsEnabled &&
                  venue.rateLimitBuckets.size() == 1u &&
                  venue.rateLimitActions.size() == 1u,
              "consumed venue rate-limit policy must be retained") ||
      !expect(venue.hasRiskMinEquityPct && venue.hasRiskMinEquity &&
                  venue.riskMinEquityPctE8 == 6000000000ll &&
                  venue.riskMinEquityE8 == 300000000ll,
              "venue risk minimum presence and values must be retained")) {
    return false;
  }

  const BacktestPortfolioConfig& portfolio = config.portfolios[0];
  const trading_core::StrategyParamContext params =
      makeBacktestStrategyParamContext(config, venue);
  return expect(portfolio.legs.size() == 2u &&
                    symbolEquals(portfolio.legs[0].symbol, "BTC_USDT") &&
                    symbolEquals(portfolio.legs[1].symbol, "ETH_USDT"),
                "portfolio legs must preserve configured order") &&
      expect(portfolio.legs[0].routeHandle.raw == 1u &&
                 portfolio.legs[1].routeHandle.raw == 2u,
             "portfolio route handles must follow leg order") &&
      expect(portfolio.primaryLegIndex == 1u &&
                 portfolio.tradeMode == BacktestPortfolioTradeMode::Primary,
             "portfolio primary index and trade mode must be preserved") &&
      expect(params.rateLimitGuardEnabled &&
                 params.rateLimitGuardMinRemaining == 2,
             "canonical strategy param context must receive rate-limit guard");
}

[[nodiscard]] bool testMalformedConsumedValueFailsClosed() {
  static constexpr const char* kConfig = R"ini(
[strategy]
type=uptick
enabled=maybe
[venue.binance_futures]
symbols=BTC_USDT
)ini";
  BacktestStrategyConfig config{};
  std::string error{};
  return expect(!loadConfigText(
                    "hft_backtest_strategy_config_bad_bool.ini", kConfig,
                    config, error),
                "malformed consumed boolean must fail closed");
}

[[nodiscard]] bool testInvalidPortfolioReferenceFailsClosed() {
  static constexpr const char* kConfig = R"ini(
[strategy]
type=uptick
[venue.binance_futures]
symbols=BTC_USDT
[portfolio.bad]
legs=binance_futures:ETH_USDT
primary_leg_index=0
trade_mode=all
)ini";
  BacktestStrategyConfig config{};
  std::string error{};
  return expect(!loadConfigText(
                    "hft_backtest_strategy_config_bad_portfolio.ini", kConfig,
                    config, error),
                "portfolio symbol outside its venue must fail closed");
}

[[nodiscard]] bool testDescriptorOwnedInputsOverrideFailsClosed() {
  static constexpr const char* kConfig = R"ini(
[strategy]
type=uptick
inputs=bookticker
[venue.binance_futures]
symbols=BTC_USDT
)ini";
  BacktestStrategyConfig config{};
  std::string error{};
  return expect(!loadConfigText(
                    "hft_backtest_strategy_config_inputs.ini", kConfig,
                    config, error),
                "strategy input override must remain descriptor-owned");
}

[[nodiscard]] bool testReplacePolicyKnobFailsClosed() {
  static constexpr const char* kConfig = R"ini(
[strategy]
type=uptick
[venue.binance_futures]
symbols=BTC_USDT
replace_policy=optimistic
)ini";
  BacktestStrategyConfig config{};
  std::string error{};
  return expect(!loadConfigText(
                    "hft_backtest_strategy_config_replace_policy.ini",
                    kConfig, config, error),
                "configurable replace policy must fail closed") &&
      expect(error.find("cancel -> terminal cancel outcome -> new") !=
                 std::string::npos,
             "replace-policy rejection must state the fixed lifecycle");
}

[[nodiscard]] bool testPortfolioInitialBalanceSaturates() {
  constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
  constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
  BacktestRunRequest request{};
  request.legInitialBalancesE8 = {kMax, 1};
  const std::int64_t positive = portfolioInitialBalanceE8(request, 2u);
  request.legInitialBalancesE8 = {kMin, -1};
  const std::int64_t negative = portfolioInitialBalanceE8(request, 2u);
  return expect(positive == kMax,
                "portfolio initial balance saturates at INT64_MAX") &&
         expect(negative == kMin,
                "portfolio initial balance saturates at INT64_MIN");
}

}  // namespace

int main() {
  bool ok = true;
  ok = testConsumedStrategyConfigIsPreserved() && ok;
  ok = testMalformedConsumedValueFailsClosed() && ok;
  ok = testInvalidPortfolioReferenceFailsClosed() && ok;
  ok = testDescriptorOwnedInputsOverrideFailsClosed() && ok;
  ok = testReplacePolicyKnobFailsClosed() && ok;
  ok = testPortfolioInitialBalanceSaturates() && ok;
  return ok ? 0 : 1;
}
