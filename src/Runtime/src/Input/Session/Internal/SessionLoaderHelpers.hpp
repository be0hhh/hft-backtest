#pragma once

#include "../SessionLoader.hpp"

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

namespace hft_backtest::detail::session_loader {


struct TimelineRowKey {
  std::int64_t tsNs{0};
  std::int64_t ingestSeq{0};
};

bool readText(const std::filesystem::path& path, std::string& out);

char lowerAscii(char c) noexcept;

bool equalsNoCase(std::string_view lhs, std::string_view rhs) noexcept;

bool cryptoRecorderExchange(std::string_view exchange) noexcept;

bool validLocalCryptoSymbol(std::string_view symbol) noexcept;

bool legacyCryptoManifestSymbol(std::string_view exchange, std::string_view symbol) noexcept;

void skipWs(std::string_view text, std::size_t& pos) noexcept;

bool parseI64(std::string_view text, std::size_t& pos, std::int64_t& out) noexcept;

bool parseContractTradeLine(std::string_view line, TradeRow& out) noexcept;

bool parseContractBookTickerLine(std::string_view line, BookTickerRow& out) noexcept;

bool parseContractCandleLine(std::string_view line, CandleRow& out) noexcept;

bool readLine(std::string_view text, std::size_t& pos, std::string_view& line) noexcept;

void appendSessionWarning(std::vector<std::string>& warnings, std::string warning);

bool rowKeyLess(TimelineRowKey lhs, TimelineRowKey rhs) noexcept;

TimelineRowKey rowKey(const TradeRow& row) noexcept;

TimelineRowKey rowKey(const BookTickerRow& row) noexcept;

TimelineRowKey rowKey(const DepthRow& row) noexcept;

TimelineRowKey rowKey(const CandleRow& row) noexcept;

std::size_t estimatedRowsFor(std::size_t bytes, std::string_view channel) noexcept;

Status loadDepthTapeSidecarJsonl(const std::filesystem::path& tapePath,
                                 const std::filesystem::path& sidecarPath,
                                 bool required,
                                 std::vector<DepthRow>& out,
                                 std::string& error,
                                 std::vector<std::string>& warnings,
                                 bool* sortedOut = nullptr);

std::string extractStringAfter(std::string_view text, std::string_view key);

std::string_view extractObjectAfter(std::string_view text, std::string_view key) noexcept;

std::string extractChannelPath(std::string_view manifest, std::string_view channel);

std::string extractChannelSidecarPath(std::string_view manifest, std::string_view channel);

std::string extractArtifactPath(std::string_view manifest, std::string_view keyName);

void appendManifestCaptureWarnings(std::string_view manifest, std::vector<std::string>& warnings);

bool cleanRequiredRuntimeChannel(const hftrec::capture::ChannelRuntimeHealth& health) noexcept;

bool addArrivalRowCount(std::uint64_t value, std::uint64_t& total) noexcept;

bool arrivalSummaryMatchesDeclaredRows(
    const hftrec::capture::SessionManifest& manifest) noexcept;

bool validateManifestChannel(std::string_view name,
                             bool load,
                             bool required,
                             bool enabled,
                             std::uint64_t count,
                             std::string_view path,
                             const hftrec::capture::ChannelRuntimeHealth* runtime,
                             std::vector<std::string>& warnings,
                             std::string& error);

bool validateCurrentRecorderManifest(const hftrec::capture::SessionManifest& manifest,
                                     const SessionLoadPlan& plan,
                                     std::vector<std::string>& warnings,
                                     std::string& error);

bool validateLoadedRowCount(std::string_view channel,
                            std::size_t actual,
                            std::uint64_t declared,
                            std::string& error);

std::filesystem::path declaredPath(const std::filesystem::path& root, std::string_view path);

void updateRange(SessionData& data, std::int64_t ts) noexcept;

bool extractPositiveI64Field(std::string_view text, std::string_view key, std::int64_t& out) noexcept;

void loadInstrumentMetadata(const std::filesystem::path& path, InstrumentMetadata& out) noexcept;

void loadTradeHistoryManifestFields(std::string_view manifest, SessionData& out) noexcept;

void splitTradeWarmup(SessionData& out);

bool validateHistoricalSeeds(const SessionData& out, std::string& error);

bool validateCapturedReplayRows(const SessionData& out, std::string& error);

void rebuildReplayRange(SessionData& out) noexcept;

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

constexpr std::size_t kMaxSessionWarnings = 64u;




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

}  // namespace hft_backtest::detail::session_loader
