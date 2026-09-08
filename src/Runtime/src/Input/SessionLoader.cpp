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
namespace {

class MappedFile {
 public:
  MappedFile() noexcept = default;
  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;

  ~MappedFile() {
    if (data_ && data_ != MAP_FAILED) (void)::munmap(data_, size_);
    if (fd_ >= 0) (void)::close(fd_);
  }

  bool open(const std::filesystem::path& path) noexcept {
    fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) return false;
    struct stat st {};
    if (::fstat(fd_, &st) != 0) return false;
    if (st.st_size <= 0) {
      size_ = 0u;
      return true;
    }
    size_ = static_cast<std::size_t>(st.st_size);
    data_ = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
    return data_ != MAP_FAILED;
  }

  std::string_view view() const noexcept {
    if (!data_ || data_ == MAP_FAILED || size_ == 0u) return {};
    return std::string_view(static_cast<const char*>(data_), size_);
  }

  std::size_t size() const noexcept {
    return size_;
  }

 private:
  int fd_{-1};
  void* data_{nullptr};
  std::size_t size_{0u};
};

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

void skipWs(std::string_view text, std::size_t& pos) noexcept {
  while (pos < text.size()) {
    const char c = text[pos];
    if (c != ' ' && c != '\n' && c != '\r' && c != '\t') break;
    ++pos;
  }
}

bool parseI64(std::string_view text, std::size_t& pos, std::int64_t& out) noexcept {
  skipWs(text, pos);
  if (pos >= text.size()) return false;
  const bool negative = text[pos] == '-';
  if (negative || text[pos] == '+') ++pos;
  if (pos >= text.size()) return false;
  constexpr std::uint64_t kMaxPositive = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  constexpr std::uint64_t kMaxNegativeAbs = kMaxPositive + 1u;
  const std::uint64_t limit = negative ? kMaxNegativeAbs : kMaxPositive;
  std::uint64_t value = 0u;
  bool any = false;
  while (pos < text.size()) {
    const unsigned digit = static_cast<unsigned>(static_cast<unsigned char>(text[pos]) - static_cast<unsigned char>('0'));
    if (digit > 9u) break;
    if (value > (limit - digit) / 10u) return false;
    value = value * 10u + digit;
    any = true;
    ++pos;
  }
  if (!any) return false;
  if (negative) {
    out = value == kMaxNegativeAbs ? std::numeric_limits<std::int64_t>::min() : -static_cast<std::int64_t>(value);
  } else {
    out = static_cast<std::int64_t>(value);
  }
  return true;
}

bool parseContractTradeLine(std::string_view line, TradeRow& out) noexcept {
  hftrec::replay::TradeRow row{};
  if (!hftrec::isOk(hftrec::replay::parseTradeLine(line, row))) return false;
  out.priceE8 = row.priceE8;
  out.qtyE8 = row.qtyE8;
  out.side = row.side;
  out.tsNs = row.tsNs;
  out.ingestSeq = row.ingestSeq;
  out.arrival = row.arrival;
  return true;
}

bool parseContractBookTickerLine(std::string_view line, BookTickerRow& out) noexcept {
  hftrec::replay::BookTickerRow row{};
  if (!hftrec::isOk(hftrec::replay::parseBookTickerLine(line, row)) || row.eventId == 0u) return false;
  out.bidPriceE8 = row.bidPriceE8;
  out.bidQtyE8 = row.bidQtyE8;
  out.askPriceE8 = row.askPriceE8;
  out.askQtyE8 = row.askQtyE8;
  out.tsNs = row.tsNs;
  out.ingestSeq = row.ingestSeq;
  out.arrival = row.arrival;
  out.bidPresent = row.bidPriceE8 > 0;
  out.askPresent = row.askPriceE8 > 0;
  return true;
}

bool parseContractPriceLimitLine(std::string_view line, PriceLimitRow& out) noexcept {
  hftrec::replay::PriceLimitRow row{};
  if (!hftrec::isOk(hftrec::replay::parsePriceLimitLine(line, row))) return false;
  out.tsNs = row.tsNs;
  out.buyLimitE8 = row.buyLimitE8;
  out.sellLimitE8 = row.sellLimitE8;
  out.enabled = row.enabled;
  out.ingestSeq = row.ingestSeq;
  out.arrival = row.arrival;
  return true;
}

