#include "capital/Sizing/PreparePairLotsSizing.hpp"
#include "BacktestCanonicalPairAdapter.hpp"
#include "BacktestCanonicalPairSelection.hpp"
#include "../Inputs/BacktestStrategyInputFreshness.hpp"
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
       (engine.genericCommandInputAdmission ||
        (!(engine.capturedExecutionRequiredChannels & ~engine.capturedActiveChannels) &&
         !(engine.capturedExecutionRequiredChannels & engine.capturedUnhealthyChannels)))));
}
}

bool BacktestCanonicalExecutionAdapter::bindCold(const StrategyDescriptor& descriptor,
    const StrategyParamStorage& params, void* state,
    std::span<const StrategyInstrumentSpec> instruments,
    std::span<const BacktestCanonicalSource> sources,
    detail::EngineView engines, std::uint64_t nowUtc, std::string& error,
    const StrategyRuntimeConfiguration& configuration) noexcept {
  const auto refuse = [&](const char* reason) noexcept {
    try { error = reason; } catch (...) { error.clear(); }
    return false;
  };
  if (descriptor.runtimePolicy && !descriptor.runtimePolicy->available) {
    // hasStrategyBudget is normalized to these flags by SimulationSession.
    // Account collateral admission does not implement a strategy budget.
    if (configuration.budgetConfigured || configuration.amountBudget)
      return refuse("configured virtualHost strategy budget is unsupported by the generic replay financial adapter");
    return bindGenericCold(descriptor, params, state, instruments, sources, engines, nowUtc, error);
  }
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
  try { pairResources_ = std::make_unique<PairResources>(); }
  catch (...) { return refuse("canonical pair execution pool storage unavailable"); }
  if (!pairResources_->pool.bindCold(pairResources_->rows, 8u)) return refuse("canonical pair execution pool unavailable");
  retained_ = {state, descriptor.footprint.stateBytes, false, nullptr};
  // The runtime initialized this stable descriptor-owned object. Bind it
  // through the public retained-state seam, never through actor layout offsets.
  if (!instance_.bindInstrumentCold(descriptor, params, instruments.first(1u), {}, {&retained_, 1u}))
    return refuse("canonical strategy instance binding failed");
  std::array<StrategyOrderEvidenceView, 2u> views{
      detail::simulationOrderEvidenceView(engines[0]), detail::simulationOrderEvidenceView(engines[1])};
  if (!instance_.bindExecutionContextCold(0u,
      {views, 1u, std::numeric_limits<std::uint64_t>::max(), &pairResources_->pool}) ||
      !descriptor.runtimePolicy->configureCold(state, {}))
    return refuse("canonical strategy execution binding failed");
  try {
    sources_.assign(sources.begin(), sources.end());
    gates_.resize(2u); consumedCuts_.resize(2u); inputViews_.resize(2u); feeSchedules_.resize(2u);
    feedback_.resize(8u);
  } catch (...) { return refuse("canonical replay cold storage unavailable"); }
  params_ = params;
  contextIndex_ = engines[0].activeContextIndex;
  descriptor_ = &descriptor;
  feeQualificationMono_ = nowUtc;
  pairPolicy_ = true;
  dispatchCommitted_ = true;
  return true;
}

