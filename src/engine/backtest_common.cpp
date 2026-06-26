#include "engine/backtest_engine.hpp"

namespace hft_backtest::detail {
namespace {

Side orderBookSideFromRow(std::int64_t raw) noexcept {
  return raw == 0 ? Side::Buy() : Side::Sell();
}

void appendSnapshotLevel(cxet::composite::OrderBookSnapshot& out, const PriceLevelRow& level) noexcept {
  Price price{};
  Amount qty{};
  price.raw = level.priceE8;
  qty.raw = level.qtyE8;
  (void)cxet::composite::appendOrderBookLevel(out, price, qty, orderBookSideFromRow(level.side));
}

bool appendCandle(cxet::composite::CandleLite* out, CountVal& count, const CandleRow& row) noexcept {
  if (!out || count.raw >= cxet::composite::kTieredCandleHistoryCapacity) return true;
  cxet::composite::CandleLite& candle = out[count.raw++];
  candle.ts.raw = static_cast<std::uint64_t>(row.tsNs > 0 ? row.tsNs : 0);
  candle.high.raw = row.highE8;
  candle.low.raw = row.lowE8;
  candle.quoteAmount.raw = row.quoteAmountE8;
  return true;
}

std::int64_t midpointE8(std::int64_t high, std::int64_t low) noexcept {
  return (high / 2) + (low / 2) + ((high & 1ll) && (low & 1ll) ? 1 : 0);
}

}  // namespace

std::string quote(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 2);
  out.push_back('"');
  for (char c : value) {
    if (c == '\\' || c == '"') out.push_back('\\');
    if (c == '\n') out += "\\n";
    else if (c == '\r') out += "\\r";
    else if (c == '\t') out += "\\t";
    else out.push_back(c);
  }
  out.push_back('"');
  return out;
}

ExchangeId exchangeFromText(const std::string& value) noexcept {
  if (value == "binance") return canon::kExchangeIdBinance;
  if (value == "bybit") return canon::kExchangeIdBybit;
  if (value == "kucoin") return canon::kExchangeIdKucoin;
  if (value == "gate") return canon::kExchangeIdGate;
  if (value == "bitget") return canon::kExchangeIdBitget;
  if (value == "aster") return canon::kExchangeIdAster;
  if (value == "okx") return canon::kExchangeIdOkx;
  if (value == "mexc") return canon::kExchangeIdMexc;
  if (value == "finam") return canon::kExchangeIdFinam;
  return canon::kExchangeIdUnknown;
}

canon::MarketType marketFromText(const std::string& value) noexcept {
  if (value == "futures" ||
      value == "futures_usd" ||
      value == "futures_usdt" ||
      value == "futures_usdc" ||
      value == "usdt" ||
      value == "usdc" ||
      value == "linear") return canon::kMarketTypeFutures;
  if (value == "spot") return canon::kMarketTypeSpot;
  if (value == "margin") return canon::kMarketTypeMargin;
  if (value == "swap") return canon::kMarketTypeSwap;
  if (value == "inverse") return canon::kMarketTypeInverse;
  return canon::kMarketTypeUnknown;
}

Side sideFromRow(std::int64_t raw) noexcept {
  return raw == 1 ? Side::Buy() : Side::Sell();
}

cxet::composite::TradeRuntimeV1 toRuntime(const TradeRow& row) noexcept {
  cxet::composite::TradeRuntimeV1 out{};
  out.ts.raw = static_cast<std::uint64_t>(row.tsNs > 0 ? row.tsNs : 0);
  out.price.raw = row.priceE8;
  out.qty.raw = row.qtyE8;
  out.side = sideFromRow(row.side);
  return out;
}

cxet::composite::BookTickerRuntimeV1 toRuntime(const BookTickerRow& row) noexcept {
  cxet::composite::BookTickerRuntimeV1 out{};
  out.ts.raw = static_cast<std::uint64_t>(row.tsNs > 0 ? row.tsNs : 0);
  out.bid.px.raw = row.bidPriceE8;
  out.bid.qty.raw = row.bidQtyE8;
  out.ask.px.raw = row.askPriceE8;
  out.ask.qty.raw = row.askQtyE8;
  return out;
}

cxet::composite::PriceLimitRuntimeV1 toRuntime(const PriceLimitRow& row) noexcept {
  cxet::composite::PriceLimitRuntimeV1 out{};
  out.ts.raw = static_cast<std::uint64_t>(row.tsNs > 0 ? row.tsNs : 0);
  out.buyLimit.raw = row.buyLimitE8;
  out.sellLimit.raw = row.sellLimitE8;
  out.enabled = row.enabled;
  return out;
}

cxet::composite::LiquidationEvent toRuntime(const LiquidationRow& row,
                                            ExchangeId exchange,
                                            const Symbol& symbol) noexcept {
  cxet::composite::LiquidationEvent out{};
  out.exchangeId = exchange;
  out.symbol = symbol;
  out.ts.raw = static_cast<std::uint64_t>(row.tsNs > 0 ? row.tsNs : 0);
  out.side = sideFromRow(row.side);
  out.price.raw = row.priceE8;
  out.amount.raw = row.qtyE8;
  out.avgPrice.raw = row.avgPriceE8 > 0 ? row.avgPriceE8 : row.priceE8;
  out.lastFilledAmount.raw = row.filledQtyE8 > 0 ? row.filledQtyE8 : row.qtyE8;
  out.filledAmount.raw = out.lastFilledAmount.raw;
  out.orderType = static_cast<canon::OrderType>(row.orderType);
  out.timeInForce = static_cast<canon::TimeInForce>(row.timeInForce);
  out.orderStatus = static_cast<canon::OrderStatus>(row.status);
  out.sourceMode = static_cast<cxet::composite::LiquidationSourceMode>(row.sourceMode);
  return out;
}

