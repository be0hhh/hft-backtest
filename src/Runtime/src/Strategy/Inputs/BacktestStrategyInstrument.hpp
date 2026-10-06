#pragma once
#include "../../Input/Session/SessionLoader.hpp"
#include "trading_core/Contracts/Strategy/StrategyContract.hpp"
namespace hft_backtest {
[[nodiscard]] Status prepareBacktestStrategyInstrument(const detail::InstrumentMetadata& metadata,
    trading_core::StrategyInstrumentSpec& instrument, std::string& error);
}
