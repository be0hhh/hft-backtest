#include "trading_core/Algorithm/Math/Integer/Saturating.hpp"
#include "../BacktestEngine.hpp"

#include <limits>

#include "trading_core/Algorithm/Math/PriceQuantity/NotionalQty.hpp"

namespace hft_backtest::detail {

void initializeAccounting(EngineState& engine, const BacktestRunRequest& request) noexcept {
  engine.initialBalanceE8 = request.initialBalanceE8;
  engine.walletBalanceE8 = request.initialBalanceE8;
  engine.makerFeeBpsE8 = request.makerFeeBpsE8;
  engine.takerFeeBpsE8 = request.takerFeeBpsE8;
  engine.position.netQty.raw = engine.netQtyRaw;
  engine.position.entryPrice.raw = engine.entryPriceE8;
  engine.position.unrealizedPnl.raw = 0;
  engine.position.valid = true;
  bumpContextRevision(engine);
}

trading_core::FillPositionAccountingResult updatePosition(
    EngineState& engine,
    Side side,
    std::int64_t priceE8,
    std::int64_t qtyE8,
    std::int64_t tsNs,
    std::int64_t feeBpsE8) noexcept {
  const trading_core::FillPositionAccountingResult result =
      trading_core::applyFillPositionAccounting(
          trading_core::FillPositionAccountingState{engine.netQtyRaw,
                                                     engine.entryPriceE8},
          side,
          priceE8,
          qtyE8,
          feeBpsE8);
  if (!result.applied) return result;

  engine.netQtyRaw = result.position.netQtyRaw;
  engine.entryPriceE8 = result.position.entryPriceE8;
  engine.grossRealizedPnlE8 = trading_core::saturatingAddI64(
      engine.grossRealizedPnlE8, result.grossRealizedPnlE8);
  engine.feesPaidE8 =
      trading_core::saturatingAddI64(engine.feesPaidE8, result.feeE8);
  engine.netRealizedPnlE8 = trading_core::saturatingAddI64(
      engine.netRealizedPnlE8, result.netRealizedPnlE8);
  engine.walletBalanceE8 = trading_core::saturatingAddI64(
      engine.walletBalanceE8, result.netRealizedPnlE8);
  engine.position.netQty.raw = engine.netQtyRaw;
  engine.position.entryPrice.raw = engine.entryPriceE8;
  engine.position.lastPrice.raw = priceE8;
  engine.position.unrealizedPnl.raw = unrealizedPnlE8(engine);
  engine.position.lastUpdateUtc.raw = static_cast<std::uint64_t>(tsNs > 0 ? tsNs : 0);
  engine.position.valid = true;
  bumpContextRevision(engine);
  return result;
}

std::int64_t unrealizedPnlE8(const EngineState& engine) noexcept {
  const cxet::composite::BookTickerRuntimeV1& top = effectiveBookTicker(engine.venueSnapshot);
  return trading_core::markToMarketPnlE8(
      trading_core::FillPositionAccountingState{engine.netQtyRaw,
                                                 engine.entryPriceE8},
      top.bid.px.raw,
      top.ask.px.raw);
}

std::int64_t equityE8(const EngineState& engine) noexcept {
  return trading_core::saturatingAddI64(engine.walletBalanceE8,
                                        unrealizedPnlE8(engine));
}

void appendEquityPoint(EngineState& engine, std::int64_t tsNs) {
  engine.equityPoints.push_back(JsonEquityPoint{tsNs,
                                                engine.grossRealizedPnlE8,
                                                engine.netRealizedPnlE8,
                                                0,
                                                engine.grossRealizedPnlE8,
                                                engine.netRealizedPnlE8,
                                                engine.netRealizedPnlE8,
                                                engine.feesPaidE8,
                                                engine.walletBalanceE8,
                                                engine.netQtyRaw,
                                                static_cast<std::uint64_t>(engine.fills.size())});
}

void appendRiskSnapshot(EngineState& engine, std::int64_t tsNs, std::uint8_t reason) {
  const std::int64_t unrealized = unrealizedPnlE8(engine);
  const std::int64_t grossTotal = trading_core::saturatingAddI64(
      engine.grossRealizedPnlE8, unrealized);
  const std::int64_t netTotal = trading_core::saturatingAddI64(
      engine.netRealizedPnlE8, unrealized);
  const cxet::composite::BookTickerRuntimeV1& top = effectiveBookTicker(engine.venueSnapshot);
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
  engine.position.valid = true;
  engine.position.netQty.raw = 0;
  engine.position.entryPrice.raw = 0;
  engine.position.unrealizedPnl.raw = 0;
  engine.position.lastUpdateUtc.raw = static_cast<std::uint64_t>(tsNs > 0 ? tsNs : 0);
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
  engine.position.valid = true;
  engine.position.netQty.raw = engine.netQtyRaw;
  engine.position.entryPrice.raw = engine.entryPriceE8;
  engine.position.unrealizedPnl.raw = unrealizedPnlE8(engine);
  engine.position.lastUpdateUtc.raw = static_cast<std::uint64_t>(tsNs > 0 ? tsNs : 0);
  appendEquityPoint(engine, tsNs);
  appendRiskSnapshot(engine, tsNs, kRiskSnapshotReasonTraderRiskStop);
  bumpContextRevision(engine);
}

}  // namespace hft_backtest::detail
