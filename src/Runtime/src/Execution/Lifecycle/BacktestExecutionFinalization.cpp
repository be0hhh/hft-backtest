#include "../../Engine/BacktestEngine.hpp"

#include "../Model/BacktestExecutionDepth.hpp"
#include "../BacktestClientOrderId.hpp"
#include "../Model/BacktestFillModel.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <utility>

#include "../Internal/BacktestExecutionHelpers.hpp"

namespace hft_backtest::detail::execution_detail {

}  // namespace hft_backtest::detail::execution_detail

namespace hft_backtest::detail {
using execution_detail::boundedPositionMagnitude;
using execution_detail::nextOrderId;

bool forceClosePosition(EngineState& engine, std::int64_t tsNs, std::uint64_t orderLatencyUs) {
  if (engine.netQtyRaw == 0) return true;
  if (engine.netQtyRaw == std::numeric_limits<std::int64_t>::min()) return false;
  const cxet::composite::BookTicker& top = effectiveBookTicker(engine.venueSnapshot);
  if (top.bid.px.raw <= 0 || top.ask.px.raw <= 0) return false;

  trading_core::OrderIntent intent{};
  intent.clientIntentId.raw = nextOrderId(engine);
  intent.side = engine.netQtyRaw > 0 ? Side::Sell() : Side::Buy();
  intent.orderType = canon::OrderType::Market;
  intent.qty.raw = boundedPositionMagnitude(engine.netQtyRaw);
  intent.positionReduction = trading_core::PositionReductionIntent::Reduce;

  std::array<trading_core::OrderIntent, 1> orders{intent};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, tsNs, orderLatencyUs, 0u);
  applyDueActions(engine, delayedTs(tsNs, orderLatencyUs));
  return engine.netQtyRaw == 0;
}

bool forceClosePosition(EngineState& engine, std::int64_t tsNs, BacktestLatencyRuntime& latency) {
  if (engine.netQtyRaw == 0) return true;
  if (engine.netQtyRaw == std::numeric_limits<std::int64_t>::min()) return false;
  const cxet::composite::BookTicker& top = effectiveBookTicker(engine.venueSnapshot);
  if (top.bid.px.raw <= 0 || top.ask.px.raw <= 0) return false;

  trading_core::OrderIntent intent{};
  intent.clientIntentId.raw = nextOrderId(engine);
  intent.side = engine.netQtyRaw > 0 ? Side::Sell() : Side::Buy();
  intent.orderType = canon::OrderType::Market;
  intent.qty.raw = boundedPositionMagnitude(engine.netQtyRaw);
  intent.positionReduction = trading_core::PositionReductionIntent::Reduce;

  std::array<trading_core::OrderIntent, 1> orders{intent};
  trading_core::StrategyIo io{};
  io.orderOut = Span<trading_core::OrderIntent>(orders.data(), orders.size());
  io.orderCount = orders.size();
  applyIntents(engine, io, tsNs, latency);
  return true;
}

void cancelOpenOrders(EngineState& engine, std::int64_t tsNs, std::uint64_t cancelLatencyUs) {
  std::array<trading_core::CancelIntent, kMaxOrderStates> cancels{};
  std::size_t cancelCount = 0;
  std::uint64_t commandId = nextOrderId(engine);
  for (const OpenOrder& order : engine.open) {
    if (!order.state.valid || cancelCount >= cancels.size()) continue;
    trading_core::CancelIntent& intent = cancels[cancelCount++];
    intent.clientIntentId.raw = commandId++;
    intent.targetClientIntentId = order.state.clientIntentId;
    intent.targetOrderType = order.state.orderType;
  }
  if (cancelCount == 0) return;

  trading_core::StrategyIo io{};
  io.cancelOut = Span<trading_core::CancelIntent>(cancels.data(), cancelCount);
  io.cancelCount = cancelCount;
  applyIntents(engine, io, tsNs, 0u, cancelLatencyUs);
  applyDueActions(engine, delayedTs(tsNs, cancelLatencyUs));
}

void cancelOpenOrders(EngineState& engine, std::int64_t tsNs, BacktestLatencyRuntime& latency) {
  std::array<trading_core::CancelIntent, kMaxOrderStates> cancels{};
  std::size_t cancelCount = 0;
  std::uint64_t commandId = nextOrderId(engine);
  for (const OpenOrder& order : engine.open) {
    if (!order.state.valid || cancelCount >= cancels.size()) continue;
    trading_core::CancelIntent& intent = cancels[cancelCount++];
    intent.clientIntentId.raw = commandId++;
    intent.targetClientIntentId = order.state.clientIntentId;
    intent.targetOrderType = order.state.orderType;
  }
  if (cancelCount == 0) return;

  trading_core::StrategyIo io{};
  io.cancelOut = Span<trading_core::CancelIntent>(cancels.data(), cancelCount);
  io.cancelCount = cancelCount;
  const std::uint64_t maxCancelLatencyUs = latencyProfileMaxUs(latency.config.cancelOrder);
  applyIntents(engine, io, tsNs, latency);
  applyDueActions(engine, delayedTs(tsNs, maxCancelLatencyUs));
}


}  // namespace hft_backtest::detail