bool parseContractLiquidationLine(std::string_view line, LiquidationRow& out) noexcept {
  hftrec::replay::LiquidationRow row{};
  if (!hftrec::isOk(hftrec::replay::parseLiquidationLine(line, row))) return false;
  out.priceE8 = row.priceE8;
  out.qtyE8 = row.qtyE8;
  out.side = row.side;
  out.tsNs = row.tsNs;
  out.avgPriceE8 = row.avgPriceE8;
  out.filledQtyE8 = row.filledQtyE8;
  out.orderType = row.orderType;
  out.timeInForce = row.timeInForce;
  out.status = row.status;
  out.sourceMode = row.sourceMode;
  out.ingestSeq = row.ingestSeq;
  out.arrival = row.arrival;
  return true;
}

bool parseContractMarkPriceLine(std::string_view line, MarkPriceRow& out) noexcept {
  hftrec::replay::MarkPriceRow row{};
  if (!hftrec::isOk(hftrec::replay::parseMarkPriceLine(line, row))) return false;
  out.tsNs = row.tsNs;
  out.markPriceE8 = row.markPriceE8;
  out.ingestSeq = row.ingestSeq;
  out.arrival = row.arrival;
  return true;
}

bool parseContractIndexPriceLine(std::string_view line, IndexPriceRow& out) noexcept {
  hftrec::replay::IndexPriceRow row{};
  if (!hftrec::isOk(hftrec::replay::parseIndexPriceLine(line, row))) return false;
  out.tsNs = row.tsNs;
  out.indexPriceE8 = row.indexPriceE8;
  out.ingestSeq = row.ingestSeq;
  out.arrival = row.arrival;
  return true;
}

bool parseContractFundingLine(std::string_view line, FundingRow& out) noexcept {
  hftrec::replay::FundingRow row{};
  if (!hftrec::isOk(hftrec::replay::parseFundingLine(line, row))) return false;
  out.tsNs = row.tsNs;
  out.fundingRateE8 = row.fundingRateE8;
  out.fundingTsNs = row.fundingTsNs;
  out.nextFundingTsNs = row.nextFundingTsNs;
  out.ingestSeq = row.ingestSeq;
  out.arrival = row.arrival;
  return true;
}

bool parseContractCandleLine(std::string_view line, CandleRow& out) noexcept {
  hftrec::replay::CandleRow row{};
  if (!hftrec::isOk(hftrec::replay::parseCandleLine(line, row))) return false;
  out.tier = row.tier;
  out.tsNs = row.tsNs;
  out.highE8 = row.highE8;
  out.lowE8 = row.lowE8;
  out.quoteAmountE8 = row.quoteAmountE8;
  out.openE8 = row.openE8;
  out.closeE8 = row.closeE8;
  out.volumeE8 = row.volumeE8;
  out.hasOhlc = row.hasOhlc;
  out.arrival = row.arrival;
  return true;
}

bool readLine(std::string_view text, std::size_t& pos, std::string_view& line) noexcept {
  if (pos >= text.size()) return false;
  const std::size_t lineStart = pos;
  while (pos < text.size() && text[pos] != '\n') ++pos;
  std::size_t lineEnd = pos;
  if (pos < text.size() && text[pos] == '\n') ++pos;
  if (lineEnd > lineStart && text[lineEnd - 1u] == '\r') --lineEnd;
  line = std::string_view{text.data() + lineStart, lineEnd - lineStart};
  return true;
}

constexpr std::size_t kMaxSessionWarnings = 64u;

void appendSessionWarning(std::vector<std::string>& warnings, std::string warning) {
  if (warnings.size() < kMaxSessionWarnings) {
    warnings.push_back(std::move(warning));
  } else if (warnings.size() == kMaxSessionWarnings) {
    warnings.push_back("additional session warnings suppressed");
  }
}

struct TimelineRowKey {
  std::int64_t tsNs{0};
  std::int64_t ingestSeq{0};
};

bool rowKeyLess(TimelineRowKey lhs, TimelineRowKey rhs) noexcept {
  if (lhs.tsNs != rhs.tsNs) return lhs.tsNs < rhs.tsNs;
  return lhs.ingestSeq < rhs.ingestSeq;
}

