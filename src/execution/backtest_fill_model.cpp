#include "execution/backtest_fill_model.hpp"

#include <cstdint>
#include <limits>

namespace hft_backtest::detail {
namespace {

constexpr std::int64_t kPct100E8 = 100ll * 100000000ll;

[[nodiscard]] std::int64_t clampI128ToI64(__int128 value) noexcept {
  if (value > static_cast<__int128>(std::numeric_limits<std::int64_t>::max())) {
    return std::numeric_limits<std::int64_t>::max();
  }
  if (value < static_cast<__int128>(std::numeric_limits<std::int64_t>::min())) {
    return std::numeric_limits<std::int64_t>::min();
  }
  return static_cast<std::int64_t>(value);
}

[[nodiscard]] std::int64_t makerFillMinCrossE8(const EngineState& engine) noexcept {
  if (engine.makerFillMinCrossTicks == 0u) return 0;
  if (engine.tickSizeE8 <= 0) return std::numeric_limits<std::int64_t>::max();
  const auto ticks = static_cast<std::uint64_t>(engine.makerFillMinCrossTicks);
  const auto tickSize = static_cast<std::uint64_t>(engine.tickSizeE8);
  const auto maxValue = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  if (tickSize != 0u && ticks > maxValue / tickSize) return std::numeric_limits<std::int64_t>::max();
  return static_cast<std::int64_t>(ticks * tickSize);
}

[[nodiscard]] bool makerTradeCrossesOrder(const EngineState& engine,
                                          const OpenOrder& order,
                                          const cxet::composite::TradeRuntimeV1& trade) noexcept {
  const std::int64_t minCross = makerFillMinCrossE8(engine);
  if (minCross == std::numeric_limits<std::int64_t>::max()) return false;
  if (trade.side.raw == Side::Buy().raw && order.state.side.raw == Side::Sell().raw) {
    if (order.state.price.raw > std::numeric_limits<std::int64_t>::max() - minCross) return false;
    return trade.price.raw >= order.state.price.raw + minCross;
  }
  if (trade.side.raw == Side::Sell().raw && order.state.side.raw == Side::Buy().raw) {
    if (order.state.price.raw < std::numeric_limits<std::int64_t>::min() + minCross) return false;
    return trade.price.raw <= order.state.price.raw - minCross;
  }
  return false;
}

[[nodiscard]] bool makerBboTouchesOrder(const OpenOrder& order,
                                        const cxet::composite::BookTickerRuntimeV1& top,
                                        std::int64_t& oppositeQtyE8) noexcept {
  oppositeQtyE8 = 0;
  if (order.state.side.raw == Side::Sell().raw) {
    if (top.bid.px.raw <= 0 || top.bid.px.raw < order.state.price.raw) return false;
    oppositeQtyE8 = top.bid.qty.raw;
    return oppositeQtyE8 > 0;
  }
  if (order.state.side.raw == Side::Buy().raw) {
    if (top.ask.px.raw <= 0 || top.ask.px.raw > order.state.price.raw) return false;
    oppositeQtyE8 = top.ask.qty.raw;
    return oppositeQtyE8 > 0;
  }
  return false;
}

[[nodiscard]] bool passiveBboTouchEligible(const OpenOrder& order,
                                           std::int64_t bboTsNs) noexcept {
  if (order.state.orderType != canon::OrderType::Limit) return false;
  if (order.lastAggressiveBookTsNs == bboTsNs) return false;
  if (order.postOnly) return true;
  if (order.lastAggressiveBookTsNs != 0 || order.filledQtyRaw > 0) return true;
  return bboTsNs > order.activeTsNs;
}

[[nodiscard]] hft_trader::core::BookSide passiveBookSide(const OpenOrder& order) noexcept {
  return order.state.side.raw == Side::Buy().raw ? hft_trader::core::BookSide::Bid
                                                 : hft_trader::core::BookSide::Ask;
}

[[nodiscard]] hft_trader::core::BookSide aggressiveBookSide(const OpenOrder& order) noexcept {
  return order.state.side.raw == Side::Buy().raw ? hft_trader::core::BookSide::Ask
                                                 : hft_trader::core::BookSide::Bid;
}

[[nodiscard]] bool levelExecutableByLimit(const OpenOrder& order, std::int64_t levelPriceE8) noexcept {
  if (order.state.orderType == canon::OrderType::Market ||
      (order.state.orderType == canon::OrderType::Stop && order.stopTriggered)) return true;
  if (order.state.side.raw == Side::Buy().raw) return levelPriceE8 <= order.state.price.raw;
  return levelPriceE8 >= order.state.price.raw;
}

[[nodiscard]] std::int64_t visibleQtyAtPassivePrice(const EngineState& engine,
                                                    const OpenOrder& order,
                                                    const hft_trader::core::LocalOrderBookState& book) noexcept {
  (void)engine;
  if (order.state.price.raw <= 0) return 0;
  const hft_trader::core::LocalOrderBookView view = hft_trader::core::orderBook(book);
  const hft_trader::core::LocalOrderBookSideState* side = view.side(passiveBookSide(order));
  if (side == nullptr) return 0;
  for (std::uint32_t i = 0u; i < side->activeCount.raw; ++i) {
    const hft_trader::core::LocalOrderBookLevel& level = side->levels[i];
    if (level.px.raw == order.state.price.raw) return level.qty.raw > 0 ? level.qty.raw : 0;
  }
  return 0;
}

[[nodiscard]] std::int64_t visibleQtyAtPassivePrice(const EngineState& engine,
                                                    const OpenOrder& order) noexcept {
  return visibleQtyAtPassivePrice(engine, order, engine.venueOrderBook);
}

[[nodiscard]] std::int64_t cumulativeAvgPrice(__int128 notionalSum,
                                              std::int64_t qtyE8) noexcept {
  if (qtyE8 <= 0) return 0;
  return clampI128ToI64(notionalSum / static_cast<__int128>(qtyE8));
}

[[nodiscard]] std::int64_t consumedPctE8(std::int64_t filledQtyE8,
                                         std::int64_t visibleQtyE8) noexcept {
  if (filledQtyE8 <= 0 || visibleQtyE8 <= 0) return 0;
  const __int128 numerator = static_cast<__int128>(filledQtyE8) * static_cast<__int128>(kPct100E8);
  return clampI128ToI64(numerator / static_cast<__int128>(visibleQtyE8));
}

[[nodiscard]] std::int64_t slippageBpsE8(std::int64_t slippageE8,
                                         std::int64_t referencePriceE8) noexcept {
  if (referencePriceE8 <= 0 || slippageE8 == 0) return 0;
  constexpr std::int64_t kBpsE8 = 10000ll * 100000000ll;
  const __int128 numerator = static_cast<__int128>(slippageE8) * static_cast<__int128>(kBpsE8);
  return clampI128ToI64(numerator / static_cast<__int128>(referencePriceE8));
}

void finalizeAggressivePlan(const OpenOrder& order, AggressiveFillPlan& out) noexcept {
  if (out.count == 0u || out.filledQtyE8 <= 0) return;
  out.executionAvgPriceE8 = out.chunks[out.count - 1u].avgPriceE8;
  out.executionBookConsumedPctE8 = consumedPctE8(out.filledQtyE8, out.chunks[0].bookVisibleExecutableQtyE8);
  if (out.referencePriceE8 > 0 && out.executionAvgPriceE8 > 0) {
    out.slippageE8 = order.state.side.raw == Side::Buy().raw
        ? out.executionAvgPriceE8 - out.referencePriceE8
        : out.referencePriceE8 - out.executionAvgPriceE8;
    out.slippageBpsE8 = slippageBpsE8(out.slippageE8, out.referencePriceE8);
  }
  const std::uint32_t chunkCount = static_cast<std::uint32_t>(out.count);
  for (std::size_t i = 0u; i < out.count; ++i) {
    AggressiveFillChunk& chunk = out.chunks[i];
    chunk.chunkIndex = static_cast<std::uint32_t>(i + 1u);
    chunk.chunkCount = chunkCount;
    chunk.executionQtyE8 = out.filledQtyE8;
    chunk.executionAvgPriceE8 = out.executionAvgPriceE8;
    chunk.referencePriceE8 = out.referencePriceE8;
    chunk.slippageE8 = out.slippageE8;
    chunk.slippageBpsE8 = out.slippageBpsE8;
    chunk.executionBookConsumedPctE8 = out.executionBookConsumedPctE8;
    chunk.bookConsumedPctE8 = out.executionBookConsumedPctE8;
  }
}

void resetAggressivePlan(AggressiveFillPlan& out) noexcept {
  out.count = 0u;
  out.requestedQtyE8 = 0;
  out.filledQtyE8 = 0;
  out.remainingQtyE8 = 0;
  out.bookTsNs = 0;
  out.referencePriceE8 = 0;
  out.executionAvgPriceE8 = 0;
  out.slippageE8 = 0;
  out.slippageBpsE8 = 0;
  out.executionBookConsumedPctE8 = 0;
  out.visibleDepthCapped = false;
}

}  // namespace

std::int64_t openOrderRemainingQty(const OpenOrder& order) noexcept {
  if (order.state.qty.raw <= order.filledQtyRaw) return 0;
  return order.state.qty.raw - order.filledQtyRaw;
}

void initializePassiveQueue(OpenOrder& order, const EngineState& engine) noexcept {
  initializePassiveQueue(order, engine, engine.venueOrderBook);
}

void initializePassiveQueue(OpenOrder& order,
                            const EngineState& engine,
                            const hft_trader::core::LocalOrderBookState& book) noexcept {
  if (!order.state.valid || !order.active || order.state.orderType != canon::OrderType::Limit) return;
  if (order.queueInitialized) return;
  order.queueAheadQtyRaw = visibleQtyAtPassivePrice(engine, order, book);
  order.queueInitialized = true;
}

bool buildAggressiveBookFillPlan(const EngineState& engine,
                                 const OpenOrder& order,
                                 AggressiveFillPlan& out) noexcept {
  return buildAggressiveBookFillPlan(engine, order, engine.venueOrderBook, out);
}

bool buildAggressiveBookFillPlan(const EngineState& engine,
                                 const OpenOrder& order,
                                 const hft_trader::core::LocalOrderBookState& book,
                                 AggressiveFillPlan& out) noexcept {
  resetAggressivePlan(out);
  if (!order.state.valid || !order.active) return false;
  if (order.postOnly) return false;
  out.requestedQtyE8 = openOrderRemainingQty(order);
  out.remainingQtyE8 = out.requestedQtyE8;
  if (out.requestedQtyE8 <= 0) return false;

  const bool marketOrder = order.state.orderType == canon::OrderType::Market ||
      (order.state.orderType == canon::OrderType::Stop && order.stopTriggered);
  const bool limitOrder = order.state.orderType == canon::OrderType::Limit;
  if (!marketOrder && !limitOrder) return false;
  if (limitOrder && order.lastAggressiveBookTsNs != 0) return false;

  const hft_trader::core::LocalOrderBookView view = hft_trader::core::orderBook(book);
  const hft_trader::core::LocalOrderBookSideState* side = view.side(aggressiveBookSide(order));
  if (side == nullptr) return false;
  out.bookTsNs = book.ts.raw <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                     ? static_cast<std::int64_t>(book.ts.raw)
                     : std::numeric_limits<std::int64_t>::max();
  if (side->activeCount.raw != 0u) out.referencePriceE8 = side->levels[0].px.raw;

  std::int64_t visibleExecutableQty = 0;
  for (std::uint32_t i = 0u; i < side->activeCount.raw; ++i) {
    const hft_trader::core::LocalOrderBookLevel& level = side->levels[i];
    if (level.px.raw <= 0 || level.qty.raw <= 0) continue;
    if (!levelExecutableByLimit(order, level.px.raw)) break;
    if (visibleExecutableQty > std::numeric_limits<std::int64_t>::max() - level.qty.raw) {
      visibleExecutableQty = std::numeric_limits<std::int64_t>::max();
      break;
    }
    visibleExecutableQty += level.qty.raw;
  }

  if (visibleExecutableQty <= 0) {
    if (!marketOrder) return false;
    out.visibleDepthCapped = true;
    return true;
  }

  __int128 notionalSum = 0;
  std::int64_t remaining = out.requestedQtyE8;
  for (std::uint32_t i = 0u; i < side->activeCount.raw && remaining > 0 && out.count < out.chunks.size(); ++i) {
    const hft_trader::core::LocalOrderBookLevel& level = side->levels[i];
    if (level.px.raw <= 0 || level.qty.raw <= 0) continue;
    if (!levelExecutableByLimit(order, level.px.raw)) break;
    const std::int64_t fillQty = level.qty.raw < remaining ? level.qty.raw : remaining;
    if (fillQty <= 0) continue;
    remaining -= fillQty;
    out.filledQtyE8 += fillQty;
    notionalSum += static_cast<__int128>(level.px.raw) * static_cast<__int128>(fillQty);
    out.chunks[out.count++] = AggressiveFillChunk{
        level.px.raw,
        fillQty,
        cumulativeAvgPrice(notionalSum, out.filledQtyE8),
        level.qty.raw,
        visibleExecutableQty,
        consumedPctE8(out.filledQtyE8, visibleExecutableQty),
    };
  }

  out.remainingQtyE8 = out.requestedQtyE8 - out.filledQtyE8;
  out.visibleDepthCapped = out.remainingQtyE8 > 0;
  finalizeAggressivePlan(order, out);
  return out.filledQtyE8 > 0 || marketOrder;
}

PassiveQueueTradeFill consumePassiveQueueForTrade(EngineState& engine,
                                                  OpenOrder& order,
                                                  const cxet::composite::TradeRuntimeV1& trade) noexcept {
  PassiveQueueTradeFill out{};
  if (!order.state.valid || !order.active || order.state.orderType != canon::OrderType::Limit) return out;
  if (!order.queueInitialized) {
    order.queueAheadQtyRaw = 0;
    order.queueInitialized = true;
  }
  out.queueAheadBeforeE8 = order.queueAheadQtyRaw;
  out.queueAheadAfterE8 = order.queueAheadQtyRaw;
  if (trade.qty.raw <= 0 || !makerTradeCrossesOrder(engine, order, trade)) return out;
  if (trade.qty.raw <= order.queueAheadQtyRaw) {
    order.queueAheadQtyRaw -= trade.qty.raw;
    out.queueAheadAfterE8 = order.queueAheadQtyRaw;
    return out;
  }
  out.requestedQtyE8 = trade.qty.raw - order.queueAheadQtyRaw;
  order.queueAheadQtyRaw = 0;
  out.queueAheadAfterE8 = 0;
  return out;
}

PassiveBboTouchFill consumePassiveQueueForBboTouch(EngineState& engine,
                                                   OpenOrder& order,
                                                   const cxet::composite::BookTickerRuntimeV1& top,
                                                   std::int64_t tsNs) noexcept {
  PassiveBboTouchFill out{};
  if (!order.state.valid || !order.active || order.state.orderType != canon::OrderType::Limit) return out;
  if (tsNs <= 0 || top.ts.raw == 0u ||
      top.ts.raw > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    return out;
  }
  const std::int64_t bboTsNs = static_cast<std::int64_t>(top.ts.raw);
  if (bboTsNs != tsNs || order.lastPassiveBboTouchTsNs == bboTsNs ||
      !passiveBboTouchEligible(order, bboTsNs)) {
    return out;
  }
  if (!order.queueInitialized) {
    order.queueAheadQtyRaw = 0;
    order.queueInitialized = true;
  }
  std::int64_t oppositeQtyE8 = 0;
  if (!makerBboTouchesOrder(order, top, oppositeQtyE8)) return out;
  order.lastPassiveBboTouchTsNs = bboTsNs;
  out.queueAheadBeforeE8 = order.queueAheadQtyRaw;
  out.queueAheadAfterE8 = order.queueAheadQtyRaw;
  if (oppositeQtyE8 <= order.queueAheadQtyRaw) {
    order.queueAheadQtyRaw -= oppositeQtyE8;
    out.queueAheadAfterE8 = order.queueAheadQtyRaw;
    return out;
  }
  out.requestedQtyE8 = oppositeQtyE8 - order.queueAheadQtyRaw;
  order.queueAheadQtyRaw = 0;
  out.queueAheadAfterE8 = 0;
  return out;
}

}  // namespace hft_backtest::detail
