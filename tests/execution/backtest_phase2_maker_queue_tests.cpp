#include "engine/backtest_engine.hpp"
#include "execution/backtest_execution_reprice.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using namespace hft_backtest::detail;

constexpr std::int64_t kQty1 = 100000000ll;

void writeText(const std::filesystem::path& path, std::string_view text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::out | std::ios::trunc | std::ios::binary);
  out << text;
}

bool expect(bool value, std::string_view message) {
  if (value) return true;
  std::cerr << "FAIL: " << message << "\n";
  return false;
}

JsonOrder limitOrder(std::uint64_t id,
                     std::uint8_t side,
                     std::uint8_t status,
                     std::int64_t priceE8) noexcept {
  return JsonOrder{
      0u,
      id,
      0u,
      500,
      1000,
      500,
      kRecordActionOrder,
      side,
      kRecordTypeLimit,
      status,
      priceE8,
      kQty1,
      false,
      true};
}

bool testPhase2MakerQueueFillsBestBidLimitAfterBboQueueBurn() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_phase2_maker_bid_" + std::to_string(std::rand()));
  writeText(dir / "jsonl" / "bookticker.jsonl",
            "[105000000,100000000,106000000,100000000,1000]\n");
  writeText(dir / "jsonl" / "trades.jsonl",
            "[105000000,40000000,0,2000]\n"
            "[105000000,70000000,0,3000]\n");

  EngineState engine{};
  engine.orders.push_back(limitOrder(77u, kRecordSideBuy, kRecordStatusActive, 105000000));

  Phase2MakerQueueStats stats{};
  const bool ok = repriceMakerLimitQueueWithBboTrades(dir, engine, 4000, stats);

  return expect(ok, "phase2 maker queue opens BBO and trade tapes") &&
         expect(stats.ordersVisited == 1u, "phase2 maker queue visits one limit order") &&
         expect(stats.ordersTopOfBook == 1u, "phase2 maker queue detects best-bid order") &&
         expect(stats.makerFillsCreated == 1u, "phase2 maker queue creates one maker fill") &&
         expect(engine.fills.size() == 1u, "phase2 maker queue writes fill") &&
         expect(engine.fills[0].orderId == 77u, "phase2 maker queue fill keeps order id") &&
         expect(engine.fills[0].exitTsNs == 3000, "phase2 maker queue fills after queue burn trade") &&
         expect(engine.fills[0].priceE8 == 105000000, "phase2 maker queue fills at order price") &&
         expect(engine.fills[0].qtyE8 == 10000000, "phase2 maker queue subtracts BBO queue ahead") &&
         expect(engine.fills[0].liquidity == kFillLiquidityMaker, "phase2 maker queue marks maker liquidity") &&
         expect(engine.fills[0].fillReason == kFillReasonPassiveQueue, "phase2 maker queue marks passive queue reason") &&
         expect(engine.fills[0].queueAheadBeforeE8 == 60000000, "phase2 maker queue records queue before fill trade") &&
         expect(engine.fills[0].queueAheadAfterE8 == 0, "phase2 maker queue records queue after fill trade") &&
         expect(engine.orders[0].status == kRecordStatusActive, "partial phase2 maker fill leaves order active");
}

bool testPhase2MakerQueueFillsBestAskLimitFromBuyTrades() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_phase2_maker_ask_" + std::to_string(std::rand()));
  writeText(dir / "jsonl" / "bookticker.jsonl",
            "[105000000,100000000,106000000,30000000,1000]\n");
  writeText(dir / "jsonl" / "trades.jsonl",
            "[106000000,120000000,1,2000]\n");

  EngineState engine{};
  engine.orders.push_back(limitOrder(78u, kRecordSideSell, kRecordStatusActive, 106000000));

  Phase2MakerQueueStats stats{};
  const bool ok = repriceMakerLimitQueueWithBboTrades(dir, engine, 4000, stats);

  return expect(ok, "phase2 maker queue opens ask-side tapes") &&
         expect(stats.ordersTopOfBook == 1u, "phase2 maker queue detects best-ask order") &&
         expect(engine.fills.size() == 1u, "phase2 maker queue writes ask-side fill") &&
         expect(engine.fills[0].side == kRecordSideSell, "phase2 maker queue keeps sell side") &&
         expect(engine.fills[0].qtyE8 == 90000000, "phase2 maker queue burns ask qty before fill") &&
         expect(engine.fills[0].queueAheadBeforeE8 == 30000000, "phase2 maker queue records ask queue before") &&
         expect(engine.orders[0].status == kRecordStatusActive, "partial sell phase2 fill leaves order active");
}

