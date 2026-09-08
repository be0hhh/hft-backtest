#include "../../src/Runtime/src/BacktestEngine.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <string_view>
#include <utility>

namespace {

using namespace hft_backtest;
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

trading_core::OrderIntent stopMarketIntent(std::uint64_t orderId,
                                               Side side,
                                               std::int64_t triggerPx,
                                               canon::OrderTriggerSource triggerSource = canon::OrderTriggerSource::Mark) noexcept {
  trading_core::OrderIntent out{};
  out.clientIntentId = id(orderId);
  out.side = side;
  out.orderType = canon::OrderType::Stop;
  out.triggerPrice = price(triggerPx);
  out.triggerSource = triggerSource;
  out.qty = amount(kQty1);
  return out;
}

trading_core::OrderIntent triggeredLimitStopIntent(std::uint64_t orderId,
                                                       Side side,
                                                       std::int64_t triggerPx,
                                                       std::int64_t capPx,
                                                       canon::TimeInForce triggeredTimeInForce) noexcept {
  trading_core::OrderIntent out = stopMarketIntent(orderId, side, triggerPx);
  out.price = price(capPx);
  out.triggeredTimeInForce = triggeredTimeInForce;
  return out;
}

void setTop(EngineState& engine, std::int64_t bid, std::int64_t ask) noexcept {
  cxet::composite::BookTickerRuntimeV1 top{};
  top.bid.px.raw = bid;
  top.ask.px.raw = ask;
  trading_core::copyBookTickerSnapshot(engine.snapshot, top);
  trading_core::copyBookTickerSnapshot(engine.venueSnapshot, top);
}

void setTopWithQty(EngineState& engine,
                   std::int64_t bid,
                   std::int64_t bidQty,
                   std::int64_t ask,
                   std::int64_t askQty) noexcept {
  setTop(engine, bid, ask);
  engine.snapshot.lastBookTicker.bid.qty.raw = bidQty;
  engine.snapshot.lastBookTicker.ask.qty.raw = askQty;
  engine.venueSnapshot.lastBookTicker.bid.qty.raw = bidQty;
  engine.venueSnapshot.lastBookTicker.ask.qty.raw = askQty;
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
  trading_core::LocalOrderBookConfig config{};
  config.maxStoredLevelsPerSide.raw = static_cast<std::uint32_t>(
      std::max(bids.size(), asks.size()));
  if (!engine.venueOrderBook.reserveLevelsPerSide(
          config.maxStoredLevelsPerSide) ||
      !trading_core::applySnapshot(
          engine.venueOrderBook, snapshot, engine.symbol, config)) {
    return;
  }
  trading_core::bindOrderBookSnapshot(engine.venueSnapshot, engine.venueOrderBook);
  engine.venueOrderBookExecutionEnabled = true;
  if (!bids.size() || !asks.size()) return;
  setTop(engine, bids.begin()->first, asks.begin()->first);
}

void submitTriggeredStop(EngineState& engine, trading_core::OrderIntent& order) noexcept {
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(&order, 1u);
  io.orderCount = 1u;
  applyIntents(engine, io, 1000, 0, 0);
}

void triggerMark(EngineState& engine, std::int64_t markPx, std::uint64_t tsNs) noexcept {
  cxet::composite::MarkPriceRuntimeV1 mark{};
  mark.ts.raw = tsNs;
  mark.markPrice.raw = markPx;
  tryTriggerStopMarketsOnMark(engine, mark, tsNs);
}

bool expect(bool value, std::string_view message) {
  if (value) return true;
  std::cerr << "FAIL: " << message << "\n";
  return false;
}

bool testTriggeredIocLimitStopCapsExecutionAndCancelsResidualFromDepth() {
  EngineState engine{};
  seedVenueBook(engine,
                {{104000000ll, kQty1}},
                {{105000000ll, 40000000ll}, {106000000ll, 60000000ll}});

  trading_core::OrderIntent order =
      triggeredLimitStopIntent(79, Side::Buy(), 110000000ll, 105000000ll, canon::TimeInForce::IOC);
  submitTriggeredStop(engine, order);
  triggerMark(engine, 110000000ll, 2700);

  return expect(engine.fills.size() == 1u, "depth IOC stop-limit fills only executable cap liquidity") &&
         expect(engine.fills[0].priceE8 == 105000000ll, "depth IOC stop-limit fills at capped ask") &&
         expect(engine.fills[0].qtyE8 == 40000000ll, "depth IOC stop-limit does not cross beyond cap") &&
         expect(engine.orders[0].status == kRecordStatusPartialUnfilled, "depth IOC stop-limit cancels residual") &&
         expect(!engine.open[0].state.valid, "depth IOC stop-limit clears open residual");
}

bool testTriggeredFokLimitStopRejectsPartialExecutableDepthLiquidity() {
  EngineState engine{};
  seedVenueBook(engine,
                {{104000000ll, kQty1}},
                {{105000000ll, 40000000ll}, {106000000ll, 60000000ll}});

  trading_core::OrderIntent order =
      triggeredLimitStopIntent(80, Side::Buy(), 110000000ll, 105000000ll, canon::TimeInForce::FOK);
  submitTriggeredStop(engine, order);
  triggerMark(engine, 110000000ll, 2800);

  return expect(engine.fills.empty(), "depth FOK stop-limit does not partially fill") &&
         expect(engine.orders[0].status == kRecordStatusPartialUnfilled,
                "depth FOK stop-limit cancels when full cap qty is unavailable") &&
         expect(!engine.open[0].state.valid, "depth FOK stop-limit clears open order");
}

bool testTriggeredIocLimitStopUsesBboQtyWhenDepthIsUnavailable() {
  EngineState engine{};
  setTopWithQty(engine, 104000000ll, kQty1, 105000000ll, 40000000ll);

  trading_core::OrderIntent order =
      triggeredLimitStopIntent(81, Side::Buy(), 110000000ll, 105000000ll, canon::TimeInForce::IOC);
  submitTriggeredStop(engine, order);
  triggerMark(engine, 110000000ll, 2900);

  return expect(engine.fills.size() == 1u, "BBO IOC stop-limit fills top quantity") &&
         expect(engine.fills[0].priceE8 == 105000000ll, "BBO IOC stop-limit fills at capped ask") &&
         expect(engine.fills[0].qtyE8 == 40000000ll, "BBO IOC stop-limit caps fill to ask quantity") &&
         expect(engine.orders[0].status == kRecordStatusPartialUnfilled, "BBO IOC stop-limit cancels residual") &&
         expect(!engine.open[0].state.valid, "BBO IOC stop-limit clears open residual");
}

bool testTriggeredFokLimitStopRejectsInsufficientBboQtyWhenDepthIsUnavailable() {
  EngineState engine{};
  setTopWithQty(engine, 104000000ll, kQty1, 105000000ll, 40000000ll);

  trading_core::OrderIntent order =
      triggeredLimitStopIntent(82, Side::Buy(), 110000000ll, 105000000ll, canon::TimeInForce::FOK);
  submitTriggeredStop(engine, order);
  triggerMark(engine, 110000000ll, 3000);

  return expect(engine.fills.empty(), "BBO FOK stop-limit does not partially fill") &&
         expect(engine.orders[0].status == kRecordStatusPartialUnfilled,
                "BBO FOK stop-limit cancels when top quantity is insufficient") &&
         expect(!engine.open[0].state.valid, "BBO FOK stop-limit clears open order");
}

bool testTriggeredFokLimitStopFillsWhenBboQtyIsSufficient() {
  EngineState engine{};
  setTopWithQty(engine, 104000000ll, kQty1, 105000000ll, kQty1);

  trading_core::OrderIntent order =
      triggeredLimitStopIntent(83, Side::Buy(), 110000000ll, 105000000ll, canon::TimeInForce::FOK);
  submitTriggeredStop(engine, order);
  triggerMark(engine, 110000000ll, 3100);

  return expect(engine.fills.size() == 1u, "BBO FOK stop-limit fills when top quantity is sufficient") &&
         expect(engine.fills[0].priceE8 == 105000000ll, "BBO FOK stop-limit fills at capped ask") &&
         expect(engine.fills[0].qtyE8 == kQty1, "BBO FOK stop-limit fills full order quantity") &&
         expect(engine.orders[0].status == kRecordStatusFilled, "BBO FOK stop-limit records filled order") &&
         expect(!engine.open[0].state.valid, "BBO FOK stop-limit clears open order after fill");
}

}  // namespace

int main() {
  bool ok = true;
  ok = testTriggeredIocLimitStopCapsExecutionAndCancelsResidualFromDepth() && ok;
  ok = testTriggeredFokLimitStopRejectsPartialExecutableDepthLiquidity() && ok;
  ok = testTriggeredIocLimitStopUsesBboQtyWhenDepthIsUnavailable() && ok;
  ok = testTriggeredFokLimitStopRejectsInsufficientBboQtyWhenDepthIsUnavailable() && ok;
  ok = testTriggeredFokLimitStopFillsWhenBboQtyIsSufficient() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