bool BacktestCanonicalExecutionAdapter::bindGenericCold(const StrategyDescriptor& descriptor,
    const StrategyParamStorage& params, void* state,
    std::span<const StrategyInstrumentSpec> instruments,
    std::span<const BacktestCanonicalSource> sources, detail::EngineView engines,
    std::uint64_t nowUtc, std::string& error) noexcept {
  const auto refuse = [&](const char* reason) noexcept {
    try { error = reason; } catch (...) { error.clear(); }
    return false;
  };
  if (descriptor_ || !state || !strategyDescriptorContractValid(descriptor) ||
      !strategyDescriptorHasCanonicalExecution(descriptor) || !descriptor.bindExecutionCold ||
      !descriptor.commitPreparedDispatch || !descriptor.retainsExecutionIntent ||
      sources.empty() || sources.size() != instruments.size() || sources.size() != engines.size() ||
      sources.size() < descriptor.minLegs || sources.size() > descriptor.maxLegs)
    return refuse("unsupported canonical execution adapter contract");
  for (std::size_t leg = 0u; leg < descriptor.legInputRequirementCount; ++leg)
    if (descriptor.legInputRequirements[leg].required.raw &
        (kStrategyInputPriceLimit.raw | kStrategyInputFunding.raw | kStrategyInputCandle.raw))
      return refuse("required scalar/candle replay input has no recorded causal adapter");
  try {
    sources_.assign(sources.begin(), sources.end());
    legs_.resize(sources.size()); gates_.resize(sources.size());
    inputViews_.resize(sources.size()); feeSchedules_.resize(sources.size());
    consumedCuts_.resize(sources.size()); slots_.resize(descriptor.executionSlotCapacity);
    feedback_.resize(descriptor.footprint.maxOrders + descriptor.footprint.maxCancels +
        2u * strategyRuntimeCommandCapacity(descriptor.footprint));
    commandResults_.resize(strategyRuntimeCommandCapacity(descriptor.footprint));
  } catch (...) { return refuse("canonical replay cold storage unavailable"); }
  std::vector<StrategyOrderEvidenceView> views;
  try { views.resize(sources.size()); } catch (...) { return refuse("canonical replay evidence views unavailable"); }
  contextIndex_ = engines[0].activeContextIndex;
  for (std::size_t leg = 0u; leg < engines.size(); ++leg) {
    auto& engine = engines[leg];
    const auto reason = evaluateStrategyInstrumentEligibility(descriptor.instrumentRequirements,
        sources[leg].instrument, nowUtc);
    if (reason != StrategyInstrumentEligibilityReason::None)
      return refuse(strategyInstrumentEligibilityReasonName(reason));
    if (engine.activeContextIndex != contextIndex_ || !sources[leg].feesKnown ||
        !detail::knownAccountingModel(engine) || !detail::qualifySimulationGenericExecutionCold(engine))
      return refuse("canonical simulation account evidence unavailable");
    if (detail::isLinearAccounting(engine)) {
      const auto& sizing = sources[leg].sizing;
      if (!sizing.capitalRequirementKnown || sizing.capitalCurrency != sizing.quoteCurrency ||
          !detail::configureSimulationDerivativeCollateral(engine, sizing.initialCapitalRaw,
              sizing.capitalBasisQtyRaw, sizing.capitalCurrency))
        return refuse("captured derivative initial-margin collateral authority unavailable");
    }
    views[leg] = detail::simulationOrderEvidenceView(engine);
    const auto& view = views[leg];
    legs_[leg] = {static_cast<std::uint32_t>(leg + 1u),
        ExecutionRouteHandle{static_cast<std::uint32_t>(leg + 1u)}, view.binding, view.owner,
        view.expectedGeneration, view.contextIndex};
  }
  if (!planStrategyExecutionSlotsCold(legs_,
          {descriptor.maxOrdersPerLeg.data(), legs_.size()}, slots_))
    return refuse("canonical strategy execution slots unavailable");
  retained_ = {state, descriptor.footprint.stateBytes, false, nullptr};
  if (!instance_.bindInstrumentCold(descriptor, params, instruments.first(1u), {}, {&retained_, 1u}) ||
      !instance_.bindExecutionContextCold(0u, {views, 1u,
          std::numeric_limits<std::uint64_t>::max(), nullptr, slots_, legs_}))
    return refuse("canonical strategy execution binding failed");
  params_ = params; descriptor_ = &descriptor; dispatchCommitted_ = true; feeQualificationMono_ = nowUtc;
  marketMaximumAgeNs_ = resolveBacktestStrategyInputPolicy(descriptor, params).maxAgeNs;
  return true;
}

