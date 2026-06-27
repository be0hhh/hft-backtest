#include "hft_backtest/backtest_sweep.hpp"

#include "sweep/backtest_sweep_internal.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string_view>
#include <vector>

namespace {

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

bool testSearchPlanHonorsBudgetAcrossManyParams() {
  hft_backtest::BacktestSweepRequest request{};
  request.runBudget = 7u;
  request.ranges.push_back(hft_backtest::BacktestSweepParamRange{"distance_bps", 100, 300, 50});
  request.ranges.push_back(hft_backtest::BacktestSweepParamRange{"trigger_bps", 5, 25, 5});
  request.ranges.push_back(hft_backtest::BacktestSweepParamRange{"close_delay_us", 100000, 500000, 100000});

  const std::vector<hft_backtest::detail::BacktestSweepPoint> plan = hft_backtest::detail::buildSweepSearchPlan(request);

  return expect(plan.size() == 7u, "search plan is capped by run budget") &&
         expect(plan[0].values.size() == 3u, "search point contains every parameter") &&
         expect(plan[0].pointId == 0u, "search point ids are deterministic") &&
         expect(plan[6].pointId == 6u, "last search point id follows ordinal");
}

bool testSearchPlanSamplesGridParamsDeterministically() {
  hft_backtest::BacktestSweepRequest request{};
  request.runBudget = 5u;
  request.searchSeed = 123u;
  request.ranges.push_back(hft_backtest::BacktestSweepParamRange{"distance_bps", 100, 300, 100, hft_backtest::BacktestSweepParamMode::Grid});
  request.ranges.push_back(hft_backtest::BacktestSweepParamRange{"trigger_bps", 5, 25, 5, hft_backtest::BacktestSweepParamMode::Grid});

  const std::vector<hft_backtest::detail::BacktestSweepPoint> first = hft_backtest::detail::buildSweepSearchPlan(request);
  const std::vector<hft_backtest::detail::BacktestSweepPoint> second = hft_backtest::detail::buildSweepSearchPlan(request);

  bool same = first.size() == second.size();
  for (std::size_t i = 0u; i < first.size() && i < second.size(); ++i) {
    same = same && first[i].values == second[i].values;
  }
  return expect(first.size() == 5u, "grid plan is capped by budget") &&
         expect(same, "same sweep request produces same sampled values") &&
         expect(first.front().values != first.back().values, "grid dimensions vary across points");
}

bool testSweepRunsSummaryOnlyAndWritesCompactArtifacts() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_sweep_" + std::to_string(std::rand()));
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
            "  \"artifacts\": {\"instrument_metadata_path\": \"instrument_metadata.json\"},\n"
            "  \"summary\": {\"trades\": {\"history_requested_start_ns\": 1, \"history_requested_end_ns\": 2500000000, \"history_rows\": 3}}\n"
            "}\n");
  writeText(dir / "instrument_metadata.json", "{\"tick_size_e8\":1000000,\"lot_size_e8\":1000,\"contract_base_qty_e8\":100000000}\n");
  writeText(dir / "jsonl" / "trades.jsonl",
            "[10000000000,100000000,1,1500000000,1,1,1,10000000000,0,\"BTCUSDT\",\"binance\",\"futures\",1,1]\n"
            "[10020000000,100000000,0,1600000000,2,2,2,10020000000,0,\"BTCUSDT\",\"binance\",\"futures\",2,2]\n"
            "[10050000000,100000000,1,2100000000,3,3,3,10050000000,0,\"BTCUSDT\",\"binance\",\"futures\",3,3]\n"
            "[10060000000,100000000,1,3100000000,4,4,4,10060000000,0,\"BTCUSDT\",\"binance\",\"futures\",4,4]\n");
  writeText(dir / "jsonl" / "bookticker.jsonl", "[9900000000,100000000,10100000000,100000000,3000000000,3,3]\n");
  writeText(dir / "jsonl" / "price_limit.jsonl", "[3000000000,9500000000,10500000000,1]\n");
  writeText(dir / "jsonl" / "depth.jsonl", "[[9900000000,100000000,1],[10100000000,100000000,0],2500000000,2,2]\n");
  writeText(dir / "jsonl" / "candles.jsonl",
            "[1,1000000000,10200000000,9800000000,500000000]\n"
            "[2,900000000,10300000000,9700000000,600000000]\n"
            "[3,800000000,10400000000,9600000000,700000000]\n");
  writeText(dir / "spread.ini",
            "[runtime]\nname=spread\nenv_path=.env\n"
            "[strategy]\ntype=spread_maker1and2\nenabled=1\n"
            "distance_bps=100\n"
            "trigger_bps=5\n"
            "amount_qty=25\n"
            "[venue.binance_futures]\napi_slot=1\nsymbols=BTCUSDT\n");

  hft_backtest::BacktestSweepRequest request{};
  request.baseRun.sessionPath = dir;
  request.baseRun.configPath = dir / "spread.ini";
  request.baseRun.strategy = "spread_maker1and2";
  request.outputPath = dir / "sweep-out";
  request.sweepId = "sweep-contract";
  request.runBudget = 2u;
  request.searchSeed = 99u;
  request.ranges.push_back(hft_backtest::BacktestSweepParamRange{"distance_bps", 100, 200, 100, hft_backtest::BacktestSweepParamMode::Grid});
  request.ranges.push_back(hft_backtest::BacktestSweepParamRange{"trigger_bps", 5, 10, 5, hft_backtest::BacktestSweepParamMode::Grid});

  const hft_backtest::BacktestSweepResult result = hft_backtest::runBacktestSweep(request, nullptr, nullptr);
  const std::string manifest = readText(result.manifestPath);
  const std::string rows = readText(result.rowsPath);
  const std::string curves = readText(result.curvesPath);
  const std::string pointConfig = readText(result.resultPath / "point_configs" / "point-0.ini");

  return expect(result.status == hft_backtest::Status::Ok, "sweep run completes") &&
         expect(result.pointsEvaluated == 2u, "sweep evaluates requested budget") &&
         expect(manifest.find("\"type\": \"sweep.result.v1\"") != std::string::npos, "manifest declares sweep schema") &&
         expect(manifest.find("\"mode\": \"whole_session_exploration\"") != std::string::npos, "manifest labels exploration mode") &&
         expect(manifest.find("\"search_seed\": 99") != std::string::npos, "manifest declares search seed") &&
         expect(manifest.find("\"curves\"") != std::string::npos, "manifest declares compact curve artifact") &&
         expect(manifest.find("\"legs\"") != std::string::npos, "manifest declares leg container") &&
         expect(manifest.find("\"session_path\"") != std::string::npos, "manifest declares leg session path") &&
         expect(rows.find("\"distance_bps\"") != std::string::npos, "rows include distance_bps") &&
         expect(rows.find("\"trigger_bps\"") != std::string::npos, "rows include trigger_bps") &&
         expect(rows.find("\"legs\"") != std::string::npos, "rows include per-leg pnl container") &&
         expect(rows.find("\"max_drawdown_e8\"") != std::string::npos, "rows include drawdown metric") &&
         expect(rows.find("\"fills\"") != std::string::npos, "rows include fill count") &&
         expect(rows.find("\"risk_stopped\"") != std::string::npos, "rows include risk stop flag") &&
         expect(rows.find("\"liquidated\"") != std::string::npos, "rows include liquidation flag") &&
         expect(rows.find("\"score_e8\"") == std::string::npos, "rows omit score metric") &&
         expect(rows.find("\"random_seed\"") == std::string::npos, "rows omit random seed") &&
         expect(curves.find("\"curve_e8\"") != std::string::npos, "curves include compact pnl series") &&
         expect(curves.find("\"legs\"") != std::string::npos, "curves include per-leg pnl series container") &&
         expect(curves.find("\"max_drawdown_e8\"") != std::string::npos, "curves include drawdown metric") &&
         expect(curves.find("\"fills\"") != std::string::npos, "curves include fill count") &&
         expect(pointConfig.find("[runtime]\nname=spread") != std::string::npos, "point config keeps base config") &&
         expect(pointConfig.find("distance_bps=") != std::string::npos, "point config writes distance override") &&
         expect(!std::filesystem::exists(request.outputPath / (request.sweepId + "-point-0") / "manifest.json"), "sweep does not write full run dirs per point");
}

