#include "engine/backtest_engine.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <string_view>
#include <utility>

namespace {

using namespace hft_backtest::detail;

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

hft_trader::core::OrderIntent postOnlyOrderIntent(std::uint64_t orderId,
                                                  Side side,
                                                  std::int64_t px,
                                                  std::int64_t qty = kQty1) noexcept {
  hft_trader::core::OrderIntent out{};
  out.clientIntentId = id(orderId);
  out.side = side;
  out.orderType = canon::OrderType::Limit;
  out.price = price(px);
  out.qty = amount(qty);
  out.limitTimeInForcePolicy = hft_trader::core::LimitTimeInForcePolicy::PostOnlyPrefer;
  return out;
}

hft_trader::core::OrderIntent limitOrderIntent(std::uint64_t orderId,
                                               Side side,
                                               std::int64_t px,
                                               std::int64_t qty = kQty1) noexcept {
  hft_trader::core::OrderIntent out{};
  out.clientIntentId = id(orderId);
  out.side = side;
  out.orderType = canon::OrderType::Limit;
  out.price = price(px);
  out.qty = amount(qty);
  return out;
}

void setTop(EngineState& engine,
            std::int64_t bid,
            std::int64_t bidQty,
            std::int64_t ask,
            std::int64_t askQty,
            std::uint64_t tsNs) noexcept {
  cxet::composite::BookTickerRuntimeV1 top{};
  top.bid.px.raw = bid;
  top.bid.qty.raw = bidQty;
  top.ask.px.raw = ask;
  top.ask.qty.raw = askQty;
  top.ts.raw = tsNs;
  hft_trader::core::copyBookTickerSnapshot(engine.snapshot, top);
  hft_trader::core::copyBookTickerSnapshot(engine.venueSnapshot, top);
}

void seedVenueBook(EngineState& engine,
                   std::initializer_list<std::pair<std::int64_t, std::int64_t>> bids,
                   std::initializer_list<std::pair<std::int64_t, std::int64_t>> asks) noexcept {
  cxet::composite::OrderBookSnapshot snapshot{};
  snapshot.ts.raw = 1000u;
  for (const auto& level : bids) {
    (void)cxet::composite::appendOrderBookLevel(snapshot, price(level.first), amount(level.second), Side::Buy());
  }
  for (const auto& level : asks) {
    (void)cxet::composite::appendOrderBookLevel(snapshot, price(level.first), amount(level.second), Side::Sell());
  }
  hft_trader::core::LocalOrderBookConfig config{};
  config.maxStoredLevelsPerSide.raw = static_cast<std::uint32_t>(
      std::max(bids.size(), asks.size()));
  if (!engine.venueOrderBook.reserveLevelsPerSide(
          config.maxStoredLevelsPerSide) ||
      !hft_trader::core::applySnapshot(
          engine.venueOrderBook, snapshot, engine.symbol, config)) {
    return;
  }
  hft_trader::core::bindOrderBookSnapshot(engine.venueSnapshot, engine.venueOrderBook);
  engine.venueOrderBookExecutionEnabled = true;
  if (!bids.size() || !asks.size()) return;
  setTop(engine, bids.begin()->first, bids.begin()->second, asks.begin()->first, asks.begin()->second, 1000u);
}

bool expect(bool value, std::string_view message) {
  if (value) return true;
  std::cerr << "FAIL: " << message << "\n";
  return false;
}

void submitOne(EngineState& engine, hft_trader::core::OrderIntent& order, std::int64_t tsNs) {
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(&order, 1u);
  io.orderCount = 1u;
  applyIntents(engine, io, tsNs, 0u, 0u);
}

bool testPassiveSellFillsWhenBestBidTouchesOrderAsk() {
  EngineState engine{};
  setTop(engine, 10000000000ll, kQty1, 10200000000ll, kQty1, 1000u);
  auto order = postOnlyOrderIntent(1u, Side::Sell(), 10100000000ll);
  submitOne(engine, order, 1000);

  setTop(engine, 10100000000ll, 60000000ll, 10200000000ll, kQty1, 1500u);
  tryFillOnTop(engine, 2000, true);

  return expect(engine.fills.size() == 1u, "passive sell fills when best bid touches ask") &&
         expect(engine.fills[0].fillReason == kFillReasonPassiveBboTouch, "passive sell uses BBO-touch fill reason") &&
         expect(engine.fills[0].liquidity == kFillLiquidityMaker, "passive sell BBO-touch fill is maker") &&
         expect(engine.fills[0].exitTsNs == 2000,
                "passive sell uses projected venue timestamp") &&
         expect(engine.fills[0].priceE8 == 10100000000ll, "passive sell fills at order price") &&
         expect(engine.fills[0].qtyE8 == 60000000ll, "passive sell BBO-touch fill is capped by bid qty") &&
         expect(engine.orders[0].status == kRecordStatusActive, "partial passive sell remains active");
}

bool testPassiveBuyFillsWhenBestAskTouchesOrderBid() {
  EngineState engine{};
  setTop(engine, 10000000000ll, kQty1, 10200000000ll, kQty1, 1000u);
  auto order = postOnlyOrderIntent(2u, Side::Buy(), 10100000000ll);
  submitOne(engine, order, 1000);

  setTop(engine, 10000000000ll, kQty1, 10100000000ll, kQty1, 2000u);
  tryFillOnTop(engine, 2000, true);

  return expect(engine.fills.size() == 1u, "passive buy fills when best ask touches bid") &&
         expect(engine.fills[0].fillReason == kFillReasonPassiveBboTouch, "passive buy uses BBO-touch fill reason") &&
         expect(engine.fills[0].liquidity == kFillLiquidityMaker, "passive buy BBO-touch fill is maker") &&
         expect(engine.fills[0].priceE8 == 10100000000ll, "passive buy fills at order price") &&
         expect(engine.orders[0].status == kRecordStatusFilled, "full passive buy marks order filled");
}

bool testPassiveSellDoesNotFillWhenOnlyAskMovesAway() {
  EngineState engine{};
  setTop(engine, 10000000000ll, kQty1, 10200000000ll, kQty1, 1000u);
  auto order = postOnlyOrderIntent(3u, Side::Sell(), 10100000000ll);
  submitOne(engine, order, 1000);

  setTop(engine, 10000000000ll, kQty1, 10300000000ll, kQty1, 2000u);
  tryFillOnTop(engine, 2000, true);

  return expect(engine.fills.empty(), "same-side ask move away does not fill passive sell") &&
         expect(engine.open[0].state.valid, "same-side ask move leaves passive sell open") &&
         expect(engine.orders[0].status == kRecordStatusActive, "same-side ask move keeps order active");
}

bool testPassiveBboTouchConsumesKnownQueueAheadBeforeFill() {
  EngineState engine{};
  seedVenueBook(engine,
                {{9900000000ll, kQty1}},
                {{10100000000ll, 55000000ll}});
  auto order = postOnlyOrderIntent(4u, Side::Sell(), 10100000000ll);
  submitOne(engine, order, 1000);
  const bool capturedQueueAhead = engine.open[0].queueAheadQtyRaw == 55000000ll;

  setTop(engine, 10100000000ll, 70000000ll, 10200000000ll, kQty1, 2000u);
  tryFillOnTop(engine, 2000, true);

  return expect(capturedQueueAhead, "passive BBO-touch captures queue ahead at activation") &&
         expect(engine.fills.size() == 1u, "BBO-touch fills only after queue ahead is consumed") &&
         expect(engine.fills[0].qtyE8 == 15000000ll, "BBO-touch fill subtracts queue ahead from opposite top qty") &&
         expect(engine.fills[0].queueAheadBeforeE8 == 55000000ll, "BBO-touch fill records queue ahead before") &&
         expect(engine.fills[0].queueAheadAfterE8 == 0, "BBO-touch fill records queue after");
}

bool testPassiveBboTouchIgnoresTradeThroughMinCrossTicks() {
  EngineState engine{};
  engine.tickSizeE8 = 100000000ll;
  engine.makerFillMinCrossTicks = 1u;
  setTop(engine, 10000000000ll, kQty1, 10200000000ll, kQty1, 1000u);
  auto order = postOnlyOrderIntent(5u, Side::Sell(), 10100000000ll);
  submitOne(engine, order, 1000);

  setTop(engine, 10100000000ll, kQty1, 10200000000ll, kQty1, 2000u);
  tryFillOnTop(engine, 2000, true);

  return expect(engine.fills.size() == 1u, "BBO-touch fills on equality even when trade min-cross requires through") &&
         expect(engine.fills[0].fillReason == kFillReasonPassiveBboTouch, "BBO-touch equality uses BBO-touch reason") &&
         expect(engine.fills[0].priceE8 == 10100000000ll, "BBO-touch equality fills at order price");
}

bool testFreshCrossingNonPostOnlyLimitStaysTaker() {
  EngineState engine{};
  setTop(engine, 10000000000ll, kQty1, 10100000000ll, kQty1, 1000u);
  auto order = limitOrderIntent(6u, Side::Buy(), 10100000000ll);
  submitOne(engine, order, 1000);

  return expect(engine.fills.size() == 1u, "fresh crossing non-postonly limit fills once") &&
         expect(engine.fills[0].fillReason == kFillReasonTop, "fresh crossing non-postonly limit keeps top fill reason") &&
         expect(engine.fills[0].liquidity == kFillLiquidityTaker, "fresh crossing non-postonly limit remains taker");
}

bool testDistinctBboEventsAtSameVenueTimestampRemainDistinct() {
  EngineState engine{};
  setTop(engine, 10000000000ll, kQty1, 10200000000ll, kQty1, 1000u);
  auto order = postOnlyOrderIntent(7u, Side::Sell(), 10100000000ll);
  submitOne(engine, order, 1000);

  setTop(engine, 10100000000ll, 60000000ll, 10200000000ll, kQty1, 2000u);
  tryFillOnTop(engine, 2000, true);
  const bool firstPartialFill = engine.fills.size() == 1u &&
                                engine.fills[0].qtyE8 == 60000000ll &&
                                engine.open[0].filledQtyRaw == 60000000ll;
  tryFillOnTop(engine, 2000, true);

  return expect(firstPartialFill,
                "first BBO-touch creates one partial maker fill") &&
         expect(engine.fills.size() == 2u,
                "distinct BBO event at the same venue timestamp is retained") &&
         expect(engine.fills[1].qtyE8 == 40000000ll,
                "second tied BBO event fills the remaining quantity") &&
         expect(!engine.open[0].state.valid,
                "second tied BBO event closes the filled order");
}

bool testBboTouchBurnsQueueAheadWithoutFillUntilNextTouch() {
  EngineState engine{};
  seedVenueBook(engine,
                {{9900000000ll, kQty1}},
                {{10100000000ll, 70000000ll}});
  auto order = postOnlyOrderIntent(8u, Side::Sell(), 10100000000ll);
  submitOne(engine, order, 1000);
  const bool capturedQueueAhead = engine.open[0].queueAheadQtyRaw == 70000000ll;

  setTop(engine, 10100000000ll, 40000000ll, 10200000000ll, kQty1, 2000u);
  tryFillOnTop(engine, 2000, true);
  const bool burnedOnly = engine.fills.empty() && engine.open[0].queueAheadQtyRaw == 30000000ll;

  setTop(engine, 10100000000ll, 50000000ll, 10200000000ll, kQty1, 3000u);
  tryFillOnTop(engine, 3000, true);

  return expect(capturedQueueAhead, "BBO-touch burn test captures queue ahead at activation") &&
         expect(burnedOnly, "first BBO-touch only burns queue ahead") &&
         expect(engine.fills.size() == 1u, "second BBO-touch creates fill after queue burn") &&
         expect(engine.fills[0].qtyE8 == 20000000ll, "second BBO-touch subtracts remaining queue ahead") &&
         expect(engine.fills[0].queueAheadBeforeE8 == 30000000ll, "second BBO-touch records remaining queue before fill") &&
         expect(engine.fills[0].queueAheadAfterE8 == 0, "second BBO-touch clears queue ahead");
}

bool testRestingNonPostOnlyLimitFillsOnLaterBboTouchAsMaker() {
  EngineState engine{};
  setTop(engine, 10000000000ll, kQty1, 10200000000ll, kQty1, 1000u);
  auto order = limitOrderIntent(9u, Side::Buy(), 10100000000ll);
  submitOne(engine, order, 1000);
  const bool initiallyResting = engine.fills.empty() && engine.open[0].state.valid;

  setTop(engine, 10000000000ll, kQty1, 10100000000ll, 45000000ll, 2000u);
  tryFillOnTop(engine, 2000, true);

  return expect(initiallyResting, "non-postonly non-crossing limit rests after activation") &&
         expect(engine.fills.size() == 1u, "resting non-postonly limit fills on later BBO-touch") &&
         expect(engine.fills[0].fillReason == kFillReasonPassiveBboTouch, "later BBO-touch uses maker touch reason") &&
         expect(engine.fills[0].liquidity == kFillLiquidityMaker, "later BBO-touch is maker for resting non-postonly limit") &&
         expect(engine.fills[0].qtyE8 == 45000000ll, "later BBO-touch is capped by opposite top qty");
}

bool testPassiveBboTouchRequiresVenueBboUpdate() {
  EngineState engine{};
  setTop(engine, 10000000000ll, kQty1, 10200000000ll, kQty1, 1000u);
  auto order = postOnlyOrderIntent(10u, Side::Sell(), 10100000000ll);
  submitOne(engine, order, 1000);

  setTop(engine, 10100000000ll, kQty1, 10200000000ll, kQty1, 2000u);
  tryFillOnTop(engine, 2500, false);

  return expect(engine.fills.empty(),
                "non-BBO execution event does not reuse retained top as a touch") &&
         expect(engine.open[0].state.valid,
                "non-BBO execution event leaves passive order open");
}

}  // namespace

int main() {
  bool ok = true;
  ok = testPassiveSellFillsWhenBestBidTouchesOrderAsk() && ok;
  ok = testPassiveBuyFillsWhenBestAskTouchesOrderBid() && ok;
  ok = testPassiveSellDoesNotFillWhenOnlyAskMovesAway() && ok;
  ok = testPassiveBboTouchConsumesKnownQueueAheadBeforeFill() && ok;
  ok = testPassiveBboTouchIgnoresTradeThroughMinCrossTicks() && ok;
  ok = testFreshCrossingNonPostOnlyLimitStaysTaker() && ok;
  ok = testDistinctBboEventsAtSameVenueTimestampRemainDistinct() && ok;
  ok = testBboTouchBurnsQueueAheadWithoutFillUntilNextTouch() && ok;
  ok = testRestingNonPostOnlyLimitFillsOnLaterBboTouchAsMaker() && ok;
  ok = testPassiveBboTouchRequiresVenueBboUpdate() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
