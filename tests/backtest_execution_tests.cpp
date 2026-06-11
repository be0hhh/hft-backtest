#include "backtest_engine.hpp"

#include "backtest_latency.hpp"

#include "hft_trader/api/UserDataTools.hpp"

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
  engine.venueSnapshot.lastBookTicker.bid.px.raw = bid;
  engine.venueSnapshot.lastBookTicker.ask.px.raw = ask;
}

bool expect(bool value, std::string_view message) {
  if (value) return true;
  std::cerr << "FAIL: " << message << "\n";
  return false;
}

std::string readText(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::in | std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
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
         expect(engine.orders[0].status == kRecordStatusFilled, "original order remains filled");
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
  return expect(engine.orders[0].status == kRecordStatusPending, "late order initially pending") &&
         (applyDueActions(engine, 101000), true) &&
         expect(engine.orders[0].status == kRecordStatusActive, "final drain activates due order");
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

bool testReduceOnlyMarketBuyDoesNotOpenWhenFlat() {
  EngineState engine{};
  setTop(engine, kPx100 - 300000000ll, kPx100 - 200000000ll);
  std::array<hft_trader::core::OrderIntent, 1> orders{reduceMarketIntent(7, Side::Buy())};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;
  applyIntents(engine, io, 200, 0, 0, 0);
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
  applyIntents(engine, io, 1000, 0, 0, 0);

  cancelOpenOrders(engine, 2000, 100);

  return expect(engine.orders.size() == 2u, "final cleanup writes cancel command") &&
         expect(engine.orders[0].status == kRecordStatusCancelled, "final cleanup cancels active quote row") &&
         expect(engine.orders[1].action == kRecordActionCancel, "final cleanup uses cancel action") &&
         expect(engine.orders[1].targetId == 1u, "final cleanup targets live client intent id") &&
         expect(engine.orders[1].status == kRecordStatusAcked, "final cleanup cancel is acked") &&
         expect(engine.orders[1].activeTsNs == 102000, "final cleanup honors cancel latency") &&
         expect(!engine.open[0].state.valid, "final cleanup clears open order state");
}
bool testAmendFillBelongsToReplacementOrder() {
  EngineState engine{};
  setTop(engine, kPx100 - 200000000ll, kPx100 + 200000000ll);
  std::array<hft_trader::core::OrderIntent, 1> orders{orderIntent(1, Side::Buy(), kPx100 - 300000000ll)};
  std::array<hft_trader::core::AmendIntent, 1> amends{amendIntent(2, 1, kPx100 - 100000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;
  applyIntents(engine, io, 1000, 0, 0, 0);
  io.clear();
  io.amendOut = Span<hft_trader::core::AmendIntent>(amends.data(), amends.size());
  io.amendCount = 1;
  applyIntents(engine, io, 2000, 0, 100, 0);
  applyDueActions(engine, 102000);
  cxet::composite::TradeRuntimeV1 trade{};
  trade.ts.raw = 150000;
  trade.side = Side::Sell();
  trade.price.raw = kPx100 - 100000000ll;
  tryFillOnTrade(engine, trade);

  return expect(engine.orders.size() == 3u, "amend scenario records order cancel and replacement") &&
         expect(engine.orders[1].action == kRecordActionCancel, "replace writes cancel leg") &&
         expect(engine.orders[2].action == kRecordActionOrder, "replace writes new order leg") &&
         expect(engine.fills.size() == 1u, "replacement order fills once") &&
         expect(engine.orders[0].status == kRecordStatusCancelled, "original order is cancelled by replace") &&
         expect(engine.orders[1].status == kRecordStatusAcked, "replace cancel leg is acked") &&
         expect(engine.orders[2].status == kRecordStatusFilled, "replacement row is marked filled") &&
         expect(engine.fills[0].orderId == 2u, "fill belongs to replacement client intent id") &&
         expect(engine.fills[0].entryTsNs == 102000, "replacement entry uses amend latency without second order latency");
}

bool testAmendChainFillBelongsToLastReplacementOrder() {
  EngineState engine{};
  setTop(engine, kPx100 - 300000000ll, kPx100 + 300000000ll);
  std::array<hft_trader::core::OrderIntent, 1> orders{orderIntent(10, Side::Buy(), kPx100 - 400000000ll)};
  std::array<hft_trader::core::AmendIntent, 2> amends{amendIntent(11, 10, kPx100 - 200000000ll), amendIntent(12, 11, kPx100 - 100000000ll)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;
  applyIntents(engine, io, 1000, 0, 0, 0);
  io.clear();
  io.amendOut = Span<hft_trader::core::AmendIntent>(&amends[0], 1);
  io.amendCount = 1;
  applyIntents(engine, io, 2000, 0, 100, 0);
  applyDueActions(engine, 102000);
  io.clear();
  io.amendOut = Span<hft_trader::core::AmendIntent>(&amends[1], 1);
  io.amendCount = 1;
  applyIntents(engine, io, 120000, 0, 100, 0);
  applyDueActions(engine, 220000);
  cxet::composite::TradeRuntimeV1 trade{};
  trade.ts.raw = 250000;
  trade.side = Side::Sell();
  trade.price.raw = kPx100 - 100000000ll;
  tryFillOnTrade(engine, trade);

  return expect(engine.orders.size() == 5u, "amend chain records cancel and new order legs") &&
         expect(engine.orders[1].action == kRecordActionCancel, "first replace writes cancel leg") &&
         expect(engine.orders[2].action == kRecordActionOrder, "first replace writes new order leg") &&
         expect(engine.orders[3].action == kRecordActionCancel, "second replace writes cancel leg") &&
         expect(engine.orders[4].action == kRecordActionOrder, "second replace writes new order leg") &&
         expect(engine.fills.size() == 1u, "last replacement fills once") &&
         expect(engine.orders[0].status == kRecordStatusCancelled, "original chain row is cancelled by first replace") &&
         expect(engine.orders[2].status == kRecordStatusCancelled, "intermediate replacement row is cancelled by second replace") &&
         expect(engine.orders[4].status == kRecordStatusFilled, "last replacement row is filled") &&
         expect(engine.fills[0].orderId == 12u, "fill belongs to last replacement id");
}

bool testCancelAfterAmendCancelsReplacementOrder() {
  EngineState engine{};
  setTop(engine, kPx100 - 300000000ll, kPx100 + 300000000ll);
  std::array<hft_trader::core::OrderIntent, 1> orders{orderIntent(20, Side::Buy(), kPx100 - 400000000ll)};
  std::array<hft_trader::core::AmendIntent, 1> amends{amendIntent(21, 20, kPx100 - 200000000ll)};
  std::array<hft_trader::core::CancelIntent, 1> cancels{cancelIntent(22, 21)};
  hft_trader::core::StrategyIo io{};
  io.orderOut = Span<hft_trader::core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = 1;
  applyIntents(engine, io, 1000, 0, 0, 0);
  io.clear();
  io.amendOut = Span<hft_trader::core::AmendIntent>(amends.data(), amends.size());
  io.amendCount = 1;
  applyIntents(engine, io, 2000, 0, 100, 0);
  applyDueActions(engine, 102000);
  io.clear();
  io.cancelOut = Span<hft_trader::core::CancelIntent>(cancels.data(), cancels.size());
  io.cancelCount = 1;
  applyIntents(engine, io, 120000, 0, 0, 100);
  applyDueActions(engine, 220000);

  return expect(engine.orders.size() == 4u, "cancel after amend records order replace cancel and new order cancel") &&
         expect(engine.orders[0].status == kRecordStatusCancelled, "original order is cancelled by replace") &&
         expect(engine.orders[1].status == kRecordStatusAcked, "replace cancel leg is acked") &&
         expect(engine.orders[2].status == kRecordStatusCancelled, "replacement order is terminal-cancelled") &&
         expect(engine.orders[3].status == kRecordStatusAcked, "cancel command is acked") &&
         expect(!engine.open[0].state.valid, "replacement cancel clears active order");
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
            "    \"depth\": {\"path\": \"jsonl/depth.jsonl\"},\n"
            "    \"candles\": {\"path\": \"jsonl/candles.jsonl\"}\n"
            "  },\n"
            "  \"artifacts\": {\"instrument_metadata_path\": \"instrument_metadata.json\"}\n"
            "}\n");
  writeText(dir / "instrument_metadata.json", "{\"tick_size_e8\":1000000,\"lot_size_e8\":1000}\n");
  writeText(dir / "jsonl" / "trades.jsonl", "[10000000000,100000000,1,1000]\n");
  writeText(dir / "jsonl" / "bookticker.jsonl", "[9900000000,100000000,10100000000,100000000,1001]\n");
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

bool testDepthRowBuildsInitialOrderBookSnapshot() {
  DepthRow row{};
  row.tsNs = 1002;
  row.levels.push_back(PriceLevelRow{9900000000ll, 100000000ll, 1});
  row.levels.push_back(PriceLevelRow{10100000000ll, 200000000ll, 0});
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
  result.indicatorProfile = "trade_range";
  result.sessionPath = dir.parent_path() / "session";
  result.configPath = dir / "source_config.ini";
  result.resultPath = dir;
  result.orderLatencyUs = 3000;
  result.amendLatencyUs = 3000;
  result.cancelLatencyUs = 3000;
  result.events = 2;
  result.trades = 1;
  result.bookTickers = 1;
  result.initialBalanceE8 = 100000000000ll;
  result.walletBalanceE8 = 100100000000ll;
  result.totalPnlE8 = 100000000ll;

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
  engine.strategyIndicatorProfile = "trade_range";
  engine.strategyIndicatorTitle = "Trade range";
  engine.strategyIndicatorValueLabel = "EMA";
  engine.strategyIndicatorAuxLabel = "Sample";
  engine.strategyIndicatorUnit = "bps";
  engine.strategyIndicator.push_back(JsonStrategyIndicatorPoint{150, 42, 44, 1, 7, 3, 1});

  const hft_backtest::Status status = writeResult(result, engine);
  const std::string manifest = readText(dir / "manifest.json");
  const std::string orders = readText(dir / "orders.jsonl");
  const std::string fills = readText(dir / "fills.jsonl");
  const std::string equity = readText(dir / "equity.jsonl");
  const std::string risk = readText(dir / "risk_snapshots.jsonl");
  const std::string indicator = readText(dir / "strategy_indicator.jsonl");

  return expect(status == hft_backtest::Status::Ok, "v2 writer reports ok") &&
         expect(std::filesystem::is_directory(dir), "result path is a directory") &&
         expect(manifest.find("\"type\": \"run.result.v2\"") != std::string::npos, "manifest declares v2 result") &&
         expect(manifest.find("\"orders\": 1") != std::string::npos, "manifest summary includes order count") &&
         expect(manifest.find("\"risk_snapshots\": {") != std::string::npos, "manifest declares risk snapshots stream") &&
         expect(manifest.find("\"strategy_indicator\": {") != std::string::npos, "manifest declares strategy indicator stream") &&
         expect(manifest.find("\"profile\": \"trade_range\"") != std::string::npos, "indicator stream carries profile") &&
         expect(orders == "[1,0,100,110,100,1,1,1,3,10000000000,100000000,0,0]\n", "orders stream uses compact arrays") &&
         expect(fills == "[1,110,150,1,10000000000,100000000,0,0,0]\n", "fills stream uses compact arrays") &&
         expect(equity == "[150,0,0,0,0,0,0,0,100000000000,100000000,1]\n", "equity stream uses compact arrays") &&
         expect(risk == "[150,0,0,0,0,0,0,100000000000,100000000,10000000000,9900000000,10100000000,1,2]\n", "risk stream uses compact arrays") &&
         expect(indicator == "[150,42,44,1,7,3,1]\n", "strategy indicator stream uses compact arrays") &&
         expect(std::filesystem::exists(dir / "config.ini"), "writer copies run config into result directory") &&
         expect(!std::filesystem::exists(dir.parent_path() / "run-v2.summary.json"), "writer does not create legacy summary json");
}

bool testBacktestProbeReportsFullInputCoverage() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_probe_full_" + std::to_string(std::rand()));
  writeText(dir / "manifest.json",
            "{\n"
            "  \"identity\": {\"exchange\": \"binance\", \"market\": \"futures\", \"symbols\": [\"BTCUSDT\"]},\n"
            "  \"channels\": {\n"
            "    \"trades\": {\"path\": \"jsonl/trades.jsonl\"},\n"
            "    \"bookticker\": {\"path\": \"jsonl/bookticker.jsonl\"},\n"
            "    \"depth\": {\"path\": \"jsonl/depth.jsonl\"},\n"
            "    \"candles\": {\"path\": \"jsonl/candles.jsonl\"}\n"
            "  },\n"
            "  \"artifacts\": {\"instrument_metadata_path\": \"instrument_metadata.json\"},\n"
            "  \"summary\": {\"trades\": {\"history_requested_start_ns\": 1, \"history_requested_end_ns\": 2500000000, \"history_rows\": 3}}\n"
            "}\n");
  writeText(dir / "instrument_metadata.json", "{\"tick_size_e8\":1000000,\"lot_size_e8\":1000,\"contract_base_qty_e8\":100000000}\n");
  writeText(dir / "snapshot_000.json", "[[9900000000,100000000,1],[10100000000,100000000,0],2500000000]\n");
  writeText(dir / "jsonl" / "trades.jsonl",
            "[10000000000,100000000,1,1500000000,1,1,1,10000000000,0,\"BTCUSDT\",\"binance\",\"futures\",1,1]\n"
            "[10020000000,100000000,0,1600000000,2,2,2,10020000000,0,\"BTCUSDT\",\"binance\",\"futures\",2,2]\n"
            "[10050000000,100000000,1,2100000000,3,3,3,10050000000,0,\"BTCUSDT\",\"binance\",\"futures\",3,3]\n"
            "[10060000000,100000000,1,3100000000,4,4,4,10060000000,0,\"BTCUSDT\",\"binance\",\"futures\",4,4]\n");
  writeText(dir / "jsonl" / "bookticker.jsonl", "[9900000000,100000000,10100000000,100000000,3000000000,3,3]\n");
  writeText(dir / "jsonl" / "depth.jsonl", "[[9900000000,100000000,1],[10100000000,100000000,0],2900000000,2,2]\n");
  writeText(dir / "jsonl" / "candles.jsonl",
            "[1,1000000000,10200000000,9800000000,500000000]\n"
            "[2,900000000,10300000000,9700000000,600000000]\n"
            "[3,800000000,10400000000,9600000000,700000000]\n");
  writeText(dir / "probe.ini",
            "[runtime]\nname=probe\nenv_path=.env\n"
            "[strategy]\ntype=backtest_probe\nenabled=1\n"
            "[venue.binance_futures]\napi_slot=1\nsymbols=BTCUSDT\n");

  hft_backtest::BacktestRunRequest request{};
  request.sessionPath = dir;
  request.configPath = dir / "probe.ini";
  request.strategy = "backtest_probe";
  request.runId = "probe-full";
  request.outputPath = dir / "backtests" / "probe-full";
  const hft_backtest::BacktestRunResult result = hft_backtest::runBacktest(request, nullptr, nullptr);
  const std::string manifest = readText(request.outputPath / "manifest.json");
  const std::string probeMetrics = readText(request.outputPath / "probe_metrics.jsonl");

  return expect(result.status == hft_backtest::Status::Ok, "probe backtest completes on full corpus") &&
         expect(result.orders > 0u, "probe strategy emits orders") &&
         expect(result.fills > 0u, "probe strategy receives simulated fills") &&
         expect(result.userEvents > 0u, "probe strategy receives user events") &&
         expect(result.timerWakeEvents > 0u, "probe strategy receives timer wakes") &&
         expect(result.strategyClosed, "probe strategy closes its position") &&
         expect(result.inputCoverage.strategyCycles > 0u, "probe strategy was invoked") &&
         expect(result.inputCoverage.cyclesWithTrade > 0u, "strategy context exposes trades") &&
         expect(result.inputCoverage.cyclesWithBookTicker > 0u, "strategy context exposes bookticker") &&
         expect(result.inputCoverage.cyclesWithDepth > 0u, "strategy context exposes depth") &&
         expect(result.inputCoverage.cyclesWithOrderBook > 0u, "strategy context exposes reconstructed orderbook") &&
         expect(result.inputCoverage.seedTradeRows == 3u, "probe reports seeded trade warmup rows") &&
         expect(result.inputCoverage.seedM1Candles == 1u, "probe reports m1 candle seed") &&
         expect(result.inputCoverage.seedM15Candles == 1u, "probe reports m15 candle seed") &&
         expect(result.inputCoverage.seedD1Candles == 1u, "probe reports d1 candle seed") &&
         expect(manifest.find("\"probe_metrics\"") != std::string::npos, "manifest declares probe metrics stream") &&
         expect(!probeMetrics.empty(), "probe metrics stream is written");
}

bool testBacktestProbeRequiresCalculatedTradeWarmup() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_probe_short_warmup_" + std::to_string(std::rand()));
  writeText(dir / "manifest.json",
            "{\"identity\":{\"exchange\":\"binance\",\"market\":\"futures\",\"symbols\":[\"BTCUSDT\"]},"
            "\"channels\":{\"trades\":{\"path\":\"jsonl/trades.jsonl\"},\"bookticker\":{\"path\":\"jsonl/bookticker.jsonl\"},\"depth\":{\"path\":\"jsonl/depth.jsonl\"},\"candles\":{\"path\":\"jsonl/candles.jsonl\"}},"
            "\"summary\":{\"trades\":{\"history_requested_start_ns\":1,\"history_requested_end_ns\":2500000000,\"history_rows\":1}}}\n");
  writeText(dir / "jsonl" / "trades.jsonl", "[10000000000,100000000,1,1500000000]\n[10050000000,100000000,1,2600000000]\n");
  writeText(dir / "jsonl" / "bookticker.jsonl", "[9900000000,100000000,10100000000,100000000,3000000000]\n");
  writeText(dir / "jsonl" / "depth.jsonl", "[[9900000000,100000000,1],[10100000000,100000000,0],2900000000]\n");
  writeText(dir / "jsonl" / "candles.jsonl",
            "[1,1000000000,10200000000,9800000000,500000000]\n"
            "[2,900000000,10300000000,9700000000,600000000]\n"
            "[3,800000000,10400000000,9600000000,700000000]\n");
  writeText(dir / "probe.ini",
            "[strategy]\ntype=backtest_probe\nenabled=1\n[venue.binance_futures]\napi_slot=1\nsymbols=BTCUSDT\n");

  hft_backtest::BacktestRunRequest request{};
  request.sessionPath = dir;
  request.configPath = dir / "probe.ini";
  request.strategy = "backtest_probe";
  request.runId = "probe-short-warmup";
  request.outputPath = dir / "backtests" / "probe-short-warmup";
  const hft_backtest::BacktestRunResult result = hft_backtest::runBacktest(request, nullptr, nullptr);

  return expect(result.status == hft_backtest::Status::CorruptData, "probe rejects warmup that cannot calculate range/volume") &&
         expect(result.error.find("warmup") != std::string::npos || result.error.find("seedTrades") != std::string::npos,
                "calculated warmup error names warmup seed");
}

bool testBacktestProbeRequiresCandleSeed() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_probe_no_candles_" + std::to_string(std::rand()));
  writeText(dir / "manifest.json",
            "{\"identity\":{\"exchange\":\"binance\",\"market\":\"futures\",\"symbols\":[\"BTCUSDT\"]},"
            "\"channels\":{\"trades\":{\"path\":\"jsonl/trades.jsonl\"},\"bookticker\":{\"path\":\"jsonl/bookticker.jsonl\"},\"depth\":{\"path\":\"jsonl/depth.jsonl\"}},"
            "\"summary\":{\"trades\":{\"history_requested_start_ns\":1000000000,\"history_requested_end_ns\":2000000000,\"history_rows\":1}}}\n");
  writeText(dir / "jsonl" / "trades.jsonl", "[10000000000,100000000,1,1500000000]\n[10050000000,100000000,1,3100000000]\n");
  writeText(dir / "jsonl" / "bookticker.jsonl", "[9900000000,100000000,10100000000,100000000,3000000000]\n");
  writeText(dir / "jsonl" / "depth.jsonl", "[[9900000000,100000000,1],[10100000000,100000000,0],2900000000]\n");
  writeText(dir / "probe.ini",
            "[strategy]\ntype=backtest_probe\nenabled=1\n[venue.binance_futures]\napi_slot=1\nsymbols=BTCUSDT\n");

  hft_backtest::BacktestRunRequest request{};
  request.sessionPath = dir;
  request.configPath = dir / "probe.ini";
  request.strategy = "backtest_probe";
  request.runId = "probe-no-candles";
  request.outputPath = dir / "backtests" / "probe-no-candles";
  const hft_backtest::BacktestRunResult result = hft_backtest::runBacktest(request, nullptr, nullptr);

  return expect(result.status == hft_backtest::Status::CorruptData, "probe rejects missing candle channel") &&
         expect(result.error.find("candles") != std::string::npos || result.error.find("candle") != std::string::npos,
                "missing candle error names candle channel");
}

bool testBacktestProbeRequiresAllCandleSeedTiers() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_probe_partial_candles_" + std::to_string(std::rand()));
  writeText(dir / "manifest.json",
            "{\n"
            "  \"identity\": {\"exchange\": \"binance\", \"market\": \"futures\", \"symbols\": [\"BTCUSDT\"]},\n"
            "  \"channels\": {\n"
            "    \"trades\": {\"path\": \"jsonl/trades.jsonl\"},\n"
            "    \"bookticker\": {\"path\": \"jsonl/bookticker.jsonl\"},\n"
            "    \"depth\": {\"path\": \"jsonl/depth.jsonl\"},\n"
            "    \"candles\": {\"path\": \"jsonl/candles.jsonl\"}\n"
            "  },\n"
            "  \"artifacts\": {\"instrument_metadata_path\": \"instrument_metadata.json\"},\n"
            "  \"summary\": {\"trades\": {\"history_requested_start_ns\": 1, \"history_requested_end_ns\": 2500000000, \"history_rows\": 3}}\n"
            "}\n");
  writeText(dir / "instrument_metadata.json", "{\"tick_size_e8\":1000000,\"lot_size_e8\":1000,\"contract_base_qty_e8\":100000000}\n");
  writeText(dir / "jsonl" / "trades.jsonl",
            "[10000000000,100000000,1,1500000000]\n"
            "[10020000000,100000000,0,1600000000]\n"
            "[10050000000,100000000,1,2100000000]\n"
            "[10060000000,100000000,1,3100000000]\n");
  writeText(dir / "jsonl" / "bookticker.jsonl", "[9900000000,100000000,10100000000,100000000,3000000000]\n");
  writeText(dir / "jsonl" / "depth.jsonl", "[[9900000000,100000000,1],[10100000000,100000000,0],2900000000]\n");
  writeText(dir / "jsonl" / "candles.jsonl", "[1,1000000000,10200000000,9800000000,500000000]\n");
  writeText(dir / "probe.ini",
            "[strategy]\ntype=backtest_probe\nenabled=1\n[venue.binance_futures]\napi_slot=1\nsymbols=BTCUSDT\n");

  hft_backtest::BacktestRunRequest request{};
  request.sessionPath = dir;
  request.configPath = dir / "probe.ini";
  request.strategy = "backtest_probe";
  request.runId = "probe-partial-candles";
  request.outputPath = dir / "backtests" / "probe-partial-candles";
  const hft_backtest::BacktestRunResult result = hft_backtest::runBacktest(request, nullptr, nullptr);

  return expect(result.status == hft_backtest::Status::CorruptData, "probe rejects partial candle seed tiers") &&
         expect(result.error.find("candle seed") != std::string::npos, "partial candle error names candle seed");
}

