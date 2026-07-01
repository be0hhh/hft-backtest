#include "hft_backtest/backtest.hpp"

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

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

std::size_t lineCount(std::string_view text) noexcept {
  std::size_t out = 0u;
  bool sawContent = false;
  for (char ch : text) {
    if (ch == '\n') {
      if (sawContent) ++out;
      sawContent = false;
    } else {
      sawContent = true;
    }
  }
  if (sawContent) ++out;
  return out;
}

std::size_t firstLineCommaCount(std::string_view text) noexcept {
  std::size_t out = 0u;
  for (char ch : text) {
    if (ch == '\n') break;
    if (ch == ',') ++out;
  }
  return out;
}

bool metadataHasIndicator(const hft_backtest::StrategyMetadata& metadata, std::string_view id) noexcept {
  for (std::size_t i = 0; i < metadata.indicatorCount; ++i) {
    if (metadata.indicators[i].id && id == metadata.indicators[i].id) return true;
  }
  return false;
}

bool metadataHasParam(const hft_backtest::StrategyMetadata& metadata, std::string_view key) noexcept {
  for (std::size_t i = 0; i < metadata.paramCount; ++i) {
    if (metadata.params[i].key && key == metadata.params[i].key) return true;
  }
  return false;
}

bool metadataDisplayNameIs(const hft_backtest::StrategyMetadata& metadata, std::string_view expected) noexcept {
  return metadata.displayNameRu != nullptr && expected == metadata.displayNameRu;
}

void writeToxicFlowPairSession(const std::filesystem::path& dir,
                               std::string_view exchange,
                               bool buyFlow,
                               std::int64_t priceOffset) {
  std::string manifest =
      "{\n"
      "  \"identity\": {\"exchange\": \"";
  manifest.append(exchange);
  manifest +=
      "\", \"market\": \"futures\", \"symbols\": [\"BTCUSDT\"]},\n"
      "  \"channels\": {\n"
      "    \"trades\": {\"path\": \"jsonl/trades.jsonl\"},\n"
      "    \"bookticker\": {\"path\": \"jsonl/bookticker.jsonl\"},\n"
      "    \"depth\": {\"path\": \"jsonl/depth.jsonl\"}\n"
      "  },\n"
      "  \"artifacts\": {\"instrument_metadata_path\": \"instrument_metadata.json\"}\n"
      "}\n";
  writeText(dir / "manifest.json", manifest);
  writeText(dir / "instrument_metadata.json", "{\"tick_size_e8\":1000000,\"lot_size_e8\":1000,\"contract_base_qty_e8\":100000000}\n");
  std::string trades;
  std::string bookticker;
  std::string depth;
  for (std::uint64_t second = 1u; second <= 45u; ++second) {
    const std::int64_t bid = 10000000000ll + priceOffset + static_cast<std::int64_t>(second) * 1000000ll;
    const std::int64_t ask = 10100000000ll + priceOffset + static_cast<std::int64_t>(second) * 1000000ll;
    const std::int64_t tradePx = buyFlow ? ask : bid;
    const int side = buyFlow ? 1 : 0;
    const std::uint64_t ts = second * 1000000000ull;
    trades += "[" + std::to_string(tradePx) + ",100000000," + std::to_string(side) + "," +
              std::to_string(ts) + ",1,1,1," + std::to_string(tradePx) +
              ",0,\"BTCUSDT\",\"" + std::string{exchange} + "\",\"futures\",1," + std::to_string(second) + "]\n";
    bookticker += "[" + std::to_string(bid) + ",100000000," + std::to_string(ask) +
                  ",100000000," + std::to_string(ts) + ",1," + std::to_string(second) + "]\n";
    depth += "[[" + std::to_string(bid) + ",100000000,0],[" + std::to_string(ask) + ",100000000,1]," +
             std::to_string(ts) + "]\n";
  }
  writeText(dir / "jsonl" / "trades.jsonl", trades);
  writeText(dir / "jsonl" / "bookticker.jsonl", bookticker);
  writeText(dir / "jsonl" / "depth.jsonl", depth);
}

