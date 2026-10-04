#pragma once

#include "../../Engine/BacktestEngine.hpp"

#include "../Model/BacktestExecutionDepth.hpp"
#include "../BacktestClientOrderId.hpp"
#include "../Model/BacktestFillModel.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <utility>


namespace hft_backtest::detail::execution_detail {

constexpr std::int64_t kMarketDepthMaxAgeNs = 500000000ll;


std::size_t findOpenIndex(EngineState& engine, Id id) noexcept;
std::size_t findFreeIndex(EngineState& engine) noexcept;
bool matchesExpectedOrder(const OpenOrder& order, const PendingAction& action) noexcept;
std::int64_t latencyNs(std::uint64_t latencyUs) noexcept;
std::uint8_t recordSide(Side side) noexcept;
std::uint8_t recordType(canon::OrderType type) noexcept;
std::uint64_t nextOrderId(const EngineState& engine) noexcept;
Side oppositeSide(Side side) noexcept;
bool serviceOrder(const OpenOrder& order) noexcept;
std::int64_t addMs(std::int64_t tsNs, std::uint32_t ms) noexcept;
bool pendingEarlier(const PendingAction& lhs, const PendingAction& rhs) noexcept;
void pushPendingAction(EngineState& engine, PendingAction action);
bool submitCancelIntent(EngineState& engine,
                        const trading_core::CancelIntent& intent,
                        std::int64_t tsNs,
                        std::int64_t activeTs);
[[nodiscard]] bool submitOrderIntent(EngineState& engine,
                                     const trading_core::OrderIntent& intent,
                                     std::int64_t tsNs,
                                     std::int64_t activeTs,
                                     bool warmup = false,
                                     std::uint32_t warmupCancelTimeoutMs = 0u,
                                     bool warmupCancelAfterAck = false);
cxet::composite::UserDataEvent makeFillEvent(EngineState& engine,
                                             const OpenOrder& order,
                                             std::int64_t priceE8,
                                             std::int64_t qtyE8,
                                             std::int64_t cumulativeQtyE8,
                                             std::int64_t tsNs,
                                             std::int64_t feeE8,
                                             std::int64_t realizedPnlE8,
                                             bool taker,
                                             bool finalFill) noexcept;
cxet::composite::UserDataEvent makeRejectedOrderEvent(EngineState& engine,
                                                      const OpenOrder& order,
                                                      std::int64_t tsNs,
                                                      canon::OrderProblem problem = canon::OrderProblem::None) noexcept;
cxet::composite::UserDataEvent makeCanceledOrderEvent(EngineState& engine,
                                                      const OpenOrder& order,
                                                      std::int64_t tsNs) noexcept;
std::int64_t userEventDeliveryTs(EngineState& engine, std::int64_t eventTsNs) noexcept;
void pushUserEvent(EngineState& engine, cxet::composite::UserDataEvent event, std::int64_t eventTsNs);
void publishProblem(EngineState& engine,
                    canon::OrderProblem problem,
                    std::uint8_t commandKind,
                    Id clientIntentId,
                    Id targetClientIntentId) noexcept;
void recordRateLimitReject(EngineState& engine, std::uint8_t commandKind, std::int64_t tsNs) noexcept;
std::int64_t rateLimitCooldownTsNs(const trading_core::execution::PermitDecision& decision,
                                   std::int64_t tsNs) noexcept;
bool beginRateLimitCooldown(EngineState& engine,
                            const trading_core::execution::PermitDecision& decision,
                            std::int64_t tsNs) noexcept;
TimeNs timeNsFromI64Local(std::int64_t value) noexcept;
std::int64_t remainingPctE4(std::int64_t remaining, std::int64_t limit) noexcept;
bool sameRateLimitUsagePoint(const JsonRateLimitUsagePoint& lhs,
                             const JsonRateLimitUsagePoint& rhs) noexcept;
std::int64_t boundedPositionMagnitude(std::int64_t netQtyRaw) noexcept;
std::int64_t reduceOnlyPositionCloseQty(const EngineState& engine, const OpenOrder& order) noexcept;
std::int64_t fillableOrderQty(const EngineState& engine,
                              const OpenOrder& order,
                              std::int64_t requestedQty) noexcept;
bool rejectOrder(EngineState& engine, OpenOrder& order, std::int64_t tsNs);
void terminalizeUnavailableLiquidity(EngineState& engine, OpenOrder& order, std::int64_t tsNs);
std::int64_t triggeredStopLimitBboPrice(const OpenOrder& order,
                                        const cxet::composite::BookTicker& top) noexcept;
std::int64_t triggeredStopLimitBboQty(const OpenOrder& order,
                                      const cxet::composite::BookTicker& top) noexcept;
bool triggeredStopLimitBboCapOk(const OpenOrder& order, std::int64_t topPrice) noexcept;
void fillTriggeredStopLimitFromBbo(EngineState& engine,
                                   OpenOrder& order,
                                   const cxet::composite::BookTicker& top,
                                   std::int64_t tsNs,
                                   FillExecutionMetadata metadata);
bool isPostOnlyPolicy(trading_core::LimitTimeInForcePolicy policy) noexcept;
bool postOnlyCrossesTop(const EngineState& engine, const OpenOrder& order) noexcept;
bool stopMarketCrossed(const OpenOrder& order, std::int64_t sourcePrice) noexcept;
bool knownStopMarketTriggerSource(canon::OrderTriggerSource source) noexcept;
bool badStopMarketIntent(const trading_core::OrderIntent& intent) noexcept;
bool stopMarketSourcePrice(const EngineState& engine,
                           const OpenOrder& order,
                           std::int64_t& outPrice) noexcept;
bool stopMarketCrossesAtActivation(const EngineState& engine, const OpenOrder& order) noexcept;
bool marketExecutionOrder(const OpenOrder& order) noexcept;
const trading_core::LocalOrderBookState* executionDepthBook(EngineState& engine,
                                                               std::int64_t tsNs) noexcept;
bool venueOrderBookExecutionConfigured(const EngineState& engine) noexcept;
bool needsTopExecutionBook(const OpenOrder& order, std::int64_t bid, std::int64_t ask) noexcept;
bool hasTopExecutionCandidate(const EngineState& engine, std::int64_t bid, std::int64_t ask) noexcept;
bool bboOverlayCandidate(const cxet::composite::BookTicker& top) noexcept;
trading_core::AlgoFlag effectiveBidQtyKnown(
    const trading_core::StrategyMarketSnapshot& snapshot,
    const cxet::composite::BookTicker& top) noexcept;
trading_core::AlgoFlag effectiveAskQtyKnown(
    const trading_core::StrategyMarketSnapshot& snapshot,
    const cxet::composite::BookTicker& top) noexcept;
const trading_core::LocalOrderBookState& overlayedTakerExecutionBook(
    const trading_core::StrategyMarketSnapshot& snapshot,
    const cxet::composite::BookTicker& top,
    const trading_core::LocalOrderBookState& rawBook,
    trading_core::LocalOrderBookState& scratch) noexcept;
bool marketDepthFreshEnough(const trading_core::LocalOrderBookState& book,
                            const cxet::composite::BookTicker& top) noexcept;
trading_core::LocalOrderBookState& topExecutionOverlayScratch(
    EngineState& engine,
    const trading_core::LocalOrderBookState& source);
AggressiveFillPlan& topExecutionFillPlanScratch(EngineState& engine);
bool marketDepthPlanUsable(const OpenOrder& order,
                           const AggressiveFillPlan& plan,
                           const trading_core::LocalOrderBookState& book,
                           const cxet::composite::BookTicker& top) noexcept;
void activateOrder(EngineState& engine, OpenOrder& order, std::int64_t activeTsNs);

}  // namespace hft_backtest::detail::execution_detail
