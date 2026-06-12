#include "backtest_engine.hpp"

namespace hft_backtest::detail {

std::int64_t mulPriceQtyE8(std::int64_t px, std::int64_t qty) noexcept {
  if (px == 0 || qty <= 0) return 0;
  const bool negative = px < 0;
  const std::uint64_t absPx = static_cast<std::uint64_t>(negative ? -px : px);
  const unsigned __int128 product = static_cast<unsigned __int128>(absPx) *
                                    static_cast<unsigned __int128>(static_cast<std::uint64_t>(qty));
  const std::int64_t value = static_cast<std::int64_t>(product / 100000000ull);
  return negative ? -value : value;
}

std::int64_t feeForFillE8(std::int64_t priceE8, std::int64_t qtyE8, std::int64_t feeBpsE8) noexcept {
  if (priceE8 <= 0 || qtyE8 <= 0 || feeBpsE8 <= 0) return 0;
  const std::int64_t notionalE8 = mulPriceQtyE8(priceE8, qtyE8);
  if (notionalE8 <= 0) return 0;
  const unsigned __int128 product = static_cast<unsigned __int128>(static_cast<std::uint64_t>(notionalE8)) *
                                    static_cast<unsigned __int128>(static_cast<std::uint64_t>(feeBpsE8));
  return static_cast<std::int64_t>(product / 1000000000000ull);
}

void initializeAccounting(EngineState& engine, const BacktestRunRequest& request) noexcept {
  engine.initialBalanceE8 = request.initialBalanceE8;
  engine.walletBalanceE8 = request.initialBalanceE8;
  engine.makerFeeBpsE8 = request.makerFeeBpsE8;
  engine.takerFeeBpsE8 = request.takerFeeBpsE8;
}

void updatePosition(EngineState& engine, Side side, std::int64_t priceE8, std::int64_t qtyE8, std::int64_t tsNs, std::int64_t feeBpsE8, std::int64_t& netRealized) noexcept {
  std::int64_t grossRealized = 0;
  const std::int64_t signedQty = side.raw == Side::Buy().raw ? qtyE8 : -qtyE8;
  if (engine.netQtyRaw == 0 || (engine.netQtyRaw > 0 && signedQty > 0) || (engine.netQtyRaw < 0 && signedQty < 0)) {
    const std::int64_t oldAbs = engine.netQtyRaw < 0 ? -engine.netQtyRaw : engine.netQtyRaw;
    const std::int64_t addAbs = signedQty < 0 ? -signedQty : signedQty;
    const std::int64_t newAbs = oldAbs + addAbs;
    if (newAbs > 0) {
      const unsigned __int128 oldNotional = static_cast<unsigned __int128>(static_cast<std::uint64_t>(engine.entryPriceE8 > 0 ? engine.entryPriceE8 : priceE8)) * static_cast<unsigned __int128>(static_cast<std::uint64_t>(oldAbs));
      const unsigned __int128 addNotional = static_cast<unsigned __int128>(static_cast<std::uint64_t>(priceE8)) * static_cast<unsigned __int128>(static_cast<std::uint64_t>(addAbs));
      engine.entryPriceE8 = static_cast<std::int64_t>((oldNotional + addNotional) / static_cast<unsigned __int128>(static_cast<std::uint64_t>(newAbs)));
    }
    engine.netQtyRaw += signedQty;
  } else {
    const std::int64_t oldAbs = engine.netQtyRaw < 0 ? -engine.netQtyRaw : engine.netQtyRaw;
    const std::int64_t closeAbs = (signedQty < 0 ? -signedQty : signedQty) < oldAbs ? (signedQty < 0 ? -signedQty : signedQty) : oldAbs;
    if (engine.netQtyRaw > 0) grossRealized = mulPriceQtyE8(priceE8 - engine.entryPriceE8, closeAbs);
    else grossRealized = mulPriceQtyE8(engine.entryPriceE8 - priceE8, closeAbs);
    engine.grossRealizedPnlE8 += grossRealized;
    engine.netQtyRaw += signedQty;
    if (engine.netQtyRaw == 0) engine.entryPriceE8 = 0;
    if ((engine.netQtyRaw > 0 && signedQty > oldAbs) || (engine.netQtyRaw < 0 && -signedQty > oldAbs)) engine.entryPriceE8 = priceE8;
  }
  const std::int64_t fee = feeForFillE8(priceE8, qtyE8, feeBpsE8);
  engine.feesPaidE8 += fee;
  netRealized = grossRealized - fee;
  engine.netRealizedPnlE8 += netRealized;
  engine.walletBalanceE8 += netRealized;
  engine.position.netQty.raw = engine.netQtyRaw;
  engine.position.entryPrice.raw = engine.entryPriceE8;
  engine.position.lastPrice.raw = priceE8;
  engine.position.unrealizedPnl.raw = unrealizedPnlE8(engine);
  engine.position.lastUpdateUtc.raw = static_cast<std::uint64_t>(tsNs > 0 ? tsNs : 0);
  engine.position.valid = true;
}

std::int64_t unrealizedPnlE8(const EngineState& engine) noexcept {
  if (engine.netQtyRaw == 0 || engine.entryPriceE8 <= 0) return 0;
  const bool longPosition = engine.netQtyRaw > 0;
  const std::int64_t qty = longPosition ? engine.netQtyRaw : -engine.netQtyRaw;
  const cxet::composite::BookTickerRuntimeV1& top = effectiveBookTicker(engine.venueSnapshot);
  const std::int64_t bid = top.bid.px.raw;
  const std::int64_t ask = top.ask.px.raw;
  if (longPosition) return bid > 0 ? mulPriceQtyE8(bid - engine.entryPriceE8, qty) : 0;
  return ask > 0 ? mulPriceQtyE8(engine.entryPriceE8 - ask, qty) : 0;
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
  const std::int64_t grossTotal = engine.grossRealizedPnlE8 + unrealized;
  const std::int64_t netTotal = engine.netRealizedPnlE8 + unrealized;
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
  if (engine.hasRiskSnapshot && tsNs < engine.lastRiskSnapshotTsNs + kRiskSnapshotIntervalNs) return false;
  appendRiskSnapshot(engine, tsNs, kRiskSnapshotReasonInterval);
  return true;
}

}  // namespace hft_backtest::detail
