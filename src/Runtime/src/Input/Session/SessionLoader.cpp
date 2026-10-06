#include "Internal/SessionLoaderHelpers.hpp"
#include "SessionLoader.hpp"

#include "cxet/Api/Resolve/LocalSymbolValidation.hpp"
#include "hftrec/CorpusContract/JsonLineParser.hpp"
#include "hftrec/CorpusContract/SessionManifest.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace hft_backtest::detail {
namespace session_loader {



bool readText(const std::filesystem::path& path, std::string& out) {
  std::ifstream in(path, std::ios::in | std::ios::binary);
  if (!in.is_open()) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

char lowerAscii(char c) noexcept {
  return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equalsNoCase(std::string_view lhs, std::string_view rhs) noexcept {
  if (lhs.size() != rhs.size()) return false;
  for (std::size_t i = 0u; i < lhs.size(); ++i) {
    if (lowerAscii(lhs[i]) != lowerAscii(rhs[i])) return false;
  }
  return true;
}

bool cryptoRecorderExchange(std::string_view exchange) noexcept {
  static constexpr std::string_view kExchanges[] = {
      "aster",       "backpack", "binance",  "bingx",   "bitget",
      "bitunix",     "bybit",    "coinbase", "coinex",
      "gate",        "htx",      "hyperliquid", "kraken", "kucoin",
      "mexc",        "okx",      "phemex",   "poloniex", "toobit",
      "xt",
  };
  for (const std::string_view known : kExchanges) {
    if (equalsNoCase(exchange, known)) return true;
  }
  return false;
}

bool validLocalCryptoSymbol(std::string_view symbol) noexcept {
  std::string text{symbol};
  return cxet::api::isLocalCryptoSymbolText(text.c_str());
}

bool legacyCryptoManifestSymbol(std::string_view exchange, std::string_view symbol) noexcept {
  return cryptoRecorderExchange(exchange) && !validLocalCryptoSymbol(symbol);
}




















void appendSessionWarning(std::vector<std::string>& warnings, std::string warning) {
  if (warnings.size() < kMaxSessionWarnings) {
    warnings.push_back(std::move(warning));
  } else if (warnings.size() == kMaxSessionWarnings) {
    warnings.push_back("additional session warnings suppressed");
  }
}
















std::string extractStringAfter(std::string_view text, std::string_view key) {
  const std::size_t keyPos = text.find(key);
  if (keyPos == std::string_view::npos) return {};
  const std::size_t colon = text.find(':', keyPos + key.size());
  if (colon == std::string_view::npos) return {};
  const std::size_t firstQuote = text.find('"', colon + 1);
  if (firstQuote == std::string_view::npos) return {};
  std::string out;
  bool escaped = false;
  for (std::size_t pos = firstQuote + 1u; pos < text.size(); ++pos) {
    const char c = text[pos];
    if (escaped) {
      if (c == 'n') out.push_back('\n');
      else if (c == 'r') out.push_back('\r');
      else if (c == 't') out.push_back('\t');
      else out.push_back(c);
      escaped = false;
      continue;
    }
    if (c == '\\') {
      escaped = true;
      continue;
    }
    if (c == '"') return out;
    out.push_back(c);
  }
  return {};
}

std::string_view extractObjectAfter(std::string_view text, std::string_view key) noexcept {
  const std::size_t keyPos = text.find(key);
  if (keyPos == std::string_view::npos) return {};
  std::size_t objectBegin = text.find(':', keyPos + key.size());
  if (objectBegin == std::string_view::npos) return {};
  ++objectBegin;
  skipWs(text, objectBegin);
  if (objectBegin >= text.size() || text[objectBegin] != '{') return {};
  std::uint32_t depth = 0;
  bool inString = false;
  bool escaped = false;
  for (std::size_t pos = objectBegin; pos < text.size(); ++pos) {
    const char c = text[pos];
    if (escaped) {
      escaped = false;
      continue;
    }
    if (inString) {
      if (c == '\\') escaped = true;
      else if (c == '"') inString = false;
      continue;
    }
    if (c == '"') {
      inString = true;
    } else if (c == '{') {
      ++depth;
    } else if (c == '}') {
      if (depth == 0u) return {};
      --depth;
      if (depth == 0u) return text.substr(objectBegin, pos - objectBegin + 1u);
    }
  }
  return {};
}

std::string extractChannelPath(std::string_view manifest, std::string_view channel) {
  std::string key{"\""};
  key += channel;
  key += "\"";
  const std::string_view object = extractObjectAfter(manifest, key);
  if (object.empty()) return extractStringAfter(manifest, key);
  return extractStringAfter(object, "\"path\"");
}

std::string extractChannelSidecarPath(std::string_view manifest, std::string_view channel) {
  std::string key{"\""};
  key += channel;
  key += "\"";
  const std::string_view object = extractObjectAfter(manifest, key);
  return object.empty() ? std::string{} : extractStringAfter(object, "\"sidecar_path\"");
}

std::string extractArtifactPath(std::string_view manifest, std::string_view keyName) {
  std::string key{"\""};
  key += keyName;
  key += "\"";
  const std::string_view artifacts = extractObjectAfter(manifest, "\"artifacts\"");
  if (!artifacts.empty()) return extractStringAfter(artifacts, key);
  return extractStringAfter(manifest, key);
}

void appendManifestCaptureWarnings(std::string_view manifest, std::vector<std::string>& warnings) {
  const std::string sessionHealth = extractStringAfter(manifest, "\"session_health\"");
  if (!sessionHealth.empty() && sessionHealth != "clean") {
    appendSessionWarning(warnings, "capture health: " + sessionHealth);
  }

  const std::string warningSummary = extractStringAfter(manifest, "\"warning_summary\"");
  if (!warningSummary.empty()) {
    appendSessionWarning(warnings, "capture warning: " + warningSummary);
  }
}













std::filesystem::path declaredPath(const std::filesystem::path& root, std::string_view path) {
  if (path.empty()) return {};
  return root / std::filesystem::path{std::string{path}};
}

void updateRange(SessionData& data, std::int64_t ts) noexcept {
  if (ts <= 0) return;
  if (data.firstTsNs == 0 || ts < data.firstTsNs) data.firstTsNs = ts;
  if (ts > data.lastTsNs) data.lastTsNs = ts;
}

bool extractPositiveI64Field(std::string_view text, std::string_view key, std::int64_t& out) noexcept {
  const std::size_t keyPos = text.find(key);
  if (keyPos == std::string_view::npos) return false;
  std::size_t pos = text.find(':', keyPos + key.size());
  if (pos == std::string_view::npos) return false;
  ++pos;
  skipWs(text, pos);
  if (text.compare(pos, 4, "null") == 0) return false;
  std::int64_t value = 0;
  if (!parseI64(text, pos, value) || value <= 0) return false;
  out = value;
  return true;
}

void loadInstrumentMetadata(const std::filesystem::path& path, InstrumentMetadata& out) noexcept {
  std::string text;
  if (!readText(path, text)) return;
  (void)extractPositiveI64Field(text, "\"tick_size_e8\"", out.tickSizeE8);
  (void)extractPositiveI64Field(text, "\"lot_size_e8\"", out.lotSizeE8);
  const bool canonicalStep = extractPositiveI64Field(text, "\"canonical_step_size_e8\"", out.lotSizeE8);
  const bool executableStep = extractPositiveI64Field(text, "\"execution_quantity_step_e8\"", out.executionQuantityStepE8);
  out.hasExecutionQuantityGrid = canonicalStep && executableStep;
  (void)extractPositiveI64Field(text, "\"contract_base_qty_e8\"", out.contractBaseQtyE8);
  (void)extractPositiveI64Field(text, "\"price_basis_qty_e8\"", out.priceBasisQtyE8);
  (void)extractPositiveI64Field(text, "\"expiry_utc_ns\"", out.expiryUtcNs);
}

void loadTradeHistoryManifestFields(std::string_view manifest, SessionData& out) noexcept {
  const std::string_view summary = extractObjectAfter(manifest, "\"summary\"");
  const std::string_view trades = summary.empty() ? extractObjectAfter(manifest, "\"trades\"") : extractObjectAfter(summary, "\"trades\"");
  const std::string_view source = trades.empty() ? manifest : trades;
  (void)extractPositiveI64Field(source, "\"history_warmup_sec\"", out.tradeHistoryWarmupSec);
  (void)extractPositiveI64Field(source, "\"history_requested_start_ns\"", out.tradeHistoryRequestedStartNs);
  (void)extractPositiveI64Field(source, "\"history_requested_end_ns\"", out.tradeHistoryRequestedEndNs);
  (void)extractPositiveI64Field(source, "\"history_rows\"", out.tradeHistoryRows);
}

void splitTradeWarmup(SessionData& out) {
  if (out.trades.empty()) return;
  std::vector<TradeRow> replay{};
  replay.reserve(out.trades.size());
  out.tradeWarmup.clear();
  out.tradeWarmup.reserve(out.trades.size());
  for (const TradeRow& row : out.trades) {
    if (hftrec::replay::isHistoricalBackfill(row.arrival)) out.tradeWarmup.push_back(row);
    else replay.push_back(row);
  }
  out.trades = std::move(replay);
}









void rebuildReplayRange(SessionData& out) noexcept {
  out.firstTsNs = 0;
  out.lastTsNs = 0;
  for (const TradeRow& row : out.trades) updateRange(out, row.tsNs);
  for (const BookTickerRow& row : out.bookTickers) updateRange(out, row.tsNs);
  for (const DepthRow& row : out.depths) updateRange(out, row.tsNs);
}

}  // namespace session_loader

using namespace session_loader;

Status loadSession(const std::filesystem::path& sessionPath,
                   const SessionLoadPlan& plan,
                   SessionData& out,
                   std::string& error) noexcept {
  out = SessionData{};
  out.sessionPath = sessionPath;
  if (sessionPath.empty() || !std::filesystem::is_directory(sessionPath)) {
    error = "session path is not a directory";
    return Status::InvalidArgument;
  }

  std::string manifest;
  if (!readText(sessionPath / "manifest.json", manifest)) {
    error = "missing manifest.json";
    return Status::IoError;
  }
  hftrec::capture::SessionManifest recorderManifest{};
  if (!hftrec::isOk(hftrec::capture::parseManifestJson(manifest, recorderManifest))) {
    error = "malformed or incompatible recorder manifest.json; current captured-arrival v3 contract is required";
    return Status::CorruptData;
  }
  // Recorder owns immutable historical channel tags and manifest counts.
  // Retired public feeds remain corpus evidence and are never replay inputs.
  if (plan.requireCandles && recorderManifest.exchange != "finam") {
    error = "candle history is supported only for FINAM archive sources";
    return Status::Unsupported;
  }
  if (plan.loadCandles && recorderManifest.exchange != "finam") {
    appendSessionWarning(out.warnings, "non-FINAM candle archive channel ignored");
  }
  if (!validateCurrentRecorderManifest(recorderManifest, plan, out.warnings, error)) {
    return Status::CorruptData;
  }
  out.exchange = recorderManifest.exchange;
  out.market = recorderManifest.market;
  out.symbol = recorderManifest.symbols.empty() ? std::string{} : recorderManifest.symbols.front();
  out.firstReceiveMonotonicNs = recorderManifest.arrivalClock.firstReceiveMonotonicNs;
  out.lastReceiveMonotonicNs = recorderManifest.arrivalClock.lastReceiveMonotonicNs;
  out.firstReceiveRealtimeNs = recorderManifest.arrivalClock.firstReceiveRealtimeNs;
  out.lastReceiveRealtimeNs = recorderManifest.arrivalClock.lastReceiveRealtimeNs;
  out.realtimeRegressionCount = recorderManifest.arrivalClock.realtimeRegressions;
  out.monotonicNonIncreasingCount = recorderManifest.arrivalClock.monotonicNonIncreasing;
  out.exchangeAheadOfReceiveCount = recorderManifest.arrivalClock.exchangeAheadOfReceive;
  out.exchangeTimestampMissingCount = recorderManifest.arrivalClock.exchangeTimestampMissing;
  const std::string& tradesPath = recorderManifest.tradesPath;
  const std::string& bookTickerPath = recorderManifest.bookTickerPath;
  const std::string& depthPath = recorderManifest.depthPath;
  const std::string& depthSidecarPath = recorderManifest.depthSidecarPath;
  const std::string& candlesPath = recorderManifest.candlesPath;
  const std::string& candles2Path = recorderManifest.candles2Path;
  loadTradeHistoryManifestFields(manifest, out);
  if (out.exchange.empty() || out.market.empty() || out.symbol.empty()) {
    error = "manifest identity is incomplete";
    return Status::CorruptData;
  }
  if (legacyCryptoManifestSymbol(out.exchange, out.symbol)) {
    error = "manifest identity has legacy crypto symbol; expected local symbol format BASE_QUOTE";
    return Status::CorruptData;
  }

  const std::string& metadataPath = recorderManifest.instrumentMetadataPath;
  loadInstrumentMetadata(declaredPath(sessionPath, metadataPath), out.instrument);

  if (plan.loadTrades) {
    Status status = loadJsonl<TradeRow>(declaredPath(sessionPath, tradesPath), "trades", plan.requireTrades, out.trades, parseContractTradeLine, error);
    if (!isOk(status)) return status;
    if (!validateLoadedRowCount(
            "trades", out.trades.size(), recorderManifest.tradesCount,
            error)) return Status::CorruptData;
    splitTradeWarmup(out);
  }
  if (plan.loadBookTickers) {
    Status status = loadJsonl<BookTickerRow>(declaredPath(sessionPath, bookTickerPath), "bookticker", plan.requireBookTickers, out.bookTickers, parseContractBookTickerLine, error);
    if (!isOk(status)) return status;
    if (!validateLoadedRowCount(
            "bookticker", out.bookTickers.size(),
            recorderManifest.bookTickerCount, error))
      return Status::CorruptData;
  }
  if (plan.loadDepths) {
    const std::filesystem::path tapePath = declaredPath(sessionPath, depthPath);
    const std::filesystem::path sidecarPath = declaredPath(sessionPath, depthSidecarPath);
    const Status status = loadDepthTapeSidecarJsonl(
        tapePath, sidecarPath, plan.requireDepths, out.depths, error, out.warnings);
    if (!isOk(status)) return status;
    if (!validateLoadedRowCount(
            "depth", out.depths.size(), recorderManifest.depthCount,
            error)) return Status::CorruptData;
  }
  if (plan.loadCandles && out.exchange == "finam") {
    const bool detailed = recorderManifest.candles2Enabled;
    const std::filesystem::path candlePath = declaredPath(
        sessionPath, detailed ? std::string_view{candles2Path} : std::string_view{candlesPath});
    Status status = loadJsonl<CandleRow>(candlePath,
                                         detailed ? "candles2" : "candles",
                                         plan.requireCandles,
                                         out.candles,
                                         parseContractCandleLine,
                                         error);
    if (!isOk(status)) return status;
    const std::uint64_t declaredCount = detailed
        ? recorderManifest.candles2Count
        : recorderManifest.candlesCount;
    if (!validateLoadedRowCount(
            detailed ? "candles2" : "candles",
            out.candles.size(), declaredCount, error))
      return Status::CorruptData;
  }

  if (!validateCapturedReplayRows(out, error)) return Status::CorruptData;
  out.timelineChannelSorted.fill(false);
  out.timelineOrderKnown = false;
  rebuildReplayRange(out);
  return Status::Ok;
}

Status loadSession(const std::filesystem::path& sessionPath, SessionData& out, std::string& error) noexcept {
  SessionLoadPlan plan{};
  plan.loadCandles = true;
  return loadSession(sessionPath, plan, out, error);
}

}  // namespace hft_backtest::detail