bool testPhase2MakerQueueIgnoresSameSideTrades() {
  const std::filesystem::path bidDir = std::filesystem::temp_directory_path() /
                                       ("hft_backtest_phase2_maker_same_bid_" + std::to_string(std::rand()));
  writeText(bidDir / "jsonl" / "bookticker.jsonl",
            "[105000000,100000000,106000000,100000000,1000]\n");
  writeText(bidDir / "jsonl" / "trades.jsonl",
            "[105000000,200000000,1,2000]\n");

  EngineState bidEngine{};
  bidEngine.orders.push_back(limitOrder(177u, kRecordSideBuy, kRecordStatusActive, 105000000));
  Phase2MakerQueueStats bidStats{};
  const bool bidOk = repriceMakerLimitQueueWithBboTrades(bidDir, bidEngine, 4000, bidStats);

  const std::filesystem::path askDir = std::filesystem::temp_directory_path() /
                                       ("hft_backtest_phase2_maker_same_ask_" + std::to_string(std::rand()));
  writeText(askDir / "jsonl" / "bookticker.jsonl",
            "[105000000,100000000,106000000,100000000,1000]\n");
  writeText(askDir / "jsonl" / "trades.jsonl",
            "[106000000,200000000,0,2000]\n");

  EngineState askEngine{};
  askEngine.orders.push_back(limitOrder(178u, kRecordSideSell, kRecordStatusActive, 106000000));
  Phase2MakerQueueStats askStats{};
  const bool askOk = repriceMakerLimitQueueWithBboTrades(askDir, askEngine, 4000, askStats);

  return expect(bidOk, "phase2 maker queue opens same-side bid tapes") &&
         expect(bidStats.ordersTopOfBook == 1u, "same-side bid scenario reaches top of book") &&
         expect(bidStats.makerFillsCreated == 0u, "same-side buy trade does not fill passive buy") &&
         expect(bidEngine.fills.empty(), "same-side bid scenario leaves fills empty") &&
         expect(bidEngine.orders[0].status == kRecordStatusActive, "same-side bid order remains active") &&
         expect(askOk, "phase2 maker queue opens same-side ask tapes") &&
         expect(askStats.ordersTopOfBook == 1u, "same-side ask scenario reaches top of book") &&
         expect(askStats.makerFillsCreated == 0u, "same-side sell trade does not fill passive sell") &&
         expect(askEngine.fills.empty(), "same-side ask scenario leaves fills empty") &&
         expect(askEngine.orders[0].status == kRecordStatusActive, "same-side ask order remains active");
}

bool testPhase2MakerQueueBurnsQueueWithoutFillWhenVolumeMatchesTop() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_phase2_maker_exact_burn_" + std::to_string(std::rand()));
  writeText(dir / "jsonl" / "bookticker.jsonl",
            "[105000000,100000000,106000000,100000000,1000]\n");
  writeText(dir / "jsonl" / "trades.jsonl",
            "[105000000,40000000,0,2000]\n"
            "[105000000,60000000,0,3000]\n");

  EngineState engine{};
  engine.orders.push_back(limitOrder(179u, kRecordSideBuy, kRecordStatusActive, 105000000));

  Phase2MakerQueueStats stats{};
  const bool ok = repriceMakerLimitQueueWithBboTrades(dir, engine, 4000, stats);

  return expect(ok, "phase2 maker queue opens exact-burn tapes") &&
         expect(stats.ordersVisited == 1u, "exact-burn visits one order") &&
         expect(stats.ordersTopOfBook == 1u, "exact-burn reaches top of book") &&
         expect(stats.makerFillsCreated == 0u, "exact-burn does not fill without excess trade qty") &&
         expect(engine.fills.empty(), "exact-burn leaves fills empty") &&
         expect(engine.orders[0].status == kRecordStatusActive, "exact-burn order remains active");
}

