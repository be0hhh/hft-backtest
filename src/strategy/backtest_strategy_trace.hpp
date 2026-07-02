#pragma once

#include "hft_backtest/backtest.hpp"

#include "engine/backtest_engine.hpp"

#include "hft_trader/core/strategy/StrategyContract.hpp"
#include "hft_trader/runtime/runner/StrategyDescriptor.hpp"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace hft_backtest {

struct StrategyIndicatorProfileInfo {
  std::string_view id{};
  std::string_view title{};
  std::string_view valueLabel{};
  std::string_view auxLabel{};
  std::string_view unit{};
};

[[nodiscard]] bool resolveStrategyIndicatorProfile(std::string_view id,
                                                   StrategyIndicatorProfileInfo& out) noexcept;
[[nodiscard]] bool strategyDeclaresIndicator(std::string_view strategy,
                                             std::string_view profile) noexcept;
[[nodiscard]] bool strategyIndicatorEnabled(const detail::EngineState& engine) noexcept;
void configureStrategyIndicator(detail::EngineState& engine,
                                const StrategyIndicatorProfileInfo& profile);
[[nodiscard]] bool emitsArbSpreadTrace(const hft_trader::runtime::StrategyDescriptor& descriptor) noexcept;
[[nodiscard]] bool emitsPriceRangeTrace(const hft_trader::runtime::StrategyDescriptor& descriptor) noexcept;
void reserveEngineStorage(detail::EngineState& engine,
                          std::size_t events,
                          bool captureIndicatorTrace,
                          bool captureRangeTrace,
                          bool captureSpreadTrace);
void appendStrategyIndicator(detail::EngineState& engine,
                             std::int64_t tsNs,
                             const hft_trader::core::StrategyDecisionTrace& trace);
void appendStrategyRange(detail::EngineState& engine,
                         std::int64_t tsNs,
                         const hft_trader::core::StrategyDecisionTrace& trace);
void appendStrategySpread(detail::EngineState& engine,
                          std::int64_t tsNs,
                          const hft_trader::core::StrategyDecisionTrace& trace);
void recordInputCoverage(detail::EngineState& engine) noexcept;

}  // namespace hft_backtest
