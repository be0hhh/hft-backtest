#include "session_loader.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <sstream>
#include <string_view>

namespace hft_backtest::detail {
namespace {

bool readText(const std::filesystem::path& path, std::string& out) {
  std::ifstream in(path, std::ios::in | std::ios::binary);
  if (!in.is_open()) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
}

void skipWs(std::string_view text, std::size_t& pos) noexcept {
  while (pos < text.size()) {
    const char c = text[pos];
    if (c != ' ' && c != '\n' && c != '\r' && c != '\t') break;
    ++pos;
  }
}

bool consume(std::string_view text, std::size_t& pos, char expected) noexcept {
  skipWs(text, pos);
  if (pos >= text.size() || text[pos] != expected) return false;
  ++pos;
  return true;
}

bool parseI64(std::string_view text, std::size_t& pos, std::int64_t& out) noexcept {
  skipWs(text, pos);
  const char* first = text.data() + pos;
  const char* last = text.data() + text.size();
  const auto [ptr, ec] = std::from_chars(first, last, out);
  if (ec != std::errc{}) return false;
  pos = static_cast<std::size_t>(ptr - text.data());
  return true;
}

bool parseLevel(std::string_view text, std::size_t& pos, PriceLevelRow& out) noexcept {
  return consume(text, pos, '[') && parseI64(text, pos, out.priceE8) && consume(text, pos, ',') &&
         parseI64(text, pos, out.qtyE8) && consume(text, pos, ',') && parseI64(text, pos, out.side) &&
         (out.side == 0 || out.side == 1) && consume(text, pos, ']');
}

bool parseTradeLine(std::string_view line, TradeRow& out) noexcept {
  std::size_t pos = 0;
  out = TradeRow{};
  if (!consume(line, pos, '[')) return false;
  if (!parseI64(line, pos, out.priceE8) || !consume(line, pos, ',')) return false;
  if (!parseI64(line, pos, out.qtyE8) || !consume(line, pos, ',')) return false;
  if (!parseI64(line, pos, out.side) || (out.side != 0 && out.side != 1) || !consume(line, pos, ',')) return false;
  if (!parseI64(line, pos, out.tsNs)) return false;
  for (int comma = 0; comma < 8 && consume(line, pos, ','); ++comma) {
    std::int64_t ignored = 0;
    if (comma == 7) {
      (void)parseI64(line, pos, out.ingestSeq);
    } else if (line[pos] == '"') {
      ++pos;
      while (pos < line.size() && line[pos] != '"') ++pos;
      if (pos < line.size()) ++pos;
    } else {
      (void)parseI64(line, pos, ignored);
      if (line.compare(pos, 4, "true") == 0) pos += 4;
      else if (line.compare(pos, 5, "false") == 0) pos += 5;
    }
  }
  return true;
}

bool parseBookTickerLine(std::string_view line, BookTickerRow& out) noexcept {
  std::size_t pos = 0;
  out = BookTickerRow{};
  if (!consume(line, pos, '[') || !parseI64(line, pos, out.bidPriceE8) || !consume(line, pos, ',') ||
      !parseI64(line, pos, out.bidQtyE8) || !consume(line, pos, ',') || !parseI64(line, pos, out.askPriceE8) ||
      !consume(line, pos, ',') || !parseI64(line, pos, out.askQtyE8) || !consume(line, pos, ',') ||
      !parseI64(line, pos, out.tsNs)) return false;
  std::int64_t captureSeq = 0;
  if (consume(line, pos, ',') && (!parseI64(line, pos, captureSeq) || !consume(line, pos, ',') || !parseI64(line, pos, out.ingestSeq))) return false;
  return true;
}

bool parseCandleLine(std::string_view line, CandleRow& out) noexcept {
  std::size_t pos = 0;
  out = CandleRow{};
  if (!consume(line, pos, '[') || !parseI64(line, pos, out.tier) || !consume(line, pos, ',') ||
      !parseI64(line, pos, out.tsNs) || !consume(line, pos, ',') || !parseI64(line, pos, out.highE8) ||
      !consume(line, pos, ',') || !parseI64(line, pos, out.lowE8) || !consume(line, pos, ',') ||
      !parseI64(line, pos, out.quoteAmountE8)) return false;
  return (out.tier >= 1 && out.tier <= 3 && out.tsNs > 0 && out.highE8 > 0 && out.lowE8 > 0 &&
          out.highE8 >= out.lowE8 && out.quoteAmountE8 >= 0);
}

bool parseBookDocument(std::string_view line, std::vector<PriceLevelRow>& levels, std::int64_t& tsNs, std::int64_t* ingestSeq = nullptr) noexcept {
  std::size_t pos = 0;
  levels.clear();
  if (!consume(line, pos, '[')) return false;
  while (true) {
    skipWs(line, pos);
    if (pos >= line.size()) return false;
    if (line[pos] != '[') break;
    PriceLevelRow level{};
    if (!parseLevel(line, pos, level)) return false;
    levels.push_back(level);
    if (!consume(line, pos, ',')) return false;
  }
  if (!parseI64(line, pos, tsNs)) return false;
  std::int64_t captureSeq = 0;
  std::int64_t parsedIngestSeq = 0;
  if (consume(line, pos, ',') && (!parseI64(line, pos, captureSeq) || !consume(line, pos, ',') || !parseI64(line, pos, parsedIngestSeq))) return false;
  if (ingestSeq) *ingestSeq = parsedIngestSeq;
  return true;
}

template<typename Row, typename ParseFn>
Status loadJsonl(const std::filesystem::path& path,
                 std::string_view channel,
                 bool required,
                 std::vector<Row>& out,
                 ParseFn parse,
                 std::string& error) {
  out.clear();
  if (!std::filesystem::exists(path)) {
    if (required) {
      error = "missing required ";
      error += channel;
      error += " channel: ";
      error += path.generic_string();
      return Status::CorruptData;
    }
    return Status::Ok;
  }
  std::ifstream in(path);
  if (!in.is_open()) {
    error = "failed to open ";
    error += path.generic_string();
    return Status::IoError;
  }
  std::string line;
  std::uint64_t lineNo = 0;
  while (std::getline(in, line)) {
    ++lineNo;
    if (line.empty()) continue;
    Row row{};
    if (!parse(std::string_view{line}, row)) {
      error = "failed to parse ";
      error += path.generic_string();
      error += ":";
      error += std::to_string(lineNo);
      return Status::CorruptData;
    }
    out.push_back(std::move(row));
  }
  if (required && out.empty()) {
    error = "required ";
    error += channel;
    error += " channel is empty: ";
    error += path.generic_string();
    return Status::CorruptData;
  }
  return Status::Ok;
}

std::string extractStringAfter(std::string_view text, std::string_view key) {
  const std::size_t keyPos = text.find(key);
  if (keyPos == std::string_view::npos) return {};
  const std::size_t colon = text.find(':', keyPos + key.size());
  if (colon == std::string_view::npos) return {};
  const std::size_t firstQuote = text.find('"', colon + 1);
  if (firstQuote == std::string_view::npos) return {};
  const std::size_t secondQuote = text.find('"', firstQuote + 1);
  if (secondQuote == std::string_view::npos) return {};
  return std::string{text.substr(firstQuote + 1, secondQuote - firstQuote - 1)};
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

std::string extractArtifactPath(std::string_view manifest, std::string_view keyName) {
  std::string key{"\""};
  key += keyName;
  key += "\"";
  const std::string_view artifacts = extractObjectAfter(manifest, "\"artifacts\"");
  if (!artifacts.empty()) return extractStringAfter(artifacts, key);
  return extractStringAfter(manifest, key);
}

std::filesystem::path existingPath(const std::filesystem::path& root, std::string_view path, std::string_view fallback) {
  const std::filesystem::path declared = root / std::filesystem::path{std::string{path}};
  if (!path.empty() && std::filesystem::exists(declared)) return declared;
  const std::filesystem::path direct = root / std::filesystem::path{std::string{fallback}};
  if (std::filesystem::exists(direct)) return direct;
  return root / "jsonl" / std::filesystem::path{std::string{fallback}};
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
  (void)extractPositiveI64Field(text, "\"contract_base_qty_e8\"", out.contractBaseQtyE8);
}

void loadTradeHistoryManifestFields(std::string_view manifest, SessionData& out) noexcept {
  const std::string_view trades = extractObjectAfter(manifest, "\"trades\"");
  const std::string_view source = trades.empty() ? manifest : trades;
  (void)extractPositiveI64Field(source, "\"history_warmup_sec\"", out.tradeHistoryWarmupSec);
  (void)extractPositiveI64Field(source, "\"history_requested_start_ns\"", out.tradeHistoryRequestedStartNs);
  (void)extractPositiveI64Field(source, "\"history_requested_end_ns\"", out.tradeHistoryRequestedEndNs);
  (void)extractPositiveI64Field(source, "\"history_rows\"", out.tradeHistoryRows);
}

void splitTradeWarmup(SessionData& out) {
  if (out.tradeHistoryRequestedEndNs <= 0 || out.trades.empty()) return;
  std::vector<TradeRow> replay{};
  replay.reserve(out.trades.size());
  out.tradeWarmup.clear();
  out.tradeWarmup.reserve(out.trades.size());
  for (const TradeRow& row : out.trades) {
    if (row.tsNs > 0 && row.tsNs < out.tradeHistoryRequestedEndNs) out.tradeWarmup.push_back(row);
    else replay.push_back(row);
  }
  out.trades = std::move(replay);
}

void rebuildReplayRange(SessionData& out) noexcept {
  out.firstTsNs = 0;
  out.lastTsNs = 0;
  if (out.hasSnapshot) updateRange(out, out.snapshot.tsNs);
  for (const TradeRow& row : out.trades) updateRange(out, row.tsNs);
  for (const BookTickerRow& row : out.bookTickers) updateRange(out, row.tsNs);
  for (const DepthRow& row : out.depths) updateRange(out, row.tsNs);
}

}  // namespace

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
  out.exchange = extractStringAfter(manifest, "\"exchange\"");
  out.market = extractStringAfter(manifest, "\"market\"");
  out.symbol = extractStringAfter(manifest, "\"symbols\"");
  const std::string tradesPath = extractChannelPath(manifest, "trades");
  const std::string bookTickerPath = extractChannelPath(manifest, "bookticker");
  const std::string depthPath = extractChannelPath(manifest, "depth");
  const std::string candlesPath = extractChannelPath(manifest, "candles");
  loadTradeHistoryManifestFields(manifest, out);
  if (out.exchange.empty() || out.market.empty() || out.symbol.empty()) {
    error = "manifest identity is incomplete";
    return Status::CorruptData;
  }

