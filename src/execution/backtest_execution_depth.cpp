#include "execution/backtest_execution_depth.hpp"

#include "primitives/composite/OrderBookTapeRuntimeV1.hpp"

#include <chrono>
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <new>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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
    try {
      levels.push_back(level);
    } catch (...) {
      return false;
    }
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
    try {
      row.levels.push_back(level);
    } catch (...) {
      return false;
    }
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

void toRuntimeSnapshot(const CursorDepthRow& row,
                       cxet::composite::OrderBookSnapshot& out) noexcept {
  cxet::composite::resetOrderBookSnapshotHeader(out);
  out.ts.raw = static_cast<std::uint64_t>(row.tsNs > 0 ? row.tsNs : 0);
  for (const PriceLevelRow& level : row.levels) {
    Price price{};
    Amount qty{};
    price.raw = level.priceE8;
    qty.raw = level.qtyE8;
    (void)cxet::composite::appendOrderBookLevel(out, price, qty, orderBookSideFromDepthRow(level.side));
  }
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
  cxet::composite::OrderBookSnapshot snapshotScratch{};
  cxet::composite::OrderBookTapeRuntimeV1 tapeScratch{};
  cxet::composite::OrderBookTapeSidesRuntimeV1 sideScratch{};
  bool pendingValid{false};
  bool eof{false};
  std::uint64_t sourceRows{0u};
  DepthSource source{DepthSource::None};
};

namespace {

constexpr std::uint64_t kDepthCheckpointRows = 65536u;

struct ExecutionDepthCheckpoint {
  std::int64_t tsNs{0};
  std::size_t legacyPos{0u};
  std::size_t tapePos{0u};
  std::size_t sidecarPos{0u};
  std::uint64_t sourceRows{0u};
  hft_trader::core::LocalOrderBookState book{};
};

}  // namespace

class ExecutionDepthCheckpointCache {
 public:
  bool restore(std::int64_t tsNs,
               ExecutionDepthProviderImpl& impl,
               hft_trader::core::LocalOrderBookState& book) const noexcept {
    std::lock_guard lock{mutex_};
    const auto it = std::upper_bound(
        checkpoints_.begin(), checkpoints_.end(), tsNs,
        [](std::int64_t value, const ExecutionDepthCheckpoint& checkpoint) noexcept {
          return value < checkpoint.tsNs;
        });
    if (it == checkpoints_.begin()) return false;
    const ExecutionDepthCheckpoint& checkpoint = *(it - 1);
    impl.legacyPos = checkpoint.legacyPos;
    impl.tapePos = checkpoint.tapePos;
    impl.sidecarPos = checkpoint.sidecarPos;
    impl.sourceRows = checkpoint.sourceRows;
    impl.pendingValid = false;
    impl.eof = false;
    book = checkpoint.book;
    return true;
  }

  void publish(const ExecutionDepthProviderImpl& impl,
               std::int64_t tsNs,
               const hft_trader::core::LocalOrderBookState& book) noexcept {
    if (impl.sourceRows == 0u || (impl.sourceRows % kDepthCheckpointRows) != 0u) return;
    try {
      std::lock_guard lock{mutex_};
      if (!checkpoints_.empty() && checkpoints_.back().sourceRows >= impl.sourceRows) return;
      if (!checkpoints_.empty() && checkpoints_.back().tsNs > tsNs) return;
      checkpoints_.push_back(ExecutionDepthCheckpoint{
          tsNs, impl.legacyPos, impl.tapePos, impl.sidecarPos, impl.sourceRows, book});
    } catch (...) {
    }
  }

  void markFailed(const char* reason) noexcept {
    std::lock_guard lock{mutex_};
    failed_ = true;
    failureReason_ = reason;
  }

  bool failed(const char*& reason) const noexcept {
    std::lock_guard lock{mutex_};
    reason = failureReason_;
    return failed_;
  }

 private:
  mutable std::mutex mutex_{};
  std::vector<ExecutionDepthCheckpoint> checkpoints_{};
  bool failed_{false};
  const char* failureReason_{""};
};

