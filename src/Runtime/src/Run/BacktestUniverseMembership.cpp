#include "BacktestUniverse.hpp"
#include <limits>

namespace hft_backtest::run_detail {
namespace {
void resetCapturedChannels(detail::EngineState& engine, std::uint16_t channels) noexcept {
  namespace corpus = hftrec::corpus;
  const auto touched = [&](corpus::BinaryMarketChannel channel) {
    return (channels & corpus::binaryMarketChannelBit(channel)) != 0u;
  };
  const bool resetTrade = touched(corpus::BinaryMarketChannel::Trade);
  const bool resetBbo = touched(corpus::BinaryMarketChannel::BookTicker);
  const bool resetDepth = touched(corpus::BinaryMarketChannel::Depth);
  if (resetTrade) {
    engine.reconstructionTradeRebasePending = true;
    engine.venueReconstructionTradeRebasePending = true;
    for (auto* snapshot : {&engine.snapshot, &engine.venueSnapshot}) {
      snapshot->lastTrade = {};
      snapshot->lastTradeIdentityShape = cxet::api::market::PublicMarketNativeIdentityShape::None;
      snapshot->hasTrade = trading_core::kAlgoFlagFalse;
    }
    engine.reconstructionTradeIdentity = {};
    engine.venueReconstructionTradeIdentity = {};
    detail::setStrategySourceDeliveryTimestamp(engine, cxet::api::market::PublicMarketDataStream::Trades, 0);
  }
  if (resetBbo) {
    engine.reconstructedQuoteReceiveUtc=0u;
    trading_core::clearBookTickerSnapshot(engine.snapshot);
    trading_core::clearBookTickerSnapshot(engine.venueSnapshot);
    engine.reconstructionBookTickerIdentity = {};
    engine.venueReconstructionBookTickerIdentity = {};
    detail::setStrategySourceDeliveryTimestamp(engine, cxet::api::market::PublicMarketDataStream::BookTicker, 0);
  }
  if (resetBbo) {
    trading_core::resetBboReconstruction(engine.bboReconstruction);
    trading_core::resetBboReconstruction(engine.venueBboReconstruction);
    trading_core::copyReconstructedBboSnapshot(engine.snapshot, trading_core::bboReconstructionView(engine.bboReconstruction));
    trading_core::copyReconstructedBboSnapshot(engine.venueSnapshot, trading_core::bboReconstructionView(engine.venueBboReconstruction));
    engine.reconstructionTradeRebasePending = false;
    engine.venueReconstructionTradeRebasePending = false;
  }
  if (resetDepth) {
    engine.capturedDepthRecoveryPending = false;
    engine.capturedDepthAdministrativeUnhealthy = false;
    engine.executionLiquidity.reset();
    engine.strategyDepthBook.reset();
    engine.venueDepthBook.reset();
    const auto resetProjection = [](trading_core::LocalOrderBookState& book) {
      const auto config = book.config;
      const auto symbol = book.symbol;
      book.reset(); book.config = config; book.symbol = symbol;
    };
    resetProjection(engine.orderBook);
    resetProjection(engine.venueOrderBook);
    trading_core::clearOrderBookSnapshot(engine.snapshot);
    trading_core::clearOrderBookSnapshot(engine.venueSnapshot);
    detail::setStrategySourceDeliveryTimestamp(engine, cxet::api::market::PublicMarketDataStream::Orderbook, 0);
  }
}
bool advanceExecutionBoundary(detail::EngineState& engine, std::string& error) {
  if (engine.executionBoundaryEpoch == std::numeric_limits<std::uint64_t>::max()) {
    error = "replay execution boundary capacity exhausted"; return false;
  }
  ++engine.executionBoundaryEpoch;
  return true;
}
}
bool applyCapturedSourceHealth(detail::EngineState& engine,
    const detail::SourceHealthRow& row, std::int64_t replayTsNs, std::string& error) {
  namespace corpus = hftrec::corpus;
  const auto& header = row.record.header;
  if (!corpus::validBinaryMarketChannel(header.channel) ||
      header.channel == corpus::BinaryMarketChannel::SourceLifecycle ||
      header.sourceGeneration != engine.capturedSourceGeneration ||
      (header.flags & corpus::BinaryMarketRecordGapBoundary) != 0u) {
    error = "observed source health does not match replay source authority"; return false;
  }
  const auto bit = corpus::binaryMarketChannelBit(header.channel);
  if (header.channel == corpus::BinaryMarketChannel::Depth &&
      !row.unhealthy && engine.capturedDepthAdministrativeUnhealthy) return true;
  const bool previous = (engine.capturedUnhealthyChannels & bit) != 0u;
  if (row.unhealthy) engine.capturedUnhealthyChannels |= bit;
  else engine.capturedUnhealthyChannels &= static_cast<std::uint16_t>(~bit);
  if (previous != row.unhealthy) {
    if ((bit & engine.capturedExecutionRequiredChannels) != 0u && !advanceExecutionBoundary(engine, error)) return false;
    resetCapturedChannels(engine, bit);
    engine.replayNotBeforeNs = replayTsNs;
    detail::bumpContextRevision(engine);
  }
  return true;
}

bool applyCapturedSourceLifecycle(detail::EngineState& engine,
    const detail::SourceLifecycleRow& row, std::int64_t replayTsNs,
    std::string& error) {
  namespace corpus = hftrec::corpus;
  const auto& lifecycle = row.payload;
  const auto& source = lifecycle.source;
  if (lifecycle.cause!=corpus::BinaryMarketSourceTransitionCause::Subscription) {
    if (lifecycle.kind!=corpus::BinaryMarketSourceLifecycleKind::DirectoryChanged ||
        source.sourceId!=row.arrival.sourceId || source.initialSourceGeneration!=row.arrival.sourceGeneration ||
        source.initialSourceGeneration<engine.capturedSourceGeneration ||
        (source.initialSourceGeneration!=engine.capturedSourceGeneration &&
         lifecycle.cause!=corpus::BinaryMarketSourceTransitionCause::NativeRecovered) ||
        lifecycle.affectedChannel==0u || lifecycle.affectedChannel>3u ||
        (engine.sharedAccount && source.quoteAssetId!=engine.sharedAccount->quoteAssetId)) {
      error="native source health does not match replay source authority";return false;
    }
    const bool changedGeneration=source.initialSourceGeneration!=engine.capturedSourceGeneration;
    const auto bit=corpus::binaryMarketChannelBit(static_cast<corpus::BinaryMarketChannel>(lifecycle.affectedChannel));
    if (lifecycle.affectedChannel==static_cast<std::uint8_t>(corpus::BinaryMarketChannel::Depth)) {
      if (changedGeneration) {error="depth health marker cannot change source generation";return false;}
      if ((bit & engine.capturedExecutionRequiredChannels)!=0u && !advanceExecutionBoundary(engine,error)) return false;
      // Availability is independent of the retained native book. A recovered
      // marker authorizes only the next fresh cut, never the old snapshot.
      engine.capturedUnhealthyChannels |= bit;
      engine.capturedDepthAdministrativeUnhealthy = true;
      engine.capturedDepthRecoveryPending = lifecycle.cause==corpus::BinaryMarketSourceTransitionCause::NativeRecovered;
      engine.replayNotBeforeNs=replayTsNs;
      detail::bumpContextRevision(engine);
      return true;
    }
    if ((changedGeneration || (bit & engine.capturedExecutionRequiredChannels)!=0u) &&
        !advanceExecutionBoundary(engine,error)) return false;
    if (corpus::binaryMarketTransitionIsUnhealthy(lifecycle.cause)) engine.capturedUnhealthyChannels|=bit;
    else if (lifecycle.cause==corpus::BinaryMarketSourceTransitionCause::NativeRecovered)
      engine.capturedUnhealthyChannels&=static_cast<std::uint16_t>(~bit);
    engine.capturedSourceGeneration=source.initialSourceGeneration;
    resetCapturedChannels(engine,changedGeneration?std::uint16_t{0xffu}:bit);
    engine.replayNotBeforeNs=replayTsNs;
    detail::bumpContextRevision(engine);
    return true;
  }
  const bool added = lifecycle.kind == corpus::BinaryMarketSourceLifecycleKind::Added;
  const bool readded = lifecycle.kind == corpus::BinaryMarketSourceLifecycleKind::Readded;
  const bool removed = lifecycle.kind == corpus::BinaryMarketSourceLifecycleKind::Removed;
  if ((!added && !readded && !removed &&
       lifecycle.kind != corpus::BinaryMarketSourceLifecycleKind::DirectoryChanged) ||
      source.sourceId != row.arrival.sourceId ||
      source.initialSourceGeneration != row.arrival.sourceGeneration ||
      lifecycle.directoryRevision <= engine.capturedDirectoryRevision ||
      lifecycle.channelMaskBefore != engine.capturedActiveChannels ||
      (added ? engine.capturedSourceMember || lifecycle.previousSourceGeneration != 0u :
          lifecycle.previousSourceGeneration != engine.capturedSourceGeneration) ||
      (readded && source.initialSourceGeneration < engine.capturedSourceGeneration) ||
      (removed && source.initialSourceGeneration != engine.capturedSourceGeneration) ||
      (engine.sharedAccount && source.quoteAssetId != engine.sharedAccount->quoteAssetId)) {
    error = "captured source lifecycle authority does not match replay owner";
    return false;
  }
  const bool changedGeneration = source.initialSourceGeneration != engine.capturedSourceGeneration;
  engine.capturedSourceManaged = true;
  engine.capturedSourceMember = lifecycle.channelMaskAfter != 0u;
  engine.capturedActiveChannels = lifecycle.channelMaskAfter;
  engine.capturedSourceGeneration = source.initialSourceGeneration;
  engine.capturedDirectoryRevision = lifecycle.directoryRevision;
  // Logical source generations can remain unchanged across native-lane
  // remove/readd. Execution continuity follows the observed lifecycle boundary.
  const auto changedChannels = static_cast<std::uint16_t>(
      lifecycle.channelMaskBefore ^ lifecycle.channelMaskAfter);
  const auto affectedChannels = static_cast<std::uint16_t>(changedChannels |
      (lifecycle.affectedChannel > 0u &&
          lifecycle.affectedChannel <= corpus::kBinaryMarketChannelCount
          ? corpus::binaryMarketChannelBit(static_cast<corpus::BinaryMarketChannel>(lifecycle.affectedChannel)) : 0u));
  if ((changedGeneration || (affectedChannels & engine.capturedExecutionRequiredChannels) != 0u) &&
      !advanceExecutionBoundary(engine, error)) return false;
  engine.capturedUnhealthyChannels &= static_cast<std::uint16_t>(~(changedGeneration ? std::uint16_t{0xffu} : affectedChannels));
  resetCapturedChannels(engine, changedGeneration ? std::uint16_t{0xffu} : affectedChannels);
  if (engine.capturedSourceMember && (added || readded || changedGeneration || changedChannels != 0u))
    engine.replayNotBeforeNs = replayTsNs;
  detail::bumpContextRevision(engine);
  return true;
}
}