  const std::string metadataPath = extractArtifactPath(manifest, "instrument_metadata_path");
  loadInstrumentMetadata(existingPath(sessionPath, metadataPath, "instrument_metadata.json"), out.instrument);

  if (plan.loadTrades) {
    Status status = loadJsonl<TradeRow>(existingPath(sessionPath, tradesPath, "trades.jsonl"), "trades", plan.requireTrades, out.trades, parseTradeLine, error);
    if (!isOk(status)) return status;
    splitTradeWarmup(out);
  }
  if (plan.loadBookTickers) {
    Status status = loadJsonl<BookTickerRow>(existingPath(sessionPath, bookTickerPath, "bookticker.jsonl"), "bookticker", plan.requireBookTickers, out.bookTickers, parseBookTickerLine, error);
    if (!isOk(status)) return status;
  }
  if (plan.loadDepths) {
    Status status = loadJsonl<DepthRow>(existingPath(sessionPath, depthPath, "depth.jsonl"), "depth", plan.requireDepths, out.depths, [](std::string_view line, DepthRow& row) noexcept {
      return parseBookDocument(line, row.levels, row.tsNs, &row.ingestSeq);
    }, error);
    if (!isOk(status)) return status;
  }
  if (plan.loadCandles) {
    Status status = loadJsonl<CandleRow>(existingPath(sessionPath, candlesPath, "candles.jsonl"), "candles", plan.requireCandles, out.candles, parseCandleLine, error);
    if (!isOk(status)) return status;
  }

  std::string snapshotText;
  const std::filesystem::path snapshotPath = sessionPath / "snapshot_000.json";
  if (readText(snapshotPath, snapshotText)) {
    out.hasSnapshot = parseBookDocument(snapshotText, out.snapshot.levels, out.snapshot.tsNs);
    if (!out.hasSnapshot) {
      error = "failed to parse snapshot_000.json";
      return Status::CorruptData;
    }
    updateRange(out, out.snapshot.tsNs);
  }
  rebuildReplayRange(out);
  return Status::Ok;
}

Status loadSession(const std::filesystem::path& sessionPath, SessionData& out, std::string& error) noexcept {
  SessionLoadPlan plan{};
  plan.loadCandles = true;
  return loadSession(sessionPath, plan, out, error);
}

}  // namespace hft_backtest::detail
