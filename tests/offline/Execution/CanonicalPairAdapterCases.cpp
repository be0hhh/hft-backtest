#include "OfflineCase.hpp"
#include "Execution/BacktestOrderEvidence.hpp"
#include "Engine/BacktestEngine.hpp"
#include "Strategy/Execution/BacktestCanonicalPairAdapter.hpp"
#include "Strategy/Execution/BacktestCanonicalPairSelection.hpp"
#include "Engine/BacktestEngineSetup.hpp"
#include "strategy/trader/DeliverySumSignal/Compiled.hpp"
#include "strategy/trader/PairApiProbe/Compiled.hpp"
#include "trading_core/Strategy/Orders/Evidence/OrderDecisionCursor.hpp"

#include <limits>
#include <memory>
#include <algorithm>

namespace {
namespace detail = hft_backtest::detail;
namespace execution = trading_core::execution;

struct Proof {
  bool consumed{true};
  std::uint64_t through{std::numeric_limits<std::uint64_t>::max()};
  Id retained{};
  static bool released(void* raw, std::uint32_t, Id id, std::uint64_t cut) noexcept {
    const auto& self = *static_cast<Proof*>(raw);
    return self.consumed && cut <= self.through && id.raw != self.retained.raw;
  }
  detail::SimulationEvidenceRetirementProof view() noexcept { return {this, &released}; }
};

struct Fixture {
  std::unique_ptr<detail::EngineState> engine{std::make_unique<detail::EngineState>()};
  Proof proof{};
  Fixture() {
    engine->exchange.raw = 1u; engine->market = canon::kMarketTypeFutures;
    engine->accountingModel = detail::SimulationAccountingModel::Linear;
    CXET_CHECK(engine->symbol.copyFrom("AAA_BBB"));
    CXET_CHECK(detail::prepareSimulationOrderEvidence(*engine));
  }
  trading_core::StrategyOrderEvidence identity(std::uint64_t id) {
    const auto view = detail::simulationOrderEvidenceView(*engine);
    const auto* row = trading_core::strategy::canonicalOrderEvidenceIdentity(view, Id{id}, 1u);
    CXET_CHECK(row != nullptr); return *row;
  }
  trading_core::StrategyOrderEvidence install(std::uint64_t id) {
    trading_core::OrderIntent intent{};
    intent.clientIntentId.raw = id; intent.side = Side::Buy();
    intent.orderType = canon::OrderType::Market; intent.qty.raw = 1;
    CXET_CHECK(detail::installSimulationOrderIntent(*engine, intent));
    CXET_CHECK(detail::settleSimulationOrderSubmit(*engine, intent.clientIntentId, true));
    return identity(id);
  }
  trading_core::StrategyOrderEvidence finish(std::uint64_t id) {
    const auto row = install(id);
    auto& owner = *engine->simulationOrders;
    execution::OrderObservation observation{};
    observation.key = {owner.binding, row.commandId};
    observation.status = execution::VenueStatus::Filled;
    observation.cumulativeKnown = observation.terminalQuantityFinal = true;
    observation.cumulativeQtyE8 = 1;
    CXET_CHECK(owner.store->observe(observation).result == execution::StateResult::Applied);
    for (std::size_t slot = 0u; slot < owner.identities.size(); ++slot)
      if (owner.identities[slot].commandId == row.commandId)
        owner.deliveredCuts[slot] = ++owner.deliveryCut;
    return row;
  }
  std::size_t reclaim() { return detail::reclaimSimulationOrderEvidence(*engine, proof.view()); }
};

void terminalRecordWaitsForConsumedActorCut() {
  Fixture f; f.finish(1u); f.proof.consumed = false;
  CXET_CHECK(f.reclaim() == 0u);
  f.proof.consumed = true; f.proof.through = 0u;
  CXET_CHECK(f.reclaim() == 0u);
  f.proof.through = 1u; f.proof.retained.raw = 1u;
  CXET_CHECK(f.reclaim() == 0u);
  f.proof.retained = {};
  CXET_CHECK(f.reclaim() == 1u);
}

void pendingDispatchBorrowPreventsRetirement() {
  Fixture f; f.finish(7u);
  detail::PendingAction pending{}; pending.targetClientIntentId.raw = 7u;
  f.engine->pending.push_back(pending);
  CXET_CHECK(f.reclaim() == 0u);
  f.engine->pending.clear();
  CXET_CHECK(f.reclaim() == 1u);
}

void finalPrivateDeliveryStillQueuedPreventsRetirement() {
  Fixture f; const auto row = f.finish(9u);
  f.engine->userEventCommandIds.push_back(row.commandId);
  f.engine->userEventConsumed.push_back(0u);
  CXET_CHECK(f.reclaim() == 0u);
  f.engine->userEventConsumed[0] = 1u;
  CXET_CHECK(f.reclaim() == 1u);
}

void deletingCollisionPreservesRemainingIdentity() {
  Fixture f; f.finish(1u); const auto other = f.finish(1025u);
  f.proof.retained.raw = 1025u;
  CXET_CHECK(f.reclaim() == 1u);
  const auto found = f.identity(1025u);
  CXET_CHECK(found.commandId == other.commandId);
  CXET_CHECK(found.record == other.record);
  f.proof.retained = {};
  CXET_CHECK(f.reclaim() == 1u);
}

void completedHistoryDoesNotExhaustEvidenceOrLoseInventory() {
  Fixture f;
  std::uint64_t previousCommand{};
  for (std::uint64_t id = 1u; id <= 4097u; ++id) {
    const auto row = f.finish(id);
    CXET_CHECK(row.commandId > previousCommand);
    previousCommand = row.commandId;
    CXET_CHECK(f.reclaim() == 1u);
  }
  std::int64_t inventory{};
  auto& owner = *f.engine->simulationOrders;
  CXET_CHECK(owner.store->ownedHistoryCompleteCold());
  CXET_CHECK(owner.store->ownedQuantityCold(owner.binding.instrumentId,
      owner.binding.positionSelector, inventory));
  CXET_CHECK(inventory == 4097);
}

void commandExhaustionNeverReusesHistoricalIdentity() {
  Fixture f; const auto first = f.finish(1u); CXET_CHECK(f.reclaim() == 1u);
  f.engine->simulationOrders->nextCommandCounter =
      static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 1u;
  trading_core::OrderIntent intent{};
  intent.clientIntentId.raw = 2u; intent.side = Side::Buy();
  intent.orderType = canon::OrderType::Market; intent.qty.raw = 1;
  CXET_CHECK(!detail::installSimulationOrderIntent(*f.engine, intent));
  CXET_CHECK(!f.engine->simulationOrders->store->find(
      {f.engine->simulationOrders->binding, first.commandId}));
}

struct ActorFixture {
  static constexpr std::int64_t scale = 100'000'000;
  static constexpr std::uint64_t day = 20'000u * trading_core::kCivilDayNs - trading_core::kMoscowUtcOffsetNs;
  static constexpr std::uint64_t utc = day + 1'000'000'000u;
  std::array<std::unique_ptr<detail::EngineState>, 2u> owners{
      std::make_unique<detail::EngineState>(), std::make_unique<detail::EngineState>()};
  std::array<detail::EngineState*, 2u> routes{owners[0].get(), owners[1].get()};
  detail::EngineView engines{routes.data(), routes.size()};
  std::array<hft_backtest::BacktestCanonicalPairSource, 2u> sources{};
  std::array<trading_core::StrategyInstrumentSpec, 2u> instruments{};
  std::array<trading_core::StrategyMarketSnapshot, 2u> market{};
  std::array<trading_core::StrategyOrderEvidenceView, 2u> evidence{};
  std::array<trading_core::OrderIntent, 2u> orders{};
  std::array<trading_core::CancelIntent, 2u> cancels{};
  alignas(trading_core::kStrategyRuntimeStateAlign)
      std::array<std::byte, trading_core::kStrategyRuntimeStateBytes> state{};
  trading_core::StrategyParamStorage params{};
  trading_core::SymbolShardConfig shard{};
  trading_core::StrategyContext context{};
  hft_backtest::BacktestCanonicalPairAdapter adapter{};
  hft_backtest::BacktestCanonicalPairSelection selection{};
  trading_core::DeliveryValuationReference delivery{};
  trading_core::DeliveryValuationCentralBankReference centralBank{};
  std::array<trading_core::CentralBankRatePoint, 1u> rates{{{day - trading_core::kCivilDayNs, 140'000'000}}};
  hft_backtest::BacktestCanonicalPairReferences references{&delivery, &centralBank, {1u, 2u}};
  const trading_core::StrategyDescriptor& descriptor;

