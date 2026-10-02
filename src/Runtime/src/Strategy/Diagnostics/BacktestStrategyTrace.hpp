#pragma once

#include "hft_backtest/Backtest.hpp"

#include "../../Engine/BacktestEngine.hpp"

#include "trading_core/Contracts/Strategy/StrategyContract.hpp"
#include "trading_core/Strategy/Descriptor/StrategyDescriptor.hpp"

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
[[nodiscard]] bool emitsArbSpreadTrace(const trading_core::StrategyDescriptor& descriptor) noexcept;
[[nodiscard]] bool emitsPriceRangeTrace(const trading_core::StrategyDescriptor& descriptor) noexcept;
void reserveEngineStorage(detail::EngineState& engine,
                          std::size_t events,
                          std::uint64_t replayDurationNs,
                          bool captureIndicatorTrace,
                          bool captureRangeTrace,
                          bool captureSpreadRows);
void appendStrategyIndicator(detail::EngineState& engine,
                             std::int64_t tsNs,
                             const trading_core::StrategyDecisionTrace& trace);
void appendStrategyRange(detail::EngineState& engine,
                         std::int64_t tsNs,
                         const trading_core::StrategyDecisionTrace& trace);
void appendStrategySpread(detail::EngineState& engine,
                          std::int64_t tsNs,
                          const trading_core::StrategyDecisionTrace& trace);
void recordStrategySpread(detail::EngineState& engine,
                          std::int64_t tsNs,
                          const trading_core::StrategyDecisionTrace& trace,
                          bool captureRows);
void recordInputCoverage(detail::EngineState& engine) noexcept;
void recordExecutionQualityActions(
    detail::EngineState& engine,
    std::int64_t tsNs,
    const trading_core::StrategyExecutionTraceBatch& batch);
void recordExecutionQualityFill(
    detail::EngineState& ledger,
    const cxet::composite::UserDataEvent& event,
    std::uint32_t legIndex,
    std::int64_t deliveryTsNs,
    const trading_core::StrategyInstrumentSpec& instrument) noexcept;
void finalizeExecutionQuality(detail::EngineState& engine) noexcept;

void calculateExecutionQualityDeadlines(
    detail::EngineState& engine,
    const detail::SessionData* sessions,
    const trading_core::StrategyInstrumentSpec* instruments,
    std::size_t legCount) noexcept;

}  // namespace hft_backtest
