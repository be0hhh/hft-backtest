#include "capital/Sizing/PreparePairLotsSizing.hpp"
#include "BacktestCanonicalPairAdapter.hpp"
#include "BacktestCanonicalPairSelection.hpp"
#include "../../Execution/BacktestOrderEvidence.hpp"
#include "trading_core/Strategy/Orders/Evidence/OrderDecisionCursor.hpp"
#include "trading_core/Strategy/StrategyMarketData.hpp"

#include <algorithm>
#include <limits>

namespace hft_backtest {
using namespace trading_core;
using namespace trading_core::strategy;
namespace {

bool sourceReady(const detail::EngineState& engine) noexcept {
  return !engine.simulationEvidenceFailed && !engine.liquidated && !engine.riskStopped &&
      (!engine.capturedSourceManaged || (engine.capturedSourceMember &&
       !(engine.capturedExecutionRequiredChannels & ~engine.capturedActiveChannels) &&
       !(engine.capturedExecutionRequiredChannels & engine.capturedUnhealthyChannels)));
}
}

bool BacktestCanonicalPairAdapter::bindCold(const StrategyDescriptor& descriptor,
    const StrategyParamStorage& params, void* state,
    std::span<const StrategyInstrumentSpec> instruments,
    std::span<const BacktestCanonicalPairSource> sources,
    detail::EngineView engines, std::uint64_t nowUtc, std::string& error,
    const StrategyRuntimeConfiguration& configuration) noexcept {
  const auto refuse = [&](const char* reason) noexcept {
    try { error = reason; } catch (...) { error.clear(); }
    return false;
  };
  if (descriptor_ || !state || sources.size() != 2u || instruments.size() != 2u ||
      engines.size() != 2u || descriptor.stateDiscipline != StrategyStateDiscipline::CanonicalEvidence ||
      descriptor.footprint.stateScope != StrategyStateScope::Instrument ||
      !strategyDescriptorContractValid(descriptor) || !descriptor.runtimePolicy ||
      !descriptor.runtimePolicy->available || !descriptor.commitPreparedDispatch ||
      !descriptor.retainsExecutionIntent) return refuse("unsupported canonical pair adapter contract");
  auto reason = evaluateStrategyPairEligibility(descriptor.pairRequirements,
      sources[0].instrument, sources[1].instrument, nowUtc);
  if (reason != StrategyInstrumentEligibilityReason::None)
    return refuse(strategyInstrumentEligibilityReasonName(reason));
  for (std::size_t leg = 0u; leg < 2u; ++leg) {
    reason = evaluateStrategyInstrumentEligibility(descriptor.instrumentRequirements,
        sources[leg].instrument, nowUtc);
    if (reason != StrategyInstrumentEligibilityReason::None)
      return refuse(strategyInstrumentEligibilityReasonName(reason));
    const auto& sizing = sources[leg].sizing;
    const auto& instrument = instruments[leg];
    if (!sources[leg].feesKnown || !instrument.quoteCurrency.valid() ||
        !instrument_eligibility_detail::sameText(instrument.quoteCurrency.data,
            sources[leg].instrument.quoteAsset.data) ||
        !sizing.quoteCurrency || sizing.nativeLotBaseQtyRaw <= 0 ||
        sizing.normalizedBasePerLotRaw <= 0 || sizing.priceBasisQtyRaw <= 0 ||
        sizing.grid.stepSizeRaw != instrument.stepSizeRaw ||
        sizing.grid.executionQuantityStepRaw != instrument.executionQuantityStepRaw ||
        sizing.priceBasisQtyRaw != instrument.priceBasisQtyRaw ||
        sizing.nativeLotBaseQtyRaw != instrument.nativeLotBaseQtyRaw)
      return refuse("missing or conflicting captured pair sizing metadata");
    if (!detail::prepareSimulationOrderEvidence(engines[leg]))
      return refuse("virtual canonical order evidence unavailable");
    if (!detail::knownAccountingModel(engines[leg]) ||
        (sources[leg].instrument.kind == cxet::api::InstrumentKind::Spot) != detail::isCashAccounting(engines[leg]))
      return refuse("captured instrument kind does not prove the virtual accounting model");
    if (detail::isLinearAccounting(engines[leg]) &&
        (!sizing.capitalRequirementKnown || sizing.capitalCurrency != sizing.quoteCurrency ||
         !detail::configureSimulationDerivativeCollateral(engines[leg], sizing.initialCapitalRaw,
             sizing.capitalBasisQtyRaw, sizing.capitalCurrency)))
      return refuse("captured derivative initial-margin collateral authority unavailable");
  }
  if (engines[0].activeContextIndex != engines[1].activeContextIndex)
    return refuse("canonical pair context identity mismatch");
  if (!descriptor.runtimePolicy->validateCold(configuration, error) ||
      !descriptor.runtimePolicy->readParameters(params, parameters_) ||
      !descriptor.runtimePolicy->validatePairSizingCold(parameters_.sizing,
          descriptor.runtimePolicy->sizingPolicy == PairSizingPolicy::ConfiguredLots, error)) return false;
  if (sources[0].sizing.quoteCurrency != parameters_.sizing.quoteCurrency ||
      sources[1].sizing.quoteCurrency != parameters_.sizing.quoteCurrency)
    return refuse("captured quote identity differs from pair sizing currency");
  if (descriptor.runtimePolicy->requiredQuoteCurrency.size() &&
      descriptor.runtimePolicy->requiredQuoteCurrency != cxet::StringView::fromCString(
          sources[0].instrument.quoteAsset.data, sizeof(sources[0].instrument.quoteAsset.data)))
    return refuse("captured quote currency is unsupported by the selected strategy policy");
  std::array<capital::sizing::PairSizingLeg, 2u> metadata{sources[0].sizing, sources[1].sizing};
  if (descriptor.runtimePolicy->sizingPolicy == PairSizingPolicy::ConfiguredLots ||
      descriptor.runtimePolicy->alwaysPrepareLots) {
    preparedLots_ = capital::sizing::preparePairLotsSizingCold(parameters_.sizing, metadata, {1u, 1u});
    if (preparedLots_.status != capital::sizing::PairSizingStatus::Ok)
      return refuse("captured native lot metadata cannot prepare exact pair sizing");
  }
  if (!pool_.bindCold(poolRows_, 8u)) return refuse("canonical pair execution pool unavailable");
  retained_ = {state, descriptor.footprint.stateBytes, false, nullptr};
  // The runtime initialized this stable descriptor-owned object. Bind it
  // through the public retained-state seam, never through actor layout offsets.
  if (!instance_.bindInstrumentCold(descriptor, params, instruments.first(1u), {}, {&retained_, 1u}))
    return refuse("canonical strategy instance binding failed");
  std::array<StrategyOrderEvidenceView, 2u> views{
      detail::simulationOrderEvidenceView(engines[0]), detail::simulationOrderEvidenceView(engines[1])};
  if (!instance_.bindExecutionContextCold(0u,
      {views, 1u, std::numeric_limits<std::uint64_t>::max(), &pool_}) ||
      !descriptor.runtimePolicy->configureCold(state, {}))
    return refuse("canonical strategy execution binding failed");
  sources_ = {sources[0], sources[1]};
  params_ = params;
  contextIndex_ = engines[0].activeContextIndex;
  descriptor_ = &descriptor;
  dispatchCommitted_ = true;
  return true;
}

bool BacktestCanonicalPairAdapter::prepareCycle(StrategyContext& context,
    detail::EngineView engines, const BacktestCanonicalPairReferences& references,
    PairStrategyInputs& input) noexcept {
  if (!descriptor_ || engines.size() != 2u || cyclePrepared_ ||
      context.state.orders.executionLegCount != 2u || !context.nowMono.raw ||
      (descriptor_->runtimePolicy->selection.enabled() && !selection_)) return false;
  input = {};
  input.sizingPolicy = descriptor_->runtimePolicy->sizingPolicy;
  input.openingSignalPolicy = descriptor_->runtimePolicy->openingSignalPolicy;
  if (preparedLots_.status == capital::sizing::PairSizingStatus::Ok) input.preparedLots = &preparedLots_;
  bool financialAuthorityCurrent = true;
  for (std::size_t leg = 0u; leg < 2u; ++leg) {
    auto& engine = engines[leg];
    if (!engine.feePolicyKnown) financialAuthorityCurrent = false;
    if (detail::isLinearAccounting(engine) && !engine.simulationOrders->collateral.authorityCurrent)
      financialAuthorityCurrent = false;
    const auto& view = context.state.orders.executionLegs[leg];
    if (engine.activeContextIndex != contextIndex_ || view.contextIndex != contextIndex_ ||
        !view.store || view.store->reconciliationRequired() || !view.store->connected()) return false;
    auto& gate = input.execution.legs[leg];
    gate.binding = view.binding; gate.owner = view.owner; gate.generation = view.expectedGeneration;
    gate.epoch = view.epoch; gate.contextIndex = view.contextIndex;
    // Account truth remains the simulator's delivered account cut. Source
    // pauses block placement, while canonical observation remains available.
    gate.accountReady = !engine.simulationEvidenceFailed && engine.position.valid && detail::knownAccountingModel(engine) &&
        (detail::isCashAccounting(engine) || engine.simulationOrders->collateral.known);
    gate.orderReady = sourceReady(engine); gate.cancelReady = sourceReady(engine);
    const auto& quote = detail::effectiveBookTicker(engine.snapshot);
    gate.buyPrice = quote.ask.px; gate.sellPrice = quote.bid.px;
    const auto received = engine.snapshot.hasReconstructedBbo.raw ? engine.snapshot.reconstructedBookTickerRecvMono :
        engine.snapshot.lastBookTickerRecvMono;
    gate.orderReady = gate.orderReady && gate.buyPrice.raw > 0 && gate.sellPrice.raw > 0 &&
        gate.sellPrice.raw <= gate.buyPrice.raw && received.raw && received.raw <= context.nowMono.raw &&
        (context.tradableLegMask & (std::uint64_t{1u} << leg));
    // This stamps the current validated executable cut, not quote receipt.
    // Receipt/freshness remains in the snapshot and the caller's source mask.
    if (gate.orderReady) gate.priceMono = context.nowMono;
    input.sizingLegs[leg] = sources_[leg].sizing;
    input.sizingLegs[leg].sizingPrice = descriptor_->runtimePolicy->openingSides[leg].raw == Side::Buy().raw
        ? gate.buyPrice : gate.sellPrice;
  }
  // A private callback still services retained evidence while membership or
  // instrument facts are withdrawn. It cannot begin a fresh pair from the
  // cold-bound historical GO after the visible financial cut has changed.
  context.allowNewEntry = context.allowNewEntry && financialAuthorityCurrent;
  input.execution.dispatch = {feedback_.data(), feedbackCount_};
  if (descriptor_->runtimePolicy->requiresPairSignal) {
    if (!references.delivery || !references.centralBank || !references.sourceIds[0] ||
        !references.sourceIds[1]) return false;
    DeliveryValuationInput valuation{};
    for (std::size_t leg = 0u; leg < 2u; ++leg) {
      const auto& snapshot = engines[leg].snapshot;
      const auto& quote = detail::effectiveBookTicker(snapshot);
      auto& value = leg == 0u ? valuation.spot : valuation.future;
      value.bidPriceRaw = quote.bid.px.raw; value.askPriceRaw = quote.ask.px.raw;
      value.priceBasisQtyRaw = static_cast<std::uint64_t>(sources_[leg].sizing.priceBasisQtyRaw);
      value.valid = sourceReady(engines[leg]) && value.bidPriceRaw > 0 && value.askPriceRaw > 0 &&
          value.bidPriceRaw <= value.askPriceRaw;
      value.priceBasisPresent = value.bidPresent = value.askPresent = true;
    }
    valuation.reference = references.delivery; valuation.centralBank = references.centralBank;
    valuation.spotSourceId = references.sourceIds[0]; valuation.futureSourceId = references.sourceIds[1];
    const auto quoteUtc = [&](std::size_t leg) noexcept {
      if (references.quoteReceiveUtc[leg].raw) return references.quoteReceiveUtc[leg].raw;
      const auto& snapshot = engines[leg].snapshot;
      if (snapshot.hasReconstructedBbo.raw && snapshot.reconstructedBookTickerRecvMono.raw !=
          snapshot.lastBookTickerRecvMono.raw) return std::uint64_t{};
      return snapshot.lastBookTickerRecvUtc.raw;
    };
    const auto firstUtc = quoteUtc(0u), secondUtc = quoteUtc(1u);
    if (firstUtc > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        secondUtc > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return false;
    valuation.sourceATimestampNs = static_cast<std::int64_t>(firstUtc);
    valuation.sourceBTimestampNs = static_cast<std::int64_t>(secondUtc);
    valuation.projectionRealtimeNs = context.nowUtc.raw; valuation.buySpot = true;
    const auto result = evaluateDeliveryValuation(valuation);
    const bool known = result.boundaryValid &&
        result.centralBankRateState == DeliveryValuationCentralBankState::Ready;
    input.signal.entry = {result.centralBankSumPpm, known};
    std::int64_t exit{};
    input.signal.exit = {exit, false};
    if (known && calculateCentralBankSumPpm(result.reverseMarketBasisPpm,
        result.centralBankCarryPpm, &exit)) input.signal.exit = {exit, true};
  }
  if (context.hasUserEvent && context.userEvent && context.userEventClientIntentId.raw &&
      context.eventLegIndex < 2u && descriptor_->runtimePolicy->observePrivateFill &&
      context.userEvent->lastFilledQty.raw > 0) {
    const auto leg = context.eventLegIndex;
    const auto& view = context.state.orders.executionLegs[leg];
    const auto* identity = canonicalOrderEvidenceIdentity(view, context.userEventClientIntentId,
        context.userEventGeneration);
    if (!identity || !identity->record || context.userEvent->lastFilledQty.raw > identity->record->cumulativeQtyE8)
      return false;
    const auto slot = static_cast<std::size_t>(identity - engines[leg].simulationOrders->identities.data());
    if (slot >= detail::kSimulationOrderEvidenceCapacity) return false;
    const auto cut = engines[leg].simulationOrders->deliveredCuts[slot];
    if (!cut) return false;
    if (cut > privateFillCuts_[leg] && !descriptor_->runtimePolicy->observePrivateFill(retained_.state,
            {static_cast<std::uint8_t>(leg), context.userEvent->side,
             context.userEvent->lastFilledQty.raw, context.nowMono.raw})) return false;
    privateFillCuts_[leg] = std::max(privateFillCuts_[leg], cut);
    if (identity->record->terminalQuantityFinal && identity->record->terminal() &&
        identity->record->cumulativeQtyE8 == identity->record->request.qtyE8 &&
        identity->commandId > finalFillCommands_[leg]) {
      finalFillCommands_[leg] = identity->commandId;
      if (filledOrders_ != std::numeric_limits<std::uint32_t>::max()) ++filledOrders_;
    }
  }
  if (selection_) {
    const bool eligible = input.execution.legs[0].accountReady && input.execution.legs[1].accountReady &&
        input.execution.legs[0].orderReady && input.execution.legs[1].orderReady && financialAuthorityCurrent;
    (void)selection_->observe(selectionIndex_, params_, input, context, eligible);
    context.allowNewEntry = context.allowNewEntry && selection_->allows(selectionIndex_);
  }
  if (descriptor_->runtimePolicy->updateHealth) {
    StrategyActorSnapshot snapshot{};
    if (descriptor_->runtimePolicy->snapshot) descriptor_->runtimePolicy->snapshot(retained_.state, snapshot);
    const bool fullReady = input.execution.legs[0].accountReady && input.execution.legs[1].accountReady;
    const bool healthy = sourceReady(engines[0]) && sourceReady(engines[1]);
    const bool unknown = snapshot.status == StrategyActorStatus::Unknown;
    const bool flat = snapshot.exposure[0] == 0 && snapshot.exposure[1] == 0;
    result_ = descriptor_->runtimePolicy->updateHealth(retained_.state,
        {fullReady, healthy, flat, unknown, false,
         !descriptor_->hasExecutionExposure(retained_.state), filledOrders_, context.nowMono.raw});
  }
  cyclePrepared_ = true; dispatchCommitted_ = false;
  return true;
}

bool BacktestCanonicalPairAdapter::decideAndExecute(StrategyContext& context) noexcept {
  if (!descriptor_ || !cyclePrepared_ || !context.pairInputs) return false;
  const auto expected = context.io;
  const bool produced = instance_.runCycle(0u, context);
  if (produced && descriptor_->executeCycle) (void)instance_.executeCycle(0u, context);
  if (context.io.orderOut.data() != expected.orderOut.data() ||
      context.io.cancelOut.data() != expected.cancelOut.data() ||
      context.io.commandOut.data() != expected.commandOut.data() ||
      context.io.transactionOut.data() != expected.transactionOut.data() ||
      context.io.capacityExceeded || context.io.transactionCount || context.io.commandCount ||
      !StrategyInstance::directCapabilitiesDeclared(*descriptor_, context.io)) return false;
  // The actor has consumed the previous feedback; new dispatch replaces it.
  feedbackCount_ = 0u;
  return true;
}

bool BacktestCanonicalPairAdapter::dispatchAndCommit(StrategyContext& context,
    detail::EngineView engines, std::int64_t tsNs) noexcept {
  if (!descriptor_ || !cyclePrepared_ || engines.size() != 2u ||
      context.io.orderCount + context.io.cancelCount > feedback_.size()) return false;
  const auto submit = [&](std::size_t leg, Id local, Id target, bool cancel,
                          const StrategyIo& io) noexcept {
    if (leg >= 2u) return false;
    auto& engine = engines[leg];
    detail::applyIntents(engine, io, tsNs, engine.latency);
    if (engine.simulationEvidenceFailed) return false;
    const auto view = detail::simulationOrderEvidenceView(engine);
    const auto* identity = canonicalOrderEvidenceIdentity(view, target, view.expectedGeneration);
    PairDispatchObservation observation{local, ExecutionRouteHandle{static_cast<std::uint32_t>(leg + 1u)},
        view.binding, view.owner, view.expectedGeneration, 0u, view.contextIndex, {}, PairDispatchKind::ReleasedUnsent};
    if (identity && identity->record) {
      const auto& record = *identity->record;
      observation.commandId = cancel ? record.cancelCommandId : identity->commandId;
      observation.kind = PairDispatchKind::CommandBound;
      const bool attempted = cancel ? record.cancelSettled &&
          record.cancelSend != execution::SendDisposition::NotAttempted :
          record.placeSettled && record.placeSend != execution::SendDisposition::NotAttempted;
      if (attempted) { observation.kind = PairDispatchKind::AttemptTime; observation.attemptedMono = context.nowMono; }
    }
    feedback_[feedbackCount_++] = observation;
    return true;
  };
  // Preserve the canonical ordered cancellation/placement protocol used by
  // the simulator: cancels precede new orders at the same owner coordinate.
  for (std::size_t i = 0u; i < context.io.cancelCount; ++i) {
    auto& cancel = context.io.cancelOut[i];
    if (!cancel.routeHandle.raw || cancel.routeHandle.raw > 2u) return false;
    StrategyIo io{}; io.cancelOut = {&cancel, 1u}; io.cancelCount = 1u;
    if (!submit(cancel.routeHandle.raw - 1u, cancel.clientIntentId, cancel.targetClientIntentId, true, io)) return false;
  }
  for (std::size_t i = 0u; i < context.io.orderCount; ++i) {
    auto& order = context.io.orderOut[i];
    if (!order.routeHandle.raw || order.routeHandle.raw > 2u) return false;
    StrategyIo io{}; io.orderOut = {&order, 1u}; io.orderCount = 1u;
    if (!submit(order.routeHandle.raw - 1u, order.clientIntentId, order.clientIntentId, false, io)) return false;
  }
  if (!descriptor_->commitPreparedDispatch(retained_.state)) return false;
  for (std::size_t leg = 0u; leg < 2u; ++leg)
    consumedCuts_[leg] = engines[leg].simulationOrders->deliveryCut;
  cyclePrepared_ = false; dispatchCommitted_ = true;
  return true;
}

bool BacktestCanonicalPairAdapter::released(std::size_t leg, std::uint32_t context,
    Id id, std::uint64_t cut) const noexcept {
  if (!descriptor_ || leg >= 2u || context != contextIndex_ || cyclePrepared_ ||
      !dispatchCommitted_ || consumedCuts_[leg] < cut ||
      descriptor_->retainsExecutionIntent(retained_.state, id)) return false;
  for (std::size_t i = 0u; i < feedbackCount_; ++i)
    if (feedback_[i].route.raw == leg + 1u && feedback_[i].clientIntentId.raw == id.raw) return false;
  return true;
}

bool BacktestCanonicalPairAdapter::bindSelectionOwner(BacktestCanonicalPairSelection& selection,
    std::uint32_t index) noexcept {
  if (!descriptor_ || selection_ || !descriptor_->runtimePolicy->selection.enabled() ||
      selection.policy() != descriptor_->runtimePolicy) return false;
  selection_ = &selection; selectionIndex_ = index;
  return true;
}

TimeNs BacktestCanonicalPairAdapter::nextWakeMono() const noexcept {
  if (!descriptor_) return {};
  auto wake = descriptor_->nextWakeMono ? descriptor_->nextWakeMono(retained_.state) : TimeNs{};
  const auto selectionWake = selection_ ? selection_->nextWakeMono() : TimeNs{};
  if (selectionWake.raw && (!wake.raw || selectionWake.raw < wake.raw)) wake = selectionWake;
  return wake;
}
} // namespace hft_backtest
