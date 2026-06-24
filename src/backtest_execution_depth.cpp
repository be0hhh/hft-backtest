#include "backtest_execution_depth.hpp"

#include "primitives/composite/OrderBookTapeRuntimeV1.hpp"

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <new>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "absl/container/inlined_vector.h"

namespace hft_backtest::detail {
namespace {

class MappedFile {
 public:
  MappedFile() noexcept = default;
  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;

  ~MappedFile() {
    close();
  }

  void close() noexcept {
    if (data_ && data_ != MAP_FAILED) (void)::munmap(data_, size_);
    data_ = nullptr;
    size_ = 0u;
    if (fd_ >= 0) (void)::close(fd_);
    fd_ = -1;
  }

  [[nodiscard]] bool open(const std::filesystem::path& path) noexcept {
    close();
    fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) return false;
    struct stat st {};
    if (::fstat(fd_, &st) != 0) return false;
    if (st.st_size <= 0) return true;
    size_ = static_cast<std::size_t>(st.st_size);
    data_ = ::mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
    return data_ != MAP_FAILED;
  }

  [[nodiscard]] std::string_view view() const noexcept {
    if (!data_ || data_ == MAP_FAILED || size_ == 0u) return {};
    return std::string_view(static_cast<const char*>(data_), size_);
  }

 private:
  int fd_{-1};
  void* data_{nullptr};
  std::size_t size_{0u};
};

enum class DepthSource : std::uint8_t {
  None = 0u,
  LegacySnapshot = 1u,
  TapeSidecar = 2u,
};

enum class ReadDepthRowResult : std::uint8_t {
  Eof = 0u,
  Row = 1u,
  Corrupt = 2u,
};

constexpr std::size_t kCursorInlineDepthLevels = 64u;

struct CursorDepthRow {
  std::int64_t tsNs{0};
  std::int64_t ingestSeq{0};
  absl::InlinedVector<PriceLevelRow, kCursorInlineDepthLevels> levels{};

