#include "Internal/SessionLoaderHelpers.hpp"

namespace hft_backtest::detail::session_loader {

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
  if (!hftrec::isOk(hftrec::replay::parseTradeLine(line, row)) ||
      row.priceE8 < 0 || row.qtyE8 < 0) return false;
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

bool rowKeyLess(TimelineRowKey lhs, TimelineRowKey rhs) noexcept {
  if (lhs.tsNs != rhs.tsNs) return lhs.tsNs < rhs.tsNs;
  return lhs.ingestSeq < rhs.ingestSeq;
}

TimelineRowKey rowKey(const TradeRow& row) noexcept { return TimelineRowKey{row.tsNs, row.ingestSeq}; }

TimelineRowKey rowKey(const BookTickerRow& row) noexcept { return TimelineRowKey{row.tsNs, row.ingestSeq}; }

TimelineRowKey rowKey(const DepthRow& row) noexcept { return TimelineRowKey{row.tsNs, row.ingestSeq}; }

TimelineRowKey rowKey(const CandleRow& row) noexcept { return TimelineRowKey{row.tsNs, 0}; }

std::size_t estimatedRowsFor(std::size_t bytes, std::string_view channel) noexcept {
  if (bytes == 0u) return 0u;
  const std::size_t avgBytes = channel == "depth" || channel == "depth_tape"
      ? 192u : (channel == "bookticker" || channel == "candles" || channel == "candles2" ? 64u : 96u);
  return (bytes / avgBytes) + 16u;
}

Status loadDepthTapeSidecarJsonl(const std::filesystem::path& tapePath,
                                 const std::filesystem::path& sidecarPath,
                                 bool required,
                                 std::vector<DepthRow>& out,
                                 std::string& error,
                                 std::vector<std::string>& warnings,
                                 bool* sortedOut) {
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

}  // namespace hft_backtest::detail::session_loader