bool testBacktestProbeRequiresTradeWarmupCoverage() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_probe_no_warmup_" + std::to_string(std::rand()));
  writeText(dir / "manifest.json",
            "{\"identity\":{\"exchange\":\"binance\",\"market\":\"futures\",\"symbols\":[\"BTCUSDT\"]},"
            "\"channels\":{\"trades\":{\"path\":\"jsonl/trades.jsonl\"},\"bookticker\":{\"path\":\"jsonl/bookticker.jsonl\"},\"depth\":{\"path\":\"jsonl/depth.jsonl\"},\"candles\":{\"path\":\"jsonl/candles.jsonl\"}}}\n");
  writeText(dir / "jsonl" / "trades.jsonl", "[10050000000,100000000,1,3100000000]\n");
  writeText(dir / "jsonl" / "bookticker.jsonl", "[9900000000,100000000,10100000000,100000000,3000000000]\n");
  writeText(dir / "jsonl" / "depth.jsonl", "[[9900000000,100000000,1],[10100000000,100000000,0],2900000000]\n");
  writeText(dir / "jsonl" / "candles.jsonl",
            "[1,1000000000,10200000000,9800000000,500000000]\n"
            "[2,900000000,10300000000,9700000000,600000000]\n"
            "[3,800000000,10400000000,9600000000,700000000]\n");
  writeText(dir / "probe.ini",
            "[strategy]\ntype=backtest_probe\nenabled=1\n[venue.binance_futures]\napi_slot=1\nsymbols=BTCUSDT\n");

  hft_backtest::BacktestRunRequest request{};
  request.sessionPath = dir;
  request.configPath = dir / "probe.ini";
  request.strategy = "backtest_probe";
  request.runId = "probe-no-warmup";
  request.outputPath = dir / "backtests" / "probe-no-warmup";
  const hft_backtest::BacktestRunResult result = hft_backtest::runBacktest(request, nullptr, nullptr);

  return expect(result.status == hft_backtest::Status::CorruptData, "probe rejects missing warmup coverage") &&
         expect(result.error.find("warmup") != std::string::npos || result.error.find("history") != std::string::npos,
                "missing warmup error names trade history");
}

