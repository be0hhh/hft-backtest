#include "BacktestFillModel.hpp"
#include "BacktestExecutionDepth.hpp"
#include "../Internal/BacktestExecutionHelpers.hpp"

#include <algorithm>
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
                                          const cxet::composite::Trade& trade) noexcept {
  const std::int64_t minCross = makerFillMinCrossE8(engine);
  if (minCross == std::numeric_limits<std::int64_t>::max()) return false;
  if (trade.initiatorSide() == cxet::composite::TradeInitiatorSide::Buyer && order.state.side.raw == Side::Sell().raw) {
    if (order.state.price.raw > std::numeric_limits<std::int64_t>::max() - minCross) return false;
    return trade.price().raw >= order.state.price.raw + minCross;
  }
  if (trade.initiatorSide() == cxet::composite::TradeInitiatorSide::Seller && order.state.side.raw == Side::Buy().raw) {
    if (order.state.price.raw < std::numeric_limits<std::int64_t>::min() + minCross) return false;
    return trade.price().raw <= order.state.price.raw - minCross;
  }
  return false;
}

[[nodiscard]] trading_core::BookSide passiveBookSide(const OpenOrder& order) noexcept {
  return order.state.side.raw == Side::Buy().raw ? trading_core::BookSide::Bid
                                                 : trading_core::BookSide::Ask;
}

[[nodiscard]] trading_core::BookSide aggressiveBookSide(const OpenOrder& order) noexcept {
  return order.state.side.raw == Side::Buy().raw ? trading_core::BookSide::Ask
                                                 : trading_core::BookSide::Bid;
}

[[nodiscard]] bool levelExecutableByLimit(const OpenOrder& order, std::int64_t levelPriceE8) noexcept {
  if (order.state.orderType == canon::OrderType::Market) return true;
  if (order.state.orderType == canon::OrderType::Stop && order.stopTriggered) {
    if (order.state.price.raw <= 0 || (order.state.triggeredTimeInForce != canon::TimeInForce::IOC &&
                                       order.state.triggeredTimeInForce != canon::TimeInForce::FOK)) {
      return true;
    }
    if (order.state.side.raw == Side::Buy().raw) return levelPriceE8 <= order.state.price.raw;
    return levelPriceE8 >= order.state.price.raw;
  }
  if (order.state.side.raw == Side::Buy().raw) return levelPriceE8 <= order.state.price.raw;
  return levelPriceE8 >= order.state.price.raw;
}

