#include "../../src/Runtime/src/Execution/BacktestClientOrderId.hpp"

#include "strategy/StatArbEdgeMaker/Strategy.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>

namespace {

using trading_core::strategy::StatArbEdgeMakerDesiredSlot;
using trading_core::strategy::StatArbEdgeMakerDirection;
using trading_core::strategy::StatArbEdgeMakerStrategy;

[[nodiscard]] bool expect(bool value, const char* message) {
  if (!value) std::cerr << "FAIL: " << message << '\n';
  return value;
}

[[nodiscard]] constexpr TimeNs timeNs(std::uint64_t raw) noexcept {
  return TimeNs{raw};
}

[[nodiscard]] constexpr Price price(std::int64_t raw) noexcept {
  return Price{raw};
}

[[nodiscard]] constexpr Amount amount(std::int64_t raw) noexcept {
  return Amount{raw};
}

void copyText(SmallBuf& out, const char* text) noexcept {
  out = SmallBuf{};
  if (!text) return;
  std::size_t i = 0u;
  for (; i + 1u < SmallBuf::capacity && text[i] != '\0'; ++i) {
    out.data[i] = text[i];
  }
  out.data[i] = '\0';
}

[[nodiscard]] cxet::composite::BookTickerRuntimeV1 ticker(
    std::uint64_t eventTs,
    std::int64_t bidRaw,
    std::int64_t askRaw) noexcept {
  cxet::composite::BookTickerRuntimeV1 out{};
  out.ts = timeNs(eventTs);
  out.bid.px = price(bidRaw);
  out.bid.qty = amount(100000000ll);
  out.ask.px = price(askRaw);
  out.ask.qty = amount(100000000ll);
  return out;
}

struct StrategyHarness {
  trading_core::OrderIntent orders[32]{};
  trading_core::CancelIntent cancels[32]{};
  trading_core::StrategyCommandIntent commands[32]{};
  trading_core::StrategyMarketSnapshot snapshots[2]{};
  trading_core::StrategyInstrumentSpec instruments[2]{};
  trading_core::StrategyAccountState accounts[2]{};
  trading_core::StrategyPositionState positions[2]{};
  trading_core::StrategyOrderState orderStorage[2][16]{};
  trading_core::StrategyOrderStateView orderViews[2]{};
  trading_core::StrategyDecisionTrace trace{};
  trading_core::ShardControlState control{};
  trading_core::StrategyContext ctx{};

  StrategyHarness() noexcept {
    ctx.marketData.snapshots = snapshots;
    ctx.marketData.count = 2u;
    ctx.control = &control;
    ctx.tradableLegMask = 3u;
    for (trading_core::StrategyInstrumentSpec& instrument : instruments) {
      instrument.tickSizeRaw = 1000000ll;
      instrument.stepSizeRaw = 100000000ll;
      instrument.contractBaseQtyRaw = 100000000ll;
      instrument.tickGrid =
          trading_core::prepareFastStepGrid(instrument.tickSizeRaw);
      instrument.qtyStepGrid =
          trading_core::prepareFastStepGrid(instrument.stepSizeRaw);
      instrument.contractBaseQtyGrid =
          trading_core::prepareFastStepGrid(instrument.contractBaseQtyRaw);
    }
    ctx.instrumentSpec = instruments[0];
    ctx.instrumentSpecs.legs = instruments;
    ctx.instrumentSpecs.count = 2u;
    ctx.accountStates.legs = accounts;
    ctx.accountStates.count = 2u;
    ctx.positionStates.legs = positions;
    ctx.positionStates.count = 2u;
    for (trading_core::StrategyPositionState& positionState : positions) {
      positionState.valid = true;
      positionState.netQty = amount(0ll);
    }
    for (std::size_t i = 0u; i < 2u; ++i) {
      orderViews[i].orders = orderStorage[i];
      orderViews[i].count = 0u;
    }
    ctx.orderState = orderViews[0];
    ctx.orderStates.legs = orderViews;
    ctx.orderStates.count = 2u;
    ctx.io.orderOut = Span<trading_core::OrderIntent>(orders, 32u);
    ctx.io.cancelOut = Span<trading_core::CancelIntent>(cancels, 32u);
    ctx.io.commandOut =
        Span<trading_core::StrategyCommandIntent>(commands, 32u);
    ctx.decisionTrace = &trace;
  }

