#include "SimulationInput.hpp"
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


Status SimulationInput::prepare(const corpus::CorpusCut& cut,
    std::span<const BacktestCorpusSourceRequest> requests, const SessionLoadPlan& plan,
    std::string& error) {
  if (cut.sources.empty() || cut.sources.size()>corpus::kBinaryMarketMaximumSources || plan.requireCandles) {
    error="simulation requires a bounded catalog and available strategy channels";return Status::Unsupported;
  }
  if(cut.sources.size()<sourceLegs.size()) {error="captured source catalog shrank";return Status::CorruptData;}
  sourceLegs.resize(cut.sources.size(),std::numeric_limits<std::size_t>::max());
  for (const auto& captured:cut.sources) {
    if (!captured.sourceId || captured.sourceId>sourceLegs.size()) {error="invalid captured source id";return Status::CorruptData;}
    const auto exchange=std::string(binary_corpus::fixedText(captured.venue.data(),captured.venue.size(),captured.venueBytes));
    const auto market=std::string(binary_corpus::fixedText(captured.market.data(),captured.market.size(),captured.marketBytes));
    const auto symbol=std::string(binary_corpus::fixedText(captured.canonicalSymbol.data(),captured.canonicalSymbol.size(),captured.canonicalSymbolBytes));
    if (!requests.empty() && std::none_of(requests.begin(),requests.end(),[&](const auto& selected){
      return selected.exchange==exchange && selected.market==market && selected.canonicalSymbol==symbol;
    })) continue;
    if(sourceLegs[captured.sourceId-1u]!=std::numeric_limits<std::size_t>::max()) {
      const auto& bound=sources[sourceLegs[captured.sourceId-1u]].selection.source;
      if(bound.venueId!=captured.venueId || bound.marketRaw!=captured.marketRaw ||
          bound.canonicalSymbolId!=captured.canonicalSymbolId || bound.quoteAssetId!=captured.quoteAssetId ||
          bound.quantityConversion!=captured.quantityConversion) {
        error="captured immutable source identity or quantity authority changed";return Status::Unsupported;
      }
      continue;
    }
    const auto leg=sources.size();
    sourceLegs[captured.sourceId-1u]=leg;
    auto& source=sources.emplace_back();
    source.selection.manifest=cut.manifest;source.selection.source=captured;
    source.requestedMask=requestedMask(plan,false);source.requiredMask=requestedMask(plan,true);
    if ((source.requiredMask & ~captured.configuredChannelMask)!=0u ||
        (source.requiredMask & captured.availableChannelMask & ~captured.traderReplayChannelMask)!=0u) {
      error="captured source lacks required compatible market capabilities";return Status::Unsupported;
    }
    source.selectedMask=source.requestedMask & captured.availableChannelMask & captured.traderReplayChannelMask;
    corpus::BinaryMarketSelectionRequest request;
    request.exchange=exchange;request.market=market;request.canonicalSymbol=symbol;
    auto& session=sessions.emplace_back();
    const auto status=binary_corpus::initializeBinaryMarketSession(request,source.selection,plan,session,error);
    if (!isOk(status)) return status;
  }
  if(sessions.empty()) {error="simulation selection has no captured sources";return Status::ConfigMismatch;}
  return Status::Ok;
}
Status SimulationInput::append(const corpus::BinaryMarketRecord& record,
    std::vector<TimelineEvent>& events,std::string& error) {
  events.clear();
  const auto& header=record.header;
  if (!header.sourceId || header.sourceId>sourceLegs.size()) {error="record source is outside prepared catalog";return Status::CorruptData;}
  const auto leg=sourceLegs[header.sourceId-1u];
  if (leg>=sources.size()) {
    if (!transaction.empty()) {error="foreign record splits captured transaction";return Status::CorruptData;}
    return Status::Ok;
  }
  auto& source=sources[leg];
  if (transaction.empty()) {
    if (priorLeg<sessions.size()) clearRows(sessions[priorLeg]);
    const auto bit=corpus::binaryMarketChannelBit(header.channel);
    if (header.channel!=corpus::BinaryMarketChannel::SourceLifecycle &&
        (source.requestedMask & bit)==0u) return Status::Ok;
    if ((source.requiredMask & bit)!=0u && !observedUnhealthy(record) &&
        (header.flags & corpus::BinaryMarketRecordTraderReplayCompatible)==0u) {
      error="required market row is recorded-only";return Status::Unsupported;
    }
    transactionLeg=leg;
  } else {
    const auto& first=transaction.front().header;
    if (leg!=transactionLeg || header.sourceGeneration!=first.sourceGeneration ||
        header.sessionEpoch!=first.sessionEpoch || header.frameSequence!=first.frameSequence ||
        header.receiveMonotonicNs!=first.receiveMonotonicNs || header.channel!=first.channel) {
      error="captured application transaction is incomplete";return Status::CorruptData;
    }
  }
  constexpr auto maxRecords=cxet::market::kMaximumDepthTransactionMutations / corpus::kBinaryMarketDepthLevelsPerRecord +
      cxet::market::kMaximumDepthTransactionParts;
  if (transaction.size()>=maxRecords) {error="captured transaction exceeds canonical bound";return Status::Unsupported;}
  transaction.push_back(record);
  if (header.channel==corpus::BinaryMarketChannel::Depth) {
    const auto* depth=binary_corpus::checkedPayload<corpus::BinaryMarketDepthChunkPayload>(record);
    if (!depth || !depth->partCount || !depth->transactionPartCount ||
        depth->partIndex>=depth->partCount || depth->transactionPartIndex>=depth->transactionPartCount ||
        depth->transactionPartCount>cxet::market::kMaximumDepthTransactionParts) {
      error="invalid depth transaction shape";return Status::CorruptData;
    }
    if (depth->partIndex+1u!=depth->partCount ||
        depth->transactionPartIndex+1u!=depth->transactionPartCount) return Status::Ok;
  }
  const auto status=decode(events,error);
  transaction.clear();return status;
}
Status SimulationInput::decode(std::vector<TimelineEvent>& events,std::string& error) {
  const auto leg=transactionLeg;
  auto& source=sources[leg];
  auto& selection=source.selection;
  selection.records.swap(transaction);
  const auto first=selection.records.front().header;
    auto& session = sessions[leg];
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
    priorLeg = leg;
    return Status::Ok;
}
} // namespace hft_backtest::detail