cxet::composite::MarkPriceRuntimeV1 toRuntime(const MarkPriceRow& row) noexcept {
  cxet::composite::MarkPriceRuntimeV1 out{};
  out.ts.raw = static_cast<std::uint64_t>(row.tsNs > 0 ? row.tsNs : 0);
  out.markPrice.raw = row.markPriceE8;
  return out;
}

cxet::composite::IndexPriceRuntimeV1 toRuntime(const IndexPriceRow& row) noexcept {
  cxet::composite::IndexPriceRuntimeV1 out{};
  out.ts.raw = static_cast<std::uint64_t>(row.tsNs > 0 ? row.tsNs : 0);
  out.indexPrice.raw = row.indexPriceE8;
  return out;
}

cxet::composite::FundingRuntimeV1 toRuntime(const FundingRow& row) noexcept {
  cxet::composite::FundingRuntimeV1 out{};
  out.ts.raw = static_cast<std::uint64_t>(row.tsNs > 0 ? row.tsNs : 0);
  out.fundingRate.raw = row.fundingRateE8;
  out.fundingTs.raw = static_cast<std::uint64_t>(row.fundingTsNs > 0 ? row.fundingTsNs : 0);
  out.nextFundingTs.raw = static_cast<std::uint64_t>(row.nextFundingTsNs > 0 ? row.nextFundingTsNs : 0);
  return out;
}

cxet::composite::OpenInterestRuntimeV1 toRuntime(const OpenInterestRow& row) noexcept {
  cxet::composite::OpenInterestRuntimeV1 out{};
  out.ts.raw = static_cast<std::uint64_t>(row.tsNs > 0 ? row.tsNs : 0);
  out.openInterest.raw = row.openInterestE8;
  out.openInterestValue.raw = row.openInterestValueE8;
  return out;
}

hft_trader::core::StrategyCandleSnapshot toRuntime(const CandleRow& row) noexcept {
  hft_trader::core::StrategyCandleSnapshot out{};
  out.ts.raw = static_cast<std::uint64_t>(row.tsNs > 0 ? row.tsNs : 0);
  out.tier.raw = static_cast<std::uint64_t>(row.tier > 0 ? row.tier : 0);
  out.high.raw = row.highE8;
  out.low.raw = row.lowE8;
  const std::int64_t fallbackClose = midpointE8(row.highE8, row.lowE8);
  out.open.raw = row.hasOhlc ? row.openE8 : fallbackClose;
  out.close.raw = row.hasOhlc ? row.closeE8 : fallbackClose;
  out.volume.raw = row.volumeE8;
  out.quoteAmount.raw = row.quoteAmountE8;
  out.hasOhlc = row.hasOhlc ? hft_trader::core::kAlgoFlagTrue : hft_trader::core::kAlgoFlagFalse;
  out.valid = row.tsNs > 0 && out.close.raw > 0 && out.high.raw >= out.low.raw
      ? hft_trader::core::kAlgoFlagTrue
      : hft_trader::core::kAlgoFlagFalse;
  return out;
}

void toRuntime(const DepthRow& row,
               cxet::composite::OrderBookTapeRuntimeV1& tape,
               cxet::composite::OrderBookTapeSidesRuntimeV1& sides) noexcept {
  cxet::composite::resetOrderBookTapeRuntimeHeader(tape);
  cxet::composite::resetOrderBookTapeSidesRuntimeHeader(sides);
  TimeNs ts{};
  ts.raw = static_cast<std::uint64_t>(row.tsNs > 0 ? row.tsNs : 0);
  (void)cxet::composite::appendOrderBookTapeTimestamp(tape, ts);
  for (const PriceLevelRow& level : row.levels) {
    Price price{};
    Amount qty{};
    price.raw = level.priceE8;
    qty.raw = level.qtyE8;
    (void)cxet::composite::appendOrderBookTapePriceQtyWithSide(tape, sides, price, qty, orderBookSideFromRow(level.side));
  }
}

cxet::composite::OrderBookSnapshot toRuntimeSnapshot(const DepthRow& row) noexcept {
  cxet::composite::OrderBookSnapshot out{};
  out.ts.raw = static_cast<std::uint64_t>(row.tsNs > 0 ? row.tsNs : 0);
  for (const PriceLevelRow& level : row.levels) {
    appendSnapshotLevel(out, level);
  }
  return out;
}

bool buildTieredCandleHistory(const std::vector<CandleRow>& rows,
                              cxet::composite::TieredCandleHistory& out) noexcept {
  out = cxet::composite::TieredCandleHistory{};
  for (const CandleRow& row : rows) {
    if (row.tier == 1) {
      (void)appendCandle(out.m1, out.m1Count, row);
    } else if (row.tier == 2) {
      (void)appendCandle(out.m15, out.m15Count, row);
    } else if (row.tier == 3) {
      (void)appendCandle(out.d1, out.d1Count, row);
    }
  }
  out.m1Status = out.m1Count.raw == 0u ? cxet::composite::CandleHistoryTierStatus::Empty
                                        : cxet::composite::CandleHistoryTierStatus::Ok;
  out.m15Status = out.m15Count.raw == 0u ? cxet::composite::CandleHistoryTierStatus::Empty
                                          : cxet::composite::CandleHistoryTierStatus::Ok;
  out.d1Status = out.d1Count.raw == 0u ? cxet::composite::CandleHistoryTierStatus::Empty
                                        : cxet::composite::CandleHistoryTierStatus::Ok;
  return out.m1Count.raw != 0u || out.m15Count.raw != 0u || out.d1Count.raw != 0u;
}

}  // namespace hft_backtest::detail
