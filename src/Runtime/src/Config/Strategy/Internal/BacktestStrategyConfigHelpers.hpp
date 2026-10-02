#pragma once

#include "../BacktestStrategyConfig.hpp"

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

namespace hft_backtest::strategy_config {

[[nodiscard]] std::string trim(cxet::StringView value);

[[nodiscard]] std::string lower(std::string value);

[[nodiscard]] std::vector<std::string> splitCsv(const std::string& value);

[[nodiscard]] bool parseUint(const std::string& value,
                             unsigned& out) noexcept;

[[nodiscard]] bool parseInt64(const std::string& value,
                              std::int64_t& out) noexcept;

[[nodiscard]] bool parseBoolStrict(const std::string& value,
                                   bool& out);

[[nodiscard]] bool parseDecimalE8(const std::string& value,
                                  std::int64_t& out) noexcept;

[[nodiscard]] bool cryptoExchangeRequiresLocalSymbols(
    ExchangeId exchange) noexcept;

[[nodiscard]] bool validLocalCryptoSymbol(const char* symbol) noexcept;

[[nodiscard]] bool venueFromSection(
    const std::string& section,
    BacktestStrategyVenueConfig& out);

[[nodiscard]] bool parseLiveProbeMode(const std::string& value,
                                      BacktestLiveProbeMode& out);

[[nodiscard]] bool parsePortfolioTradeMode(
    const std::string& value,
    BacktestPortfolioTradeMode& out);

[[nodiscard]] bool parseRateLimitBucketKind(
    const std::string& value,
    trading_core::RateLimitBucketKind& out);

[[nodiscard]] bool parseRateLimitActionKind(
    const std::string& value,
    trading_core::RateLimitActionKind& out);

[[nodiscard]] bool parseRateLimitScopeKind(
    const std::string& value,
    trading_core::RateLimitScopeKind& out);

[[nodiscard]] bool parseRateLimitWindowMode(
    const std::string& value,
    trading_core::RateLimitWindowMode& out);

[[nodiscard]] trading_core::RateLimitBucketConfig* upsertRateLimitBucket(
    BacktestStrategyVenueConfig& venue,
    trading_core::RateLimitBucketKind kind,
    std::uint8_t configSlot);

[[nodiscard]] trading_core::RateLimitActionConfig* upsertRateLimitAction(
    BacktestStrategyVenueConfig& venue,
    trading_core::RateLimitActionKind action);

[[nodiscard]] bool setRateLimitActionCost(
    trading_core::RateLimitActionConfig& action,
    trading_core::RateLimitBucketKind bucket,
    std::int64_t cost) noexcept;

[[nodiscard]] bool parseVenueRateLimitKey(
    BacktestStrategyVenueConfig& venue,
    const std::string& key,
    const std::string& value,
    std::string& error);

[[nodiscard]] bool resolveVenueRateLimitPolicy(
    BacktestStrategyVenueConfig& venue,
    std::string& error) noexcept;

[[nodiscard]] bool parsePortfolioLeg(const std::string& value,
                                     BacktestPortfolioLegConfig& out);

[[nodiscard]] bool ignoredVenueOnlyKey(const std::string& key) noexcept;

[[nodiscard]] const BacktestStrategyVenueConfig* findVenue(
    const BacktestStrategyConfig& config,
    const std::string& name) noexcept;

[[nodiscard]] bool venueHasSymbol(const BacktestStrategyVenueConfig& venue,
                                  const Symbol& symbol) noexcept;

[[nodiscard]] bool validateConfig(BacktestStrategyConfig& config,
                                  std::string& error);

}  // namespace hft_backtest::strategy_config
