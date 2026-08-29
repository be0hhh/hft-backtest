#include "input/session_loader.hpp"
#include "input/binary_corpus_loader.hpp"
#include "engine/backtest_engine.hpp"
#include "hftrec/corpus_contract/BinaryMarketCorpusWriter.hpp"
#include "hftrec/corpus_contract/SessionManifest.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
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

template <std::size_t Capacity>
bool setBinaryText(std::array<char, Capacity>& output,
                   std::uint8_t& bytes,
                   std::string_view value) {
  if (value.empty() || value.size() > Capacity) return false;
  std::copy(value.begin(), value.end(), output.begin());
  bytes = static_cast<std::uint8_t>(value.size());
  return true;
}

hftrec::corpus::BinaryMarketSource binaryBookTickerSource() {
  namespace binary = hftrec::corpus;
  binary::BinaryMarketSource source{};
  source.sourceId = 1u;
  source.canonicalSymbolId = 1u;
  source.initialSourceGeneration = 7u;
  source.tickSizeRaw = 1;
  source.stepSizeRaw = 1;
  source.economicBaseAssetId = 1u;
  source.quoteAssetId = 2u;
  source.venueId = 1u;
  source.marketRaw = 1u;
  source.marketKindRaw = 2u;
  source.assetDomainRaw = 0u;
  source.priceScale = 8u;
  source.quantityScale = 8u;
  source.directoryFlags = binary::BinaryMarketDirectoryBookTicker |
      binary::BinaryMarketDirectoryTickSize;
  source.instrumentMetadataFlags = binary::BinaryMarketInstrumentStepSize;
  source.configuredChannelMask = binary::binaryMarketChannelBit(
      binary::BinaryMarketChannel::BookTicker);
  source.availableChannelMask = source.configuredChannelMask;
  source.traderReplayChannelMask = source.configuredChannelMask;
  source.compatibility[binary::binaryMarketChannelIndex(
      binary::BinaryMarketChannel::BookTicker)] =
      binary::BinaryMarketCompatibility::ExactTraderReplay;
  (void)setBinaryText(source.venue, source.venueBytes, "binance");
  (void)setBinaryText(source.market, source.marketBytes, "futures");
  (void)setBinaryText(source.canonicalSymbol, source.canonicalSymbolBytes,
                      "BTC_USDT");
  (void)setBinaryText(source.nativeSymbol, source.nativeSymbolBytes,
                      "btcusdt");
  return source;
}

hftrec::corpus::BinaryMarketRecord binaryBookTickerRecord(
    std::uint64_t frameSequence,
    std::uint64_t shardSequence,
    std::int64_t exchangeTimestampNs,
    std::int64_t receiveRealtimeNs,
    std::uint64_t receiveMonotonicNs,
    bool freshnessOnly) {
  namespace binary = hftrec::corpus;
  binary::BinaryMarketRecord record{};
  record.header.sourceId = 1u;
  record.header.flags = binary::BinaryMarketRecordTraderReplayCompatible |
      (freshnessOnly ? binary::BinaryMarketRecordBboFreshnessOnly : 0u);
  record.header.sourceGeneration = 7u;
  record.header.sessionEpoch = 1u;
  record.header.eventSequence = 1u;
  record.header.frameSequence = frameSequence;
  record.header.exchangeTimestampNs = exchangeTimestampNs;
  record.header.receiveRealtimeNs = receiveRealtimeNs;
  record.header.receiveMonotonicNs = receiveMonotonicNs;
  record.header.shardSequence = shardSequence;
  record.header.payloadBytes =
      sizeof(binary::BinaryMarketBookTickerPayload);
  record.header.sourceFlags =
      binary::BinaryMarketSourceBookTickerBidPresent |
      binary::BinaryMarketSourceBookTickerAskPresent;
  record.header.channel = binary::BinaryMarketChannel::BookTicker;
  auto* payload =
      binary::binaryMarketPayload<binary::BinaryMarketBookTickerPayload>(
          &record);
  payload->bidPriceRaw = 99;
  payload->bidQtyRaw = 1;
  payload->askPriceRaw = 101;
  payload->askQtyRaw = 1;
  return record;
}

constexpr std::int64_t kReceiveRealtimeBaseNs =
    1'900'000'000'000'000'000LL;
constexpr std::uint64_t kReceiveMonotonicBaseNs =
    900'000'000'000'000'000ULL;

hftrec::corpus::BinaryMarketSource binaryDepthSource() {
  namespace binary = hftrec::corpus;
  binary::BinaryMarketSource source = binaryBookTickerSource();
  source.directoryFlags = binary::BinaryMarketDirectoryDepth |
      binary::BinaryMarketDirectoryTickSize;
  source.configuredChannelMask = binary::binaryMarketChannelBit(
      binary::BinaryMarketChannel::Depth);
  source.availableChannelMask = source.configuredChannelMask;
  source.traderReplayChannelMask = source.configuredChannelMask;
  source.compatibility.fill(binary::BinaryMarketCompatibility::Unavailable);
  source.compatibility[binary::binaryMarketChannelIndex(
      binary::BinaryMarketChannel::Depth)] =
      binary::BinaryMarketCompatibility::ExactTraderReplay;
  return source;
}

hftrec::corpus::BinaryMarketRecord binaryDepthRebaseRecord(
    bool snapshot,
    std::uint16_t transactionPartIndex,
    std::uint16_t transactionPartCount,
    std::uint64_t eventSequence,
    std::uint64_t shardSequence,
    std::int64_t exchangeTimestampNs) {
  namespace binary = hftrec::corpus;
  binary::BinaryMarketRecord record{};
  record.header.sourceId = 1u;
  record.header.flags = binary::BinaryMarketRecordTraderReplayCompatible |
      binary::BinaryMarketRecordDepthRebase |
      (snapshot ? binary::BinaryMarketRecordDepthSnapshot
                : binary::BinaryMarketRecordDepthDelta);
  record.header.sourceGeneration = 7u;
  record.header.sessionEpoch = 1u;
  record.header.eventSequence = eventSequence;
  record.header.frameSequence = 42u;
  record.header.nativeIdentityFirst = snapshot ? 10u : 11u;
  record.header.exchangeTimestampNs = exchangeTimestampNs;
  record.header.receiveRealtimeNs = kReceiveRealtimeBaseNs + 10;
  record.header.receiveMonotonicNs = kReceiveMonotonicBaseNs + 10u;
  record.header.shardSequence = shardSequence;
  record.header.payloadBytes =
      sizeof(binary::BinaryMarketDepthChunkPayload);
  record.header.channel = binary::BinaryMarketChannel::Depth;
  record.header.nativeIdentityShape = 1u;
  auto* payload =
      binary::binaryMarketPayload<binary::BinaryMarketDepthChunkPayload>(
          &record);
  payload->sequence = snapshot
      ? binary::BinaryMarketDepthSequence{10u, 10u, 0u}
      : binary::BinaryMarketDepthSequence{11u, 11u, 10u};
  payload->totalLevelCount = 1u;
  payload->partCount = 1u;
  payload->levelCount = 1u;
  payload->frameKind = snapshot
      ? binary::BinaryMarketDepthFrameKind::Snapshot
      : binary::BinaryMarketDepthFrameKind::Delta;
  payload->state = binary::BinaryMarketDepthState::RebaseInFlight;
  payload->transactionPartIndex = transactionPartIndex;
  payload->transactionPartCount = transactionPartCount;
  payload->sourceReceiveRealtimeNs =
      static_cast<std::uint64_t>(kReceiveRealtimeBaseNs +
                                 (snapshot ? 8 : 2));
  payload->sourceFreshnessMonotonicNs =
      kReceiveMonotonicBaseNs + (snapshot ? 8u : 2u);
  payload->levels[0u] = {
      snapshot ? 100u : 101u,
      snapshot ? 10u : 5u,
      1u,
      1u,
      0u,
      0u};
  return record;
}

