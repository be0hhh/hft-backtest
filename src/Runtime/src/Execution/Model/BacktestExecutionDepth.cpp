#include "BacktestExecutionDepth.hpp"

#include "../../Replay/BacktestReplayClock.hpp"
#include "hftrec/CorpusContract/JsonLineParser.hpp"
#include "hftrec/CorpusContract/SessionManifest.hpp"

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
  ReplayDepthBook depthBook{};
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
  trading_core::LocalOrderBookState book{};
  std::vector<cxet::composite::BookLevelPxQty> coreBids{};
  std::vector<cxet::composite::BookLevelPxQty> coreAsks{};
  std::uint64_t coreLastUpdateId{0u};
  cxet::market::DepthSourceState coreState{cxet::market::DepthSourceState::Empty};
  cxet::runtime::market::DepthBookProjection coreProjection{};
  hftrec::replay::EventArrival coreArrival{};
  bool coreSourcePosition{true};
  bool coreFirstReplay{false};
};

}  // namespace

class ExecutionDepthCheckpointCache {
 public:
  // Bound existing cold cache growth by the admitted corpus row count.
  bool prepare(std::uint64_t sourceRows) noexcept {
    try {
      std::lock_guard lock{mutex_};
      if (prepared_) return admittedSourceRows_ == sourceRows;
      const auto maximum = sourceRows / kDepthCheckpointRows;
      if (maximum > checkpoints_.max_size()) return false;
      maximumCheckpoints_ = static_cast<std::size_t>(maximum);
      checkpoints_.reserve(maximumCheckpoints_);
      admittedSourceRows_ = sourceRows;
      prepared_ = true;
      return true;
    } catch (...) { return false; }
  }

  bool restore(std::int64_t tsNs,
               ExecutionDepthProviderImpl& impl,
               trading_core::LocalOrderBookState& book) const noexcept {
    std::lock_guard lock{mutex_};
    const auto it = std::upper_bound(
        checkpoints_.begin(), checkpoints_.end(), tsNs,
        [](std::int64_t value, const ExecutionDepthCheckpoint& checkpoint) noexcept {
          return value < checkpoint.tsNs;
        });
    if (it == checkpoints_.begin()) return false;
    const ExecutionDepthCheckpoint& checkpoint = *(it - 1);
    if (!book.hasStorage() ||
        book.capacityLevelsPerSide().raw != checkpoint.book.capacityLevelsPerSide().raw ||
        checkpoint.book.bids.storedCount.raw > book.capacityLevelsPerSide().raw ||
        checkpoint.book.asks.storedCount.raw > book.capacityLevelsPerSide().raw) return false;
    const cxet::runtime::market::LocalOrderBookCheckpointView coreCheckpoint{
        checkpoint.coreBids, checkpoint.coreAsks, checkpoint.coreLastUpdateId,
        checkpoint.coreState, checkpoint.coreProjection, checkpoint.coreFirstReplay};
    if (!impl.depthBook.restore(coreCheckpoint, checkpoint.coreArrival,
            checkpoint.coreSourcePosition) ||
        !book.copyFromBounded(checkpoint.book)) return false;
    impl.tapePos = checkpoint.tapePos;
    impl.sidecarPos = checkpoint.sidecarPos;
    impl.sourceRows = checkpoint.sourceRows;
    impl.lastSourceKey = checkpoint.lastSourceKey;
    impl.lastSourceRowValid = checkpoint.lastSourceRowValid;
    impl.pendingValid = false;
    impl.eof = false;
    return true;
  }

