#include "trading_core/Algorithm/Math/Integer/Saturating.hpp"
#include "../BacktestStrategyRuntime.hpp"

#include "../../Config/BacktestRunConfig.hpp"
#include "../Execution/BacktestStrategyIntentLowering.hpp"
#include "../Diagnostics/BacktestStrategyTrace.hpp"

#include "trading_core/Strategy/Context/StrategyContextView.hpp"
#include "trading_core/Strategy/Orders/WarmupIntents.hpp"
#include "trading_core/Strategy/StrategyInputFreshness.hpp"
#include "trading_core/Strategy/StrategyMarketData.hpp"

#include <algorithm>
#include <limits>
#include <string>

#include "../Internal/BacktestStrategyRuntimeHelpers.hpp"

namespace hft_backtest::strategy_runtime_detail {

std::int64_t toInt64Time(TimeNs value) noexcept {
  if (value.raw > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return std::numeric_limits<std::int64_t>::max();
  return static_cast<std::int64_t>(value.raw);
}

[[nodiscard]] bool hasExactExchangeTimestamp(
    const hftrec::replay::EventArrival& arrival,
    std::int64_t exchangeTimestampNs) noexcept {
  return exchangeTimestampNs > 0 &&
      (arrival.flags &
       hftrec::replay::EventArrivalExchangeTimestampMissing) == 0u;
}

[[nodiscard]] trading_core::BboReconstructionTimestampOrigin
reconstructionTimestampOrigin(
    const hftrec::replay::EventArrival& arrival,
    std::int64_t exchangeTimestampNs) noexcept {
  return hasExactExchangeTimestamp(arrival, exchangeTimestampNs)
      ? trading_core::BboReconstructionTimestampOrigin::Exchange
      : trading_core::BboReconstructionTimestampOrigin::Unknown;
}

void overlayBacktestReconstructedBbo(
    trading_core::StrategyMarketSnapshot& snapshot,
    trading_core::LocalOrderBookState& orderBook,
    const trading_core::BboReconstructionView& view) noexcept {
  if (trading_core::validBboReconstruction(view) &&
      orderBook.initialized.raw == trading_core::kAlgoFlagTrue.raw) {
    (void)trading_core::applyBboOverlay(
        orderBook, view.bookTicker,
        trading_core::AlgoFlag{view.bidQtyKnown.raw},
        trading_core::AlgoFlag{view.askQtyKnown.raw});
    trading_core::bindOrderBookSnapshot(snapshot, orderBook);
  }
}

void publishBacktestReconstructedBbo(
    trading_core::StrategyMarketSnapshot& snapshot,
    trading_core::LocalOrderBookState& orderBook,
    const trading_core::BboReconstructionState& bbo,
    std::uint64_t receiveMonotonicNs) noexcept {
  const trading_core::BboReconstructionView view = trading_core::bboReconstructionView(bbo);
  TimeNs receiveMonotonic{};
  receiveMonotonic.raw = receiveMonotonicNs;
  trading_core::copyReconstructedBboSnapshot(
      snapshot, view, receiveMonotonic);
  overlayBacktestReconstructedBbo(snapshot, orderBook, view);
}

void refreshCapturedBookTicker(
    trading_core::StrategyMarketSnapshot& snapshot,
    const detail::TimelineEvent& event) noexcept {
  TimeNs receiveMonotonic{};
  receiveMonotonic.raw = static_cast<std::uint64_t>(
      event.deliveryTsNs > 0 ? event.deliveryTsNs : 0);
  TimeNs receiveRealtime{};
  receiveRealtime.raw = static_cast<std::uint64_t>(
      event.deliveryRealtimeNs > 0 ? event.deliveryRealtimeNs : 0);
  if (snapshot.hasBookTicker.raw ==
      trading_core::kAlgoFlagTrue.raw) {
    snapshot.lastBookTickerRecvMono = receiveMonotonic;
    snapshot.lastBookTickerRecvUtc = receiveRealtime;
  }
  if (snapshot.hasReconstructedBbo.raw ==
      trading_core::kAlgoFlagTrue.raw) {
    snapshot.reconstructedBookTickerRecvMono = receiveMonotonic;
  }
}

[[nodiscard]] bool applyCapturedBookTicker(
    trading_core::StrategyMarketSnapshot& snapshot,
    trading_core::BboReconstructionState& bbo,
    const detail::BookTickerRow& row,
    const detail::TimelineEvent& event,
    std::uint64_t replayReceiveMonotonicNs,
    const cxet::composite::BookTicker& ticker) noexcept {
  TimeNs receiveMonotonic{};
  receiveMonotonic.raw = replayReceiveMonotonicNs;
  TimeNs receiveRealtime{};
  receiveRealtime.raw = static_cast<std::uint64_t>(
      event.deliveryRealtimeNs > 0 ? event.deliveryRealtimeNs : 0);
  const bool exactExchangeTimestamp =
      hasExactExchangeTimestamp(row.arrival, row.tsNs);
  const auto timestampOrigin = exactExchangeTimestamp
      ? trading_core::MarketTimestampOrigin::Exchange
      : trading_core::MarketTimestampOrigin::Receive;
  const auto reconstructionOrigin =
      reconstructionTimestampOrigin(row.arrival, row.tsNs);
  if (row.bidPresent && row.askPresent) {
    if (!trading_core::applyBboReconstructionBookTicker(
            bbo, ticker, reconstructionOrigin)) {
      return false;
    }
    trading_core::copyBookTickerSnapshot(
        snapshot, ticker, receiveMonotonic, receiveRealtime,
        timestampOrigin);
    return true;
  }

  if (!row.bidPresent && !row.askPresent) return false;
  cxet::composite::BookTickerSideUpdate update{};
  update.ts = ticker.ts;
  bool applied = false;
  if (row.bidPresent) {
    update.side = Side::Buy();
    update.action = cxet::composite::BookTickerSideAction::Upsert;
    update.level = ticker.bid;
    applied = trading_core::applyBboReconstructionBookTickerSide(
        bbo, update, reconstructionOrigin);
    if (!applied) return false;
  }
  if (row.askPresent) {
    update.side = Side::Sell();
    update.action = cxet::composite::BookTickerSideAction::Upsert;
    update.level = ticker.ask;
    applied = trading_core::applyBboReconstructionBookTickerSide(
        bbo, update, reconstructionOrigin);
    if (!applied) return false;
  }
  const auto capturedView = trading_core::bboReconstructionView(bbo);
  if (trading_core::validBboReconstruction(capturedView)) {
    trading_core::copyBookTickerSnapshot(
        snapshot, capturedView.bookTicker, receiveMonotonic, receiveRealtime,
        timestampOrigin);
  } else {
    trading_core::copyPartialBookTickerSnapshot(
        snapshot, capturedView, receiveMonotonic, receiveRealtime,
        timestampOrigin);
  }
  return applied;
}

}  // namespace hft_backtest::strategy_runtime_detail

namespace hft_backtest {
using strategy_intent_lowering::applyToLegs;
using strategy_intent_lowering::applyToSingleLeg;
using strategy_intent_lowering::applyTransactionCycle;
using strategy_intent_lowering::bindTransactionLegs;
using strategy_intent_lowering::configuredDefaultExecutionRoute;
using strategy_intent_lowering::configuredPrimaryLegIndex;
using strategy_intent_lowering::footprintSupported;
using strategy_intent_lowering::validateOutput;
using strategy_intent_lowering::routeLegIndex;
using strategy_intent_lowering::routeTradable;
using strategy_intent_lowering::transactionAdmission;
using strategy_runtime_detail::applyCapturedBookTicker;
using strategy_runtime_detail::overlayBacktestReconstructedBbo;
using strategy_runtime_detail::publishBacktestReconstructedBbo;
using strategy_runtime_detail::reconstructionTimestampOrigin;
using strategy_runtime_detail::refreshCapturedBookTicker;

void MultiLegCycleScratch::reset(std::size_t count) noexcept {
  legCount = count;
  revisions.fill(0u);
}

void MultiLegCycleScratch::refreshLeg(std::size_t leg,
                                      const std::vector<detail::EngineState>& engines,
                                      const std::vector<StrategyInstrumentSpec>& legInstruments) noexcept {
  if (leg >= legCount || leg >= engines.size()) return;
  const detail::EngineState& engine = engines[leg];
  snapshots[leg] = engine.snapshot;
  candles[leg] = engine.candle;
  orderViews[leg].orders = orderStorage[leg].data();
  orderViews[leg].count = detail::visibleOrders(engine, orderStorage[leg].data(), orderStorage[leg].size());
  positions[leg] = engine.position;
  accounts[leg] = accountStateFromEngine(engine);
  instruments[leg] = leg < legInstruments.size() ? legInstruments[leg] : StrategyInstrumentSpec{};
  markets[leg] = engine.market;
  revisions[leg] = engine.contextRevision;
}

void MultiLegCycleScratch::refreshDirty(const std::vector<detail::EngineState>& engines,
                                        const std::vector<StrategyInstrumentSpec>& legInstruments) noexcept {
  for (std::size_t leg = 0; leg < legCount; ++leg) {
    if (leg >= engines.size()) break;
    if (revisions[leg] != engines[leg].contextRevision) refreshLeg(leg, engines, legInstruments);
  }
}


TimeNs timeNsFromI64(std::int64_t value) noexcept {
  TimeNs out{};
  if (value > 0) out.raw = static_cast<std::uint64_t>(value);
  return out;
}

TimeNs replayUtcFromMonotonic(std::int64_t monotonicNs,
                              std::int64_t utcOffsetNs) noexcept {
  TimeNs out{};
  const __int128 value = static_cast<__int128>(monotonicNs) +
      static_cast<__int128>(utcOffsetNs);
  if (value <= 0) return out;
  out.raw = value > std::numeric_limits<std::uint64_t>::max()
      ? std::numeric_limits<std::uint64_t>::max()
      : static_cast<std::uint64_t>(value);
  return out;
}

void applyMarketEventToSnapshots(detail::EngineState& engine,
                                 const detail::SessionData& session,
                                 const detail::TimelineEvent& event,
                                 const Symbol& selectedSymbol,
                                 const StrategyInstrumentSpec& instrument,
                                 bool strategyVisible) {
  trading_core::StrategyMarketSnapshot& snapshot = strategyVisible ? engine.snapshot : engine.venueSnapshot;
  trading_core::LocalOrderBookState& orderBook = strategyVisible ? engine.orderBook : engine.venueOrderBook;
  trading_core::BboReconstructionState& bbo = strategyVisible ? engine.bboReconstruction : engine.venueBboReconstruction;
  detail::ReconstructionSourceIdentity* identity = nullptr;
  detail::ReconstructionSourceIdentity* counterpart = nullptr;
  if (event.kind == detail::kTimelineKindBookTicker) {
    identity = strategyVisible
        ? &engine.reconstructionBookTickerIdentity
        : &engine.venueReconstructionBookTickerIdentity;
    counterpart = strategyVisible
        ? &engine.reconstructionTradeIdentity
        : &engine.venueReconstructionTradeIdentity;
  } else if (event.kind == detail::kTimelineKindTrade) {
    identity = strategyVisible
        ? &engine.reconstructionTradeIdentity
        : &engine.venueReconstructionTradeIdentity;
    counterpart = strategyVisible
        ? &engine.reconstructionBookTickerIdentity
        : &engine.venueReconstructionBookTickerIdentity;
  }
  if (identity) {
    const detail::ReconstructionSourceIdentity nextIdentity{
        .producerEpoch = event.producerEpoch,
        .sourceGeneration = event.sourceGeneration,
        .sessionEpoch = event.sessionEpoch,
        .sourceId = event.sourceId,
        .initialized = true,
    };
    const auto sameIdentity = [](const detail::ReconstructionSourceIdentity& left,
                                 const detail::ReconstructionSourceIdentity& right) noexcept {
      return left.initialized == right.initialized &&
          left.producerEpoch == right.producerEpoch &&
          left.sourceGeneration == right.sourceGeneration &&
          left.sessionEpoch == right.sessionEpoch &&
          left.sourceId == right.sourceId;
    };
    const bool changed = identity->initialized &&
        !sameIdentity(*identity, nextIdentity);
    const bool counterpartAlreadyCurrent =
        counterpart && counterpart->initialized &&
        sameIdentity(*counterpart, nextIdentity);
    *identity = nextIdentity;
    if (changed && !counterpartAlreadyCurrent) {
      trading_core::resetBboReconstruction(bbo);
      trading_core::clearBookTickerSnapshot(snapshot);
      trading_core::copyReconstructedBboSnapshot(
          snapshot, trading_core::BboReconstructionView{});
      const trading_core::LocalOrderBookConfig config = orderBook.config;
      const Symbol symbol = orderBook.symbol;
      orderBook.reset();
      orderBook.config = config;
      orderBook.symbol = symbol;
      trading_core::clearOrderBookSnapshot(snapshot);
    }
  }
  const std::int64_t replayReceiveMonotonic =
      strategyVisible ? event.deliveryTsNs : event.tsNs;
  // Synthetic execution is sequenced on replay delivery, the same causal
  // clock used to submit and activate simulated orders. Exchange timestamps
  // are market payload evidence and may be in a completely different epoch.
  const std::int64_t executionTsNs =
      event.deliveryTsNs > 0 ? event.deliveryTsNs : event.tsNs;
  const std::uint64_t replayReceiveMonotonicNs =
      static_cast<std::uint64_t>(
          replayReceiveMonotonic > 0 ? replayReceiveMonotonic : 0);
  bool changed = false;
  if (event.kind == detail::kTimelineKindDepth) {
    const detail::DepthRow& depth = session.depths[event.index];
    auto& depthBook = strategyVisible ? engine.strategyDepthBook : engine.venueDepthBook;
    if (!depthBook.apply(depth.levels, depth.tsNs, depth.arrival,
            depth.resetBookBeforeApply, depth.capturedSequence,
            depth.capturedPublicationSequence)) {
      engine.depthReplayFailed = true;
      trading_core::clearOrderBookSnapshot(snapshot);
      return;
    }
    TimeNs depthTimestamp{};
    depthTimestamp.raw = static_cast<std::uint64_t>(depth.tsNs > 0 ? depth.tsNs : 0);
    trading_core::copyDepthTimestamp(snapshot, depthTimestamp);
    orderBook.config.tickSizeRaw = instrument.tickSizeRaw;
    if (orderBook.hasStorage()) {
      (void)trading_core::refreshAuthoritativeOrderBook(
          orderBook, depthBook.view(), selectedSymbol);
    }
    overlayBacktestReconstructedBbo(
        snapshot, orderBook,
        trading_core::bboReconstructionView(bbo));
    trading_core::bindOrderBookSnapshot(snapshot, orderBook);
    changed = true;
  } else if (event.kind == detail::kTimelineKindTrade) {
    const auto& row = session.trades[event.index];
    const auto trade = detail::toRuntime(row);
    trading_core::copyTradeSnapshot(snapshot, trade);
    const bool reconstructionApplied =
        trading_core::applyBboReconstructionKnownInitiatorTrade(
        bbo, trade,
        reconstructionTimestampOrigin(row.arrival, row.tsNs));
    if (reconstructionApplied) {
      publishBacktestReconstructedBbo(
          snapshot, orderBook, bbo, replayReceiveMonotonicNs);
    }
    if (!strategyVisible) detail::tryFillOnTrade(engine, trade, executionTsNs);
    changed = true;
  } else if (event.kind == detail::kTimelineKindBookTicker) {
    const auto& row = session.bookTickers[event.index];
    if (row.freshnessOnly) {
      if (strategyVisible) {
        refreshCapturedBookTicker(snapshot, event);
        changed = true;
      }
    } else {
      const auto ticker = detail::toRuntime(row);
      const bool reconstructionApplied = applyCapturedBookTicker(
          snapshot, bbo, row, event,
          replayReceiveMonotonicNs, ticker);
      if (reconstructionApplied) {
        publishBacktestReconstructedBbo(
            snapshot, orderBook, bbo, replayReceiveMonotonicNs);
      }
      trading_core::bindOrderBookSnapshot(snapshot, orderBook);
      if (!strategyVisible) {
        detail::tryFillOnTop(engine, executionTsNs, true);
      }
      changed = true;
    }
  }
  if (changed) {
    if (strategyVisible) {
      recordBacktestStrategySourceDelivery(engine, event);
    }
    detail::bumpContextRevision(engine);
  }
}

void applyMarketEventToSnapshots(detail::EngineState& engine,
                                 const std::vector<detail::SessionData>& sessions,
                                 const detail::TimelineEvent& event,
                                 const Symbol& selectedSymbol,
                                 const StrategyInstrumentSpec& instrument,
                                 bool strategyVisible) {
  const std::size_t legIndex = event.legIndex;
  if (legIndex >= sessions.size()) return;
  applyMarketEventToSnapshots(engine, sessions[legIndex], event, selectedSymbol, instrument, strategyVisible);
}

void applyMarketEventToSnapshots(std::vector<detail::EngineState>& engines,
                                 const std::vector<detail::SessionData>& sessions,
                                 const detail::TimelineEvent& event,
                                 const std::vector<Symbol>& symbols,
                                 const std::vector<StrategyInstrumentSpec>& instruments,
                                 bool strategyVisible) {
  const std::size_t legIndex = event.legIndex;
  if (legIndex >= engines.size() || legIndex >= sessions.size() || legIndex >= symbols.size() || legIndex >= instruments.size()) return;
  applyMarketEventToSnapshots(engines[legIndex], sessions[legIndex], event, symbols[legIndex], instruments[legIndex], strategyVisible);
}


}  // namespace hft_backtest
