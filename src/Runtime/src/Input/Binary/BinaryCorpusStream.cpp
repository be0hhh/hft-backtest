#include "BinaryCorpusStream.hpp"
#include "Internal/BinaryCorpusHelpers.hpp"

#include <algorithm>
#include <limits>
#include <tuple>

namespace hft_backtest::detail {
namespace corpus = hftrec::corpus;
namespace {
std::uint16_t requestedMask(const SessionLoadPlan& plan, bool required) noexcept {
  std::uint16_t mask = 0u;
  if (required ? plan.requireTrades : plan.loadTrades)
    mask |= corpus::binaryMarketChannelBit(corpus::BinaryMarketChannel::Trade);
  if (required ? plan.requireBookTickers : plan.loadBookTickers)
    mask |= corpus::binaryMarketChannelBit(corpus::BinaryMarketChannel::BookTicker);
  if (required ? plan.requireDepths : plan.loadDepths)
    mask |= corpus::binaryMarketChannelBit(corpus::BinaryMarketChannel::Depth);
  return mask;
}
bool observedUnhealthy(const corpus::BinaryMarketRecord& record) noexcept {
  return corpus::binaryMarketRecordIsSourceHealth(record);
}
void resetDecoderChannel(binary_corpus::BinaryDecodeState& state, corpus::BinaryMarketChannel channel) noexcept {
  if (channel == corpus::BinaryMarketChannel::BookTicker) {
    state.retainedBboSequence = state.retainedBboSessionEpoch = 0u;
    state.retainedBboPayload = {}; state.retainedBboPresenceFlags = 0u;
  } else if (channel == corpus::BinaryMarketChannel::Depth) state.depthRebase = {};
}
void clearRows(SessionData& session) noexcept {
  std::vector<TradeRow>().swap(session.trades);
  std::vector<BookTickerRow>().swap(session.bookTickers);
  std::vector<DepthRow>().swap(session.depths);
  std::vector<SourceLifecycleRow>().swap(session.sourceLifecycle);
  std::vector<SourceHealthRow>().swap(session.sourceHealth);
}
}

struct BinaryCorpusStream::Impl {
  struct Source {
    corpus::BinaryMarketSelection selection;
    binary_corpus::BinaryDecodeState decoder;
    std::uint16_t selectedMask{};
    std::uint16_t requiredMask{};
    std::uint16_t requestedMask{};
    std::uint16_t unhealthyMask{};
  };
  corpus::BinaryMarketCorpusCursor cursor;
  corpus::BinaryMarketRecord head{};
  bool available{};
  std::vector<std::size_t> sourceLegs;
  std::vector<Source> sources;
  std::vector<SessionData> sessions;
  std::vector<corpus::BinaryMarketRecord> records;
  std::size_t previousLeg{std::numeric_limits<std::size_t>::max()};
};
BinaryCorpusStream::BinaryCorpusStream() noexcept = default;
BinaryCorpusStream::~BinaryCorpusStream() = default;
const std::vector<SessionData>& BinaryCorpusStream::sessions() const noexcept {
  static const std::vector<SessionData> empty;
  return impl_ ? impl_->sessions : empty;
}

Status BinaryCorpusStream::open(const std::filesystem::path& path,
    std::span<const BacktestCorpusSourceRequest> requests,
    const SessionLoadPlan& plan, std::string& error, bool allowQuietSources) noexcept {
  impl_.reset();
  if (requests.empty() || requests.size() > cxet::market::kMaximumDepthSourceCapacity ||
      plan.requireCandles) {
    error = plan.requireCandles ? "binary corpus has no required candle history" :
        "invalid selected streaming context leg count";
    return Status::Unsupported;
  }
  try {
    corpus::BinaryMarketManifest manifest;
    std::vector<corpus::BinaryMarketSource> catalog;
    const auto catalogStatus = corpus::BinaryMarketCorpusReader{}.catalog(path, manifest, catalog, error);
    if (!hftrec::isOk(catalogStatus)) return binary_corpus::convertStatus(catalogStatus);
    auto owner = std::make_unique<Impl>();
    owner->sourceLegs.resize(catalog.size(), std::numeric_limits<std::size_t>::max());
    owner->sources.resize(requests.size());
    corpus::BinaryMarketSelectionRequest common;
    common.root = path;
    common.allSources = true;
    common.beginReceiveNs = requests[0].beginReceiveNs;
    common.endReceiveNs = requests[0].endReceiveNs;
    common.channelMask = requestedMask(plan, false);
    common.requireTraderReplayCompatibility = false;
    owner->sessions.resize(requests.size());
    for (std::size_t leg = 0u; leg < requests.size(); ++leg) {
      const auto& request = requests[leg];
      auto& source = owner->sources[leg];
      if (request.beginReceiveNs != common.beginReceiveNs || request.endReceiveNs != common.endReceiveNs) {
        error = "one replay universe requires one common captured interval";
        return Status::InvalidArgument;
      }
      corpus::BinaryMarketSelectionRequest selection{};
      selection.root = path;
      selection.exchange = request.exchange;
      selection.market = request.market;
      selection.canonicalSymbol = request.canonicalSymbol;
      selection.beginReceiveNs = request.beginReceiveNs;
      selection.endReceiveNs = request.endReceiveNs;
      const corpus::BinaryMarketSource* selected = nullptr;
      for (const auto& candidate : catalog) {
        if (binary_corpus::fixedText(candidate.venue.data(), candidate.venue.size(), candidate.venueBytes) != request.exchange ||
            binary_corpus::fixedText(candidate.market.data(), candidate.market.size(), candidate.marketBytes) != request.market ||
            binary_corpus::fixedText(candidate.canonicalSymbol.data(), candidate.canonicalSymbol.size(), candidate.canonicalSymbolBytes) != request.canonicalSymbol) continue;
        if (selected) { error = "binary corpus source selection is ambiguous"; return Status::CorruptData; }
        selected = &candidate;
      }
      if (!selected) { error = "binary corpus source was not found"; return Status::InvalidArgument; }
      selection.requiredChannelMask = requestedMask(plan, true);
      if (allowQuietSources && (selection.requiredChannelMask & ~selected->configuredChannelMask) != 0u) {
        error = "whole-universe source lacks configured required market capability";
        return Status::Unsupported;
      }
      // A dormant catalog source may acquire its actual native lanes later.
      // Required compatibility is proven from preflight interval coverage,
      // rather than inferred from its historical registration metadata.
      selection.channelMask = requestedMask(plan, false) & selected->availableChannelMask & selected->traderReplayChannelMask;
      selection.requireTraderReplayCompatibility = true;
      if (owner->sourceLegs[selected->sourceId - 1u] != std::numeric_limits<std::size_t>::max()) {
        error = "replay universe repeats one source"; return Status::InvalidArgument;
      }
      owner->sourceLegs[selected->sourceId - 1u] = leg;
      common.sourceIds.push_back(selected->sourceId);
      source.selectedMask = selection.channelMask;
      source.requiredMask = selection.requiredChannelMask;
      source.requestedMask = requestedMask(plan, false);
      source.selection.manifest = manifest;
      source.selection.source = *selected;
    }
    corpus::BinaryMarketSelection metadata;
    auto status = owner->cursor.open(common, metadata, error);
    if (!hftrec::isOk(status)) return binary_corpus::convertStatus(status);
    for (const auto sourceId : common.sourceIds) {
      const auto required = requestedMask(plan, true);
      if (!allowQuietSources && (sourceId > metadata.sourceObservedChannelMasks.size() ||
          (required & ~metadata.sourceObservedChannelMasks[sourceId - 1u]) != 0u)) {
        error = "selected binary corpus source interval has no required channel rows";
        return Status::CorruptData;
      }
    }
    for (std::size_t leg = 0u; leg < requests.size(); ++leg) {
      auto& source = owner->sources[leg];
      const auto sourceId = source.selection.source.sourceId;
      if (sourceId > metadata.sourcesAtBegin.size() ||
          sourceId > metadata.sourceLifecycleRevisionsAtBegin.size() ||
          sourceId > metadata.sourceUnhealthyChannelMasksAtBegin.size()) {
        error = "bounded corpus preflight omitted effective source membership";
        return Status::CorruptData;
      }
      source.selection.source = metadata.sourcesAtBegin[sourceId - 1u];
      if ((source.requiredMask & source.selection.source.availableChannelMask &
           ~source.selection.source.traderReplayChannelMask) != 0u) {
        error = "required available source channel is recorded-only and not trader-replay compatible";
        return Status::Unsupported;
      }
      source.selectedMask = source.requestedMask & source.selection.source.availableChannelMask &
          source.selection.source.traderReplayChannelMask;
      corpus::BinaryMarketSelectionRequest selected;
      selected.root = path; selected.exchange = requests[leg].exchange;
      selected.market = requests[leg].market; selected.canonicalSymbol = requests[leg].canonicalSymbol;
      const auto initialized = binary_corpus::initializeBinaryMarketSession(
          selected, source.selection, plan, owner->sessions[leg], error);
      if (!isOk(initialized)) return initialized;
      owner->sessions[leg].capturedDirectoryRevision = metadata.sourceLifecycleRevisionsAtBegin[sourceId - 1u];
      owner->sessions[leg].capturedUnhealthyChannels = source.unhealthyMask =
          metadata.sourceUnhealthyChannelMasksAtBegin[sourceId - 1u];
    }
    status = owner->cursor.next(owner->head, owner->available, error);
    if (!hftrec::isOk(status)) return binary_corpus::convertStatus(status);
    impl_ = std::move(owner);
    error.clear();
    return Status::Ok;
  } catch (...) {
    error = "failed to allocate bounded corpus stream";
    return Status::IoError;
  }
}

Status BinaryCorpusStream::next(std::vector<TimelineEvent>& events,
    bool& available, std::string& error) noexcept {
  events.clear();
  available = false;
  if (!impl_) return Status::InvalidArgument;
  try {
    if (impl_->previousLeg < impl_->sessions.size())
      clearRows(impl_->sessions[impl_->previousLeg]);
    std::size_t leg = impl_->sources.size();
    while (impl_->available) {
      const auto& header = impl_->head.header;
      if (header.sourceId > 0u && header.sourceId <= impl_->sourceLegs.size()) {
        leg = impl_->sourceLegs[header.sourceId - 1u];
        if (leg < impl_->sources.size() &&
            (impl_->sources[leg].requiredMask & corpus::binaryMarketChannelBit(header.channel)) != 0u &&
            !observedUnhealthy(impl_->head) &&
            (header.flags & corpus::BinaryMarketRecordTraderReplayCompatible) == 0u) {
          error = "required captured market row is not trader-replay compatible";
          return Status::Unsupported;
        }
        if (leg < impl_->sources.size() &&
            (header.channel == corpus::BinaryMarketChannel::SourceLifecycle ||
             ((impl_->sources[leg].requestedMask & corpus::binaryMarketChannelBit(header.channel)) != 0u &&
              (observedUnhealthy(impl_->head) ||
               ((impl_->sources[leg].selectedMask & corpus::binaryMarketChannelBit(header.channel)) != 0u &&
                (header.flags & corpus::BinaryMarketRecordTraderReplayCompatible) != 0u))))) break;
      }
      const auto status = impl_->cursor.next(impl_->head, impl_->available, error);
      if (!hftrec::isOk(status)) return binary_corpus::convertStatus(status);
    }
    if (!impl_->available) return Status::Ok;
    auto& source = impl_->sources[leg];
    auto& selection = source.selection;
    selection.records.swap(impl_->records);
    selection.records.clear();
    const auto first = impl_->head.header;
    std::uint16_t transactions = 1u;
    if (first.channel == corpus::BinaryMarketChannel::Depth) {
      const auto* depth = binary_corpus::checkedPayload<corpus::BinaryMarketDepthChunkPayload>(impl_->head);
      if (!depth || !depth->transactionPartCount ||
          depth->transactionPartCount > cxet::market::kMaximumDepthTransactionParts) {
        error = "invalid bounded depth transaction count";
        return Status::CorruptData;
      }
      transactions = depth->transactionPartCount;
    }
    // A parser recovery transaction has at most one snapshot and the canonical
    // pending delta bound. Each part has the canonical normalized level bound.
    std::uint64_t totalLevels = 0u;
    for (std::uint16_t transaction = 0u; transaction < transactions; ++transaction) {
      std::uint16_t chunks = 1u;
      if (first.channel == corpus::BinaryMarketChannel::Depth) {
        const auto* depth = binary_corpus::checkedPayload<corpus::BinaryMarketDepthChunkPayload>(impl_->head);
        const auto maxChunks = (rawdata::kMaxOrderbookLevels +
            corpus::kBinaryMarketDepthLevelsPerRecord - 1u) / corpus::kBinaryMarketDepthLevelsPerRecord;
        if (!depth || !depth->partCount || depth->partCount > maxChunks ||
            depth->transactionPartIndex != transaction || depth->transactionPartCount != transactions) {
          error = "invalid bounded depth transaction part";
          return Status::CorruptData;
        }
        totalLevels += depth->totalLevelCount;
        if (totalLevels > cxet::market::kMaximumDepthTransactionMutations) {
          error = "binary corpus depth transaction exceeds canonical mutation bound";
          return Status::CorruptData;
        }
        chunks = depth->partCount;
      }
      for (std::uint16_t chunk = 0u; chunk < chunks; ++chunk) {
        if (!impl_->available || impl_->head.header.sourceId != first.sourceId ||
            impl_->head.header.sourceGeneration != first.sourceGeneration ||
            impl_->head.header.sessionEpoch != first.sessionEpoch ||
            impl_->head.header.frameSequence != first.frameSequence ||
            impl_->head.header.receiveMonotonicNs != first.receiveMonotonicNs) {
          error = "truncated binary corpus application transaction";
          return Status::CorruptData;
        }
        selection.records.push_back(impl_->head);
        const auto status = impl_->cursor.next(impl_->head, impl_->available, error);
        if (!hftrec::isOk(status)) return binary_corpus::convertStatus(status);
      }
    }
    auto& session = impl_->sessions[leg];
    if (first.channel == corpus::BinaryMarketChannel::SourceLifecycle) {
      const auto* lifecycle = binary_corpus::checkedPayload<corpus::BinaryMarketSourceLifecyclePayload>(selection.records[0]);
      if (!lifecycle || lifecycle->source.sourceId != first.sourceId ||
          lifecycle->source.initialSourceGeneration != first.sourceGeneration) {
        error = "invalid captured source lifecycle identity"; return Status::CorruptData;
      }
      if (lifecycle->source.quantityConversion != selection.source.quantityConversion) {
        error = "captured market quantity authority changed without canonical state rebind";
        return Status::Unsupported;
      }
      if (lifecycle->cause == corpus::BinaryMarketSourceTransitionCause::Subscription &&
          (source.requiredMask & lifecycle->source.availableChannelMask &
           ~lifecycle->source.traderReplayChannelMask) != 0u) {
        error = "required available lifecycle channel is recorded-only and not trader-replay compatible";
        return Status::Unsupported;
      }
      const auto arrival = binary_corpus::makeArrival(selection.manifest, first);
      if (!hftrec::replay::hasCapturedSourceLifecycleArrival(arrival)) {
        error = "source lifecycle has no exact administrative arrival"; return Status::CorruptData;
      }
      session.sourceLifecycle.push_back({*lifecycle, arrival, first.exchangeTimestampNs});
      binary_corpus::includeEvidence(session, first);
      if (lifecycle->source.initialSourceGeneration != source.selection.source.initialSourceGeneration ||
          ((lifecycle->channelMaskBefore ^ lifecycle->channelMaskAfter) &
              corpus::binaryMarketChannelBit(corpus::BinaryMarketChannel::BookTicker)) != 0u ||
          lifecycle->affectedChannel == static_cast<std::uint8_t>(corpus::BinaryMarketChannel::BookTicker)) {
        const auto ingest = source.decoder.ingestSequence;
        source.decoder = {};
        source.decoder.ingestSequence = ingest;
      }
      const auto lifecycleChannels = static_cast<std::uint16_t>((lifecycle->channelMaskBefore ^ lifecycle->channelMaskAfter) |
          (lifecycle->affectedChannel > 0u && lifecycle->affectedChannel <= corpus::kBinaryMarketChannelCount
              ? corpus::binaryMarketChannelBit(static_cast<corpus::BinaryMarketChannel>(lifecycle->affectedChannel)) : 0u));
      if (lifecycle->cause!=corpus::BinaryMarketSourceTransitionCause::Subscription) {
        const auto bit=corpus::binaryMarketChannelBit(static_cast<corpus::BinaryMarketChannel>(lifecycle->affectedChannel));
        if (corpus::binaryMarketTransitionIsUnhealthy(lifecycle->cause)) source.unhealthyMask|=bit;
        else if (lifecycle->cause==corpus::BinaryMarketSourceTransitionCause::NativeRecovered)
          source.unhealthyMask&=static_cast<std::uint16_t>(~bit);
        // Native markers sample the shared directory after their frame arrived.
        // Retain the selection state established by actual operator boundaries.
        selection.source.initialSourceGeneration=lifecycle->source.initialSourceGeneration;
      } else {
        source.unhealthyMask &= static_cast<std::uint16_t>(~(lifecycle->source.initialSourceGeneration !=
            selection.source.initialSourceGeneration ? std::uint16_t{0xffu} : lifecycleChannels));
        selection.source = lifecycle->source;
        source.selectedMask = source.requestedMask & selection.source.availableChannelMask & selection.source.traderReplayChannelMask;
      }
      corpus::BinaryMarketSelectionRequest changed;
      changed.root = session.sessionPath; changed.exchange = session.exchange;
      changed.market = session.market; changed.canonicalSymbol = session.symbol;
      SessionLoadPlan noHistory;
      noHistory.loadTrades = noHistory.loadBookTickers = false;
      const auto updated = binary_corpus::initializeBinaryMarketSession(changed, selection, noHistory, session, error);
      if (!isOk(updated)) return updated;
    } else {
      const auto bit = corpus::binaryMarketChannelBit(first.channel);
      const bool unhealthy = std::any_of(selection.records.begin(), selection.records.end(), observedUnhealthy);
      const bool previous = (source.unhealthyMask & bit) != 0u;
      if (unhealthy || previous) {
        // Keep the exact observed record, including native depth Gap payload.
        // It supplies readiness evidence and never installs unhealthy prices.
        const auto arrival = binary_corpus::makeArrival(selection.manifest, first);
        if (!hftrec::replay::hasCapturedApplicationArrival(arrival)) {
          error = "observed health lacks exact application arrival"; return Status::CorruptData;
        }
        session.sourceHealth.push_back({selection.records.front(), arrival, unhealthy});
        resetDecoderChannel(source.decoder, first.channel);
        if (unhealthy) source.unhealthyMask |= bit;
        else source.unhealthyMask &= static_cast<std::uint16_t>(~bit);
      }
      if (unhealthy) {
        binary_corpus::includeEvidence(session, first);
      } else {
        const auto status = binary_corpus::decodeBinaryMarketRecords(
            selection, source.decoder, session, error);
        if (!isOk(status)) return status;
      }
    }
    buildTimeline(session, events);
    // Replay cannot use a later captured frame to manufacture earlier venue
    // state. Exchange timestamps remain in payloads and exchangeTsNs.
    for (auto& event : events) {
      event.legIndex = static_cast<std::uint32_t>(leg);
      event.tsNs = event.deliveryTsNs;
      event.venueScheduleAdjusted = !event.applicationOnly && event.exchangeTsNs != event.deliveryRealtimeNs;
    }
    std::stable_sort(events.begin(), events.end(), [](const auto& a, const auto& b) {
      return std::tuple{a.shardSequence, a.eventOrdinal, a.kind == kTimelineKindHealth ? 0u : 1u, a.ingestSeq} <
          std::tuple{b.shardSequence, b.eventOrdinal, b.kind == kTimelineKindHealth ? 0u : 1u, b.ingestSeq};
    });
    selection.records.clear();
    selection.records.swap(impl_->records);
    impl_->previousLeg = leg;
    available = !events.empty();
    return Status::Ok;
  } catch (...) {
    error = "bounded corpus transaction decoding failed";
    return Status::IoError;
  }
}
}  // namespace hft_backtest::detail
