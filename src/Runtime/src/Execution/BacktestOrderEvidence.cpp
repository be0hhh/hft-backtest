#include "BacktestOrderEvidence.hpp"
#include "../Engine/BacktestEngine.hpp"
#include "BacktestClientOrderId.hpp"
#include "trading_core/Strategy/Orders/OrderDecisionCursor.hpp"
#include "trading_core/Algorithm/Math/Integer/Saturating.hpp"
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
  for (std::size_t probe = 0u; probe < trading_core::strategy::kOrderEvidenceMaxProbes; ++probe) {
    const auto index = (first + probe) & (kSimulationOrderEvidenceCapacity - 1u);
    const auto& row = engine.simulationOrders->identities[index];
    if (!row.clientIntentId.raw) return empty ? index : kSimulationOrderEvidenceCapacity;
    if (row.clientIntentId.raw == id.raw && row.contextIndex == contextIndex)
      return empty ? kSimulationOrderEvidenceCapacity : index;
  }
  return kSimulationOrderEvidenceCapacity;
}
std::int64_t& reservedQuote(EngineState& engine, bool observed) noexcept {
  if (engine.sharedAccount) return observed ? engine.sharedAccount->observedReservedQuoteE8 : engine.sharedAccount->venueReservedQuoteE8;
  return observed ? engine.observedReservedQuoteE8 : engine.venueReservedQuoteE8;
}
std::int64_t reservedQuote(const EngineState& engine, bool observed) noexcept {
  if (engine.sharedAccount) return observed ? engine.sharedAccount->observedReservedQuoteE8 : engine.sharedAccount->venueReservedQuoteE8;
  return observed ? engine.observedReservedQuoteE8 : engine.venueReservedQuoteE8;
}
bool quoteCost(std::int64_t price, std::int64_t qty, std::int64_t feeRate, std::int64_t& out) noexcept {
  if (!qty) { out = 0; return true; }
  if (qty < 0 || price <= 0) return false;
  const auto principal = trading_core::quoteNotionalE8(price, qty);
  const auto fee = trading_core::fillFeeE8(price, qty, feeRate);
  if (principal == std::numeric_limits<std::int64_t>::max() ||
      principal > std::numeric_limits<std::int64_t>::max() - fee) return false;
  out = principal + fee; return true;
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
  owner.store.emplace(owner.records, binding);
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
  return view;
}
bool installSimulationOrderIntent(EngineState& engine, const trading_core::OrderIntent& intent) noexcept {
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
  if (!record) return false;
  owner.identities[index] = {intent.clientIntentId, 1u, command, record, engine.activeContextIndex};
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
  if (settled && !sent && engine.market.raw == canon::kMarketTypeSpot.raw) {
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
  if (engine.market.raw != canon::kMarketTypeSpot.raw) return true;
  const auto index = evidenceSlot(engine, intent.clientIntentId, false, engine.activeContextIndex);
  if (index == kSimulationOrderEvidenceCapacity || intent.qty.raw <= 0) return false;
  auto& row = engine.simulationOrders->spotReservations[index];
  row.feeBpsE8 = std::max(engine.makerFeeBpsE8, engine.takerFeeBpsE8);
  if (intent.side.raw == Side::Buy().raw) {
    row.estimatedPriceE8 = intent.price.raw > 0 ? intent.price.raw : effectiveBookTicker(engine.snapshot).ask.px.raw;
    std::int64_t cost{};
    const auto cash = engine.sharedAccount ? engine.sharedAccount->observedWalletBalanceE8 : engine.observedWalletBalanceE8;
    const auto held = reservedQuote(static_cast<const EngineState&>(engine), true);
    if (!quoteCost(row.estimatedPriceE8, intent.qty.raw, row.feeBpsE8, cost) || held > cash || cost > cash - held) return false;
    return setReservation(engine, row, true, cost, 0);
  } else {
    if (intent.side.raw != Side::Sell().raw || engine.observedReservedBaseE8 > engine.position.netQty.raw ||
        intent.qty.raw > engine.position.netQty.raw - engine.observedReservedBaseE8) return false;
    return setReservation(engine, row, true, 0, intent.qty.raw);
  }
}
bool reserveSpotVenueOrder(EngineState& engine, const OpenOrder& order) noexcept {
  if (engine.market.raw != canon::kMarketTypeSpot.raw) return true;
  const auto index = evidenceSlot(engine, order.state.clientIntentId, false, order.contextIndex);
  if (index == kSimulationOrderEvidenceCapacity) return false;
  auto& row = engine.simulationOrders->spotReservations[index];
  row.feeBpsE8 = std::max(engine.makerFeeBpsE8, engine.takerFeeBpsE8);
  if (order.state.side.raw == Side::Buy().raw) {
    const auto price = order.state.price.raw > 0 ? order.state.price.raw : effectiveBookTicker(engine.venueSnapshot).ask.px.raw;
    row.venuePriceE8 = price;
    std::int64_t cost{};
    const auto cash = engine.sharedAccount ? engine.sharedAccount->walletBalanceE8 : engine.walletBalanceE8;
    const auto held = reservedQuote(static_cast<const EngineState&>(engine), false) - row.venueQuoteE8;
    if (!quoteCost(price, order.state.qty.raw - order.filledQtyRaw, row.feeBpsE8, cost) || held < 0 || held > cash || cost > cash - held) return false;
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
    std::int64_t price, std::int64_t qty, std::int64_t feeRate) noexcept {
  if (engine.market.raw != canon::kMarketTypeSpot.raw) return true;
  const auto index = evidenceSlot(engine, order.state.clientIntentId, false, order.contextIndex);
  if (index == kSimulationOrderEvidenceCapacity || qty <= 0) return false;
  const auto& row = engine.simulationOrders->spotReservations[index];
  std::int64_t cashDelta{};
  if (!spotCashDeltaE8(order.state.side, price, qty, trading_core::fillFeeE8(price, qty, feeRate), cashDelta)) return false;
  if (order.state.side.raw == Side::Buy().raw) {
    const auto cash = engine.sharedAccount ? engine.sharedAccount->walletBalanceE8 : engine.walletBalanceE8;
    const auto otherHeld = reservedQuote(engine, false) - row.venueQuoteE8;
    return otherHeld >= 0 && otherHeld <= cash && -cashDelta <= cash - otherHeld;
  }
  const auto otherHeld = engine.venueReservedBaseE8 - row.venueBaseE8;
  return otherHeld >= 0 && otherHeld <= engine.netQtyRaw && qty <= engine.netQtyRaw - otherHeld;
}
void updateSpotVenueReservation(EngineState& engine, const OpenOrder& order, std::int64_t remaining) noexcept {
  if (engine.market.raw != canon::kMarketTypeSpot.raw) return;
  const auto index = evidenceSlot(engine, order.state.clientIntentId, false, order.contextIndex);
  if (index == kSimulationOrderEvidenceCapacity) { engine.simulationEvidenceFailed = true; return; }
  auto& row = engine.simulationOrders->spotReservations[index];
  std::int64_t quote{};
  if (remaining > 0 && order.state.side.raw == Side::Buy().raw &&
      !quoteCost(row.venuePriceE8, remaining, row.feeBpsE8, quote)) { engine.simulationEvidenceFailed = true; return; }
  setReservation(engine, row, false, quote, remaining > 0 && order.state.side.raw == Side::Sell().raw ? remaining : 0);
}
bool deliverSimulationOrderEvent(EngineState& engine, std::size_t index, std::int64_t nowNs) noexcept {
  if (index >= engine.userEvents.size() || index >= engine.userEventDeliveryTsNs.size() ||
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
      const auto identitySlot = evidenceSlot(engine, identity->clientIntentId, false, engine.activeContextIndex);
      if (identitySlot == kSimulationOrderEvidenceCapacity) return false;
      const bool serviceFill = (owner.serviceFlags[identitySlot] &
          (kSimulationWarmupOrder | kSimulationWarmupUnwindOrder)) != 0u;
      auto observedServiceQty = engine.observedServiceNetQtyRaw;
      trading_core::FillPositionAccountingResult projected{};
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
        if (serviceFill) {
          const auto delta = event.side.raw == Side::Buy().raw ? fillDelta : -fillDelta;
          if ((delta > 0 && observedServiceQty > std::numeric_limits<std::int64_t>::max() - delta) ||
              (delta < 0 && observedServiceQty < std::numeric_limits<std::int64_t>::min() - delta)) return false;
          observedServiceQty += delta;
        }
        projected = trading_core::applyFillPositionAccounting(
            {engine.position.netQty.raw, engine.position.entryPrice.raw}, event.side, event.lastPrice.raw, fillDelta, 0);
        if (!projected.applied || trading_core::saturatingAddI64(projected.grossRealizedPnlE8, -event.fee.raw) != event.realizedPnl.raw) return false;
        if (engine.market.raw == canon::kMarketTypeSpot.raw) {
          const auto cash = engine.sharedAccount ? engine.sharedAccount->observedWalletBalanceE8 : engine.observedWalletBalanceE8;
          if (projected.position.netQtyRaw < 0 || !spotCashDeltaE8(event.side, event.lastPrice.raw, fillDelta, event.fee.raw, walletDelta) ||
              (walletDelta < 0 && cash < -walletDelta)) return false;
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
      if (engine.market.raw == canon::kMarketTypeSpot.raw) {
        const auto slot = evidenceSlot(engine, identity->clientIntentId, false, engine.activeContextIndex);
        if (slot == kSimulationOrderEvidenceCapacity) return false;
        auto& reserve = owner.spotReservations[slot];
        const auto remaining = record->terminal() ? 0 : record->request.qtyE8 - record->cumulativeQtyE8;
        std::int64_t heldQuote{};
        if (remaining > 0 && event.side.raw == Side::Buy().raw &&
            !quoteCost(reserve.estimatedPriceE8, remaining, reserve.feeBpsE8, heldQuote)) return false;
        if (!setReservation(engine, reserve, true, heldQuote,
            remaining > 0 && event.side.raw == Side::Sell().raw ? remaining : 0)) return false;
      }
    }
  }
  ++engine.deliveredUserEvents; engine.lastDeliveredUserEvent = index;
  engine.lastReplayMonotonicNs = std::max(engine.lastReplayMonotonicNs, nowNs);
  engine.userEventConsumed[index] = 1u; bumpContextRevision(engine);
  return true;
}
}
