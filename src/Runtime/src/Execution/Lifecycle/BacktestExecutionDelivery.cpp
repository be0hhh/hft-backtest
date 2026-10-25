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
#include "trading_core/Strategy/Orders/Evidence/OrderDecisionCursor.hpp"

namespace hft_backtest::detail::execution_detail {

cxet::composite::UserDataEvent makeFillEvent(EngineState& engine,
                                             const OpenOrder& order,
                                             std::int64_t priceE8,
                                             std::int64_t qtyE8,
                                             std::int64_t cumulativeQtyE8,
                                             std::int64_t tsNs,
                                             std::int64_t feeE8,
                                             std::int64_t realizedPnlE8,
                                             bool taker,
                                             bool finalFill) noexcept {
  cxet::composite::UserDataEvent event{};
  event.eventId.raw = ++engine.userEventSequence;
  event.exchangeId = engine.exchange;
  event.kind = cxet::composite::UserDataEventKind::Order;
  event.symbol = engine.symbol;
  (void)makeBacktestClientOrderId(
      engine.exchange, Id{simulationCommandId(engine, order.state.clientIntentId, order.contextIndex)},
      event.clientOrderId, false);
  event.orderId = event.clientOrderId;
  event.side = order.state.side;
  event.orderType = order.state.orderType;
  event.orderStatus = finalFill ? canon::OrderStatus::Closed : canon::OrderStatus::PartiallyFilled;
  event.price.raw = priceE8;
  event.avgPrice.raw = cumulativeQtyE8 > 0 && order.filledNotionalRaw > 0
      ? static_cast<std::int64_t>(order.filledNotionalRaw / cumulativeQtyE8) : priceE8;
  event.lastPrice.raw = priceE8;
  event.qty = order.state.qty;
  event.quantityUnit = cxet::contract::Unit::BaseQuantity;
  event.quantityDenomination = cxet::contract::QuantityDenomination::Canonical;
  event.filledQty.raw = cumulativeQtyE8;
  event.lastFilledQty.raw = qtyE8;
  event.fee.raw = feeE8;
  event.realizedPnl.raw = realizedPnlE8;
  event.eventTime.raw = static_cast<std::uint64_t>(tsNs > 0 ? tsNs : 0);
  event.updateTime = event.eventTime;
  event.reduceOnly = order.reduceOnly ? 1u : 0u;
  event.maker = taker ? 0u : 1u;
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::Symbol);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::ClientOrderId);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::OrderId);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::Side);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::OrderType);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::OrderStatus);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::Price);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::AvgPrice);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::LastPrice);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::Qty);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::QuantityUnit);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::QuantityDenomination);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::FilledQty);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::LastFilledQty);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::Fee);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::RealizedPnl);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::EventTime);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::UpdateTime);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::ReduceOnly);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::Maker);
  return event;
}

cxet::composite::UserDataEvent makeRejectedOrderEvent(EngineState& engine,
                                                      const OpenOrder& order,
                                                      std::int64_t tsNs,
                                                      canon::OrderProblem problem) noexcept {
  cxet::composite::UserDataEvent event{};
  event.eventId.raw = ++engine.userEventSequence;
  event.exchangeId = engine.exchange;
  event.kind = cxet::composite::UserDataEventKind::Order;
  event.symbol = engine.symbol;
  (void)makeBacktestClientOrderId(
      engine.exchange, Id{simulationCommandId(engine, order.state.clientIntentId, order.contextIndex)},
      event.clientOrderId, false);
  event.orderId = event.clientOrderId;
  event.side = order.state.side;
  event.orderType = order.state.orderType;
  event.orderStatus = canon::OrderStatus::Rejected;
  event.orderProblem = problem;
  event.qty = order.state.qty;
  event.quantityUnit = cxet::contract::Unit::BaseQuantity;
  event.quantityDenomination = cxet::contract::QuantityDenomination::Canonical;
  event.eventTime.raw = static_cast<std::uint64_t>(tsNs > 0 ? tsNs : 0);
  event.updateTime = event.eventTime;
  event.reduceOnly = order.reduceOnly ? 1u : 0u;
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::Symbol);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::ClientOrderId);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::OrderId);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::Side);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::OrderType);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::OrderStatus);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::OrderProblem);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::Qty);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::QuantityUnit);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::QuantityDenomination);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::EventTime);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::UpdateTime);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::ReduceOnly);
  return event;
}

