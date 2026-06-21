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

bool testLoadSessionParsesInstrumentPriceBasis() {
  const std::filesystem::path dir = makeSessionDir("price_basis_metadata");
  writeText(dir / "manifest.json",
            "{\n"
            "  \"identity\": {\"exchange\": \"finam\", \"market\": \"futures\", \"symbols\": [\"SRM6@RTSX\"]},\n"
            "  \"artifacts\": {\"instrument_metadata_path\": \"instrument_metadata.json\"}\n"
            "}\n");
  writeText(dir / "instrument_metadata.json",
            "{\"tick_size_e8\":1000000,\"lot_size_e8\":100000000,\"contract_base_qty_e8\":100000000,"
            "\"price_basis_qty_e8\":10000000000,\"expiry_utc_ns\":1781913600000000000}\n");

  hft_backtest::detail::SessionData data{};
  hft_backtest::detail::SessionLoadPlan plan{};
  plan.loadTrades = false;
  plan.loadBookTickers = false;
  plan.loadPriceLimits = false;
  plan.loadDepths = false;
  std::string error{};

  const hft_backtest::Status status = hft_backtest::detail::loadSession(dir, plan, data, error);

  return expect(status == hft_backtest::Status::Ok, "session with price basis metadata loads") &&
         expect(data.instrument.tickSizeE8 == 1000000, "tick size metadata parsed") &&
         expect(data.instrument.lotSizeE8 == 100000000, "lot size metadata parsed") &&
         expect(data.instrument.contractBaseQtyE8 == 100000000, "contract base metadata parsed") &&
         expect(data.instrument.priceBasisQtyE8 == 10000000000ll, "price basis metadata parsed") &&
         expect(data.instrument.expiryUtcNs == 1781913600000000000ll, "expiry metadata parsed");
}