bool metadataHasParam(const hft_backtest::StrategyMetadata& metadata, std::string_view key) noexcept {
  for (std::size_t i = 0; i < metadata.paramCount; ++i) {
    if (metadata.params[i].key && key == metadata.params[i].key) return true;
  }
  return false;
}

bool testStrategyMetadataExposesOnlySupportedSpreadMakerParams() {
  const hft_backtest::StrategyMetadata* metadata = hft_backtest::findStrategyMetadata("spread_maker1and2");
  return expect(metadata != nullptr, "spread_maker1and2 metadata is exported") &&
         expect(metadata->displayNameRu != nullptr && std::string_view{metadata->displayNameRu}.find("1/2") != std::string_view::npos,
                "spread_maker1and2 metadata carries Russian display name") &&
         expect(metadataHasParam(*metadata, "distance_bps"), "spread metadata contains distance_bps") &&
         expect(metadataHasParam(*metadata, "trigger_bps"), "spread metadata contains trigger_bps") &&
         expect(metadataHasParam(*metadata, "amount_qty"), "spread metadata contains amount_qty") &&
         expect(metadataHasParam(*metadata, "order_qty"), "spread metadata contains order_qty") &&
         expect(!metadataHasParam(*metadata, "distance_natr_pct"), "spread metadata does not expose unsupported distance_natr_pct") &&
         expect(!metadataHasParam(*metadata, "trigger_natr_pct"), "spread metadata does not expose unsupported trigger_natr_pct") &&
         expect(!metadataHasParam(*metadata, "natr_ema_period_seconds"), "spread metadata does not expose unsupported natr_ema_period_seconds") &&
         expect(metadata->indicatorCount == 0u, "spread metadata declares no indicator profiles");
}