  void clear() {
    tsNs = 0;
    ingestSeq = 0;
    levels.clear();
  }
};

[[nodiscard]] bool readText(const std::filesystem::path& path, std::string& out) {
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

[[nodiscard]] bool consume(std::string_view text, std::size_t& pos, char expected) noexcept {
  skipWs(text, pos);
  if (pos >= text.size() || text[pos] != expected) return false;
  ++pos;
  return true;
}

[[nodiscard]] bool parseI64(std::string_view text, std::size_t& pos, std::int64_t& out) noexcept {
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
  out = negative
      ? (value == kMaxNegativeAbs ? std::numeric_limits<std::int64_t>::min() : -static_cast<std::int64_t>(value))
      : static_cast<std::int64_t>(value);
  return true;
}

[[nodiscard]] bool parseU64(std::string_view text, std::size_t& pos, std::uint64_t& out) noexcept {
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

[[nodiscard]] bool parseLevel(std::string_view text, std::size_t& pos, PriceLevelRow& out) noexcept {
  return consume(text, pos, '[') && parseI64(text, pos, out.priceE8) && consume(text, pos, ',') &&
         parseI64(text, pos, out.qtyE8) && consume(text, pos, ',') && parseI64(text, pos, out.side) &&
         (out.side == 0 || out.side == 1) && consume(text, pos, ']');
}

template<typename Levels>
[[nodiscard]] bool parseBookDocument(std::string_view line,
                                     Levels& levels,
                                     std::int64_t& tsNs,
                                     std::int64_t* ingestSeq = nullptr) noexcept {
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
  if (consume(line, pos, ',') &&
      (!parseI64(line, pos, captureSeq) || !consume(line, pos, ',') || !parseI64(line, pos, parsedIngestSeq))) {
    return false;
  }
  if (ingestSeq) *ingestSeq = parsedIngestSeq;
  return true;
}

[[nodiscard]] bool parseDepthTapeLine(std::string_view line, CursorDepthRow& row, std::uint64_t& taggedTs) noexcept {
  std::size_t pos = 0;
  row.clear();
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

[[nodiscard]] bool applyDepthSidecarLine(std::string_view line,
                                         std::uint64_t expectedTaggedTs,
                                         CursorDepthRow& row) noexcept {
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

[[nodiscard]] bool readLine(std::string_view text, std::size_t& pos, std::string_view& line) noexcept {
  if (pos >= text.size()) return false;
  const std::size_t lineStart = pos;
  while (pos < text.size() && text[pos] != '\n') ++pos;
  std::size_t lineEnd = pos;
  if (pos < text.size() && text[pos] == '\n') ++pos;
  if (lineEnd > lineStart && text[lineEnd - 1u] == '\r') --lineEnd;
  line = std::string_view{text.data() + lineStart, lineEnd - lineStart};
  return true;
}

[[nodiscard]] std::string extractStringAfter(std::string_view text, std::string_view key) {
  const std::size_t keyPos = text.find(key);
  if (keyPos == std::string_view::npos) return {};
  const std::size_t colon = text.find(':', keyPos + key.size());
  if (colon == std::string_view::npos) return {};
  std::size_t firstQuote = colon + 1u;
  skipWs(text, firstQuote);
  if (firstQuote >= text.size() || text[firstQuote] != '"') return {};
  std::size_t secondQuote = firstQuote + 1u;
  while (secondQuote < text.size()) {
    if (text[secondQuote] == '"' && text[secondQuote - 1u] != '\\') break;
    ++secondQuote;
  }
  if (secondQuote >= text.size()) return {};
  return std::string{text.substr(firstQuote + 1u, secondQuote - firstQuote - 1u)};
}

[[nodiscard]] std::string_view extractObjectAfter(std::string_view text, std::string_view key) noexcept {
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

[[nodiscard]] std::string extractChannelPath(std::string_view manifest, std::string_view channel) {
  std::string key{"\""};
  key += channel;
  key += "\"";
  const std::string_view object = extractObjectAfter(manifest, key);
  if (object.empty()) return extractStringAfter(manifest, key);
  return extractStringAfter(object, "\"path\"");
}

[[nodiscard]] std::string extractChannelSidecarPath(std::string_view manifest, std::string_view channel) {
  std::string key{"\""};
  key += channel;
  key += "\"";
  const std::string_view object = extractObjectAfter(manifest, key);
  return object.empty() ? std::string{} : extractStringAfter(object, "\"sidecar_path\"");
}

[[nodiscard]] std::filesystem::path existingPath(const std::filesystem::path& root,
                                                 std::string_view path,
                                                 std::string_view fallback) {
  const std::filesystem::path declared = root / std::filesystem::path{std::string{path}};
  if (!path.empty() && std::filesystem::exists(declared)) return declared;
  const std::filesystem::path direct = root / std::filesystem::path{std::string{fallback}};
  if (std::filesystem::exists(direct)) return direct;
  return root / "jsonl" / std::filesystem::path{std::string{fallback}};
}

[[nodiscard]] Side orderBookSideFromDepthRow(std::int64_t side) noexcept {
  return side == 0 ? Side::Buy() : Side::Sell();
}

[[nodiscard]] cxet::composite::OrderBookSnapshot toRuntimeSnapshot(const CursorDepthRow& row) noexcept {
  cxet::composite::OrderBookSnapshot out{};
  out.ts.raw = static_cast<std::uint64_t>(row.tsNs > 0 ? row.tsNs : 0);
  for (const PriceLevelRow& level : row.levels) {
    Price price{};
    Amount qty{};
    price.raw = level.priceE8;
    qty.raw = level.qtyE8;
    (void)cxet::composite::appendOrderBookLevel(out, price, qty, orderBookSideFromDepthRow(level.side));
  }
  return out;
}

void toRuntime(const CursorDepthRow& row,
               cxet::composite::OrderBookTapeRuntimeV1& tape,
               cxet::composite::OrderBookTapeSidesRuntimeV1& sides) noexcept {
  cxet::composite::resetOrderBookTapeRuntimeHeader(tape);
  cxet::composite::resetOrderBookTapeSidesRuntimeHeader(sides);
  TimeNs ts{};
  ts.raw = static_cast<std::uint64_t>(row.tsNs > 0 ? row.tsNs : 0);
  (void)cxet::composite::appendOrderBookTapeTimestamp(tape, ts);
  for (const PriceLevelRow& level : row.levels) {
    Price price{};
    Amount qty{};
    price.raw = level.priceE8;
    qty.raw = level.qtyE8;
    (void)cxet::composite::appendOrderBookTapePriceQtyWithSide(
        tape,
        sides,
        price,
        qty,
        orderBookSideFromDepthRow(level.side));
  }
}

}  // namespace

struct ExecutionDepthProviderImpl {
  MappedFile legacyFile{};
  MappedFile tapeFile{};
  MappedFile sidecarFile{};
  std::string_view legacyText{};
  std::string_view tapeText{};
  std::string_view sidecarText{};
  std::size_t legacyPos{0u};
  std::size_t tapePos{0u};
  std::size_t sidecarPos{0u};
  CursorDepthRow pending{};
  CursorDepthRow scratch{};
  bool pendingValid{false};
  bool eof{false};
  DepthSource source{DepthSource::None};
};

ExecutionDepthProvider::~ExecutionDepthProvider() {
  delete impl_;
}

bool ExecutionDepthProvider::open(const std::filesystem::path& sessionPath,
                                  const Symbol& symbol,
                                  const hft_trader::core::LocalOrderBookConfig& config,
                                  const hft_trader::core::LocalOrderBookState& initialBook) noexcept {
  delete impl_;
  impl_ = nullptr;
  book_ = initialBook;
  symbol_ = symbol;
  config_ = config;
  rowsRead_ = 0;
  available_ = false;

  std::string manifest;
  (void)readText(sessionPath / "manifest.json", manifest);
  const std::string depthPath = extractChannelPath(manifest, "depth");
  const std::string depthSidecarPath = extractChannelSidecarPath(manifest, "depth");

  const std::filesystem::path declaredDepth = !depthPath.empty()
      ? sessionPath / std::filesystem::path{depthPath}
      : std::filesystem::path{};
  const bool declaredTape = !declaredDepth.empty() && declaredDepth.filename() == "depth_tape.jsonl";
  std::filesystem::path tapePath = declaredTape
      ? existingPath(sessionPath, depthPath, "depth_tape.jsonl")
      : existingPath(sessionPath, std::string_view{}, "depth_tape.jsonl");
  std::filesystem::path sidecarPath = !depthSidecarPath.empty()
      ? existingPath(sessionPath, depthSidecarPath, "depth_sidecar.jsonl")
      : existingPath(sessionPath, std::string_view{}, "depth_sidecar.jsonl");
  const bool hasTape = std::filesystem::exists(tapePath) && std::filesystem::exists(sidecarPath);

  std::filesystem::path legacyPath = existingPath(sessionPath, depthPath, "depth.jsonl");
  const bool hasLegacy = std::filesystem::exists(legacyPath);
  if (!hasTape && !hasLegacy) return false;

  ExecutionDepthProviderImpl* next = new (std::nothrow) ExecutionDepthProviderImpl{};
  if (next == nullptr) return false;

  if (hasTape) {
    if (!next->tapeFile.open(tapePath) || !next->sidecarFile.open(sidecarPath)) {
      delete next;
      return false;
    }
    next->tapeText = next->tapeFile.view();
    next->sidecarText = next->sidecarFile.view();
    next->source = DepthSource::TapeSidecar;
  } else {
    if (!next->legacyFile.open(legacyPath)) {
      delete next;
      return false;
    }
    next->legacyText = next->legacyFile.view();
    next->source = DepthSource::LegacySnapshot;
  }

  impl_ = next;
  available_ = true;
  return true;
}

bool ExecutionDepthProvider::ready() const noexcept {
  return available_ && hft_trader::core::orderBook(book_).initialized();
}

ReadDepthRowResult readLegacyRow(ExecutionDepthProviderImpl& impl, CursorDepthRow& out) noexcept {
  while (impl.legacyPos < impl.legacyText.size()) {
    std::string_view line{};
    if (!readLine(impl.legacyText, impl.legacyPos, line)) break;
    if (line.empty()) continue;
    out.clear();
    if (!parseBookDocument(line, out.levels, out.tsNs, &out.ingestSeq)) return ReadDepthRowResult::Corrupt;
    return ReadDepthRowResult::Row;
  }
  return ReadDepthRowResult::Eof;
}

ReadDepthRowResult readTapeSidecarRow(ExecutionDepthProviderImpl& impl, CursorDepthRow& out) noexcept {
  while (impl.tapePos < impl.tapeText.size() || impl.sidecarPos < impl.sidecarText.size()) {
    std::string_view tapeLine{};
    std::string_view sidecarLine{};
    const bool hasTape = readLine(impl.tapeText, impl.tapePos, tapeLine);
    const bool hasSidecar = readLine(impl.sidecarText, impl.sidecarPos, sidecarLine);
    if (!hasTape && !hasSidecar) break;
    if (tapeLine.empty() && sidecarLine.empty()) continue;
    if (!hasTape || !hasSidecar || tapeLine.empty() || sidecarLine.empty()) return ReadDepthRowResult::Corrupt;
    out.clear();
    std::uint64_t taggedTs = 0u;
    if (!parseDepthTapeLine(tapeLine, out, taggedTs) || !applyDepthSidecarLine(sidecarLine, taggedTs, out)) {
      return ReadDepthRowResult::Corrupt;
    }
    return ReadDepthRowResult::Row;
  }
  return ReadDepthRowResult::Eof;
}

ReadDepthRowResult readNextDepthRow(ExecutionDepthProviderImpl& impl, CursorDepthRow& out) noexcept {
  if (impl.source == DepthSource::LegacySnapshot) return readLegacyRow(impl, out);
  if (impl.source == DepthSource::TapeSidecar) return readTapeSidecarRow(impl, out);
  return ReadDepthRowResult::Eof;
}

bool applyDepthRow(DepthSource source,
                   hft_trader::core::LocalOrderBookState& book,
                   const Symbol& symbol,
                   const hft_trader::core::LocalOrderBookConfig& config,
                   const CursorDepthRow& row) noexcept {
  if (source == DepthSource::LegacySnapshot) {
    hft_trader::core::applySnapshot(book, toRuntimeSnapshot(row), symbol, config);
    return true;
  }
  if (source == DepthSource::TapeSidecar) {
    if (!hft_trader::core::orderBook(book).initialized()) {
      hft_trader::core::applySnapshot(book, toRuntimeSnapshot(row), symbol, config);
      return true;
    }
    cxet::composite::OrderBookTapeRuntimeV1 tape{};
    cxet::composite::OrderBookTapeSidesRuntimeV1 sides{};
    toRuntime(row, tape, sides);
    return hft_trader::core::applyTapeDelta(book, tape, sides);
  }
  return false;
}

bool ExecutionDepthProvider::advanceTo(std::int64_t tsNs) noexcept {
  if (!available_ || impl_ == nullptr) return false;
  if (tsNs <= 0) return ready();
  while (!impl_->eof) {
    if (impl_->pendingValid) {
      if (impl_->pending.tsNs > tsNs) break;
      if (!applyDepthRow(impl_->source, book_, symbol_, config_, impl_->pending)) {
        available_ = false;
        return false;
      }
      impl_->pending.clear();
      impl_->pendingValid = false;
      ++rowsRead_;
      continue;
    }
    impl_->scratch.clear();
    const ReadDepthRowResult status = readNextDepthRow(*impl_, impl_->scratch);
    if (status == ReadDepthRowResult::Eof) {
      impl_->eof = true;
      break;
    }
    if (status == ReadDepthRowResult::Corrupt) {
      available_ = false;
      return false;
    }
    if (impl_->scratch.tsNs > tsNs) {
      impl_->pending = impl_->scratch;
      impl_->pendingValid = true;
      break;
    }
    if (!applyDepthRow(impl_->source, book_, symbol_, config_, impl_->scratch)) {
      available_ = false;
      return false;
    }
    ++rowsRead_;
  }
  return ready();
}

}  // namespace hft_backtest::detail