bool testSweepCanRunTwiceInOneProcess() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_sweep_twice_" + std::to_string(std::rand()));
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
            "  \"artifacts\": {\"instrument_metadata_path\": \"instrument_metadata.json\"},\n"
            "  \"summary\": {\"trades\": {\"history_requested_start_ns\": 1, \"history_requested_end_ns\": 2500000000, \"history_rows\": 3}}\n"
            "}\n");
  writeText(dir / "instrument_metadata.json", "{\"tick_size_e8\":1000000,\"lot_size_e8\":1000,\"contract_base_qty_e8\":100000000}\n");
  writeText(dir / "jsonl" / "trades.jsonl",
            "[10000000000,100000000,1,1500000000,1,1,1,10000000000,0,\"BTCUSDT\",\"binance\",\"futures\",1,1]\n"
            "[10020000000,100000000,0,1600000000,2,2,2,10020000000,0,\"BTCUSDT\",\"binance\",\"futures\",2,2]\n"
            "[10050000000,100000000,1,2100000000,3,3,3,10050000000,0,\"BTCUSDT\",\"binance\",\"futures\",3,3]\n"
            "[10060000000,100000000,1,3100000000,4,4,4,10060000000,0,\"BTCUSDT\",\"binance\",\"futures\",4,4]\n");
  writeText(dir / "jsonl" / "bookticker.jsonl", "[9900000000,100000000,10100000000,100000000,3000000000,3,3]\n");
  writeText(dir / "jsonl" / "price_limit.jsonl", "[3000000000,9500000000,10500000000,1]\n");
  writeText(dir / "jsonl" / "depth.jsonl", "[[9900000000,100000000,1],[10100000000,100000000,0],2500000000,2,2]\n");
  writeText(dir / "jsonl" / "candles.jsonl",
            "[1,1000000000,10200000000,9800000000,500000000]\n"
            "[2,900000000,10300000000,9700000000,600000000]\n"
            "[3,800000000,10400000000,9600000000,700000000]\n");
  writeText(dir / "spread.ini",
            "[runtime]\nname=spread\nenv_path=.env\n"
            "[strategy]\ntype=spread_maker1and2\nenabled=1\n"
            "distance_bps=100\n"
            "trigger_bps=5\n"
            "amount_qty=25\n"
            "[venue.binance_futures]\napi_slot=1\nsymbols=BTCUSDT\n");

  auto makeRequest = [&](const std::filesystem::path& out, std::string_view sweepId) {
    hft_backtest::BacktestSweepRequest request{};
    request.baseRun.sessionPath = dir;
    request.baseRun.configPath = dir / "spread.ini";
    request.baseRun.strategy = "spread_maker1and2";
    request.outputPath = out;
    request.sweepId = std::string{sweepId};
    request.runBudget = 2u;
    request.searchSeed = 99u;
    request.ranges.push_back(hft_backtest::BacktestSweepParamRange{"distance_bps", 100, 200, 100, hft_backtest::BacktestSweepParamMode::Grid});
    request.ranges.push_back(hft_backtest::BacktestSweepParamRange{"trigger_bps", 5, 10, 5, hft_backtest::BacktestSweepParamMode::Grid});
    return request;
  };

  const hft_backtest::BacktestSweepResult first = hft_backtest::runBacktestSweep(makeRequest(dir / "sweep-out-a", "sweep-a"), nullptr, nullptr);
  const hft_backtest::BacktestSweepResult second = hft_backtest::runBacktestSweep(makeRequest(dir / "sweep-out-b", "sweep-b"), nullptr, nullptr);

  return expect(first.status == hft_backtest::Status::Ok, "first sweep run completes") &&
         expect(second.status == hft_backtest::Status::Ok, "second sweep run completes") &&
         expect(first.pointsEvaluated == 2u && second.pointsEvaluated == 2u, "both sweep runs evaluate requested budget");
}