bool testLoadSessionReadsOptionalReferenceStreams() {
  const std::filesystem::path dir = makeSessionDir("reference_streams");
  writeText(dir / "manifest.json",
            "{\n"
            "  \"identity\": {\"exchange\": \"binance\", \"market\": \"futures\", \"symbols\": [\"BTCUSDT\"]},\n"
            "  \"channels\": {\n"
            "    \"liquidations\": {\"path\": \"jsonl/liquidations.jsonl\"},\n"
            "    \"mark_price\": {\"path\": \"jsonl/mark_price.jsonl\"},\n"
            "    \"index_price\": {\"path\": \"jsonl/index_price.jsonl\"},\n"
            "    \"funding\": {\"path\": \"jsonl/funding.jsonl\"},\n"
            "    \"open_interest\": {\"path\": \"jsonl/open_interest.jsonl\"}\n"
            "  }\n"
            "}\n");
  writeText(dir / "jsonl" / "liquidations.jsonl",
            "[10000000000,200000000,1,1000,10010000000,150000000,\"BTCUSDT\",\"binance\",\"futures\",1,2,3,2,11,21]\n");
  writeText(dir / "jsonl" / "mark_price.jsonl", "[2000,10020000000]\n");
  writeText(dir / "jsonl" / "index_price.jsonl", "[2100,10000000000]\n");
  writeText(dir / "jsonl" / "funding.jsonl", "[2200,1234,2200,5800]\n");
  writeText(dir / "jsonl" / "open_interest.jsonl", "[2300,800000000,90000000000]\n");

  hft_backtest::detail::SessionData data{};
  hft_backtest::detail::SessionLoadPlan plan{};
  plan.loadTrades = false;
  plan.loadBookTickers = false;
  plan.loadDepths = false;
  plan.loadLiquidations = true;
  plan.loadMarkPrices = true;
  plan.loadIndexPrices = true;
  plan.loadFunding = true;
  plan.loadOpenInterest = true;
  std::string error{};

  const hft_backtest::Status status = hft_backtest::detail::loadSession(dir, plan, data, error);

  return expect(status == hft_backtest::Status::Ok, "optional reference streams load") &&
         expect(data.liquidations.size() == 1u, "liquidation row parsed") &&
         expect(data.liquidations[0].tsNs == 1000, "liquidation timestamp parsed") &&
         expect(data.liquidations[0].avgPriceE8 == 10010000000ll, "liquidation avg price parsed") &&
         expect(data.liquidations[0].filledQtyE8 == 150000000ll, "liquidation filled qty parsed") &&
         expect(data.liquidations[0].ingestSeq == 21, "liquidation ingest seq parsed") &&
         expect(data.markPrices.size() == 1u && data.markPrices[0].markPriceE8 == 10020000000ll, "mark price parsed") &&
         expect(data.indexPrices.size() == 1u && data.indexPrices[0].indexPriceE8 == 10000000000ll, "index price parsed") &&
         expect(data.funding.size() == 1u && data.funding[0].fundingRateE8 == 1234, "funding parsed") &&
         expect(data.openInterest.size() == 1u && data.openInterest[0].openInterestE8 == 800000000ll, "open interest parsed") &&
         expect(data.firstTsNs == 1000, "reference stream first timestamp tracked") &&
         expect(data.lastTsNs == 2300, "reference stream last timestamp tracked");
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

bool testLoadSessionSortsUnorderedTimelineChannels() {
  const std::filesystem::path dir = makeSessionDir("unordered_channels");
  writeManifest(dir);
  writeText(dir / "jsonl" / "trades.jsonl",
            "[10030000000,100000000,1,3000,1,1,1,10030000000,0,\"BTCUSDT\",\"binance\",\"futures\",3,30]\n"
            "[10010000000,100000000,0,1000,1,1,1,10010000000,0,\"BTCUSDT\",\"binance\",\"futures\",1,10]\n");
  writeText(dir / "jsonl" / "bookticker.jsonl",
            "[9900000000,100000000,10100000000,100000000,2000,2,20]\n");

  hft_backtest::detail::SessionData data{};
  hft_backtest::detail::SessionLoadPlan plan{};
  plan.loadTrades = true;
  plan.requireTrades = true;
  plan.loadBookTickers = true;
  plan.requireBookTickers = true;
  plan.loadDepths = false;
  plan.loadCandles = false;
  std::string error{};

  const hft_backtest::Status status = hft_backtest::detail::loadSession(dir, plan, data, error);

  std::vector<hft_backtest::detail::TimelineEvent> out{};
  hft_backtest::detail::buildTimeline(data, out);

  return expect(status == hft_backtest::Status::Ok, "unordered loaded session succeeds") &&
         expect(data.timelineOrderKnown, "loaded session timeline order is known") &&
         expect(data.timelineChannelSorted[hft_backtest::detail::kTimelineKindTrade], "loaded trades are marked sorted") &&
         expect(data.trades.size() == 2u, "unordered trade rows parsed") &&
         expect(data.trades[0].tsNs == 1000, "loader sorted earlier trade first") &&
         expect(data.trades[1].tsNs == 3000, "loader sorted later trade second") &&
         expect(out.size() == 3u, "timeline includes sorted loaded events") &&
         expect(out[0].tsNs == 1000 && out[0].kind == hft_backtest::detail::kTimelineKindTrade, "loaded timeline first trade") &&
         expect(out[1].tsNs == 2000 && out[1].kind == hft_backtest::detail::kTimelineKindBookTicker, "loaded timeline bookticker") &&
         expect(out[2].tsNs == 3000 && out[2].kind == hft_backtest::detail::kTimelineKindTrade, "loaded timeline second trade");
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

bool testBuildTimelineIncludesOptionalReferenceStreams() {
  hft_backtest::detail::SessionData session{};
  session.depths.push_back(hft_backtest::detail::DepthRow{100, 1, {}});
  session.trades.push_back(hft_backtest::detail::TradeRow{0, 0, 1, 200, 2});
  session.bookTickers.push_back(hft_backtest::detail::BookTickerRow{0, 0, 0, 0, 300, 3});
  session.priceLimits.push_back(hft_backtest::detail::PriceLimitRow{0, 0, 1u, 400, 4});
  session.liquidations.push_back(hft_backtest::detail::LiquidationRow{0, 0, 1, 500, 0, 0, 0, 0, 0, 0, 5});
  session.markPrices.push_back(hft_backtest::detail::MarkPriceRow{600, 0, 6});
  session.indexPrices.push_back(hft_backtest::detail::IndexPriceRow{700, 0, 7});
  session.funding.push_back(hft_backtest::detail::FundingRow{800, 0, 0, 0, 8});
  session.openInterest.push_back(hft_backtest::detail::OpenInterestRow{900, 0, 0, 9});

  std::vector<hft_backtest::detail::TimelineEvent> out{};
  hft_backtest::detail::buildTimeline(session, out);

  return expect(out.size() == 9u, "timeline has every optional reference event") &&
         expect(out[4].kind == hft_backtest::detail::kTimelineKindLiquidation && out[4].tsNs == 500, "liquidation event included") &&
         expect(out[5].kind == hft_backtest::detail::kTimelineKindMarkPrice && out[5].tsNs == 600, "mark price event included") &&
         expect(out[6].kind == hft_backtest::detail::kTimelineKindIndexPrice && out[6].tsNs == 700, "index price event included") &&
         expect(out[7].kind == hft_backtest::detail::kTimelineKindFunding && out[7].tsNs == 800, "funding event included") &&
         expect(out[8].kind == hft_backtest::detail::kTimelineKindOpenInterest && out[8].tsNs == 900, "open interest event included");
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

bool testLoadSessionPrefersCandles2ManifestPath() {
  const std::filesystem::path dir = makeSessionDir("candles2_manifest");
  writeText(dir / "manifest.json",
            "{\n"
            "  \"identity\": {\"exchange\": \"finam\", \"market\": \"spot\", \"symbols\": [\"SBER@MISX\"]},\n"
            "  \"channels\": {\n"
            "    \"candles\": {\"path\": \"jsonl/candles_15m.jsonl\"},\n"
            "    \"candles2\": {\"path\": \"jsonl/candles2_15m.jsonl\"}\n"
            "  }\n"
            "}\n");
  writeText(dir / "jsonl" / "candles_15m.jsonl", "[2,1000,10200000000,9800000000,500000000]\n");
  writeText(dir / "jsonl" / "candles2_15m.jsonl",
            "[2,1000,10000000000,10300000000,9900000000,10100000000,1000000000,101000000000]\n");

  hft_backtest::detail::SessionData data{};
  hft_backtest::detail::SessionLoadPlan plan{};
  plan.loadTrades = false;
  plan.loadBookTickers = false;
  plan.loadDepths = false;
  plan.loadCandles = true;
  plan.requireCandles = true;
  std::string error{};
  const hft_backtest::Status status = hft_backtest::detail::loadSession(dir, plan, data, error);

  return expect(status == hft_backtest::Status::Ok, "loader accepts candles2 manifest path") &&
         expect(data.candles.size() == 1u, "loader reads one candles2 row") &&
         expect(data.candles[0].tier == 2, "candles2 15m tier parsed") &&
         expect(data.candles[0].hasOhlc, "candles2 OHLC flag parsed") &&
         expect(data.candles[0].openE8 == 10000000000ll, "candles2 open parsed") &&
         expect(data.candles[0].closeE8 == 10100000000ll, "candles2 close parsed") &&
         expect(data.candles[0].quoteAmountE8 == 101000000000ll, "candles2 quote amount parsed");
}

bool testLoadSessionParsesStringDetailedCandles2Rows() {
  const std::filesystem::path dir = makeSessionDir("candles2_string");
  writeText(dir / "manifest.json",
            "{\n"
            "  \"identity\": {\"exchange\": \"finam\", \"market\": \"spot\", \"symbols\": [\"SBER@MISX\"]},\n"
            "  \"channels\": {\n"
            "    \"candles2\": {\"path\": \"jsonl/candles2_5m.jsonl\"}\n"
            "  }\n"
            "}\n");
  writeText(dir / "jsonl" / "candles2_5m.jsonl",
            "[\"finam\",\"spot\",\"SBER@MISX\",\"5m\",1000,10000000000,10300000000,9900000000,10100000000,1000000000,101000000000]\n");

  hft_backtest::detail::SessionData data{};
  hft_backtest::detail::SessionLoadPlan plan{};
  plan.loadTrades = false;
  plan.loadBookTickers = false;
  plan.loadDepths = false;
  plan.loadCandles = true;
  plan.requireCandles = true;
  std::string error{};
  const hft_backtest::Status status = hft_backtest::detail::loadSession(dir, plan, data, error);

  return expect(status == hft_backtest::Status::Ok, "loader accepts string candles2 rows") &&
         expect(data.candles.size() == 1u, "loader reads one string candles2 row") &&
         expect(data.candles[0].tier == 0, "unknown non-tier timeframe is kept as tier zero") &&
         expect(data.candles[0].hasOhlc, "string candles2 OHLC flag parsed") &&
         expect(data.candles[0].closeE8 == 10100000000ll, "string candles2 close parsed");
}

}  // namespace

int main() {
  bool ok = true;
  ok = testLoadSessionParsesInstrumentPriceBasis() && ok;
  ok = testLoadSessionReadsOptionalReferenceStreams() && ok;
  ok = testLoadSessionParsesCrLfAndFinalLineWithoutNewline() && ok;
  ok = testLoadSessionRejectsI64Overflow() && ok;
  ok = testLoadSessionSortsUnorderedTimelineChannels() && ok;
  ok = testBuildTimelineMergesSortedChannelsInStableOrder() && ok;
  ok = testBuildTimelineIncludesOptionalReferenceStreams() && ok;
  ok = testBuildTimelineFallbackSortsUnorderedChannel() && ok;
  ok = testLoadSessionPrefersCandles2ManifestPath() && ok;
  ok = testLoadSessionParsesStringDetailedCandles2Rows() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
