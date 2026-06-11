#include "session_loader.hpp"
#include "backtest_engine.hpp"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

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

std::filesystem::path makeSessionDir(std::string_view name) {
  return std::filesystem::temp_directory_path() /
         ("hft_backtest_loader_" + std::string{name} + "_" + std::to_string(std::rand()));
}

void writeManifest(const std::filesystem::path& dir) {
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
  writeText(dir / "instrument_metadata.json",
            "{\"tick_size_e8\":1000000,\"lot_size_e8\":1000,\"contract_base_qty_e8\":100000000}\n");
}

bool testLoadSessionParsesCrLfAndFinalLineWithoutNewline() {
  const std::filesystem::path dir = makeSessionDir("crlf");
  writeManifest(dir);
  writeText(dir / "jsonl" / "trades.jsonl",
            "[10000000000,100000000,1,1000,1,1,1,10000000000,0,\"BTCUSDT\",\"binance\",\"futures\",1,11]\r\n"
            "[10010000000,200000000,0,2000,2,2,2,10010000000,0,\"BTCUSDT\",\"binance\",\"futures\",2,12]");
  writeText(dir / "jsonl" / "bookticker.jsonl",
            "[9900000000,100000000,10100000000,100000000,1500,3,21]");
  writeText(dir / "jsonl" / "depth.jsonl",
            "[[9900000000,100000000,1],[10100000000,100000000,0],1750,4,31]\r\n");
  writeText(dir / "jsonl" / "candles.jsonl",
            "[1,1000,10200000000,9800000000,500000000]");

  hft_backtest::detail::SessionData data{};
  hft_backtest::detail::SessionLoadPlan plan{};
  plan.loadTrades = true;
  plan.requireTrades = true;
  plan.loadBookTickers = true;
  plan.requireBookTickers = true;
  plan.loadDepths = true;
  plan.requireDepths = true;
  plan.loadCandles = true;
  plan.requireCandles = true;
  std::string error{};

  const hft_backtest::Status status = hft_backtest::detail::loadSession(dir, plan, data, error);

  return expect(status == hft_backtest::Status::Ok, "session with CRLF and final line without newline loads") &&
         expect(data.trades.size() == 2u, "two trade rows parsed") &&
         expect(data.trades[0].ingestSeq == 11, "first trade ingest seq parsed") &&
         expect(data.trades[1].priceE8 == 10010000000ll, "final no-newline trade parsed") &&
         expect(data.bookTickers.size() == 1u, "bookticker parsed") &&
         expect(data.depths.size() == 1u, "depth parsed") &&
         expect(data.candles.size() == 1u, "candle parsed") &&
         expect(data.firstTsNs == 1000, "first timestamp tracked") &&
         expect(data.lastTsNs == 2000, "last timestamp tracked");
}

bool testLoadSessionRejectsI64Overflow() {
  const std::filesystem::path dir = makeSessionDir("overflow");
  writeManifest(dir);
  writeText(dir / "jsonl" / "trades.jsonl",
            "[9223372036854775808,100000000,1,1000,1,1,1,10000000000,0,\"BTCUSDT\",\"binance\",\"futures\",1,11]\n");

  hft_backtest::detail::SessionData data{};
  hft_backtest::detail::SessionLoadPlan plan{};
  plan.loadTrades = true;
  plan.requireTrades = true;
  plan.loadBookTickers = false;
  plan.loadDepths = false;
  plan.loadCandles = false;
  std::string error{};

  const hft_backtest::Status status = hft_backtest::detail::loadSession(dir, plan, data, error);

  return expect(status == hft_backtest::Status::CorruptData, "overflowing integer rejects session") &&
         expect(error.find("trades.jsonl:1") != std::string::npos, "overflow error carries file and line");
}

bool testBuildTimelineMergesSortedChannelsInStableOrder() {
  hft_backtest::detail::SessionData session{};
  session.depths.push_back(hft_backtest::detail::DepthRow{100, 1, {}});
  session.depths.push_back(hft_backtest::detail::DepthRow{400, 4, {}});
  session.trades.push_back(hft_backtest::detail::TradeRow{0, 0, 1, 200, 2});
  session.bookTickers.push_back(hft_backtest::detail::BookTickerRow{0, 0, 0, 0, 300, 3});

  std::vector<hft_backtest::detail::TimelineEvent> out{};
  hft_backtest::detail::buildTimeline(session, out);

  return expect(out.size() == 4u, "timeline has every event") &&
         expect(out[0].kind == 0u && out[0].tsNs == 100, "depth first") &&
         expect(out[1].kind == 1u && out[1].tsNs == 200, "trade second") &&
         expect(out[2].kind == 2u && out[2].tsNs == 300, "bookticker third") &&
         expect(out[3].kind == 0u && out[3].tsNs == 400, "depth fourth");
}

bool testBuildTimelineFallbackSortsUnorderedChannel() {
  hft_backtest::detail::SessionData session{};
  session.trades.push_back(hft_backtest::detail::TradeRow{0, 0, 1, 300, 3});
  session.trades.push_back(hft_backtest::detail::TradeRow{0, 0, 1, 100, 1});
  session.bookTickers.push_back(hft_backtest::detail::BookTickerRow{0, 0, 0, 0, 200, 2});

  std::vector<hft_backtest::detail::TimelineEvent> out{};
  hft_backtest::detail::buildTimeline(session, out);

  return expect(out.size() == 3u, "fallback timeline has every event") &&
         expect(out[0].tsNs == 100 && out[0].kind == 1u, "fallback sorts first trade") &&
         expect(out[1].tsNs == 200 && out[1].kind == 2u, "fallback sorts bookticker") &&
         expect(out[2].tsNs == 300 && out[2].kind == 1u, "fallback sorts second trade");
}

}  // namespace

int main() {
  bool ok = true;
  ok = testLoadSessionParsesCrLfAndFinalLineWithoutNewline() && ok;
  ok = testLoadSessionRejectsI64Overflow() && ok;
  ok = testBuildTimelineMergesSortedChannelsInStableOrder() && ok;
  ok = testBuildTimelineFallbackSortsUnorderedChannel() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
