#pragma once

#include "hft_backtest/Backtest.hpp"

#include "../BacktestEngine.hpp"

#include "trading_core/Strategy/Descriptor/StrategyDescriptor.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace hft_backtest::strategy_intent_lowering {

[[nodiscard]] std::uint32_t configuredPrimaryLegIndex(
    const BacktestRunRequest& request,
    std::size_t legCount) noexcept;

[[nodiscard]] trading_core::ExecutionRouteHandle configuredDefaultExecutionRoute(
    const BacktestRunRequest& request,
    std::size_t legCount) noexcept;

[[nodiscard]] std::size_t routeLegIndex(
    const trading_core::StrategyContext& context,
    trading_core::ExecutionRouteHandle route) noexcept;

[[nodiscard]] bool routeTradable(
    const BacktestRunRequest& request,
    std::size_t legCount,
    std::size_t leg) noexcept;

[[nodiscard]] bool footprintSupported(
    const trading_core::StrategyDescriptor& descriptor) noexcept;

[[nodiscard]] bool validateOutput(
    trading_core::StrategyContext& context) noexcept;

[[nodiscard]] bool bindTransactionLegs(
    trading_core::StrategyContext& context) noexcept;

struct StrategyTransactionAdmission final {
  std::uint64_t localFaultMask{0u};
  std::uint64_t deferredMask{0u};

  [[nodiscard]] constexpr std::uint64_t blockedMask() const noexcept {
    return localFaultMask | deferredMask;
  }
};

[[nodiscard]] StrategyTransactionAdmission transactionAdmission(
    const trading_core::StrategyContext& context,
    std::size_t legCount,
    const BacktestRunRequest& request,
    bool newOrderEmissionBlocked) noexcept;

void applyToSingleLeg(
    detail::EngineState& engine,
    const trading_core::StrategyContext& context,
    std::int64_t tsNs,
    detail::BacktestLatencyRuntime& latency,
    const BacktestRunRequest& request);

void applyToLegs(
    Span<detail::EngineState> engines,
    const trading_core::StrategyContext& context,
    std::int64_t tsNs,
    const BacktestRunRequest& request);

[[nodiscard]] bool applyTransactionCycle(
    const trading_core::StrategyDescriptor& descriptor,
    void* state,
    const trading_core::StrategyContext& context,
    Span<detail::EngineState> engines,
    std::int64_t tsNs,
    StrategyTransactionAdmission admission);

}  // namespace hft_backtest::strategy_intent_lowering
