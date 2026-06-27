#include "hft_backtest/backtest.hpp"
#include "hft_backtest/status.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

constexpr std::int64_t kQty1 = 100000000ll;
constexpr std::int64_t kPartialQty = 2000000ll;
constexpr std::int64_t kLeg0PassiveBidFillPx = 9989000000ll;

bool expect(bool value, std::string_view message) {
  if (value) return true;
  std::cerr << "FAIL: " << message << "\n";
  return false;
}

void writeText(const std::filesystem::path& path, std::string_view text) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream out(path, std::ios::out | std::ios::trunc | std::ios::binary);
  out << text;
}

std::filesystem::path makeRoot() {
  return std::filesystem::temp_directory_path() /
         ("hft_backtest_stat_arb_edge_maker_replay_" + std::to_string(std::rand()));
}

void writeSessionManifest(const std::filesystem::path& dir,
                          std::string_view exchange,
                          std::string_view symbol) {
  writeText(dir / "manifest.json",
            "{\n"
            "  \"identity\": {\"exchange\": \"" + std::string{exchange} +
                "\", \"market\": \"futures\", \"symbols\": [\"" + std::string{symbol} + "\"]},\n"
            "  \"channels\": {\n"
            "    \"trades\": {\"path\": \"jsonl/trades.jsonl\"},\n"
            "    \"bookticker\": {\"path\": \"jsonl/bookticker.jsonl\"}\n"
            "  },\n"
            "  \"artifacts\": {\"instrument_metadata_path\": \"instrument_metadata.json\"}\n"
            "}\n");
  writeText(dir / "instrument_metadata.json",
            "{\"tick_size_e8\":1000000,\"lot_size_e8\":1000000,\"contract_base_qty_e8\":100000000}\n");
}

std::string bookTickerRow(std::int64_t bid, std::int64_t ask, std::int64_t tsNs, std::int64_t seq) {
  return "[" + std::to_string(bid) + ",100000000," + std::to_string(ask) +
         ",100000000," + std::to_string(tsNs) + ",1," + std::to_string(seq) + "]\n";
}

std::string tradeRow(std::int64_t price,
                     std::int64_t qty,
                     std::int64_t side,
                     std::int64_t tsNs,
                     std::int64_t seq,
                     std::string_view symbol,
                     std::string_view exchange) {
  return "[" + std::to_string(price) + "," + std::to_string(qty) + "," + std::to_string(side) +
         "," + std::to_string(tsNs) + ",1,1,1," + std::to_string(price) + ",0,\"" +
         std::string{symbol} + "\",\"" + std::string{exchange} + "\",\"futures\",1," +
         std::to_string(seq) + "]\n";
}

void writeLeg0Session(const std::filesystem::path& dir) {
  writeSessionManifest(dir, "binance", "BTCUSDT");
  std::string book;
  std::string trades;
  book += bookTickerRow(10000000000ll, 10010000000ll, 1000000ll, 10);
  book += bookTickerRow(10000000000ll, 10010000000ll, 3000000ll, 20);
  trades += tradeRow(10005000000ll, kQty1, 1, 1000000ll, 11, "BTCUSDT", "binance");
  for (std::int64_t i = 0; i < 50; ++i) {
    const std::int64_t ts = 600000000ll + i * 1000000ll;
    trades += tradeRow(kLeg0PassiveBidFillPx, kPartialQty, 0, ts, 100 + i, "BTCUSDT", "binance");
  }
  writeText(dir / "jsonl" / "bookticker.jsonl", book);
  writeText(dir / "jsonl" / "trades.jsonl", trades);
}

void writeLeg1Session(const std::filesystem::path& dir) {
  writeSessionManifest(dir, "bybit", "BTCUSDT");
  std::string book;
  std::string trades;
  book += bookTickerRow(10200000000ll, 10210000000ll, 1000000ll, 10);
  book += bookTickerRow(10200000000ll, 10210000000ll, 3000000ll, 20);
  trades += tradeRow(10205000000ll, kQty1, 1, 1000000ll, 11, "BTCUSDT", "bybit");
  writeText(dir / "jsonl" / "bookticker.jsonl", book);
  writeText(dir / "jsonl" / "trades.jsonl", trades);
}

void writeConfig(const std::filesystem::path& path) {
  writeText(path,
            "[strategy]\n"
            "type=stat_arb_edge_maker\n"
            "enabled=1\n"
            "ema_window_ms=1\n"
            "entry_edge_bps=0\n"
            "exit_edge_bps=1000\n"
            "trigger_edge_bps=0\n"
            "ladder_step_bps=1\n"
            "max_ladder_levels=3\n"
            "order_qty=1\n"
            "\n"
            "[venue.binance_futures]\n"
            "api_slot=1\n"
            "market_enabled=true\n"
            "user_enabled=true\n"
            "order_enabled=true\n"
            "control_enabled=true\n"
            "symbols=BTCUSDT\n"
            "\n"
            "[venue.bybit_futures]\n"
            "api_slot=1\n"
            "market_enabled=true\n"
            "user_enabled=true\n"
            "order_enabled=true\n"
            "control_enabled=true\n"
            "symbols=BTCUSDT\n");
}