cxet::composite::UserDataEvent makeCanceledOrderEvent(EngineState& engine,
                                                      const OpenOrder& order,
                                                      std::int64_t tsNs) noexcept {
  cxet::composite::UserDataEvent event{};
  event.eventId.raw = ++engine.userEventSequence;
  event.exchangeId = engine.exchange;
  event.kind = cxet::composite::UserDataEventKind::Order;
  event.symbol = engine.symbol;
  (void)makeBacktestClientOrderId(
      engine.exchange, Id{simulationCommandId(engine, order.state.clientIntentId, order.contextIndex)},
      event.clientOrderId, false);
  event.orderId = event.clientOrderId;
  event.side = order.state.side;
  event.orderType = order.state.orderType;
  event.orderStatus = canon::OrderStatus::Canceled;
  event.price = order.state.price;
  event.qty = order.state.qty;
  event.quantityUnit = cxet::contract::Unit::BaseQuantity;
  event.quantityDenomination = cxet::contract::QuantityDenomination::Canonical;
  event.filledQty.raw = order.filledQtyRaw;
  if (order.filledQtyRaw > 0) {
    event.avgPrice.raw = static_cast<std::int64_t>(order.filledNotionalRaw / order.filledQtyRaw);
    cxet::composite::markUserDataFieldPresent(event, cxet::composite::UserDataField::AvgPrice);
  }
  event.lastFilledQty.raw = 0;
  event.eventTime.raw = static_cast<std::uint64_t>(tsNs > 0 ? tsNs : 0);
  event.updateTime = event.eventTime;
  event.reduceOnly = order.reduceOnly ? 1u : 0u;
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::Symbol);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::ClientOrderId);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::OrderId);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::Side);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::OrderType);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::OrderStatus);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::Price);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::Qty);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::QuantityUnit);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::QuantityDenomination);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::FilledQty);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::LastFilledQty);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::EventTime);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::UpdateTime);
  cxet::composite::markUserDataFieldPresent(
      event, cxet::composite::UserDataField::ReduceOnly);
  return event;
}

std::int64_t userEventDeliveryTs(EngineState& engine, std::int64_t eventTsNs) noexcept {
  const std::uint64_t latencyUs = sampleLatencyUs(engine.latency, BacktestLatencyChannel::UserData);
  const std::int64_t add = latencyNs(latencyUs);
  const auto sampled = eventTsNs > std::numeric_limits<std::int64_t>::max() - add
      ? std::numeric_limits<std::int64_t>::max() : eventTsNs + add;
  // One simulated producer stream preserves its append order. Latency jitter
  // may delay a later event, but cannot deliver it ahead of an earlier one.
  auto scheduled = engine.userEventDeliveryTsNs.empty() ? sampled :
      std::max(sampled, engine.userEventDeliveryTsNs.back());
  if (engine.sharedAccount) {
    scheduled = std::max(scheduled, engine.sharedAccount->lastPrivateDeliveryNs + 1);
    engine.sharedAccount->lastPrivateDeliveryNs = scheduled;
  }
  return scheduled;
}

void pushUserEvent(EngineState& engine, cxet::composite::UserDataEvent event, std::int64_t eventTsNs, Id clientIntentId, std::uint32_t contextIndex, bool service) {
  if (engine.streamingOutput && engine.userEvents.size() >= kMaxOrderStates * 16u) {
    engine.simulationEvidenceFailed = true; return;
  }
  if (engine.sharedAccount && engine.sharedAccount->lastPrivateDeliveryNs == std::numeric_limits<std::int64_t>::max()) {
    engine.simulationEvidenceFailed = true; return;
  }
  const auto deliveryTs = userEventDeliveryTs(engine, eventTsNs);
  const auto savedContext = engine.activeContextIndex;
  engine.activeContextIndex = contextIndex;
  const auto view = simulationOrderEvidenceView(engine);
  const auto* evidence = trading_core::strategy::canonicalOrderEvidenceIdentity(view, clientIntentId, 1u);
  engine.activeContextIndex = savedContext;
  engine.userEventCommandIds.push_back(evidence ? evidence->commandId : 0u);
  engine.userEventGenerations.push_back(evidence ? evidence->generation : 0u);
  engine.userEventServiceFlags.push_back(service ? 1u : 0u);
  engine.userEventContextIndices.push_back(contextIndex);
  engine.userEventConsumed.push_back(0u);
  engine.userEventClientIntentIds.push_back(clientIntentId);
  engine.userEvents.push_back(event);
  engine.userEventDeliveryTsNs.push_back(deliveryTs);
}

void publishProblem(EngineState& engine,
                    canon::OrderProblem problem,
                    std::uint8_t commandKind,
                    Id clientIntentId,
                    Id targetClientIntentId) noexcept {
  if (problem == canon::OrderProblem::None) return;
  engine.pendingProblem = trading_core::StrategyProblemEvent{};
  engine.pendingProblem.problem = problem;
  engine.pendingProblem.commandKind = commandKind;
  engine.pendingProblem.clientIntentId = clientIntentId;
  engine.pendingProblem.targetClientIntentId = targetClientIntentId;
  engine.pendingProblem.valid = true;
  engine.hasPendingProblem = true;
}