hftrec::replay::EventArrival applicationArrival(
    std::uint64_t sequence,
    std::int64_t exchangeTsNs) {
  hftrec::replay::EventArrival out{};
  out.receiveRealtimeNs =
      kReceiveRealtimeBaseNs + static_cast<std::int64_t>(sequence);
  out.receiveMonotonicNs = kReceiveMonotonicBaseNs + sequence;
  out.producerEpoch = 1u;
  out.sourceGeneration = 1u;
  out.sessionEpoch = 1u;
  out.frameSequence = sequence;
  out.shardSequence = sequence;
  out.sourceId = 1u;
  out.flags = hftrec::replay::EventArrivalApplicationFrame;
  if (exchangeTsNs > out.receiveRealtimeNs) {
    out.flags |= hftrec::replay::EventArrivalExchangeAheadOfReceive;
  }
  return out;
}

std::string applicationArrivalTail(std::uint64_t sequence,
                                   std::int64_t exchangeTsNs) {
  const auto arrival = applicationArrival(sequence, exchangeTsNs);
  return std::to_string(arrival.receiveRealtimeNs) + "," +
      std::to_string(arrival.receiveMonotonicNs) + "," +
      std::to_string(arrival.producerEpoch) + "," +
      std::to_string(arrival.sourceGeneration) + "," +
      std::to_string(arrival.sessionEpoch) + "," +
      std::to_string(arrival.frameSequence) + "," +
      std::to_string(arrival.shardSequence) + "," +
      std::to_string(arrival.sourceId) + ",0,0," +
      std::to_string(arrival.flags);
}

constexpr std::string_view historicalArrivalTail() noexcept {
  return "0,0,0,0,0,0,0,0,0,0,2";
}

void finalizeCapturedManifest(hftrec::capture::SessionManifest& manifest) {
  const std::uint64_t capturedRows = manifest.tradesCount +
      manifest.liquidationsCount + manifest.bookTickerCount +
      manifest.markPriceCount + manifest.indexPriceCount +
      manifest.fundingCount + manifest.priceLimitCount + manifest.depthCount;
  manifest.structurallyLoadable = true;
  manifest.exactReplayEligible = capturedRows != 0u &&
      manifest.sessionHealth == hftrec::SessionHealth::Clean &&
      manifest.sessionStatus == "complete";
  manifest.arrivalClock.capturedRows = capturedRows;
  manifest.arrivalClock.historicalRows =
      manifest.candlesCount + manifest.candles2Count;
  manifest.arrivalClock.unavailableRows = 0u;
  if (capturedRows != 0u) {
    manifest.arrivalClock.firstReceiveRealtimeNs =
        kReceiveRealtimeBaseNs + 1;
    manifest.arrivalClock.lastReceiveRealtimeNs =
        kReceiveRealtimeBaseNs + static_cast<std::int64_t>(capturedRows);
    manifest.arrivalClock.firstReceiveMonotonicNs =
        kReceiveMonotonicBaseNs + 1u;
    manifest.arrivalClock.lastReceiveMonotonicNs =
        kReceiveMonotonicBaseNs + capturedRows;
  }
}

void primeSessionClock(hft_backtest::detail::SessionData& session) {
  session.firstReceiveRealtimeNs = kReceiveRealtimeBaseNs + 1;
  session.lastReceiveRealtimeNs = kReceiveRealtimeBaseNs + 32;
  session.firstReceiveMonotonicNs = kReceiveMonotonicBaseNs + 1u;
  session.lastReceiveMonotonicNs = kReceiveMonotonicBaseNs + 32u;
}

void writeManifest(const std::filesystem::path& dir) {
  hftrec::capture::SessionManifest manifest{};
  manifest.manifestSchemaVersion = hftrec::capture::kManifestSchemaVersionCurrent;
  manifest.corpusSchemaVersion = hftrec::capture::kCorpusSchemaVersionCurrent;
  manifest.sessionId = "loader-test";
  manifest.exchange = "binance";
  manifest.market = "futures";
  manifest.symbols = {"BTC_USDT"};
  manifest.sessionStatus = "complete";
  manifest.startedAtNs = 1;
  manifest.endedAtNs = 2;
  manifest.tradesEnabled = true;
  manifest.tradesCount = 2;
  manifest.tradesRuntime.state = "live";
  manifest.tradesRuntime.required = true;
  manifest.bookTickerEnabled = true;
  manifest.bookTickerCount = 1;
  manifest.bookTickerRuntime.state = "live";
  manifest.bookTickerRuntime.required = true;
  manifest.orderbookEnabled = true;
  manifest.depthCount = 1;
  manifest.depthRuntime.state = "live";
  manifest.depthRuntime.required = true;
  manifest.candlesEnabled = true;
  manifest.candlesCount = 1;
  finalizeCapturedManifest(manifest);
  writeText(dir / "manifest.json", hftrec::capture::renderManifestJson(manifest));
  writeText(dir / "instrument_metadata.json",
            "{\"tick_size_e8\":1000000,\"lot_size_e8\":1000,\"contract_base_qty_e8\":100000000}\n");
}

template <typename Configure>
void writeCurrentManifest(const std::filesystem::path& dir,
                          std::string exchange,
                          std::string market,
                          std::string symbol,
                          Configure&& configure) {
  hftrec::capture::SessionManifest manifest{};
  manifest.manifestSchemaVersion = hftrec::capture::kManifestSchemaVersionCurrent;
  manifest.corpusSchemaVersion = hftrec::capture::kCorpusSchemaVersionCurrent;
  manifest.sessionId = "loader-test";
  manifest.exchange = std::move(exchange);
  manifest.market = std::move(market);
  manifest.symbols = {std::move(symbol)};
  manifest.sessionStatus = "complete";
  manifest.startedAtNs = 1;
  manifest.endedAtNs = 2;
  manifest.bookTickerEnabled = true;
  manifest.bookTickerCount = 1u;
  manifest.bookTickerRuntime.state = "live";
  manifest.bookTickerRuntime.required = true;
  configure(manifest);
  finalizeCapturedManifest(manifest);
  writeText(dir / "manifest.json", hftrec::capture::renderManifestJson(manifest));
}