bool testToxicFlowPairProbeWritesTwoLegIndicator() {
  const std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                    ("hft_backtest_toxic_flow_pair_probe_" + std::to_string(std::rand()));
  const std::filesystem::path leg0 = dir / "leg0";
  const std::filesystem::path leg1 = dir / "leg1";
  writeToxicFlowPairSession(leg0, "binance", true, 0ll);
  writeToxicFlowPairSession(leg1, "bybit", false, 5000000000ll);
  writeText(dir / "toxic_flow_pair_probe.ini",
            "[strategy]\n"
            "type=toxic_flow_pair_probe\n"
            "enabled=1\n"
            "[venue.binance_futures]\n"
            "api_slot=1\n"
            "symbols=BTCUSDT\n"
            "[venue.bybit_futures]\n"
            "api_slot=1\n"
            "symbols=BTCUSDT\n");

  hft_backtest::BacktestRunRequest request{};
  request.sessionPath = leg0;
  request.sessions.push_back(hft_backtest::BacktestSessionRequest{leg1, "bybit_futures", "BTCUSDT"});
  request.configPath = dir / "toxic_flow_pair_probe.ini";
  request.strategy = "toxic_flow_pair_probe";
  request.indicatorProfile = "toxic_flow_pair";
  request.runId = "toxic-flow-pair-probe";
  request.outputPath = dir / "backtests" / "toxic-flow-pair-probe";
  const hft_backtest::BacktestRunResult result = hft_backtest::runBacktest(request, nullptr, nullptr);
  const std::string manifest = readText(request.outputPath / "manifest.json");
  const std::string indicator = readText(request.outputPath / "strategy_indicator.jsonl");

  return expect(result.status == hft_backtest::Status::Ok, "toxic_flow_pair_probe backtest completes") &&
         expect(result.inputCoverage.strategyCycles > 0u, "toxic_flow_pair_probe was invoked") &&
         expect(result.inputCoverage.cyclesWithTrade > 0u, "toxic_flow_pair_probe sees trades") &&
         expect(result.inputCoverage.cyclesWithBookTicker > 0u, "toxic_flow_pair_probe sees bookticker") &&
         expect(result.inputCoverage.cyclesWithOrderBook > 0u, "toxic_flow_pair_probe sees orderbook") &&
         expect(manifest.find("\"strategy_indicator\": {") != std::string::npos, "manifest declares pair toxic indicator stream") &&
         expect(manifest.find("\"profile\": \"toxic_flow_pair\"") != std::string::npos, "manifest carries pair toxic profile") &&
         expect(manifest.find("\"title\": \"Toxic flow A/B\"") != std::string::npos, "manifest carries pair toxic title") &&
         expect(manifest.find("\"value_label\": \"A\"") != std::string::npos, "manifest labels leg A") &&
         expect(manifest.find("\"aux_label\": \"B\"") != std::string::npos, "manifest labels leg B") &&
         expect(manifest.find("toxic_pressure_score") == std::string::npos, "pair toxic indicator uses base row schema") &&
         expect(!indicator.empty(), "pair toxic indicator rows are written") &&
         expect(firstLineCommaCount(indicator) == 6u, "pair toxic rows keep value and aux as the two plotted legs") &&
         expect(lineCount(indicator) <= 45u, "pair toxic indicator is aggregated to one row per second");
}

bool testBasisConvergenceMetadataRequiresTwoSessions() {
  const hft_backtest::StrategyMetadata* metadata = hft_backtest::findStrategyMetadata("basis_convergence_probe");
  return expect(metadata != nullptr, "basis_convergence_probe metadata is exported") &&
         expect(metadata->configTemplateFile != nullptr &&
                    std::string_view{metadata->configTemplateFile} == "strategy_profiles/basis_convergence_probe.ini",
                "basis_convergence_probe metadata discovers strategy profile") &&
         expect(metadataDisplayNameIs(*metadata, "Базис spot/futures"),
                "basis convergence display name comes from ini metadata") &&
         expect(metadata->minSessionCount == 2u, "basis convergence probe requires spot and futures sessions") &&
         expect(metadata->maxSessionCount == 2u, "basis convergence probe allows exactly two sessions") &&
         expect(!metadataHasParam(*metadata, "source"), "basis convergence metadata keeps source auto-resolved") &&
         expect(!metadataHasParam(*metadata, "timeframe_sec"), "basis convergence metadata keeps timeframe defaulted") &&
         expect(!metadataHasParam(*metadata, "expiry_utc_ns"), "basis convergence metadata keeps expiry auto-resolved");
}

bool testToxicFlowPairProbeMetadataRequiresTwoSessionsIndicator() {
  const hft_backtest::StrategyMetadata* metadata = hft_backtest::findStrategyMetadata("toxic_flow_pair_probe");
  return expect(metadata != nullptr, "toxic_flow_pair_probe metadata is exported") &&
         expect(metadata->configTemplateFile != nullptr &&
                    std::string_view{metadata->configTemplateFile} == "strategy_profiles/toxic_flow_pair_probe.ini",
                "toxic_flow_pair_probe metadata discovers strategy profile") &&
         expect(metadataDisplayNameIs(*metadata, "Диагностика toxic flow A/B"),
                "toxic_flow_pair_probe display name comes from ini metadata") &&
         expect(metadata->minSessionCount == 2u, "toxic_flow_pair_probe uses two sessions") &&
         expect(metadata->maxSessionCount == 2u, "toxic_flow_pair_probe allows exactly two sessions") &&
         expect(metadata->paramCount == 0u, "toxic_flow_pair_probe has no strategy params") &&
         expect(metadataHasIndicator(*metadata, "toxic_flow_pair"), "toxic_flow_pair_probe exposes toxic_flow_pair indicator");
}

}  // namespace

int main() {
  bool ok = true;
  ok = testToxicFlowPairProbeWritesTwoLegIndicator() && ok;
  ok = testBasisConvergenceMetadataRequiresTwoSessions() && ok;
  ok = testToxicFlowPairProbeMetadataRequiresTwoSessionsIndicator() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