  explicit ActorFixture(const trading_core::StrategyDescriptor& selected) : descriptor(selected) {
    using namespace cxet::api;
    for (std::size_t leg = 0u; leg < 2u; ++leg) {
      auto& engine = engines[leg]; engine.legIndex = static_cast<std::uint32_t>(leg);
      engine.exchange.raw = 1u; engine.market = leg ? canon::kMarketTypeFutures : canon::kMarketTypeSpot;
      engine.accountingModel = leg ? detail::SimulationAccountingModel::Linear : detail::SimulationAccountingModel::Cash;
      CXET_CHECK(engine.symbol.copyFrom(leg ? "AAA_FUT" : "AAA"));
      hft_backtest::BacktestRunRequest request{}; request.initialBalanceE8 = 10'000 * scale;
      request.makerFeeBpsE8 = request.takerFeeBpsE8 = 0;
      request.hasMakerFeeBps = request.hasTakerFeeBps = true;
      detail::initializeAccounting(engine, request);
      CXET_CHECK(engine.executionLiquidity.prepare(16u));
      hft_backtest::BacktestRateLimitSchedule schedule{}; schedule.enabled = false;
      std::string error; CXET_CHECK(hft_backtest::applyRateLimitSchedule(engine, schedule, error));
      auto& instrument = instruments[leg];
      instrument.tickSizeRaw = 1; instrument.stepSizeRaw = instrument.executionQuantityStepRaw = scale;
      instrument.contractBaseQtyRaw = instrument.nativeLotBaseQtyRaw = instrument.priceBasisQtyRaw = scale;
      std::copy_n("RUB", 4u, instrument.quoteCurrency.data);
      CXET_CHECK(trading_core::prepareStrategyInstrumentSpecDerived(instrument));
      auto& source = sources[leg]; source.feesKnown = true;
      auto& row = source.instrument;
      row.kind = leg ? InstrumentKind::Delivery : InstrumentKind::Spot;
      row.assetClass = leg ? InstrumentAssetClass::Future : InstrumentAssetClass::Equity;
      row.assetDomain = InstrumentAssetDomain::TradFi;
      CXET_CHECK(row.nativeSymbol.copyFrom(leg ? "AAA_FUT" : "AAA"));
      std::copy_n(leg ? "RTSX" : "MISX", 5u, row.executionMic);
      std::copy_n(leg ? "future-id" : "spot-id", leg ? 10u : 8u, row.providerInstrumentId);
      std::copy_n("RUB", 4u, row.quoteAsset.data); row.settleAsset = row.quoteAsset;
      row.expiryRealtimeNs = day + 30u * trading_core::kCivilDayNs;
      auto& facts = row.economicFacts; facts.source = InstrumentEconomicSource::NativeCatalog;
      facts.sourceVersion = 1u; facts.sourceFingerprint = 123u;
      if (!leg) {
        facts.equityClass = InstrumentEquityClass::Common;
        facts.presenceMask = kInstrumentEconomicEquityClass | kInstrumentEconomicCurrencies;
      } else {
        facts.underlyingAssetClass = InstrumentAssetClass::Equity;
        facts.underlyingEquityClass = InstrumentEquityClass::Common;
        facts.term = InstrumentTerm::Dated; facts.settlement = InstrumentSettlement::Physical;
        facts.underlyingNativeSymbol = sources[0].instrument.nativeSymbol;
        facts.presenceMask = kInstrumentEconomicUnderlyingAssetClass | kInstrumentEconomicUnderlyingEquityClass |
            kInstrumentEconomicTerm | kInstrumentEconomicSettlement | kInstrumentEconomicUnderlyingIdentity |
            kInstrumentEconomicExpiry | kInstrumentEconomicCurrencies;
      }
      auto& sizing = source.sizing;
      sizing.grid = {scale, scale, instrument.qtyStepGrid, instrument.executionQuantityGrid};
      sizing.normalizedBasePerLotRaw = sizing.nativeLotBaseQtyRaw = sizing.priceBasisQtyRaw = scale;
      sizing.quoteCurrency = 1u;
      if (leg) {
        sizing.initialCapitalRaw = 10 * scale; sizing.capitalBasisQtyRaw = scale;
        sizing.capitalCurrency = 1u; sizing.capitalRequirementKnown = true;
      }
      auto& snapshot = engine.snapshot; snapshot.hasBookTicker = snapshot.hasReconstructedBbo = trading_core::kAlgoFlagTrue;
      snapshot.bookTickerBidPresent = snapshot.bookTickerAskPresent = trading_core::kAlgoFlagTrue;
      snapshot.lastBookTicker.bid.px.raw = (leg ? 110 : 99) * scale;
      snapshot.lastBookTicker.ask.px.raw = (leg ? 111 : 100) * scale;
      snapshot.lastBookTicker.bid.qty.raw = snapshot.lastBookTicker.ask.qty.raw = 10 * scale;
      snapshot.reconstructedBookTicker = snapshot.lastBookTicker;
      snapshot.lastBookTickerRecvMono.raw = snapshot.reconstructedBookTickerRecvMono.raw = 1'000'000'000u;
      snapshot.lastBookTickerRecvUtc.raw = utc;
      engine.venueSnapshot = snapshot; market[leg] = snapshot;
      CXET_CHECK(detail::prepareSimulationOrderEvidence(engine));
      evidence[leg] = detail::simulationOrderEvidenceView(engine);
    }
    trading_core::StrategyResolvedPairParameters parameters{};
    parameters.sizing = {capital::sizing::PairSizingMode::AnchorLots, 1u, 1, {}, 1u};
    parameters.entryPpm = 5'000; parameters.exitPpm = 0;
    CXET_CHECK(descriptor.runtimePolicy->storeParameters(parameters, params));
    CXET_CHECK(descriptor.initState(state.data(), params, instruments[0]));
    trading_core::StrategyRuntimeConfiguration configuration{};
    configuration.exchange = 1u; configuration.ownerCount = 2u; configuration.strategyCount = 1u;
    configuration.independentOwner = configuration.budgetConfigured = configuration.amountBudget = true;
    configuration.budgetValueRaw = 10'000 * scale; configuration.budgetCurrency = cxet::StringView{"RUB", 3u};
    std::string error;
    CXET_CHECK(adapter.bindCold(descriptor, params, state.data(), instruments, sources, engines, utc, error, configuration));
    if (descriptor.runtimePolicy->selection.enabled()) {
      const std::array<trading_core::StrategySelectionIdentity, 1u> identities{{
          {sources[1].instrument.expiryRealtimeNs, cxet::StringView{"AAA_FUT", 7u}}}};
      CXET_CHECK(selection.bindCold(*descriptor.runtimePolicy, identities, 120'000'000'000u, error));
      CXET_CHECK(adapter.bindSelectionOwner(selection, 0u));
    }
    auto& model = delivery.current;
    model.futureSourceId = 2u; model.spotSourceId = 1u; model.anchorRealtimeNs = day;
    model.lastTradeEndRealtimeNs = model.deliveryStartRealtimeNs = sources[1].instrument.expiryRealtimeNs;
    model.referenceObservedRealtimeNs = utc; model.revision = 88u;
    model.curveFingerprint = 99u; model.cashflowFingerprint = 101u; model.carryFactorQ9 = 1'010'000'000;
    model.termsFresh = true; model.state = trading_core::DeliveryValuationState::Ready;
    delivery.currentActivationRealtimeNs = utc;
    CXET_CHECK(trading_core::validateCentralBankRateCurve({rates.data(), 1u, utc,
        trading_core::CentralBankCarryQuality::Ready}, &centralBank.currentView));
    centralBank.currentActivationRealtimeNs = centralBank.currentObservedRealtimeNs = utc;
    centralBank.currentState = trading_core::DeliveryValuationCentralBankState::Ready;
    shard.strategyId.raw = shard.shardId.raw = 1u; context.shard = &shard;
    context.marketData = {market.data(), 2u}; context.instrumentSpecs = {instruments.data(), 2u};
    context.state.orders.executionLegs = evidence.data(); context.state.orders.executionLegCount = 2u;
    context.io.orderOut = orders; context.io.cancelOut = cancels;
    context.tradableLegMask = 3u; context.primaryLegIndex = 0u;
  }
  void cycle(std::uint64_t mono) {
    context.io.clear(); context.nowMono.raw = mono; context.nowUtc.raw = utc + mono - 1'000'000'000u;
    context.allowNewEntry = true;
    trading_core::strategy::PairStrategyInputs input{};
    CXET_CHECK(adapter.prepareCycle(context, engines, references, input));
    trading_core::strategy::PairStrategyCycleInputs cut{input}; context.pairInputs = &cut;
    CXET_CHECK(adapter.decideAndExecute(context));
    CXET_CHECK(adapter.dispatchAndCommit(context, engines, static_cast<std::int64_t>(mono)));
    context.pairInputs = nullptr;
  }
  void deliverOpeningFills(std::uint64_t mono) {
    auto latest = mono;
    for (std::size_t leg = 0u; leg < 2u; ++leg) {
      auto& engine = engines[leg]; detail::applyDueActions(engine, static_cast<std::int64_t>(mono));
      CXET_CHECK(!engine.simulationEvidenceFailed);
      while (engine.deliveredUserEvents < engine.userEvents.size()) {
        const auto index = engine.deliveredUserEvents;
        latest = std::max(latest, static_cast<std::uint64_t>(engine.userEventDeliveryTsNs[index]));
        CXET_CHECK(detail::deliverSimulationOrderEvent(engine, index, static_cast<std::int64_t>(latest)));
        const auto& event = engine.userEvents[index];
        context.userEvent = &event; context.hasUserEvent = true;
        context.eventLegIndex = static_cast<std::uint32_t>(leg);
        context.userEventClientIntentId = engine.userEventClientIntentIds[index];
        context.userEventGeneration = engine.userEventGenerations[index];
        cycle(latest);
      }
    }
    context.userEvent = nullptr; context.hasUserEvent = false;
    CXET_CHECK(engines[0].position.netQty.raw == scale);
    CXET_CHECK(engines[1].position.netQty.raw == -scale);
    CXET_CHECK(engines[0].simulationOrders->store->ownedHistoryCompleteCold());
    CXET_CHECK(engines[1].simulationOrders->store->ownedHistoryCompleteCold());
  }
};

void compiledDeliveryActorUsesVirtualCanonicalOrdersAndFills() {
  constexpr trading_core::CompiledDeliveryParameters profile{
      .entryPpm = 5'000, .exitPpm = 0, .sizingMode = capital::sizing::PairSizingMode::AnchorLots,
      .sizingAnchorLeg = 1u, .sizingValueRaw = 1, .exchangeRaw = 1u};
  auto fixture = std::make_unique<ActorFixture>(*trading_core::compiledDeliveryDescriptor<profile>());
  fixture->cycle(1'000'000'000u);
  CXET_CHECK(fixture->engines[0].orders.size() == 1u && fixture->engines[1].orders.size() == 1u);
  CXET_CHECK(fixture->descriptor.hasExecutionExposure(fixture->state.data()));
  fixture->deliverOpeningFills(2'000'000'000u);
}

void compiledProbeActorWaitsForPolicySelectionThenUsesCanonicalFills() {
  auto fixture = std::make_unique<ActorFixture>(*trading_core::compiledPairApiProbeDescriptor<1u>());
  fixture->cycle(1'000'000'000u); fixture->selection.finishCut(1'000'000'000u, true, true);
  CXET_CHECK(fixture->engines[0].orders.empty() && fixture->engines[1].orders.empty());
  fixture->cycle(61'000'000'000u); fixture->selection.finishCut(61'000'000'000u, true, true);
  CXET_CHECK(fixture->selection.status() == trading_core::StrategyInstanceSelection::Status::Selected);
  fixture->cycle(61'000'000'001u);
  CXET_CHECK(fixture->engines[0].orders.size() == 1u && fixture->engines[1].orders.size() == 1u);
  fixture->deliverOpeningFills(62'000'000'000u);
}

void selectionAppendPreservesWindowAndRequiresCompleteCurrentCut() {
  auto fixture = std::make_unique<ActorFixture>(*trading_core::compiledPairApiProbeDescriptor<1u>());
  fixture->cycle(1'000'000'000u); fixture->selection.finishCut(1'000'000'000u, true, true);
  const auto expiry = fixture->sources[1].instrument.expiryRealtimeNs;
  const std::array<trading_core::StrategySelectionIdentity, 2u> appended{{
      {expiry, cxet::StringView{"AAA_FUT", 7u}}, {expiry, cxet::StringView{"ZZZ_FUT", 7u}}}};
  std::string error;
  CXET_CHECK(fixture->selection.appendCold(appended, 120'000'000'000u, error));
  CXET_CHECK(fixture->selection.nextWakeMono().raw == 61'000'000'000u);
  fixture->cycle(61'000'000'000u);
  fixture->selection.finishCut(61'000'000'000u, true, true);
  CXET_CHECK(fixture->selection.status() == trading_core::StrategyInstanceSelection::Status::Waiting);
  fixture->selection.withdraw(1u, 61'000'000'000u);
  fixture->selection.finishCut(61'000'000'000u, true, true);
  CXET_CHECK(fixture->selection.status() == trading_core::StrategyInstanceSelection::Status::Selected);
  CXET_CHECK(fixture->selection.allows(0u));
  const std::array<trading_core::StrategySelectionIdentity, 3u> afterSelection{{
      appended[0], appended[1], {expiry, cxet::StringView{"BBB_FUT", 7u}}}};
  CXET_CHECK(fixture->selection.appendCold(afterSelection, 120'000'000'000u, error));
  CXET_CHECK(fixture->selection.status() == trading_core::StrategyInstanceSelection::Status::Selected);
  CXET_CHECK(fixture->selection.allows(0u));
}

void privateBalanceCycleCannotOpenAfterFinancialAuthorityWithdrawal(bool changedMargin) {
  constexpr trading_core::CompiledDeliveryParameters profile{
      .entryPpm = 5'000, .exitPpm = 0, .sizingMode = capital::sizing::PairSizingMode::AnchorLots,
      .sizingAnchorLeg = 1u, .sizingValueRaw = 1, .exchangeRaw = 1u};
  auto fixture = std::make_unique<ActorFixture>(*trading_core::compiledDeliveryDescriptor<profile>());
  auto& future = fixture->engines[1];
  const auto latestMargin = changedMargin ? 100 * ActorFixture::scale : 0;
  const auto& retained = future.simulationOrders->collateral;
  const bool current = latestMargin > 0 && retained.initialMarginE8 == latestMargin;
  detail::setSimulationDerivativeAuthorityCurrent(future, current);
  CXET_CHECK(!retained.authorityCurrent && retained.initialMarginE8 == 10 * ActorFixture::scale);
  cxet::composite::UserDataEvent event{}; event.kind = cxet::composite::UserDataEventKind::Balance;
  fixture->context.hasUserEvent = true; fixture->context.userEvent = &event;
  fixture->cycle(1'000'000'000u);
  CXET_CHECK(fixture->engines[0].orders.empty() && future.orders.empty());
  CXET_CHECK(!fixture->descriptor.hasExecutionExposure(fixture->state.data()));
}

constexpr cxet::testing::Case cases[]{
  {"canonical_terminal_waits_for_actor_consumed_cut", terminalRecordWaitsForConsumedActorCut},
  {"canonical_pending_dispatch_borrow_prevents_retirement", pendingDispatchBorrowPreventsRetirement},
  {"canonical_queued_private_delivery_prevents_retirement", finalPrivateDeliveryStillQueuedPreventsRetirement},
  {"canonical_retired_collision_preserves_hash_chain", deletingCollisionPreservesRemainingIdentity},
  {"canonical_completed_history_reuses_storage_preserves_inventory", completedHistoryDoesNotExhaustEvidenceOrLoseInventory},
  {"canonical_command_exhaustion_refuses_identity_reuse", commandExhaustionNeverReusesHistoricalIdentity},
  {"compiled_delivery_actor_virtual_canonical_orders_and_fills", compiledDeliveryActorUsesVirtualCanonicalOrdersAndFills},
  {"compiled_probe_actor_policy_selection_and_canonical_fills", compiledProbeActorWaitsForPolicySelectionThenUsesCanonicalFills},
  {"canonical_selection_append_preserves_window_and_final_cut", selectionAppendPreservesWindowAndRequiresCompleteCurrentCut},
  {"canonical_private_opening_after_changed_latest_margin_refused", +[] {
      privateBalanceCycleCannotOpenAfterFinancialAuthorityWithdrawal(true); }},
  {"canonical_private_opening_after_unavailable_latest_margin_refused", +[] {
      privateBalanceCycleCannotOpenAfterFinancialAuthorityWithdrawal(false); }},
};
}

int main(int argc, char** argv) { return cxet::testing::runCases(argc, argv, cases); }