std::shared_ptr<ExecutionDepthCheckpointCache> makeExecutionDepthCheckpointCache() {
  return std::make_shared<ExecutionDepthCheckpointCache>();
}

ExecutionDepthProvider::~ExecutionDepthProvider() {
  delete impl_;
}

ExecutionDepthOpenStatus ExecutionDepthProvider::open(
    const std::filesystem::path& sessionPath,
    const Symbol& symbol,
    const hft_trader::core::LocalOrderBookConfig& config,
    const hft_trader::core::LocalOrderBookState& initialBook,
    std::shared_ptr<ExecutionDepthCheckpointCache> cache) noexcept {
  delete impl_;
  impl_ = nullptr;
  book_ = initialBook;
  symbol_ = symbol;
  config_ = config;
  rowsRead_ = 0;
  readNs_ = 0;
  available_ = false;
  failed_ = false;
  failureReason_ = "";
  cache_ = std::move(cache);
  try {
    if (cache_) {
      const char* cachedFailure = "";
      if (cache_->failed(cachedFailure)) {
        failed_ = true;
        failureReason_ = cachedFailure;
        return ExecutionDepthOpenStatus::Failed;
      }
    }

  std::string manifest;
  (void)readText(sessionPath / "manifest.json", manifest);
  const std::string depthPath = extractChannelPath(manifest, "depth");
  const std::string depthSidecarPath = extractChannelSidecarPath(manifest, "depth");
  const bool depthDeclared = !depthPath.empty() || !depthSidecarPath.empty();

  const std::filesystem::path declaredDepth = !depthPath.empty()
      ? sessionPath / std::filesystem::path{depthPath}
      : std::filesystem::path{};
  const bool declaredTape = !declaredDepth.empty() && declaredDepth.filename() == "depth_tape.jsonl";
  const bool declaredTapePair = declaredTape || !depthSidecarPath.empty();
  std::filesystem::path tapePath{};
  std::filesystem::path sidecarPath{};
  std::filesystem::path legacyPath{};
  if (depthDeclared) {
    if (declaredTapePair) {
      tapePath = !depthPath.empty()
          ? sessionPath / std::filesystem::path{depthPath}
          : existingPath(sessionPath, std::string_view{}, "depth_tape.jsonl");
      sidecarPath = !depthSidecarPath.empty()
          ? sessionPath / std::filesystem::path{depthSidecarPath}
          : existingPath(sessionPath, std::string_view{}, "depth_sidecar.jsonl");
    } else {
      legacyPath = declaredDepth;
    }
  } else {
    tapePath = existingPath(sessionPath, std::string_view{}, "depth_tape.jsonl");
    sidecarPath = existingPath(sessionPath, std::string_view{}, "depth_sidecar.jsonl");
    legacyPath = existingPath(sessionPath, std::string_view{}, "depth.jsonl");
  }
  const bool hasTape = !tapePath.empty() && !sidecarPath.empty() &&
                       std::filesystem::exists(tapePath) && std::filesystem::exists(sidecarPath);
  const bool hasLegacy = !legacyPath.empty() && std::filesystem::exists(legacyPath);
  if (!hasTape && !hasLegacy) {
    if (depthDeclared) {
      failed_ = true;
      failureReason_ = "declared_files_missing";
      return ExecutionDepthOpenStatus::Failed;
    }
    return ExecutionDepthOpenStatus::Absent;
  }

  std::unique_ptr<ExecutionDepthProviderImpl> next{new (std::nothrow) ExecutionDepthProviderImpl{}};
  if (next == nullptr) {
    failed_ = true;
    failureReason_ = "allocation_failed";
    return ExecutionDepthOpenStatus::Failed;
  }

  if (hasTape) {
    if (!next->tapeFile.open(tapePath) || !next->sidecarFile.open(sidecarPath)) {
      failed_ = true;
      failureReason_ = "open_failed";
      return ExecutionDepthOpenStatus::Failed;
    }
    next->tapeText = next->tapeFile.view();
    next->sidecarText = next->sidecarFile.view();
    next->source = DepthSource::TapeSidecar;
  } else {
    if (!next->legacyFile.open(legacyPath)) {
      failed_ = true;
      failureReason_ = "open_failed";
      return ExecutionDepthOpenStatus::Failed;
    }
    next->legacyText = next->legacyFile.view();
    next->source = DepthSource::LegacySnapshot;
  }

  impl_ = next.release();
  available_ = true;
  return ExecutionDepthOpenStatus::Ready;
  } catch (...) {
    delete impl_;
    impl_ = nullptr;
    available_ = false;
    failed_ = true;
    failureReason_ = "allocation_failed";
    return ExecutionDepthOpenStatus::Failed;
  }
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
                   const CursorDepthRow& row,
                   cxet::composite::OrderBookSnapshot& snapshotScratch,
                   cxet::composite::OrderBookTapeRuntimeV1& tapeScratch,
                   cxet::composite::OrderBookTapeSidesRuntimeV1& sideScratch) noexcept {
  if (source == DepthSource::LegacySnapshot) {
    toRuntimeSnapshot(row, snapshotScratch);
    hft_trader::core::applySnapshot(book, snapshotScratch, symbol, config);
    return true;
  }
  if (source == DepthSource::TapeSidecar) {
    if (!hft_trader::core::orderBook(book).initialized()) {
      toRuntimeSnapshot(row, snapshotScratch);
      hft_trader::core::applySnapshot(book, snapshotScratch, symbol, config);
      return true;
    }
    toRuntime(row, tapeScratch, sideScratch);
    return hft_trader::core::applyTapeDelta(book, tapeScratch, sideScratch);
  }
  return false;
}