bool testSweepManifestDeclaresTwoLegSessionPaths() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_sweep_manifest_legs_" + std::to_string(std::rand()));
  std::filesystem::create_directories(dir);

  hft_backtest::BacktestSweepRequest request{};
  request.baseRun.sessionPath = dir / "leg-a";
  request.baseRun.configPath = dir / "config.ini";
  request.baseRun.strategy = "stat_arb_band_ladder";
  request.baseRun.sessions.push_back(hft_backtest::BacktestSessionRequest{dir / "leg-b", "kucoin_futures", "B-USDT"});
  request.runBudget = 1u;

  hft_backtest::BacktestSweepResult result{};
  result.sweepId = "sweep-legs";
  result.manifestPath = dir / "manifest.json";

  hft_backtest::detail::BacktestSweepPointResult point{};
  point.run.legs.push_back(hft_backtest::BacktestLegSummary{});
  point.run.legs.back().sessionPath = dir / "leg-a";
  point.run.legs.back().exchange = "aster";
  point.run.legs.back().market = "futures";
  point.run.legs.back().symbol = "A-USDT";
  point.run.legs.push_back(hft_backtest::BacktestLegSummary{});
  point.run.legs.back().sessionPath = dir / "leg-b";
  point.run.legs.back().exchange = "kucoin";
  point.run.legs.back().market = "futures";
  point.run.legs.back().symbol = "B-USDT";

  const hft_backtest::Status status = hft_backtest::detail::writeSweepManifest(request, result, {point});
  const std::string manifest = readText(result.manifestPath);

  return expect(status == hft_backtest::Status::Ok, "two-leg sweep manifest write succeeds") &&
         expect(manifest.find("\"legs\"") != std::string::npos, "two-leg manifest includes legs") &&
         expect(manifest.find((dir / "leg-a").generic_string()) != std::string::npos, "two-leg manifest includes primary session") &&
         expect(manifest.find((dir / "leg-b").generic_string()) != std::string::npos, "two-leg manifest includes extra session") &&
         expect(manifest.find("\"leg_index\": 1") != std::string::npos, "two-leg manifest declares second leg index");
}

}  // namespace

int main() {
  bool ok = true;
  ok = testSearchPlanHonorsBudgetAcrossManyParams() && ok;
  ok = testSearchPlanSamplesGridParamsDeterministically() && ok;
  ok = testSweepRunsSummaryOnlyAndWritesCompactArtifacts() && ok;
  ok = testSweepCanRunTwiceInOneProcess() && ok;
  ok = testSweepManifestDeclaresTwoLegSessionPaths() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
