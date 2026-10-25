#pragma once

#include "../BinaryCorpusLoader.hpp"

#include "hftrec/CorpusContract/BinaryMarketCorpusReader.hpp"
#include "hftrec/CorpusContract/BinaryMarketDepthTransaction.hpp"
#include "cxet/Primitives/Buf/CanonConstants.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace hft_backtest::detail::binary_corpus {

namespace corpus = hftrec::corpus;

inline constexpr std::uint8_t kCanonicalScaleDigits = 8u;
inline constexpr std::size_t kMaximumDepthApplicationRecords =
    cxet::market::kMaximumDepthTransactionMutations / corpus::kBinaryMarketDepthLevelsPerRecord +
    cxet::market::kMaximumDepthTransactionParts +
    (cxet::market::kMaximumDepthSourceCapacity + corpus::kBinaryMarketDepthCoveragePerRecord - 1u) /
        corpus::kBinaryMarketDepthCoveragePerRecord;

[[nodiscard]] inline bool completeDepthApplication(
    std::span<const corpus::BinaryMarketRecord> records) noexcept {
  if (records.empty() || records.size() > kMaximumDepthApplicationRecords) return false;
  corpus::BinaryMarketDepthTransactionState state{};
  for (std::size_t index = 0u; index < records.size(); ++index) {
    if (records[index].header.channel != corpus::BinaryMarketChannel::Depth ||
        !corpus::advanceBinaryMarketDepthTransaction(records[index], state) ||
        (!state.active() && index + 1u != records.size())) return false;
  }
  return !state.active();
}

template <typename Payload>
[[nodiscard]] const Payload* checkedPayload(
    const corpus::BinaryMarketRecord& record) noexcept {
  if (record.header.payloadBytes != sizeof(Payload)) return nullptr;
  const auto* payload = corpus::binaryMarketPayload<Payload>(&record);
  const auto firstTail = record.payload.begin() + sizeof(Payload);
  if (!std::all_of(firstTail, record.payload.end(),
                   [](std::byte value) noexcept {
                     return value == std::byte{};
                   })) {
    return nullptr;
  }
  return payload;
}

[[nodiscard]] inline bool includeDepthCoverageMaximum(const corpus::BinaryMarketRecord& record,
    std::span<std::uint32_t> maximumBySource) noexcept {
  if (record.header.channel != corpus::BinaryMarketChannel::Depth ||
      (record.header.flags & corpus::BinaryMarketRecordDepthCoverage) == 0u) return true;
  const auto* payload = checkedPayload<corpus::BinaryMarketDepthCoveragePayload>(record);
  if (!record.header.sourceId || record.header.sourceId > maximumBySource.size() ||
      !payload || !corpus::validBinaryMarketDepthCoveragePayload(*payload)) return false;
  auto& maximum = maximumBySource[record.header.sourceId - 1u];
  maximum = std::max(maximum, payload->totalCoverageCount);
  return true;
}

[[nodiscard]] inline bool depthRecordCanRecoverSource(const corpus::BinaryMarketRecord& record) noexcept {
  if (corpus::binaryMarketDepthRecordIsComposedCut(record)) {
    const auto state = corpus::binaryMarketPayload<corpus::BinaryMarketDepthChunkPayload>(&record)->state;
    return state == corpus::BinaryMarketDepthState::Healthy || state == corpus::BinaryMarketDepthState::Partial;
  }
  return corpus::binaryMarketDepthRecordIsNativeRebaseSnapshot(record);
}

struct DepthRebaseCursor final {
  std::uint64_t sourceGeneration{0u};
  std::uint64_t sessionEpoch{0u};
  std::uint64_t frameSequence{0u};
  std::uint64_t receiveMonotonicNs{0u};
  std::uint64_t nextShardSequence{0u};
  std::uint64_t publicationSequence{0u};
  std::uint64_t lastUpdateId{0u};
  std::int64_t receiveRealtimeNs{0};
  std::uint32_t sourceId{0u};
  std::uint32_t replayDeltaCount{0u};
  std::uint16_t shardIndex{0u};
  std::uint16_t transactionPartCount{0u};
  std::uint16_t nextTransactionPartIndex{0u};
  bool active{false};
};