  void setTickers(std::uint64_t eventTs,
                  std::int64_t bid0,
                  std::int64_t ask0,
                  std::int64_t bid1,
                  std::int64_t ask1) noexcept {
    snapshots[0] = trading_core::StrategyMarketSnapshot{};
    snapshots[1] = trading_core::StrategyMarketSnapshot{};
    const cxet::composite::BookTickerRuntimeV1 leg0 =
        ticker(eventTs, bid0, ask0);
    const cxet::composite::BookTickerRuntimeV1 leg1 =
        ticker(eventTs, bid1, ask1);
    trading_core::copyBookTickerSnapshot(snapshots[0], leg0);
    trading_core::copyBookTickerSnapshot(snapshots[1], leg1);

    trading_core::BboReconstructionView reconstructed0{};
    reconstructed0.bookTicker = leg0;
    reconstructed0.source = trading_core::kBboReconstructionSourceTrade;
    reconstructed0.valid = trading_core::kBboReconstructionFlagTrue;
    reconstructed0.bidQtyKnown = trading_core::kBboReconstructionFlagTrue;
    reconstructed0.askQtyKnown = trading_core::kBboReconstructionFlagTrue;
    trading_core::BboReconstructionView reconstructed1 = reconstructed0;
    reconstructed1.bookTicker = leg1;
    trading_core::copyReconstructedBboSnapshot(
        snapshots[0], reconstructed0, timeNs(eventTs));
    trading_core::copyReconstructedBboSnapshot(
        snapshots[1], reconstructed1, timeNs(eventTs));

    ctx.nowMono = timeNs(eventTs);
    ctx.nowUtc = ctx.nowMono;
    trading_core::initStrategyDecisionTrace(trace, eventTs);
    ctx.userEvent = nullptr;
    ctx.userEventAdmittedSemanticMask = 0u;
    ctx.hasUserEvent = false;
    ctx.problemEvent = nullptr;
    ctx.hasProblemEvent = false;
    ctx.hasEventLeg = false;
    ctx.eventLegIndex = 0u;
    ctx.io.clear();
  }

  void installLiveLimitOrdersFromOutput() noexcept {
    for (trading_core::StrategyOrderStateView& view : orderViews) {
      view.count = 0u;
    }
    for (std::size_t i = 0u; i < ctx.io.orderCount; ++i) {
      const trading_core::OrderIntent& intent = orders[i];
      if (intent.orderType != canon::OrderType::Limit ||
          intent.routeHandle.raw == 0u) {
        continue;
      }
      const std::size_t legIndex =
          static_cast<std::size_t>(intent.routeHandle.raw - 1u);
      if (legIndex >= 2u || orderViews[legIndex].count >= 16u) continue;
      trading_core::StrategyOrderState& state =
          orderStorage[legIndex][orderViews[legIndex].count++];
      state = trading_core::StrategyOrderState{};
      state.clientIntentId = intent.clientIntentId;
      state.desiredClientIntentId = intent.clientIntentId;
      state.quoteSlotId = intent.quoteSlotId;
      state.side = intent.side;
      state.orderType = intent.orderType;
      state.limitTimeInForcePolicy = intent.limitTimeInForcePolicy;
      state.price = intent.price;
      state.qty = intent.qty;
      state.desiredPrice = intent.price;
      state.desiredQty = intent.qty;
      state.reduceOnly =
          trading_core::positionReductionRequested(intent.positionReduction);
      state.state = trading_core::StrategyOrderLocalState::LiveLocal;
      state.valid = true;
    }
    ctx.orderState = orderViews[0];
    ctx.io.clear();
  }

