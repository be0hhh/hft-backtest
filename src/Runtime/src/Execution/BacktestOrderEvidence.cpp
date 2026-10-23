#include "trading_core/Execution/Identity.hpp"
#include "capital/Accounting/FillPositionAccounting.hpp"
#include "../Accounting/BacktestPositionQuantity.hpp"
#include "BacktestOrderEvidence.hpp"
#include "../Engine/BacktestEngine.hpp"
#include "BacktestClientOrderId.hpp"
#include "trading_core/Strategy/Orders/Evidence/OrderDecisionCursor.hpp"
#include "trading_core/Contracts/Numeric/Integer/Saturating.hpp"
#include <algorithm>
#include <iterator>
#include <limits>
namespace hft_backtest::detail {
namespace {
namespace execution = trading_core::execution;
execution::ExecutionBinding bindingFor(const EngineState& engine) noexcept {
  return {(static_cast<std::uint64_t>(engine.exchange.raw) << 32u) |
      (static_cast<std::uint32_t>(engine.market.raw) + 1u),
      static_cast<std::uint64_t>(engine.legIndex) + 1u, 1u,
      static_cast<std::uint32_t>(engine.market.raw) + 1u};
}
execution::OwnerIdentity ownerFor(const EngineState& engine) noexcept {
  return {static_cast<std::uint64_t>(engine.legIndex) + 1u,
      static_cast<std::uint64_t>(engine.activeContextIndex) + 1u};
}
std::size_t evidenceSlot(const EngineState& engine, Id id, bool empty, std::uint32_t contextIndex) noexcept {
  if (!engine.simulationOrders || !id.raw) return kSimulationOrderEvidenceCapacity;
  const auto prefix = static_cast<std::uint64_t>(contextIndex) + 1u;
  const auto first = static_cast<std::size_t>((prefix ^ id.raw) * 0x100000001b3ULL) &
      (kSimulationOrderEvidenceCapacity - 1u);
  std::size_t vacant = kSimulationOrderEvidenceCapacity;
  for (std::size_t probe = 0u; probe < trading_core::strategy::kOrderEvidenceMaxProbes; ++probe) {
    const auto index = (first + probe) & (kSimulationOrderEvidenceCapacity - 1u);
    const auto& row = engine.simulationOrders->identities[index];
    if (!row.clientIntentId.raw) {
      if (vacant == kSimulationOrderEvidenceCapacity) vacant = index;
      if (row.generation) continue;
      return empty ? vacant : kSimulationOrderEvidenceCapacity;
    }
    if (row.clientIntentId.raw == id.raw && row.contextIndex == contextIndex)
      return empty ? kSimulationOrderEvidenceCapacity : index;
  }
  return empty ? vacant : kSimulationOrderEvidenceCapacity;
}
std::int64_t& reservedQuote(EngineState& engine, bool observed) noexcept {
  if (engine.sharedAccount) return observed ? engine.sharedAccount->observedReservedQuoteE8 : engine.sharedAccount->venueReservedQuoteE8;
  return observed ? engine.observedReservedQuoteE8 : engine.venueReservedQuoteE8;
}
std::int64_t reservedQuote(const EngineState& engine, bool observed) noexcept {
  if (engine.sharedAccount) return observed ? engine.sharedAccount->observedReservedQuoteE8 : engine.sharedAccount->venueReservedQuoteE8;
  return observed ? engine.observedReservedQuoteE8 : engine.venueReservedQuoteE8;
}
std::int64_t protectedQuote(const EngineState& engine, bool observed) noexcept {
  return engine.sharedAccount ? (observed ? engine.sharedAccount->observedProtectedCollateralE8 :
      engine.sharedAccount->venueProtectedCollateralE8) : 0;
}
bool collateralUnavailable(const EngineState& engine, bool observed) noexcept {
  return engine.sharedAccount && (observed ? engine.sharedAccount->observedCollateralUnavailableSources :
      engine.sharedAccount->venueCollateralUnavailableSources) != 0u;
}
bool quoteCost(std::int64_t price, std::int64_t qty, std::int64_t feeRate, std::int64_t& out) noexcept {
  if (!qty) { out = 0; return true; }
  if (qty < 0 || price <= 0) return false;
  const auto fee = capital::accounting::fillFeeE8(price, qty, feeRate);
  std::int64_t delta{};
  if (!capital::accounting::spotCashDeltaE8(Side::Buy(), price, qty, fee, delta)) return false;
  out = -delta; return true;
}
bool setReservation(EngineState& engine, SpotOrderReservation& row, bool observed,
    std::int64_t quote, std::int64_t base) noexcept {
  auto& oldQuote = observed ? row.observedQuoteE8 : row.venueQuoteE8;
  auto& oldBase = observed ? row.observedBaseE8 : row.venueBaseE8;
  auto& totalQuote = reservedQuote(engine, observed);
  auto& totalBase = observed ? engine.observedReservedBaseE8 : engine.venueReservedBaseE8;
  if (quote < 0 || base < 0 || totalQuote < oldQuote || totalBase < oldBase ||
      quote > std::numeric_limits<std::int64_t>::max() - (totalQuote - oldQuote) ||
      base > std::numeric_limits<std::int64_t>::max() - (totalBase - oldBase)) {
    engine.simulationEvidenceFailed = true; return false;
  }
  totalQuote = totalQuote - oldQuote + quote; totalBase = totalBase - oldBase + base;
  oldQuote = quote; oldBase = base;
  return true;
}
std::uint64_t nextCommand(EngineState& engine) noexcept {
  auto& owner = *engine.simulationOrders;
  if (engine.legIndex == std::numeric_limits<std::uint32_t>::max() ||
      owner.nextCommandCounter > std::numeric_limits<std::uint32_t>::max()) return 0u;
  return ((static_cast<std::uint64_t>(engine.legIndex) + 1u) << 32u) |
      owner.nextCommandCounter++;
}
}
bool prepareSimulationOrderEvidence(EngineState& engine) noexcept {
  if (!engine.simulationOrders) return false;
  auto& owner = *engine.simulationOrders;
  const auto binding = bindingFor(engine);
  if (owner.ready) return owner.binding == binding;
  for (const auto& order : engine.open) if (order.state.valid) return false;
  owner.binding = binding;
  owner.store.emplace(owner.records, owner.index, binding, owner.retiredOwned);
  // The virtual producer has complete per-command fill coverage. Select the
  // public per-order finality seam cold; no live/private authority is implied.
  if (!owner.store->enableOrderScopedUnknownCold()) return false;
  const execution::OwnerRegistration registration{1u, ownerFor(engine),
      binding.accountId, binding.instrumentId, binding.productId,
      binding.positionSelector};
  execution::IdentityAllocator freshRun({&registration, 1u});
  if (!freshRun.prepareCold(1u, std::numeric_limits<std::uint64_t>::max()) ||
      !owner.store->startCurrentRunCold(freshRun)) return false;
  // This is a newly created virtual venue with no commands or venue orders.
  // The exact empty model cut provides no real/private account authority.
  if (owner.store->reconcile(1u, 0u, {}) != execution::StateResult::Applied) return false;
  owner.ready = true;
  return true;
}
trading_core::StrategyOrderEvidenceView simulationOrderEvidenceView(const EngineState& engine) noexcept {
  trading_core::StrategyOrderEvidenceView view;
  if (!engine.simulationOrders || !engine.simulationOrders->ready) return view;
  const auto& owner = *engine.simulationOrders;
  view.rows = owner.identities.data(); view.count = owner.identities.size();
  view.binding = owner.binding; view.owner = ownerFor(engine); view.store = &*owner.store;
  view.epoch = owner.store->epoch(); view.hashPrefix = static_cast<std::uint64_t>(engine.activeContextIndex) + 1u;
  view.expectedGeneration = 1u; view.contextIndex = engine.activeContextIndex;
  // Direct rows borrow exactly the retained identity vector, with the default
  // row stride; no corpus-supplied count or stride is copied into this view.
  if (!trading_core::strategy::validOrderEvidenceStorage(view)) return {};
  return view;
}
bool installSimulationOrderIntent(EngineState& engine, const trading_core::OrderIntent& intent) noexcept {
  if (engine.simulationEvidenceFailed) return false;
  if (!prepareSimulationOrderEvidence(engine)) return false;
  std::size_t unresolved = 0u;
  for (const auto& record : engine.simulationOrders->records)
    if (record.occupied && !record.terminal()) ++unresolved;
  if (unresolved >= kMaxOrderStates) return false;
  const auto index = evidenceSlot(engine, intent.clientIntentId, true, engine.activeContextIndex);
  if (index == kSimulationOrderEvidenceCapacity) return false;
  const auto command = nextCommand(engine);
  if (!command) return false;
  execution::OrderRequest request;
  request.identity = {command, ownerFor(engine), engine.simulationOrders->binding};
  request.side = intent.side; request.qtyE8 = intent.qty.raw;
  request.type = intent.orderType == canon::OrderType::Market ||
      (intent.orderType == canon::OrderType::Stop && intent.price.raw == 0)
      ? execution::OrderType::Market : execution::OrderType::Limit;
  request.priceE8 = request.type == execution::OrderType::Market ? 0 : intent.price.raw;
  auto& owner = *engine.simulationOrders;
  if (owner.store->install(request) != execution::StateResult::Applied) return false;
  const auto* record = owner.store->find(request.key());
  const auto recordGeneration = owner.store->recordGeneration(record);
  if (!recordGeneration) return false;
  owner.identities[index] = {intent.clientIntentId, 1u, command, record, engine.activeContextIndex, recordGeneration};
  auto& local = owner.localStates[index];
  local.clientIntentId = local.desiredClientIntentId = intent.clientIntentId;
  local.quoteSlotId = intent.quoteSlotId; local.side = intent.side; local.orderType = intent.orderType;
  local.limitTimeInForcePolicy = intent.limitTimeInForcePolicy;
  local.triggerSource = local.desiredTriggerSource = intent.triggerSource;
  local.triggeredTimeInForce = local.desiredTriggeredTimeInForce = intent.triggeredTimeInForce;
  local.price = local.desiredPrice = intent.price; local.triggerPrice = local.desiredTriggerPrice = intent.triggerPrice;
  local.qty = local.desiredQty = intent.qty; local.valid = true;
  local.reduceOnly = trading_core::positionReductionRequested(intent.positionReduction);
  local.state = trading_core::StrategyOrderLocalState::PendingNew;
  owner.serviceFlags[index] = intent.service ? 1u : 0u;
  return true;
}
bool markSimulationWarmupIntent(EngineState& engine, Id id, bool unwind) noexcept {
  const auto slot = evidenceSlot(engine, id, false, engine.activeContextIndex);
  if (slot == kSimulationOrderEvidenceCapacity) return false;
  engine.simulationOrders->serviceFlags[slot] = kSimulationServiceOrder |
      (unwind ? kSimulationWarmupUnwindOrder : kSimulationWarmupOrder);
  return true;
}
bool settleSimulationOrderSubmit(EngineState& engine, Id localId, bool sent) noexcept {
  const auto index = evidenceSlot(engine, localId, false, engine.activeContextIndex);
  if (index == kSimulationOrderEvidenceCapacity) return false;
  const auto& row = engine.simulationOrders->identities[index];
  const auto settled = engine.simulationOrders->store->markPlaceSend(
      {engine.simulationOrders->binding, row.commandId}, sent ? execution::SendDisposition::Complete :
          execution::SendDisposition::NotAttempted) == execution::StateResult::Applied;
  if (settled && !sent) {
    auto& reserve = engine.simulationOrders->spotReservations[index];
    setReservation(engine, reserve, false, 0, 0); setReservation(engine, reserve, true, 0, 0);
  }
  return settled;
}
std::uint64_t simulationCommandId(const EngineState& engine, Id localId, std::uint32_t contextIndex) noexcept {
  auto view = simulationOrderEvidenceView(engine);
  view.contextIndex = contextIndex;
  view.hashPrefix = static_cast<std::uint64_t>(contextIndex) + 1u;
  view.owner.strategyInstanceId = static_cast<std::uint64_t>(contextIndex) + 1u;
  const auto* evidence = trading_core::strategy::canonicalOrderEvidenceIdentity(view, localId, 1u);
  return evidence ? evidence->commandId : 0u;
}
bool beginSimulationCancel(EngineState& engine, Id target, Id cancelLocalId) noexcept {
  const auto index = evidenceSlot(engine, target, false, engine.activeContextIndex);
  if (index == kSimulationOrderEvidenceCapacity) return false;
  const auto command = nextCommand(engine);
  if (!command) return false;
  auto& owner = *engine.simulationOrders;
  const execution::OrderKey key{owner.binding, owner.identities[index].commandId};
  if (owner.store->beginCancel(key, command) != execution::StateResult::Applied ||
      owner.store->markCancelSend(key, command, execution::SendDisposition::Complete) != execution::StateResult::Applied) return false;
  owner.localStates[index].pendingClientIntentId = cancelLocalId;
  return true;
}
bool reserveSpotObservedIntent(EngineState& engine, const trading_core::OrderIntent& intent) noexcept {
  if (engine.simulationEvidenceFailed || !knownAccountingModel(engine) || !engine.feePolicyKnown) return false;
  const auto index = evidenceSlot(engine, intent.clientIntentId, false, engine.activeContextIndex);
  if (index == kSimulationOrderEvidenceCapacity || intent.qty.raw <= 0) return false;
  auto& row = engine.simulationOrders->spotReservations[index];
  if (!isCashAccounting(engine)) {
    const auto& quote = effectiveBookTicker(engine.snapshot);
    const auto price = intent.price.raw > 0 ? intent.price.raw :
        (intent.side.raw == Side::Buy().raw ? quote.ask.px.raw : quote.bid.px.raw);
    return reserveDerivativeOrder(engine, row, intent.side, intent.qty.raw, price, true);
  }
  row.feeBpsE8 = std::max(engine.makerFeeBpsE8, engine.takerFeeBpsE8);
  if (intent.side.raw == Side::Buy().raw) {
    row.estimatedPriceE8 = intent.price.raw > 0 ? intent.price.raw : effectiveBookTicker(engine.snapshot).ask.px.raw;
    std::int64_t cost{};
    const auto cash = engine.sharedAccount ? engine.sharedAccount->observedWalletBalanceE8 : engine.observedWalletBalanceE8;
    const auto held = static_cast<__int128>(reservedQuote(static_cast<const EngineState&>(engine), true)) + protectedQuote(engine, true);
    if (collateralUnavailable(engine, true) || !quoteCost(row.estimatedPriceE8, intent.qty.raw, row.feeBpsE8, cost) ||
        held > cash || cost > static_cast<__int128>(cash) - held) return false;
    return setReservation(engine, row, true, cost, 0);
  } else {
    if (intent.side.raw != Side::Sell().raw || engine.observedReservedBaseE8 > engine.position.netQty.raw ||
        intent.qty.raw > engine.position.netQty.raw - engine.observedReservedBaseE8) return false;
    return setReservation(engine, row, true, 0, intent.qty.raw);
  }
}
bool reserveSpotVenueOrder(EngineState& engine, const OpenOrder& order) noexcept {
  if (engine.simulationEvidenceFailed || !knownAccountingModel(engine) || !engine.feePolicyKnown) return false;
  const auto index = evidenceSlot(engine, order.state.clientIntentId, false, order.contextIndex);
  if (index == kSimulationOrderEvidenceCapacity) return false;
  auto& row = engine.simulationOrders->spotReservations[index];
  if (!isCashAccounting(engine)) {
    const auto& quote = effectiveBookTicker(engine.venueSnapshot);
    const auto price = order.state.price.raw > 0 ? order.state.price.raw :
        (order.state.side.raw == Side::Buy().raw ? quote.ask.px.raw : quote.bid.px.raw);
    return reserveDerivativeOrder(engine, row, order.state.side,
        order.state.qty.raw - order.filledQtyRaw, price, false);
  }
  row.feeBpsE8 = std::max(engine.makerFeeBpsE8, engine.takerFeeBpsE8);
  if (order.state.side.raw == Side::Buy().raw) {
    const auto price = order.state.price.raw > 0 ? order.state.price.raw : effectiveBookTicker(engine.venueSnapshot).ask.px.raw;
    row.venuePriceE8 = price;
    std::int64_t cost{};
    const auto cash = engine.sharedAccount ? engine.sharedAccount->walletBalanceE8 : engine.walletBalanceE8;
    const auto held = static_cast<__int128>(reservedQuote(static_cast<const EngineState&>(engine), false)) - row.venueQuoteE8 + protectedQuote(engine, false);
    if (collateralUnavailable(engine, false) || !quoteCost(price, order.state.qty.raw - order.filledQtyRaw, row.feeBpsE8, cost) ||
        held < 0 || held > cash || cost > static_cast<__int128>(cash) - held) return false;
    if (!setReservation(engine, row, false, cost, 0)) return false;
    if (order.serviceAutounwind && engine.serviceExposureOpen) {
      row.estimatedPriceE8 = price;
      if (!setReservation(engine, row, true, cost, 0)) return false;
    }
  } else {
    const auto held = engine.venueReservedBaseE8 - row.venueBaseE8;
    const auto remaining = order.state.qty.raw - order.filledQtyRaw;
    if (order.state.side.raw != Side::Sell().raw || held < 0 || held > engine.netQtyRaw || remaining > engine.netQtyRaw - held) return false;
    if (!setReservation(engine, row, false, 0, remaining)) return false;
    if (order.serviceAutounwind && engine.serviceExposureOpen)
      if (!setReservation(engine, row, true, 0, remaining)) return false;
  }
  return true;
}
bool spotVenueFillAffordable(const EngineState& engine, const OpenOrder& order,
    std::int64_t price, std::int64_t qty, std::int64_t actualFee) noexcept {
  if (engine.simulationEvidenceFailed || !knownAccountingModel(engine) || !engine.feePolicyKnown) return false;
  const auto index = evidenceSlot(engine, order.state.clientIntentId, false, order.contextIndex);
  if (index == kSimulationOrderEvidenceCapacity || qty <= 0) return false;
  const auto& row = engine.simulationOrders->spotReservations[index];
  if (!isCashAccounting(engine))
    return derivativeVenueFillAffordable(engine, row, order, price, qty, actualFee);
  std::int64_t cashDelta{};
  if (!capital::accounting::spotCashDeltaE8(order.state.side, price, qty, actualFee, cashDelta)) return false;
  if (order.state.side.raw == Side::Buy().raw) {
    const auto cash = engine.sharedAccount ? engine.sharedAccount->walletBalanceE8 : engine.walletBalanceE8;
    const auto otherHeld = static_cast<__int128>(reservedQuote(engine, false)) - row.venueQuoteE8 + protectedQuote(engine, false);
    return !collateralUnavailable(engine, false) && otherHeld >= 0 && otherHeld <= cash && -cashDelta <= static_cast<__int128>(cash) - otherHeld;
  }
  const auto otherHeld = engine.venueReservedBaseE8 - row.venueBaseE8;
  return otherHeld >= 0 && otherHeld <= engine.netQtyRaw && qty <= engine.netQtyRaw - otherHeld;
}
bool spotVenueFillPlanAffordable(const EngineState& engine, const OpenOrder& order,
    const AggressiveFillPlan& plan, std::int64_t feeRate) noexcept {
  if (engine.simulationEvidenceFailed || !knownAccountingModel(engine) || !engine.feePolicyKnown) return false;
  const auto index = evidenceSlot(engine, order.state.clientIntentId, false, order.contextIndex);
  if (index == kSimulationOrderEvidenceCapacity) return false;
  const auto& row = engine.simulationOrders->spotReservations[index];
  if (!isCashAccounting(engine))
    return derivativeVenueFillPlanAffordable(engine, row, order, plan, feeRate);
  const auto cash = engine.sharedAccount ? engine.sharedAccount->walletBalanceE8 : engine.walletBalanceE8;
  const auto otherQuote = static_cast<__int128>(reservedQuote(engine, false)) - row.venueQuoteE8 + protectedQuote(engine, false);
  const auto otherBase = engine.venueReservedBaseE8 - row.venueBaseE8;
  if (otherQuote < 0 || otherQuote > cash || otherBase < 0 || otherBase > engine.netQtyRaw) return false;
  if (order.state.side.raw == Side::Sell().raw && plan.filledQtyE8 > engine.netQtyRaw - otherBase) return false;
  if (order.state.side.raw == Side::Buy().raw && collateralUnavailable(engine, false)) return false;
  __int128 totalDebit = 0;
  for (std::size_t i = 0u; i < plan.count; ++i) {
    const auto& chunk = plan.chunks[i];
    std::int64_t delta{};
    if (!capital::accounting::spotCashDeltaE8(order.state.side, chunk.priceE8, chunk.qtyE8,
          capital::accounting::fillFeeE8(chunk.priceE8, chunk.qtyE8, feeRate), delta)) return false;
    if (delta < 0) totalDebit -= static_cast<__int128>(delta);
    if (totalDebit > cash - otherQuote) return false;
  }
  return true;
}

void updateSpotVenueReservation(EngineState& engine, const OpenOrder& order, std::int64_t remaining) noexcept {
  if (!knownAccountingModel(engine)) { engine.simulationEvidenceFailed = true; return; }
  const auto index = evidenceSlot(engine, order.state.clientIntentId, false, order.contextIndex);
  if (index == kSimulationOrderEvidenceCapacity) { engine.simulationEvidenceFailed = true; return; }
  auto& row = engine.simulationOrders->spotReservations[index];
  if (!isCashAccounting(engine)) {
    updateDerivativeOrderReservation(engine, row, order.state.side, remaining, false); return;
  }
  std::int64_t quote{};
  if (remaining > 0 && order.state.side.raw == Side::Buy().raw &&
      !quoteCost(row.venuePriceE8, remaining, row.feeBpsE8, quote)) { engine.simulationEvidenceFailed = true; return; }
  setReservation(engine, row, false, quote, remaining > 0 && order.state.side.raw == Side::Sell().raw ? remaining : 0);
}
bool deliverSimulationOrderEvent(EngineState& engine, std::size_t index, std::int64_t nowNs) noexcept {
  if (engine.simulationEvidenceFailed || index >= engine.userEvents.size() || index >= engine.userEventDeliveryTsNs.size() ||
      nowNs < engine.userEventDeliveryTsNs[index] || index >= engine.userEventConsumed.size() ||
      index >= engine.userEventContextIndices.size() || index >= engine.userEventClientIntentIds.size() ||
      index >= engine.userEventGenerations.size() || index >= engine.userEventCommandIds.size() ||
      index >= engine.userEventServiceFlags.size() || engine.userEventConsumed[index] ||
      index != engine.deliveredUserEvents ||
      engine.userEventContextIndices[index] != engine.activeContextIndex) return false;
  const auto& event = engine.userEvents[index];
  if (engine.userEventCommandIds[index]) {
    const auto view = simulationOrderEvidenceView(engine);
    const auto* identity = trading_core::strategy::canonicalOrderEvidenceIdentity(
        view, engine.userEventClientIntentIds[index], engine.userEventGenerations[index]);
    SmallBuf expectedClient{};
    if (!identity || identity->commandId != engine.userEventCommandIds[index] ||
        engine.userEventGenerations[index] != 1u || event.exchangeId.raw != engine.exchange.raw ||
        event.kind != cxet::composite::UserDataEventKind::Order ||
        !makeBacktestClientOrderId(engine.exchange, Id{identity->commandId}, expectedClient, false) ||
        !std::equal(std::begin(expectedClient.data), std::end(expectedClient.data), std::begin(event.clientOrderId.data)) ||
        !std::equal(std::begin(engine.symbol.data), std::end(engine.symbol.data), std::begin(event.symbol.data))) return false;
    auto& owner = *engine.simulationOrders;
    execution::OrderObservation observation;
    observation.key = {owner.binding, engine.userEventCommandIds[index]};
    const auto* record = owner.store->find(observation.key);
    if (!record || record != identity->record || event.side.raw != record->request.side.raw ||
        event.qty.raw != record->request.qtyE8) return false;
    if (record->placeSend != execution::SendDisposition::NotAttempted || !record->placeSettled) {
      if (!knownAccountingModel(engine)) return false;
      const auto identitySlot = evidenceSlot(engine, identity->clientIntentId, false, engine.activeContextIndex);
      if (identitySlot == kSimulationOrderEvidenceCapacity) return false;
      const bool serviceFill = (owner.serviceFlags[identitySlot] &
          (kSimulationWarmupOrder | kSimulationWarmupUnwindOrder)) != 0u;
      auto observedServiceQty = engine.observedServiceNetQtyRaw;
      capital::accounting::FillPositionAccountingResult projected{};
      auto walletDelta = event.realizedPnl.raw;
      const auto fillDelta = event.filledQty.raw > record->cumulativeQtyE8
          ? event.filledQty.raw - record->cumulativeQtyE8 : 0;
      if (fillDelta > 0) {
        if (!cxet::composite::userDataFieldPresent(event, cxet::composite::UserDataField::FilledQty) ||
            !cxet::composite::userDataFieldPresent(event, cxet::composite::UserDataField::LastFilledQty) ||
            !cxet::composite::userDataFieldPresent(event, cxet::composite::UserDataField::LastPrice) ||
            !cxet::composite::userDataFieldPresent(event, cxet::composite::UserDataField::Fee) ||
            !cxet::composite::userDataFieldPresent(event, cxet::composite::UserDataField::RealizedPnl) ||
            event.lastFilledQty.raw != fillDelta || event.fee.raw < 0) return false;
        if (fillPositionQuantityOverflows(engine.position.netQty.raw, event.side, fillDelta)) {
          engine.simulationEvidenceFailed = true;
          return false;
        }
        if (serviceFill) {
          const auto delta = event.side.raw == Side::Buy().raw ? fillDelta : -fillDelta;
          if ((delta > 0 && observedServiceQty > std::numeric_limits<std::int64_t>::max() - delta) ||
              (delta < 0 && observedServiceQty < std::numeric_limits<std::int64_t>::min() - delta)) return false;
          observedServiceQty += delta;
        }
        projected = capital::accounting::applyFillPositionAccounting(
            {engine.position.netQty.raw, engine.position.entryPrice.raw}, event.side, event.lastPrice.raw, fillDelta, 0);
        if (!projected.applied || trading_core::saturatingAddI64(projected.grossRealizedPnlE8, -event.fee.raw) != event.realizedPnl.raw) return false;
        if (isCashAccounting(engine)) {
          if (projected.position.netQtyRaw < 0 || !capital::accounting::spotCashDeltaE8(
              event.side, event.lastPrice.raw, fillDelta, event.fee.raw, walletDelta)) return false;
        }
      }
      switch (event.orderStatus) {
        case canon::OrderStatus::Open: observation.status = execution::VenueStatus::Ack; break;
        case canon::OrderStatus::PartiallyFilled: observation.status = execution::VenueStatus::Partial; break;
        case canon::OrderStatus::Closed: observation.status = event.filledQty.raw == record->request.qtyE8
            ? execution::VenueStatus::Filled : execution::VenueStatus::Closed; break;
        case canon::OrderStatus::Canceled: observation.status = execution::VenueStatus::Canceled; break;
        case canon::OrderStatus::Rejected: observation.status = execution::VenueStatus::Rejected; break;
        default: return false;
      }
      observation.cumulativeKnown = cxet::composite::userDataFieldPresent(event, cxet::composite::UserDataField::FilledQty);
      observation.cumulativeQtyE8 = event.filledQty.raw;
      // This producer models complete per-order fills and fees, and applies
      // them to the observed account in this same delivery operation.
      observation.terminalQuantityFinal = observation.cumulativeKnown &&
          (event.orderStatus == canon::OrderStatus::Closed ||
           event.orderStatus == canon::OrderStatus::Canceled ||
           event.orderStatus == canon::OrderStatus::Rejected);
      if (event.orderStatus == canon::OrderStatus::Canceled) {
        observation.cancelCommandId = record->cancelCommandId;
        observation.cancelStatus = record->cancelCommandId ? execution::CancelStatus::Confirmed : execution::CancelStatus::None;
      }
      const auto observed = owner.store->observe(observation);
      if (observed.result != execution::StateResult::Applied && observed.result != execution::StateResult::Duplicate) return false;
      if (observed.fillDeltaE8 != fillDelta) return false;
      if (observation.status == execution::VenueStatus::Ack ||
          observation.status == execution::VenueStatus::Partial ||
          observation.status == execution::VenueStatus::Filled) {
        engine.lastOrderPathAckMono.raw = static_cast<std::uint64_t>(nowNs > 0 ? nowNs : 0);
        engine.lastWarmupSkipReason = 0u;
      }
      if (engine.warmupInFlight && engine.warmupClientIntentId.raw == identity->clientIntentId.raw &&
          engine.warmupContextIndex == engine.activeContextIndex &&
          (record->venueObserved || record->terminal())) engine.warmupInFlight = false;
      if (fillDelta > 0) {
        engine.observedServiceNetQtyRaw = observedServiceQty;
        recordSpotValuationMark(engine, true);
        engine.position.netQty.raw = projected.position.netQtyRaw;
        engine.position.entryPrice.raw = projected.position.entryPriceE8;
        engine.position.lastPrice = event.lastPrice;
        engine.position.lastUpdateUtc.raw = event.updateTime.raw;
        engine.position.valid = true;
        engine.position.unrealizedPnl.raw = observedUnrealizedPnlE8(engine);
        engine.observedWalletBalanceE8 = trading_core::saturatingAddI64(engine.observedWalletBalanceE8, walletDelta);
        if (engine.sharedAccount) {
          auto& account = *engine.sharedAccount;
          if (!account.observedInitialized) { account.observedWalletBalanceE8 = account.initialBalanceE8; account.observedInitialized = true; }
          account.observedWalletBalanceE8 = trading_core::saturatingAddI64(account.observedWalletBalanceE8, walletDelta);
          ++account.revision;
        }
        refreshAccountPositionValue(engine, true);
      }
      if (isCashAccounting(engine)) {
        const auto slot = evidenceSlot(engine, identity->clientIntentId, false, engine.activeContextIndex);
        if (slot == kSimulationOrderEvidenceCapacity) return false;
        auto& reserve = owner.spotReservations[slot];
        const auto remaining = record->terminal() ? 0 : record->request.qtyE8 - record->cumulativeQtyE8;
        std::int64_t heldQuote{};
        if (remaining > 0 && event.side.raw == Side::Buy().raw &&
            !quoteCost(reserve.estimatedPriceE8, remaining, reserve.feeBpsE8, heldQuote)) return false;
        if (!setReservation(engine, reserve, true, heldQuote,
            remaining > 0 && event.side.raw == Side::Sell().raw ? remaining : 0)) return false;
      } else {
        auto& reserve = owner.spotReservations[identitySlot];
        const auto remaining = record->terminal() ? 0 : record->request.qtyE8 - record->cumulativeQtyE8;
        updateDerivativeOrderReservation(engine, reserve, event.side, remaining, true);
      }
    }
  }
  ++engine.deliveredUserEvents; engine.lastDeliveredUserEvent = index;
  engine.lastReplayMonotonicNs = std::max(engine.lastReplayMonotonicNs, nowNs);
  engine.userEventConsumed[index] = 1u; bumpContextRevision(engine);
  if (engine.userEventCommandIds[index]) {
    auto& owner = *engine.simulationOrders;
    const auto slot = evidenceSlot(engine, engine.userEventClientIntentIds[index],
        false, engine.activeContextIndex);
    if (slot == kSimulationOrderEvidenceCapacity ||
        owner.deliveryCut == std::numeric_limits<std::uint64_t>::max()) return false;
    owner.deliveredCuts[slot] = ++owner.deliveryCut;
  }
  return true;
}

std::size_t reclaimSimulationOrderEvidence(EngineState& engine,
    SimulationEvidenceRetirementProof proof) noexcept {
  if (!proof.released || !engine.simulationOrders ||
      !engine.simulationOrders->ready) return 0u;
  auto& owner = *engine.simulationOrders;
  std::size_t reclaimed{};
  for (std::size_t slot = 0u; slot < owner.identities.size(); ++slot) {
    const auto identity = owner.identities[slot];
    if (!identity.clientIntentId.raw || !identity.record ||
        !owner.deliveredCuts[slot]) continue;
    const execution::OrderKey key{owner.binding, identity.commandId};
    if (!owner.store->validateRecord(identity.record, identity.recordGeneration, key) ||
        owner.store->reclaimable(key, true) != execution::StateResult::Applied) continue;
    const auto& reservation = owner.spotReservations[slot];
    if (reservation.observedQuoteE8 || reservation.venueQuoteE8 ||
        reservation.observedBaseE8 || reservation.venueBaseE8) continue;
    bool borrowed = false;
    for (const auto& order : engine.open)
      borrowed = borrowed || (order.state.valid && order.contextIndex == identity.contextIndex &&
          order.state.clientIntentId.raw == identity.clientIntentId.raw);
    for (const auto& action : engine.pending)
      borrowed = borrowed || (action.contextIndex == identity.contextIndex &&
          (action.targetClientIntentId.raw == identity.clientIntentId.raw ||
           action.commandClientIntentId.raw == identity.clientIntentId.raw));
    for (std::size_t event = 0u; event < engine.userEventCommandIds.size(); ++event)
      if (engine.userEventCommandIds[event] == identity.commandId &&
          (event >= engine.userEventConsumed.size() || !engine.userEventConsumed[event])) borrowed = true;
    if (borrowed || !proof.released(proof.owner, identity.contextIndex,
        identity.clientIntentId, owner.deliveredCuts[slot])) continue;
    if (owner.store->reclaim(key) != execution::StateResult::Applied) continue;
    // A generation-only tombstone preserves every colliding lookup chain.
    owner.identities[slot] = {}; owner.identities[slot].generation = 1u;
    owner.localStates[slot] = {}; owner.serviceFlags[slot] = 0u;
    owner.spotReservations[slot] = {}; owner.deliveredCuts[slot] = 0u;
    ++reclaimed;
  }
  return reclaimed;
}
}