  void publish(const ExecutionDepthProviderImpl& impl,
               std::int64_t tsNs,
               const trading_core::LocalOrderBookState& book) noexcept {
    if (impl.sourceRows == 0u || (impl.sourceRows % kDepthCheckpointRows) != 0u) return;
    try {
      std::lock_guard lock{mutex_};
      if (!prepared_ || checkpoints_.size() >= maximumCheckpoints_) return;
      if (!checkpoints_.empty() && checkpoints_.back().sourceRows >= impl.sourceRows) return;
      if (!checkpoints_.empty() && checkpoints_.back().tsNs > tsNs) return;
      const auto* owner = impl.depthBook.book();
      if (!owner) return;
      ExecutionDepthCheckpoint checkpoint{};
      checkpoint.tsNs = tsNs;
      checkpoint.tapePos = impl.tapePos;
      checkpoint.sidecarPos = impl.sidecarPos;
      checkpoint.sourceRows = impl.sourceRows;
      checkpoint.lastSourceKey = impl.lastSourceKey;
      checkpoint.lastSourceRowValid = impl.lastSourceRowValid;
      checkpoint.book = book;
      cxet::runtime::market::LocalOrderBookCheckpointView coreCheckpoint{};
      if (!owner->checkpoint(&coreCheckpoint)) return;
      checkpoint.coreBids.assign(coreCheckpoint.bids.begin(), coreCheckpoint.bids.end());
      checkpoint.coreAsks.assign(coreCheckpoint.asks.begin(), coreCheckpoint.asks.end());
      checkpoint.coreLastUpdateId = coreCheckpoint.lastUpdateId;
      checkpoint.coreState = coreCheckpoint.state;
      checkpoint.coreProjection = coreCheckpoint.projection;
      checkpoint.coreFirstReplay = coreCheckpoint.firstReplay;
      checkpoint.coreArrival = impl.depthBook.arrival();
      checkpoint.coreSourcePosition = impl.depthBook.sourcePosition();
      checkpoints_.push_back(std::move(checkpoint));
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
  std::uint64_t admittedSourceRows_{0u};
  std::size_t maximumCheckpoints_{0u};
  bool prepared_{false};
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
    const trading_core::LocalOrderBookConfig& config,
    const trading_core::LocalOrderBookState& initialBook,
    std::shared_ptr<ExecutionDepthCheckpointCache> cache) noexcept {
  delete impl_;
  impl_ = nullptr;
  CountVal bookCapacity =
      trading_core::boundedMaxStoredLevels(
          config.maxStoredLevelsPerSide);
  if (config.maxStoredLevelsPerSide.raw == 0u &&
      initialBook.capacityLevelsPerSide().raw != 0u) {
    bookCapacity = initialBook.capacityLevelsPerSide();
  }
  if (!book_.reserveLevelsPerSide(bookCapacity) ||
      (initialBook.initialized.raw ==
           trading_core::kAlgoFlagTrue.raw &&
       !book_.copyFromBounded(initialBook))) {
    available_ = false;
    failed_ = true;
    failureReason_ = "allocation_failed";
    return ExecutionDepthOpenStatus::Failed;
  }
  symbol_ = symbol;
  config_ = config;
  rowsRead_ = 0;
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

  if ((cache_ && !cache_->prepare(manifest.depthCount)) ||
      !next->depthBook.prepare()) {
    failed_ = true;
    failureReason_ = "allocation_failed";
    return ExecutionDepthOpenStatus::Failed;
  }
  if (initialBook.initialized.raw == trading_core::kAlgoFlagTrue.raw) {
    std::vector<cxet::composite::BookLevelPxQty> initialBids;
    std::vector<cxet::composite::BookLevelPxQty> initialAsks;
    initialBids.reserve(initialBook.bids.storedCount.raw);
    initialAsks.reserve(initialBook.asks.storedCount.raw);
    for (std::uint32_t i = 0u; i < initialBook.bids.storedCount.raw; ++i) {
      const auto& level = initialBook.bids.levels[i];
      initialBids.push_back({level.px, level.qty});
    }
    for (std::uint32_t i = 0u; i < initialBook.asks.storedCount.raw; ++i) {
      const auto& level = initialBook.asks.levels[i];
      initialAsks.push_back({level.px, level.qty});
    }
    cxet::runtime::market::DepthBookProjection projection{};
    projection.exchangeTimestampNs = initialBook.ts.raw;
    projection.sequenceFlags = cxet::market::DepthEventSourcePosition;
    const auto state = initialBook.coverage ==
        cxet::api::market::PublicMarketOrderBookCoverage::SnapshotAligned
            ? cxet::market::DepthSourceState::Healthy
            : cxet::market::DepthSourceState::Partial;
    const cxet::runtime::market::LocalOrderBookCheckpointView initialCheckpoint{
        initialBids, initialAsks, 1u, state, projection, false};
    if (!next->depthBook.restore(initialCheckpoint, {}, true)) {
      failed_ = true;
      failureReason_ = "initial_depth_rejected";
      return ExecutionDepthOpenStatus::Failed;
    }
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
  return available_ && trading_core::orderBook(book_).initialized();
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

bool applyDepthRow(trading_core::LocalOrderBookState& book,
                   const Symbol& symbol,
                   const trading_core::LocalOrderBookConfig& config,
                   const CursorDepthRow& row,
                   ReplayDepthBook& depthBook) noexcept {
  if (!depthBook.apply({row.levels.data(), row.levels.size()}, row.exchangeTsNs,
                       row.arrival, false)) return false;
  book.config = config;
  if (book.config.maxStoredLevelsPerSide.raw == 0u)
    book.config.maxStoredLevelsPerSide = book.capacityLevelsPerSide();
  const auto source = depthBook.view();
  (void)trading_core::refreshAuthoritativeOrderBook(book, source, symbol);
  return source.ready;
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
  while (!impl_->eof) {
    if (impl_->pendingValid) {
      if (impl_->pending.scheduleTsNs > tsNs) break;
      const std::int64_t appliedTsNs = impl_->pending.scheduleTsNs;
      if (!applyDepthRow(book_,
                         symbol_,
                         config_,
                         impl_->pending,
                         impl_->depthBook)) {
        available_ = false;
        failed_ = true;
        failureReason_ = "apply_failed";
        if (cache_) cache_->markFailed(failureReason_);
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
        return false;
      }
      impl_->pendingValid = true;
      break;
    }
    if (!applyDepthRow(book_,
                       symbol_,
                       config_,
                       impl_->scratch,
                       impl_->depthBook)) {
      available_ = false;
      failed_ = true;
      failureReason_ = "apply_failed";
      if (cache_) cache_->markFailed(failureReason_);
      return false;
    }
    ++rowsRead_;
    ++impl_->sourceRows;
    if (cache_)
      cache_->publish(*impl_, impl_->scratch.scheduleTsNs, book_);
  }
  return ready();
}

}  // namespace hft_backtest::detail
