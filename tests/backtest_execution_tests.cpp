#include "backtest_engine.hpp"

#include "backtest_latency.hpp"

#include "hft_trader/api/UserDataTools.hpp"
#include "hft_trader/runtime/GeneratedStrategyRegistry.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string_view>
#include <vector>

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

hft_trader::core::OrderIntent postOnlyOrderIntent(std::uint64_t orderId, Side side, std::int64_t px) noexcept {
  hft_trader::core::OrderIntent out = orderIntent(orderId, side, px);
  out.limitTimeInForcePolicy = hft_trader::core::LimitTimeInForcePolicy::PostOnlyPrefer;
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

void setTop(EngineState& engine, std::int64_t bid, std::int64_t ask) noexcept {
  engine.snapshot.lastBookTicker.bid.px.raw = bid;
  engine.snapshot.lastBookTicker.ask.px.raw = ask;
  engine.venueSnapshot.lastBookTicker.bid.px.raw = bid;
  engine.venueSnapshot.lastBookTicker.ask.px.raw = ask;
}

bool expect(bool value, std::string_view message) {
  if (value) return true;
  std::cerr << "FAIL: " << message << "\n";
  return false;
}

bool testBacktestCanonicalizesAsterSessionExchange() {
  return expect(exchangeFromText("aster").raw == canon::kExchangeIdAster.raw,
                "Aster recorder session exchange is recognized by backtest");
}

std::string readText(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::in | std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::size_t lineCount(std::string_view text) noexcept {
  std::size_t out = 0u;
  for (char ch : text) {
    if (ch == '\n') ++out;
  }
  return out;
}

void writeText(const std::filesystem::path& path, std::string_view text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::out | std::ios::trunc | std::ios::binary);
  out << text;
}

bool testStaleCancelDoesNotCancelReusedSlot() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<hft_trader::core::OrderIntent, 2> orders{orderIntent(1, Side::Buy(), kPx100 - 200000000ll), orderIntent(2, Side::Sell(), kPx100 + 200000000ll)};
  std::array<hft_trader::core::CancelIntent, 1> cancels{cancelIntent(10, 1)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), 1);
  io.orderCount = 1;
  applyIntents(engine, io, 0, 0, 0);
  io.clear();
  io.cancelOut = Span<hft_trader::core::CancelIntent>(cancels.data(), cancels.size());
  io.cancelCount = 1;
  applyIntents(engine, io, 10, 0, 100);
  (void)fillOrder(engine, engine.open[0], kPx100, 20, false);
  io.clear();
  io.orderOut = Span<hft_trader::core::OrderIntent>(&orders[1], 1);
  io.orderCount = 1;
  applyIntents(engine, io, 30, 0, 0);
  applyDueActions(engine, 200000);
  return expect(engine.open[0].state.valid, "reused slot remains valid after stale cancel") &&
         expect(engine.open[0].state.clientIntentId.raw == 2, "stale cancel does not cancel replacement order") &&
         expect(engine.orders[0].status == kRecordStatusFilled, "original order remains filled");
}

