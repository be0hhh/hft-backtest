#include "execution/backtest_execution_depth.hpp"

#include "engine/backtest_replay_clock.hpp"
#include "core/capture/SessionManifest.hpp"
#include "core/replay/JsonLineParser.hpp"
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

enum class ReadDepthRowResult : std::uint8_t {
  Eof = 0u,
  Row = 1u,
  Corrupt = 2u,
};

constexpr std::size_t kCursorInlineDepthLevels = 64u;

struct CursorDepthRow {
  std::int64_t exchangeTsNs{0};
  std::int64_t scheduleTsNs{0};
  std::int64_t deliveryTsNs{0};
  std::int64_t ingestSeq{0};
  hftrec::replay::EventArrival arrival{};
  absl::InlinedVector<PriceLevelRow, kCursorInlineDepthLevels> levels{};

  void clear() {
    exchangeTsNs = 0;
    scheduleTsNs = 0;
    deliveryTsNs = 0;
    ingestSeq = 0;
    arrival = {};
    levels.clear();
  }
};

struct DepthScheduleKey {
  std::int64_t scheduleTsNs{0};
  std::int64_t deliveryTsNs{0};
  hftrec::replay::EventArrival arrival{};
};

[[nodiscard]] bool readText(const std::filesystem::path& path, std::string& out) {
  std::ifstream in(path, std::ios::in | std::ios::binary);
  if (!in.is_open()) return false;
  std::ostringstream ss;
  ss << in.rdbuf();
  out = ss.str();
  return true;
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

[[nodiscard]] Side orderBookSideFromDepthRow(std::int64_t side) noexcept {
  return side == 0 ? Side::Buy() : Side::Sell();
}

void toRuntime(const CursorDepthRow& row,
               cxet::composite::OrderBookTapeRuntimeV1& tape,
               cxet::composite::OrderBookTapeSidesRuntimeV1& sides) noexcept {
  cxet::composite::resetOrderBookTapeRuntimeHeader(tape);
  cxet::composite::resetOrderBookTapeSidesRuntimeHeader(sides);
  TimeNs ts{};
  ts.raw = static_cast<std::uint64_t>(
      row.exchangeTsNs > 0 ? row.exchangeTsNs : 0);
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
  MappedFile tapeFile{};
  MappedFile sidecarFile{};
  std::string_view tapeText{};
  std::string_view sidecarText{};
  std::size_t tapePos{0u};
  std::size_t sidecarPos{0u};
  CapturedReplayClockAnchor clockAnchor{};
  hftrec::replay::DepthRow contractScratch{};
  DepthScheduleKey lastSourceKey{};
  CursorDepthRow pending{};
  CursorDepthRow scratch{};
  cxet::composite::OrderBookTapeRuntimeV1 tapeScratch{};
  cxet::composite::OrderBookTapeSidesRuntimeV1 sideScratch{};
  bool lastSourceRowValid{false};
  bool pendingValid{false};
  bool eof{false};
  std::uint64_t sourceRows{0u};
};

namespace {

constexpr std::uint64_t kDepthCheckpointRows = 65536u;

struct ExecutionDepthCheckpoint {
  std::int64_t tsNs{0};
  std::size_t tapePos{0u};
  std::size_t sidecarPos{0u};
  std::uint64_t sourceRows{0u};
  DepthScheduleKey lastSourceKey{};
  bool lastSourceRowValid{false};
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
    impl.tapePos = checkpoint.tapePos;
    impl.sidecarPos = checkpoint.sidecarPos;
    impl.sourceRows = checkpoint.sourceRows;
    impl.lastSourceKey = checkpoint.lastSourceKey;
    impl.lastSourceRowValid = checkpoint.lastSourceRowValid;
    impl.pendingValid = false;
    impl.eof = false;
    return book.copyFromBounded(checkpoint.book);
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
          tsNs,
          impl.tapePos,
          impl.sidecarPos,
          impl.sourceRows,
          impl.lastSourceKey,
          impl.lastSourceRowValid,
          book});
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

ReadDepthRowResult readNextDepthRow(ExecutionDepthProviderImpl& impl,
                                    CursorDepthRow& out) noexcept;

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
  CountVal bookCapacity =
      hft_trader::core::boundedMaxStoredLevels(
          config.maxStoredLevelsPerSide);
  if (config.maxStoredLevelsPerSide.raw == 0u &&
      initialBook.capacityLevelsPerSide().raw != 0u) {
    bookCapacity = initialBook.capacityLevelsPerSide();
  }
  if (!book_.reserveLevelsPerSide(bookCapacity) ||
      (initialBook.initialized.raw ==
           hft_trader::core::kAlgoFlagTrue.raw &&
       !book_.copyFromBounded(initialBook))) {
    available_ = false;
    failed_ = true;
    failureReason_ = "allocation_failed";
    return ExecutionDepthOpenStatus::Failed;
  }
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

  std::string manifestDocument;
  hftrec::capture::SessionManifest manifest{};
  if (!readText(sessionPath / "manifest.json", manifestDocument) ||
      !hftrec::isOk(hftrec::capture::parseManifestJson(
          manifestDocument, manifest)) ||
      manifest.manifestSchemaVersion !=
          hftrec::capture::kManifestSchemaVersionCurrent ||
      manifest.corpusSchemaVersion !=
          hftrec::capture::kCorpusSchemaVersionCurrent ||
      manifest.captureContractVersion !=
          hftrec::capture::kCaptureContractVersionCurrent ||
      manifest.sessionStatus != "complete" ||
      !manifest.structurallyLoadable ||
      manifest.sessionHealth != hftrec::SessionHealth::Clean ||
      !manifest.exactReplayEligible ||
      manifest.arrivalClock.unavailableRows != 0u ||
      manifest.arrivalClock.firstReceiveRealtimeNs <= 0 ||
      manifest.arrivalClock.firstReceiveMonotonicNs == 0u) {
    failed_ = true;
    failureReason_ = "manifest_contract_mismatch";
    return ExecutionDepthOpenStatus::Failed;
  }
  if (!manifest.orderbookEnabled || manifest.depthCount == 0u) {
    return ExecutionDepthOpenStatus::Absent;
  }
  if (manifest.depthRowSchema != hftrec::capture::kDepthRowSchemaCurrent ||
      manifest.depthPath.empty() || manifest.depthSidecarPath.empty()) {
    failed_ = true;
    failureReason_ = "depth_contract_mismatch";
    return ExecutionDepthOpenStatus::Failed;
  }
  const hftrec::capture::ChannelRuntimeHealth& depthRuntime =
      manifest.depthRuntime;
  if (depthRuntime.state != "live" || depthRuntime.reconnectCount != 0u ||
      depthRuntime.droppedEventCount != 0u ||
      depthRuntime.unroutableEventCount != 0u ||
      !depthRuntime.lastError.empty()) {
    failed_ = true;
    failureReason_ = "depth_capture_not_clean";
    return ExecutionDepthOpenStatus::Failed;
  }

  const std::filesystem::path tapePath =
      sessionPath / std::filesystem::path{manifest.depthPath};
  const std::filesystem::path sidecarPath =
      sessionPath / std::filesystem::path{manifest.depthSidecarPath};
  if (!std::filesystem::exists(tapePath) ||
      !std::filesystem::exists(sidecarPath)) {
    failed_ = true;
    failureReason_ = "declared_files_missing";
    return ExecutionDepthOpenStatus::Failed;
  }

  std::unique_ptr<ExecutionDepthProviderImpl> next{new (std::nothrow) ExecutionDepthProviderImpl{}};
  if (next == nullptr) {
    failed_ = true;
    failureReason_ = "allocation_failed";
    return ExecutionDepthOpenStatus::Failed;
  }

  if (!next->tapeFile.open(tapePath) ||
      !next->sidecarFile.open(sidecarPath)) {
    failed_ = true;
    failureReason_ = "open_failed";
    return ExecutionDepthOpenStatus::Failed;
  }
  next->tapeText = next->tapeFile.view();
  next->sidecarText = next->sidecarFile.view();
  next->clockAnchor.firstReceiveRealtimeNs =
      manifest.arrivalClock.firstReceiveRealtimeNs;
  next->clockAnchor.firstReceiveMonotonicNs =
      manifest.arrivalClock.firstReceiveMonotonicNs;

  std::uint64_t validatedRows = 0u;
  CursorDepthRow validationRow{};
  while (true) {
    const ReadDepthRowResult validationStatus =
        readNextDepthRow(*next, validationRow);
    if (validationStatus == ReadDepthRowResult::Eof) break;
    if (validationStatus == ReadDepthRowResult::Corrupt ||
        validatedRows == std::numeric_limits<std::uint64_t>::max()) {
      failed_ = true;
      failureReason_ = "depth_contract_validation_failed";
      if (cache_) cache_->markFailed(failureReason_);
      return ExecutionDepthOpenStatus::Failed;
    }
    ++validatedRows;
  }
  if (validatedRows != manifest.depthCount) {
    failed_ = true;
    failureReason_ = "depth_declared_count_mismatch";
    if (cache_) cache_->markFailed(failureReason_);
    return ExecutionDepthOpenStatus::Failed;
  }
  next->tapePos = 0u;
  next->sidecarPos = 0u;
  next->lastSourceKey = {};
  next->lastSourceRowValid = false;

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

bool depthScheduleLess(const CursorDepthRow& lhs,
                       const DepthScheduleKey& rhs) noexcept {
  if (lhs.scheduleTsNs != rhs.scheduleTsNs)
    return lhs.scheduleTsNs < rhs.scheduleTsNs;
  if (lhs.deliveryTsNs != rhs.deliveryTsNs)
    return lhs.deliveryTsNs < rhs.deliveryTsNs;
  if (lhs.arrival.producerEpoch != rhs.arrival.producerEpoch)
    return lhs.arrival.producerEpoch < rhs.arrival.producerEpoch;
  if (lhs.arrival.shardId != rhs.arrival.shardId)
    return lhs.arrival.shardId < rhs.arrival.shardId;
  if (lhs.arrival.shardSequence != rhs.arrival.shardSequence)
    return lhs.arrival.shardSequence < rhs.arrival.shardSequence;
  if (lhs.arrival.eventOrdinal != rhs.arrival.eventOrdinal)
    return lhs.arrival.eventOrdinal < rhs.arrival.eventOrdinal;
  if (lhs.arrival.sourceId != rhs.arrival.sourceId)
    return lhs.arrival.sourceId < rhs.arrival.sourceId;
  if (lhs.arrival.sourceGeneration != rhs.arrival.sourceGeneration)
    return lhs.arrival.sourceGeneration < rhs.arrival.sourceGeneration;
  if (lhs.arrival.sessionEpoch != rhs.arrival.sessionEpoch)
    return lhs.arrival.sessionEpoch < rhs.arrival.sessionEpoch;
  return lhs.arrival.frameSequence < rhs.arrival.frameSequence;
}

bool copyContractDepthRow(const hftrec::replay::DepthRow& source,
                          const CapturedReplayClockAnchor& anchor,
                          CursorDepthRow& out) noexcept {
  out.clear();
  const CapturedReplayCoordinate clock = projectCapturedReplayCoordinate(
      anchor, source.arrival, source.tsNs);
  if (clock.deliveryTsNs <= 0 || clock.venueTsNs <= 0) return false;
  out.exchangeTsNs = source.tsNs;
  out.scheduleTsNs = clock.venueTsNs;
  out.deliveryTsNs = clock.deliveryTsNs;
  out.ingestSeq = source.ingestSeq;
  out.arrival = source.arrival;
  try {
    out.levels.reserve(source.levels.size());
    for (const hftrec::replay::PricePair& level : source.levels) {
      out.levels.push_back(PriceLevelRow{
          level.priceE8, level.qtyE8, level.side});
    }
  } catch (...) {
    out.clear();
    return false;
  }
  return true;
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
    if (!hftrec::isOk(hftrec::replay::parseDepthTapeSidecarLine(
            tapeLine, sidecarLine, impl.contractScratch)) ||
        !copyContractDepthRow(
            impl.contractScratch, impl.clockAnchor, out) ||
        (impl.lastSourceRowValid &&
         depthScheduleLess(out, impl.lastSourceKey))) {
      return ReadDepthRowResult::Corrupt;
    }
    impl.lastSourceKey = DepthScheduleKey{
        out.scheduleTsNs, out.deliveryTsNs, out.arrival};
    impl.lastSourceRowValid = true;
    return ReadDepthRowResult::Row;
  }
  return ReadDepthRowResult::Eof;
}

