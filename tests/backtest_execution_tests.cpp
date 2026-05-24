#include "backtest_engine.hpp"

#include "hft_trader/api/UserDataTools.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

using namespace hft_backtest::detail;

constexpr std::int64_t kPx100 = 10000000000ll;
constexpr std::int64_t kQty1 = 100000000ll;

Id id(std::uint64_t raw) noexcept {
  Id out{};
  out.raw = raw;
  return out;
}

Price price(std::int64_t raw) noexcept {
  Price out{};
  out.raw = raw;
  return out;
}

Amount amount(std::int64_t raw) noexcept {
  Amount out{};
  out.raw = raw;
  return out;
}

hft_trader::core::OrderIntent orderIntent(std::uint64_t orderId, Side side, std::int64_t px) noexcept {
  hft_trader::core::OrderIntent out{};
  out.clientIntentId = id(orderId);
  out.side = side;
  out.orderType = canon::OrderType::Limit;
  out.price = price(px);
  out.qty = amount(kQty1);
  return out;
}

hft_trader::core::OrderIntent reduceMarketIntent(std::uint64_t orderId, Side side) noexcept {
  hft_trader::core::OrderIntent out{};
  out.clientIntentId = id(orderId);
  out.side = side;
  out.orderType = canon::OrderType::Market;
  out.qty = amount(kQty1);
  out.reduceOnly = true;
  return out;
}

hft_trader::core::CancelIntent cancelIntent(std::uint64_t commandId, std::uint64_t targetId) noexcept {
  hft_trader::core::CancelIntent out{};
  out.clientIntentId = id(commandId);
  out.targetClientIntentId = id(targetId);
  out.targetOrderType = canon::OrderType::Limit;
  return out;
}

hft_trader::core::AmendIntent amendIntent(std::uint64_t commandId, std::uint64_t targetId, std::int64_t px) noexcept {
  hft_trader::core::AmendIntent out{};
  out.clientIntentId = id(commandId);
  out.targetClientIntentId = id(targetId);
  out.side = Side::Buy();
  out.targetOrderType = canon::OrderType::Limit;
  out.price = price(px);
  out.qty = amount(kQty1);
  return out;
}

void setTop(EngineState& engine, std::int64_t bid, std::int64_t ask) noexcept {
  engine.snapshot.lastBookTicker.bid.px.raw = bid;
  engine.snapshot.lastBookTicker.ask.px.raw = ask;
}

bool expect(bool value, std::string_view message) {
  if (value) return true;
  std::cerr << "FAIL: " << message << "\n";
  return false;
}

bool testStaleCancelDoesNotCancelReusedSlot() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<hft_trader::core::OrderIntent, 2> orders{orderIntent(1, Side::Buy(), kPx100 - 200000000ll), orderIntent(2, Side::Sell(), kPx100 + 200000000ll)};
  std::array<hft_trader::core::CancelIntent, 1> cancels{cancelIntent(10, 1)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), 1);
  io.orderCount = 1;
  applyIntents(engine, io, 0, 0, 0, 0);
  io.clear();
  io.cancelOut = Span<hft_trader::core::CancelIntent>(cancels.data(), cancels.size());
  io.cancelCount = 1;
  applyIntents(engine, io, 10, 0, 0, 100);
  (void)fillOrder(engine, engine.open[0], kPx100, 20, false);
  io.clear();
  io.orderOut = Span<hft_trader::core::OrderIntent>(&orders[1], 1);
  io.orderCount = 1;
  applyIntents(engine, io, 30, 0, 0, 0);
  applyDueActions(engine, 200000);
  return expect(engine.open[0].state.valid, "reused slot remains valid after stale cancel") &&
         expect(engine.open[0].state.clientIntentId.raw == 2, "stale cancel does not cancel replacement order") &&
         expect(engine.orders[0].status == "filled", "original order remains filled");
}

bool testStaleAmendDoesNotMutateReusedSlot() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<hft_trader::core::OrderIntent, 2> orders{orderIntent(1, Side::Buy(), kPx100 - 200000000ll), orderIntent(2, Side::Sell(), kPx100 + 200000000ll)};
  std::array<hft_trader::core::AmendIntent, 1> amends{amendIntent(11, 1, kPx100 - 300000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), 1);
  io.orderCount = 1;
  applyIntents(engine, io, 0, 0, 0, 0);
  io.clear();
  io.amendOut = Span<hft_trader::core::AmendIntent>(amends.data(), amends.size());
  io.amendCount = 1;
  applyIntents(engine, io, 10, 0, 100, 0);
  (void)fillOrder(engine, engine.open[0], kPx100, 20, false);
  io.clear();
  io.orderOut = Span<hft_trader::core::OrderIntent>(&orders[1], 1);
  io.orderCount = 1;
  applyIntents(engine, io, 30, 0, 0, 0);
  applyDueActions(engine, 200000);
  return expect(engine.open[0].state.valid, "reused slot remains valid after stale amend") &&
         expect(engine.open[0].state.clientIntentId.raw == 2, "stale amend does not replace order id") &&
         expect(engine.open[0].state.side.raw == Side::Sell().raw, "stale amend does not replace order side") &&
         expect(engine.open[0].state.price.raw == kPx100 + 200000000ll, "stale amend does not replace order price");
}