bool testPendingDrainActivatesLateOrder() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<hft_trader::core::OrderIntent, 1> orders{orderIntent(3, Side::Buy(), kPx100 - 200000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;
  applyIntents(engine, io, 1000, 100, 0);
  return expect(engine.orders[0].status == kRecordStatusPending, "late order initially pending") &&
         (applyDueActions(engine, 101000), true) &&
         expect(engine.orders[0].status == kRecordStatusActive, "final drain activates due order");
}

bool testCancelThenReplacementLimitMaterializesReplace() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<hft_trader::core::OrderIntent, 1> orders{postOnlyOrderIntent(1, Side::Buy(), kPx100 - 300000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;
  applyIntents(engine, io, 1000, 0, 0);

  std::array<hft_trader::core::CancelIntent, 1> cancels{cancelIntent(2, 1)};
  std::array<hft_trader::core::OrderIntent, 1> replacements{postOnlyOrderIntent(2, Side::Buy(), kPx100 - 200000000ll)};
  io.clear();
  io.cancelOut = Span<hft_trader::core::CancelIntent>(cancels.data(), cancels.size());
  io.cancelCount = 1;
  io.orderOut = Span<hft_trader::core::OrderIntent>(replacements.data(), replacements.size());
  io.orderCount = 1;
  applyIntents(engine, io, 2000, 0, 0);
  std::array<hft_trader::core::StrategyOrderState, 4> visible{};
  const std::size_t visibleCount = visibleOrders(engine, visible.data(), visible.size());

  return expect(engine.orders.size() == 3u, "replace writes cancel and replacement order records") &&
         expect(engine.orders[0].status == kRecordStatusCancelled, "replace cancels original order row") &&
         expect(engine.orders[1].action == kRecordActionCancel, "replace writes cancel command row") &&
         expect(engine.orders[1].targetId == 1u, "replace cancel targets original order") &&
         expect(engine.orders[1].status == kRecordStatusAcked, "replace cancel command is acked") &&
         expect(engine.orders[2].action == kRecordActionOrder, "replace writes replacement order row") &&
         expect(engine.orders[2].id == 2u, "replacement order uses replacement client intent id") &&
         expect(engine.orders[2].type == kRecordTypeLimit, "replacement remains a limit order") &&
         expect(engine.orders[2].priceE8 == kPx100 - 200000000ll, "replacement keeps replacement price") &&
         expect(engine.orders[2].postOnly, "replacement keeps post-only policy") &&
         expect(visibleCount == 1u, "replacement order is visible to strategy") &&
         expect(visible[0].clientIntentId.raw == 2u, "visible order is replacement id") &&
         expect(visible[0].desiredPrice.raw == kPx100 - 200000000ll, "visible order has replacement desired price");
}

bool testLatencySamplerIsReproducibleAndDeterministicWithoutJitter() {
  BacktestLatencyConfig config{};
  config.seed = 77u;
  config.marketData.baseUs = 2500u;
  config.marketData.jitterUs = 1000u;
  config.marketOrder.baseUs = 3100u;
  config.marketOrder.jitterUs = 0u;

  BacktestLatencyRuntime first{};
  BacktestLatencyRuntime second{};
  initLatencyRuntime(first, config);
  initLatencyRuntime(second, config);

  bool same = true;
  bool varied = false;
  std::uint64_t previous = 0u;
  for (std::uint32_t i = 0u; i < 64u; ++i) {
    const std::uint64_t a = sampleLatencyUs(first, BacktestLatencyChannel::MarketData);
    const std::uint64_t b = sampleLatencyUs(second, BacktestLatencyChannel::MarketData);
    same = same && a == b;
    if (i != 0u && a != previous) varied = true;
    previous = a;
  }

  return expect(same, "same latency seed produces same sequence") &&
         expect(varied, "jittered latency sequence varies") &&
         expect(sampleLatencyUs(first, BacktestLatencyChannel::MarketOrder) == 3100u,
                "zero jitter market order latency returns base") &&
         expect(latencyProfileMaxUs(config.marketData) == 10000u, "market data latency cap uses built-in spike bound");
}

bool testLatencyRuntimeSplitsMarketAndLimitOrderActivation() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  BacktestLatencyConfig config{};
  config.seed = 5u;
  config.marketOrder.baseUs = 100u;
  config.limitOrder.baseUs = 700u;
  initLatencyRuntime(engine.latency, config);

  std::array<hft_trader::core::OrderIntent, 2> orders{reduceMarketIntent(30, Side::Buy()), orderIntent(31, Side::Buy(), kPx100 - 200000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, engine.latency);

  return expect(engine.orders.size() == 2u, "market and limit orders are recorded") &&
         expect(engine.orders[0].type == kRecordTypeMarket, "first order is market") &&
         expect(engine.orders[0].activeTsNs == 101000, "market order uses market order latency") &&
         expect(engine.orders[1].type == kRecordTypeLimit, "second order is limit") &&
         expect(engine.orders[1].activeTsNs == 701000, "limit order uses limit order latency");
}

bool testFillPublishesRuntimeUserEvent() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<hft_trader::core::OrderIntent, 1> orders{orderIntent(4, Side::Buy(), kPx100 - 200000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;
  applyIntents(engine, io, 0, 0, 0);
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

bool testPostOnlyCrossingBuyRejectsWithoutTakerFill() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<hft_trader::core::OrderIntent, 1> orders{postOnlyOrderIntent(40, Side::Buy(), kPx100 + 100000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;

  applyIntents(engine, io, 1000, 0, 0);

  return expect(engine.orders.size() == 1u, "post-only crossing buy is recorded") &&
         expect(engine.orders[0].postOnly, "post-only crossing buy record keeps post-only flag") &&
         expect(engine.orders[0].status == kRecordStatusRejected, "post-only crossing buy is rejected") &&
         expect(engine.fills.empty(), "post-only crossing buy does not taker-fill") &&
         expect(!engine.open[0].state.valid, "post-only crossing buy leaves no open order") &&
         expect(engine.userEvents.size() == 1u, "post-only crossing buy publishes rejected user event") &&
         expect(engine.userEvents[0].orderStatus == canon::OrderStatus::Rejected, "post-only crossing buy event is rejected");
}

bool testPostOnlyCrossingSellRejectsWithoutTakerFill() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<hft_trader::core::OrderIntent, 1> orders{postOnlyOrderIntent(41, Side::Sell(), kPx100 - 100000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;

  applyIntents(engine, io, 1000, 0, 0);

  return expect(engine.orders.size() == 1u, "post-only crossing sell is recorded") &&
         expect(engine.orders[0].status == kRecordStatusRejected, "post-only crossing sell is rejected") &&
         expect(engine.fills.empty(), "post-only crossing sell does not taker-fill") &&
         expect(!engine.open[0].state.valid, "post-only crossing sell leaves no open order");
}

bool testPostOnlyRestingOrderDoesNotFillOnTopButFillsOnTrade() {
  EngineState engine{};
  setTop(engine, kPx100 - 200000000ll, kPx100 + 200000000ll);
  std::array<hft_trader::core::OrderIntent, 1> orders{postOnlyOrderIntent(42, Side::Buy(), kPx100 - 100000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;
  applyIntents(engine, io, 1000, 0, 0);
  const bool policyVisible = engine.open[0].state.limitTimeInForcePolicy == hft_trader::core::LimitTimeInForcePolicy::PostOnlyPrefer;

  setTop(engine, kPx100 - 200000000ll, kPx100 - 100000000ll);
  tryFillOnTop(engine, 2000);

  cxet::composite::TradeRuntimeV1 trade{};
  trade.ts.raw = 3000;
  trade.side = Side::Sell();
  trade.price.raw = kPx100 - 100000000ll;
  tryFillOnTrade(engine, trade);

  return expect(policyVisible, "post-only resting order state keeps post-only policy") &&
         expect(engine.orders[0].status == kRecordStatusFilled, "post-only resting order fills from trade") &&
         expect(engine.fills.size() == 1u, "post-only resting order has one maker fill") &&
         expect(engine.fills[0].orderId == 42u, "post-only maker fill uses order id") &&
         expect(engine.userEvents.size() == 1u, "post-only maker fill publishes one user event") &&
         expect(engine.userEvents[0].maker == 1u, "post-only trade fill is maker");
}

bool testGtcCrossingLimitStillTakerFills() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<hft_trader::core::OrderIntent, 1> orders{orderIntent(45, Side::Buy(), kPx100 + 100000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;

  applyIntents(engine, io, 1000, 0, 0);

  return expect(engine.orders[0].status == kRecordStatusFilled, "gtc crossing buy still fills") &&
         expect(engine.fills.size() == 1u, "gtc crossing buy writes fill") &&
         expect(engine.userEvents.size() == 1u, "gtc crossing buy publishes fill event") &&
         expect(engine.userEvents[0].maker == 0u, "gtc crossing buy is taker");
}

bool testReduceOnlyMarketBuyDoesNotOpenWhenFlat() {
  EngineState engine{};
  setTop(engine, kPx100 - 300000000ll, kPx100 - 200000000ll);
  std::array<hft_trader::core::OrderIntent, 1> orders{reduceMarketIntent(7, Side::Buy())};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;
  applyIntents(engine, io, 200, 0, 0);
  return expect(engine.netQtyRaw == 0, "reduce-only buy cannot open long when flat") &&
         expect(engine.fills.empty(), "invalid reduce-only order does not write fill") &&
         expect(engine.orders.size() == 1u, "invalid reduce-only order is recorded") &&
         expect(engine.orders[0].status == kRecordStatusRejected, "invalid reduce-only order is rejected") &&
         expect(engine.userEvents.size() == 1u, "invalid reduce-only order publishes final user event") &&
         expect(engine.userEvents[0].orderStatus == canon::OrderStatus::Rejected, "invalid reduce-only event is rejected");
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
  applyIntents(engine, io, 200, 0, 0);
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
  applyIntents(engine, io, 200, 0, 0);
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

bool testEquityPointCapturesRealizedOnlyPnl() {
  EngineState engine{};
  engine.initialBalanceE8 = 100000000000ll;
  engine.walletBalanceE8 = engine.initialBalanceE8;
  engine.feesPaidE8 = 30000000ll;
  engine.grossRealizedPnlE8 = 120000000ll;
  engine.netRealizedPnlE8 = 90000000ll;
  setTop(engine, kPx100 + 200000000ll, kPx100 + 300000000ll);
  std::int64_t netRealized = 0;
  updatePosition(engine, Side::Buy(), kPx100, kQty1, 100, 0, netRealized);

  appendEquityPoint(engine, 200);

  return expect(engine.equityPoints.size() == 1u, "equity point is appended") &&
         expect(engine.equityPoints[0].grossRealizedPnlE8 == 120000000ll, "gross realized pnl is captured") &&
         expect(engine.equityPoints[0].realizedPnlE8 == 90000000ll, "realized pnl is net pnl") &&
         expect(engine.equityPoints[0].unrealizedPnlE8 == 0, "equity point does not carry mark-to-market pnl") &&
         expect(engine.equityPoints[0].grossTotalPnlE8 == 120000000ll, "gross total stays realized-only") &&
         expect(engine.equityPoints[0].netTotalPnlE8 == 90000000ll, "net total stays realized-only") &&
         expect(engine.equityPoints[0].totalPnlE8 == engine.equityPoints[0].netTotalPnlE8, "total pnl aliases net total") &&
         expect(engine.equityPoints[0].feesPaidE8 == 30000000ll, "fees are captured as positive cost") &&
         expect(engine.equityPoints[0].walletBalanceE8 == engine.initialBalanceE8, "wallet is preserved before close") &&
         expect(engine.equityPoints[0].positionQtyE8 == kQty1, "position qty is captured");
}

bool testRiskSnapshotCapturesCompactMarkToMarketPnl() {
  EngineState engine{};
  engine.initialBalanceE8 = 100000000000ll;
  engine.walletBalanceE8 = engine.initialBalanceE8;
  engine.feesPaidE8 = 30000000ll;
  engine.grossRealizedPnlE8 = 120000000ll;
  engine.netRealizedPnlE8 = 90000000ll;
  setTop(engine, kPx100 + 200000000ll, kPx100 + 300000000ll);
  std::int64_t netRealized = 0;
  updatePosition(engine, Side::Buy(), kPx100, kQty1, 100, 0, netRealized);

  appendRiskSnapshot(engine, 200, kRiskSnapshotReasonFill);

  return expect(engine.riskSnapshots.size() == 1u, "risk snapshot is appended") &&
         expect(engine.riskSnapshots[0].unrealizedPnlE8 == 200000000ll, "risk snapshot marks long position to bid") &&
         expect(engine.riskSnapshots[0].grossTotalPnlE8 == 320000000ll, "risk gross total includes unrealized pnl") &&
         expect(engine.riskSnapshots[0].netTotalPnlE8 == 290000000ll, "risk net total includes unrealized pnl") &&
         expect(engine.riskSnapshots[0].entryPriceE8 == kPx100, "risk snapshot stores entry price") &&
         expect(engine.riskSnapshots[0].bidPriceE8 == kPx100 + 200000000ll, "risk snapshot stores bid") &&
         expect(engine.riskSnapshots[0].askPriceE8 == kPx100 + 300000000ll, "risk snapshot stores ask") &&
         expect(engine.riskSnapshots[0].reason == kRiskSnapshotReasonFill, "risk snapshot stores reason");
}

bool testRiskSnapshotCadenceLimitsOpenPositionMtmRows() {
  EngineState engine{};
  setTop(engine, kPx100, kPx100 + 100000000ll);
  std::int64_t netRealized = 0;
  updatePosition(engine, Side::Buy(), kPx100, kQty1, 100, 0, netRealized);
  appendRiskSnapshot(engine, 100, kRiskSnapshotReasonFill);

  setTop(engine, kPx100 + 100000000ll, kPx100 + 200000000ll);
  const bool early = appendRiskSnapshotIfDue(engine, 100 + 500000000ll);
  setTop(engine, kPx100 + 200000000ll, kPx100 + 300000000ll);
  const bool due = appendRiskSnapshotIfDue(engine, 100 + 1000000000ll);
  const bool unchanged = appendRiskSnapshotIfDue(engine, 100 + 2000000000ll);

  return expect(!early, "risk cadence skips sub-second MTM changes") &&
         expect(due, "risk cadence writes after one second") &&
         expect(!unchanged, "risk cadence skips unchanged MTM value") &&
         expect(engine.riskSnapshots.size() == 2u, "risk snapshots stay compact") &&
         expect(engine.riskSnapshots[1].reason == kRiskSnapshotReasonInterval, "cadence row is marked interval");
}

bool testHardLiquidationClearsStateAndBlocksNewOrders() {
  hft_backtest::BacktestRunRequest request{};
  request.initialBalanceE8 = 5000000000ll;

  EngineState engine{};
  initializeAccounting(engine, request);
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<hft_trader::core::OrderIntent, 1> orders{orderIntent(40, Side::Buy(), kPx100 - 200000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, 100, 0);

  std::int64_t netRealized = 0;
  updatePosition(engine, Side::Buy(), kPx100, kQty1, 1200, 0, netRealized);
  setTop(engine, kPx100 - 6000000000ll, kPx100 - 5900000000ll);
  const std::int64_t breachedEquity = equityE8(engine);

  hardLiquidate(engine, 2000, breachedEquity);
  std::array<hft_trader::core::StrategyOrderState, 4> visible{};
  io.clear();
  orders[0] = orderIntent(41, Side::Sell(), kPx100 + 200000000ll);
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 3000, 0, 0);

  return expect(breachedEquity <= 0, "test setup breaches 100 percent equity loss") &&
         expect(engine.liquidated, "engine records hard liquidation") &&
         expect(engine.liquidationTsNs == 2000, "liquidation stores timestamp") &&
         expect(engine.liquidationEquityE8 == breachedEquity, "liquidation stores breached equity") &&
         expect(engine.walletBalanceE8 == 0, "liquidation zeroes wallet") &&
         expect(engine.position.valid && engine.position.netQty.raw == 0, "liquidation flattens strategy position view") &&
         expect(engine.netQtyRaw == 0, "liquidation clears accounting position") &&
         expect(engine.pending.empty(), "liquidation clears pending actions") &&
         expect(visibleOrders(engine, visible.data(), visible.size()) == 0u, "liquidation exposes no open orders") &&
         expect(engine.riskSnapshots.back().reason == kRiskSnapshotReasonLiquidation, "liquidation writes risk snapshot reason") &&
         expect(engine.orders.size() == 1u, "liquidated engine ignores later order intents");
}

bool testTraderRiskStopCancelsOrdersKeepsPositionAndBlocksNewOrders() {
  hft_backtest::BacktestRunRequest request{};
  request.initialBalanceE8 = 5000000000ll;

  EngineState engine{};
  initializeAccounting(engine, request);
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<hft_trader::core::OrderIntent, 1> orders{orderIntent(50, Side::Buy(), kPx100 - 200000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 1000, 0, 0);

  std::int64_t netRealized = 0;
  updatePosition(engine, Side::Buy(), kPx100, kQty1, 1200, 0, netRealized);
  setTop(engine, kPx100 - 3000000000ll, kPx100 - 2900000000ll);
  const std::int64_t breachedEquity = equityE8(engine);
  const std::int64_t thresholdEquity = 3000000000ll;

  stopForTraderRisk(engine, 2000, breachedEquity, thresholdEquity);
  std::array<hft_trader::core::StrategyOrderState, 4> visible{};
  io.clear();
  orders[0] = orderIntent(51, Side::Sell(), kPx100 + 200000000ll);
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, 3000, 0, 0);

  return expect(breachedEquity < thresholdEquity, "test setup breaches trader risk threshold") &&
         expect(!engine.liquidated, "trader risk stop is not hard liquidation") &&
         expect(engine.riskStopped, "engine records trader risk stop") &&
         expect(engine.riskStopTsNs == 2000, "risk stop stores timestamp") &&
         expect(engine.riskStopEquityE8 == breachedEquity, "risk stop stores breached equity") &&
         expect(engine.riskStopThresholdEquityE8 == thresholdEquity, "risk stop stores threshold equity") &&
         expect(engine.walletBalanceE8 == request.initialBalanceE8, "risk stop keeps wallet balance") &&
         expect(engine.position.valid && engine.position.netQty.raw == kQty1, "risk stop keeps strategy position view") &&
         expect(engine.netQtyRaw == kQty1, "risk stop keeps accounting position") &&
         expect(engine.pending.empty(), "risk stop clears pending actions") &&
         expect(visibleOrders(engine, visible.data(), visible.size()) == 0u, "risk stop exposes no open orders") &&
         expect(engine.riskSnapshots.back().reason == kRiskSnapshotReasonTraderRiskStop, "risk stop writes risk snapshot reason") &&
         expect(engine.orders.size() == 1u, "risk-stopped engine ignores later order intents");
}

bool testForceCloseOpenShortPositionSubmitsReduceOnlyMarketBuy() {
  EngineState engine{};
  engine.takerFeeBpsE8 = 0;
  setTop(engine, kPx100 - 300000000ll, kPx100 - 200000000ll);
  std::int64_t netRealized = 0;
  updatePosition(engine, Side::Sell(), kPx100, kQty1, 100, 0, netRealized);

  const bool closed = forceClosePosition(engine, 200, 100);

  return expect(closed, "force close reports success") &&
         expect(engine.netQtyRaw == 0, "force close flattens short") &&
         expect(engine.orders.size() == 1u, "force close writes one order") &&
         expect(engine.orders[0].side == kRecordSideBuy, "short force close submits buy") &&
         expect(engine.orders[0].type == kRecordTypeMarket, "force close uses market order") &&
         expect(engine.orders[0].reduceOnly, "force close order is reduce-only") &&
         expect(engine.orders[0].activeTsNs == 100200, "force close honors order latency") &&
         expect(engine.fills.size() == 1u, "force close writes fill") &&
         expect(engine.fills[0].reduceOnly, "force close fill is reduce-only");
}


bool testCancelOpenOrdersAtFinishWritesCancelAcks() {
  EngineState engine{};
  setTop(engine, kPx100 - 100000000ll, kPx100 + 100000000ll);
  std::array<hft_trader::core::OrderIntent, 1> orders{orderIntent(1, Side::Buy(), kPx100 - 200000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;
  applyIntents(engine, io, 1000, 0, 0);

  cancelOpenOrders(engine, 2000, 100);

  return expect(engine.orders.size() == 2u, "final cleanup writes cancel command") &&
         expect(engine.orders[0].status == kRecordStatusCancelled, "final cleanup cancels active quote row") &&
         expect(engine.orders[1].action == kRecordActionCancel, "final cleanup uses cancel action") &&
         expect(engine.orders[1].targetId == 1u, "final cleanup targets live client intent id") &&
         expect(engine.orders[1].status == kRecordStatusAcked, "final cleanup cancel is acked") &&
         expect(engine.orders[1].activeTsNs == 102000, "final cleanup honors cancel latency") &&
         expect(!engine.open[0].state.valid, "final cleanup clears open order state");
}
bool testLoadSessionUsesManifestChannelPathsAndMetadata() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_loader_manifest_" + std::to_string(std::rand()));
  writeText(dir / "manifest.json",
            "{\n"
            "  \"identity\": {\"exchange\": \"binance\", \"market\": \"futures\", \"symbols\": [\"BTCUSDT\"]},\n"
            "  \"channels\": {\n"
            "    \"trades\": {\"path\": \"jsonl/trades.jsonl\"},\n"
            "    \"bookticker\": {\"path\": \"jsonl/bookticker.jsonl\"},\n"
            "    \"price_limit\": {\"path\": \"jsonl/price_limit.jsonl\"},\n"
            "    \"depth\": {\"path\": \"jsonl/depth.jsonl\"},\n"
            "    \"candles\": {\"path\": \"jsonl/candles.jsonl\"}\n"
            "  },\n"
            "  \"artifacts\": {\"instrument_metadata_path\": \"instrument_metadata.json\"}\n"
            "}\n");
  writeText(dir / "instrument_metadata.json", "{\"tick_size_e8\":1000000,\"lot_size_e8\":1000}\n");
  writeText(dir / "jsonl" / "trades.jsonl", "[10000000000,100000000,1,1000]\n");
  writeText(dir / "jsonl" / "bookticker.jsonl", "[9900000000,100000000,10100000000,100000000,1001]\n");
  writeText(dir / "jsonl" / "price_limit.jsonl", "[9500000000,10500000000,1,1001]\n");
  writeText(dir / "jsonl" / "depth.jsonl", "[[9900000000,100000000,1],[10100000000,100000000,0],1002]\n");
  writeText(dir / "jsonl" / "candles.jsonl", "[1,900,10200000000,9800000000,500000000]\n");

  SessionLoadPlan plan{};
  plan.loadTrades = true;
  plan.loadBookTickers = true;
  plan.loadDepths = true;
  plan.loadCandles = true;
  plan.requireTrades = true;
  plan.requireBookTickers = true;
  plan.requireDepths = true;
  plan.requireCandles = true;
  SessionData data{};
  std::string error;
  const hft_backtest::Status status = loadSession(dir, plan, data, error);

  return expect(status == hft_backtest::Status::Ok, "loader accepts current manifest channel path schema") &&
         expect(data.exchange == "binance", "loader reads exchange identity") &&
         expect(data.market == "futures", "loader reads market identity") &&
         expect(data.symbol == "BTCUSDT", "loader reads first symbol identity") &&
         expect(data.trades.size() == 1u, "loader reads trades from channel path") &&
         expect(data.bookTickers.size() == 1u, "loader reads bookticker from channel path") &&
         expect(data.priceLimits.size() == 1u, "loader reads price_limit from channel path") &&
         expect(data.depths.size() == 1u, "loader reads depth from channel path") &&
         expect(data.candles.size() == 1u, "loader reads candles from channel path") &&
         expect(data.instrument.tickSizeE8 == 1000000, "loader reads tick size metadata") &&
         expect(data.instrument.lotSizeE8 == 1000, "loader reads lot size metadata");
}

bool testLoadSessionFailsMissingRequiredDepth() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_loader_missing_depth_" + std::to_string(std::rand()));
  writeText(dir / "manifest.json",
            "{\"identity\":{\"exchange\":\"binance\",\"market\":\"futures\",\"symbols\":[\"BTCUSDT\"]},"
            "\"channels\":{\"depth\":{\"path\":\"jsonl/depth.jsonl\"}}}\n");
  SessionLoadPlan plan{};
  plan.loadDepths = true;
  plan.requireDepths = true;
  SessionData data{};
  std::string error;
  const hft_backtest::Status status = loadSession(dir, plan, data, error);

  return expect(status == hft_backtest::Status::CorruptData, "loader rejects missing required depth") &&
         expect(error.find("depth") != std::string::npos, "missing required depth error names channel");
}

bool testLoadSessionPrefersDepthTapeSidecar() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_depth_tape_" + std::to_string(std::rand()));
  std::filesystem::create_directories(dir / "jsonl");
  writeText(dir / "manifest.json",
            "{\"identity\":{\"exchange\":\"binance\",\"market\":\"futures\",\"symbols\":[\"BTCUSDT\"]},"
            "\"channels\":{\"depth\":{\"path\":\"jsonl/depth_tape.jsonl\","
            "\"sidecar_path\":\"jsonl/depth_sidecar.jsonl\"}}}\n");
  writeText(dir / "jsonl" / "depth_tape.jsonl",
            "[9223372036854776810,9900000000,100000000,10100000000,200000000]\n");
  writeText(dir / "jsonl" / "depth_sidecar.jsonl", "[9223372036854776810,0,1,1,1]\n");
  writeText(dir / "jsonl" / "depth.jsonl", "[[9700000000,100000000,0],[10300000000,100000000,1],1002]\n");

  SessionLoadPlan plan{};
  plan.loadDepths = true;
  plan.requireDepths = true;
  SessionData data{};
  std::string error;
  const hft_backtest::Status status = loadSession(dir, plan, data, error);

  return expect(status == hft_backtest::Status::Ok, "loader accepts depth tape sidecar package") &&
         expect(data.depths.size() == 1u, "depth tape sidecar yields one depth row") &&
         expect(data.depths[0].tsNs == 1002, "depth tape sidecar decodes tagged timestamp") &&
         expect(data.depths[0].levels.size() == 2u, "depth tape sidecar decodes levels") &&
         expect(data.depths[0].levels[0].priceE8 == 9900000000ll, "depth tape sidecar wins over legacy depth") &&
         expect(data.depths[0].levels[0].side == 0, "depth tape sidecar applies bid side") &&
         expect(data.depths[0].levels[1].side == 1, "depth tape sidecar applies ask side");
}

bool testLoadSessionKeepsDepthTapeSidecarPrefixWhenTapeTailIsUnpaired() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_depth_tape_partial_tail_" + std::to_string(std::rand()));
  std::filesystem::create_directories(dir / "jsonl");
  writeText(dir / "manifest.json",
            "{\"identity\":{\"exchange\":\"binance\",\"market\":\"futures\",\"symbols\":[\"BTCUSDT\"]},"
            "\"channels\":{\"depth\":{\"path\":\"jsonl/depth_tape.jsonl\","
            "\"sidecar_path\":\"jsonl/depth_sidecar.jsonl\"}}}\n");
  writeText(dir / "jsonl" / "depth_tape.jsonl",
            "[9223372036854776810,9900000000,100000000,10100000000,200000000]\n"
            "[9223372036854776811,9910000000,100000000,10110000000,200000000]\n"
            "[9223372036854776812,9920000000,100000000,10120000000,200000000]\n");
  writeText(dir / "jsonl" / "depth_sidecar.jsonl",
            "[9223372036854776810,0,1,1,1]\n"
            "[9223372036854776811,0,1,1,1]\n");

  SessionLoadPlan plan{};
  plan.loadDepths = true;
  plan.requireDepths = true;
  SessionData data{};
  std::string error;
  const hft_backtest::Status status = loadSession(dir, plan, data, error);

  return expect(status == hft_backtest::Status::Ok, "loader accepts paired depth prefix with unpaired tape tail") &&
         expect(error.empty(), "partial depth tail does not become hard loader error") &&
         expect(data.depths.size() == 2u, "partial depth tail keeps valid paired prefix") &&
         expect(data.warnings.size() == 1u, "partial depth tail emits one warning") &&
         expect(data.warnings[0].find("dropped 1 unpaired depth_tape rows") != std::string::npos,
                "partial depth tail warning names dropped tape rows");
}

bool testLoadSessionResyncsDepthTapeSidecarAfterMissingMiddleSidecarRow() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_depth_tape_resync_" + std::to_string(std::rand()));
  std::filesystem::create_directories(dir / "jsonl");
  writeText(dir / "manifest.json",
            "{\"identity\":{\"exchange\":\"binance\",\"market\":\"futures\",\"symbols\":[\"BTCUSDT\"]},"
            "\"channels\":{\"depth\":{\"path\":\"jsonl/depth_tape.jsonl\","
            "\"sidecar_path\":\"jsonl/depth_sidecar.jsonl\"}}}\n");
  writeText(dir / "jsonl" / "depth_tape.jsonl",
            "[9223372036854776810,9900000000,100000000,10100000000,200000000]\n"
            "[9223372036854776811,9910000000,100000000,10110000000,200000000]\n"
            "[9223372036854776812,9920000000,100000000,10120000000,200000000]\n");
  writeText(dir / "jsonl" / "depth_sidecar.jsonl",
            "[9223372036854776810,0,1,1,1]\n"
            "[9223372036854776812,0,1,1,1]\n");

  SessionLoadPlan plan{};
  plan.loadDepths = true;
  plan.requireDepths = true;
  SessionData data{};
  std::string error;
  const hft_backtest::Status status = loadSession(dir, plan, data, error);

  return expect(status == hft_backtest::Status::Ok, "loader resyncs after a missing middle sidecar row") &&
         expect(error.empty(), "resynced depth corruption does not become hard loader error") &&
         expect(data.depths.size() == 2u, "resynced depth load keeps rows before and after bad row") &&
         expect(data.depths[0].tsNs == 1002, "resynced depth keeps first row") &&
         expect(data.depths[1].tsNs == 1004, "resynced depth resumes at next matching timestamp") &&
         expect(!data.warnings.empty(), "resynced depth emits warning") &&
         expect(data.warnings[0].find("dropped 1 unpaired depth_tape rows") != std::string::npos,
                "resync warning names skipped tape row");
}

bool testLoadSessionSkipsCorruptMiddleDepthTapeSidecarPair() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_depth_tape_corrupt_pair_" + std::to_string(std::rand()));
  std::filesystem::create_directories(dir / "jsonl");
  writeText(dir / "manifest.json",
            "{\"identity\":{\"exchange\":\"binance\",\"market\":\"futures\",\"symbols\":[\"BTCUSDT\"]},"
            "\"channels\":{\"depth\":{\"path\":\"jsonl/depth_tape.jsonl\","
            "\"sidecar_path\":\"jsonl/depth_sidecar.jsonl\"}}}\n");
  writeText(dir / "jsonl" / "depth_tape.jsonl",
            "[9223372036854776810,9900000000,100000000,10100000000,200000000]\n"
            "[9223372036854776811,9910000000,100000000,10110000000,200000000]\n"
            "[9223372036854776812,9920000000,100000000,10120000000,200000000]\n");
  writeText(dir / "jsonl" / "depth_sidecar.jsonl",
            "[9223372036854776810,0,1,1,1]\n"
            "[9223372036854776811,0,3]\n"
            "[9223372036854776812,0,1,1,1]\n");

  SessionLoadPlan plan{};
  plan.loadDepths = true;
  plan.requireDepths = true;
  SessionData data{};
  std::string error;
  const hft_backtest::Status status = loadSession(dir, plan, data, error);

  return expect(status == hft_backtest::Status::Ok, "loader skips a corrupt middle depth pair") &&
         expect(error.empty(), "skipped corrupt depth pair does not become hard loader error") &&
         expect(data.depths.size() == 2u, "skipped corrupt depth pair keeps surrounding rows") &&
         expect(data.depths[0].tsNs == 1002, "skipped corrupt depth keeps first row") &&
         expect(data.depths[1].tsNs == 1004, "skipped corrupt depth resumes on later valid row") &&
         expect(!data.warnings.empty(), "skipped corrupt depth pair emits warning") &&
         expect(data.warnings[0].find("dropped corrupt depth_tape/depth_sidecar row") != std::string::npos,
                "corrupt pair warning names skipped row");
}

bool testLoadSessionRejectsDepthTapeWithoutTaggedTimestamp() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_depth_tape_bad_tag_" + std::to_string(std::rand()));
  std::filesystem::create_directories(dir / "jsonl");
  writeText(dir / "manifest.json",
            "{\"identity\":{\"exchange\":\"binance\",\"market\":\"futures\",\"symbols\":[\"BTCUSDT\"]},"
            "\"channels\":{\"depth\":{\"path\":\"jsonl/depth_tape.jsonl\","
            "\"sidecar_path\":\"jsonl/depth_sidecar.jsonl\"}}}\n");
  writeText(dir / "jsonl" / "depth_tape.jsonl",
            "[1002,9900000000,100000000]\n");
  writeText(dir / "jsonl" / "depth_sidecar.jsonl", "[1002,1,1]\n");

  SessionLoadPlan plan{};
  plan.loadDepths = true;
  plan.requireDepths = true;
  SessionData data{};
  std::string error;
  const hft_backtest::Status status = loadSession(dir, plan, data, error);

  return expect(status == hft_backtest::Status::CorruptData,
                "loader rejects depth tape without tagged timestamp") &&
         expect(error.find("depth_tape") != std::string::npos,
                "bad depth tape error names depth_tape");
}

bool testDepthRowBuildsInitialOrderBookSnapshot() {
  DepthRow row{};
  row.tsNs = 1002;
  row.levels.push_back(PriceLevelRow{9900000000ll, 100000000ll, 0});
  row.levels.push_back(PriceLevelRow{10100000000ll, 200000000ll, 1});
  cxet::composite::OrderBookTapeRuntimeV1 tape{};
  cxet::composite::OrderBookTapeSidesRuntimeV1 sides{};
  toRuntime(row, tape, sides);
  cxet::composite::OrderBookSnapshot snapshot = toRuntimeSnapshot(row);

  hft_trader::core::LocalOrderBookState book{};
  hft_trader::core::LocalOrderBookConfig config{};
  config.maxStoredLevelsPerSide.raw = rawdata::kMaxOrderbookLevels;
  config.tickSizeRaw = 1000000;
  Symbol symbol{};
  symbol.copyFrom("BTCUSDT");
  hft_trader::core::applySnapshot(book, snapshot, symbol, config);

  const hft_trader::core::LocalOrderBookView view = hft_trader::core::orderBook(book);
  return expect(snapshot.levelCount.raw == 2u, "depth row snapshot carries levels") &&
         expect(sides.sideCount.raw == 2u, "depth row tape carries side count") &&
         expect(sides.sides[0] == Side::Buy().raw, "depth row tape maps side zero to bid") &&
         expect(sides.sides[1] == Side::Sell().raw, "depth row tape maps side one to ask") &&
         expect(book.initialized.raw == hft_trader::core::kAlgoFlagTrue.raw, "depth row snapshot initializes local orderbook") &&
         expect(hft_trader::core::bestBid(view).raw == 9900000000ll, "depth row snapshot sets best bid") &&
         expect(hft_trader::core::bestAsk(view).raw == 10100000000ll, "depth row snapshot sets best ask");
}

bool testTieredCandleHistoryBuildsCounts() {
  std::vector<CandleRow> rows{};
  rows.push_back(CandleRow{1, 1000, 10200000000ll, 9800000000ll, 500000000ll});
  rows.push_back(CandleRow{2, 900, 10300000000ll, 9700000000ll, 600000000ll});
  rows.push_back(CandleRow{3, 800, 10400000000ll, 9600000000ll, 700000000ll});
  cxet::composite::TieredCandleHistory history{};
  const bool ok = buildTieredCandleHistory(rows, history);

  return expect(ok, "tiered candle history accepts valid rows") &&
         expect(history.m1Count.raw == 1u, "tiered candle history writes m1 count") &&
         expect(history.m15Count.raw == 1u, "tiered candle history writes m15 count") &&
         expect(history.d1Count.raw == 1u, "tiered candle history writes d1 count") &&
         expect(history.m1Status == cxet::composite::CandleHistoryTierStatus::Ok, "tiered candle history marks m1 ok") &&
         expect(history.m15Status == cxet::composite::CandleHistoryTierStatus::Ok, "tiered candle history marks m15 ok") &&
         expect(history.d1Status == cxet::composite::CandleHistoryTierStatus::Ok, "tiered candle history marks d1 ok") &&
         expect(history.m1[0].quoteAmount.raw == 500000000ll, "tiered candle history preserves quote amount");
}

bool testWriterCreatesV2RunDirectory() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_v2_writer_" + std::to_string(std::rand()));
  hft_backtest::BacktestRunResult result{};
  result.status = hft_backtest::Status::Ok;
  result.requestId = "run-v2";
  result.runId = "run-v2";
  result.strategy = "spread_maker1and2";
  result.indicatorProfile = "trend_score";
  result.sessionPath = dir.parent_path() / "session";
  result.configPath = dir / "source_config.ini";
  result.resultPath = dir;
  result.orderLatencyUs = 3000;
  result.cancelLatencyUs = 3000;
  result.events = 2;
  result.trades = 1;
  result.bookTickers = 1;
  result.initialBalanceE8 = 100000000000ll;
  result.walletBalanceE8 = 100100000000ll;
  result.totalPnlE8 = 100000000ll;
  result.warnings.push_back("partial depth replay: dropped 1 unpaired depth_tape rows; exact replay lost");
  result.liveProbe.configured = true;
  result.liveProbe.enabled = true;
  result.liveProbe.ignored = true;
  result.liveProbe.notionalUsdtE8 = 1000000000ll;
  result.liveProbe.timeoutMs = 1000u;
  result.liveProbe.limitOffsetSpreadPct = 5u;
  hft_backtest::BacktestLegSummary leg{};
  leg.exchange = "binance";
  leg.market = "futures";
  leg.symbol = "BTCUSDT";
  leg.marketDataLatency = hft_backtest::BacktestLatencyProfile{250u, 100u};
  leg.marketOrderLatency = hft_backtest::BacktestLatencyProfile{2500u, 1000u};
  leg.limitOrderLatency = hft_backtest::BacktestLatencyProfile{1800u, 700u};
  result.legs.push_back(leg);

  std::filesystem::create_directories(dir);
  {
    std::ofstream config(result.configPath, std::ios::out | std::ios::trunc);
    config << "[strategy]\ntype=spread_maker1and2\n";
  }

  EngineState engine{};
  engine.orders.push_back(JsonOrder{0, 1, 0, 100, 110, 100, kRecordActionOrder, kRecordSideBuy, kRecordTypeLimit, kRecordStatusFilled, kPx100, kQty1, false});
  engine.fills.push_back(JsonFill{0, 1, 110, 150, kRecordSideBuy, kPx100, kQty1, 0, false});
  engine.equityPoints.push_back(JsonEquityPoint{150, 0, 0, 0, 0, 0, 0, 0, 100000000000ll, kQty1, 1});
  engine.riskSnapshots.push_back(JsonRiskSnapshot{150, 0, 0, 0, 0, 0, 0, 100000000000ll, kQty1, kPx100, kPx100 - 100000000ll, kPx100 + 100000000ll, 1, kRiskSnapshotReasonFill});
  engine.strategyIndicatorProfile = "trend_score";
  engine.strategyIndicatorTitle = "Trend score";
  engine.strategyIndicatorValueLabel = "Score";
  engine.strategyIndicatorAuxLabel = "Raw";
  engine.strategyIndicatorUnit = "score";
  engine.strategyIndicator.push_back(JsonStrategyIndicatorPoint{150, 42, 44, 1, 7, 3, 1});
  engine.strategySpread.push_back(JsonStrategySpreadPoint{150, 100000000, 90000000, 10000000, 5000000, 5000000, 1, 3, 0});

  const hft_backtest::Status status = writeResult(result, engine);
  const std::string manifest = readText(dir / "manifest.json");
  const std::string orderLifetimes = readText(dir / "order_lifetimes.jsonl");
  const std::string fills = readText(dir / "fills.jsonl");
  const std::string equity = readText(dir / "equity.jsonl");
  const std::string risk = readText(dir / "risk_snapshots.jsonl");
  const std::string indicator = readText(dir / "strategy_indicator.jsonl");
  const std::string spread = readText(dir / "strategy_spread.jsonl");

  return expect(status == hft_backtest::Status::Ok, "v2 writer reports ok") &&
         expect(std::filesystem::is_directory(dir), "result path is a directory") &&
         expect(manifest.find("\"type\": \"run.result.v2\"") != std::string::npos, "manifest declares v2 result") &&
         expect(manifest.find("\"orders\": 1") != std::string::npos, "manifest summary includes order count") &&
         expect(manifest.find("\"order_lifetimes\": {") != std::string::npos, "manifest declares order lifetimes stream") &&
         expect(manifest.find("\"risk_snapshots\": {") != std::string::npos, "manifest declares risk snapshots stream") &&
         expect(manifest.find("\"strategy_indicator\": {") != std::string::npos, "manifest declares strategy indicator stream") &&
         expect(manifest.find("\"strategy_spread\": {") != std::string::npos, "manifest declares strategy spread stream") &&
         expect(manifest.find("\"warnings\": [{\"message\": \"partial depth replay: dropped 1 unpaired depth_tape rows; exact replay lost\"}]") != std::string::npos,
                "manifest saves run warnings separately from errors") &&
         expect(manifest.find("\"profile\": \"trend_score\"") != std::string::npos, "indicator stream carries profile") &&
         expect(manifest.find("\"aux_label\": \"Raw\"") != std::string::npos, "trend score aux line is raw score") &&
         expect(manifest.find("\"live_probe\": {") != std::string::npos, "manifest carries live probe metadata") &&
         expect(manifest.find("\"mode\": \"ignored_backtest\"") != std::string::npos, "manifest marks live probe ignored in backtest") &&
         expect(manifest.find("\"notional_usdt_e8\": 1000000000") != std::string::npos, "manifest carries live probe notional") &&
         expect(manifest.find("\"market_data_latency\": {\"base_us\": 250, \"jitter_us\": 100}") != std::string::npos,
                "manifest leg summary carries per-leg market data latency") &&
         expect(orderLifetimes == "[110,150,10000000000,100000000,1,0,0]\n", "order lifetimes stream uses compact arrays") &&
         expect(!std::filesystem::exists(dir / "orders.jsonl"), "writer does not emit raw orders stream") &&
         expect(fills == "[1,110,150,1,10000000000,100000000,0,0,0]\n", "fills stream uses compact arrays") &&
         expect(equity == "[150,0,0,0,0,0,0,0,100000000000,100000000,1]\n", "equity stream uses compact arrays") &&
         expect(risk == "[150,0,0,0,0,0,0,100000000000,100000000,10000000000,9900000000,10100000000,1,2]\n", "risk stream uses compact arrays") &&
         expect(indicator == "[150,42,44,1,7,3,1]\n", "strategy indicator stream uses compact arrays") &&
         expect(spread == "[150,100000000,90000000,10000000,5000000,5000000,1,3,0]\n", "strategy spread stream uses compact arrays") &&
         expect(std::filesystem::exists(dir / "config.ini"), "writer copies run config into result directory") &&
         expect(!std::filesystem::exists(dir.parent_path() / "run-v2.summary.json"), "writer does not create legacy summary json");
}

bool testTrendProbeWritesTrendScoreIndicator() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_trend_probe_" + std::to_string(std::rand()));
  writeText(dir / "manifest.json",
            "{\n"
            "  \"identity\": {\"exchange\": \"binance\", \"market\": \"futures\", \"symbols\": [\"BTCUSDT\"]},\n"
            "  \"channels\": {\n"
            "    \"trades\": {\"path\": \"jsonl/trades.jsonl\"},\n"
            "    \"bookticker\": {\"path\": \"jsonl/bookticker.jsonl\"}\n"
            "  },\n"
            "  \"artifacts\": {\"instrument_metadata_path\": \"instrument_metadata.json\"}\n"
            "}\n");
  writeText(dir / "instrument_metadata.json", "{\"tick_size_e8\":1000000,\"lot_size_e8\":1000,\"contract_base_qty_e8\":100000000}\n");
  std::string trades;
  std::string bookticker;
  for (std::uint64_t second = 1u; second <= 650u; ++second) {
    const std::int64_t px = 10000000000ll + static_cast<std::int64_t>(second) * 1000000ll;
    const std::uint64_t ts = second * 1000000000ull;
    trades += "[" + std::to_string(px) + ",100000000,1," + std::to_string(ts) + ",1,1,1," +
              std::to_string(px) + ",0,\"BTCUSDT\",\"binance\",\"futures\",1,1]\n";
    bookticker += "[" + std::to_string(px - 1000000ll) + ",100000000," + std::to_string(px + 1000000ll) +
                  ",100000000," + std::to_string(ts) + ",1,1]\n";
  }
  writeText(dir / "jsonl" / "trades.jsonl", trades);
  writeText(dir / "jsonl" / "bookticker.jsonl", bookticker);
  writeText(dir / "trend_probe.ini",
            "[strategy]\n"
            "type=trend_probe\n"
            "enabled=1\n"
            "[live_probe]\n"
            "enabled=true\n"
            "notional_usdt=10\n"
            "timeout_ms=1000\n"
            "limit_offset_spread_pct=5\n"
            "[venue.binance_futures]\n"
            "api_slot=1\n"
            "symbols=BTCUSDT\n");

  hft_backtest::BacktestRunRequest request{};
  request.sessionPath = dir;
  request.configPath = dir / "trend_probe.ini";
  request.strategy = "trend_probe";
  request.indicatorProfile = "trend_score";
  request.runId = "trend-probe";
  request.outputPath = dir / "backtests" / "trend-probe";
  const hft_backtest::BacktestRunResult result = hft_backtest::runBacktest(request, nullptr, nullptr);
  const std::string manifest = readText(request.outputPath / "manifest.json");
  const std::string indicator = readText(request.outputPath / "strategy_indicator.jsonl");

  return expect(result.status == hft_backtest::Status::Ok, "trend_probe backtest completes") &&
         expect(result.liveProbe.configured, "backtest result carries configured live probe") &&
         expect(result.liveProbe.enabled, "backtest result carries enabled live probe") &&
         expect(result.liveProbe.ignored, "backtest marks live probe ignored") &&
         expect(result.liveProbe.notionalUsdtE8 == 1000000000ll, "backtest result carries live probe notional") &&
         expect(result.inputCoverage.strategyCycles > 0u, "trend_probe was invoked") &&
         expect(result.inputCoverage.cyclesWithTrade > 0u, "trend_probe sees trades") &&
         expect(result.inputCoverage.cyclesWithBookTicker > 0u, "trend_probe sees bookticker") &&
         expect(manifest.find("\"live_probe\": {") != std::string::npos, "trend_probe manifest carries live probe metadata") &&
         expect(manifest.find("\"mode\": \"ignored_backtest\"") != std::string::npos, "trend_probe manifest marks live probe ignored") &&
         expect(manifest.find("\"strategy_indicator\": {") != std::string::npos, "manifest declares strategy indicator stream") &&
         expect(manifest.find("\"profile\": \"trend_score\"") != std::string::npos, "manifest carries trend score profile") &&
         expect(manifest.find("\"aux_label\": \"Raw\"") != std::string::npos, "manifest carries raw trend aux label") &&
         expect(!indicator.empty(), "trend score indicator rows are written") &&
         expect(lineCount(indicator) <= 650u, "trend score indicator is aggregated to one row per second") &&
         expect(lineCount(indicator) < result.inputCoverage.strategyCycles, "trend score indicator no longer mirrors every tick");
}

bool testVolatilityProbeWritesVolatilityIndicator() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_volatility_probe_" + std::to_string(std::rand()));
  writeText(dir / "manifest.json",
            "{\n"
            "  \"identity\": {\"exchange\": \"binance\", \"market\": \"futures\", \"symbols\": [\"BTCUSDT\"]},\n"
            "  \"channels\": {\n"
            "    \"trades\": {\"path\": \"jsonl/trades.jsonl\"}\n"
            "  },\n"
            "  \"artifacts\": {\"instrument_metadata_path\": \"instrument_metadata.json\"}\n"
            "}\n");
  writeText(dir / "instrument_metadata.json", "{\"tick_size_e8\":1000000,\"lot_size_e8\":1000,\"contract_base_qty_e8\":100000000}\n");
  std::string trades;
  for (std::uint64_t second = 1u; second <= 700u; ++second) {
    const std::int64_t px = 10000000000ll + static_cast<std::int64_t>(second % 2u == 0u ? 30000000ll : -30000000ll);
    const std::int64_t nextPx = 10000000000ll + static_cast<std::int64_t>(second % 2u == 0u ? -30000000ll : 30000000ll);
    const std::uint64_t ts = second * 1000000000ull;
    trades += "[" + std::to_string(px) + ",100000000,1," + std::to_string(ts) + ",1,1,1," +
              std::to_string(px) + ",0,\"BTCUSDT\",\"binance\",\"futures\",1,1]\n";
    trades += "[" + std::to_string(nextPx) + ",100000000,1," + std::to_string(ts + 1u) + ",1,1,1," +
              std::to_string(nextPx) + ",0,\"BTCUSDT\",\"binance\",\"futures\",1,1]\n";
  }
  writeText(dir / "jsonl" / "trades.jsonl", trades);
  writeText(dir / "volatility_probe.ini",
            "[strategy]\n"
            "type=volatility_probe\n"
            "enabled=1\n"
            "[venue.binance_futures]\n"
            "api_slot=1\n"
            "symbols=BTCUSDT\n");

  hft_backtest::BacktestRunRequest request{};
  request.sessionPath = dir;
  request.configPath = dir / "volatility_probe.ini";
  request.strategy = "volatility_probe";
  request.indicatorProfile = "volatility_bps";
  request.runId = "volatility-probe";
  request.outputPath = dir / "backtests" / "volatility-probe";
  const hft_backtest::BacktestRunResult result = hft_backtest::runBacktest(request, nullptr, nullptr);
  const std::string manifest = readText(request.outputPath / "manifest.json");
  const std::string indicator = readText(request.outputPath / "strategy_indicator.jsonl");

  return expect(result.status == hft_backtest::Status::Ok, "volatility_probe backtest completes") &&
         expect(result.inputCoverage.strategyCycles > 0u, "volatility_probe was invoked") &&
         expect(result.inputCoverage.cyclesWithTrade > 0u, "volatility_probe sees trades") &&
         expect(manifest.find("\"strategy_indicator\": {") != std::string::npos, "manifest declares volatility indicator stream") &&
         expect(manifest.find("\"profile\": \"volatility_bps\"") != std::string::npos, "manifest carries volatility profile") &&
         expect(manifest.find("\"value_label\": \"EMA\"") != std::string::npos, "manifest carries volatility value label") &&
         expect(manifest.find("\"aux_label\": \"Sample\"") != std::string::npos, "manifest carries volatility aux label") &&
         expect(!indicator.empty(), "volatility indicator rows are written") &&
         expect(lineCount(indicator) <= 700u, "volatility indicator is aggregated to one row per second") &&
         expect(lineCount(indicator) < result.inputCoverage.strategyCycles, "volatility indicator no longer mirrors every tick");
}

bool testToxicFlowProbeWritesDepthBackedIndicator() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_toxic_flow_probe_" + std::to_string(std::rand()));
  writeText(dir / "manifest.json",
            "{\n"
            "  \"identity\": {\"exchange\": \"binance\", \"market\": \"futures\", \"symbols\": [\"BTCUSDT\"]},\n"
            "  \"channels\": {\n"
            "    \"trades\": {\"path\": \"jsonl/trades.jsonl\"},\n"
            "    \"bookticker\": {\"path\": \"jsonl/bookticker.jsonl\"},\n"
            "    \"depth\": {\"path\": \"jsonl/depth.jsonl\"}\n"
            "  },\n"
            "  \"artifacts\": {\"instrument_metadata_path\": \"instrument_metadata.json\"}\n"
            "}\n");
  writeText(dir / "instrument_metadata.json", "{\"tick_size_e8\":1000000,\"lot_size_e8\":1000,\"contract_base_qty_e8\":100000000}\n");
  std::string trades;
  std::string bookticker;
  std::string depth;
  for (std::uint64_t second = 1u; second <= 45u; ++second) {
    const std::int64_t bid = 10000000000ll + static_cast<std::int64_t>(second) * 1000000ll;
    const std::int64_t ask = 10100000000ll + static_cast<std::int64_t>(second) * 1000000ll;
    const std::uint64_t ts = second * 1000000000ull;
    trades += "[" + std::to_string(ask) + ",100000000,1," + std::to_string(ts) + ",1,1,1," +
              std::to_string(ask) + ",0,\"BTCUSDT\",\"binance\",\"futures\",1,1]\n";
    bookticker += "[" + std::to_string(bid) + ",100000000," + std::to_string(ask) +
                  ",100000000," + std::to_string(ts) + ",1,1]\n";
    depth += "[[" + std::to_string(bid) + ",100000000,0],[" + std::to_string(ask) + ",100000000,1]," +
             std::to_string(ts) + ",1]\n";
  }
  writeText(dir / "jsonl" / "trades.jsonl", trades);
  writeText(dir / "jsonl" / "bookticker.jsonl", bookticker);
  writeText(dir / "jsonl" / "depth.jsonl", depth);
  writeText(dir / "toxic_flow_probe.ini",
            "[strategy]\n"
            "type=toxic_flow_probe\n"
            "enabled=1\n"
            "[venue.binance_futures]\n"
            "api_slot=1\n"
            "symbols=BTCUSDT\n");

  hft_backtest::BacktestRunRequest request{};
  request.sessionPath = dir;
  request.configPath = dir / "toxic_flow_probe.ini";
  request.strategy = "toxic_flow_probe";
  request.indicatorProfile = "toxic_flow";
  request.runId = "toxic-flow-probe";
  request.outputPath = dir / "backtests" / "toxic-flow-probe";
  const hft_backtest::BacktestRunResult result = hft_backtest::runBacktest(request, nullptr, nullptr);
  const std::string manifest = readText(request.outputPath / "manifest.json");
  const std::string indicator = readText(request.outputPath / "strategy_indicator.jsonl");

  return expect(result.status == hft_backtest::Status::Ok, "toxic_flow_probe backtest completes") &&
         expect(result.inputCoverage.strategyCycles > 0u, "toxic_flow_probe was invoked") &&
         expect(result.inputCoverage.cyclesWithTrade > 0u, "toxic_flow_probe sees trades") &&
         expect(result.inputCoverage.cyclesWithBookTicker > 0u, "toxic_flow_probe sees bookticker") &&
         expect(result.inputCoverage.cyclesWithOrderBook > 0u, "toxic_flow_probe sees orderbook") &&
         expect(manifest.find("\"strategy_indicator\": {") != std::string::npos, "manifest declares toxic flow indicator stream") &&
         expect(manifest.find("\"profile\": \"toxic_flow\"") != std::string::npos, "manifest carries toxic flow profile") &&
         expect(manifest.find("\"aux_label\": \"Raw\"") != std::string::npos, "manifest carries raw toxic aux label") &&
         expect(!indicator.empty(), "toxic flow indicator rows are written") &&
         expect(indicator.find("[") == 0u, "toxic flow indicator uses compact rows") &&
         expect(lineCount(indicator) <= 45u, "toxic flow indicator is aggregated to one row per second");
}

bool metadataHasParam(const hft_backtest::StrategyMetadata& metadata, std::string_view key) noexcept {
  for (std::size_t i = 0; i < metadata.paramCount; ++i) {
    if (metadata.params[i].key && key == metadata.params[i].key) return true;
  }
  return false;
}

bool metadataParamDefaultIs(const hft_backtest::StrategyMetadata& metadata,
                            std::string_view key,
                            std::string_view expected) noexcept {
  for (std::size_t i = 0; i < metadata.paramCount; ++i) {
    if (metadata.params[i].key && key == metadata.params[i].key) {
      return metadata.params[i].defaultValue && expected == metadata.params[i].defaultValue;
    }
  }
  return false;
}

bool testStrategyMetadataMirrorsTraderRegistry() {
  const std::size_t descriptorCount = hft_trader::runtime::strategyDescriptorCount();
  if (!expect(hft_backtest::strategyMetadataCount() == descriptorCount, "metadata count mirrors trader registry")) return false;
  for (std::size_t i = 0; i < descriptorCount; ++i) {
    const hft_trader::runtime::StrategyDescriptor* descriptor = hft_trader::runtime::strategyDescriptorAt(i);
    const hft_backtest::StrategyMetadata* metadata = hft_backtest::strategyMetadataAt(i);
    if (!expect(descriptor != nullptr && descriptor->name != nullptr, "registry descriptor has a name")) return false;
    if (!expect(metadata != nullptr && metadata->id != nullptr, "metadata row exists for registry descriptor")) return false;
    if (!expect(std::string_view{metadata->id} == descriptor->name, "metadata id matches registry descriptor")) return false;
    if (!expect(hft_backtest::findStrategyMetadata(descriptor->name) == metadata, "metadata lookup finds registry strategy")) return false;
  }
  return expect(hft_backtest::findStrategyMetadata("removed_strategy") == nullptr, "unknown metadata is hidden");
}

bool testStrategyMetadataExposesOnlySupportedSpreadMakerParams() {
  const hft_backtest::StrategyMetadata* metadata = hft_backtest::findStrategyMetadata("spread_maker1and2");
  return expect(metadata != nullptr, "spread_maker1and2 metadata is exported") &&
         expect(metadata->configTemplateFile != nullptr && std::string_view{metadata->configTemplateFile} == "1and2.ini",
                "spread_maker1and2 metadata discovers 1and2.ini") &&
         expect(metadataHasParam(*metadata, "distance_bps"), "spread metadata contains distance_bps") &&
         expect(metadataHasParam(*metadata, "trigger_bps"), "spread metadata contains trigger_bps") &&
         expect(metadataHasParam(*metadata, "amount_qty"), "spread metadata contains amount_qty") &&
         expect(metadataHasParam(*metadata, "order_qty"), "spread metadata contains order_qty") &&
         expect(metadataParamDefaultIs(*metadata, "amount_qty", "25"), "spread amount_qty default comes from ini") &&
         expect(metadataParamDefaultIs(*metadata, "order_qty", "0.001"), "spread order_qty default comes from commented ini template") &&
         expect(!metadataHasParam(*metadata, "fixed quote distance in basis points. 1 bps"),
                "spread metadata ignores prose comments with equals") &&
         expect(!metadataHasParam(*metadata, "minimum interval between quote refreshes in milliseconds. 0"),
                "spread metadata ignores prose comments before refresh_ms") &&
         expect(!metadataHasParam(*metadata, "min_equity_pct"),
                "spread metadata does not treat commented risk template as a strategy param") &&
         expect(!metadataHasParam(*metadata, "distance_natr_pct"), "spread metadata does not expose unsupported distance_natr_pct") &&
         expect(!metadataHasParam(*metadata, "trigger_natr_pct"), "spread metadata does not expose unsupported trigger_natr_pct") &&
         expect(!metadataHasParam(*metadata, "natr_ema_period_seconds"), "spread metadata does not expose unsupported natr_ema_period_seconds") &&
         expect(metadata->indicatorCount == 0u, "spread metadata declares no indicator profiles");
}

bool testStatArbBandLadderMetadataRequiresTwoSessions() {
  const hft_backtest::StrategyMetadata* metadata = hft_backtest::findStrategyMetadata("stat_arb_band_ladder");
  return expect(metadata != nullptr, "stat_arb_band_ladder metadata is exported") &&
         expect(metadata->configTemplateFile != nullptr && std::string_view{metadata->configTemplateFile} == "stat_arb_band_ladder.ini",
                "stat arb band ladder discovers stat_arb_band_ladder.ini") &&
         expect(metadata->minSessionCount == 2u, "stat arb band ladder requires at least two sessions") &&
         expect(metadata->maxSessionCount == 2u, "stat arb band ladder allows exactly two sessions") &&
         expect(metadataHasParam(*metadata, "ema_window_ms"), "stat arb band ladder metadata contains ema_window_ms") &&
         expect(metadataHasParam(*metadata, "entry_edge_bps"), "stat arb band ladder metadata contains entry_edge_bps") &&
         expect(metadataHasParam(*metadata, "exit_edge_bps"), "stat arb band ladder metadata contains exit_edge_bps") &&
         expect(!metadataHasParam(*metadata, "pending_timeout_ms"), "stat arb band ladder metadata hides pending_timeout_ms") &&
         expect(!metadataHasParam(*metadata, "max_quote_age_ms"), "stat arb band ladder metadata hides max_quote_age_ms") &&
         expect(!metadataHasParam(*metadata, "max_leg_skew_ms"), "stat arb band ladder metadata hides max_leg_skew_ms") &&
         expect(metadataHasParam(*metadata, "ladder_step_bps"), "stat arb band ladder metadata exposes ladder_step_bps") &&
         expect(metadataHasParam(*metadata, "max_ladder_levels"), "stat arb band ladder metadata exposes max_ladder_levels") &&
         expect(metadataParamDefaultIs(*metadata, "max_ladder_levels", "3"), "ladder levels default comes from ini") &&
         expect(metadataHasParam(*metadata, "amount_qty"), "stat arb band ladder metadata contains amount_qty") &&
         expect(metadataHasParam(*metadata, "order_qty"), "stat arb band ladder metadata contains order_qty") &&
         expect(metadataParamDefaultIs(*metadata, "order_qty", "0.001"), "ladder order_qty default comes from commented ini template");
}

bool testTrendProbeMetadataExposesTrendScoreIndicator() {
  const hft_backtest::StrategyMetadata* metadata = hft_backtest::findStrategyMetadata("trend_probe");
  return expect(metadata != nullptr, "trend_probe metadata is exported") &&
         expect(metadata->configTemplateFile != nullptr && std::string_view{metadata->configTemplateFile} == "trend_probe.ini",
                "trend_probe metadata discovers trend_probe.ini") &&
         expect(metadata->minSessionCount == 1u, "trend_probe uses one session") &&
         expect(metadata->maxSessionCount == 1u, "trend_probe allows one session") &&
         expect(metadata->paramCount == 0u, "trend_probe has no strategy params") &&
         expect(metadataHasIndicator(*metadata, "trend_score"), "trend_probe exposes trend_score indicator");
}

bool testVolatilityProbeMetadataExposesVolatilityIndicator() {
  const hft_backtest::StrategyMetadata* metadata = hft_backtest::findStrategyMetadata("volatility_probe");
  return expect(metadata != nullptr, "volatility_probe metadata is exported") &&
         expect(metadata->configTemplateFile != nullptr && std::string_view{metadata->configTemplateFile} == "volatility_probe.ini",
                "volatility_probe metadata discovers volatility_probe.ini") &&
         expect(metadata->minSessionCount == 1u, "volatility_probe uses one session") &&
         expect(metadata->maxSessionCount == 1u, "volatility_probe allows one session") &&
         expect(metadata->paramCount == 0u, "volatility_probe has no strategy params") &&
         expect(metadataHasIndicator(*metadata, "volatility_bps"), "volatility_probe exposes volatility_bps indicator");
}

bool testToxicFlowProbeMetadataRequiresDepthIndicator() {
  const hft_backtest::StrategyMetadata* metadata = hft_backtest::findStrategyMetadata("toxic_flow_probe");
  return expect(metadata != nullptr, "toxic_flow_probe metadata is exported") &&
         expect(metadata->configTemplateFile != nullptr && std::string_view{metadata->configTemplateFile} == "toxic_flow_probe.ini",
                "toxic_flow_probe metadata discovers toxic_flow_probe.ini") &&
         expect(metadata->minSessionCount == 1u, "toxic_flow_probe uses one session") &&
         expect(metadata->maxSessionCount == 1u, "toxic_flow_probe allows one session") &&
         expect(metadata->paramCount == 0u, "toxic_flow_probe has no strategy params") &&
         expect(metadataHasIndicator(*metadata, "toxic_flow"), "toxic_flow_probe exposes toxic_flow indicator");
}
}  // namespace