bool ExecutionDepthProvider::advanceTo(std::int64_t tsNs) noexcept {
  if (!available_ || impl_ == nullptr) return false;
  if (tsNs <= 0) return ready();
  if (cache_ && impl_->sourceRows == 0u && !impl_->pendingValid) {
    (void)cache_->restore(tsNs, *impl_, book_);
  }
  if (impl_->eof || (impl_->pendingValid && impl_->pending.tsNs > tsNs)) return ready();
  const auto readStart = std::chrono::steady_clock::now();
  auto finishRead = [&]() noexcept {
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - readStart).count();
    if (elapsed > 0) readNs_ += static_cast<std::uint64_t>(elapsed);
  };
  while (!impl_->eof) {
    if (impl_->pendingValid) {
      if (impl_->pending.tsNs > tsNs) break;
      const std::int64_t appliedTsNs = impl_->pending.tsNs;
      if (!applyDepthRow(impl_->source,
                         book_,
                         symbol_,
                         config_,
                         impl_->pending,
                         impl_->snapshotScratch,
                         impl_->tapeScratch,
                         impl_->sideScratch)) {
        available_ = false;
        failed_ = true;
        failureReason_ = "apply_failed";
        if (cache_) cache_->markFailed(failureReason_);
        finishRead();
        return false;
      }
      impl_->pending.clear();
      impl_->pendingValid = false;
      ++rowsRead_;
      ++impl_->sourceRows;
      if (cache_) cache_->publish(*impl_, appliedTsNs, book_);
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
      failed_ = true;
      failureReason_ = "parse_failed";
      if (cache_) cache_->markFailed(failureReason_);
      finishRead();
      return false;
    }
    if (impl_->scratch.tsNs > tsNs) {
      try {
        impl_->pending = impl_->scratch;
      } catch (...) {
        available_ = false;
        failed_ = true;
        failureReason_ = "allocation_failed";
        if (cache_) cache_->markFailed(failureReason_);
        finishRead();
        return false;
      }
      impl_->pendingValid = true;
      break;
    }
    if (!applyDepthRow(impl_->source,
                       book_,
                       symbol_,
                       config_,
                       impl_->scratch,
                       impl_->snapshotScratch,
                       impl_->tapeScratch,
                       impl_->sideScratch)) {
      available_ = false;
      failed_ = true;
      failureReason_ = "apply_failed";
      if (cache_) cache_->markFailed(failureReason_);
      finishRead();
      return false;
    }
    ++rowsRead_;
    ++impl_->sourceRows;
    if (cache_) cache_->publish(*impl_, impl_->scratch.tsNs, book_);
  }
  finishRead();
  return ready();
}

}  // namespace hft_backtest::detail