bool testPendingDrainActivatesLateOrder() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<hft_trader::core::OrderIntent, 1> orders{orderIntent(3, Side::Buy(), kPx100 - 200000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;
  applyIntents(engine, io, 1000, 100, 0, 0);
  return expect(engine.orders[0].status == "pending", "late order initially pending") &&
         (applyDueActions(engine, 101000), true) &&
         expect(engine.orders[0].status == "active", "final drain activates due order");
}

bool testFillPublishesRuntimeUserEvent() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<hft_trader::core::OrderIntent, 1> orders{orderIntent(4, Side::Buy(), kPx100 - 200000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;
  applyIntents(engine, io, 0, 0, 0, 0);
  const bool filled = fillOrder(engine, engine.open[0], kPx100, 20, false);
  hft_trader::strategy::RuntimeClientOrderId parsed{};
  return expect(filled, "manual fill succeeds") &&
         expect(engine.userEvents.size() == 1u, "fill creates one user event") &&
         expect(engine.userEvents[0].kind == cxet::composite::UserDataEventKind::Order, "user event is order kind") &&
         expect(engine.userEvents[0].orderStatus == canon::OrderStatus::Closed, "user event is final fill") &&
         expect(engine.userEvents[0].lastFilledQty.raw == kQty1, "user event has last fill qty") &&
         expect(hft_trader::strategy::parseRuntimeClientOrderId(engine.userEvents[0].clientOrderId, parsed), "client order id parses") &&
         expect(parsed.clientIntentId.raw == 4, "client order id carries intent id");
}

bool testReduceOnlyMarketSellClosesLongAtBid() {
  EngineState engine{};
  engine.takerFeeBpsE8 = 0;
  setTop(engine, kPx100 + 200000000ll, kPx100 + 300000000ll);
  std::int64_t netRealized = 0;
  updatePosition(engine, Side::Buy(), kPx100, kQty1, 100, 0, netRealized);
  std::array<hft_trader::core::OrderIntent, 1> orders{reduceMarketIntent(5, Side::Sell())};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;
  applyIntents(engine, io, 200, 0, 0, 0);
  return expect(engine.netQtyRaw == 0, "reduce-only sell flattens long") &&
         expect(engine.fills.size() == 1u, "reduce-only close writes fill") &&
         expect(engine.fills[0].priceE8 == kPx100 + 200000000ll, "long close uses bid") &&
         expect(engine.fills[0].reduceOnly, "fill is marked reduce-only") &&
         expect(engine.reduceOnlyOrders == 1u, "reduce-only order is counted") &&
         expect(engine.reduceOnlyFills == 1u, "reduce-only fill is counted") &&
         expect(engine.netRealizedPnlE8 == 200000000ll, "reduce-only close realizes bid-entry pnl");
}

bool testReduceOnlyMarketBuyClosesShortAtAsk() {
  EngineState engine{};
  engine.takerFeeBpsE8 = 0;
  setTop(engine, kPx100 - 300000000ll, kPx100 - 200000000ll);
  std::int64_t netRealized = 0;
  updatePosition(engine, Side::Sell(), kPx100, kQty1, 100, 0, netRealized);
  std::array<hft_trader::core::OrderIntent, 1> orders{reduceMarketIntent(6, Side::Buy())};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;
  applyIntents(engine, io, 200, 0, 0, 0);
  return expect(engine.netQtyRaw == 0, "reduce-only buy flattens short") &&
         expect(engine.fills.size() == 1u, "short close writes fill") &&
         expect(engine.fills[0].priceE8 == kPx100 - 200000000ll, "short close uses ask") &&
         expect(engine.netRealizedPnlE8 == 200000000ll, "short close realizes entry-ask pnl");
}

bool testFeesReduceWalletAndNetPnl() {
  EngineState engine{};
  engine.initialBalanceE8 = 100000000000ll;
  engine.walletBalanceE8 = engine.initialBalanceE8;
  engine.takerFeeBpsE8 = 100000000ll;
  std::int64_t netRealized = 0;
  updatePosition(engine, Side::Buy(), kPx100, kQty1, 100, engine.takerFeeBpsE8, netRealized);
  updatePosition(engine, Side::Sell(), kPx100 + 100000000ll, kQty1, 200, engine.takerFeeBpsE8, netRealized);
  return expect(engine.grossRealizedPnlE8 == 100000000ll, "gross pnl ignores fees") &&
         expect(engine.feesPaidE8 > 0, "fees are charged") &&
         expect(engine.netRealizedPnlE8 == engine.grossRealizedPnlE8 - engine.feesPaidE8, "net pnl subtracts fees") &&
         expect(engine.walletBalanceE8 == engine.initialBalanceE8 + engine.netRealizedPnlE8, "wallet tracks net pnl");
}

}  // namespace

int main() {
  bool ok = true;
  ok = testStaleCancelDoesNotCancelReusedSlot() && ok;
  ok = testStaleAmendDoesNotMutateReusedSlot() && ok;
  ok = testPendingDrainActivatesLateOrder() && ok;
  ok = testFillPublishesRuntimeUserEvent() && ok;
  ok = testReduceOnlyMarketSellClosesLongAtBid() && ok;
  ok = testReduceOnlyMarketBuyClosesShortAtAsk() && ok;
  ok = testFeesReduceWalletAndNetPnl() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
