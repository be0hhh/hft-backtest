#include "Internal/BinaryCorpusHelpers.hpp"

namespace hft_backtest::detail::binary_corpus {

[[nodiscard]] bool sameDepthFrame(
    const corpus::BinaryMarketRecordHeader& first,
    const corpus::BinaryMarketRecordHeader& current,
    std::uint16_t partIndex) noexcept {
  constexpr std::uint32_t anomalyFlags =
      corpus::BinaryMarketRecordRealtimeRegression |
      corpus::BinaryMarketRecordMonotonicNonIncreasing |
      corpus::BinaryMarketRecordExchangeAheadOfReceive |
      corpus::BinaryMarketRecordDepthTransactionEnd;
  return current.sourceId == first.sourceId &&
      current.sourceGeneration == first.sourceGeneration &&
      current.sessionEpoch == first.sessionEpoch &&
      current.eventSequence == first.eventSequence &&
      current.frameSequence == first.frameSequence &&
      current.nativeIdentityFirst == first.nativeIdentityFirst &&
      current.nativeIdentitySecond == first.nativeIdentitySecond &&
      current.exchangeTimestampNs == first.exchangeTimestampNs &&
      current.receiveRealtimeNs == first.receiveRealtimeNs &&
      current.receiveMonotonicNs == first.receiveMonotonicNs &&
      current.shardIndex == first.shardIndex &&
      current.sourceFlags == first.sourceFlags &&
      current.channel == corpus::BinaryMarketChannel::Depth &&
      current.nativeIdentityShape == first.nativeIdentityShape &&
      current.eventOrdinal == partIndex &&
      first.shardSequence <=
          std::numeric_limits<std::uint64_t>::max() - partIndex &&
      current.shardSequence == first.shardSequence + partIndex &&
      (current.flags & ~anomalyFlags) == (first.flags & ~anomalyFlags);
}

[[nodiscard]] bool advanceDepthRebase(
    const corpus::BinaryMarketRecordHeader& header,
    const corpus::BinaryMarketDepthChunkPayload& payload,
    DepthRebaseCursor& cursor) noexcept {
  if (payload.transactionPartCount == 0u ||
      payload.transactionPartIndex >= payload.transactionPartCount) {
    return false;
  }
  const bool rebase = payload.state ==
      corpus::BinaryMarketDepthState::RebaseInFlight;
  if (!rebase) {
    if (cursor.active || payload.transactionPartIndex != 0u ||
        payload.transactionPartCount != 1u) {
      return false;
    }
    cursor = {};
    return true;
  }
  const bool same = cursor.active &&
      header.sourceId == cursor.sourceId &&
      header.sourceGeneration == cursor.sourceGeneration &&
      header.sessionEpoch == cursor.sessionEpoch &&
      header.frameSequence == cursor.frameSequence &&
      header.receiveRealtimeNs == cursor.receiveRealtimeNs &&
      header.receiveMonotonicNs == cursor.receiveMonotonicNs &&
      header.shardIndex == cursor.shardIndex;
  const bool snapshot = payload.frameKind ==
      corpus::BinaryMarketDepthFrameKind::Snapshot;
  if (!cursor.active) {
    if (!snapshot || payload.transactionPartIndex != 0u) return false;
    cursor = {.sourceGeneration = header.sourceGeneration,
              .sessionEpoch = header.sessionEpoch,
              .frameSequence = header.frameSequence,
              .receiveMonotonicNs = header.receiveMonotonicNs,
              .publicationSequence = header.eventSequence,
              .lastUpdateId = payload.sequence.lastUpdateId,
              .receiveRealtimeNs = header.receiveRealtimeNs,
              .sourceId = header.sourceId,
              .shardIndex = header.shardIndex,
              .transactionPartCount = payload.transactionPartCount,
              .nextTransactionPartIndex = 1u,
              .active = true};
  } else {
    if (!same || snapshot ||
        payload.transactionPartCount != cursor.transactionPartCount ||
        payload.transactionPartIndex !=
            cursor.nextTransactionPartIndex ||
        cursor.publicationSequence ==
                        std::numeric_limits<std::uint64_t>::max() ||
        header.eventSequence != cursor.publicationSequence + 1u ||
        header.shardSequence != cursor.nextShardSequence ||
        cursor.lastUpdateId == std::numeric_limits<std::uint64_t>::max() ||
        (cursor.replayDeltaCount != 0u &&
         payload.sequence.previousUpdateId != cursor.lastUpdateId) ||
        payload.sequence.firstUpdateId > cursor.lastUpdateId + 1u ||
        payload.sequence.lastUpdateId < cursor.lastUpdateId + 1u) {
      return false;
    }
    cursor.publicationSequence = header.eventSequence;
    cursor.lastUpdateId = payload.sequence.lastUpdateId;
    if (cursor.replayDeltaCount ==
        std::numeric_limits<std::uint32_t>::max()) {
      return false;
    }
    ++cursor.replayDeltaCount;
    ++cursor.nextTransactionPartIndex;
  }
  if (header.shardSequence >
      std::numeric_limits<std::uint64_t>::max() - payload.partCount) {
    return false;
  }
  cursor.nextShardSequence = header.shardSequence + payload.partCount;
  if (cursor.nextTransactionPartIndex == cursor.transactionPartCount) {
    cursor.active = false;
  }
  return true;
}

[[nodiscard]] bool appendDepth(
    const corpus::BinaryMarketSelection& selection,
    std::size_t& recordIndex,
    std::int64_t ingestSequence,
    SessionData& output,
    std::string& error) {
  const auto& firstRecord = selection.records[recordIndex];
  const auto* first = checkedPayload<corpus::BinaryMarketDepthChunkPayload>(
      firstRecord);
  const bool composed = corpus::binaryMarketDepthRecordIsComposedCut(firstRecord);
  if (!first || first->partIndex != 0u || first->partCount == 0u ||
      (first->totalLevelCount == 0u &&
       first->frameKind != corpus::BinaryMarketDepthFrameKind::Snapshot) ||
      first->totalLevelCount > rawdata::kMaxOrderbookLevels ||
      first->partCount !=
          std::max<std::size_t>(1u, (first->totalLevelCount +
           corpus::kBinaryMarketDepthLevelsPerRecord - 1u) /
              corpus::kBinaryMarketDepthLevelsPerRecord) ||
      (first->frameKind !=
           corpus::BinaryMarketDepthFrameKind::Snapshot &&
       first->frameKind != corpus::BinaryMarketDepthFrameKind::Delta) ||
      (!composed && first->state != corpus::BinaryMarketDepthState::Partial &&
       first->state != corpus::BinaryMarketDepthState::RebaseInFlight &&
       first->state != corpus::BinaryMarketDepthState::Healthy) ||
      (!composed && first->failure != 0u) ||
      (!composed && first->sourceReceiveRealtimeNs == 0u) ||
      first->sourceReceiveRealtimeNs > static_cast<std::uint64_t>(
          std::numeric_limits<std::int64_t>::max()) ||
      (!composed && first->sourceFreshnessMonotonicNs == 0u) ||
      first->transactionPartCount == 0u ||
      first->transactionPartIndex >= first->transactionPartCount ||
      first->reserved8 != 0u || first->reserved16 != 0u) {
    error = "malformed binary corpus depth frame";
    return false;
  }
  const bool snapshot = first->frameKind ==
      corpus::BinaryMarketDepthFrameKind::Snapshot;
  const std::uint32_t kindFlag = snapshot
      ? corpus::BinaryMarketRecordDepthSnapshot
      : corpus::BinaryMarketRecordDepthDelta;
  const std::uint32_t otherKindFlag = snapshot
      ? corpus::BinaryMarketRecordDepthDelta
      : corpus::BinaryMarketRecordDepthSnapshot;
  const bool partial =
      first->state == corpus::BinaryMarketDepthState::Partial;
  const bool rebaseInFlight =
      first->state == corpus::BinaryMarketDepthState::RebaseInFlight;
  const bool partialFlag =
      (firstRecord.header.flags &
       corpus::BinaryMarketRecordDepthPartial) != 0u;
  const bool rebaseFlag =
      (firstRecord.header.flags &
       corpus::BinaryMarketRecordDepthRebase) != 0u;
  if ((firstRecord.header.flags & kindFlag) == 0u ||
      (firstRecord.header.flags & otherKindFlag) != 0u ||
      (firstRecord.header.flags &
       corpus::BinaryMarketRecordDepthState) != 0u ||
      partialFlag != partial || (!composed && snapshot && partial) ||
      rebaseFlag != rebaseInFlight ||
      (!composed && snapshot && !rebaseInFlight) ||
      (rebaseInFlight &&
       (snapshot != (first->transactionPartIndex == 0u))) ||
      (!rebaseInFlight &&
       (first->transactionPartIndex != 0u ||
        first->transactionPartCount != 1u)) ||
      (!snapshot && !rebaseInFlight &&
       first->state != corpus::BinaryMarketDepthState::Partial &&
       first->state != corpus::BinaryMarketDepthState::Healthy) ||
      (snapshot
           ? first->sequence.firstUpdateId == 0u ||
                 first->sequence.firstUpdateId !=
                     first->sequence.lastUpdateId ||
                 first->sequence.previousUpdateId != 0u
           : first->sequence.firstUpdateId == 0u ||
                 first->sequence.lastUpdateId <
                     first->sequence.firstUpdateId)) {
    error = "binary corpus depth sequence contract mismatch";
    return false;
  }

  DepthRow row{};
  row.tsNs = firstRecord.header.exchangeTimestampNs;
  row.ingestSeq = ingestSequence;
  row.arrival = makeArrival(selection.manifest, firstRecord.header);
  row.resetBookBeforeApply = snapshot;
  row.capturedPublicationSequence = firstRecord.header.eventSequence;
  row.capturedSequence = {first->sequence.firstUpdateId,
      first->sequence.lastUpdateId, first->sequence.previousUpdateId};
  row.nativeEvidence = first->nativeEvidence;
  row.sourceReceiveRealtimeNs = first->sourceReceiveRealtimeNs;
  row.sourceFreshnessMonotonicNs = first->sourceFreshnessMonotonicNs;
  row.capturedSourcePosition = (firstRecord.header.flags &
      corpus::BinaryMarketRecordDepthSourcePosition) != 0u;
  row.capturedComposedCut = composed;
  row.capturedState = static_cast<cxet::market::DepthSourceState>(first->state);
  row.capturedFailure = static_cast<cxet::market::DepthFailure>(first->failure);
  row.applicationAtomicRebase = rebaseInFlight;
  row.levels.reserve(first->totalLevelCount);

  bool asks = false;
  std::uint64_t previousPrice = 0u;
  for (std::uint16_t partIndex = 0u; partIndex < first->partCount;
       ++partIndex) {
    const std::size_t index = recordIndex + partIndex;
    if (index >= selection.records.size()) {
      error = "truncated binary corpus depth frame";
      return false;
    }
    const auto& record = selection.records[index];
    const auto* part = checkedPayload<corpus::BinaryMarketDepthChunkPayload>(
        record);
    const std::uint32_t offset =
        static_cast<std::uint32_t>(partIndex) *
        corpus::kBinaryMarketDepthLevelsPerRecord;
    const std::uint16_t expectedLevels = static_cast<std::uint16_t>(
        std::min<std::uint32_t>(
            corpus::kBinaryMarketDepthLevelsPerRecord,
            first->totalLevelCount - offset));
    if (!part || !corpus::validBinaryMarketRecord(record) || !channelFlagsValid(record.header) ||
        !sameDepthFrame(firstRecord.header, record.header, partIndex) ||
        part->totalLevelCount != first->totalLevelCount ||
        part->levelOffset != offset || part->partIndex != partIndex ||
        part->partCount != first->partCount ||
        part->levelCount != expectedLevels ||
        part->frameKind != first->frameKind ||
        part->state != first->state || part->failure != first->failure ||
        part->sequence.firstUpdateId != first->sequence.firstUpdateId ||
        part->sequence.lastUpdateId != first->sequence.lastUpdateId ||
        part->sequence.previousUpdateId !=
            first->sequence.previousUpdateId ||
        part->transactionPartIndex != first->transactionPartIndex ||
        part->transactionPartCount != first->transactionPartCount ||
        part->sourceReceiveRealtimeNs !=
            first->sourceReceiveRealtimeNs ||
        part->sourceFreshnessMonotonicNs !=
            first->sourceFreshnessMonotonicNs ||
        part->nativeEvidence != first->nativeEvidence ||
        !cxet::api::market::validMarketNativeEvidence(part->nativeEvidence) ||
        (((record.header.flags & corpus::BinaryMarketRecordDepthTransactionEnd) != 0u) !=
            ((record.header.flags & corpus::BinaryMarketRecordDepthHasCoverage) == 0u &&
             first->transactionPartIndex + 1u == first->transactionPartCount &&
             partIndex + 1u == first->partCount)) ||
        part->reserved8 != 0u || part->reserved16 != 0u) {
      error = "binary corpus depth chunk contract mismatch";
      return false;
    }
    for (std::uint16_t levelIndex = 0u;
         levelIndex < part->levelCount; ++levelIndex) {
      const auto& level = part->levels[levelIndex];
      if ((level.side != 1u && level.side != 2u) ||
          (level.action != 1u && level.action != 2u) ||
          level.priceRaw == 0u ||
          (level.action == 1u && level.quantityRaw == 0u) ||
          (level.action == 2u && level.quantityRaw != 0u) ||
          (snapshot && level.action != 1u) || level.reserved16 != 0u ||
          level.reserved32 != 0u) {
        error = "invalid binary corpus depth level";
        return false;
      }
      if (level.side == 2u) {
        if (!asks) {
          asks = true;
          previousPrice = 0u;
        }
        if (previousPrice != 0u && level.priceRaw <= previousPrice) {
          error = "binary corpus asks are not canonical";
          return false;
        }
      } else {
        if (asks ||
            (previousPrice != 0u && level.priceRaw >= previousPrice)) {
          error = "binary corpus bids are not canonical";
          return false;
        }
      }
      previousPrice = level.priceRaw;
      PriceLevelRow converted{};
      if (!rescaleToE8(level.priceRaw, selection.source.priceScale,
                       converted.priceE8) ||
          !canonicalMarketQuantityToE8(selection.source, corpus::BinaryMarketChannel::Depth,
                       level.quantityRaw, converted.qtyE8)) {
        error = "binary corpus depth value cannot be represented exactly in E8";
        return false;
      }
      converted.side = level.side == 1u ? 0 : 1;
      row.levels.push_back(converted);
    }
  }
  includeEvidence(output, firstRecord.header);
  output.depths.push_back(std::move(row));
  recordIndex += first->partCount;
  return true;
}

[[nodiscard]] bool appendDepthTransaction(
    const corpus::BinaryMarketSelection& selection,
    std::size_t& recordIndex,
    BinaryDecodeState& state,
    SessionData& output,
    std::string& error) {
  const auto start = recordIndex;
  const auto& root = selection.records[start].header;
  const auto* initial = checkedPayload<corpus::BinaryMarketDepthChunkPayload>(selection.records[start]);
  if (!initial || initial->transactionPartIndex != 0u ||
      initial->transactionPartCount == 0u ||
      initial->transactionPartCount > cxet::market::kMaximumDepthTransactionParts ||
      (root.flags & corpus::BinaryMarketRecordDepthCoverage) != 0u) {
    error = "invalid binary corpus depth transaction start";
    return false;
  }
  const auto count = initial->transactionPartCount;
  const bool hasCoverage = (root.flags & corpus::BinaryMarketRecordDepthHasCoverage) != 0u;
  corpus::BinaryMarketDepthTransactionState transaction{};
  std::size_t transactionEnd = start;
  do {
    if (transactionEnd - start >= kMaximumDepthApplicationRecords ||
        transactionEnd >= selection.records.size() ||
        !corpus::advanceBinaryMarketDepthTransaction(selection.records[transactionEnd], transaction)) {
      error = "incomplete binary corpus depth application transaction"; return false;
    }
    ++transactionEnd;
  } while (transaction.active());
  const bool composed = corpus::binaryMarketDepthRecordIsComposedCut(selection.records[start]);
  if (composed && state.composedSourceGeneration == root.sourceGeneration &&
      state.composedSessionEpoch == root.sessionEpoch &&
      root.eventSequence <= state.composedPublicationSequence) {
    error = "binary corpus composed depth publication is stale"; return false;
  }
  // Stage all native parts and their coverage before publishing any replay row.
  SessionData staged{};
  std::array<std::size_t, cxet::market::kMaximumDepthTransactionParts> starts{};
  std::uint64_t totalLevels = 0u;
  std::size_t next = start;
  for (std::uint16_t part = 0u; part < count; ++part) {
    if (next >= selection.records.size()) {
      error = "truncated binary corpus depth transaction"; return false;
    }
    const auto& record = selection.records[next];
    const auto* payload = checkedPayload<corpus::BinaryMarketDepthChunkPayload>(record);
    if (!payload || payload->transactionPartIndex != part || payload->transactionPartCount != count ||
        ((record.header.flags & corpus::BinaryMarketRecordDepthHasCoverage) != 0u) != hasCoverage ||
        !advanceDepthRebase(record.header, *payload, state.depthRebase)) {
      error = "binary corpus depth transaction part mismatch"; return false;
    }
    totalLevels += payload->totalLevelCount;
    if (totalLevels > cxet::market::kMaximumDepthTransactionMutations ||
        (part != 0u && state.ingestSequence == std::numeric_limits<std::int64_t>::max())) {
      error = "binary corpus depth transaction exceeds bounded replay capacity"; return false;
    }
    if (part != 0u) ++state.ingestSequence;
    starts[part] = next;
    if (!appendDepth(selection, next, state.ingestSequence, staged, error)) return false;
  }
  if (state.depthRebase.active) {
    error = "binary corpus depth transaction is incomplete"; return false;
  }
  if (hasCoverage) {
    if (next >= selection.records.size()) {
      error = "missing binary corpus depth coverage tail"; return false;
    }
    const auto& last = selection.records[next - 1u].header;
    const auto* firstCoverage = checkedPayload<corpus::BinaryMarketDepthCoveragePayload>(selection.records[next]);
    if (!firstCoverage || !corpus::validBinaryMarketDepthCoveragePayload(*firstCoverage)) {
      error = "invalid binary corpus depth coverage tail"; return false;
    }
    auto& coverage = staged.depths.back().capturedCoverage;
    coverage.reserve(firstCoverage->totalCoverageCount);
    for (std::uint16_t part = 0u; part < firstCoverage->partCount; ++part) {
      if (next >= selection.records.size()) {
        error = "truncated binary corpus depth coverage tail"; return false;
      }
      const auto& record = selection.records[next];
      const auto& header = record.header;
      const auto* payload = checkedPayload<corpus::BinaryMarketDepthCoveragePayload>(record);
      if (!payload || !corpus::validBinaryMarketRecord(record) ||
          !corpus::validBinaryMarketDepthCoveragePayload(*payload) ||
          header.shardIndex != last.shardIndex || header.eventOrdinal != part ||
          last.shardSequence > std::numeric_limits<std::uint64_t>::max() - part - 1u ||
          header.shardSequence != last.shardSequence + part + 1u) {
        error = "binary corpus depth coverage transaction mismatch"; return false;
      }
      for (std::uint16_t index = 0u; index < payload->coverageCount; ++index) {
        auto feed = corpus::decodeBinaryMarketDepthCoverage(payload->coverage[index]);
        if (!rescaleToE8(feed.lowestBidPriceRaw, selection.source.priceScale, feed.lowestBidPriceRaw) ||
            !rescaleToE8(feed.highestAskPriceRaw, selection.source.priceScale, feed.highestAskPriceRaw) ||
            !rescaleToE8(feed.lowestProtectedBidPriceRaw, selection.source.priceScale, feed.lowestProtectedBidPriceRaw) ||
            !rescaleToE8(feed.highestProtectedAskPriceRaw, selection.source.priceScale, feed.highestProtectedAskPriceRaw)) {
          error = "binary corpus depth coverage price cannot be represented exactly in E8"; return false;
        }
        coverage.push_back(feed);
      }
      ++next;
    }
  }
  if (next != transactionEnd) {
    error = "binary corpus depth transaction boundary mismatch"; return false;
  }
  if (composed) {
    state.composedPublicationSequence = root.eventSequence;
    state.composedSourceGeneration = root.sourceGeneration;
    state.composedSessionEpoch = root.sessionEpoch;
  }
  output.maximumCapturedDepthCoverageSources = std::max(output.maximumCapturedDepthCoverageSources,
      static_cast<std::uint32_t>(staged.depths.back().capturedCoverage.size()));
  output.depths.reserve(output.depths.size() + count);
  for (std::uint16_t part = 0u; part < count; ++part) {
    includeEvidence(output, selection.records[starts[part]].header);
    output.depths.push_back(std::move(staged.depths[part]));
  }
  recordIndex = next;
  return true;
}

}  // namespace hft_backtest::detail::binary_corpus
