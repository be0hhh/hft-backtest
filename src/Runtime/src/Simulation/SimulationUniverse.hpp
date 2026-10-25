#pragma once
#include "SimulationReferences.hpp"
#include "../Run/BacktestUniverse.hpp"
namespace hft_backtest::detail {
[[nodiscard]] Status discoverSimulationUniverse(const trading_core::StrategyDescriptor&,
    const SimulationInput&,std::span<const BacktestCanonicalSource>,
    std::size_t maximumContexts,std::vector<run_detail::UniverseContextSelection>&,std::string&);
[[nodiscard]] Status requireSimulationInstrument(const SimulationReferences&,
    const hftrec::corpus::BinaryMarketSource&,bool active,std::string&);
}