bool BacktestCanonicalExecutionAdapter::prepareCycle(StrategyContext& context,
    detail::EngineView engines, const BacktestCanonicalPairReferences& references,
    PairStrategyInputs& input) noexcept {
  if (!descriptor_ || engines.size() != sources_.size()) return false;
  for (std::size_t leg = 0u; leg < engines.size(); ++leg) {
    const auto& engine = engines[leg];
    auto& view = inputViews_[leg]; view = {};
    view.legId = static_cast<std::uint32_t>(leg + 1u);
    view.market = context.marketData.legs && leg < context.marketData.count
        ? &context.marketData.legs[leg] : nullptr;
    view.candle = context.candles.get(leg);
    const auto evidence = [&](cxet::api::market::PublicMarketDataStream stream) noexcept {
      auto value = engine.strategySourceEvidence[cxet::api::market::publicMarketDataStreamIndex(stream)];
      if (engine.capturedSourceManaged && (!engine.capturedSourceMember ||
          value.sourceGeneration != engine.capturedSourceGeneration)) value.available = false;
      if (engine.capturedSourceManaged) {
        using Channel = hftrec::corpus::BinaryMarketChannel;
        const auto channel = stream == cxet::api::market::PublicMarketDataStream::Trades ? Channel::Trade :
            stream == cxet::api::market::PublicMarketDataStream::BookTicker ? Channel::BookTicker : Channel::Depth;
        const auto bit = hftrec::corpus::binaryMarketChannelBit(channel);
        value.continuous = (engine.capturedActiveChannels & bit) && !(engine.capturedUnhealthyChannels & bit);
      }
      return value;
    };
    view.tradeEvidence = evidence(cxet::api::market::PublicMarketDataStream::Trades);
    view.bookTickerEvidence = evidence(cxet::api::market::PublicMarketDataStream::BookTicker);
    view.depthEvidence = evidence(cxet::api::market::PublicMarketDataStream::Orderbook);
    if (engine.feePolicyKnown) {
      auto& fee = feeSchedules_[leg];
      fee = {OrderCommissionModel::Rate, engine.makerFeeBpsE8, engine.takerFeeBpsE8};
      view.fee = {&fee, 1u,
          static_cast<std::uint32_t>((static_cast<std::uint32_t>(engine.exchange.raw) << 8u) | engine.market.raw),
          sources_[leg].sizing.quoteCurrency, engine.exchange.raw, engine.market.raw,
          StrategyFeeScheduleProvenance::ManualConfiguration, feeQualificationMono_,
          engine.legIndex + 1u, static_cast<std::uint32_t>(engine.market.raw) + 1u, 0u};
    }
  }
  context.legInputs = {inputViews_.data(), inputViews_.size()};
  if (descriptor_ && !pairPolicy_) {
    if (engines.size() != legs_.size() || cyclePrepared_ || !context.nowMono.raw ||
        context.state.orders.executionLegCount != legs_.size()) return false;
    input = {};
    for (std::size_t leg = 0u; leg < engines.size(); ++leg) {
      auto& engine = engines[leg];
      const auto& view = context.state.orders.executionLegs[leg];
      if (engine.activeContextIndex != contextIndex_ || view.contextIndex != contextIndex_ ||
          !view.store ||
          view.binding != legs_[leg].binding || view.owner != legs_[leg].owner ||
          view.expectedGeneration != legs_[leg].generation) return false;
      auto& gate = gates_[leg]; gate = {};
      gate.strategyLegId = legs_[leg].legId; gate.binding = view.binding; gate.owner = view.owner;
      gate.generation = view.expectedGeneration; gate.epoch = view.epoch; gate.contextIndex = view.contextIndex;
      const bool currentOrders = view.store->connected() && !view.store->reconciliationRequired();
      gate.accountReady = currentOrders && !engine.simulationEvidenceFailed && engine.position.valid &&
          detail::knownAccountingModel(engine) &&
          (detail::isCashAccounting(engine) || engine.simulationOrders->collateral.known);
      gate.cancelReady = currentOrders && !engine.simulationEvidenceFailed;
      const auto& quote = detail::effectiveBookTicker(engine.snapshot);
      gate.buyPrice = quote.ask.px; gate.sellPrice = quote.bid.px;
      const auto received = engine.snapshot.hasReconstructedBbo.raw
          ? engine.snapshot.reconstructedBookTickerRecvMono : engine.snapshot.lastBookTickerRecvMono;
      gate.orderReady = currentOrders && sourceReady(engine) && engine.feePolicyKnown &&
          (detail::isCashAccounting(engine) || engine.simulationOrders->collateral.authorityCurrent) &&
          (context.tradableLegMask & (std::uint64_t{1u} << leg));
      const bool quoteAvailable = engine.snapshot.hasReconstructedBbo.raw ||
          (engine.snapshot.hasBookTicker.raw && engine.snapshot.bookTickerBidPresent.raw && engine.snapshot.bookTickerAskPresent.raw);
      if (quoteAvailable && gate.buyPrice.raw > 0 && gate.sellPrice.raw > 0 && gate.sellPrice.raw <= gate.buyPrice.raw &&
          received.raw && received.raw <= context.nowMono.raw) gate.priceMono = received;
    }
    cyclePrepared_ = true; dispatchCommitted_ = false;
    return true;
  }
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
    gate.strategyLegId = static_cast<std::uint32_t>(leg + 1u);
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
  gates_[0] = input.execution.legs[0]; gates_[1] = input.execution.legs[1];
  cyclePrepared_ = true; dispatchCommitted_ = false;
  return true;
}