bool testLoadSessionParsesInstrumentPriceBasis() {
  const std::filesystem::path dir = makeSessionDir("price_basis_metadata");
  writeCurrentManifest(dir, "finam", "futures", "SRM6@RTSX",
                       [](hftrec::capture::SessionManifest&) {});
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
  writeCurrentManifest(dir, "binance", "futures", "BTC_USDT", [](hftrec::capture::SessionManifest& manifest) {
    manifest.liquidationsEnabled = true;
    manifest.liquidationsCount = 1;
    manifest.liquidationsRuntime.state = "live";
    manifest.markPriceEnabled = true;
    manifest.markPriceCount = 1;
    manifest.markPriceRuntime.state = "live";
    manifest.indexPriceEnabled = true;
    manifest.indexPriceCount = 1;
    manifest.indexPriceRuntime.state = "live";
    manifest.fundingEnabled = true;
    manifest.fundingCount = 1;
    manifest.fundingRuntime.state = "live";
  });
  const std::string liquidationArrival = applicationArrivalTail(2u, 1000);
  const std::string markArrival = applicationArrivalTail(3u, 2000);
  const std::string indexArrival = applicationArrivalTail(4u, 2100);
  const std::string fundingArrival = applicationArrivalTail(5u, 2200);
  writeText(dir / "jsonl" / "liquidations.jsonl",
            "[10000000000,200000000,1,1000,10010000000,150000000,\"BTC_USDT\",\"binance\",\"futures\",1,2,3,2,11,21," +
                liquidationArrival + "]\n");
  writeText(dir / "jsonl" / "mark_price.jsonl",
            "[2000,10020000000,12,22," + markArrival + "]\n");
  writeText(dir / "jsonl" / "index_price.jsonl",
            "[2100,10000000000,13,23," + indexArrival + "]\n");
  writeText(dir / "jsonl" / "funding.jsonl",
            "[2200,1234,2200,5800,14,24," + fundingArrival + "]\n");

  hft_backtest::detail::SessionData data{};
  hft_backtest::detail::SessionLoadPlan plan{};
  plan.loadTrades = false;
  plan.loadBookTickers = false;
  plan.loadDepths = false;
  plan.loadLiquidations = true;
  plan.loadMarkPrices = true;
  plan.loadIndexPrices = true;
  plan.loadFunding = true;
  plan.loadOpenInterest = false;
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
         expect(data.openInterest.empty(), "unsupported recorder open interest is not fabricated") &&
         expect(data.firstTsNs == 1000, "reference stream first timestamp tracked") &&
         expect(data.lastTsNs == 2200, "reference stream last timestamp tracked");
}

bool testLoadSessionRejectsLegacyCryptoManifestSymbol() {
  const std::filesystem::path dir = makeSessionDir("legacy_crypto_symbol");
  writeCurrentManifest(dir, "okx", "futures", "H-USDT-SWAP",
                       [](hftrec::capture::SessionManifest&) {});

  hft_backtest::detail::SessionData data{};
  hft_backtest::detail::SessionLoadPlan plan{};
  plan.loadTrades = false;
  plan.loadBookTickers = false;
  plan.loadDepths = false;
  std::string error{};

  const hft_backtest::Status status = hft_backtest::detail::loadSession(dir, plan, data, error);

  return expect(status == hft_backtest::Status::CorruptData, "legacy crypto manifest symbol is rejected") &&
         expect(error.find("legacy crypto symbol") != std::string::npos, "legacy crypto symbol error is explicit");
}

bool testLoadSessionRejectsLegacyRecorderCorpus() {
  const std::filesystem::path dir = makeSessionDir("legacy_recorder_corpus");
  writeText(dir / "manifest.json",
            "{\n"
            "  \"manifest_schema_version\": 1,\n"
            "  \"corpus_schema_version\": 1,\n"
            "  \"identity\": {\"exchange\": \"binance\", \"market\": \"futures\", \"symbols\": [\"BTC_USDT\"]}\n"
            "}\n");

  hft_backtest::detail::SessionData data{};
  hft_backtest::detail::SessionLoadPlan plan{};
  plan.loadTrades = false;
  plan.loadBookTickers = false;
  plan.loadDepths = false;
  std::string error{};

  const hft_backtest::Status status = hft_backtest::detail::loadSession(dir, plan, data, error);

  return expect(status == hft_backtest::Status::CorruptData, "legacy recorder corpus is view-only") &&
         expect(error.find("captured-arrival v3 contract is required") != std::string::npos,
                "legacy recorder rejection explains the required contract");
}

bool testLoadSessionAcceptsUtf8LocalCryptoManifestSymbol() {
  const std::filesystem::path dir = makeSessionDir("utf8_crypto_symbol");
  writeCurrentManifest(dir, "binance", "futures", "龙虾_USDT",
                       [](hftrec::capture::SessionManifest&) {});

  hft_backtest::detail::SessionData data{};
  hft_backtest::detail::SessionLoadPlan plan{};
  plan.loadTrades = false;
  plan.loadBookTickers = false;
  plan.loadDepths = false;
  std::string error{};

  const hft_backtest::Status status = hft_backtest::detail::loadSession(dir, plan, data, error);

  return expect(status == hft_backtest::Status::Ok, "utf8 local crypto manifest symbol loads") &&
         expect(data.symbol == "龙虾_USDT", "utf8 manifest symbol is preserved") &&
         expect(error.empty(), "utf8 manifest symbol does not produce loader error");
}

bool testLoadSessionCarriesCaptureWarningsFromManifest() {
  const std::filesystem::path dir = makeSessionDir("capture_warning");
  writeCurrentManifest(dir, "bitget", "futures", "AGLD_USDT", [](hftrec::capture::SessionManifest& manifest) {
    manifest.sessionStatus = "complete_degraded";
    manifest.sessionHealth = hftrec::SessionHealth::Degraded;
    manifest.warningSummary = "reference: route status=disconnected stream=mark_price symbol=AGLD_USDT";
    manifest.bookTickerEnabled = true;
    manifest.bookTickerCount = 1;
    manifest.bookTickerRuntime.state = "live";
    manifest.bookTickerRuntime.required = true;
  });
  writeText(dir / "jsonl" / "bookticker.jsonl",
            "[1,21870000,60640000000,21900000,122950000000,1782577492350000000]\n");

  hft_backtest::detail::SessionData data{};
  hft_backtest::detail::SessionLoadPlan plan{};
  plan.loadTrades = false;
  plan.loadBookTickers = true;
  plan.loadDepths = false;
  std::string error{};

  const hft_backtest::Status status = hft_backtest::detail::loadSession(dir, plan, data, error);

  return expect(status == hft_backtest::Status::CorruptData,
                "degraded capture is rejected for exact backtest") &&
         expect(error.find("not finalized") !=
                    std::string::npos,
                "degraded capture rejection is explicit");
}

