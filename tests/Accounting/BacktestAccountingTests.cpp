#include "../../src/Runtime/src/BacktestEngine.hpp"

#include <cstdlib>
#include <iostream>
#include <limits>
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

bool testCanonicalFillTransitionsAndMarkToMarket() {
  constexpr std::int64_t kPrice100 = 10000000000ll;
  constexpr std::int64_t kQty1 = 100000000ll;
  constexpr std::int64_t kOneBpE8 = 100000000ll;

  const trading_core::FillPositionAccountingResult opened =
      trading_core::applyFillPositionAccounting(
          {}, Side::Buy(), kPrice100, kQty1, kOneBpE8);
  const trading_core::FillPositionAccountingResult increased =
      trading_core::applyFillPositionAccounting(
          opened.position, Side::Buy(), 11000000000ll, kQty1);
  const trading_core::FillPositionAccountingResult partial =
      trading_core::applyFillPositionAccounting(
          increased.position, Side::Sell(), 12000000000ll, 50000000ll);
  const trading_core::FillPositionAccountingResult closed =
      trading_core::applyFillPositionAccounting(
          partial.position, Side::Sell(), 9500000000ll, 150000000ll);
  const trading_core::FillPositionAccountingResult flipped =
      trading_core::applyFillPositionAccounting(
          {kQty1, kPrice100}, Side::Sell(), 11000000000ll, 200000000ll);

  return expect(opened.applied, "valid opening fill is applied") &&
         expect(opened.position.netQtyRaw == kQty1,
                "opening buy creates a long position") &&
         expect(opened.position.entryPriceE8 == kPrice100,
                "opening fill establishes entry price") &&
         expect(opened.feeE8 == 1000000ll,
                "one basis point fill fee preserves E8 floor division") &&
         expect(opened.netRealizedPnlE8 == -1000000ll,
                "opening fill realizes only its fee") &&
         expect(increased.position.netQtyRaw == 200000000ll,
                "same-direction fill increases quantity") &&
         expect(increased.position.entryPriceE8 == 10500000000ll,
                "same-direction fill uses weighted entry") &&
         expect(partial.position.netQtyRaw == 150000000ll,
                "partial close preserves the remaining position") &&
         expect(partial.position.entryPriceE8 == 10500000000ll,
                "partial close preserves entry price") &&
         expect(partial.grossRealizedPnlE8 == 750000000ll,
                "partial close realizes price difference times close quantity") &&
         expect(closed.position.netQtyRaw == 0 &&
                    closed.position.entryPriceE8 == 0,
                "full close clears quantity and entry price") &&
         expect(closed.grossRealizedPnlE8 == -1500000000ll,
                "loss on full close preserves signed floor arithmetic") &&
         expect(flipped.position.netQtyRaw == -kQty1 &&
                    flipped.position.entryPriceE8 == 11000000000ll,
                "oversized opposing fill flips at the fill price") &&
         expect(flipped.grossRealizedPnlE8 == 1000000000ll,
                "flip realizes only the closed quantity") &&
         expect(trading_core::markToMarketPnlE8(
                    flipped.position, 10700000000ll, 10800000000ll) ==
                    200000000ll,
                "short position marks to ask");
}

bool testCanonicalAccountingBoundsAndInvalidInputs() {
  constexpr std::int64_t kMax = std::numeric_limits<std::int64_t>::max();
  constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
  constexpr std::int64_t kPrice100 = 10000000000ll;

  const trading_core::FillPositionAccountingResult saturatedLong =
      trading_core::applyFillPositionAccounting(
          {kMax, kPrice100}, Side::Buy(), kPrice100, kMax);
  const trading_core::FillPositionAccountingResult saturatedShort =
      trading_core::applyFillPositionAccounting(
          {kMin, kPrice100}, Side::Sell(), kPrice100, kMax);
  const trading_core::FillPositionAccountingResult invalidSide =
      trading_core::applyFillPositionAccounting(
          {}, Side{2u}, kPrice100, 100000000ll);
  const trading_core::FillPositionAccountingResult invalidFlat =
      trading_core::applyFillPositionAccounting(
          {0, kPrice100}, Side::Buy(), kPrice100, 100000000ll);
  const trading_core::FillPositionAccountingResult invalidOpen =
      trading_core::applyFillPositionAccounting(
          {100000000ll, 0}, Side::Sell(), kPrice100, 100000000ll);

  return expect(trading_core::quoteNotionalE8(kMax, kMax) == kMax,
                "positive quote notional saturates at int64 max") &&
         expect(trading_core::quoteNotionalE8(kMin, kMax) == kMin,
                "negative quote notional saturates at int64 min") &&
         expect(trading_core::fillFeeE8(kMax, kMax, kMax) == kMax,
                "fill fee saturates at int64 max") &&
         expect(trading_core::fillFeeE8(kMax, kMax, 1) ==
                    850705917302346158ll,
                "fill fee uses the full wide notional before final saturation") &&
         expect(trading_core::weightedEntryPriceE8(
                    kPrice100, 100000000u, 10100000000ll, 200000000u) ==
                    10066666666ll,
                "weighted entry preserves floor division") &&
         expect(saturatedLong.applied &&
                    saturatedLong.position.netQtyRaw == kMax,
                "long quantity addition saturates") &&
         expect(saturatedShort.applied &&
                    saturatedShort.position.netQtyRaw == kMin,
                "short quantity addition saturates") &&
         expect(trading_core::markToMarketPnlE8(
                    {kMin, kMax}, 1, 1) == kMax,
                "INT64_MIN short magnitude remains bounded and observable") &&
         expect(!invalidSide.applied,
                "unknown side fails closed") &&
         expect(!invalidFlat.applied &&
                    invalidFlat.position.netQtyRaw == 0 &&
                    invalidFlat.position.entryPriceE8 == kPrice100,
                "flat state with retained entry fails closed unchanged") &&
         expect(!invalidOpen.applied &&
                    invalidOpen.position.netQtyRaw == 100000000ll &&
                    invalidOpen.position.entryPriceE8 == 0,
                "open state without entry basis fails closed unchanged");
}

}  // namespace

int main() {
  bool ok = true;
  ok = testInitializeAccountingSeedsValidFlatPosition() && ok;
  ok = testCanonicalFillTransitionsAndMarkToMarket() && ok;
  ok = testCanonicalAccountingBoundsAndInvalidInputs() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