TimelineRowKey rowKey(const TradeRow& row) noexcept { return TimelineRowKey{row.tsNs, row.ingestSeq}; }
TimelineRowKey rowKey(const BookTickerRow& row) noexcept { return TimelineRowKey{row.tsNs, row.ingestSeq}; }
TimelineRowKey rowKey(const PriceLimitRow& row) noexcept { return TimelineRowKey{row.tsNs, row.ingestSeq}; }
TimelineRowKey rowKey(const LiquidationRow& row) noexcept { return TimelineRowKey{row.tsNs, row.ingestSeq}; }
TimelineRowKey rowKey(const MarkPriceRow& row) noexcept { return TimelineRowKey{row.tsNs, row.ingestSeq}; }
TimelineRowKey rowKey(const IndexPriceRow& row) noexcept { return TimelineRowKey{row.tsNs, row.ingestSeq}; }
TimelineRowKey rowKey(const FundingRow& row) noexcept { return TimelineRowKey{row.tsNs, row.ingestSeq}; }
TimelineRowKey rowKey(const OpenInterestRow& row) noexcept { return TimelineRowKey{row.tsNs, row.ingestSeq}; }
TimelineRowKey rowKey(const DepthRow& row) noexcept { return TimelineRowKey{row.tsNs, row.ingestSeq}; }
TimelineRowKey rowKey(const CandleRow& row) noexcept { return TimelineRowKey{row.tsNs, 0}; }

std::size_t estimatedRowsFor(std::size_t bytes, std::string_view channel) noexcept {
  if (bytes == 0u) return 0u;
  std::size_t avgBytes = 96u;
  if (channel == "bookticker" || channel == "mark_price" || channel == "index_price" ||
      channel == "funding" || channel == "open_interest" || channel == "candles" ||
      channel == "candles2" || channel == "candlesv2") {
    avgBytes = 64u;
  } else if (channel == "depth" || channel == "depth_tape") {
    avgBytes = 192u;
  } else if (channel == "liquidations") {
    avgBytes = 160u;
  }
  return (bytes / avgBytes) + 16u;
}

template<typename Row, typename ParseFn>
Status loadJsonl(const std::filesystem::path& path,
                 std::string_view channel,
                 bool required,
                 std::vector<Row>& out,
                 ParseFn parse,
                 std::string& error,
                 bool* sortedOut = nullptr) {
  out.clear();
  if (sortedOut) *sortedOut = true;
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
  MappedFile file{};
  if (!file.open(path)) {
    error = "failed to open ";
    error += path.generic_string();
    if (errno != 0) {
      error += ": ";
      error += std::strerror(errno);
    }
    return Status::IoError;
  }
  const std::string_view text = file.view();
  out.reserve(estimatedRowsFor(text.size(), channel));
  bool sorted = true;
  std::uint64_t lineNo = 0;
  std::size_t pos = 0u;
  while (pos < text.size()) {
    ++lineNo;
    const std::size_t lineStart = pos;
    while (pos < text.size() && text[pos] != '\n') ++pos;
    std::size_t lineEnd = pos;
    if (pos < text.size() && text[pos] == '\n') ++pos;
    if (lineEnd > lineStart && text[lineEnd - 1u] == '\r') --lineEnd;
    if (lineEnd == lineStart) continue;
    const std::string_view line{text.data() + lineStart, lineEnd - lineStart};
    Row row{};
    if (!parse(line, row)) {
      error = "failed to parse ";
      error += path.generic_string();
      error += ":";
      error += std::to_string(lineNo);
      return Status::CorruptData;
    }
    if (!out.empty() && rowKeyLess(rowKey(row), rowKey(out.back()))) sorted = false;
    out.push_back(std::move(row));
  }
  if (required && out.empty()) {
    error = "required ";
    error += channel;
    error += " channel is empty: ";
    error += path.generic_string();
    return Status::CorruptData;
  }
  // Preserve the recorder channel exactly as captured.  buildTimeline() owns
  // the replay ordering step and uses sortedOut to select either the bounded
  // merge path or the stable-sort path.  Sorting here loses capture-order
  // evidence and makes the flag below lie about the source channel.
  if (sortedOut) *sortedOut = sorted;
  return Status::Ok;
}

