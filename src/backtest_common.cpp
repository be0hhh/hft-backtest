#include "backtest_engine.hpp"

namespace hft_backtest::detail {
namespace {

void appendSnapshotLevel(cxet::composite::OrderBookSnapshot& out, const PriceLevelRow& level) noexcept {
  Price price{};
  Amount qty{};
  price.raw = level.priceE8;
  qty.raw = level.qtyE8;
  (void)cxet::composite::appendOrderBookLevel(out, price, qty, sideFromRow(level.side));
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
  if (value == "okx") return canon::kExchangeIdOkx;
  return canon::kExchangeIdUnknown;
}

canon::MarketType marketFromText(const std::string& value) noexcept {
  if (value == "futures" || value == "futures_usd") return canon::kMarketTypeFutures;
  if (value == "spot") return canon::kMarketTypeSpot;
  if (value == "swap") return canon::kMarketTypeSwap;
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

cxet::composite::OrderBookDeltaRuntimeV1 toRuntime(const DepthRow& row) noexcept {
  cxet::composite::OrderBookDeltaRuntimeV1 out{};
  out.ts.raw = static_cast<std::uint64_t>(row.tsNs > 0 ? row.tsNs : 0);
  for (const PriceLevelRow& level : row.levels) {
    Price price{};
    Amount qty{};
    price.raw = level.priceE8;
    qty.raw = level.qtyE8;
    (void)cxet::composite::appendOrderBookLevel(out, price, qty, sideFromRow(level.side));
  }
  return out;
}

cxet::composite::OrderBookSnapshot toRuntimeSnapshot(const SnapshotDocument& row) noexcept {
  cxet::composite::OrderBookSnapshot out{};
  out.ts.raw = static_cast<std::uint64_t>(row.tsNs > 0 ? row.tsNs : 0);
  for (const PriceLevelRow& level : row.levels) {
    appendSnapshotLevel(out, level);
  }
  return out;
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
