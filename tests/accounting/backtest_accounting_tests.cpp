#include "engine/backtest_engine.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

using namespace hft_backtest::detail;

bool expect(bool value, std::string_view message) {
  if (value) return true;
  std::cerr << "FAIL: " << message << "\n";
  return false;
}

bool testInitializeAccountingSeedsValidFlatPosition() {
  EngineState engine{};
  hft_backtest::BacktestRunRequest request{};
  request.initialBalanceE8 = 10000000000ll;
  request.makerFeeBpsE8 = 200000000ll;
  request.takerFeeBpsE8 = 500000000ll;

  initializeAccounting(engine, request);

  return expect(engine.initialBalanceE8 == request.initialBalanceE8, "initial balance is copied") &&
         expect(engine.walletBalanceE8 == request.initialBalanceE8, "wallet balance starts at initial balance") &&
         expect(engine.makerFeeBpsE8 == request.makerFeeBpsE8, "maker fee is copied") &&
         expect(engine.takerFeeBpsE8 == request.takerFeeBpsE8, "taker fee is copied") &&
         expect(engine.position.valid, "flat starting position is valid") &&
         expect(engine.position.netQty.raw == 0, "flat starting position has zero qty") &&
         expect(engine.position.entryPrice.raw == 0, "flat starting position has no entry price") &&
         expect(engine.position.unrealizedPnl.raw == 0, "flat starting position has no unrealized pnl");
}

}  // namespace

int main() {
  bool ok = true;
  ok = testInitializeAccountingSeedsValidFlatPosition() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
