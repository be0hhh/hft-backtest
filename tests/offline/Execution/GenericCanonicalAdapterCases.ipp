struct GenericActor {
  trading_core::strategy::StrategyExecutionRuntime execution{};
  trading_core::strategy::StrategyManagedCloseAuthority close{};
  std::array<trading_core::strategy::StrategyManagedCloseAuthority::Claim, 32u> claims{};
  TimeNs observedPriceMono{};
  bool emitted{};
};

trading_core::StrategyDescriptor genericDescriptor(std::size_t count) {
  using namespace trading_core;
  auto descriptor = kDeliverySumSignalStrategyDescriptor;
  descriptor.universeTopology = StrategyUniverseTopology::ExplicitGroup;
  descriptor.minLegs = descriptor.maxLegs = static_cast<std::uint8_t>(count);
  descriptor.allowsExplicitGroup = descriptor.preparePairCatalogDependencies = false;
  descriptor.instrumentRequirements = {}; descriptor.pairRequirements = {};
  descriptor.runtimePolicy = &kEmptyStrategyRuntimePolicy;
  descriptor.footprint = strategyRuntimeFootprint<GenericActor>(2u, 2u, 0u, 32u, 0u);
  descriptor.requiredOrderCapabilities = StrategyOrderCapability::Market | StrategyOrderCapability::Limit | StrategyOrderCapability::Cancel;
  descriptor.executionSlotCapacity = static_cast<std::uint16_t>(count * 2u);
  descriptor.legInputRequirements = {};
  descriptor.legInputRequirementCount = static_cast<std::uint8_t>(count);
  for (std::size_t leg = 0u; leg < count; ++leg)
    descriptor.legInputRequirements[leg].optional = kStrategyInputPriceLimit;
  descriptor.maxOrdersPerLeg = {};
  for (std::size_t leg = 0u; leg < count; ++leg) descriptor.maxOrdersPerLeg[leg] = 2u;
  descriptor.executeCycle = nullptr; descriptor.fusedExecutionCycle = true;
  descriptor.inputFreshnessMaxAgeMs = +[](const StrategyParamStorage&) noexcept { return 10u; };
  descriptor.initState = +[](void* state, const StrategyParamStorage&, const StrategyInstrumentSpec&) noexcept {
    std::construct_at(static_cast<GenericActor*>(state)); return true;
  };
  descriptor.bindExecutionCold = +[](void* state, const StrategyParamStorage&, const StrategyExecutionColdBinding& binding) noexcept {
    auto& actor = *static_cast<GenericActor*>(state);
    return actor.execution.bindCold(binding.legs, binding.executionSlots,
        binding.firstLocalId, binding.endLocalIdExclusive) &&
        actor.close.bindCold(actor.execution, {actor.claims.data(), binding.executionSlots.size()});
  };
  descriptor.runCycle = +[](void* state, StrategyContext& context) noexcept {
    auto& actor = *static_cast<GenericActor*>(state);
    if (!context.executionInputs || context.executionInputs->legs.size() != actor.execution.legs().size()) return false;
    actor.observedPriceMono = context.executionInputs->legs[0].priceMono;
    if (actor.emitted) {
      for (const auto& proof : context.executionInputs->dispatch)
        if (proof.kind == strategy::StrategyDispatchKind::ReleasedUnsent)
          for (const auto& slot : actor.execution.slots())
            if (slot.decision.used && slot.decision.clientIntentId.raw == proof.clientIntentId.raw)
              (void)actor.execution.releaseUnsent(slot.legId, slot.slotId, context, proof);
      return false;
    }
    for (const auto& slot : actor.execution.slots()) {
      const auto& leg = actor.execution.legs()[(slot.legId - 1u)];
      if (context.executionInputs->legs[leg.route.raw - 1u].strategyLegId != leg.legId) return false;
      OrderIntent order{}; order.strategyLegId = leg.legId; order.routeHandle = leg.route;
      order.clientIntentId = actor.execution.nextLocalId(); order.quoteSlotId.raw = slot.slotId + 1u;
      order.side = Side::Buy(); order.qty.raw = ActorFixture::scale;
      order.orderType = slot.slotId == 0u ? canon::OrderType::Market : canon::OrderType::Limit;
      if (order.orderType == canon::OrderType::Limit) order.price.raw = 98 * ActorFixture::scale;
      order.semantic = StrategyOrderSemantic::EntryAdd;
      if (!actor.execution.remember(leg.legId, slot.slotId, order).valid || !context.io.pushOrderedOrder(order)) return false;
    }
    actor.emitted = true; return true;
  };
  descriptor.nextWakeMono = +[](const void*) noexcept { return TimeNs{}; };
  descriptor.retainsExecutionIntent = +[](const void* raw, Id id) noexcept {
    for (const auto& slot : static_cast<const GenericActor*>(raw)->execution.slots())
      if (slot.decision.used && slot.decision.clientIntentId.raw == id.raw) return true;
    return false;
  };
  descriptor.hasExecutionExposure = +[](const void* raw) noexcept {
    return static_cast<const GenericActor*>(raw)->execution.hasExposure();
  };
  descriptor.commitPreparedDispatch = +[](void*) noexcept { return true; };
  descriptor.executionBound = +[](const void* raw) noexcept { return static_cast<const GenericActor*>(raw)->execution.bound(); };
  descriptor.executionEvidence = +[](const void* raw, StrategyExecutionEvidence& out) noexcept {
    const auto& execution = static_cast<const GenericActor*>(raw)->execution;
    out = {}; out.bound = execution.bound(); out.legCount = static_cast<std::uint8_t>(execution.legs().size()); return true;
  };
  descriptor.executionLegBindingsMatchCold = +[](const void*, std::span<const strategy::StrategyExecutionLeg>) noexcept { return false; };
  descriptor.rebindExecutionCold = +[](void*, StrategyContext&, std::span<const strategy::StrategyExecutionLeg>) noexcept { return false; };
  descriptor.canTransferExecutionCold = +[](const void*, std::span<const strategy::StrategyExecutionLeg>, void*) noexcept { return false; };
  descriptor.transferExecutionCold = +[](void*, std::span<const strategy::StrategyExecutionLeg>, void*) noexcept { return false; };
  descriptor.advanceStopClose = +[](void*, StrategyContext&, bool) noexcept { return false; };
  descriptor.executionNativeRearmTarget = nullptr;
  descriptor.claimClosingIntent = +[](void* raw, const StrategyContext& context, const OrderIntent& intent,
      strategy::StrategyCloseGrant& grant) noexcept {
    if (intent.semantic != StrategyOrderSemantic::ManagedReduce) return strategy::StrategyCloseClaimResult::Ordinary;
    if (!intent.quoteSlotId.raw || intent.quoteSlotId.raw > 32u) return strategy::StrategyCloseClaimResult::Refused;
    return static_cast<GenericActor*>(raw)->close.claimClosingIntent(context, intent.strategyLegId,
        static_cast<std::uint16_t>(intent.quoteSlotId.raw - 1u), intent, grant);
  };
  return descriptor;
}

