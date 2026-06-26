#include "engine/backtest_prepared.hpp"

namespace hft_backtest::detail {
namespace {

thread_local PreparedBacktestInput* gPreparedBacktestInput = nullptr;

}  // namespace

PreparedBacktestScope::PreparedBacktestScope(PreparedBacktestInput* input) noexcept
    : previous_(gPreparedBacktestInput) {
  gPreparedBacktestInput = input;
}

PreparedBacktestScope::~PreparedBacktestScope() {
  gPreparedBacktestInput = previous_;
}

PreparedBacktestInput* activePreparedBacktestInput() noexcept {
  return gPreparedBacktestInput;
}

}  // namespace hft_backtest::detail
