#include "session_loader.hpp"

#include "primitives/composite/OrderBookTapeRuntimeV1.hpp"

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

bool skipJsonString(std::string_view text, std::size_t& pos) noexcept {
  skipWs(text, pos);
  if (pos >= text.size() || text[pos] != '"') return false;
  ++pos;
  while (pos < text.size()) {
    const char c = text[pos++];
    if (c == '"') return true;
    if (c == '\\') {
      if (pos >= text.size()) return false;
      ++pos;
    }
  }
  return false;
}

std::int64_t candleTierFromTimeframe(std::string_view text, std::size_t begin, std::size_t end) noexcept {
  const std::string_view timeframe = text.substr(begin, end - begin);
  if (timeframe == "1m") return 1;
  if (timeframe == "10m" || timeframe == "15m") return 2;
  if (timeframe == "1d") return 3;
  return 0;
}

bool parseJsonStringTier(std::string_view text, std::size_t& pos, std::int64_t& tier) noexcept {
  skipWs(text, pos);
  if (pos >= text.size() || text[pos] != '"') return false;
  ++pos;
  const std::size_t begin = pos;
  while (pos < text.size()) {
    const char c = text[pos++];
    if (c == '"') {
      tier = candleTierFromTimeframe(text, begin, pos - 1u);
      return true;
    }
    if (c == '\\') {
      if (pos >= text.size()) return false;
      ++pos;
    }
  }
  return false;
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

bool parseU64(std::string_view text, std::size_t& pos, std::uint64_t& out) noexcept {
  skipWs(text, pos);
  if (pos >= text.size() || text[pos] == '-' || text[pos] == '+') return false;
  std::uint64_t value = 0u;
  bool any = false;
  while (pos < text.size()) {
    const unsigned digit = static_cast<unsigned>(static_cast<unsigned char>(text[pos]) - static_cast<unsigned char>('0'));
    if (digit > 9u) break;
    if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10u) return false;
    value = value * 10u + digit;
    any = true;
    ++pos;
  }
  if (!any) return false;
  out = value;
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

bool parsePriceLimitLine(std::string_view line, PriceLimitRow& out) noexcept {
  std::size_t pos = 0;
  std::int64_t enabled = 0;
  out = PriceLimitRow{};
  if (!consume(line, pos, '[') || !parseI64(line, pos, out.buyLimitE8) || !consume(line, pos, ',') ||
      !parseI64(line, pos, out.sellLimitE8) || !consume(line, pos, ',') || !parseI64(line, pos, enabled) ||
      !consume(line, pos, ',') || !parseI64(line, pos, out.tsNs)) {
    return false;
  }
  out.enabled = enabled != 0 ? 1u : 0u;
  std::int64_t captureSeq = 0;
  if (consume(line, pos, ',') && (!parseI64(line, pos, captureSeq) || !consume(line, pos, ',') || !parseI64(line, pos, out.ingestSeq))) return false;
  return true;
}

bool skipJsonAtom(std::string_view text, std::size_t& pos) noexcept {
  skipWs(text, pos);
  if (pos >= text.size()) return false;
  if (text[pos] == '"') return skipJsonString(text, pos);
  std::int64_t ignored = 0;
  const std::size_t before = pos;
  if (parseI64(text, pos, ignored)) return true;
  pos = before;
  if (text.compare(pos, 4, "true") == 0) {
    pos += 4;
    return true;
  }
  if (text.compare(pos, 5, "false") == 0) {
    pos += 5;
    return true;
  }
  if (text.compare(pos, 4, "null") == 0) {
    pos += 4;
    return true;
  }
  return false;
}

bool parseOptionalIngestSeq(std::string_view line, std::size_t& pos, std::int64_t& out) noexcept {
  if (!consume(line, pos, ',')) return true;
  std::int64_t first = 0;
  if (!parseI64(line, pos, first)) return false;
  if (consume(line, pos, ',')) return parseI64(line, pos, out);
  out = first;
  return true;
}

bool parseLiquidationLine(std::string_view line, LiquidationRow& out) noexcept {
  std::size_t pos = 0;
  out = LiquidationRow{};
  if (!consume(line, pos, '[') || !parseI64(line, pos, out.priceE8) || !consume(line, pos, ',') ||
      !parseI64(line, pos, out.qtyE8) || !consume(line, pos, ',') ||
      !parseI64(line, pos, out.side) || (out.side != 0 && out.side != 1) || !consume(line, pos, ',') ||
      !parseI64(line, pos, out.tsNs)) return false;
  if (consume(line, pos, ',')) {
    if (!parseI64(line, pos, out.avgPriceE8) || !consume(line, pos, ',') || !parseI64(line, pos, out.filledQtyE8)) return false;
    if (consume(line, pos, ',')) {
      if (!skipJsonAtom(line, pos) || !consume(line, pos, ',') || !skipJsonAtom(line, pos) ||
          !consume(line, pos, ',') || !skipJsonAtom(line, pos) || !consume(line, pos, ',') ||
          !parseI64(line, pos, out.orderType) || !consume(line, pos, ',') ||
          !parseI64(line, pos, out.timeInForce) || !consume(line, pos, ',') ||
          !parseI64(line, pos, out.status) || !consume(line, pos, ',') ||
          !parseI64(line, pos, out.sourceMode)) return false;
      if (!parseOptionalIngestSeq(line, pos, out.ingestSeq)) return false;
    }
  }
  return true;
}

bool parseMarkPriceLine(std::string_view line, MarkPriceRow& out) noexcept {
  std::size_t pos = 0;
  out = MarkPriceRow{};
  if (!consume(line, pos, '[') || !parseI64(line, pos, out.tsNs) || !consume(line, pos, ',') ||
      !parseI64(line, pos, out.markPriceE8)) return false;
  return parseOptionalIngestSeq(line, pos, out.ingestSeq);
}

bool parseIndexPriceLine(std::string_view line, IndexPriceRow& out) noexcept {
  std::size_t pos = 0;
  out = IndexPriceRow{};
  if (!consume(line, pos, '[') || !parseI64(line, pos, out.tsNs) || !consume(line, pos, ',') ||
      !parseI64(line, pos, out.indexPriceE8)) return false;
  return parseOptionalIngestSeq(line, pos, out.ingestSeq);
}

bool parseFundingLine(std::string_view line, FundingRow& out) noexcept {
  std::size_t pos = 0;
  out = FundingRow{};
  if (!consume(line, pos, '[') || !parseI64(line, pos, out.tsNs) || !consume(line, pos, ',') ||
      !parseI64(line, pos, out.fundingRateE8) || !consume(line, pos, ',') ||
      !parseI64(line, pos, out.fundingTsNs) || !consume(line, pos, ',') ||
      !parseI64(line, pos, out.nextFundingTsNs)) return false;
  return parseOptionalIngestSeq(line, pos, out.ingestSeq);
}

bool parseOpenInterestLine(std::string_view line, OpenInterestRow& out) noexcept {
  std::size_t pos = 0;
  out = OpenInterestRow{};
  if (!consume(line, pos, '[') || !parseI64(line, pos, out.tsNs) || !consume(line, pos, ',') ||
      !parseI64(line, pos, out.openInterestE8)) return false;
  if (consume(line, pos, ',')) {
    if (!parseI64(line, pos, out.openInterestValueE8)) return false;
    if (!parseOptionalIngestSeq(line, pos, out.ingestSeq)) return false;
  }
  return true;
}

bool parseCandleLine(std::string_view line, CandleRow& out) noexcept {
  std::size_t pos = 0;
  out = CandleRow{};
  if (!consume(line, pos, '[')) return false;
  skipWs(line, pos);
  if (pos < line.size() && line[pos] == '"') {
    if (!skipJsonString(line, pos) || !consume(line, pos, ',') ||
        !skipJsonString(line, pos) || !consume(line, pos, ',') ||
        !skipJsonString(line, pos) || !consume(line, pos, ',') ||
        !parseJsonStringTier(line, pos, out.tier) || !consume(line, pos, ',') ||
        !parseI64(line, pos, out.tsNs) || !consume(line, pos, ',') ||
        !parseI64(line, pos, out.openE8) || !consume(line, pos, ',') ||
        !parseI64(line, pos, out.highE8) || !consume(line, pos, ',') ||
        !parseI64(line, pos, out.lowE8) || !consume(line, pos, ',') ||
        !parseI64(line, pos, out.closeE8) || !consume(line, pos, ',') ||
        !parseI64(line, pos, out.volumeE8) || !consume(line, pos, ',') ||
        !parseI64(line, pos, out.quoteAmountE8)) return false;
    out.hasOhlc = true;
    return out.tsNs > 0 && out.openE8 > 0 && out.highE8 > 0 && out.lowE8 > 0 &&
           out.closeE8 > 0 && out.highE8 >= out.lowE8 &&
           out.volumeE8 >= 0 && out.quoteAmountE8 >= 0;
  }
  if (!parseI64(line, pos, out.tier) || !consume(line, pos, ',') ||
      !parseI64(line, pos, out.tsNs) || !consume(line, pos, ',')) return false;
  std::int64_t firstPrice = 0;
  std::int64_t secondPrice = 0;
  std::int64_t thirdPrice = 0;
  if (!parseI64(line, pos, firstPrice) || !consume(line, pos, ',') ||
      !parseI64(line, pos, secondPrice) || !consume(line, pos, ',') ||
      !parseI64(line, pos, thirdPrice)) return false;
  if (consume(line, pos, ',')) {
    out.openE8 = firstPrice;
    out.highE8 = secondPrice;
    out.lowE8 = thirdPrice;
    if (!parseI64(line, pos, out.closeE8) || !consume(line, pos, ',') ||
        !parseI64(line, pos, out.volumeE8) || !consume(line, pos, ',') ||
        !parseI64(line, pos, out.quoteAmountE8)) return false;
    out.hasOhlc = true;
  } else {
    out.highE8 = firstPrice;
    out.lowE8 = secondPrice;
    out.quoteAmountE8 = thirdPrice;
  }
  return (out.tier >= 1 && out.tier <= 3 && out.tsNs > 0 && out.highE8 > 0 && out.lowE8 > 0 &&
          out.highE8 >= out.lowE8 && out.quoteAmountE8 >= 0 &&
          (!out.hasOhlc || (out.openE8 > 0 && out.closeE8 > 0 && out.volumeE8 >= 0)));
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

bool parseDepthTapeLine(std::string_view line, DepthRow& row, std::uint64_t& taggedTs) noexcept {
  std::size_t pos = 0;
  row = DepthRow{};
  if (!consume(line, pos, '[') || !parseU64(line, pos, taggedTs)) return false;
  if (!cxet::composite::isOrderBookTapeTimestampWord(taggedTs)) return false;
  const TimeNs ts = cxet::composite::orderBookTapeTimestampFromWord(taggedTs);
  if (ts.raw > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return false;
  row.tsNs = static_cast<std::int64_t>(ts.raw);
  while (true) {
    skipWs(line, pos);
    if (pos >= line.size()) return false;
    if (line[pos] == ']') {
      ++pos;
      skipWs(line, pos);
      return pos == line.size();
    }
    if (!consume(line, pos, ',')) return false;
    PriceLevelRow level{};
    if (!parseI64(line, pos, level.priceE8) || !consume(line, pos, ',') || !parseI64(line, pos, level.qtyE8)) {
      return false;
    }
    if (level.priceE8 < 0 || level.qtyE8 < 0) return false;
    row.levels.push_back(level);
  }
}

bool applyDepthSidecarLine(std::string_view line, std::uint64_t expectedTaggedTs, DepthRow& row) noexcept {
  std::size_t pos = 0;
  std::uint64_t taggedTs = 0u;
  if (!consume(line, pos, '[') || !parseU64(line, pos, taggedTs) || taggedTs != expectedTaggedTs) return false;
  std::size_t levelIndex = 0u;
  while (true) {
    skipWs(line, pos);
    if (pos >= line.size()) return false;
    if (line[pos] == ']') {
      ++pos;
      skipWs(line, pos);
      return pos == line.size() && levelIndex == row.levels.size();
    }
    std::int64_t side = 0;
    std::int64_t count = 0;
    if (!consume(line, pos, ',') || !parseI64(line, pos, side) || !consume(line, pos, ',') ||
        !parseI64(line, pos, count)) {
      return false;
    }
    if ((side != 0 && side != 1) || count < 0) return false;
    for (std::int64_t i = 0; i < count; ++i) {
      if (levelIndex >= row.levels.size()) return false;
      row.levels[levelIndex++].side = side;
    }
  }
}

bool parseDepthSidecarTaggedTs(std::string_view line, std::uint64_t& taggedTs) noexcept {
  std::size_t pos = 0;
  return consume(line, pos, '[') && parseU64(line, pos, taggedTs);
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

std::uint64_t countNonEmptyRemainingLines(std::string_view text, std::size_t pos) noexcept {
  std::uint64_t count = 0u;
  while (pos < text.size()) {
    std::string_view line{};
    if (!readLine(text, pos, line)) break;
    if (!line.empty()) ++count;
  }
  return count;
}

constexpr std::uint64_t kDepthResyncLookaheadRows = 512u;
constexpr std::size_t kMaxSessionWarnings = 64u;

void appendSessionWarning(std::vector<std::string>& warnings, std::string warning) {
  if (warnings.size() < kMaxSessionWarnings) {
    warnings.push_back(std::move(warning));
  } else if (warnings.size() == kMaxSessionWarnings) {
    warnings.push_back("additional session warnings suppressed");
  }
}

std::string depthRecoveryWarning(std::uint64_t lineNo,
                                 std::uint64_t loadedRows,
                                 std::uint64_t droppedTapeRows,
                                 std::uint64_t droppedSidecarRows,
                                 std::string_view reason) {
  std::ostringstream warning;
  warning << "partial depth replay: " << reason << " at line " << lineNo
          << ", loaded " << loadedRows << " paired rows";
  if (droppedTapeRows > 0u) warning << ", dropped " << droppedTapeRows << " unpaired depth_tape rows";
  if (droppedSidecarRows > 0u) warning << ", dropped " << droppedSidecarRows << " unpaired depth_sidecar rows";
  warning << "; exact replay lost";
  return warning.str();
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
  if (!sorted) {
    std::stable_sort(out.begin(), out.end(), [](const Row& lhs, const Row& rhs) noexcept {
      return rowKeyLess(rowKey(lhs), rowKey(rhs));
    });
  }
  if (sortedOut) *sortedOut = true;
  return Status::Ok;
}

Status loadDepthTapeSidecarJsonl(const std::filesystem::path& tapePath,
                                 const std::filesystem::path& sidecarPath,
                                 bool required,
                                 std::vector<DepthRow>& out,
                                 std::string& error,
                                 std::vector<std::string>& warnings,
                                 bool* sortedOut = nullptr) {
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
  bool hasPendingTape = false;
  bool hasPendingSidecar = false;
  std::string_view pendingTapeLine{};
  std::string_view pendingSidecarLine{};
  while (hasPendingTape || hasPendingSidecar || tapePos < tapeText.size() || sidecarPos < sidecarText.size()) {
    std::string_view tapeLine{};
    std::string_view sidecarLine{};
    bool hasTape = false;
    bool hasSidecar = false;
    if (hasPendingTape) {
      tapeLine = pendingTapeLine;
      hasPendingTape = false;
      hasTape = true;
    } else {
      hasTape = readLine(tapeText, tapePos, tapeLine);
    }
    if (hasPendingSidecar) {
      sidecarLine = pendingSidecarLine;
      hasPendingSidecar = false;
      hasSidecar = true;
    } else {
      hasSidecar = readLine(sidecarText, sidecarPos, sidecarLine);
    }
    ++lineNo;
    if (!hasTape && !hasSidecar) break;
    if (hasTape != hasSidecar) {
      if (!out.empty()) {
        const std::uint64_t loadedRows = static_cast<std::uint64_t>(out.size());
        const std::uint64_t droppedTapeRows =
            (hasTape && !tapeLine.empty() ? 1u : 0u) + countNonEmptyRemainingLines(tapeText, tapePos);
        const std::uint64_t droppedSidecarRows =
            (hasSidecar && !sidecarLine.empty() ? 1u : 0u) + countNonEmptyRemainingLines(sidecarText, sidecarPos);
        appendSessionWarning(warnings,
                             depthRecoveryWarning(lineNo, loadedRows, droppedTapeRows, droppedSidecarRows,
                                                  "depth_tape/depth_sidecar unpaired tail"));
        break;
      }
      error = "depth_tape/depth_sidecar line count mismatch at line ";
      error += std::to_string(lineNo);
      return Status::CorruptData;
    }
    if (tapeLine.empty() && sidecarLine.empty()) {
      continue;
    }
    DepthRow row{};
    std::uint64_t taggedTs = 0u;
    const bool tapeOk = !tapeLine.empty() && parseDepthTapeLine(tapeLine, row, taggedTs);
    const bool pairOk = tapeOk && !sidecarLine.empty() && applyDepthSidecarLine(sidecarLine, taggedTs, row);
    if (!pairOk) {
      std::uint64_t sidecarTaggedTs = 0u;
      const bool sidecarTagged = !sidecarLine.empty() && parseDepthSidecarTaggedTs(sidecarLine, sidecarTaggedTs);
      std::uint64_t droppedTapeRows = 0u;
      std::uint64_t droppedSidecarRows = 0u;
      bool recovered = false;
      if (tapeOk) {
        std::size_t scanPos = sidecarPos;
        for (std::uint64_t skipped = 1u; skipped <= kDepthResyncLookaheadRows && scanPos < sidecarText.size(); ++skipped) {
          std::string_view candidate{};
          if (!readLine(sidecarText, scanPos, candidate)) break;
          std::uint64_t candidateTaggedTs = 0u;
          if (!candidate.empty() && parseDepthSidecarTaggedTs(candidate, candidateTaggedTs) && candidateTaggedTs == taggedTs) {
            pendingTapeLine = tapeLine;
            pendingSidecarLine = candidate;
            hasPendingTape = true;
            hasPendingSidecar = true;
            sidecarPos = scanPos;
            droppedSidecarRows = skipped;
            recovered = true;
            break;
          }
        }
      }
      if (!recovered && sidecarTagged) {
        std::size_t scanPos = tapePos;
        for (std::uint64_t skipped = 1u; skipped <= kDepthResyncLookaheadRows && scanPos < tapeText.size(); ++skipped) {
          std::string_view candidate{};
          if (!readLine(tapeText, scanPos, candidate)) break;
          DepthRow candidateRow{};
          std::uint64_t candidateTaggedTs = 0u;
          if (!candidate.empty() && parseDepthTapeLine(candidate, candidateRow, candidateTaggedTs) &&
              candidateTaggedTs == sidecarTaggedTs) {
            pendingTapeLine = candidate;
            pendingSidecarLine = sidecarLine;
            hasPendingTape = true;
            hasPendingSidecar = true;
            tapePos = scanPos;
            droppedTapeRows = skipped;
            recovered = true;
            break;
          }
        }
      }
      if (recovered) {
        appendSessionWarning(warnings,
                             depthRecoveryWarning(lineNo, static_cast<std::uint64_t>(out.size()),
                                                  droppedTapeRows, droppedSidecarRows,
                                                  "depth_tape/depth_sidecar resynced"));
        continue;
      }
      appendSessionWarning(warnings,
                           depthRecoveryWarning(lineNo, static_cast<std::uint64_t>(out.size()),
                                                tapeLine.empty() ? 0u : 1u,
                                                sidecarLine.empty() ? 0u : 1u,
                                                "dropped corrupt depth_tape/depth_sidecar row"));
      continue;
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
  (void)extractPositiveI64Field(text, "\"price_basis_qty_e8\"", out.priceBasisQtyE8);
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
  for (const PriceLimitRow& row : out.priceLimits) updateRange(out, row.tsNs);
  for (const DepthRow& row : out.depths) updateRange(out, row.tsNs);
  for (const CandleRow& row : out.candles) updateRange(out, row.tsNs);
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
  out.exchange = extractStringAfter(manifest, "\"exchange\"");
  out.market = extractStringAfter(manifest, "\"market\"");
  out.symbol = extractStringAfter(manifest, "\"symbols\"");
  const std::string tradesPath = extractChannelPath(manifest, "trades");
  const std::string bookTickerPath = extractChannelPath(manifest, "bookticker");
  const std::string priceLimitPath = extractChannelPath(manifest, "price_limit");
  const std::string depthPath = extractChannelPath(manifest, "depth");
  const std::string depthSidecarPath = extractChannelSidecarPath(manifest, "depth");
  const std::string candlesPath = extractChannelPath(manifest, "candles");
  std::string candles2Path = extractChannelPath(manifest, "candles2");
  if (candles2Path.empty()) candles2Path = extractChannelPath(manifest, "candlesv2");
  const std::string liquidationsPath = extractChannelPath(manifest, "liquidations");
  const std::string markPricePath = extractChannelPath(manifest, "mark_price");
  const std::string indexPricePath = extractChannelPath(manifest, "index_price");
  const std::string fundingPath = extractChannelPath(manifest, "funding");
  const std::string openInterestPath = extractChannelPath(manifest, "open_interest");
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
  if (plan.loadPriceLimits) {
    Status status = loadJsonl<PriceLimitRow>(existingPath(sessionPath, priceLimitPath, "price_limit.jsonl"), "price_limit", plan.requirePriceLimits, out.priceLimits, parsePriceLimitLine, error);
    if (!isOk(status)) return status;
  }
  if (plan.loadDepths) {
    const std::filesystem::path declaredDepth = depthPath.empty()
                                                    ? std::filesystem::path{}
                                                    : sessionPath / std::filesystem::path{depthPath};
    const bool declaredTape = !declaredDepth.empty() && declaredDepth.filename() == "depth_tape.jsonl";
    const std::filesystem::path tapePath = declaredTape
                                               ? existingPath(sessionPath, depthPath, "depth_tape.jsonl")
                                               : existingPath(sessionPath, std::string_view{}, "depth_tape.jsonl");
    const std::filesystem::path sidecarPath = !depthSidecarPath.empty()
                                                  ? existingPath(sessionPath, depthSidecarPath, "depth_sidecar.jsonl")
                                                  : existingPath(sessionPath, std::string_view{}, "depth_sidecar.jsonl");
    Status status = Status::Ok;
    if (std::filesystem::exists(tapePath) || std::filesystem::exists(sidecarPath)) {
      status = loadDepthTapeSidecarJsonl(tapePath, sidecarPath, plan.requireDepths, out.depths, error, out.warnings);
    } else {
      status = loadJsonl<DepthRow>(existingPath(sessionPath, depthPath, "depth.jsonl"), "depth", plan.requireDepths, out.depths, [](std::string_view line, DepthRow& row) noexcept {
        return parseBookDocument(line, row.levels, row.tsNs, &row.ingestSeq);
      }, error);
    }
    if (!isOk(status)) return status;
  }
  if (plan.loadCandles) {
    std::filesystem::path detailedPath = existingPath(sessionPath, candles2Path, "candles2.jsonl");
    if (!std::filesystem::exists(detailedPath)) {
      const std::filesystem::path candlesV2Path = existingPath(sessionPath, std::string_view{}, "candlesv2.jsonl");
      if (std::filesystem::exists(candlesV2Path)) detailedPath = candlesV2Path;
    }
    const std::filesystem::path legacyPath = existingPath(sessionPath, candlesPath, "candles.jsonl");
    const bool detailedExists = std::filesystem::exists(detailedPath);
    Status status = loadJsonl<CandleRow>(detailedExists ? detailedPath : legacyPath,
                                         detailedExists ? "candles2" : "candles",
                                         plan.requireCandles,
                                         out.candles,
                                         parseCandleLine,
                                         error);
    if (!isOk(status)) return status;
  }
  if (plan.loadLiquidations) {
    Status status = loadJsonl<LiquidationRow>(existingPath(sessionPath, liquidationsPath, "liquidations.jsonl"), "liquidations", plan.requireLiquidations, out.liquidations, parseLiquidationLine, error);
    if (!isOk(status)) return status;
  }
  if (plan.loadMarkPrices) {
    Status status = loadJsonl<MarkPriceRow>(existingPath(sessionPath, markPricePath, "mark_price.jsonl"), "mark_price", plan.requireMarkPrices, out.markPrices, parseMarkPriceLine, error);
    if (!isOk(status)) return status;
  }
  if (plan.loadIndexPrices) {
    Status status = loadJsonl<IndexPriceRow>(existingPath(sessionPath, indexPricePath, "index_price.jsonl"), "index_price", plan.requireIndexPrices, out.indexPrices, parseIndexPriceLine, error);
    if (!isOk(status)) return status;
  }
  if (plan.loadFunding) {
    Status status = loadJsonl<FundingRow>(existingPath(sessionPath, fundingPath, "funding.jsonl"), "funding", plan.requireFunding, out.funding, parseFundingLine, error);
    if (!isOk(status)) return status;
  }
  if (plan.loadOpenInterest) {
    Status status = loadJsonl<OpenInterestRow>(existingPath(sessionPath, openInterestPath, "open_interest.jsonl"), "open_interest", plan.requireOpenInterest, out.openInterest, parseOpenInterestLine, error);
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
  out.timelineChannelSorted.fill(true);
  out.timelineOrderKnown = true;
  rebuildReplayRange(out);
  return Status::Ok;
}

Status loadSession(const std::filesystem::path& sessionPath, SessionData& out, std::string& error) noexcept {
  SessionLoadPlan plan{};
  plan.loadCandles = true;
  return loadSession(sessionPath, plan, out, error);
}

}  // namespace hft_backtest::detail
