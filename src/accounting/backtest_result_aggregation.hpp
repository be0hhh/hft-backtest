#pragma once

#include "hft_backtest/backtest.hpp"

#include "engine/backtest_engine.hpp"
#include "input/session_loader.hpp"

#include <cstdint>
#include <vector>

namespace hft_backtest {

BacktestInputCoverage addCoverage(BacktestInputCoverage lhs, const BacktestInputCoverage& rhs) noexcept;
BacktestLegSummary legSummaryFromEngine(const detail::SessionData& session,
                                        const detail::EngineState& engine) noexcept;
void populateAggregateArtifactEngine(const std::vector<detail::EngineState>& engines,
                                     std::int64_t initialBalanceE8,
                                     std::int64_t finalTsNs,
                                     detail::EngineState& out);
void populateResultPnl(BacktestRunResult& result, const detail::EngineState& engine) noexcept;
void populatePortfolioResultPnl(BacktestRunResult& result,
                                const std::vector<detail::SessionData>& sessions,
                                const std::vector<detail::EngineState>& engines,
                                std::int64_t initialBalanceE8) noexcept;
void appendRateLimitWarnings(BacktestRunResult& result,
                             const detail::EngineState& engine,
                             const std::vector<detail::EngineState>* legEngines = nullptr) noexcept;

}  // namespace hft_backtest