bool testLoadSessionReadsCanonicalPriceLimitRows() {
  const std::filesystem::path dir = makeSessionDir("canonical_price_limit");
  writeCurrentManifest(dir, "okx", "futures", "H_USDT", [](hftrec::capture::SessionManifest& manifest) {
    manifest.priceLimitEnabled = true;
    manifest.priceLimitCount = 2;
    manifest.priceLimitRuntime.state = "live";
    manifest.priceLimitRuntime.required = true;
  });
  const std::string firstArrival =
      applicationArrivalTail(2u, 1781522104825000000LL);
  const std::string secondArrival =
      applicationArrivalTail(3u, 1781522105025000000LL);
  writeText(dir / "jsonl" / "price_limit.jsonl",
            "[1781522104825000000,46213000,36009000,1,1,11," +
                firstArrival + "]\n" +
            "[1781522105025000000,46213000,36007000,1,2,12," +
                secondArrival + "]\n");

  hft_backtest::detail::SessionData data{};
  hft_backtest::detail::SessionLoadPlan plan{};
  plan.loadTrades = false;
  plan.loadBookTickers = false;
  plan.loadDepths = false;
  plan.loadCandles = false;
  plan.loadPriceLimits = true;
  plan.requirePriceLimits = true;
  std::string error{};

  const hft_backtest::Status status = hft_backtest::detail::loadSession(dir, plan, data, error);
  const cxet::composite::PriceLimitRuntimeV1 runtime =
      data.priceLimits.empty() ? cxet::composite::PriceLimitRuntimeV1{} : hft_backtest::detail::toRuntime(data.priceLimits[0]);

  return expect(status == hft_backtest::Status::Ok, "canonical recorder price_limit rows load") &&
         expect(data.priceLimits.size() == 2u, "price_limit rows parsed") &&
         expect(data.priceLimits[0].tsNs == 1781522104825000000ll, "price_limit timestamp parsed first") &&
         expect(data.priceLimits[0].buyLimitE8 == 46213000, "price_limit buy limit parsed first") &&
         expect(data.priceLimits[0].sellLimitE8 == 36009000, "price_limit sell limit parsed first") &&
         expect(data.priceLimits[0].enabled == 1u, "price_limit enabled parsed first") &&
         expect(data.priceLimits[1].tsNs == 1781522105025000000ll, "price_limit timestamp parsed second") &&
         expect(data.priceLimits[1].buyLimitE8 == 46213000, "price_limit buy limit parsed second") &&
         expect(data.priceLimits[1].sellLimitE8 == 36007000, "price_limit sell limit parsed second") &&
         expect(runtime.ts.raw == 1781522104825000000ull, "price_limit runtime timestamp preserved") &&
         expect(runtime.buyLimit.raw == 46213000, "price_limit runtime buy limit preserved") &&
         expect(runtime.sellLimit.raw == 36009000, "price_limit runtime sell limit preserved") &&
         expect(runtime.enabled == 1u, "price_limit runtime enabled preserved") &&
         expect(data.firstTsNs == 1781522104825000000ll, "price_limit first timestamp tracked") &&
         expect(data.lastTsNs == 1781522105025000000ll, "price_limit last timestamp tracked");
}

bool testLoadSessionParsesCrLfAndFinalLineWithoutNewline() {
  const std::filesystem::path dir = makeSessionDir("crlf");
  writeManifest(dir);
  const std::string tradeArrival1 = applicationArrivalTail(1u, 1000);
  const std::string tradeArrival2 = applicationArrivalTail(2u, 2000);
  const std::string bookArrival = applicationArrivalTail(3u, 1500);
  const std::string depthArrival = applicationArrivalTail(4u, 1750);
  writeText(dir / "jsonl" / "trades.jsonl",
            "[10000000000,100000000,1,1000,1,1,1,10000000000,0,\"BTC_USDT\",\"binance\",\"futures\",1,11," +
                tradeArrival1 + "]\r\n" +
            "[10010000000,200000000,0,2000,2,2,2,10010000000,0,\"BTC_USDT\",\"binance\",\"futures\",2,12," +
                tradeArrival2 + "]");
  writeText(dir / "jsonl" / "bookticker.jsonl",
            "[3,9900000000,100000000,10100000000,100000000,1500,\"BTC_USDT\",\"binance\",\"futures\",3,13," +
                bookArrival + "]");
  const std::string taggedTs = std::to_string((std::uint64_t{1} << 63u) | 1750u);
  writeText(dir / "jsonl" / "depth_tape.jsonl",
            "[4," + taggedTs + ",4,14," + depthArrival +
                ",9900000000,100000000,10100000000,100000000]\r\n");
  writeText(dir / "jsonl" / "depth_sidecar.jsonl",
            "[4," + taggedTs + ",1,1,0,1]\r\n");
  writeText(dir / "jsonl" / "candles.jsonl",
            "[1,1000,10000000000,10200000000,9800000000,10100000000,100000000,500000000,1,\"binance\",\"futures\",\"BTC_USDT\",\"1m\",60000000000,1,15," +
                std::string{historicalArrivalTail()} + "]");

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
            "[9223372036854775808,100000000,1,1000,1,1,1,10000000000,0,\"BTC_USDT\",\"binance\",\"futures\",1,11]\n");

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

bool testLoadSessionRejectsArrivalSummaryCountMismatch() {
  const std::filesystem::path dir = makeSessionDir("arrival_count_mismatch");
  hftrec::capture::SessionManifest manifest{};
  manifest.sessionId = "arrival-count-mismatch";
  manifest.exchange = "binance";
  manifest.market = "futures";
  manifest.symbols = {"BTC_USDT"};
  manifest.sessionStatus = "complete";
  manifest.structurallyLoadable = true;
  manifest.sessionHealth = hftrec::SessionHealth::Clean;
  manifest.exactReplayEligible = true;
  manifest.bookTickerEnabled = true;
  manifest.bookTickerCount = 1u;
  manifest.bookTickerRuntime.state = "live";
  manifest.arrivalClock.capturedRows = 2u;
  manifest.arrivalClock.firstReceiveRealtimeNs = kReceiveRealtimeBaseNs + 1;
  manifest.arrivalClock.lastReceiveRealtimeNs = kReceiveRealtimeBaseNs + 2;
  manifest.arrivalClock.firstReceiveMonotonicNs = kReceiveMonotonicBaseNs + 1u;
  manifest.arrivalClock.lastReceiveMonotonicNs = kReceiveMonotonicBaseNs + 2u;
  writeText(dir / "manifest.json", hftrec::capture::renderManifestJson(manifest));

  hft_backtest::detail::SessionData data{};
  hft_backtest::detail::SessionLoadPlan plan{};
  plan.loadTrades = false;
  plan.loadBookTickers = false;
  plan.loadPriceLimits = false;
  plan.loadDepths = false;
  std::string error{};

  const hft_backtest::Status status =
      hft_backtest::detail::loadSession(dir, plan, data, error);

  return expect(status == hft_backtest::Status::CorruptData,
                "arrival summary count mismatch rejects exact replay") &&
         expect(error.find("arrival-clock summary") != std::string::npos,
                "arrival summary mismatch has an explicit error");
}