bool testPhase2MakerQueueIgnoresLimitAwayFromBestBbo() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_phase2_maker_deep_" + std::to_string(std::rand()));
  writeText(dir / "jsonl" / "bookticker.jsonl",
            "[105000000,100000000,106000000,100000000,1000]\n");
  writeText(dir / "jsonl" / "trades.jsonl",
            "[104000000,200000000,1,2000]\n");

  EngineState engine{};
  engine.orders.push_back(limitOrder(79u, kRecordSideBuy, kRecordStatusActive, 104000000));

  Phase2MakerQueueStats stats{};
  const bool ok = repriceMakerLimitQueueWithBboTrades(dir, engine, 4000, stats);

  return expect(ok, "phase2 maker queue opens deep limit tapes") &&
         expect(stats.ordersVisited == 1u, "phase2 maker queue visits deep limit order") &&
         expect(stats.ordersTopOfBook == 0u, "phase2 maker queue does not treat deep limit as BBO") &&
         expect(stats.makerFillsCreated == 0u, "phase2 maker queue does not fill deep passive limit") &&
         expect(engine.fills.empty(), "phase2 maker queue leaves deep limit unfilled") &&
         expect(engine.orders[0].status == kRecordStatusActive, "deep limit status remains active");
}

bool testPhase2MakerQueueReplacesOptimisticPhase1MakerFill() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_phase2_maker_replace_" + std::to_string(std::rand()));
  writeText(dir / "jsonl" / "bookticker.jsonl",
            "[105000000,100000000,106000000,100000000,1000]\n");
  writeText(dir / "jsonl" / "trades.jsonl",
            "[105000000,40000000,0,2000]\n"
            "[105000000,70000000,0,3000]\n");

  EngineState engine{};
  engine.orders.push_back(limitOrder(80u, kRecordSideBuy, kRecordStatusFilled, 105000000));
  engine.fills.push_back(JsonFill{
      0u,
      80u,
      1000,
      2000,
      kRecordSideBuy,
      105000000,
      kQty1,
      0,
      false,
      kFillReasonPassiveQueue,
      kFillLiquidityMaker,
      kQty1,
      kQty1,
      0,
      105000000});

  Phase2MakerQueueStats stats{};
  const bool ok = repriceMakerLimitQueueWithBboTrades(dir, engine, 4000, stats);

  return expect(ok, "phase2 maker queue opens replacement tapes") &&
         expect(stats.makerFillsRemoved == 1u, "phase2 maker queue removes optimistic phase1 maker fill") &&
         expect(stats.makerFillsCreated == 1u, "phase2 maker queue creates replacement maker fill") &&
         expect(engine.fills.size() == 1u, "phase2 maker queue leaves one replacement fill") &&
         expect(engine.fills[0].exitTsNs == 3000, "replacement maker fill moves to queue burn timestamp") &&
         expect(engine.fills[0].qtyE8 == 10000000, "replacement maker fill uses queue-adjusted quantity") &&
         expect(engine.orders[0].status == kRecordStatusActive, "replacement partial fill reopens order status");
}

}  // namespace

int main() {
  bool ok = true;
  ok = testPhase2MakerQueueFillsBestBidLimitAfterBboQueueBurn() && ok;
  ok = testPhase2MakerQueueFillsBestAskLimitFromBuyTrades() && ok;
  ok = testPhase2MakerQueueIgnoresSameSideTrades() && ok;
  ok = testPhase2MakerQueueBurnsQueueWithoutFillWhenVolumeMatchesTop() && ok;
  ok = testPhase2MakerQueueIgnoresLimitAwayFromBestBbo() && ok;
  ok = testPhase2MakerQueueReplacesOptimisticPhase1MakerFill() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