bool testBacktestProbeMetadataHasTraceIndicators() {
  const hft_backtest::StrategyMetadata* metadata = hft_backtest::findStrategyMetadata("backtest_probe");
  return expect(metadata != nullptr, "backtest_probe metadata is exported") &&
         expect(metadata->paramCount == 0u, "backtest_probe has no UI params") &&
         expect(metadata->indicatorCount == 2u, "backtest_probe declares trace indicator profiles") &&
         expect(metadata->indicators[0].id != nullptr && std::string_view{metadata->indicators[0].id} == "trade_range", "backtest_probe exposes trade_range indicator") &&
         expect(metadata->indicators[1].id != nullptr && std::string_view{metadata->indicators[1].id} == "rolling_volume", "backtest_probe exposes rolling_volume indicator");
}
}  // namespace

int main() {
  bool ok = true;
  ok = testStaleCancelDoesNotCancelReusedSlot() && ok;
  ok = testStaleAmendDoesNotMutateReusedSlot() && ok;
  ok = testPendingDrainActivatesLateOrder() && ok;
  ok = testLatencySamplerIsReproducibleAndDeterministicWithoutJitter() && ok;
  ok = testLatencyRuntimeSplitsMarketAndLimitOrderActivation() && ok;
  ok = testFillPublishesRuntimeUserEvent() && ok;
  ok = testReduceOnlyMarketBuyDoesNotOpenWhenFlat() && ok;
  ok = testReduceOnlyMarketSellClosesLongAtBid() && ok;
  ok = testReduceOnlyMarketBuyClosesShortAtAsk() && ok;
  ok = testFeesReduceWalletAndNetPnl() && ok;
  ok = testEquityPointCapturesRealizedOnlyPnl() && ok;
  ok = testRiskSnapshotCapturesCompactMarkToMarketPnl() && ok;
  ok = testRiskSnapshotCadenceLimitsOpenPositionMtmRows() && ok;
  ok = testForceCloseOpenShortPositionSubmitsReduceOnlyMarketBuy() && ok;
  ok = testCancelOpenOrdersAtFinishWritesCancelAcks() && ok;
  ok = testAmendFillBelongsToReplacementOrder() && ok;
  ok = testAmendChainFillBelongsToLastReplacementOrder() && ok;
  ok = testCancelAfterAmendCancelsReplacementOrder() && ok;
  ok = testLoadSessionUsesManifestChannelPathsAndMetadata() && ok;
  ok = testLoadSessionFailsMissingRequiredDepth() && ok;
  ok = testDepthRowBuildsInitialOrderBookSnapshot() && ok;
  ok = testTieredCandleHistoryBuildsCounts() && ok;
  ok = testWriterCreatesV2RunDirectory() && ok;
  ok = testBacktestProbeReportsFullInputCoverage() && ok;
  ok = testBacktestProbeRequiresCalculatedTradeWarmup() && ok;
  ok = testBacktestProbeRequiresCandleSeed() && ok;
  ok = testBacktestProbeRequiresAllCandleSeedTiers() && ok;
  ok = testBacktestProbeRequiresTradeWarmupCoverage() && ok;
  ok = testStrategyMetadataExposesOnlySupportedSpreadMakerParams() && ok;
  ok = testBacktestProbeMetadataHasTraceIndicators() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