struct BinaryDecodeState final {
  std::int64_t ingestSequence{};
  std::uint64_t retainedBboSequence{};
  std::uint64_t retainedBboSessionEpoch{};
  corpus::BinaryMarketBookTickerPayload retainedBboPayload{};
  std::uint16_t retainedBboPresenceFlags{};
  DepthRebaseCursor depthRebase{};
  std::uint64_t composedPublicationSequence{};
  std::uint64_t composedSourceGeneration{};
  std::uint64_t composedSessionEpoch{};
};
[[nodiscard]] Status decodeBinaryMarketRecords(const corpus::BinaryMarketSelection& selection,
    BinaryDecodeState& state, SessionData& output, std::string& error);
[[nodiscard]] Status initializeBinaryMarketSession(const corpus::BinaryMarketSelectionRequest& request,
    const corpus::BinaryMarketSelection& selection, const SessionLoadPlan& plan,
    SessionData& output, std::string& error);

[[nodiscard]] Status convertStatus(hftrec::Status status) noexcept;

[[nodiscard]] std::string_view fixedText(const char* data,
                                         std::size_t capacity,
                                         std::uint8_t bytes) noexcept;

void appendWarning(SessionData& output, std::string warning);

[[nodiscard]] const char* channelName(corpus::BinaryMarketChannel channel);

[[nodiscard]] bool rescaleToE8(std::int64_t input,
                               std::uint8_t scaleDigits,
                               std::int64_t& output) noexcept;

[[nodiscard]] bool rescaleToE8(std::uint64_t input,
                               std::uint8_t scaleDigits,
                               std::int64_t& output) noexcept;

[[nodiscard]] bool canonicalMarketQuantityToE8(const corpus::BinaryMarketSource& source,
    corpus::BinaryMarketChannel channel, std::int64_t input, std::int64_t& output) noexcept;
[[nodiscard]] bool canonicalMarketQuantityToE8(const corpus::BinaryMarketSource& source,
    corpus::BinaryMarketChannel channel, std::uint64_t input, std::int64_t& output) noexcept;

[[nodiscard]] const corpus::BinaryMarketBookTickerPayload*
checkedBookTickerPayload(
    const corpus::BinaryMarketRecord& record) noexcept;

[[nodiscard]] std::uint32_t arrivalFlags(
    const corpus::BinaryMarketRecordHeader& header) noexcept;

[[nodiscard]] hftrec::replay::EventArrival makeArrival(
    const corpus::BinaryMarketManifest& manifest,
    const corpus::BinaryMarketRecordHeader& header) noexcept;

void includeEvidence(SessionData& output,
                     const corpus::BinaryMarketRecordHeader& header) noexcept;

[[nodiscard]] bool channelFlagsValid(
    const corpus::BinaryMarketRecordHeader& header) noexcept;

[[nodiscard]] bool sameDepthFrame(
    const corpus::BinaryMarketRecordHeader& first,
    const corpus::BinaryMarketRecordHeader& current,
    std::uint16_t partIndex) noexcept;

[[nodiscard]] bool advanceDepthRebase(
    const corpus::BinaryMarketRecordHeader& header,
    const corpus::BinaryMarketDepthChunkPayload& payload,
    DepthRebaseCursor& cursor) noexcept;

[[nodiscard]] bool appendDepth(
    const corpus::BinaryMarketSelection& selection,
    std::size_t& recordIndex,
    std::int64_t ingestSequence,
    SessionData& output,
    std::string& error);

[[nodiscard]] bool appendDepthTransaction(
    const corpus::BinaryMarketSelection& selection,
    std::size_t& recordIndex,
    BinaryDecodeState& state,
    SessionData& output,
    std::string& error);

[[nodiscard]] bool requiredRowsPresent(const SessionLoadPlan& plan,
                                       const SessionData& output,
                                       std::string& error);

void appendEvidenceWarnings(SessionData& output);

}  // namespace hft_backtest::detail::binary_corpus