Status loadDepthTapeSidecarJsonl(const std::filesystem::path& tapePath,
                                 const std::filesystem::path& sidecarPath,
                                 bool required,
                                 std::vector<DepthRow>& out,
                                 std::string& error,
                                 std::vector<std::string>& warnings,
                                 bool* sortedOut = nullptr) {
  (void)warnings;
  out.clear();
  if (sortedOut) *sortedOut = true;
  if (!std::filesystem::exists(tapePath) || !std::filesystem::exists(sidecarPath)) {
    if (required) {
      error = "missing required depth_tape/depth_sidecar package";
      return Status::CorruptData;
    }
    return Status::Ok;
  }
  MappedFile tapeFile{};
  MappedFile sidecarFile{};
  if (!tapeFile.open(tapePath) || !sidecarFile.open(sidecarPath)) {
    error = "failed to open depth_tape/depth_sidecar package";
    return Status::IoError;
  }
  const std::string_view tapeText = tapeFile.view();
  const std::string_view sidecarText = sidecarFile.view();
  out.reserve(estimatedRowsFor(tapeFile.size(), "depth_tape"));

  std::size_t tapePos = 0u;
  std::size_t sidecarPos = 0u;
  std::uint64_t lineNo = 0u;
  bool sorted = true;
  while (tapePos < tapeText.size() || sidecarPos < sidecarText.size()) {
    std::string_view tapeLine{};
    std::string_view sidecarLine{};
    const bool hasTape = readLine(tapeText, tapePos, tapeLine);
    const bool hasSidecar = readLine(sidecarText, sidecarPos, sidecarLine);
    ++lineNo;
    if (!hasTape && !hasSidecar) break;
    if (hasTape != hasSidecar) {
      error = "depth_tape/depth_sidecar line count mismatch at line ";
      error += std::to_string(lineNo);
      return Status::CorruptData;
    }
    if (tapeLine.empty() && sidecarLine.empty()) {
      continue;
    }
    hftrec::replay::DepthRow contractRow{};
    const bool pairOk = !tapeLine.empty() && !sidecarLine.empty() &&
        hftrec::isOk(hftrec::replay::parseDepthTapeSidecarLine(tapeLine, sidecarLine, contractRow)) &&
        contractRow.eventId != 0u;
    if (!pairOk) {
      error = "failed to parse captured-arrival depth_tape/depth_sidecar row at line ";
      error += std::to_string(lineNo);
      return Status::CorruptData;
    }
    DepthRow row{};
    row.tsNs = contractRow.tsNs;
    row.ingestSeq = contractRow.ingestSeq;
    row.arrival = contractRow.arrival;
    row.levels.reserve(contractRow.levels.size());
    for (const hftrec::replay::PricePair& level : contractRow.levels) {
      row.levels.push_back(PriceLevelRow{level.priceE8, level.qtyE8, level.side});
    }
    if (!out.empty() && rowKeyLess(rowKey(row), rowKey(out.back()))) sorted = false;
    out.push_back(std::move(row));
  }
  if (required && out.empty()) {
    error = "required depth_tape/depth_sidecar package is empty: ";
    error += tapePath.generic_string();
    return Status::CorruptData;
  }
  if (!sorted) {
    std::stable_sort(out.begin(), out.end(), [](const DepthRow& lhs, const DepthRow& rhs) noexcept {
      return rowKeyLess(rowKey(lhs), rowKey(rhs));
    });
  }
  if (sortedOut) *sortedOut = true;
  return Status::Ok;
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

bool cleanRequiredRuntimeChannel(const hftrec::capture::ChannelRuntimeHealth& health) noexcept {
  return health.state == "live" && health.reconnectCount == 0u &&
      health.droppedEventCount == 0u && health.unroutableEventCount == 0u &&
      health.lastError.empty();
}

bool addArrivalRowCount(std::uint64_t value, std::uint64_t& total) noexcept {
  if (value > std::numeric_limits<std::uint64_t>::max() - total) return false;
  total += value;
  return true;
}

bool arrivalSummaryMatchesDeclaredRows(
    const hftrec::capture::SessionManifest& manifest) noexcept {
  std::uint64_t declaredRows = 0u;
  if (!addArrivalRowCount(manifest.tradesCount, declaredRows) ||
      !addArrivalRowCount(manifest.liquidationsCount, declaredRows) ||
      !addArrivalRowCount(manifest.bookTickerCount, declaredRows) ||
      !addArrivalRowCount(manifest.markPriceCount, declaredRows) ||
      !addArrivalRowCount(manifest.indexPriceCount, declaredRows) ||
      !addArrivalRowCount(manifest.fundingCount, declaredRows) ||
      !addArrivalRowCount(manifest.priceLimitCount, declaredRows) ||
      !addArrivalRowCount(manifest.depthCount, declaredRows) ||
      !addArrivalRowCount(manifest.candlesCount, declaredRows) ||
      !addArrivalRowCount(manifest.candles2Count, declaredRows)) {
    return false;
  }

  std::uint64_t accountedRows = 0u;
  return addArrivalRowCount(manifest.arrivalClock.capturedRows, accountedRows) &&
      addArrivalRowCount(manifest.arrivalClock.historicalRows, accountedRows) &&
      addArrivalRowCount(manifest.arrivalClock.unavailableRows, accountedRows) &&
      accountedRows == declaredRows;
}

bool validateManifestChannel(std::string_view name,
                             bool load,
                             bool required,
                             bool enabled,
                             std::uint64_t count,
                             std::string_view path,
                             std::string_view rowSchema,
                             std::string_view expectedRowSchema,
                             const hftrec::capture::ChannelRuntimeHealth* runtime,
                             std::vector<std::string>& warnings,
                             std::string& error) {
  if (!load) return true;
  if (!enabled) {
    if (!required) return true;
    error = "manifest does not enable required ";
    error += name;
    error += " channel";
    return false;
  }
  if (path.empty() || rowSchema != expectedRowSchema) {
    error = "unsupported captured-arrival ";
    error += name;
    error += " channel contract";
    return false;
  }
  if (required && count == 0u) {
    error = "manifest declares empty required ";
    error += name;
    error += " channel";
    return false;
  }
  if (runtime && !cleanRequiredRuntimeChannel(*runtime)) {
    if (required) {
      error = "required ";
      error += name;
      error += " channel is not clean";
      return false;
    }
    appendSessionWarning(warnings, "optional " + std::string{name} + " channel is degraded");
  }
  return true;
}

bool validateCurrentRecorderManifest(const hftrec::capture::SessionManifest& manifest,
                                     const SessionLoadPlan& plan,
                                     std::vector<std::string>& warnings,
                                     std::string& error) {
  if (manifest.manifestSchemaVersion != hftrec::capture::kManifestSchemaVersionCurrent ||
      manifest.corpusSchemaVersion != hftrec::capture::kCorpusSchemaVersionCurrent ||
      manifest.captureContractVersion != hftrec::capture::kCaptureContractVersionCurrent) {
    error = "incompatible recorder corpus: captured-arrival contract is required";
    return false;
  }
  if (manifest.sessionStatus != "complete") {
    error = "recorder session is not finalized: status=" + manifest.sessionStatus;
    return false;
  }
  if (!manifest.structurallyLoadable ||
      manifest.sessionHealth != hftrec::SessionHealth::Clean ||
      !manifest.exactReplayEligible) {
    error = "recorder session is structurally incompatible or corrupt";
    return false;
  }
  if (manifest.arrivalClock.boundary !=
          "hft-parser.application-frame-ready" ||
      manifest.arrivalClock.realtimeClock != "CLOCK_REALTIME" ||
      manifest.arrivalClock.monotonicClock != "CLOCK_MONOTONIC") {
    error = "recorder session uses an unsupported arrival-clock boundary";
    return false;
  }
  if (manifest.arrivalClock.capturedRows == 0u ||
      manifest.arrivalClock.unavailableRows != 0u) {
    error = "recorder session is not captured-arrival replay eligible";
    return false;
  }
  if (manifest.arrivalClock.firstReceiveRealtimeNs <= 0 ||
      manifest.arrivalClock.lastReceiveRealtimeNs <= 0 ||
      manifest.arrivalClock.firstReceiveMonotonicNs == 0u ||
      manifest.arrivalClock.lastReceiveMonotonicNs == 0u ||
      !arrivalSummaryMatchesDeclaredRows(manifest)) {
    error = "recorder arrival-clock summary does not match declared corpus rows";
    return false;
  }
  if (manifest.arrivalClock.realtimeRegressions != 0u)
    appendSessionWarning(warnings, "CLOCK_REALTIME regressions preserved: " +
        std::to_string(manifest.arrivalClock.realtimeRegressions));
  if (manifest.arrivalClock.monotonicNonIncreasing != 0u)
    appendSessionWarning(warnings, "non-increasing CLOCK_MONOTONIC samples preserved: " +
        std::to_string(manifest.arrivalClock.monotonicNonIncreasing));
  if (manifest.arrivalClock.exchangeAheadOfReceive != 0u)
    appendSessionWarning(warnings, "exchange timestamps ahead of local receive preserved: " +
        std::to_string(manifest.arrivalClock.exchangeAheadOfReceive));
  if (manifest.arrivalClock.exchangeTimestampMissing != 0u)
    appendSessionWarning(warnings, "missing exchange timestamps preserved: " +
        std::to_string(manifest.arrivalClock.exchangeTimestampMissing));
  if (!manifest.warningSummary.empty()) appendSessionWarning(warnings, "capture warning: " + manifest.warningSummary);

  if (!validateManifestChannel("trades", plan.loadTrades, plan.requireTrades,
          manifest.tradesEnabled, manifest.tradesCount, manifest.tradesPath,
          manifest.tradesRowSchema, hftrec::capture::kTradesRowSchemaCurrent, &manifest.tradesRuntime,
          warnings, error)) return false;
  if (!validateManifestChannel("bookticker", plan.loadBookTickers, plan.requireBookTickers,
          manifest.bookTickerEnabled, manifest.bookTickerCount, manifest.bookTickerPath,
          manifest.bookTickerRowSchema, hftrec::capture::kBookTickerRowSchemaCurrent, &manifest.bookTickerRuntime,
          warnings, error)) return false;
  if (!validateManifestChannel("price_limit", plan.loadPriceLimits, plan.requirePriceLimits,
          manifest.priceLimitEnabled, manifest.priceLimitCount, manifest.priceLimitPath,
          manifest.priceLimitRowSchema, hftrec::capture::kPriceLimitRowSchemaCurrent, &manifest.priceLimitRuntime,
          warnings, error)) return false;
  if (!validateManifestChannel("depth", plan.loadDepths, plan.requireDepths,
          manifest.orderbookEnabled, manifest.depthCount, manifest.depthPath,
          manifest.depthRowSchema, hftrec::capture::kDepthRowSchemaCurrent, &manifest.depthRuntime,
          warnings, error)) return false;
  if (plan.loadDepths && manifest.orderbookEnabled && manifest.depthSidecarPath.empty()) {
    error = "captured-arrival depth sidecar path is missing";
    return false;
  }
  if (!validateManifestChannel("liquidations", plan.loadLiquidations, plan.requireLiquidations,
          manifest.liquidationsEnabled, manifest.liquidationsCount, manifest.liquidationsPath,
          manifest.liquidationsRowSchema, hftrec::capture::kLiquidationsRowSchemaCurrent, &manifest.liquidationsRuntime,
          warnings, error)) return false;
  if (!validateManifestChannel("mark_price", plan.loadMarkPrices, plan.requireMarkPrices,
          manifest.markPriceEnabled, manifest.markPriceCount, manifest.markPricePath,
          manifest.markPriceRowSchema, hftrec::capture::kMarkPriceRowSchemaCurrent, &manifest.markPriceRuntime,
          warnings, error)) return false;
  if (!validateManifestChannel("index_price", plan.loadIndexPrices, plan.requireIndexPrices,
          manifest.indexPriceEnabled, manifest.indexPriceCount, manifest.indexPricePath,
          manifest.indexPriceRowSchema, hftrec::capture::kIndexPriceRowSchemaCurrent, &manifest.indexPriceRuntime,
          warnings, error)) return false;
  if (!validateManifestChannel("funding", plan.loadFunding, plan.requireFunding,
          manifest.fundingEnabled, manifest.fundingCount, manifest.fundingPath,
          manifest.fundingRowSchema, hftrec::capture::kFundingRowSchemaCurrent, &manifest.fundingRuntime,
          warnings, error)) return false;
  if (plan.requireOpenInterest) {
    error = "current recorder manifest has no required open_interest captured-arrival contract";
    return false;
  }
  if (plan.loadOpenInterest) {
    appendSessionWarning(warnings, "optional open_interest captured-arrival channel is unavailable");
  }
  if (plan.loadCandles) {
    const bool useDetailed = manifest.candles2Enabled;
    const bool enabled = useDetailed || manifest.candlesEnabled;
    const std::uint64_t count = useDetailed ? manifest.candles2Count : manifest.candlesCount;
    const std::string& path = useDetailed ? manifest.candles2Path : manifest.candlesPath;
    const std::string& schema = useDetailed ? manifest.candles2RowSchema : manifest.candlesRowSchema;
    const std::string_view expected = hftrec::capture::kCandlesRowSchemaCurrent;
    if (!validateManifestChannel("candles", true, plan.requireCandles, enabled, count, path,
                                 schema, expected, nullptr, warnings, error)) return false;
  }
  return true;
}

bool validateLoadedRowCount(std::string_view channel,
                            std::size_t actual,
                            std::uint64_t declared,
                            std::string& error) {
  if (static_cast<std::uint64_t>(actual) == declared) return true;
  error = "captured-arrival ";
  error += channel;
  error += " row count does not match finalized manifest";
  return false;
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

template <typename Row>
bool validateCapturedLiveRows(std::string_view channel,
                              const std::vector<Row>& rows,
                              std::string& error) {
  for (std::size_t i = 0u; i < rows.size(); ++i) {
    if (hftrec::replay::hasCapturedApplicationArrival(rows[i].arrival)) continue;
    error = "captured-arrival ";
    error += channel;
    error += " row is not an application-frame arrival at row ";
    error += std::to_string(i + 1u);
    return false;
  }
  return true;
}

template <typename Row>
bool validateReplayClockRows(std::string_view channel,
                             const std::vector<Row>& rows,
                             __int128 replayOffset,
                             std::string& error) {
  for (std::size_t i = 0u; i < rows.size(); ++i) {
    const __int128 mapped = static_cast<__int128>(rows[i].arrival.receiveMonotonicNs) +
        replayOffset;
    if (mapped > 0 && mapped <= std::numeric_limits<std::int64_t>::max()) continue;
    error = "captured-arrival ";
    error += channel;
    error += " row cannot be mapped into replay monotonic time at row ";
    error += std::to_string(i + 1u);
    return false;
  }
  return true;
}

bool validateHistoricalSeeds(const SessionData& out, std::string& error) {
  for (std::size_t i = 0u; i < out.tradeWarmup.size(); ++i) {
    if (hftrec::replay::isHistoricalBackfill(out.tradeWarmup[i].arrival)) continue;
    error = "trade warmup row is not marked historical_backfill at row " +
        std::to_string(i + 1u);
    return false;
  }
  for (std::size_t i = 0u; i < out.candles.size(); ++i) {
    if (hftrec::replay::isHistoricalBackfill(out.candles[i].arrival)) continue;
    error = "candle seed row is not marked historical_backfill at row " +
        std::to_string(i + 1u);
    return false;
  }
  return true;
}

bool validateCapturedReplayRows(const SessionData& out, std::string& error) {
  if (out.firstReceiveRealtimeNs <= 0 || out.firstReceiveMonotonicNs == 0u) {
    error = "captured-arrival replay clock anchor is missing";
    return false;
  }
  const __int128 replayOffset = static_cast<__int128>(out.firstReceiveRealtimeNs) -
      static_cast<__int128>(out.firstReceiveMonotonicNs);
  if (replayOffset < std::numeric_limits<std::int64_t>::min() ||
      replayOffset > std::numeric_limits<std::int64_t>::max()) {
    error = "captured-arrival replay clock anchor is out of range";
    return false;
  }
  return validateCapturedLiveRows("trades", out.trades, error) &&
      validateCapturedLiveRows("bookticker", out.bookTickers, error) &&
      validateCapturedLiveRows("price_limit", out.priceLimits, error) &&
      validateCapturedLiveRows("depth", out.depths, error) &&
      validateCapturedLiveRows("liquidations", out.liquidations, error) &&
      validateCapturedLiveRows("mark_price", out.markPrices, error) &&
      validateCapturedLiveRows("index_price", out.indexPrices, error) &&
      validateCapturedLiveRows("funding", out.funding, error) &&
      validateCapturedLiveRows("open_interest", out.openInterest, error) &&
      validateReplayClockRows("trades", out.trades, replayOffset, error) &&
      validateReplayClockRows("bookticker", out.bookTickers, replayOffset, error) &&
      validateReplayClockRows("price_limit", out.priceLimits, replayOffset, error) &&
      validateReplayClockRows("depth", out.depths, replayOffset, error) &&
      validateReplayClockRows("liquidations", out.liquidations, replayOffset, error) &&
      validateReplayClockRows("mark_price", out.markPrices, replayOffset, error) &&
      validateReplayClockRows("index_price", out.indexPrices, replayOffset, error) &&
      validateReplayClockRows("funding", out.funding, replayOffset, error) &&
      validateReplayClockRows("open_interest", out.openInterest, replayOffset, error) &&
      validateHistoricalSeeds(out, error);
}

void rebuildReplayRange(SessionData& out) noexcept {
  out.firstTsNs = 0;
  out.lastTsNs = 0;
  for (const TradeRow& row : out.trades) updateRange(out, row.tsNs);
  for (const BookTickerRow& row : out.bookTickers) updateRange(out, row.tsNs);
  for (const PriceLimitRow& row : out.priceLimits) updateRange(out, row.tsNs);
  for (const DepthRow& row : out.depths) updateRange(out, row.tsNs);
  for (const LiquidationRow& row : out.liquidations) updateRange(out, row.tsNs);
  for (const MarkPriceRow& row : out.markPrices) updateRange(out, row.tsNs);
  for (const IndexPriceRow& row : out.indexPrices) updateRange(out, row.tsNs);
  for (const FundingRow& row : out.funding) updateRange(out, row.tsNs);
  for (const OpenInterestRow& row : out.openInterest) updateRange(out, row.tsNs);
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
  hftrec::capture::SessionManifest recorderManifest{};
  if (!hftrec::isOk(hftrec::capture::parseManifestJson(manifest, recorderManifest))) {
    error = "malformed or incompatible recorder manifest.json; current captured-arrival v3 contract is required";
    return Status::CorruptData;
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
  const std::string& priceLimitPath = recorderManifest.priceLimitPath;
  const std::string& depthPath = recorderManifest.depthPath;
  const std::string& depthSidecarPath = recorderManifest.depthSidecarPath;
  const std::string& candlesPath = recorderManifest.candlesPath;
  const std::string& candles2Path = recorderManifest.candles2Path;
  const std::string& liquidationsPath = recorderManifest.liquidationsPath;
  const std::string& markPricePath = recorderManifest.markPricePath;
  const std::string& indexPricePath = recorderManifest.indexPricePath;
  const std::string& fundingPath = recorderManifest.fundingPath;
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
  if (plan.loadPriceLimits) {
    Status status = loadJsonl<PriceLimitRow>(declaredPath(sessionPath, priceLimitPath), "price_limit", plan.requirePriceLimits, out.priceLimits, parseContractPriceLimitLine, error);
    if (!isOk(status)) return status;
    if (!validateLoadedRowCount(
            "price_limit", out.priceLimits.size(),
            recorderManifest.priceLimitCount, error))
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
  if (plan.loadCandles) {
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
  if (plan.loadLiquidations) {
    Status status = loadJsonl<LiquidationRow>(declaredPath(sessionPath, liquidationsPath), "liquidations", plan.requireLiquidations, out.liquidations, parseContractLiquidationLine, error);
    if (!isOk(status)) return status;
    if (!validateLoadedRowCount(
            "liquidations", out.liquidations.size(),
            recorderManifest.liquidationsCount, error))
      return Status::CorruptData;
  }
  if (plan.loadMarkPrices) {
    Status status = loadJsonl<MarkPriceRow>(declaredPath(sessionPath, markPricePath), "mark_price", plan.requireMarkPrices, out.markPrices, parseContractMarkPriceLine, error);
    if (!isOk(status)) return status;
    if (!validateLoadedRowCount(
            "mark_price", out.markPrices.size(),
            recorderManifest.markPriceCount, error))
      return Status::CorruptData;
  }
  if (plan.loadIndexPrices) {
    Status status = loadJsonl<IndexPriceRow>(declaredPath(sessionPath, indexPricePath), "index_price", plan.requireIndexPrices, out.indexPrices, parseContractIndexPriceLine, error);
    if (!isOk(status)) return status;
    if (!validateLoadedRowCount(
            "index_price", out.indexPrices.size(),
            recorderManifest.indexPriceCount, error))
      return Status::CorruptData;
  }
  if (plan.loadFunding) {
    Status status = loadJsonl<FundingRow>(declaredPath(sessionPath, fundingPath), "funding", plan.requireFunding, out.funding, parseContractFundingLine, error);
    if (!isOk(status)) return status;
    if (!validateLoadedRowCount(
            "funding", out.funding.size(), recorderManifest.fundingCount,
            error)) return Status::CorruptData;
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
