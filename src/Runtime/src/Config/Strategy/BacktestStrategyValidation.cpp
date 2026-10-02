#include "Internal/BacktestStrategyConfigHelpers.hpp"

namespace hft_backtest::strategy_config {

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

}  // namespace hft_backtest::strategy_config