void genericReplayUsesActualLegsAndMultipleSlots(std::size_t count) {
  using namespace trading_core;
  auto descriptor = genericDescriptor(count);
  CXET_CHECK(strategyDescriptorContractValid(descriptor));
  std::vector<std::unique_ptr<detail::EngineState>> owners;
  std::vector<detail::EngineState*> routes;
  std::vector<hft_backtest::BacktestCanonicalSource> sources(count);
  std::vector<StrategyInstrumentSpec> instruments(count);
  std::vector<StrategyOrderEvidenceView> evidence(count);
  std::vector<StrategyMarketSnapshot> market(count);
  for (std::size_t leg = 0u; leg < count; ++leg) {
    owners.push_back(std::make_unique<detail::EngineState>()); auto& engine = *owners.back(); routes.push_back(&engine);
    engine.legIndex = static_cast<std::uint32_t>(leg); engine.exchange.raw = 1u; engine.market = canon::kMarketTypeSpot;
    engine.accountingModel = detail::SimulationAccountingModel::Cash;
    CXET_CHECK(engine.symbol.copyFrom("AAA"));
    hft_backtest::BacktestRunRequest request{}; request.initialBalanceE8 = 10'000 * ActorFixture::scale;
    request.hasMakerFeeBps = request.hasTakerFeeBps = true;
    detail::initializeAccounting(engine, request); engine.feePolicyKnown = true;
    CXET_CHECK(engine.executionLiquidity.prepare(16u));
    hft_backtest::BacktestRateLimitSchedule schedule{}; schedule.enabled = false; std::string error;
    CXET_CHECK(hft_backtest::applyRateLimitSchedule(engine, schedule, error));
    auto& spec = instruments[leg]; spec.tickSizeRaw = 1;
    spec.stepSizeRaw = spec.executionQuantityStepRaw = spec.priceBasisQtyRaw = spec.nativeLotBaseQtyRaw = ActorFixture::scale;
    CXET_CHECK(prepareStrategyInstrumentSpecDerived(spec));
    sources[leg].feesKnown = true; sources[leg].instrument.kind = cxet::api::InstrumentKind::Spot;
    sources[leg].sizing.quoteCurrency = 1u;
    auto& snapshot = engine.snapshot; snapshot.hasBookTicker = snapshot.hasReconstructedBbo = kAlgoFlagTrue;
    snapshot.bookTickerBidPresent = snapshot.bookTickerAskPresent = kAlgoFlagTrue;
    snapshot.lastBookTicker.bid.px.raw = 99 * ActorFixture::scale; snapshot.lastBookTicker.ask.px.raw = 100 * ActorFixture::scale;
    snapshot.reconstructedBookTicker = snapshot.lastBookTicker;
    snapshot.lastBookTickerRecvMono.raw = snapshot.reconstructedBookTickerRecvMono.raw = 1'000'000'000u;
    market[leg] = snapshot;
    CXET_CHECK(detail::prepareSimulationOrderEvidence(engine)); evidence[leg] = detail::simulationOrderEvidenceView(engine);
  }
  alignas(kStrategyRuntimeStateAlign) GenericActor state{};
  StrategyParamStorage params{}; hft_backtest::BacktestCanonicalExecutionAdapter adapter;
  std::string error; detail::EngineView engines{routes.data(), routes.size()};
  StrategyRuntimeConfiguration budget{};
  budget.budgetConfigured = budget.amountBudget = true;
  budget.budgetValueRaw = ActorFixture::scale; budget.budgetCurrency = cxet::StringView{"RUB", 3u};
  CXET_CHECK(!adapter.bindCold(descriptor, params, &state, instruments, sources, engines, 1'000'000'000u, error, budget));
  CXET_CHECK(error == "configured virtualHost strategy budget is unsupported by the generic replay financial adapter");
  CXET_CHECK(!adapter.bound() && !state.execution.bound());
  for (const auto& engine : owners)
    CXET_CHECK(!engine->genericCommandInputAdmission && engine->simulationOrders->store->sequence() == 0u);
  budget.budgetValueRaw = 0u;
  CXET_CHECK(!adapter.bindCold(descriptor, params, &state, instruments, sources, engines, 1'000'000'000u, error, budget));
  CXET_CHECK(adapter.bindCold(descriptor, params, &state, instruments, sources, engines, 1'000'000'000u, error));
  CXET_CHECK(state.execution.legs().size() == count && state.execution.slots().size() == 2u * count);
  struct Output { std::array<StrategyCommandIntent, 32u> commands{}; };
  auto output = std::make_unique<Output>(); StrategyContext context{};
  context.nowMono.raw = 1'000'000'000u; context.tradableLegMask = (std::uint64_t{1u} << count) - 1u;
  context.marketData = {market.data(), count}; context.instrumentSpecs = {instruments.data(), count};
  context.state.orders.executionLegs = evidence.data(); context.state.orders.executionLegCount = count;
  context.io.commandOut = output->commands;
  strategy::PairStrategyInputs pair{};
  CXET_CHECK(adapter.prepareCycle(context, engines, {}, pair));
  CXET_CHECK(context.pairInputs == nullptr && adapter.decideAndExecute(context));
  CXET_CHECK(context.executionInputs == nullptr && context.io.commandCount == count * 2u);
  CXET_CHECK(adapter.dispatchAndCommit(context, engines, 1'000'000'000u));
  CXET_CHECK(context.io.resultCount == count * 2u);
  for (std::size_t i = 0u; i < context.io.resultCount; ++i)
    CXET_CHECK(context.io.commandResults[i].status == StrategyCommandResultStatus::Accepted);
  for (const auto& engine : owners) CXET_CHECK(engine->orders.size() == 2u);
  context.io.clear(); ++context.nowMono.raw;
  CXET_CHECK(adapter.prepareCycle(context, engines, {}, pair) && adapter.decideAndExecute(context));
  if (count == 1u) {
    // A missing named optional input blocks only its command and descendants.
    // Independent commands keep source order and may still be admitted.
    auto order = output->commands[1].order;
    order.clientIntentId.raw = 100u;
    CXET_CHECK(context.io.pushOrderedOrder(order));
    context.io.commandOut[0].inputDependencies[0] = {1u, kStrategyInputPriceLimit};
    context.io.commandOut[0].inputDependencyCount = 1u;
    order.clientIntentId.raw = 101u;
    CXET_CHECK(context.io.pushOrderedOrder(order, 1u));
    order.clientIntentId.raw = 102u;
    CXET_CHECK(context.io.pushOrderedOrder(order));
    CXET_CHECK(adapter.dispatchAndCommit(context, engines, static_cast<std::int64_t>(context.nowMono.raw)));
    CXET_CHECK(context.io.commandResults[0].status == StrategyCommandResultStatus::DependencyBlocked);
    CXET_CHECK(context.io.commandResults[1].status == StrategyCommandResultStatus::DependencyBlocked);
    CXET_CHECK(context.io.commandResults[2].status == StrategyCommandResultStatus::Accepted);
    CXET_CHECK(owners[0]->orders.size() == 3u);
    auto& engine = *owners[0]; auto& canonical = *engine.simulationOrders->store;
    CXET_CHECK(canonical.sequence() == 1u);
    // Controlled producer facts establish two managed fills. The larger
    // account cut also includes foreign inventory, which supplies no grant.
    for (std::uint16_t slot = 0u; slot < 2u; ++slot) {
      const auto id = state.execution.slots()[slot].decision.clientIntentId;
      const auto view = detail::simulationOrderEvidenceView(engine);
      const auto* identity = strategy::canonicalOrderEvidenceIdentity(view, id, view.expectedGeneration);
      CXET_CHECK(identity && identity->record);
      execution::OrderObservation fill{}; fill.key = identity->record->request.key();
      fill.status = execution::VenueStatus::Filled; fill.cumulativeQtyE8 = ActorFixture::scale;
      fill.cumulativeKnown = fill.terminalQuantityFinal = true;
      CXET_CHECK(canonical.observe(fill).result == execution::StateResult::Applied);
      CXET_CHECK(state.execution.consume(1u, slot, context).valid);
      CXET_CHECK(state.execution.retire(1u, slot, context));
    }
    CXET_CHECK(state.execution.exposure(1u) == 2 * ActorFixture::scale);
    engine.position.netQty.raw = engine.netQtyRaw = 12 * ActorFixture::scale;
    for (unsigned variant = 0u; variant < 4u; ++variant) {
      context.io.clear(); ++context.nowMono.raw;
      CXET_CHECK(adapter.prepareCycle(context, engines, {}, pair) && adapter.decideAndExecute(context));
      OrderIntent close{}; close.strategyLegId = 1u; close.routeHandle.raw = 1u; close.quoteSlotId.raw = 1u;
      close.clientIntentId = variant == 0u ? Id{999u} : state.execution.nextLocalId();
      close.orderType = canon::OrderType::Market; close.semantic = StrategyOrderSemantic::ManagedReduce;
      close.side = variant == 2u ? Side::Buy() : Side::Sell();
      close.qty.raw = (variant == 0u ? 10 : variant == 1u ? 3 : variant == 2u ? 1 : 2) * ActorFixture::scale;
      if (variant) CXET_CHECK(state.execution.remember(1u, 0u, close).valid);
      const auto before = engine.orders.size();
      CXET_CHECK(context.io.pushOrderedOrder(close));
      CXET_CHECK(adapter.dispatchAndCommit(context, engines, static_cast<std::int64_t>(context.nowMono.raw)));
      CXET_CHECK(context.io.commandResults[0].status == (variant == 3u
          ? StrategyCommandResultStatus::Accepted : StrategyCommandResultStatus::Refused));
      CXET_CHECK(engine.orders.size() == before + (variant == 3u ? 1u : 0u));
      CXET_CHECK(engine.scopedCloseGrant == nullptr);
    }
    // A late timer cannot restamp an old executable quote. A limit order keeps
    // using its own price and remains independent of that stale market quote.
    context.io.clear(); context.nowMono.raw = 1'010'000'000u;
    CXET_CHECK(adapter.prepareCycle(context, engines, {}, pair) && adapter.decideAndExecute(context));
    CXET_CHECK(state.observedPriceMono.raw == 1'000'000'000u);
    OrderIntent stale{}; stale.strategyLegId = 1u; stale.routeHandle.raw = 1u;
    stale.clientIntentId.raw = 1000u; stale.side = Side::Buy(); stale.qty.raw = ActorFixture::scale;
    stale.orderType = canon::OrderType::Market; stale.semantic = StrategyOrderSemantic::EntryAdd;
    CXET_CHECK(context.io.pushOrderedOrder(stale));
    stale.clientIntentId.raw = 1001u; stale.orderType = canon::OrderType::Limit; stale.price.raw = 98 * ActorFixture::scale;
    CXET_CHECK(context.io.pushOrderedOrder(stale));
    const auto before = engine.orders.size();
    CXET_CHECK(adapter.dispatchAndCommit(context, engines, static_cast<std::int64_t>(context.nowMono.raw)));
    CXET_CHECK(context.io.commandResults[0].status == StrategyCommandResultStatus::Refused);
    CXET_CHECK(context.io.commandResults[1].status == StrategyCommandResultStatus::Accepted);
    CXET_CHECK(engine.orders.size() == before + 1u);
  } else {
    // Two replacements in the same ordered stream preserve their own old/new
    // canonical links for both supported immediate local dispatch orders.
    for (std::size_t index = 0u; index < 2u; ++index) {
      const auto& slot = state.execution.slots()[index == 0u ? 1u : count + 1u];
      OrderIntent replacement{}; replacement.strategyLegId = slot.legId;
      replacement.routeHandle.raw = slot.legId; replacement.clientIntentId.raw = 100u + 2u * index;
      replacement.quoteSlotId = slot.decision.quoteSlotId; replacement.side = Side::Buy();
      replacement.orderType = canon::OrderType::Limit; replacement.price.raw = 97 * ActorFixture::scale;
      replacement.qty.raw = ActorFixture::scale; replacement.semantic = StrategyOrderSemantic::EntryAdd;
      CancelIntent cancellation{}; cancellation.strategyLegId = slot.legId;
      cancellation.routeHandle = replacement.routeHandle; cancellation.clientIntentId.raw = 101u + 2u * index;
      cancellation.quoteSlotId = replacement.quoteSlotId; cancellation.targetClientIntentId = slot.decision.clientIntentId;
      cancellation.targetOrderType = canon::OrderType::Limit;
      CXET_CHECK(context.io.pushReplace(cancellation, replacement,
          index == 0u ? execution::ReplacementOrder::CancelThenNew : execution::ReplacementOrder::NewThenCancel));
    }
    CXET_CHECK(adapter.dispatchAndCommit(context, engines, static_cast<std::int64_t>(context.nowMono.raw)));
    CXET_CHECK(context.io.resultCount == 2u &&
        context.io.commandResults[0].status == StrategyCommandResultStatus::Accepted &&
        context.io.commandResults[1].status == StrategyCommandResultStatus::Accepted);
    const auto view = detail::simulationOrderEvidenceView(*owners[1]);
    for (const auto id : {Id{100u}, Id{102u}}) {
      const auto* replacement = strategy::canonicalOrderEvidenceIdentity(view, id, view.expectedGeneration);
      CXET_CHECK(replacement && replacement->record && replacement->record->hasPredecessor);
    }
  }
}