[[nodiscard]] std::int64_t visibleQtyAtPassivePrice(const EngineState& engine,
                                                    const OpenOrder& order,
                                                    const trading_core::LocalOrderBookState& book) noexcept {
  if (order.state.price.raw <= 0) return 0;
  const trading_core::LocalOrderBookView view = trading_core::orderBook(book);
  const trading_core::BookSide bookSide = passiveBookSide(order);
  const trading_core::LocalOrderBookSideState* side = view.side(bookSide);
  if (side == nullptr) return 0;
  for (std::uint32_t i = 0u; i < side->activeCount.raw; ++i) {
    trading_core::LocalOrderBookLevel level{};
    if (!view.readLevel(bookSide, Index{i}, level)) return 0;
    if (level.px.raw == order.state.price.raw)
      return depthLiquidity(engine).available(bookSide, level.px.raw, level.qty.raw);
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

bool triggeredStopLimitOrder(const OpenOrder& order) noexcept {
  return order.state.valid &&
         order.state.orderType == canon::OrderType::Stop &&
         order.stopTriggered &&
         order.state.price.raw > 0 &&
         (order.state.triggeredTimeInForce == canon::TimeInForce::IOC ||
          order.state.triggeredTimeInForce == canon::TimeInForce::FOK);
}

bool triggeredStopFokLimitOrder(const OpenOrder& order) noexcept {
  return triggeredStopLimitOrder(order) &&
         order.state.triggeredTimeInForce == canon::TimeInForce::FOK;
}

void initializePassiveQueue(OpenOrder& order, const EngineState& engine) noexcept {
  initializePassiveQueue(order, engine, engine.executionDepthProvider
      ? engine.executionDepthProvider->book() : engine.venueOrderBook);
}

void initializePassiveQueue(OpenOrder& order,
                            const EngineState& engine,
                            const trading_core::LocalOrderBookState& book) noexcept {
  if (!order.state.valid || !order.active || order.state.orderType != canon::OrderType::Limit) return;
  if (order.queueInitialized) return;
  // Each price has one external queue. Own orders are ordered separately and
  // never each charge the same external liquidity against a trade.
  for (const OpenOrder& peer : engine.open) {
    if (&peer != &order && peer.state.valid && peer.active && peer.queueInitialized &&
        peer.state.orderType == canon::OrderType::Limit &&
        peer.state.side.raw == order.state.side.raw && peer.state.price.raw == order.state.price.raw &&
        peer.sourceGeneration == order.sourceGeneration &&
        peer.executionBoundaryEpoch == order.executionBoundaryEpoch) {
      order.queueAheadQtyRaw = peer.queueAheadQtyRaw;
      order.queueInitialized = true;
      return;
    }
  }
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
                                 const trading_core::LocalOrderBookState& book,
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
  if (limitOrder && order.aggressiveEvaluated) return false;

  const trading_core::LocalOrderBookView view = trading_core::orderBook(book);
  const trading_core::BookSide bookSide = aggressiveBookSide(order);
  const trading_core::LocalOrderBookSideState* side = view.side(bookSide);
  if (side == nullptr) return false;
  out.bookTsNs = book.ts.raw <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())
                     ? static_cast<std::int64_t>(book.ts.raw)
                     : std::numeric_limits<std::int64_t>::max();
  if (side->activeCount.raw != 0u) {
    trading_core::LocalOrderBookLevel level{};
    if (!view.readLevel(bookSide, Index{0u}, level)) return false;
    out.referencePriceE8 = level.px.raw;
  }

  std::int64_t visibleExecutableQty = 0;
  for (std::uint32_t i = 0u; i < side->activeCount.raw; ++i) {
    trading_core::LocalOrderBookLevel level{};
    if (!view.readLevel(bookSide, Index{i}, level)) return false;
    if (level.px.raw <= 0 || level.qty.raw <= 0) continue;
    if (!levelExecutableByLimit(order, level.px.raw)) break;
    const std::int64_t available = depthLiquidity(engine).available(bookSide, level.px.raw, level.qty.raw);
    if (visibleExecutableQty > std::numeric_limits<std::int64_t>::max() - available) {
      visibleExecutableQty = std::numeric_limits<std::int64_t>::max();
      break;
    }
    // Sum only liquidity still available to this simulation.
    visibleExecutableQty += available;
  }

  if (visibleExecutableQty <= 0) {
    if (!marketOrder) return false;
    out.visibleDepthCapped = true;
    return true;
  }

  __int128 notionalSum = 0;
  std::int64_t remaining = out.requestedQtyE8;
  for (std::uint32_t i = 0u; i < side->activeCount.raw && remaining > 0 && out.count < out.chunks.size(); ++i) {
    trading_core::LocalOrderBookLevel level{};
    if (!view.readLevel(bookSide, Index{i}, level)) return false;
    if (level.px.raw <= 0 || level.qty.raw <= 0) continue;
    if (!levelExecutableByLimit(order, level.px.raw)) break;
    const std::int64_t available = depthLiquidity(engine).available(bookSide, level.px.raw, level.qty.raw);
    const std::int64_t fillQty = available < remaining ? available : remaining;
    if (fillQty <= 0) continue;
    remaining -= fillQty;
    out.filledQtyE8 += fillQty;
    notionalSum += static_cast<__int128>(level.px.raw) * static_cast<__int128>(fillQty);
    out.chunks[out.count++] = AggressiveFillChunk{
        level.px.raw,
        fillQty,
        cumulativeAvgPrice(notionalSum, out.filledQtyE8),
        available,
        visibleExecutableQty,
        consumedPctE8(out.filledQtyE8, visibleExecutableQty),
    };
  }

  out.remainingQtyE8 = out.requestedQtyE8 - out.filledQtyE8;
  out.visibleDepthCapped = out.remainingQtyE8 > 0;
  finalizeAggressivePlan(order, out);
  return out.filledQtyE8 > 0 || marketOrder;
}

ExecutionLiquidity& depthLiquidity(EngineState& engine) noexcept {
  return engine.executionDepthProvider ? engine.executionDepthProvider->liquidity()
                                       : engine.executionLiquidity;
}

const ExecutionLiquidity& depthLiquidity(const EngineState& engine) noexcept {
  return engine.executionDepthProvider ? engine.executionDepthProvider->liquidity()
                                       : engine.executionLiquidity;
}

void tryFillOnTrade(EngineState& engine, const cxet::composite::Trade& trade,
                    std::int64_t venueTsNs, std::int64_t depthCursorTsNs) {
  if (engine.liquidated || engine.riskStopped || engine.simulationEvidenceFailed || trade.qty().raw <= 0) return;
  const bool buyer = trade.initiatorSide() == cxet::composite::TradeInitiatorSide::Buyer;
  const bool seller = trade.initiatorSide() == cxet::composite::TradeInitiatorSide::Seller;
  if (!buyer && !seller) return;
  const auto cursorTs = depthCursorTsNs > 0 ? depthCursorTsNs : venueTsNs;
  const auto* book = execution_detail::executionDepthBook(engine, cursorTs);
  std::array<OpenOrder*, kMaxOrderStates> candidates{};
  std::size_t count = 0u;
  for (OpenOrder& order : engine.open) {
    if (engine.capturedSourceManaged && (!engine.capturedSourceMember ||
        order.sourceGeneration != engine.capturedSourceGeneration ||
        order.executionBoundaryEpoch != engine.executionBoundaryEpoch)) continue;
    if (!order.state.valid || !order.active || order.activeTsNs > venueTsNs ||
        order.state.orderType != canon::OrderType::Limit ||
        !makerTradeCrossesOrder(engine, order, trade)) continue;
    initializePassiveQueue(order, engine);
    candidates[count++] = &order;
  }
  // A recorded trade consumes depth at its price; a trade through a better
  // level proves that level unavailable. Neither case can create a level.
  if (book) {
    const auto side = buyer ? trading_core::BookSide::Ask : trading_core::BookSide::Bid;
    const auto view = trading_core::orderBook(*book);
    const auto* levels = view.side(side);
    for (std::uint32_t i = 0u; levels && i < levels->activeCount.raw; ++i) {
      trading_core::LocalOrderBookLevel level{};
      if (!view.readLevel(side, Index{i}, level)) break;
      const bool better = buyer ? level.px.raw < trade.price().raw : level.px.raw > trade.price().raw;
      if (!better && level.px.raw != trade.price().raw) break;
      const auto available = depthLiquidity(engine).available(side, level.px.raw, level.qty.raw);
      const auto consumed = better ? available : std::min(trade.qty().raw, available);
      if (consumed > 0 && !depthLiquidity(engine).consume(side, level.px.raw, consumed)) {
        engine.simulationEvidenceFailed = true;
        return;
      }
      if (!better) break;
    }
  }
  std::sort(candidates.begin(), candidates.begin() + count,
      [](const OpenOrder* left, const OpenOrder* right) noexcept {
        if (left->state.price.raw != right->state.price.raw)
          return left->state.side.raw == Side::Buy().raw
              ? left->state.price.raw > right->state.price.raw
              : left->state.price.raw < right->state.price.raw;
        if (left->activeTsNs != right->activeTsNs) return left->activeTsNs < right->activeTsNs;
        if (left->activationSequence != right->activationSequence)
          return left->activationSequence < right->activationSequence;
        return left->recordIndex < right->recordIndex;
      });
  std::int64_t remaining = trade.qty().raw;
  for (std::size_t i = 0u; i < count && remaining > 0; ++i) {
    OpenOrder& order = *candidates[i];
    if (!order.state.valid) continue;
    const std::int64_t before = order.queueAheadQtyRaw;
    const std::int64_t externalConsumed = std::min(before, remaining);
    remaining -= externalConsumed;
    const std::int64_t after = before - externalConsumed;
    for (OpenOrder& peer : engine.open) {
      if (peer.state.valid && peer.active && peer.queueInitialized &&
          peer.state.orderType == canon::OrderType::Limit &&
          peer.state.side.raw == order.state.side.raw && peer.state.price.raw == order.state.price.raw &&
          peer.sourceGeneration == order.sourceGeneration &&
          peer.executionBoundaryEpoch == order.executionBoundaryEpoch)
        peer.queueAheadQtyRaw = after;
    }
    if (remaining <= 0) break;
    FillExecutionMetadata metadata{};
    metadata.depthCursorTsNs = cursorTs;
    metadata.fillReason = kFillReasonPassiveQueue;
    metadata.queueAheadBeforeE8 = before;
    metadata.queueAheadAfterE8 = after;
    const std::size_t fillsBefore = engine.fills.size();
    (void)fillOrderQty(engine, order, order.state.price.raw, remaining, venueTsNs, false, metadata);
    if (engine.fills.size() > fillsBefore) remaining -= engine.fills[fillsBefore].qtyE8;
    if (engine.simulationEvidenceFailed) return;
  }
}

}  // namespace hft_backtest::detail
