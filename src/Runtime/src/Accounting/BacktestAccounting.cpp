#include "capital/Accounting/FillPositionAccounting.hpp"
#include "trading_core/Algorithm/Math/Integer/Saturating.hpp"
#include "../Engine/BacktestEngine.hpp"

#include <limits>

#include "trading_core/Algorithm/Math/PriceQuantity/NotionalQty.hpp"

namespace hft_backtest::detail {

void initializeAccounting(EngineState& engine, const BacktestRunRequest& request) noexcept {
  if (!prepareSimulationOrderEvidence(engine)) engine.simulationEvidenceFailed = true;
  engine.initialBalanceE8 = request.initialBalanceE8;
  engine.walletBalanceE8 = request.initialBalanceE8;
  engine.observedWalletBalanceE8 = request.initialBalanceE8;
  if (engine.sharedAccount && !engine.sharedAccount->observedInitialized) {
    engine.sharedAccount->observedWalletBalanceE8 = engine.sharedAccount->initialBalanceE8;
    engine.sharedAccount->observedInitialized = true;
  }
  engine.makerFeeBpsE8 = request.makerFeeBpsE8;
  engine.takerFeeBpsE8 = request.takerFeeBpsE8;
  engine.position.netQty.raw = engine.netQtyRaw;
  engine.position.entryPrice.raw = engine.entryPriceE8;
  engine.position.unrealizedPnl.raw = 0;
  engine.position.valid = true;
  bumpContextRevision(engine);
}

capital::accounting::FillPositionAccountingResult updatePosition(
    EngineState& engine,
    Side side,
    std::int64_t priceE8,
    std::int64_t qtyE8,
    std::int64_t tsNs,
    std::int64_t feeBpsE8) noexcept {
  (void)tsNs;
  const capital::accounting::FillPositionAccountingResult result =
      capital::accounting::applyFillPositionAccounting(
          capital::accounting::FillPositionAccountingState{engine.netQtyRaw,
                                                     engine.entryPriceE8},
          side,
          priceE8,
          qtyE8,
          feeBpsE8);
  if (!result.applied) return result;

  auto walletDelta = result.netRealizedPnlE8;
  if (engine.market.raw == canon::kMarketTypeSpot.raw) {
    const auto cash = engine.sharedAccount ? engine.sharedAccount->walletBalanceE8 : engine.walletBalanceE8;
    if (result.position.netQtyRaw < 0 || !capital::accounting::spotCashDeltaE8(side, priceE8, qtyE8, result.feeE8, walletDelta) ||
        (walletDelta < 0 && cash < -walletDelta)) return {};
  }

  recordSpotValuationMark(engine, false);
  engine.netQtyRaw = result.position.netQtyRaw;
  engine.entryPriceE8 = result.position.entryPriceE8;
  engine.grossRealizedPnlE8 = trading_core::saturatingAddI64(
      engine.grossRealizedPnlE8, result.grossRealizedPnlE8);
  engine.feesPaidE8 =
      trading_core::saturatingAddI64(engine.feesPaidE8, result.feeE8);
  engine.netRealizedPnlE8 = trading_core::saturatingAddI64(
      engine.netRealizedPnlE8, result.netRealizedPnlE8);
  engine.walletBalanceE8 = trading_core::saturatingAddI64(
      engine.walletBalanceE8, walletDelta);
  if (engine.sharedAccount) {
    engine.sharedAccount->walletBalanceE8 = trading_core::saturatingAddI64(
        engine.sharedAccount->walletBalanceE8, walletDelta);
    ++engine.sharedAccount->revision;
  }
  refreshAccountPositionValue(engine, false);
  // Venue matching/results own these facts. Strategy position and account
  // projections advance only at the corresponding private-event delivery.
  return result;
}

std::int64_t unrealizedPnlE8(const EngineState& engine) noexcept {
  const cxet::composite::BookTicker& top = effectiveBookTicker(engine.venueSnapshot);
  const auto bid = engine.market.raw == canon::kMarketTypeSpot.raw && top.bid.px.raw <= 0
      ? engine.venueValuationBidE8 : top.bid.px.raw;
  return capital::accounting::markToMarketPnlE8(
      capital::accounting::FillPositionAccountingState{engine.netQtyRaw,
                                                 engine.entryPriceE8},
      bid,
      top.ask.px.raw);
}

std::int64_t observedUnrealizedPnlE8(const EngineState& engine) noexcept {
  const auto& top = effectiveBookTicker(engine.snapshot);
  const auto bid = engine.market.raw == canon::kMarketTypeSpot.raw && top.bid.px.raw <= 0
      ? engine.observedValuationBidE8 : top.bid.px.raw;
  return capital::accounting::markToMarketPnlE8(
      {engine.position.netQty.raw, engine.position.entryPrice.raw}, bid, top.ask.px.raw);
}

std::int64_t equityE8(const EngineState& engine) noexcept {
  return trading_core::saturatingAddI64(engine.walletBalanceE8,
                                        positionEquityValueE8(engine, false));
}

std::int64_t positionEquityValueE8(const EngineState& engine, bool observed) noexcept {
  if (engine.market.raw != canon::kMarketTypeSpot.raw)
    return observed ? observedUnrealizedPnlE8(engine) : unrealizedPnlE8(engine);
  const auto& top = effectiveBookTicker(observed ? engine.snapshot : engine.venueSnapshot);
  const auto qty = observed ? engine.position.netQty.raw : engine.netQtyRaw;
  const auto price = top.bid.px.raw > 0 ? top.bid.px.raw :
      (observed ? engine.observedValuationBidE8 : engine.venueValuationBidE8);
  return capital::accounting::quoteNotionalE8(price, qty > 0 ? qty : 0);
}

void refreshAccountPositionValue(EngineState& engine, bool observed) noexcept {
  if (!engine.sharedAccount) return;
  auto& old = observed ? engine.observedAccountPositionValueE8 : engine.venueAccountPositionValueE8;
  const auto value = positionEquityValueE8(engine, observed);
  auto& total = observed ? engine.sharedAccount->observedPositionEquityValueE8 : engine.sharedAccount->positionEquityValueE8;
  total = trading_core::saturatingAddI64(trading_core::saturatingAddI64(total, -old), value);
  old = value;
}

void recordSpotValuationMark(EngineState& engine, bool observed) noexcept {
  if (engine.market.raw != canon::kMarketTypeSpot.raw) return;
  const auto& snapshot = observed ? engine.snapshot : engine.venueSnapshot;
  const auto& top = effectiveBookTicker(snapshot);
  if (top.bid.px.raw > 0) {
    (observed ? engine.observedValuationBidE8 : engine.venueValuationBidE8) = top.bid.px.raw;
    (observed ? engine.observedValuationReceiveNs : engine.venueValuationReceiveNs) =
        snapshot.hasReconstructedBbo.raw == trading_core::kAlgoFlagTrue.raw
            ? snapshot.reconstructedBookTickerRecvMono.raw : snapshot.lastBookTickerRecvMono.raw;
  }
  refreshAccountPositionValue(engine, observed);
}

void appendEquityPoint(EngineState& engine, std::int64_t tsNs) {
  const auto unrealized = unrealizedPnlE8(engine);
  const auto grossTotal = trading_core::saturatingAddI64(engine.grossRealizedPnlE8, unrealized);
  const auto netTotal = trading_core::saturatingAddI64(engine.netRealizedPnlE8, unrealized);
  engine.equityPoints.push_back(JsonEquityPoint{tsNs, engine.grossRealizedPnlE8,
      engine.netRealizedPnlE8, unrealized, grossTotal, netTotal, netTotal,
      engine.feesPaidE8, engine.walletBalanceE8, engine.netQtyRaw,
      static_cast<std::uint64_t>(engine.fills.size())});
}

void appendRiskSnapshot(EngineState& engine, std::int64_t tsNs, std::uint8_t reason) {
  const std::int64_t unrealized = unrealizedPnlE8(engine);
  const std::int64_t grossTotal = trading_core::saturatingAddI64(
      engine.grossRealizedPnlE8, unrealized);
  const std::int64_t netTotal = trading_core::saturatingAddI64(
      engine.netRealizedPnlE8, unrealized);
  const cxet::composite::BookTicker& top = effectiveBookTicker(engine.venueSnapshot);
  engine.riskSnapshots.push_back(JsonRiskSnapshot{tsNs,
                                                  engine.grossRealizedPnlE8,
                                                  engine.netRealizedPnlE8,
                                                  unrealized,
                                                  grossTotal,
                                                  netTotal,
                                                  engine.feesPaidE8,
                                                  engine.walletBalanceE8,
                                                  engine.netQtyRaw,
                                                  engine.entryPriceE8,
                                                  top.bid.px.raw,
                                                  top.ask.px.raw,
                                                  static_cast<std::uint64_t>(engine.fills.size()),
                                                  reason});
  engine.lastRiskSnapshotTsNs = tsNs;
  engine.lastRiskSnapshotUnrealizedPnlE8 = unrealized;
  engine.hasRiskSnapshot = true;
}

bool appendRiskSnapshotIfDue(EngineState& engine, std::int64_t tsNs) {
  if (engine.netQtyRaw == 0) return false;
  const std::int64_t unrealized = unrealizedPnlE8(engine);
  if (engine.hasRiskSnapshot && unrealized == engine.lastRiskSnapshotUnrealizedPnlE8) return false;
  if (engine.hasRiskSnapshot) {
    if (engine.lastRiskSnapshotTsNs >
        std::numeric_limits<std::int64_t>::max() -
            kRiskSnapshotIntervalNs) {
      return false;
    }
    const std::int64_t deadline = trading_core::saturatingAddI64(
        engine.lastRiskSnapshotTsNs, kRiskSnapshotIntervalNs);
    if (tsNs < deadline) return false;
  }
  appendRiskSnapshot(engine, tsNs, kRiskSnapshotReasonInterval);
  return true;
}

void hardLiquidate(EngineState& engine, std::int64_t tsNs, std::int64_t breachedEquityE8) noexcept {
  if (engine.liquidated) return;
  engine.liquidated = true;
  engine.liquidationTsNs = tsNs;
  engine.liquidationEquityE8 = breachedEquityE8;
  engine.pending.clear();
  for (OpenOrder& order : engine.open) {
    if (order.state.valid && order.recordIndex < engine.orders.size()) {
      engine.orders[order.recordIndex].status = kRecordStatusCancelled;
      engine.orders[order.recordIndex].tsNs = tsNs;
    }
    order = OpenOrder{};
  }
  engine.netQtyRaw = 0;
  engine.entryPriceE8 = 0;
  engine.walletBalanceE8 = 0;
  if (engine.initialBalanceE8 > 0) {
    engine.netRealizedPnlE8 = -engine.initialBalanceE8;
    engine.grossRealizedPnlE8 = trading_core::saturatingAddI64(
        engine.netRealizedPnlE8, engine.feesPaidE8);
  }
  appendEquityPoint(engine, tsNs);
  appendRiskSnapshot(engine, tsNs, kRiskSnapshotReasonLiquidation);
  bumpContextRevision(engine);
}

void stopForTraderRisk(EngineState& engine,
                       std::int64_t tsNs,
                       std::int64_t breachedEquityE8,
                       std::int64_t thresholdEquityE8) noexcept {
  if (engine.liquidated || engine.riskStopped) return;
  engine.riskStopped = true;
  engine.riskStopTsNs = tsNs;
  engine.riskStopEquityE8 = breachedEquityE8;
  engine.riskStopThresholdEquityE8 = thresholdEquityE8;
  engine.pending.clear();
  for (OpenOrder& order : engine.open) {
    if (order.state.valid && order.recordIndex < engine.orders.size()) {
      engine.orders[order.recordIndex].status = kRecordStatusCancelled;
      engine.orders[order.recordIndex].tsNs = tsNs;
    }
    order = OpenOrder{};
  }
  appendEquityPoint(engine, tsNs);
  appendRiskSnapshot(engine, tsNs, kRiskSnapshotReasonTraderRiskStop);
  bumpContextRevision(engine);
}

}  // namespace hft_backtest::detail