bool testLoadSessionRejectsWrongArrivalBoundary() {
  const std::filesystem::path dir = makeSessionDir("wrong_arrival_boundary");
  writeCurrentManifest(
      dir, "binance", "futures", "BTC_USDT",
      [](hftrec::capture::SessionManifest& manifest) {
        manifest.arrivalClock.boundary = "recorder.disk-write";
      });

  hft_backtest::detail::SessionData data{};
  hft_backtest::detail::SessionLoadPlan plan{};
  plan.loadTrades = false;
  plan.loadBookTickers = false;
  plan.loadPriceLimits = false;
  plan.loadDepths = false;
  std::string error{};
  const hft_backtest::Status status =
      hft_backtest::detail::loadSession(dir, plan, data, error);

  return expect(status == hft_backtest::Status::CorruptData,
                "non-application arrival boundary fails closed") &&
         expect(error.find("malformed or incompatible recorder manifest") !=
                    std::string::npos,
                "non-canonical recorder boundary is rejected by the manifest contract");
}

bool testLoadSessionRejectsCorruptCapturedDepthPair() {
  const std::filesystem::path dir = makeSessionDir("corrupt_captured_depth");
  writeCurrentManifest(
      dir, "binance", "futures", "BTC_USDT",
      [](hftrec::capture::SessionManifest& manifest) {
        manifest.orderbookEnabled = true;
        manifest.depthCount = 1u;
        manifest.depthRuntime.state = "live";
        manifest.depthRuntime.required = true;
      });
  const std::string taggedTs =
      std::to_string((std::uint64_t{1} << 63u) | 1750u);
  const std::string arrival = applicationArrivalTail(2u, 1750);
  writeText(dir / "jsonl" / "depth_tape.jsonl",
            "[2," + taggedTs + ",2,2," + arrival +
                ",9900000000,100000000]\n");
  writeText(dir / "jsonl" / "depth_sidecar.jsonl",
            "[2," + taggedTs + ",0,2]\n");

  hft_backtest::detail::SessionData data{};
  hft_backtest::detail::SessionLoadPlan plan{};
  plan.loadTrades = false;
  plan.loadBookTickers = false;
  plan.loadPriceLimits = false;
  plan.loadDepths = true;
  plan.requireDepths = true;
  std::string error{};

  const hft_backtest::Status status =
      hft_backtest::detail::loadSession(dir, plan, data, error);

  return expect(status == hft_backtest::Status::CorruptData,
                "corrupt captured depth pair rejects the whole session") &&
         expect(error.find("depth_tape") != std::string::npos,
                "corrupt captured depth error names the tape contract");
}

bool testLoadSessionSortsUnorderedTimelineChannels() {
  const std::filesystem::path dir = makeSessionDir("unordered_channels");
  writeManifest(dir);
  const std::string tradeArrival3 = applicationArrivalTail(3u, 3000);
  const std::string tradeArrival1 = applicationArrivalTail(1u, 1000);
  const std::string bookArrival2 = applicationArrivalTail(2u, 2000);
  writeText(dir / "jsonl" / "trades.jsonl",
            "[10030000000,100000000,1,3000,1,1,1,10030000000,0,\"BTC_USDT\",\"binance\",\"futures\",3,30," +
                tradeArrival3 + "]\n" +
            "[10010000000,100000000,0,1000,1,1,1,10010000000,0,\"BTC_USDT\",\"binance\",\"futures\",1,10," +
                tradeArrival1 + "]\n");
  writeText(dir / "jsonl" / "bookticker.jsonl",
            "[2,9900000000,100000000,10100000000,100000000,2000,\"BTC_USDT\",\"binance\",\"futures\",2,20," +
                bookArrival2 + "]\n");

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
         expect(!data.timelineOrderKnown, "loader does not claim unproven source ordering") &&
         expect(!data.timelineChannelSorted[hft_backtest::detail::kTimelineKindTrade], "unordered trades are not mislabeled sorted") &&
         expect(data.trades.size() == 2u, "unordered trade rows parsed") &&
         expect(data.trades[0].tsNs == 3000, "loader preserves first captured trade row") &&
         expect(data.trades[1].tsNs == 1000, "loader preserves second captured trade row") &&
         expect(out.size() == 3u, "timeline includes sorted loaded events") &&
         expect(out[0].tsNs == 1000 && out[0].kind == hft_backtest::detail::kTimelineKindTrade, "loaded timeline first trade") &&
         expect(out[1].tsNs == 2000 && out[1].kind == hft_backtest::detail::kTimelineKindBookTicker, "loaded timeline bookticker") &&
         expect(out[2].tsNs == 3000 && out[2].kind == hft_backtest::detail::kTimelineKindTrade, "loaded timeline second trade");
}

bool testBuildTimelineMergesSortedChannelsInStableOrder() {
  hft_backtest::detail::SessionData session{};
  primeSessionClock(session);
  session.depths.push_back(hft_backtest::detail::DepthRow{
      100, 1, applicationArrival(1u, 100), {}});
  session.depths.push_back(hft_backtest::detail::DepthRow{
      400, 4, applicationArrival(4u, 400), {}});
  session.trades.push_back(hft_backtest::detail::TradeRow{
      0, 0, 1, 200, 2, applicationArrival(2u, 200)});
  session.bookTickers.push_back(hft_backtest::detail::BookTickerRow{
      0, 0, 0, 0, 300, 3, applicationArrival(3u, 300)});

  std::vector<hft_backtest::detail::TimelineEvent> out{};
  hft_backtest::detail::buildTimeline(session, out);

  return expect(out.size() == 4u, "timeline has every event") &&
         expect(out[0].kind == 0u && out[0].tsNs == 100, "depth first") &&
         expect(out[1].kind == 1u && out[1].tsNs == 200, "trade second") &&
         expect(out[2].kind == 2u && out[2].tsNs == 300, "bookticker third") &&
         expect(out[3].kind == 0u && out[3].tsNs == 400, "depth fourth");
}

