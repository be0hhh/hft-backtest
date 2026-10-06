#include "Internal/BacktestStrategyConfigHelpers.hpp"
#include "cxet/Parse/DecimalToScaled.hpp"
#include "BacktestStrategyConfig.hpp"


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
namespace strategy_config {

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























[[nodiscard]] bool ignoredVenueOnlyKey(const std::string& key) noexcept {
  return key == "api_slot" || key == "market_enabled" ||
      key == "user_enabled" || key == "order_enabled" ||
      key == "control_enabled" ||
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







}  // namespace strategy_config

using namespace strategy_config;

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
      } else if (key == "initial_balance_usdt") {
        if (!cxet::parse::parseUnsignedScaledDecimalStrict(
              cxet::StringView{value.data(), value.size()}, 8u, &currentVenue.initialBalanceE8)) {
          error = "invalid exact venue initial_balance_usdt"; return false;
        }
        currentVenue.hasInitialBalance = true;
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