hft_backtest::BacktestRunRequest makeReplayRequest(const std::filesystem::path& root,
                                                   const std::filesystem::path& leg0,
                                                   const std::filesystem::path& leg1,
                                                   std::string_view runId) {
  hft_backtest::BacktestRunRequest request{};
  request.sessionPath = leg0;
  request.sessions.push_back(hft_backtest::BacktestSessionRequest{leg1, "bybit_futures", "BTCUSDT"});
  request.configPath = root / "stat_arb_edge_maker.ini";
  request.outputPath = root / "backtests" / std::string{runId};
  request.strategy = "stat_arb_edge_maker";
  request.runId = std::string{runId};
  request.initialBalanceE8 = 10000000000ll;
  request.legInitialBalancesE8 = {5000000000ll, 5000000000ll};
  request.marketDataLatency.baseUs = 500000;
  request.limitOrderLatency.baseUs = 0;
  request.marketOrderLatency.baseUs = 0;
  request.cancelOrderLatency.baseUs = 0;
  request.userDataLatency.baseUs = 100000;
  request.orderLatencyUs = 0;
  request.cancelLatencyUs = 0;
  request.writeArtifacts = true;
  request.captureStrategySpread = true;
  return request;
}

bool hasWarningContaining(const hft_backtest::BacktestRunResult& result,
                          std::string_view needle) {
  for (const std::string& warning : result.warnings) {
    if (warning.find(needle) != std::string::npos) return true;
  }
  return false;
}

bool testBurstPartialMakerFillReplayKeepsPairExposureBounded() {
  const std::filesystem::path root = makeRoot();
  const std::filesystem::path leg0 = root / "leg0";
  const std::filesystem::path leg1 = root / "leg1";
  writeLeg0Session(leg0);
  writeLeg1Session(leg1);
  writeConfig(root / "stat_arb_edge_maker.ini");

  hft_backtest::BacktestRunRequest request = makeReplayRequest(root,
                                                               leg0,
                                                               leg1,
                                                               "stat-arb-edge-maker-replay");

  const hft_backtest::BacktestRunResult result = hft_backtest::runBacktest(request, nullptr, nullptr);
  if (!expect(result.status == hft_backtest::Status::Ok, "stat_arb_edge_maker replay completes")) {
    std::cerr << "status=" << hft_backtest::statusToString(result.status)
              << " error=" << result.error << "\n";
    return false;
  }

  return expect(result.legs.size() == 2u, "replay returns two leg summaries") &&
         expect(result.legs[0].fills == 50u + result.legs[0].reduceOnlyFills,
                "leg0 receives 50 partial maker fills plus optional close") &&
         expect(result.legs[0].userEvents >= result.legs[0].fills, "leg0 publishes fill plus terminal order user events") &&
         expect(result.legs[0].reduceOnlyFills <= 1u, "leg0 close fill is bounded") &&
         expect(result.legs[1].fills > result.legs[1].reduceOnlyFills,
                "leg1 receives reactive hedge fills before final close") &&
         expect(result.legs[1].fills <= 50u + result.legs[1].reduceOnlyFills,
                "leg1 hedge fills are bounded by maker deltas plus optional close") &&
         expect(result.legs[1].userEvents >= result.legs[1].fills, "leg1 publishes fill plus terminal order user events") &&
         expect(result.legs[1].reduceOnlyFills <= 1u, "leg1 close fill is bounded") &&
         expect(result.legs[0].openPositionQtyE8 == 0, "leg0 final exposure is closed") &&
         expect(result.legs[1].openPositionQtyE8 == 0, "leg1 final exposure is closed") &&
         expect(!result.liquidated, "replay does not liquidate") &&
         expect(!result.riskStopped, "replay does not risk stop");
}

bool testTwoPhaseReplayWarnsWhenPostHocRepriceLeavesExposureOpen() {
  const std::filesystem::path root = makeRoot();
  const std::filesystem::path leg0 = root / "leg0";
  const std::filesystem::path leg1 = root / "leg1";
  writeLeg0Session(leg0);
  writeLeg1Session(leg1);
  writeConfig(root / "stat_arb_edge_maker.ini");

  hft_backtest::BacktestRunRequest request = makeReplayRequest(root,
                                                               leg0,
                                                               leg1,
                                                               "stat-arb-edge-maker-two-phase-replay");
  request.executionPipeline = hft_backtest::BacktestExecutionPipeline::TwoPhaseReprice;

  const hft_backtest::BacktestRunResult result = hft_backtest::runBacktest(request, nullptr, nullptr);
  if (!expect(result.status == hft_backtest::Status::Ok, "two-phase stat_arb replay completes")) {
    std::cerr << "status=" << hft_backtest::statusToString(result.status)
              << " error=" << result.error << "\n";
    return false;
  }

  return expect(result.twoPhaseRepriceApplied, "two-phase replay applies repricing") &&
         expect(hasWarningContaining(result, "two-phase reprice is diagnostic"),
                "two-phase result warns that repricing is post-hoc diagnostic") &&
         expect(!result.strategyClosed, "two-phase conservative repricing can leave exposure open") &&
         expect(result.openPositionQtyE8 != 0, "two-phase result reports remaining exposure") &&
         expect(hasWarningContaining(result, "two-phase reprice left open exposure"),
                "two-phase result warns about post-hoc fill path invalidation");
}

}  // namespace

int main() {
  bool ok = true;
  ok = testBurstPartialMakerFillReplayKeepsPairExposureBounded() && ok;
  ok = testTwoPhaseReplayWarnsWhenPostHocRepriceLeavesExposureOpen() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