void recordRateLimitReject(EngineState& engine, std::uint8_t commandKind, std::int64_t tsNs) noexcept {
  if (commandKind == 3u) ++engine.rateLimitRejectedCancels;
  else ++engine.rateLimitRejectedOrders;
  if (engine.firstRateLimitRejectTsNs == 0 || tsNs < engine.firstRateLimitRejectTsNs) {
    engine.firstRateLimitRejectTsNs = tsNs;
  }
  if (tsNs > engine.lastRateLimitRejectTsNs) engine.lastRateLimitRejectTsNs = tsNs;
}

std::int64_t rateLimitCooldownTsNs(const trading_core::execution::PermitDecision& decision,
                                   std::int64_t tsNs) noexcept {
  const std::uint64_t raw = decision.retryAt;
  std::int64_t out = tsNs < std::numeric_limits<std::int64_t>::max()
      ? tsNs + 1
      : std::numeric_limits<std::int64_t>::max();
  if (raw > 0u && raw <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
    out = static_cast<std::int64_t>(raw);
  }
  if (out <= tsNs && tsNs < std::numeric_limits<std::int64_t>::max()) out = tsNs + 1;
  return out;
}

bool beginRateLimitCooldown(EngineState& engine,
                            const trading_core::execution::PermitDecision& decision,
                            std::int64_t tsNs) noexcept {
  const std::int64_t cooldownTs = rateLimitCooldownTsNs(decision, tsNs);
  const bool notify = engine.rateLimitProblemUntilTsNs <= tsNs;
  if (cooldownTs > engine.rateLimitCooldownUntilTsNs) engine.rateLimitCooldownUntilTsNs = cooldownTs;
  if (cooldownTs > engine.rateLimitProblemUntilTsNs) engine.rateLimitProblemUntilTsNs = cooldownTs;
  return notify;
}

TimeNs timeNsFromI64Local(std::int64_t value) noexcept {
  TimeNs out{};
  out.raw = static_cast<std::uint64_t>(value > 0 ? value : 0);
  return out;
}

std::int64_t remainingPctE4(std::int64_t remaining, std::int64_t limit) noexcept {
  if (limit <= 0) return 0;
  if (remaining <= 0) return 0;
  if (remaining >= limit) return 1000000ll;
  const __int128 numerator = static_cast<__int128>(remaining) * 1000000ll;
  return static_cast<std::int64_t>(numerator / limit);
}

bool sameRateLimitUsagePoint(const JsonRateLimitUsagePoint& lhs,
                             const JsonRateLimitUsagePoint& rhs) noexcept {
  return lhs.legIndex == rhs.legIndex &&
         lhs.tsNs == rhs.tsNs &&
         lhs.minRemainingPctE4 == rhs.minRemainingPctE4 &&
         lhs.remaining == rhs.remaining &&
         lhs.limit == rhs.limit &&
         lhs.bucketKind == rhs.bucketKind &&
         lhs.intervalNs == rhs.intervalNs &&
         lhs.exhausted == rhs.exhausted;
}

}  // namespace hft_backtest::detail::execution_detail

namespace hft_backtest::detail {
using execution_detail::remainingPctE4;
using execution_detail::sameRateLimitUsagePoint;
using execution_detail::timeNsFromI64Local;

void appendRateLimitUsageSnapshot(EngineState& engine, std::int64_t tsNs) {
  if (!engine.quotaOwner().configured()) return;
  trading_core::StrategyRateLimitBook book{};
  engine.quotaOwner().observationBook(trading_core::kDefaultExecutionRouteHandle,
                                     engine.exchange,
                                     engine.market,
                                     1u,
                                     timeNsFromI64Local(tsNs),
                                     book);
  JsonRateLimitUsagePoint best{};
  bool found = false;
  for (std::uint8_t i = 0u; i < book.count; ++i) {
    const trading_core::RateLimitBucketView& bucket = book.buckets[i];
    if (!bucket.valid || bucket.limit <= 0) continue;
    JsonRateLimitUsagePoint candidate{};
    candidate.legIndex = engine.legIndex;
    candidate.tsNs = tsNs;
    candidate.minRemainingPctE4 = remainingPctE4(bucket.remaining, bucket.limit);
    candidate.remaining = bucket.remaining;
    candidate.limit = bucket.limit;
    candidate.bucketKind = bucket.kind;
    candidate.intervalNs = bucket.intervalNs;
    candidate.exhausted = bucket.exhausted;
    if (!found ||
        candidate.minRemainingPctE4 < best.minRemainingPctE4 ||
        (candidate.minRemainingPctE4 == best.minRemainingPctE4 && candidate.intervalNs < best.intervalNs)) {
      best = candidate;
      found = true;
    }
  }
  if (!found) return;
  if (!engine.rateLimitUsage.empty() && sameRateLimitUsagePoint(engine.rateLimitUsage.back(), best)) return;
  if(engine.streamingOutput && engine.rateLimitUsage.size()>=detail::kMaxOrderStates*16u) {
    engine.simulationEvidenceFailed=true;return;
  }
  engine.rateLimitUsage.push_back(best);
}


}  // namespace hft_backtest::detail