int main() {
  bool ok = true;
  ok = testBacktestCanonicalizesAsterSessionExchange() && ok;
  ok = testStaleCancelDoesNotCancelReusedSlot() && ok;
  ok = testPendingDrainActivatesLateOrder() && ok;
  ok = testCancelThenReplacementLimitMaterializesReplace() && ok;
  ok = testLatencySamplerIsReproducibleAndDeterministicWithoutJitter() && ok;
  ok = testLatencyRuntimeSplitsMarketAndLimitOrderActivation() && ok;
  ok = testFillPublishesRuntimeUserEvent() && ok;
  ok = testPostOnlyCrossingBuyRejectsWithoutTakerFill() && ok;
  ok = testPostOnlyCrossingSellRejectsWithoutTakerFill() && ok;
  ok = testPostOnlyRestingOrderDoesNotFillOnTopButFillsOnTrade() && ok;
  ok = testGtcCrossingLimitStillTakerFills() && ok;
  ok = testReduceOnlyMarketBuyDoesNotOpenWhenFlat() && ok;
  ok = testReduceOnlyMarketSellClosesLongAtBid() && ok;
  ok = testReduceOnlyMarketBuyClosesShortAtAsk() && ok;
  ok = testFeesReduceWalletAndNetPnl() && ok;
  ok = testEquityPointCapturesRealizedOnlyPnl() && ok;
  ok = testRiskSnapshotCapturesCompactMarkToMarketPnl() && ok;
  ok = testRiskSnapshotCadenceLimitsOpenPositionMtmRows() && ok;
  ok = testHardLiquidationClearsStateAndBlocksNewOrders() && ok;
  ok = testTraderRiskStopCancelsOrdersKeepsPositionAndBlocksNewOrders() && ok;
  ok = testForceCloseOpenShortPositionSubmitsReduceOnlyMarketBuy() && ok;
  ok = testCancelOpenOrdersAtFinishWritesCancelAcks() && ok;
  ok = testLoadSessionUsesManifestChannelPathsAndMetadata() && ok;
  ok = testLoadSessionFailsMissingRequiredDepth() && ok;
  ok = testLoadSessionPrefersDepthTapeSidecar() && ok;
  ok = testLoadSessionKeepsDepthTapeSidecarPrefixWhenTapeTailIsUnpaired() && ok;
  ok = testLoadSessionResyncsDepthTapeSidecarAfterMissingMiddleSidecarRow() && ok;
  ok = testLoadSessionSkipsCorruptMiddleDepthTapeSidecarPair() && ok;
  ok = testLoadSessionRejectsDepthTapeWithoutTaggedTimestamp() && ok;
  ok = testDepthRowBuildsInitialOrderBookSnapshot() && ok;
  ok = testTieredCandleHistoryBuildsCounts() && ok;
  ok = testWriterCreatesV2RunDirectory() && ok;
  ok = testTrendProbeWritesTrendScoreIndicator() && ok;
  ok = testVolatilityProbeWritesVolatilityIndicator() && ok;
  ok = testToxicFlowProbeWritesDepthBackedIndicator() && ok;
  ok = testStrategyMetadataMirrorsTraderRegistry() && ok;
  ok = testStrategyMetadataExposesOnlySupportedSpreadMakerParams() && ok;
  ok = testStatArbBandLadderMetadataRequiresTwoSessions() && ok;
  ok = testTrendProbeMetadataExposesTrendScoreIndicator() && ok;
  ok = testVolatilityProbeMetadataExposesVolatilityIndicator() && ok;
  ok = testToxicFlowProbeMetadataRequiresDepthIndicator() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
