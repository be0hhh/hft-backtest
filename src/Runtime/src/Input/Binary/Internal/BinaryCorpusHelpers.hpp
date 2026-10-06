#pragma once

#include "../BinaryCorpusLoader.hpp"

#include "hftrec/CorpusContract/BinaryMarketCorpusReader.hpp"
#include "cxet/Primitives/Buf/CanonConstants.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

namespace hft_backtest::detail::binary_corpus {

namespace corpus = hftrec::corpus;

inline constexpr std::uint8_t kCanonicalScaleDigits = 8u;

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

[[nodiscard]] bool requiredRowsPresent(const SessionLoadPlan& plan,
                                       const SessionData& output,
                                       std::string& error);

void appendEvidenceWarnings(SessionData& output);

}  // namespace hft_backtest::detail::binary_corpus
