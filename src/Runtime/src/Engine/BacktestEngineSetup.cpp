#include "BacktestEngineSetup.hpp"

namespace hft_backtest {

bool applyRateLimitSchedule(detail::EngineState& engine,
                            const BacktestRateLimitSchedule& schedule,
                            std::string& error) {
  return engine.quotas.configure(schedule, error);
}

}  // namespace hft_backtest