bool testBuildTimelineSeparatesVenueAndCapturedDelivery() {
  hft_backtest::detail::SessionData session{};
  primeSessionClock(session);
  session.bookTickers.push_back(hft_backtest::detail::BookTickerRow{
      99, 1, 101, 1, 1'000, 1, applicationArrival(5u, 1'000)});

  std::vector<hft_backtest::detail::TimelineEvent> out{};
  hft_backtest::detail::buildTimeline(session, out);

  return expect(out.size() == 1u, "timeline contains captured BBO") &&
         expect(out[0].exchangeTsNs == 1'000,
                "exchange timestamp remains explicit evidence") &&
         expect(out[0].tsNs == 1'000,
                "venue plane is projected from exchange time") &&
         expect(out[0].deliveryTsNs == kReceiveRealtimeBaseNs + 5,
                "strategy delivery uses captured monotonic arrival") &&
         expect(out[0].deliveryTsNs > out[0].tsNs,
                "venue event precedes local strategy visibility");
}

bool testBuildTimelineKeepsBboFreshnessOnApplicationPlane() {
  hft_backtest::detail::SessionData session{};
  primeSessionClock(session);
  session.bookTickers.push_back(hft_backtest::detail::BookTickerRow{
      99, 1, 101, 1, 1'000, 1, applicationArrival(5u, 1'000),
      true, true, true});

  std::vector<hft_backtest::detail::TimelineEvent> out{};
  hft_backtest::detail::buildTimeline(session, out);

  return expect(out.size() == 1u,
                "timeline contains captured BBO freshness evidence") &&
         expect(out[0].applicationOnly,
                "BBO freshness is application-only") &&
         expect(out[0].tsNs == out[0].deliveryTsNs,
                "BBO freshness cannot advance the venue plane before arrival") &&
         expect(out[0].exchangeTsNs == 1'000,
                "BBO freshness preserves exchange timestamp evidence") &&
         expect(!out[0].venueScheduleAdjusted,
                "application-only freshness is not labeled a venue adjustment");
}

bool testBinaryCorpusPreservesCaughtUpBboFreshness() {
  namespace binary = hftrec::corpus;
  const std::filesystem::path dir =
      makeSessionDir("binary_bbo_freshness");
  std::error_code cleanupError;
  std::filesystem::remove_all(dir, cleanupError);

  const std::array sources{binaryBookTickerSource()};
  binary::BinaryMarketCorpusWriter writer{};
  binary::BinaryMarketWriterConfig config{};
  config.root = dir;
  config.sources =
      std::span<const binary::BinaryMarketSource>{sources};
  config.producerEpoch = 1u;
  config.maximumBytes = 1024u * 1024u;
  config.targetDurationNs = 10u;
  config.startedReceiveNs = kReceiveRealtimeBaseNs;
  config.startedMonotonicNs = kReceiveMonotonicBaseNs;
  config.ringCapacity = 2u;
  config.shardCount = 1u;
  if (writer.start(config) != hftrec::Status::Ok ||
      writer.append(binaryBookTickerRecord(
          1u, 1u, 1'000, kReceiveRealtimeBaseNs + 1,
          kReceiveMonotonicBaseNs + 1u, false)) != hftrec::Status::Ok ||
      writer.append(binaryBookTickerRecord(
          2u, 2u, 1'100, kReceiveRealtimeBaseNs + 2,
          kReceiveMonotonicBaseNs + 2u, true)) != hftrec::Status::Ok ||
      writer.finalize(binary::BinaryMarketStopReason::Requested,
                      kReceiveRealtimeBaseNs + 3) != hftrec::Status::Ok) {
    std::filesystem::remove_all(dir, cleanupError);
    return expect(false, "binary BBO freshness fixture is writable");
  }

  hft_backtest::BacktestCorpusSourceRequest sourceRequest{};
  sourceRequest.exchange = "binance";
  sourceRequest.market = "futures";
  sourceRequest.canonicalSymbol = "BTC_USDT";
  sourceRequest.beginReceiveNs = kReceiveRealtimeBaseNs;
  sourceRequest.endReceiveNs = kReceiveRealtimeBaseNs + 3;
  hft_backtest::detail::SessionLoadPlan plan{};
  plan.loadTrades = false;
  plan.loadBookTickers = true;
  plan.loadPriceLimits = false;
  plan.requireBookTickers = true;
  hft_backtest::detail::SessionData session{};
  std::string error{};
  const hft_backtest::Status status =
      hft_backtest::detail::loadBinaryMarketCorpus(
          dir, sourceRequest, plan, session, error);
  std::vector<hft_backtest::detail::TimelineEvent> timeline{};
  if (status == hft_backtest::Status::Ok) {
    hft_backtest::detail::buildTimeline(session, timeline);
  }

  const bool ok =
      expect(status == hft_backtest::Status::Ok,
             "binary corpus accepts caught-up BBO freshness") &&
      expect(session.bookTickers.size() == 2u,
             "BBO state and freshness evidence are both retained") &&
      expect(!session.bookTickers.empty() &&
                 !session.bookTickers.front().freshnessOnly,
             "first retained BBO installs state") &&
      expect(session.bookTickers.size() == 2u &&
                 session.bookTickers.back().freshnessOnly,
             "equal-sequence heartbeat remains freshness-only") &&
      expect(timeline.size() == 2u && timeline.back().applicationOnly,
             "caught-up BBO heartbeat stays off the venue plane") &&
      expect(timeline.size() == 2u &&
                 timeline.back().tsNs == timeline.back().deliveryTsNs,
             "heartbeat is scheduled at captured local arrival");
  std::filesystem::remove_all(dir, cleanupError);
  return ok;
}