  [[nodiscard]] trading_core::StrategyOrderState* findOrder(
      Id clientIntentId) noexcept {
    for (std::size_t legIndex = 0u; legIndex < 2u; ++legIndex) {
      for (std::size_t i = 0u; i < orderViews[legIndex].count; ++i) {
        trading_core::StrategyOrderState& state = orderStorage[legIndex][i];
        if (state.clientIntentId.raw == clientIntentId.raw) return &state;
      }
    }
    return nullptr;
  }
};

[[nodiscard]] constexpr std::int64_t bps(std::int64_t value) noexcept {
  return value * trading_core::kBpsE8ScaleRaw;
}

[[nodiscard]] StatArbEdgeMakerStrategy makeStrategy() noexcept {
  StatArbEdgeMakerStrategy strategy{};
  strategy.config.emaWindowMs = 5000u;
  strategy.config.entryEdgeBpsE8 = bps(10ll);
  strategy.config.exitEdgeBpsE8 = bps(2ll);
  strategy.config.triggerEdgeBpsE8 = bps(1ll);
  strategy.config.qtyRaw = 100000000ll;
  return strategy;
}

[[nodiscard]] trading_core::strategy::StatArbEdgeMakerQuote currentQuote(
    StrategyHarness& harness) noexcept {
  const auto leg0 = trading_core::strategy::marketData(
      harness.ctx, trading_core::strategy::leg(0u));
  const auto leg1 = trading_core::strategy::marketData(
      harness.ctx, trading_core::strategy::leg(1u));
  return StatArbEdgeMakerStrategy::buildQuote(
      trading_core::strategy::topOfBook(leg0),
      trading_core::strategy::topOfBook(leg1));
}

void setReadyEmaForDeviation(StatArbEdgeMakerStrategy& strategy,
                             trading_core::strategy::StatArbEdgeMakerQuote quote,
                             std::int64_t deviationBpsE8,
                             TimeNs nowMono) noexcept {
  const auto priceFromBps = [](std::int64_t baseRaw,
                               std::int64_t bpsE8) noexcept {
    const __int128 factor =
        static_cast<__int128>(trading_core::kBpsE8FactorRaw) +
        static_cast<__int128>(bpsE8);
    const __int128 raw =
        (static_cast<__int128>(baseRaw) * factor) /
        static_cast<__int128>(trading_core::kBpsE8FactorRaw);
    return raw > 0 &&
            raw <= static_cast<__int128>(
                       std::numeric_limits<std::int64_t>::max())
        ? static_cast<std::int64_t>(raw)
        : baseRaw;
  };
  strategy.config.emaWindowMs = 1u;
  constexpr std::uint64_t kNsPerMs = 1000000ull;
  const std::uint64_t windowNs =
      static_cast<std::uint64_t>(strategy.config.emaWindowMs) * kNsPerMs;
  strategy.state.emaReady = true;
  strategy.state.emaSamples = 2u;
  strategy.state.lastEmaTs = nowMono;
  strategy.state.emaWarmupStartMono =
      timeNs(nowMono.raw > windowNs ? nowMono.raw - windowNs : 1u);
  strategy.state.emaBpsE8 = quote.spreadBpsE8 - deviationBpsE8;
  trading_core::PairSideMeanReversionLegState& leg0 =
      strategy.state.sideEma[0];
  trading_core::PairSideMeanReversionLegState& leg1 =
      strategy.state.sideEma[1];
  leg0.emaReady = true;
  leg0.emaSamples = 2u;
  leg0.lastEmaTs = nowMono;
  leg0.emaWarmupStartMono = strategy.state.emaWarmupStartMono;
  leg0.signalReady = true;
  leg0.signalReadyUntilMono = timeNs(nowMono.raw + windowNs);
  leg1 = leg0;
  leg0.bidEmaRaw = quote.leg0BidRaw;
  leg0.askEmaRaw = quote.leg0AskRaw;
  leg1.bidEmaRaw = quote.leg1BidRaw;
  leg1.askEmaRaw = quote.leg1AskRaw;
  leg0.buyThenSellSpreadPenaltyEmaBpsE8 = 0ll;
  leg0.sellThenBuySpreadPenaltyEmaBpsE8 = 0ll;
  leg1.buyThenSellSpreadPenaltyEmaBpsE8 = 0ll;
  leg1.sellThenBuySpreadPenaltyEmaBpsE8 = 0ll;
  if (deviationBpsE8 >= 0ll) {
    const std::int64_t fair01 =
        quote.buyLeg0SellLeg1BpsE8 - deviationBpsE8;
    leg1.bidEmaRaw = priceFromBps(leg0.askEmaRaw, fair01);
  } else {
    const std::int64_t fair10 =
        quote.buyLeg1SellLeg0BpsE8 + deviationBpsE8;
    leg0.bidEmaRaw = priceFromBps(leg1.askEmaRaw, fair10);
  }
}

[[nodiscard]] StatArbEdgeMakerDesiredSlot makeEntryDesired(
    StatArbEdgeMakerStrategy& strategy,
    StrategyHarness& harness,
    Price desiredPrice) noexcept {
  const auto leg0 = trading_core::strategy::marketData(
      harness.ctx, trading_core::strategy::leg(0u));
  const auto leg1 = trading_core::strategy::marketData(
      harness.ctx, trading_core::strategy::leg(1u));
  const trading_core::TopOfBookPrices top0 =
      trading_core::strategy::topOfBook(leg0);
  const trading_core::TopOfBookPrices top1 =
      trading_core::strategy::topOfBook(leg1);
  const trading_core::strategy::OrderSizingResult pairQty =
      strategy.entryPairQty(harness.ctx, desiredPrice, top1.bid);
  return strategy.makeEntryDesired(
      0u,
      0u,
      StatArbEdgeMakerDirection::BuyLeg0SellLeg1,
      Side::Buy(),
      desiredPrice,
      pairQty);
}

[[nodiscard]] bool makeFillEvent(
    Id clientIntentId,
    Amount lastFillQty,
    Amount cumulativeFillQty,
    canon::OrderStatus status,
    bool includeCumulativeFill,
    const char* execId,
    cxet::composite::UserDataEvent& out) noexcept {
  out = cxet::composite::UserDataEvent{};
  out.kind = cxet::composite::UserDataEventKind::Order;
  if (!hft_backtest::detail::makeBacktestClientOrderId(
          canon::kExchangeIdBinance, clientIntentId, out.clientOrderId)) {
    return false;
  }
  out.orderStatus = status;
  out.side = Side::Buy();
  out.lastFilledQty = lastFillQty;
  out.filledQty = cumulativeFillQty;
  copyText(out.execId, execId);
  out.maker = 1u;
  using Field = cxet::composite::UserDataField;
  cxet::composite::markUserDataFieldPresent(out, Field::ClientOrderId);
  cxet::composite::markUserDataFieldPresent(out, Field::ExecId);
  cxet::composite::markUserDataFieldPresent(out, Field::OrderStatus);
  cxet::composite::markUserDataFieldPresent(out, Field::Side);
  cxet::composite::markUserDataFieldPresent(out, Field::LastFilledQty);
  if (includeCumulativeFill) {
    cxet::composite::markUserDataFieldPresent(out, Field::FilledQty);
  }
  cxet::composite::markUserDataFieldPresent(out, Field::ReduceOnly);
  cxet::composite::markUserDataFieldPresent(out, Field::Maker);
  return true;
}

void attachFillEvent(StrategyHarness& harness,
                     cxet::composite::UserDataEvent& event) noexcept {
  using Semantic = cxet::composite::UserDataSemantic;
  harness.ctx.userEvent = &event;
  harness.ctx.userEventAdmittedSemanticMask =
      cxet::composite::userDataSemanticBit(Semantic::OrderTransition);
  if (cxet::composite::userDataFieldPresent(
          event, cxet::composite::UserDataField::LastFilledQty) &&
      event.lastFilledQty.raw > 0) {
    harness.ctx.userEventAdmittedSemanticMask |=
        cxet::composite::userDataSemanticBit(Semantic::FillExecution);
  }
  harness.ctx.hasUserEvent = true;
  harness.ctx.hasEventLeg = true;
  harness.ctx.eventLegIndex = 0u;
  harness.ctx.userEventRecvMono = harness.ctx.nowMono;
  harness.ctx.userEventRecvUtc = harness.ctx.nowUtc;
  harness.ctx.io.clear();
}

[[nodiscard]] std::size_t countHedges(const StrategyHarness& harness,
                                      Amount qty) noexcept {
  std::size_t count = 0u;
  for (std::size_t i = 0u; i < harness.ctx.io.orderCount; ++i) {
    const trading_core::OrderIntent& order = harness.orders[i];
    if (order.routeHandle.raw == 2u &&
        order.orderType == canon::OrderType::Market &&
        order.side.raw == Side::Sell().raw &&
        !trading_core::positionReductionRequested(order.positionReduction) &&
        order.qty.raw == qty.raw) {
      ++count;
    }
  }
  return count;
}

[[nodiscard]] bool testMissingOwnerMovesToLateJournalWhileExposureBlocksReplacement() {
  StatArbEdgeMakerStrategy strategy = makeStrategy();
  StrategyHarness harness{};
  harness.setTickers(1000000000ull,
                     10000000000ll,
                     10010000000ll,
                     10200000000ll,
                     10210000000ll);
  setReadyEmaForDeviation(
      strategy, currentQuote(harness), bps(20ll), harness.ctx.nowMono);
  if (!expect(strategy.run(harness.ctx),
              "initial strategy cycle must emit the maker quote") ||
      !expect(harness.ctx.io.orderCount > 0u,
              "initial strategy cycle must expose an entry order")) {
    return false;
  }

  const Id oldId = harness.orders[0].clientIntentId;
  const Price oldPrice = harness.orders[0].price;
  const Amount oldQty = harness.orders[0].qty;
  const auto oldRole = strategy.state.entrySlots[0].role;
  const auto oldDirection = strategy.state.entrySlots[0].direction;
  harness.installLiveLimitOrdersFromOutput();

  const Amount priorFillQty = amount(oldQty.raw / 4ll);
  harness.setTickers(1100000000ull,
                     10000000000ll,
                     10010000000ll,
                     10200000000ll,
                     10210000000ll);
  setReadyEmaForDeviation(
      strategy, currentQuote(harness), bps(20ll), harness.ctx.nowMono);
  cxet::composite::UserDataEvent priorFill{};
  if (!expect(makeFillEvent(oldId,
                            priorFillQty,
                            priorFillQty,
                            canon::OrderStatus::PartiallyFilled,
                            true,
                            "backtest-old-owner-seen-fill-1",
                            priorFill),
              "initial partial fill client id must be representable")) {
    return false;
  }
  attachFillEvent(harness, priorFill);
  if (!expect(strategy.run(harness.ctx),
              "initial old-owner partial fill must be handled") ||
      !expect(harness.ctx.io.orderCount == 1u &&
                  countHedges(harness, priorFillQty) == 1u,
              "initial old-owner partial fill must emit one hedge") ||
      !expect(strategy.state.aggregateQty.raw == priorFillQty.raw &&
                  strategy.state.entrySlots[0].cumulativeFilled.raw ==
                      priorFillQty.raw,
              "initial fill must establish monotonic cumulative accounting")) {
    return false;
  }
  const Amount oldCumulative =
      strategy.state.entrySlots[0].cumulativeFilled;

  harness.setTickers(1200000000ull,
                     10000000000ll,
                     10010000000ll,
                     10200000000ll,
                     10210000000ll);
  setReadyEmaForDeviation(
      strategy, currentQuote(harness), bps(20ll), harness.ctx.nowMono);

  const StatArbEdgeMakerDesiredSlot desired = makeEntryDesired(
      strategy,
      harness,
      price(oldPrice.raw - harness.instruments[0].tickSizeRaw));
  // The partial fill establishes aggregate exposure, so current strategy
  // policy deliberately blocks a replacement entry.  The stale live quote
  // must still be cancelled while its original ownership is retained.
  if (!expect(!desired.valid,
              "partial exposure blocks a replacement entry quote") ||
      !expect(strategy.ensureSlot(
                  harness.ctx,
                  strategy.state.entrySlots[0],
                  desired,
                  true,
                  0u),
              "live mismatch must produce a conservative cancel") ||
      !expect(harness.ctx.io.cancelCount == 1u &&
                  harness.ctx.io.orderCount == 0u &&
                  harness.ctx.io.commandCount == 0u,
              "live mismatch must emit cancel only") ||
      !expect(strategy.state.entrySlots[0].clientIntentId.raw == oldId.raw &&
                  strategy.state.entrySlots[0].cancelPending,
              "cancel-pending slot must retain the old owner") ||
      !expect(trading_core::strategy::findLateOrder(
                  strategy.state.entryLateOrders[0],
                  oldId,
                  trading_core::strategy::route(0u)) == nullptr,
              "cancel emission must not journal a still-live owner")) {
    return false;
  }

  trading_core::StrategyOrderState* oldOrder = harness.findOrder(oldId);
  if (!expect(oldOrder != nullptr, "old runtime owner must remain visible")) {
    return false;
  }
  oldOrder->state = trading_core::StrategyOrderLocalState::Missing;
  oldOrder->missing = true;
  oldOrder->cancelPending = false;
  oldOrder->pendingClientIntentId = Id{};
  harness.ctx.io.clear();

  if (!expect(strategy.ensureSlot(
                  harness.ctx,
                  strategy.state.entrySlots[0],
                  desired,
                  true,
                  0u),
              "terminal missing owner must be released next cycle") ||
      !expect(harness.ctx.io.cancelCount == 0u &&
                  harness.ctx.io.orderCount == 0u &&
                  harness.ctx.io.commandCount == 0u,
              "partial exposure must prevent an independent replacement order")) {
    return false;
  }

  const auto* late = trading_core::strategy::findLateOrder(
      strategy.state.entryLateOrders[0],
      oldId,
      trading_core::strategy::route(0u));
  if (!expect(strategy.state.entrySlots[0].clientIntentId.raw == 0u,
              "entry slot stays empty while aggregate exposure is open") ||
      !expect(late != nullptr, "old owner must move to the late journal") ||
      !expect(late && late->qty.raw == oldQty.raw &&
                  late->cumulativeFilled.raw == oldCumulative.raw &&
                  late->payload.role == oldRole &&
                  late->payload.direction == oldDirection &&
                  late->side.raw == Side::Buy().raw &&
                  trading_core::strategy::lateOrderSeenExecId(
                      *late, priorFill.execId),
              "late journal must preserve old owner accounting identity")) {
    return false;
  }

  const Amount lateFillQty = amount(oldQty.raw / 2ll);
  const Amount finalCumulative =
      amount(priorFillQty.raw + lateFillQty.raw);
  harness.setTickers(1300000000ull,
                     10000000000ll,
                     10010000000ll,
                     10200000000ll,
                     10210000000ll);
  setReadyEmaForDeviation(
      strategy, currentQuote(harness), bps(20ll), harness.ctx.nowMono);
  cxet::composite::UserDataEvent fill{};
  if (!expect(makeFillEvent(oldId,
                            lateFillQty,
                            finalCumulative,
                            canon::OrderStatus::PartiallyFilled,
                            true,
                            "backtest-old-owner-fill-2",
                            fill),
              "late cumulative fill client id must be representable")) {
    return false;
  }
  attachFillEvent(harness, fill);
  if (!expect(strategy.run(harness.ctx),
              "reordered old-owner cumulative fill must be handled") ||
      !expect(harness.ctx.io.orderCount == 1u &&
                  countHedges(harness, lateFillQty) == 1u,
              "only the late cumulative delta must emit one opposite-leg hedge") ||
      !expect(strategy.state.aggregateQty.raw == finalCumulative.raw,
              "late cumulative delta must be accounted exactly once") ||
      !expect(trading_core::strategy::findLateOrder(
                  strategy.state.entryLateOrders[0],
                  oldId,
                  trading_core::strategy::route(0u)) != nullptr,
              "non-terminal cumulative fill must retain its journal entry") ||
      !expect(strategy.state.entrySlots[0].clientIntentId.raw == 0u,
              "late fill must not reopen the blocked entry slot")) {
    return false;
  }

  harness.setTickers(1400000000ull,
                     10000000000ll,
                     10010000000ll,
                     10200000000ll,
                     10210000000ll);
  setReadyEmaForDeviation(
      strategy, currentQuote(harness), bps(20ll), harness.ctx.nowMono);
  cxet::composite::UserDataEvent seenLastFillOnly{};
  if (!expect(makeFillEvent(oldId,
                            priorFillQty,
                            Amount{},
                            canon::OrderStatus::PartiallyFilled,
                            false,
                            "backtest-old-owner-seen-fill-1",
                            seenLastFillOnly),
              "seen LastFilledQty event must be representable")) {
    return false;
  }
  attachFillEvent(harness, seenLastFillOnly);
  if (!expect(strategy.run(harness.ctx),
              "already-seen LastFilledQty event must be consumed") ||
      !expect(harness.ctx.io.orderCount == 0u,
              "already-seen LastFilledQty exec id must not re-hedge") ||
      !expect(strategy.state.aggregateQty.raw == finalCumulative.raw,
              "already-seen LastFilledQty exec id must not re-account") ||
      !expect(([&]() noexcept {
                  const auto* lateAfterDelta =
                      trading_core::strategy::findLateOrder(
                          strategy.state.entryLateOrders[0],
                          oldId,
                          trading_core::strategy::route(0u));
                  return lateAfterDelta != nullptr &&
                      lateAfterDelta->cumulativeFilled.raw ==
                          finalCumulative.raw;
                })(),
              "duplicate must retain monotonic late cumulative accounting") ||
      !expect(strategy.state.entrySlots[0].clientIntentId.raw == 0u,
              "duplicate must not reopen the blocked entry slot")) {
    return false;
  }

  harness.setTickers(1500000000ull,
                     10000000000ll,
                     10010000000ll,
                     10200000000ll,
                     10210000000ll);
  setReadyEmaForDeviation(
      strategy, currentQuote(harness), bps(20ll), harness.ctx.nowMono);
  const Amount terminalFillQty =
      amount(oldQty.raw - finalCumulative.raw);
  cxet::composite::UserDataEvent terminal{};
  if (!expect(makeFillEvent(oldId,
                            terminalFillQty,
                            oldQty,
                            canon::OrderStatus::Closed,
                            true,
                            "backtest-old-owner-terminal-3",
                            terminal),
              "terminal old-owner event must be representable")) {
    return false;
  }
  attachFillEvent(harness, terminal);
  if (!expect(strategy.run(harness.ctx),
              "terminal old-owner event must retire the late journal") ||
      !expect(harness.ctx.io.orderCount == 1u &&
                  countHedges(harness, terminalFillQty) == 1u,
              "terminal event must hedge only its remaining cumulative delta") ||
      !expect(strategy.state.aggregateQty.raw == oldQty.raw,
              "terminal cumulative delta must complete accounting exactly once") ||
      !expect(trading_core::strategy::findLateOrder(
                  strategy.state.entryLateOrders[0],
                  oldId,
                  trading_core::strategy::route(0u)) == nullptr,
              "terminal old-owner event must clear its journal entry") ||
      !expect(strategy.state.entrySlots[0].clientIntentId.raw == 0u,
              "terminal old-owner event must leave the blocked entry slot empty")) {
    return false;
  }

  harness.setTickers(1600000000ull,
                     10000000000ll,
                     10010000000ll,
                     10200000000ll,
                     10210000000ll);
  setReadyEmaForDeviation(
      strategy, currentQuote(harness), bps(20ll), harness.ctx.nowMono);
  attachFillEvent(harness, terminal);
  return expect(strategy.run(harness.ctx),
                "duplicate old-owner fill after retirement must be consumed") &&
      expect(harness.ctx.io.orderCount == 0u &&
                 countHedges(harness, terminalFillQty) == 0u,
             "retired duplicate exec id must not emit a second hedge") &&
      expect(strategy.state.aggregateQty.raw == oldQty.raw,
             "retired duplicate exec id must not change aggregate accounting") &&
      expect(strategy.state.entrySlots[0].clientIntentId.raw == 0u,
             "retired duplicate fill must leave the entry slot empty");
}

}  // namespace

int main() {
  return testMissingOwnerMovesToLateJournalWhileExposureBlocksReplacement()
      ? 0
      : 1;
}
