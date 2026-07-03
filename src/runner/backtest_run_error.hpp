#pragma once

#include "hft_trader/runtime/runner/StrategyDescriptor.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace hft_backtest {

[[nodiscard]] inline std::string formatStrategyRunCycleError(
    const hft_trader::runtime::StrategyDescriptor& descriptor,
    const void* state,
    std::string_view phase,
    std::int64_t tsNs,
    std::uint32_t eventLegIndex) {
  std::string error = "strategy runCycle failed on ";
  error += phase;
  error += " market event";
  error += ": strategy=";
  error += descriptor.name ? descriptor.name : "unknown";
  if (descriptor.captureState) {
    hft_trader::runtime::StrategyStateSnapshot snapshot{};
    descriptor.captureState(state, snapshot);
    error += " reject_reason=";
    error += hft_trader::runtime::strategyRejectReasonName(&descriptor, snapshot.rejectReasonRaw);
    error += " reject_reason_raw=";
    error += std::to_string(static_cast<unsigned>(snapshot.rejectReasonRaw));
  }
  error += " ts_ns=";
  error += std::to_string(tsNs);
  error += " event_leg_index=";
  error += std::to_string(eventLegIndex);
  return error;
}

}  // namespace hft_backtest