bool testBinaryCorpusLoadsCompleteAtomicDepthRebase() {
  namespace binary = hftrec::corpus;
  const std::filesystem::path dir =
      makeSessionDir("binary_depth_rebase_complete");
  std::error_code cleanupError;
  std::filesystem::remove_all(dir, cleanupError);

  const std::array sources{binaryDepthSource()};
  binary::BinaryMarketCorpusWriter writer{};
  binary::BinaryMarketWriterConfig config{};
  config.root = dir;
  config.sources =
      std::span<const binary::BinaryMarketSource>{sources};
  config.producerEpoch = 1u;
  config.maximumBytes = 1024u * 1024u;
  config.targetDurationNs = 10u;
  config.startedReceiveNs = kReceiveRealtimeBaseNs;
  config.startedMonotonicNs = kReceiveMonotonicBaseNs;
  config.ringCapacity = 2u;
  config.shardCount = 1u;
  if (writer.start(config) != hftrec::Status::Ok ||
      writer.append(binaryDepthRebaseRecord(
          true, 0u, 2u, 10u, 1u, 1'000)) != hftrec::Status::Ok ||
      writer.append(binaryDepthRebaseRecord(
          false, 1u, 2u, 11u, 2u, 900)) != hftrec::Status::Ok ||
      writer.finalize(binary::BinaryMarketStopReason::Requested,
                      kReceiveRealtimeBaseNs + 11) != hftrec::Status::Ok) {
    std::filesystem::remove_all(dir, cleanupError);
    return expect(false, "complete binary depth rebase fixture is writable");
  }

  hft_backtest::BacktestCorpusSourceRequest sourceRequest{};
  sourceRequest.exchange = "binance";
  sourceRequest.market = "futures";
  sourceRequest.canonicalSymbol = "BTC_USDT";
  sourceRequest.beginReceiveNs = kReceiveRealtimeBaseNs;
  sourceRequest.endReceiveNs = kReceiveRealtimeBaseNs + 11;
  hft_backtest::detail::SessionLoadPlan plan{};
  plan.loadTrades = false;
  plan.loadBookTickers = false;
  plan.loadPriceLimits = false;
  plan.loadDepths = true;
  plan.requireDepths = true;
  hft_backtest::detail::SessionData session{};
  std::string error{};
  const hft_backtest::Status status =
      hft_backtest::detail::loadBinaryMarketCorpus(
          dir, sourceRequest, plan, session, error);
  std::vector<hft_backtest::detail::TimelineEvent> timeline{};
  if (status == hft_backtest::Status::Ok) {
    hft_backtest::detail::buildTimeline(session, timeline);
  }

  const bool ok =
      expect(status == hft_backtest::Status::Ok,
             "complete binary depth rebase loads") &&
      expect(session.depths.size() == 2u,
             "snapshot and replay delta are both retained") &&
      expect(session.depths.size() == 2u &&
                 session.depths[0].resetBookBeforeApply &&
                 !session.depths[1].resetBookBeforeApply,
             "rebase resets once before applying its delta") &&
      expect(session.depths.size() == 2u &&
                 session.depths[0].applicationAtomicRebase &&
                 session.depths[1].applicationAtomicRebase,
             "every rebase part carries atomic application semantics") &&
      expect(timeline.size() == 2u &&
                 timeline[0].applicationAtomicRebase &&
                 timeline[1].applicationAtomicRebase,
             "timeline preserves the atomic rebase marker") &&
      expect(timeline.size() == 2u &&
                 timeline[0].tsNs == timeline[0].deliveryTsNs &&
                 timeline[1].tsNs == timeline[1].deliveryTsNs &&
                 timeline[0].tsNs == timeline[1].tsNs,
             "rebase parts share captured application delivery") &&
      expect(timeline.size() == 2u &&
                 timeline[0].exchangeTsNs == 1'000 &&
                 timeline[1].exchangeTsNs == 900,
             "original exchange timestamps remain evidence") &&
      expect(timeline.size() == 2u &&
                 timeline[0].shardSequence == 1u &&
                 timeline[1].shardSequence == 2u,
             "publication order wins over reversed exchange time");
  std::filesystem::remove_all(dir, cleanupError);
  return ok;
}

bool testBinaryCorpusRejectsIncompleteAtomicDepthRebase() {
  namespace binary = hftrec::corpus;
  const std::filesystem::path dir =
      makeSessionDir("binary_depth_rebase_incomplete");
  std::error_code cleanupError;
  std::filesystem::remove_all(dir, cleanupError);

  const std::array sources{binaryDepthSource()};
  binary::BinaryMarketCorpusWriter writer{};
  binary::BinaryMarketWriterConfig config{};
  config.root = dir;
  config.sources =
      std::span<const binary::BinaryMarketSource>{sources};
  config.producerEpoch = 1u;
  config.maximumBytes = 1024u * 1024u;
  config.targetDurationNs = 10u;
  config.startedReceiveNs = kReceiveRealtimeBaseNs;
  config.startedMonotonicNs = kReceiveMonotonicBaseNs;
  config.ringCapacity = 2u;
  config.shardCount = 1u;
  if (writer.start(config) != hftrec::Status::Ok ||
      writer.append(binaryDepthRebaseRecord(
          true, 0u, 2u, 10u, 1u, 1'000)) != hftrec::Status::Ok ||
      writer.finalize(binary::BinaryMarketStopReason::Requested,
                      kReceiveRealtimeBaseNs + 11) != hftrec::Status::Ok) {
    std::filesystem::remove_all(dir, cleanupError);
    return expect(false, "incomplete binary depth rebase fixture is writable");
  }

  hft_backtest::BacktestCorpusSourceRequest sourceRequest{};
  sourceRequest.exchange = "binance";
  sourceRequest.market = "futures";
  sourceRequest.canonicalSymbol = "BTC_USDT";
  sourceRequest.beginReceiveNs = kReceiveRealtimeBaseNs;
  sourceRequest.endReceiveNs = kReceiveRealtimeBaseNs + 11;
  hft_backtest::detail::SessionLoadPlan plan{};
  plan.loadTrades = false;
  plan.loadBookTickers = false;
  plan.loadPriceLimits = false;
  plan.loadDepths = true;
  plan.requireDepths = true;
  hft_backtest::detail::SessionData session{};
  std::string error{};
  const hft_backtest::Status status =
      hft_backtest::detail::loadBinaryMarketCorpus(
          dir, sourceRequest, plan, session, error);

  const bool ok =
      expect(status == hft_backtest::Status::CorruptData,
             "incomplete binary depth rebase fails closed") &&
      expect(error.find("rebase transaction is incomplete") !=
                 std::string::npos,
             "incomplete rebase has an explicit contract error");
  std::filesystem::remove_all(dir, cleanupError);
  return ok;
}

bool testBuildTimelineIncludesOptionalReferenceStreams() {
  hft_backtest::detail::SessionData session{};
  primeSessionClock(session);
  session.depths.push_back(hft_backtest::detail::DepthRow{
      100, 1, applicationArrival(1u, 100), {}});
  session.trades.push_back(hft_backtest::detail::TradeRow{
      0, 0, 1, 200, 2, applicationArrival(2u, 200)});
  session.bookTickers.push_back(hft_backtest::detail::BookTickerRow{
      0, 0, 0, 0, 300, 3, applicationArrival(3u, 300)});
  session.priceLimits.push_back(hft_backtest::detail::PriceLimitRow{
      400, 0, 0, 1u, 4, applicationArrival(4u, 400)});
  session.liquidations.push_back(hft_backtest::detail::LiquidationRow{
      0, 0, 1, 500, 0, 0, 0, 0, 0, 0, 5,
      applicationArrival(5u, 500)});
  session.markPrices.push_back(hft_backtest::detail::MarkPriceRow{
      600, 0, 6, applicationArrival(6u, 600)});
  session.indexPrices.push_back(hft_backtest::detail::IndexPriceRow{
      700, 0, 7, applicationArrival(7u, 700)});
  session.funding.push_back(hft_backtest::detail::FundingRow{
      800, 0, 0, 0, 8, applicationArrival(8u, 800)});
  session.openInterest.push_back(hft_backtest::detail::OpenInterestRow{
      900, 0, 0, 9, applicationArrival(9u, 900)});

  std::vector<hft_backtest::detail::TimelineEvent> out{};
  hft_backtest::detail::buildTimeline(session, out);

  return expect(out.size() == 8u, "timeline includes only captured-arrival contract channels") &&
         expect(out[4].kind == hft_backtest::detail::kTimelineKindLiquidation && out[4].tsNs == 500, "liquidation event included") &&
         expect(out[5].kind == hft_backtest::detail::kTimelineKindMarkPrice && out[5].tsNs == 600, "mark price event included") &&
         expect(out[6].kind == hft_backtest::detail::kTimelineKindIndexPrice && out[6].tsNs == 700, "index price event included") &&
         expect(out[7].kind == hft_backtest::detail::kTimelineKindFunding && out[7].tsNs == 800, "funding event included");
}

bool testBuildTimelineFallbackSortsUnorderedChannel() {
  hft_backtest::detail::SessionData session{};
  primeSessionClock(session);
  session.trades.push_back(hft_backtest::detail::TradeRow{
      0, 0, 1, 300, 3, applicationArrival(3u, 300)});
  session.trades.push_back(hft_backtest::detail::TradeRow{
      0, 0, 1, 100, 1, applicationArrival(1u, 100)});
  session.bookTickers.push_back(hft_backtest::detail::BookTickerRow{
      0, 0, 0, 0, 200, 2, applicationArrival(2u, 200)});

  std::vector<hft_backtest::detail::TimelineEvent> out{};
  hft_backtest::detail::buildTimeline(session, out);

  return expect(out.size() == 3u, "fallback timeline has every event") &&
         expect(out[0].tsNs == 100 && out[0].kind == 1u, "fallback sorts first trade") &&
         expect(out[1].tsNs == 200 && out[1].kind == 2u, "fallback sorts bookticker") &&
         expect(out[2].tsNs == 300 && out[2].kind == 1u, "fallback sorts second trade");
}

bool testLoadSessionPrefersCandles2ManifestPath() {
  const std::filesystem::path dir = makeSessionDir("candles2_manifest");
  writeCurrentManifest(dir, "finam", "spot", "SBER@MISX", [](hftrec::capture::SessionManifest& manifest) {
    manifest.candlesEnabled = true;
    manifest.candlesCount = 1;
    manifest.candlesPath = "jsonl/candles_15m.jsonl";
    manifest.candles2Enabled = true;
    manifest.candles2Count = 1;
    manifest.candles2Path = "jsonl/candles2_15m.jsonl";
  });
  writeText(dir / "jsonl" / "candles_15m.jsonl",
            "[2,1000,10000000000,10200000000,9800000000,10100000000,1000000000,500000000,1,\"finam\",\"spot\",\"SBER@MISX\",\"15m\",900000000000,1,1," +
                std::string{historicalArrivalTail()} + "]\n");
  writeText(dir / "jsonl" / "candles2_15m.jsonl",
            "[2,1000,10000000000,10300000000,9900000000,10100000000,1000000000,101000000000,1,\"finam\",\"spot\",\"SBER@MISX\",\"15m\",900000000000,2,2," +
                std::string{historicalArrivalTail()} + "]\n");

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

bool testLoadSessionRejectsUnsupportedDetailedCandleTimeframe() {
  const std::filesystem::path dir = makeSessionDir("candles2_string");
  writeCurrentManifest(dir, "finam", "spot", "SBER@MISX", [](hftrec::capture::SessionManifest& manifest) {
    manifest.candles2Enabled = true;
    manifest.candles2Count = 1;
    manifest.candles2Path = "jsonl/candles2_5m.jsonl";
  });
  writeText(dir / "jsonl" / "candles2_5m.jsonl",
            "[0,1000,10000000000,10300000000,9900000000,10100000000,1000000000,101000000000,1,\"finam\",\"spot\",\"SBER@MISX\",\"5m\",300000000000,1,1," +
                std::string{historicalArrivalTail()} + "]\n");

  hft_backtest::detail::SessionData data{};
  hft_backtest::detail::SessionLoadPlan plan{};
  plan.loadTrades = false;
  plan.loadBookTickers = false;
  plan.loadDepths = false;
  plan.loadCandles = true;
  plan.requireCandles = true;
  std::string error{};
  const hft_backtest::Status status = hft_backtest::detail::loadSession(dir, plan, data, error);

  return expect(status == hft_backtest::Status::CorruptData,
                "unsupported candle timeframe fails closed") &&
         expect(error.find("candles2_5m.jsonl:1") != std::string::npos,
                "unsupported candle error identifies the row");
}

}  // namespace

int main() {
  bool ok = true;
  ok = expect(hft_backtest::hft_backtest_api_version() == hft_backtest::kBacktestApiVersion,
              "backtest shared-library API version matches its public header") && ok;
  ok = testLoadSessionParsesInstrumentPriceBasis() && ok;
  ok = testLoadSessionReadsOptionalReferenceStreams() && ok;
  ok = testLoadSessionRejectsLegacyCryptoManifestSymbol() && ok;
  ok = testLoadSessionRejectsLegacyRecorderCorpus() && ok;
  ok = testLoadSessionAcceptsUtf8LocalCryptoManifestSymbol() && ok;
  ok = testLoadSessionCarriesCaptureWarningsFromManifest() && ok;
  ok = testLoadSessionReadsCanonicalPriceLimitRows() && ok;
  ok = testLoadSessionParsesCrLfAndFinalLineWithoutNewline() && ok;
  ok = testLoadSessionRejectsI64Overflow() && ok;
  ok = testLoadSessionRejectsArrivalSummaryCountMismatch() && ok;
  ok = testLoadSessionRejectsWrongArrivalBoundary() && ok;
  ok = testLoadSessionRejectsCorruptCapturedDepthPair() && ok;
  ok = testLoadSessionSortsUnorderedTimelineChannels() && ok;
  ok = testBuildTimelineMergesSortedChannelsInStableOrder() && ok;
  ok = testBuildTimelineSeparatesVenueAndCapturedDelivery() && ok;
  ok = testBuildTimelineKeepsBboFreshnessOnApplicationPlane() && ok;
  ok = testBinaryCorpusPreservesCaughtUpBboFreshness() && ok;
  ok = testBinaryCorpusLoadsCompleteAtomicDepthRebase() && ok;
  ok = testBinaryCorpusRejectsIncompleteAtomicDepthRebase() && ok;
  ok = testBuildTimelineIncludesOptionalReferenceStreams() && ok;
  ok = testBuildTimelineFallbackSortsUnorderedChannel() && ok;
  ok = testLoadSessionPrefersCandles2ManifestPath() && ok;
  ok = testLoadSessionRejectsUnsupportedDetailedCandleTimeframe() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