bool BacktestCanonicalExecutionAdapter::decideAndExecute(StrategyContext& context) noexcept {
  if (!descriptor_ || !cyclePrepared_ || (pairPolicy_ && !context.pairInputs)) return false;
  const auto expected = context.io;
  const StrategyExecutionCycleInputs executionInput{gates_, {}, {feedback_.data(), feedbackCount_}};
  const auto* previousExecution = context.executionInputs;
  context.executionInputs = &executionInput;
  context.io.resultCount = resultCount_;
  const bool produced = instance_.runCycle(0u, context);
  if (produced && descriptor_->executeCycle) (void)instance_.executeCycle(0u, context);
  context.executionInputs = previousExecution;
  if (context.io.storage() != expected.storage() ||
      context.io.orderOut().size() != expected.orderOut().size() ||
      context.io.cancelOut().size() != expected.cancelOut().size() ||
      context.io.commandOut().size() != expected.commandOut().size() ||
      context.io.transactionOut().size() != expected.transactionOut().size() ||
      context.io.commandResults().size() != expected.commandResults().size() ||
      context.io.orderOut().data() != expected.orderOut().data() ||
      context.io.cancelOut().data() != expected.cancelOut().data() ||
      context.io.commandOut().data() != expected.commandOut().data() ||
      context.io.transactionOut().data() != expected.transactionOut().data() ||
      context.io.commandResults().data() != commandResults_.data() ||
      context.io.commandResults().size() != commandResults_.size() ||
      context.io.capacityExceeded || context.io.transactionCount ||
      context.io.orderCount > context.io.orderOut().size() || context.io.cancelCount > context.io.cancelOut().size() ||
      context.io.commandCount > context.io.commandOut().size() ||
      !StrategyInstance::directCapabilitiesDeclared(*descriptor_, context.io)) return false;
  // The actor has consumed the previous feedback; new dispatch replaces it.
  feedbackCount_ = 0u;
  return true;
}

