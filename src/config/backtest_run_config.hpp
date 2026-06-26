#pragma once

#include "hft_backtest/backtest.hpp"

#include "execution/backtest_latency.hpp"
#include "engine/backtest_prepared.hpp"
#include "input/session_loader.hpp"

#include "hft_trader/runtime/runner/StrategyDescriptor.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace hft_backtest {

[[nodiscard]] bool sameLoadPlan(const detail::SessionLoadPlan& lhs,
                                const detail::SessionLoadPlan& rhs) noexcept;
[[nodiscard]] std::vector<std::filesystem::path> sessionPathsFor(const BacktestRunRequest& request,
                                                                 bool hasExplicitSessions);
[[nodiscard]] std::size_t explicitSessionPathCount(const BacktestRunRequest& request,
                                                   bool hasExplicitSessions);
[[nodiscard]] bool preparedMatches(const detail::PreparedBacktestInput& prepared,
                                   const BacktestRunRequest& request,
                                   bool hasExplicitSessions,
                                   const detail::SessionLoadPlan& loadPlan);
void storePreparedSessions(detail::PreparedBacktestInput& prepared,
                           const BacktestRunRequest& request,
                           bool hasExplicitSessions,
                           const detail::SessionLoadPlan& loadPlan,
                           std::vector<detail::SessionData>& sessions);
void appendSessionWarnings(BacktestRunResult& result, const std::vector<detail::SessionData>& sessions);

[[nodiscard]] bool marketDataLatencyKeepsOrder(const detail::BacktestLatencyConfig& config) noexcept;
[[nodiscard]] bool hasMarketDataLatency(const BacktestRunRequest& request,
                                        const detail::BacktestLatencyConfig& fallback) noexcept;
[[nodiscard]] bool marketDataLatencyKeepsOrder(const BacktestRunRequest& request,
                                               const detail::BacktestLatencyConfig& fallback) noexcept;

[[nodiscard]] std::string makeRunId();
[[nodiscard]] std::filesystem::path resultPathFor(const BacktestRunRequest& request,
                                                  const std::string& runId);
[[nodiscard]] bool buildSessionLoadPlan(const hft_trader::runtime::StrategyDescriptor& descriptor,
                                        detail::SessionLoadPlan& out,
                                        std::string& error) noexcept;
[[nodiscard]] std::uint64_t hashRunSeed(const std::string& text) noexcept;
[[nodiscard]] BacktestFeeSchedule resolveFeeSchedule(const BacktestRunRequest& request,
                                                     const std::string& exchangeText,
                                                     const std::string& marketText) noexcept;
[[nodiscard]] BacktestRateLimitSchedule resolveRateLimitSchedule(const BacktestRunRequest& request,
                                                                 const std::string& exchangeText,
                                                                 const std::string& marketText) noexcept;
[[nodiscard]] std::int64_t legInitialBalanceE8(const BacktestRunRequest& request,
                                               std::size_t legIndex) noexcept;
[[nodiscard]] std::int64_t portfolioInitialBalanceE8(const BacktestRunRequest& request,
                                                     std::size_t legCount) noexcept;
[[nodiscard]] detail::BacktestLatencyConfig resolveLatencyConfig(const BacktestRunRequest& request,
                                                                 const std::string& exchangeText,
                                                                 const std::string& marketText) noexcept;
[[nodiscard]] detail::BacktestLatencyConfig latencyConfigForLeg(const BacktestRunRequest& request,
                                                                const std::string& exchangeText,
                                                                const std::string& marketText,
                                                                std::size_t legIndex) noexcept;
[[nodiscard]] std::uint64_t maxLatencyUs(const BacktestRunRequest& request) noexcept;

}  // namespace hft_backtest
