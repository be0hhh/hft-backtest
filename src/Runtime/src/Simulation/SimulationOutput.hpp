#pragma once
#include "hft_backtest/SimulationSession.hpp"
#include "../Engine/BacktestEngine.hpp"
namespace hft_backtest::detail::simulation_output {
[[nodiscard]] bool finishOpen(const EngineState&,std::uint32_t sourceId,std::uint64_t eventMono,std::int64_t finalTs,SimulationOutputSink);
[[nodiscard]] bool compact(EngineState&,std::uint32_t sourceId,std::uint64_t eventMono,SimulationOutputSink);
}