bool BacktestCanonicalExecutionAdapter::dispatchAndCommit(StrategyContext& context,
    detail::EngineView engines, std::int64_t tsNs) noexcept {
  if (!descriptor_ || !cyclePrepared_ || engines.size() != sources_.size() ||
      context.io.orderCount + context.io.cancelCount + 2u * context.io.commandCount > feedback_.size() ||
      context.io.commandCount > commandResults_.size() ||
      !StrategyInstance::directCapabilitiesDeclared(*descriptor_, context.io)) return false;
  const auto validRoute = [&](ExecutionRouteHandle route, std::uint32_t legId) noexcept {
    return route.raw && route.raw <= engines.size() &&
        legId == gates_[route.raw - 1u].strategyLegId;
  };
  std::array<std::uint64_t, kStrategyRuntimeMaxOrders + kStrategyRuntimeMaxCancels +
      2u * kStrategyRuntimeMaxCommands> localIds{};
  std::size_t localCount{};
  const auto freshLocal = [&](Id id) noexcept {
    if (!id.raw || localCount == localIds.size()) return false;
    for (std::size_t i = 0u; i < localCount; ++i) if (localIds[i] == id.raw) return false;
    localIds[localCount++] = id.raw; return true;
  };
  // Complete structural validation precedes the first simulated side effect.
  for (std::size_t i = 0u; i < context.io.orderCount; ++i)
    if (!validOrderIntent(context.io.orderOut()[i], false) ||
        !validRoute(context.io.orderOut()[i].routeHandle, context.io.orderOut()[i].strategyLegId) ||
        !freshLocal(context.io.orderOut()[i].clientIntentId)) return false;
  for (std::size_t i = 0u; i < context.io.cancelCount; ++i)
    if (!validCancelIntent(context.io.cancelOut()[i]) ||
        !validRoute(context.io.cancelOut()[i].routeHandle, context.io.cancelOut()[i].strategyLegId) ||
        !freshLocal(context.io.cancelOut()[i].clientIntentId)) return false;
  for (std::size_t i = 0u; i < context.io.commandCount; ++i) {
    const auto& command = context.io.commandOut()[i];
    const auto& order = command.kind == StrategyCommandIntentKind::WarmupOrder
        ? command.warmup.order : command.order;
    if (command.kind == StrategyCommandIntentKind::Order || command.kind == StrategyCommandIntentKind::WarmupOrder ||
        command.kind == StrategyCommandIntentKind::Replace) {
      if (!validOrderIntent(order, command.kind == StrategyCommandIntentKind::WarmupOrder) ||
          !validRoute(order.routeHandle, order.strategyLegId) || !freshLocal(order.clientIntentId)) return false;
    }
    if (command.kind == StrategyCommandIntentKind::Cancel || command.kind == StrategyCommandIntentKind::Replace)
      if (!validCancelIntent(command.cancel) || !validRoute(command.cancel.routeHandle, command.cancel.strategyLegId) ||
          !freshLocal(command.cancel.clientIntentId)) return false;
  }
  std::array<std::uint64_t, trading_core::contracts::kMaximumStrategyLegs> fencedAccounts{};
  std::size_t fencedCount{};
  const auto accountFenced = [&](std::size_t leg) noexcept {
    for (std::size_t i = 0u; i < fencedCount; ++i)
      if (fencedAccounts[i] == gates_[leg].binding.accountId) return true;
    return false;
  };
  const auto releasedUnsent = [&](ExecutionRouteHandle route, std::uint32_t legId, Id local) noexcept {
    const auto view = detail::simulationOrderEvidenceView(engines[route.raw - 1u]);
    StrategyDispatchObservation observation{local, route, view.binding, view.owner,
        view.expectedGeneration, 0u, view.contextIndex, {}, StrategyDispatchKind::ReleasedUnsent};
    observation.strategyLegId = legId;
    feedback_[feedbackCount_++] = observation;
  };
  const auto submit = [&](std::size_t leg, std::uint32_t strategyLegId, Id local, Id target, bool cancel,
                          const StrategyIo& io) noexcept {
    auto& engine = engines[leg];
    const auto previousView = detail::simulationOrderEvidenceView(engine);
    const auto* previous = canonicalOrderEvidenceIdentity(previousView, target, previousView.expectedGeneration);
    const auto previousCancelCommand = cancel && previous && previous->record ? previous->record->cancelCommandId : 0u;
    const bool cancelTargetReady = !cancel || (previous && previous->record && !previous->record->terminal() &&
        previous->record->cancelStatus != execution::CancelStatus::Pending);
    const bool ready = cancelTargetReady && !accountFenced(leg) &&
        (cancel ? gates_[leg].cancelReady : gates_[leg].orderReady);
    if (ready) detail::applyIntents(engine, io, tsNs, engine.latency);
    const auto view = detail::simulationOrderEvidenceView(engine);
    const auto* identity = canonicalOrderEvidenceIdentity(view, target, view.expectedGeneration);
    StrategyDispatchObservation observation{local, ExecutionRouteHandle{static_cast<std::uint32_t>(leg + 1u)},
        view.binding, view.owner, view.expectedGeneration, 0u, view.contextIndex, {}, StrategyDispatchKind::ReleasedUnsent};
    observation.strategyLegId = strategyLegId;
    auto status = StrategyCommandResultStatus::Refused;
    if (ready && identity && identity->record) {
      const auto& record = *identity->record;
      observation.commandId = cancel
          ? (record.cancelCommandId != previousCancelCommand ? record.cancelCommandId : 0u) : identity->commandId;
      if (observation.commandId) observation.kind = StrategyDispatchKind::CommandBound;
      const auto disposition = cancel ? record.cancelSend : record.placeSend;
      const bool settled = cancel ? record.cancelSettled : record.placeSettled;
      if (observation.commandId && settled && disposition != execution::SendDisposition::NotAttempted) {
        observation.kind = StrategyDispatchKind::AttemptTime;
        observation.attemptedMono = context.nowMono;
        status = disposition == execution::SendDisposition::Complete
            ? StrategyCommandResultStatus::Accepted : StrategyCommandResultStatus::Unknown;
      }
    }
    if (engine.simulationEvidenceFailed) status = StrategyCommandResultStatus::Unknown;
    if (status == StrategyCommandResultStatus::Unknown && !accountFenced(leg))
      fencedAccounts[fencedCount++] = gates_[leg].binding.accountId;
    feedback_[feedbackCount_++] = observation;
    return status;
  };
  const auto place = [&](OrderIntent& order, StrategyCommandIntent* warmup = nullptr,
                         bool alreadyOrdinary = false) noexcept {
    const auto leg = order.routeHandle.raw - 1u;
    auto& engine = engines[leg];
    StrategyCloseGrant grant;
    auto claim = StrategyCloseClaimResult::Ordinary;
    if (!pairPolicy_) {
      const auto& gate = gates_[leg];
      const bool market = order.orderType == canon::OrderType::Market || order.orderType == canon::OrderType::Stop;
      if (market && (!gate.priceMono.raw || gate.priceMono.raw > context.nowMono.raw ||
          (marketMaximumAgeNs_ && context.nowMono.raw - gate.priceMono.raw >= marketMaximumAgeNs_))) {
        releasedUnsent(order.routeHandle, order.strategyLegId, order.clientIntentId);
        return StrategyCommandResultStatus::Refused;
      }
      if (!alreadyOrdinary) claim = descriptor_->claimClosingIntent
          ? descriptor_->claimClosingIntent(retained_.state, context, order, grant) : StrategyCloseClaimResult::Refused;
      if (claim == StrategyCloseClaimResult::Refused ||
          (order.semantic == StrategyOrderSemantic::ManagedReduce && claim != StrategyCloseClaimResult::Granted) ||
          (claim == StrategyCloseClaimResult::Granted &&
           (!grant.valid() || grant.intent().clientIntentId.raw != order.clientIntentId.raw ||
            grant.binding() != gate.binding || grant.owner() != gate.owner ||
            grant.generation() != gate.generation || grant.contextIndex() != gate.contextIndex ||
            !grant.beginSubmission())) || engine.scopedCloseGrant) {
        releasedUnsent(order.routeHandle, order.strategyLegId, order.clientIntentId);
        return StrategyCommandResultStatus::Refused;
      }
    }
    struct ScopedGrant {
      detail::EngineState& engine;
      ~ScopedGrant() { engine.scopedCloseGrant = nullptr; }
    } scoped{engine};
    if (claim == StrategyCloseClaimResult::Granted) engine.scopedCloseGrant = &grant;
    const StrategyIoStorage ioStorage{warmup ? nullptr : &order, nullptr, warmup, nullptr, nullptr};
    StrategyIo io{};
    if (!io.bind(ioStorage, warmup ? 0u : 1u, 0u, warmup ? 1u : 0u, 0u, 0u))
      return StrategyCommandResultStatus::Refused;
    if (warmup) io.commandCount = 1u;
    else io.orderCount = 1u;
    return submit(order.routeHandle.raw - 1u, order.strategyLegId,
        order.clientIntentId, order.clientIntentId, false, io);
  };
  const auto cancel = [&](CancelIntent& intent) noexcept {
    const StrategyIoStorage ioStorage{nullptr, &intent, nullptr, nullptr, nullptr};
    StrategyIo io{};
    if (!io.bind(ioStorage, 0u, 1u, 0u, 0u, 0u)) return StrategyCommandResultStatus::Refused;
    io.cancelCount = 1u;
    return submit(intent.routeHandle.raw - 1u, intent.strategyLegId,
        intent.clientIntentId, intent.targetClientIntentId, true, io);
  };
  resultCount_ = 0u;
  // The explicit stream retains source order; direct outputs keep the existing
  // simulator convention of cancellation before placement after that stream.
  for (std::size_t i = 0u; i < context.io.commandCount; ++i) {
    auto& command = context.io.commandOut()[i];
    auto& order = command.kind == StrategyCommandIntentKind::WarmupOrder ? command.warmup.order : command.order;
    const bool cancelOnly = command.kind == StrategyCommandIntentKind::Cancel;
    const auto route = cancelOnly ? command.cancel.routeHandle : order.routeHandle;
    const auto local = cancelOnly ? command.cancel.clientIntentId : order.clientIntentId;
    auto& result = commandResults_[i];
    result = {static_cast<std::uint8_t>(i), command.kind, StrategyCommandResultStatus::Refused,
        local, command.cancel.targetClientIntentId, route, 0u};
    const auto firstFeedback = feedbackCount_;
    if (accountFenced(route.raw - 1u)) result.status = StrategyCommandResultStatus::AccountFenced;
    else if (!strategyCommandDependenciesSatisfied(command, {commandResults_.data(), i}) ||
        !strategyCommandInputDependenciesFresh(command, context.legInputs,
            {descriptor_->legInputRequirements.data(), engines.size()}, context.nowMono.raw))
      result.status = StrategyCommandResultStatus::DependencyBlocked;
    else if (cancelOnly) result.status = cancel(command.cancel);
    else if (command.kind == StrategyCommandIntentKind::Order) result.status = place(order);
    else if (command.kind == StrategyCommandIntentKind::WarmupOrder) {
      auto one = command; one.dependsOnCommands = 0u; one.inputDependencyCount = 0u;
      result.status = place(one.warmup.order, &one);
    } else if (command.kind == StrategyCommandIntentKind::Replace) {
      const auto leg = route.raw - 1u;
      auto view = detail::simulationOrderEvidenceView(engines[leg]);
      const auto* old = canonicalOrderEvidenceIdentity(view, command.cancel.targetClientIntentId, view.expectedGeneration);
      StrategyCloseGrant replacementGrant;
      const bool ordinaryReplacement = pairPolicy_ || (descriptor_->claimClosingIntent &&
          descriptor_->claimClosingIntent(retained_.state, context, order, replacementGrant) == StrategyCloseClaimResult::Ordinary &&
          order.semantic != StrategyOrderSemantic::ManagedReduce);
      // Immediate replacement is same-leg Limit->Limit with preserved owner,
      // side and account. No venue ACK wait or automatic quantity adjustment.
      if (ordinaryReplacement && old && old->record && old->record->request.type == execution::OrderType::Limit &&
          old->record->request.side.raw == order.side.raw && !old->record->terminal() &&
          !old->record->unknownOutcome && !old->record->hasReplacement &&
          old->record->cancelStatus != execution::CancelStatus::Pending) {
        const auto oldKey = old->record->request.key();
        StrategyCommandResultStatus first{}, second{StrategyCommandResultStatus::Refused};
        if (command.replacementOrder == execution::ReplacementOrder::CancelThenNew) {
          first = cancel(command.cancel);
          if (first == StrategyCommandResultStatus::Accepted) second = place(order, nullptr, true);
          else releasedUnsent(order.routeHandle, order.strategyLegId, order.clientIntentId);
        } else {
          first = place(order, nullptr, true);
          if (first == StrategyCommandResultStatus::Accepted) second = cancel(command.cancel);
          else releasedUnsent(command.cancel.routeHandle, command.cancel.strategyLegId, command.cancel.clientIntentId);
          if (first == StrategyCommandResultStatus::Accepted && feedbackCount_)
            feedback_[feedbackCount_ - 1u].earlierPlacementCompleted = true;
        }
        result.status = first == StrategyCommandResultStatus::Unknown || second == StrategyCommandResultStatus::Unknown
            ? StrategyCommandResultStatus::Unknown
            : first == StrategyCommandResultStatus::Accepted && second == StrategyCommandResultStatus::Accepted
                ? StrategyCommandResultStatus::Accepted : StrategyCommandResultStatus::Refused;
        const auto* replacement = canonicalOrderEvidenceIdentity(view, order.clientIntentId, view.expectedGeneration);
        if (replacement && replacement->record && replacement->record->placeSend != execution::SendDisposition::NotAttempted &&
            engines[leg].simulationOrders->store->setReplacement(oldKey, replacement->record->request.key()) != execution::StateResult::Applied)
          engines[leg].simulationEvidenceFailed = true;
      }
    }
    if (feedbackCount_ == firstFeedback) {
      if (cancelOnly || command.kind == StrategyCommandIntentKind::Replace)
        releasedUnsent(command.cancel.routeHandle, command.cancel.strategyLegId, command.cancel.clientIntentId);
      if (!cancelOnly) releasedUnsent(order.routeHandle, order.strategyLegId, order.clientIntentId);
    }
    for (std::size_t observation = firstFeedback; observation < feedbackCount_; ++observation)
      if (feedback_[observation].clientIntentId.raw == local.raw)
        result.commandId = feedback_[observation].commandId;
    ++resultCount_;
  }
  for (std::size_t i = 0u; i < context.io.cancelCount; ++i) (void)cancel(context.io.cancelOut()[i]);
  for (std::size_t i = 0u; i < context.io.orderCount; ++i) (void)place(context.io.orderOut()[i]);
  context.io.resultCount = resultCount_;
  if (!descriptor_->commitPreparedDispatch(retained_.state)) return false;
  for (std::size_t leg = 0u; leg < engines.size(); ++leg) {
    consumedCuts_[leg] = engines[leg].simulationOrders->deliveryCut;
    if (engines[leg].simulationEvidenceFailed) return false;
  }
  cyclePrepared_ = false; dispatchCommitted_ = true;
  return true;
}

