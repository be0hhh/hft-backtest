#include "../Engine/BacktestEngine.hpp"
#include "cxet/Canon/ExchangeRegistry.hpp"

#include <algorithm>
#include <new>

namespace hft_backtest::detail {
namespace {

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

bool ReplayDepthBook::prepare() noexcept {
  if (books_.ready()) return true;
  const std::array<std::uint32_t, 1u> capacities{
      cxet::market::kMaximumDepthLevelsPerSide};
  cxet::runtime::market::LocalOrderBookMemoryPlan plan{};
  if (!cxet::runtime::market::planLocalOrderBookMemory(capacities,
          std::numeric_limits<std::size_t>::max(), &plan) ||
      !books_.initialize(capacities, plan.requiredBytes)) return false;
  scratch_.reset(new (std::nothrow) IndexedMutation[
      cxet::market::kMaximumDepthSnapshotMutations]);
  if (!scratch_) { books_.reset(); return false; }
  return true;
}

void ReplayDepthBook::reset() noexcept {
  if (auto* owner = books_.book(1u)) owner->reset();
  arrival_ = {};
  sourcePosition_ = true;
}

bool ReplayDepthBook::apply(std::span<const PriceLevelRow> levels,
    std::int64_t exchangeTsNs, const hftrec::replay::EventArrival& arrival,
    bool snapshot, cxet::market::DepthSequence capturedSequence,
    std::uint64_t capturedPublicationSequence) noexcept {
  auto* owner = books_.book(1u);
  if (!owner || !scratch_ ||
      levels.size() > cxet::market::kMaximumDepthSnapshotMutations) return false;
  const bool identityChanged = arrival_.producerEpoch != 0u &&
      (arrival_.producerEpoch != arrival.producerEpoch ||
       arrival_.sourceId != arrival.sourceId ||
       arrival_.sourceGeneration != arrival.sourceGeneration ||
       arrival_.sessionEpoch != arrival.sessionEpoch);
  if (identityChanged) owner->reset();
  std::size_t count = 0u;
  for (std::size_t index = 0u; index < levels.size(); ++index) {
    const auto& input = levels[index];
    cxet::market::DepthMutation value{};
    value.priceRaw = input.priceE8;
    value.quantityRaw = input.qtyE8;
    value.side = input.side == 0 ? cxet::market::DepthSide::Bid
                               : cxet::market::DepthSide::Ask;
    value.action = input.qtyE8 == 0 ? cxet::market::DepthAction::Erase
                                  : cxet::market::DepthAction::Upsert;
    if ((input.side != 0 && input.side != 1) ||
        !cxet::market::validDepthMutation(value) ||
        (snapshot && value.action != cxet::market::DepthAction::Upsert)) {
      owner->markGap(); return false;
    }
    scratch_[count++] = {value, static_cast<std::uint32_t>(index)};
  }
  // Legacy text rows retain wire order. Collapse repeated side/price entries
  // using the last actual input mutation before Core's canonical application.
  std::sort(scratch_.get(), scratch_.get() + count,
      [](const IndexedMutation& left, const IndexedMutation& right) noexcept {
        if (left.value.side != right.value.side)
          return left.value.side == cxet::market::DepthSide::Bid;
        if (left.value.priceRaw != right.value.priceRaw)
          return left.value.side == cxet::market::DepthSide::Bid
              ? left.value.priceRaw > right.value.priceRaw
              : left.value.priceRaw < right.value.priceRaw;
        return left.ordinal > right.ordinal;
      });
  auto mutations = books_.transactionScratch();
  std::size_t unique = 0u;
  for (std::size_t index = 0u; index < count; ++index) {
    if (index != 0u && scratch_[index].value.side == scratch_[index - 1u].value.side &&
        scratch_[index].value.priceRaw == scratch_[index - 1u].value.priceRaw) continue;
    if (unique == mutations.size()) { owner->markGap(); return false; }
    mutations[unique++] = scratch_[index].value;
  }
  const bool localPosition = capturedSequence.lastUpdateId == 0u;
  cxet::market::DepthSequence sequence = capturedSequence;
  if (localPosition) {
    if (owner->lastUpdateId() == std::numeric_limits<std::uint64_t>::max()) {
      owner->markGap(); return false;
    }
    const std::uint64_t next = owner->lastUpdateId() + 1u;
    sequence = {next, next, snapshot ? 0u : owner->lastUpdateId()};
  }
  using cxet::runtime::market::LocalOrderBookStatus;
  LocalOrderBookStatus result{};
  if (snapshot) {
    result = owner->stageSnapshot(sequence, mutations.first(unique));
    if (result == LocalOrderBookStatus::RebaseStaged) result = owner->commitRebase();
  } else if (unique == 0u) {
    owner->markGap(); return false;
  } else {
    result = owner->state() == cxet::market::DepthSourceState::Healthy
        ? owner->applyDelta(sequence, mutations.first(unique))
        : owner->applyPartialDelta(sequence, mutations.first(unique));
  }
  if (result != LocalOrderBookStatus::Applied &&
      result != LocalOrderBookStatus::Partial &&
      result != LocalOrderBookStatus::RebaseCommitted) {
    owner->markGap(); return false;
  }
  sourcePosition_ = localPosition;
  arrival_ = arrival;
  owner->commitProjection({arrival.sourceGeneration, arrival.sessionEpoch,
      capturedPublicationSequence != 0u ? capturedPublicationSequence : sequence.lastUpdateId,
      arrival.receiveMonotonicNs,
      static_cast<std::uint64_t>(exchangeTsNs > 0 ? exchangeTsNs : 0),
      localPosition ? cxet::market::DepthEventSourcePosition : 0u,
      cxet::market::DepthFailure::None});
  return true;
}

bool ReplayDepthBook::restore(
    const cxet::runtime::market::LocalOrderBookCheckpointView& checkpoint,
    const hftrec::replay::EventArrival& arrival, bool sourcePosition) noexcept {
  auto* owner = books_.book(1u);
  if (!owner || !owner->restoreCheckpoint(checkpoint)) return false;
  arrival_ = arrival;
  sourcePosition_ = sourcePosition;
  return true;
}

cxet::api::market::PublicMarketOrderBookView ReplayDepthBook::view() const noexcept {
  cxet::api::market::PublicMarketOrderBookView result{};
  const auto* owner = books_.book(1u);
  if (!owner) return result;
  const auto bids = owner->bids();
  const auto asks = owner->asks();
  result.bids = bids.data(); result.asks = asks.data();
  result.bidCount = static_cast<std::uint32_t>(bids.size());
  result.askCount = static_cast<std::uint32_t>(asks.size());
  result.lastEventTs.raw = owner->projection().exchangeTimestampNs;
  result.lastEventId.raw = owner->projection().publicationSequence;
  result.ready = owner->state() == cxet::market::DepthSourceState::Healthy ||
                 owner->state() == cxet::market::DepthSourceState::Partial;
  result.coverage = owner->state() == cxet::market::DepthSourceState::Healthy
      ? cxet::api::market::PublicMarketOrderBookCoverage::SnapshotAligned
      : cxet::api::market::PublicMarketOrderBookCoverage::DeltaOnly;
  return result;
}

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
  const auto* exchange = canon::exchangeRegistryFindByName(value.c_str());
  // Match the complete owning string; the C-string lookup alone admits a
  // valid prefix before an embedded NUL.
  return exchange && value == exchange->canonicalName
      ? exchange->id : canon::kExchangeIdUnknown;
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

bool toRuntime(const TradeRow& row, cxet::composite::Trade& out) noexcept {
  using Shape = cxet::api::market::PublicMarketNativeIdentityShape;
  Id projected{};
  projected.raw = row.nativeIdentity.nativeShape == Shape::U64Pair
      ? row.nativeIdentity.nativeSecond : row.nativeIdentity.nativeFirst;
  if (!trading_core::nativeTradeIdentityMatches(projected, row.nativeIdentity)) return false;
  Price price{}; price.raw = row.priceE8;
  Amount quantity{}; quantity.raw = row.qtyE8;
  const auto side = row.side == 1 ? cxet::composite::TradeInitiatorSide::Buyer
      : (row.side == 0 ? cxet::composite::TradeInitiatorSide::Seller
                       : cxet::composite::TradeInitiatorSide::Unknown);
  if (row.side < -1 || row.side > 1 || !out.setValues(price, quantity, side)) return false;
  out.ts.raw = static_cast<std::uint64_t>(row.tsNs > 0 ? row.tsNs : 0);
  // The currently registered exact Trade pair is Hyperliquid time+tid,
  // whose native scalar component is the second limb.
  out.eventId = projected;
  return true;
}

cxet::composite::BookTicker toRuntime(const BookTickerRow& row) noexcept {
  cxet::composite::BookTicker out{};
  out.ts.raw = static_cast<std::uint64_t>(row.tsNs > 0 ? row.tsNs : 0);
  out.bid.px.raw = row.bidPriceE8;
  out.bid.qty.raw = row.bidQtyE8;
  out.ask.px.raw = row.askPriceE8;
  out.ask.qty.raw = row.askQtyE8;
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