ReadDepthRowResult readNextDepthRow(ExecutionDepthProviderImpl& impl, CursorDepthRow& out) noexcept {
  return readTapeSidecarRow(impl, out);
}

bool applyDepthRow(hft_trader::core::LocalOrderBookState& book,
                   const Symbol& symbol,
                   const hft_trader::core::LocalOrderBookConfig& config,
                   const CursorDepthRow& row,
                   cxet::composite::OrderBookTapeRuntimeV1& tapeScratch,
                   cxet::composite::OrderBookTapeSidesRuntimeV1& sideScratch) noexcept {
  if (!hft_trader::core::orderBook(book).initialized()) {
    book.symbol = symbol;
    book.config = config;
    if (book.config.maxStoredLevelsPerSide.raw == 0u) {
      book.config.maxStoredLevelsPerSide =
          book.capacityLevelsPerSide();
    }
  }
  toRuntime(row, tapeScratch, sideScratch);
  return hft_trader::core::applyTapeDelta(book, tapeScratch, sideScratch);
}

bool ExecutionDepthProvider::advanceTo(std::int64_t tsNs) noexcept {
  if (!available_ || impl_ == nullptr) return false;
  if (tsNs <= 0) return ready();
  if (cache_ && impl_->sourceRows == 0u && !impl_->pendingValid) {
    (void)cache_->restore(tsNs, *impl_, book_);
  }
  if (impl_->eof ||
      (impl_->pendingValid && impl_->pending.scheduleTsNs > tsNs))
    return ready();
  const auto readStart = std::chrono::steady_clock::now();
  auto finishRead = [&]() noexcept {
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - readStart).count();
    if (elapsed > 0) readNs_ += static_cast<std::uint64_t>(elapsed);
  };
  while (!impl_->eof) {
    if (impl_->pendingValid) {
      if (impl_->pending.scheduleTsNs > tsNs) break;
      const std::int64_t appliedTsNs = impl_->pending.scheduleTsNs;
      if (!applyDepthRow(book_,
                         symbol_,
                         config_,
                         impl_->pending,
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
    if (impl_->scratch.scheduleTsNs > tsNs) {
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
    if (!applyDepthRow(book_,
                       symbol_,
                       config_,
                       impl_->scratch,
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
    if (cache_)
      cache_->publish(*impl_, impl_->scratch.scheduleTsNs, book_);
  }
  finishRead();
  return ready();
}

}  // namespace hft_backtest::detail