bool BacktestCanonicalExecutionAdapter::released(std::size_t leg, std::uint32_t context,
    Id id, std::uint64_t cut) const noexcept {
  if (!descriptor_ || leg >= consumedCuts_.size() || context != contextIndex_ || cyclePrepared_ ||
      !dispatchCommitted_ || consumedCuts_[leg] < cut ||
      descriptor_->retainsExecutionIntent(retained_.state, id)) return false;
  for (std::size_t i = 0u; i < feedbackCount_; ++i)
    if (feedback_[i].route.raw == leg + 1u && feedback_[i].clientIntentId.raw == id.raw) return false;
  return true;
}

bool BacktestCanonicalExecutionAdapter::bindSelectionOwner(BacktestCanonicalPairSelection& selection,
    std::uint32_t index) noexcept {
  if (!descriptor_ || selection_ || !descriptor_->runtimePolicy->selection.enabled() ||
      selection.policy() != descriptor_->runtimePolicy) return false;
  selection_ = &selection; selectionIndex_ = index;
  return true;
}

TimeNs BacktestCanonicalExecutionAdapter::nextWakeMono() const noexcept {
  if (!descriptor_) return {};
  auto wake = descriptor_->nextWakeMono ? descriptor_->nextWakeMono(retained_.state) : TimeNs{};
  const auto selectionWake = selection_ ? selection_->nextWakeMono() : TimeNs{};
  if (selectionWake.raw && (!wake.raw || selectionWake.raw < wake.raw)) wake = selectionWake;
  return wake;
}
} // namespace hft_backtest
