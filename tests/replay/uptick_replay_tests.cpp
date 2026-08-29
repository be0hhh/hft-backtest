#include "hft_backtest/backtest.hpp"
#include "hft_backtest/status.hpp"
#include "../support/CapturedSessionFixture.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

constexpr std::int64_t kReplayEpochNs =
    hft_backtest::test_support::kReceiveRealtimeBaseNs;

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

std::string readText(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::in | std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

std::filesystem::path makeRoot() {
  return std::filesystem::temp_directory_path() /
         ("hft_backtest_uptick_replay_" + std::to_string(std::rand()));
}

std::string bookTickerRow(std::int64_t bid,
                          std::int64_t bidQty,
                          std::int64_t ask,
                          std::int64_t askQty,
                          std::int64_t tsNs,
                          std::int64_t seq) {
  return hft_backtest::test_support::bookTickerRow(
      bid, bidQty, ask, askQty, tsNs, static_cast<std::uint64_t>(seq),
      "H_USDT", "okx");
}

std::string priceLimitRow(std::int64_t tsNs,
                          std::int64_t lowerSellLimit,
                          std::int64_t upperBuyLimit) {
  return hft_backtest::test_support::priceLimitRow(
      tsNs, upperBuyLimit, lowerSellLimit, 1001u);
}

std::string tradeRow(std::int64_t price,
                     std::int64_t qty,
                     std::int64_t side,
                     std::int64_t tsNs,
                     std::int64_t seq) {
  return hft_backtest::test_support::tradeRow(
      price, qty, side, tsNs, static_cast<std::uint64_t>(seq), "H_USDT",
      "okx");
}

void writeUptickSession(const std::filesystem::path& dir) {
  writeText(
      dir / "manifest.json",
      hft_backtest::test_support::capturedManifestJson(
          "okx",
          "futures",
          "H_USDT",
          hft_backtest::test_support::CapturedChannelCounts{
              .trades = 2u,
              .bookTickers = 2u,
              .priceLimits = 1u,
              .depth = 3u}));
  writeText(dir / "instrument_metadata.json",
            "{\"tick_size_e8\":1000000,\"lot_size_e8\":100000000,\"contract_base_qty_e8\":100000000}\n");
  writeText(dir / "jsonl" / "price_limit.jsonl",
            priceLimitRow(kReplayEpochNs + 1000000ll,
                          9500000000ll, 10500000000ll));
  const auto depthLevels = std::initializer_list<hft_backtest::test_support::DepthLevel>{
      {9500000000ll, 5000000000ll, 0},
      {9600000000ll, 5000000000ll, 1}};
  writeText(dir / "jsonl" / "depth_tape.jsonl",
            hft_backtest::test_support::depthTapeRow(kReplayEpochNs + 1000000ll, 1000u, depthLevels) +
                hft_backtest::test_support::depthTapeRow(kReplayEpochNs + 2000000ll, 2000u, depthLevels) +
                hft_backtest::test_support::depthTapeRow(kReplayEpochNs + 3000000ll, 3000u, depthLevels));
  writeText(dir / "jsonl" / "depth_sidecar.jsonl",
            hft_backtest::test_support::depthSidecarRow(kReplayEpochNs + 1000000ll, 1000u, depthLevels) +
                hft_backtest::test_support::depthSidecarRow(kReplayEpochNs + 2000000ll, 2000u, depthLevels) +
                hft_backtest::test_support::depthSidecarRow(kReplayEpochNs + 3000000ll, 3000u, depthLevels));
  writeText(dir / "jsonl" / "bookticker.jsonl",
            bookTickerRow(9500000000ll, 5000000000ll, 9600000000ll, 5000000000ll,
                          kReplayEpochNs + 2000000ll, 2001) +
                bookTickerRow(9500000000ll, 5000000000ll, 9600000000ll, 5000000000ll,
                              kReplayEpochNs + 3000000ll, 3001));
  writeText(dir / "jsonl" / "trades.jsonl",
            tradeRow(9600000000ll, 6000000000ll, 1,
                     kReplayEpochNs + 4000000ll, 4000) +
                tradeRow(9600000000ll, 6000000000ll, 1,
                         kReplayEpochNs + 5000000ll, 5000));
}

void writeConfig(const std::filesystem::path& path) {
  writeText(path,
            "[strategy]\n"
            "type=uptick\n"
            "enabled=1\n"
            "order_qty=1\n"
            "\n"
            "[venue.okx_futures]\n"
            "api_slot=1\n"
            "market_enabled=true\n"
            "user_enabled=true\n"
            "order_enabled=true\n"
            "control_enabled=true\n"
            "symbols=H_USDT\n");
}

bool testUptickReplayPlacesAndFillsFromValidFlatStart() {
  const std::filesystem::path root = makeRoot();
  const std::filesystem::path session = root / "session";
  writeUptickSession(session);
  writeConfig(root / "uptick.ini");

  hft_backtest::BacktestRunRequest request{};
  request.sessionPath = session;
  request.configPath = root / "uptick.ini";
  request.outputPath = root / "backtests" / "uptick-synthetic";
  request.strategy = "uptick";
  request.runId = "uptick-synthetic";
  request.initialBalanceE8 = 100000000000ll;
  request.makerFeeBpsE8 = 0;
  request.takerFeeBpsE8 = 0;
  request.hasMakerFeeBps = true;
  request.hasTakerFeeBps = true;
  request.marketOrderLatency.baseUs = 0;
  request.limitOrderLatency.baseUs = 0;
  request.cancelOrderLatency.baseUs = 0;
  request.userDataLatency.baseUs = 0;
  request.orderLatencyUs = 0;
  request.cancelLatencyUs = 0;
  request.writeArtifacts = true;

  const hft_backtest::BacktestRunResult result = hft_backtest::runBacktest(request, nullptr, nullptr);
  if (!expect(result.status == hft_backtest::Status::Ok, "uptick replay completes")) {
    std::cerr << "status=" << hft_backtest::statusToString(result.status)
              << " error=" << result.error << "\n";
    return false;
  }
  const std::string manifest = readText(result.resultPath / "manifest.json");

  return expect(result.orders > 0u, "uptick synthetic replay submits orders") &&
         expect(result.fills > 0u, "uptick synthetic replay produces fills") &&
         expect(result.strategyClosed, "uptick synthetic replay closes final exposure") &&
         expect(result.openPositionQtyE8 == 0, "uptick synthetic replay reports flat final position") &&
         expect(result.inputCoverage.cyclesWithPriceLimit > 0u, "uptick synthetic replay sees price_limit") &&
         expect(result.inputCoverage.cyclesWithOrderBook > 0u, "uptick synthetic replay sees orderbook") &&
         expect(manifest.find("\"zero_activity_reason\": \"fills_present\"") != std::string::npos,
                "uptick synthetic replay diagnostics report fills present");
}

bool testMultiLegEarlyErrorManifestCarriesRequestedLegs() {
  const std::filesystem::path root = makeRoot();
  writeConfig(root / "uptick.ini");

  hft_backtest::BacktestRunRequest request{};
  request.sessionPath = root / "leg0";
  request.configPath = root / "uptick.ini";
  request.outputPath = root / "backtests" / "multi-leg-invalid-indicator";
  request.strategy = "uptick";
  request.indicatorProfile = "missing-profile";
  request.runId = "multi-leg-invalid-indicator";
  request.initialBalanceE8 = 900000000000ll;
  request.writeArtifacts = true;

  hft_backtest::BacktestSessionRequest leg1{};
  leg1.path = root / "leg1";
  leg1.symbol = "LEG1";
  request.sessions.push_back(leg1);
  hft_backtest::BacktestSessionRequest leg2{};
  leg2.path = root / "leg2";
  leg2.symbol = "LEG2";
  request.sessions.push_back(leg2);

  const hft_backtest::BacktestRunResult result = hft_backtest::runBacktest(request, nullptr, nullptr);
  if (!expect(result.status == hft_backtest::Status::InvalidArgument, "invalid indicator rejects before loading sessions")) {
    std::cerr << "status=" << hft_backtest::statusToString(result.status)
              << " error=" << result.error << "\n";
    return false;
  }
  const std::string manifest = readText(result.resultPath / "manifest.json");

  return expect(manifest.find("\"legs\": [") != std::string::npos,
                "early error manifest includes legs array") &&
         expect(manifest.find("\"leg_index\": 0") != std::string::npos,
                "early error manifest includes primary leg index") &&
         expect(manifest.find("\"leg_index\": 1") != std::string::npos,
                "early error manifest includes first extra leg index") &&
         expect(manifest.find("\"leg_index\": 2") != std::string::npos,
                "early error manifest includes second extra leg index") &&
         expect(manifest.find((root / "leg0").generic_string()) != std::string::npos,
                "early error manifest includes primary session path") &&
         expect(manifest.find((root / "leg1").generic_string()) != std::string::npos,
                "early error manifest includes first extra session path") &&
         expect(manifest.find((root / "leg2").generic_string()) != std::string::npos,
                "early error manifest includes second extra session path") &&
         expect(std::filesystem::exists(result.resultPath / "legs" / "0" / "equity.jsonl"),
                "early error writes primary leg equity stream declared by manifest") &&
         expect(std::filesystem::exists(result.resultPath / "legs" / "1" / "equity.jsonl"),
                "early error writes first extra leg equity stream declared by manifest") &&
         expect(std::filesystem::exists(result.resultPath / "legs" / "2" / "equity.jsonl"),
                "early error writes second extra leg equity stream declared by manifest");
}

}  // namespace

int main() {
  bool ok = true;
  ok = testUptickReplayPlacesAndFillsFromValidFlatStart() && ok;
  ok = testMultiLegEarlyErrorManifestCarriesRequestedLegs() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
