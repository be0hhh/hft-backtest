#include "BacktestOrderEvidence.hpp"
#include "../Engine/BacktestEngine.hpp"
#include "capital/Finance.hpp"
#include "capital/AccountState/AccountState.hpp"
#include "trading_core/Contracts/Numeric/Integer/WideDivision.hpp"

#include <algorithm>
#include <limits>

namespace hft_backtest::detail {
namespace {
bool narrow(__int128 value, std::int64_t& out) noexcept {
  if (value < 0 || value > INT64_MAX) return false;
  out = static_cast<std::int64_t>(value); return true;
}
std::int64_t& pendingTotal(EngineState& engine, bool observed) noexcept {
  return engine.sharedAccount ? (observed ? engine.sharedAccount->observedReservedQuoteE8 : engine.sharedAccount->venueReservedQuoteE8) :
      (observed ? engine.observedReservedQuoteE8 : engine.venueReservedQuoteE8);
}
std::int64_t pendingTotal(const EngineState& engine, bool observed) noexcept {
  return engine.sharedAccount ? (observed ? engine.sharedAccount->observedReservedQuoteE8 : engine.sharedAccount->venueReservedQuoteE8) :
      (observed ? engine.observedReservedQuoteE8 : engine.venueReservedQuoteE8);
}
std::int64_t protectedTotal(const EngineState& engine, bool observed) noexcept {
  const auto& own = engine.simulationOrders->collateral;
  return engine.sharedAccount ? (observed ? engine.sharedAccount->observedProtectedCollateralE8 : engine.sharedAccount->venueProtectedCollateralE8) :
      (observed ? own.observedProtectedE8 : own.venueProtectedE8);
}
bool unavailable(const EngineState& engine, bool observed) noexcept {
  const auto& own = engine.simulationOrders->collateral;
  return engine.sharedAccount ? (observed ? engine.sharedAccount->observedCollateralUnavailableSources : engine.sharedAccount->venueCollateralUnavailableSources) != 0u :
      (observed ? own.observedUnavailable : own.venueUnavailable);
}
std::int64_t cash(const EngineState& engine, bool observed) noexcept {
  return engine.sharedAccount ? (observed ? engine.sharedAccount->observedWalletBalanceE8 : engine.sharedAccount->walletBalanceE8) :
      (observed ? engine.observedWalletBalanceE8 : engine.walletBalanceE8);
}
bool marginFor(const SimulationDerivativeCollateral& model, std::int64_t quantity,
    std::int64_t& amount) noexcept {
  amount = 0;
  if (!quantity) return true;
  if (!model.known || model.initialMarginE8 <= 0 || model.basisQuantityE8 <= 0 || quantity == INT64_MIN) return false;
  const auto magnitude = static_cast<std::uint64_t>(quantity < 0 ? -quantity : quantity);
  const auto result = trading_core::ceilDivU128ByU64(
      static_cast<unsigned __int128>(magnitude) * static_cast<std::uint64_t>(model.initialMarginE8),
      static_cast<std::uint64_t>(model.basisQuantityE8));
  if (result > static_cast<unsigned __int128>(INT64_MAX)) return false;
  amount = static_cast<std::int64_t>(result); return true;
}
bool protectedPosition(const EngineState& engine, bool observed, std::int64_t quantity,
    std::int64_t entry, std::int64_t& amount) noexcept {
  if (!marginFor(engine.simulationOrders->collateral, quantity, amount)) return false;
  if (!quantity) return true;
  if (!engine.simulationOrders->collateral.authorityCurrent) return false;
  const auto& quote = effectiveBookTicker(observed ? engine.snapshot : engine.venueSnapshot);
  if (entry <= 0 || quote.bid.px.raw <= 0 || quote.ask.px.raw <= 0 || quote.bid.px.raw > quote.ask.px.raw) return false;
  const auto pnl = capital::accounting::markToMarketPnlE8({quantity, entry}, quote.bid.px.raw, quote.ask.px.raw);
  return narrow(static_cast<__int128>(amount) - std::min<std::int64_t>(0, pnl), amount);
}
bool pendingAmount(const EngineState& engine, const SpotOrderReservation& row,
    std::int64_t quantity, std::int64_t price, bool reducing, bool observed, std::int64_t& amount) noexcept {
  amount = 0;
  if (!quantity) return true;
  if (quantity < 0 || price <= 0 || row.feeBpsE8 < 0) return false;
  std::int64_t margin{};
  if (!reducing && !marginFor(engine.simulationOrders->collateral, quantity, margin)) return false;
  const auto fees = capital::accounting::fillFeeE8(price, quantity, row.feeBpsE8);
  std::int64_t executionLoss{};
  if (!reducing) {
    const auto& quote = effectiveBookTicker(observed ? engine.snapshot : engine.venueSnapshot);
    if (quote.bid.px.raw <= 0 || quote.ask.px.raw <= 0 || quote.bid.px.raw > quote.ask.px.raw ||
        (row.collateralSide.raw != Side::Buy().raw && row.collateralSide.raw != Side::Sell().raw)) return false;
    const auto signedQuantity = row.collateralSide.raw == Side::Buy().raw ? quantity : -quantity;
    const auto pnl = capital::accounting::markToMarketPnlE8({signedQuantity, price}, quote.bid.px.raw, quote.ask.px.raw);
    if (!narrow(-static_cast<__int128>(std::min<std::int64_t>(0, pnl)), executionLoss)) return false;
  }
  return fees >= 0 && narrow(static_cast<__int128>(margin) + fees + executionLoss, amount);
}
bool setPending(EngineState& engine, SpotOrderReservation& row, bool observed,
    std::int64_t quote, std::int64_t reduction) noexcept {
  auto& oldQuote = observed ? row.observedQuoteE8 : row.venueQuoteE8;
  auto& oldBase = observed ? row.observedBaseE8 : row.venueBaseE8;
  auto& totalQuote = pendingTotal(engine, observed);
  auto& totalBase = observed ? engine.observedReservedBaseE8 : engine.venueReservedBaseE8;
  std::int64_t nextQuote{}, nextBase{};
  if (quote < 0 || reduction < 0 || totalQuote < oldQuote || totalBase < oldBase ||
      !narrow(static_cast<__int128>(totalQuote) - oldQuote + quote, nextQuote) ||
      !narrow(static_cast<__int128>(totalBase) - oldBase + reduction, nextBase)) return false;
  totalQuote = nextQuote; totalBase = nextBase; oldQuote = quote; oldBase = reduction;
  return true;
}
bool reduces(Side side, std::int64_t quantity, std::int64_t position,
    std::int64_t otherReductions) noexcept {
  if (quantity <= 0 || position == INT64_MIN || otherReductions < 0) return false;
  const auto magnitude = position < 0 ? -position : position;
  return ((position > 0 && side.raw == Side::Sell().raw) || (position < 0 && side.raw == Side::Buy().raw)) &&
      otherReductions <= magnitude && quantity <= magnitude - otherReductions;
}
bool funded(const EngineState& engine, bool observed, std::int64_t ownPending,
    std::int64_t required) noexcept {
  if (required < 0 || unavailable(engine, observed) || pendingTotal(engine, observed) < ownPending) return false;
  std::int64_t protectedCash{}, free{};
  if (!narrow(static_cast<__int128>(protectedTotal(engine, observed)) + pendingTotal(engine, observed) - ownPending, protectedCash) ||
      capital::financingCapacity(capital::Model::LinearFuture, cash(engine, observed), 0, 0,
          protectedCash, true, false, free) != capital::Result::Applied) return false;
  capital::accounting::StrategyCapitalState available{};
  available.accountId = engine.simulationOrders->binding.accountId;
  available.currency = engine.simulationOrders->collateral.currency;
  available.freeCapitalE8 = free; available.epoch = 1u;
  available.known = available.fresh = true;
  std::int64_t usable{};
  return capital::accounting::availableCapital(available, usable) && required <= usable;
}
bool projectedAffordable(const EngineState& engine, const SpotOrderReservation& row,
    const OpenOrder& order, const capital::FinancialState& after,
    std::int64_t additionalFilled) noexcept {
  if (row.venueReducing) return reduces(order.state.side, additionalFilled, engine.netQtyRaw,
      engine.venueReservedBaseE8 - row.venueBaseE8);
  if (!engine.simulationOrders->collateral.authorityCurrent || unavailable(engine, false)) return false;
  std::int64_t projectedProtected{}, remainingCharge{}, protectedCash{};
  const auto remaining = order.state.qty.raw - order.filledQtyRaw - additionalFilled;
  const auto& collateral = engine.simulationOrders->collateral;
  if (remaining < 0 || !protectedPosition(engine, false, after.quantityE8, after.averagePriceE8, projectedProtected) ||
      !pendingAmount(engine, row, remaining, row.venuePriceE8, false, false, remainingCharge) ||
      protectedTotal(engine, false) < collateral.venueProtectedE8 ||
      pendingTotal(engine, false) < row.venueQuoteE8 ||
      !narrow(static_cast<__int128>(protectedTotal(engine, false)) - collateral.venueProtectedE8 + projectedProtected +
          pendingTotal(engine, false) - row.venueQuoteE8 + remainingCharge, protectedCash)) return false;
  std::int64_t capacity{};
  return capital::financingCapacity(capital::Model::LinearFuture, after.cashE8, 0, 0,
      protectedCash, true, false, capacity) == capital::Result::Applied && capacity >= 0;
}
}

bool configureSimulationDerivativeCollateral(EngineState& engine, std::int64_t amount,
    std::int64_t basis, std::uint32_t currency) noexcept {
  if (isCashAccounting(engine)) return true;
  if (!isLinearAccounting(engine)) return false;
  if (!engine.simulationOrders || amount <= 0 || basis <= 0 || !currency ||
      (engine.sharedAccount && engine.sharedAccount->quoteAssetId != currency)) return false;
  auto& model = engine.simulationOrders->collateral;
  if (model.known) return model.initialMarginE8 == amount && model.basisQuantityE8 == basis && model.currency == currency;
  model.initialMarginE8 = amount; model.basisQuantityE8 = basis; model.currency = currency;
  model.known = model.authorityCurrent = true;
  refreshSimulationDerivativeCollateral(engine, true); refreshSimulationDerivativeCollateral(engine, false);
  return !engine.simulationEvidenceFailed;
}

void setSimulationDerivativeAuthorityCurrent(EngineState& engine, bool current) noexcept {
  if (!isLinearAccounting(engine) || !engine.simulationOrders) return;
  auto& model = engine.simulationOrders->collateral;
  model.authorityCurrent = current && model.known;
  // A nonflat source with withdrawn/changed financial facts cannot certify
  // the account's remaining collateral. Retain its old charge and fence new
  // risk account-wide; current-run fills and bounded reductions still apply.
  refreshSimulationDerivativeCollateral(engine, true);
  refreshSimulationDerivativeCollateral(engine, false);
}

void refreshSimulationDerivativeCollateral(EngineState& engine, bool observed) noexcept {
  if (!isLinearAccounting(engine) || !engine.simulationOrders) return;
  auto& own = engine.simulationOrders->collateral;
  auto& old = observed ? own.observedProtectedE8 : own.venueProtectedE8;
  auto& oldUnavailable = observed ? own.observedUnavailable : own.venueUnavailable;
  const auto quantity = observed ? engine.position.netQty.raw : engine.netQtyRaw;
  const auto entry = observed ? engine.position.entryPrice.raw : engine.entryPriceE8;
  std::int64_t required{};
  const bool known = protectedPosition(engine, observed, quantity, entry, required);
  if (!known) required = std::max(old, required); // Retain known GO and never refund an uncertain charge.
  if (engine.sharedAccount) {
    auto& total = observed ? engine.sharedAccount->observedProtectedCollateralE8 : engine.sharedAccount->venueProtectedCollateralE8;
    auto& missing = observed ? engine.sharedAccount->observedCollateralUnavailableSources : engine.sharedAccount->venueCollateralUnavailableSources;
    std::int64_t next{};
    if (total < old || !narrow(static_cast<__int128>(total) - old + required, next) ||
        (known && oldUnavailable && !missing) || (!known && !oldUnavailable && missing == UINT32_MAX)) {
      engine.simulationEvidenceFailed = true; return;
    }
    total = next;
    if (known && oldUnavailable) --missing;
    if (!known && !oldUnavailable) ++missing;
  }
  old = required; oldUnavailable = !known;
}

bool reserveDerivativeOrder(EngineState& engine, SpotOrderReservation& row,
    Side side, std::int64_t quantity, std::int64_t price, bool observed) noexcept {
  if (!isLinearAccounting(engine) || !engine.feePolicyKnown) return false;
  refreshSimulationDerivativeCollateral(engine, observed);
  if (engine.simulationEvidenceFailed || quantity <= 0 || !engine.position.valid ||
      (engine.sharedAccount && !engine.sharedAccount->observedInitialized)) return false;
  const auto position = observed ? engine.position.netQty.raw : engine.netQtyRaw;
  const auto ownBase = observed ? row.observedBaseE8 : row.venueBaseE8;
  const auto totalBase = observed ? engine.observedReservedBaseE8 : engine.venueReservedBaseE8;
  const bool reducing = reduces(side, quantity, position, totalBase - ownBase);
  if (!reducing && !engine.simulationOrders->collateral.authorityCurrent) return false;
  if (!observed && row.observedReducing && !reducing) return false;
  row.feeBpsE8 = std::max(engine.makerFeeBpsE8, engine.takerFeeBpsE8);
  row.collateralSide = side;
  std::int64_t amount{};
  if (!pendingAmount(engine, row, quantity, price, reducing, observed, amount) ||
      (!reducing && !funded(engine, observed, observed ? row.observedQuoteE8 : row.venueQuoteE8, amount))) return false;
  if (!setPending(engine, row, observed, amount, reducing ? quantity : 0)) return false;
  (observed ? row.estimatedPriceE8 : row.venuePriceE8) = price;
  (observed ? row.observedReducing : row.venueReducing) = reducing;
  return true;
}

bool derivativeVenueFillAffordable(const EngineState& engine, const SpotOrderReservation& row,
    const OpenOrder& order, std::int64_t price, std::int64_t quantity, std::int64_t actualFee) noexcept {
  if (!isLinearAccounting(engine) || !engine.feePolicyKnown) return false;
  capital::FillProjection projected{};
  return capital::projectFill(capital::Model::LinearFuture,
      {engine.netQtyRaw, engine.entryPriceE8, cash(engine, false), 0}, order.state.side,
      quantity, price, actualFee, projected) == capital::Result::Applied &&
      projectedAffordable(engine, row, order, projected.after, quantity);
}

bool derivativeVenueFillPlanAffordable(const EngineState& engine, const SpotOrderReservation& row,
    const OpenOrder& order, const AggressiveFillPlan& plan, std::int64_t feeRate) noexcept {
  if (!isLinearAccounting(engine) || !engine.feePolicyKnown) return false;
  capital::FinancialState current{engine.netQtyRaw, engine.entryPriceE8, cash(engine, false), 0};
  std::int64_t filled{};
  for (std::size_t index = 0u; index < plan.count; ++index) {
    const auto& chunk = plan.chunks[index]; capital::FillProjection projected{};
    const auto fee = capital::accounting::fillFeeE8(chunk.priceE8, chunk.qtyE8, feeRate);
    if (chunk.qtyE8 <= 0 || !narrow(static_cast<__int128>(filled) + chunk.qtyE8, filled) ||
        capital::projectFill(capital::Model::LinearFuture, current, order.state.side, chunk.qtyE8,
            chunk.priceE8, fee, projected) != capital::Result::Applied ||
        !projectedAffordable(engine, row, order, projected.after, filled)) return false;
    current = projected.after;
  }
  return filled == plan.filledQtyE8;
}

void updateDerivativeOrderReservation(EngineState& engine, SpotOrderReservation& row,
    Side side, std::int64_t remaining, bool observed) noexcept {
  if (!isLinearAccounting(engine)) { engine.simulationEvidenceFailed = true; return; }
  (void)side;
  const bool reducing = observed ? row.observedReducing : row.venueReducing;
  std::int64_t amount{};
  if (!pendingAmount(engine, row, remaining, observed ? row.estimatedPriceE8 : row.venuePriceE8, reducing, observed, amount)) {
    // An accepted partial fill is still a fact when its current mark is
    // unavailable. Keep the full prior pending charge; the account fence
    // below prevents that uncertainty from authorizing another increase.
    refreshSimulationDerivativeCollateral(engine, observed); return;
  }
  if (!setPending(engine, row, observed, amount, reducing ? remaining : 0)) engine.simulationEvidenceFailed = true;
  refreshSimulationDerivativeCollateral(engine, observed);
}
}
