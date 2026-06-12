#pragma once

#include "hft_backtest/backtest.hpp"

#include "backtest_latency.hpp"
#include "session_loader.hpp"

#include "hft_trader/core/market/BboReconstruction.hpp"
#include "hft_trader/core/market/LocalOrderBook.hpp"
#include "hft_trader/core/intents/OrderState.hpp"
#include "hft_trader/core/intents/PositionState.hpp"
#include "hft_trader/core/strategy/StrategyContract.hpp"

#include "canon/PositionAndExchange.hpp"
#include "canon/Subtypes.hpp"
#include "primitives/composite/BookTickerRuntimeV1.hpp"
#include "primitives/composite/OrderBookSnapshot.hpp"
#include "primitives/composite/OrderBookTapeRuntimeV1.hpp"
#include "primitives/composite/TieredCandleHistory.hpp"
#include "primitives/composite/TradeRuntimeV1.hpp"
#include "primitives/composite/UserDataEvent.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace hft_backtest::detail {

constexpr std::size_t kIntentCapacity = 64;
constexpr std::size_t kMaxOrderStates = 256;
constexpr std::size_t kNoRecordIndex = std::numeric_limits<std::size_t>::max();
constexpr std::int64_t kRiskSnapshotIntervalNs = 1000000000ll;
constexpr std::uint8_t kRiskSnapshotReasonStart = 1u;
constexpr std::uint8_t kRiskSnapshotReasonFill = 2u;
constexpr std::uint8_t kRiskSnapshotReasonInterval = 3u;
constexpr std::uint8_t kRiskSnapshotReasonFinal = 4u;
constexpr std::uint8_t kRecordActionNone = 0u;
constexpr std::uint8_t kRecordActionOrder = 1u;
constexpr std::uint8_t kRecordActionAmend = 2u;
constexpr std::uint8_t kRecordActionCancel = 3u;
constexpr std::uint8_t kRecordSideSell = 0u;
constexpr std::uint8_t kRecordSideNone = 0u;
constexpr std::uint8_t kRecordSideBuy = 1u;
constexpr std::uint8_t kRecordTypeNone = 0u;
constexpr std::uint8_t kRecordTypeLimit = 1u;
constexpr std::uint8_t kRecordTypeMarket = 2u;
constexpr std::uint8_t kRecordStatusNone = 0u;
constexpr std::uint8_t kRecordStatusPending = 1u;
constexpr std::uint8_t kRecordStatusAcked = 2u;
constexpr std::uint8_t kRecordStatusActive = kRecordStatusAcked;
constexpr std::uint8_t kRecordStatusFilled = 3u;
constexpr std::uint8_t kRecordStatusCancelled = 4u;
constexpr std::uint8_t kRecordStatusMissing = 5u;
constexpr std::uint8_t kRecordStatusRejected = 6u;

enum class PendingActionKind : std::uint8_t {
  OrderAck = 0,
  CancelAck = 1,
  AmendAck = 2,
  ReplaceCancelAck = 3,
};

struct JsonOrder {
  std::uint32_t legIndex{0u};
  std::uint64_t id{0};
  std::uint64_t targetId{0};
  std::int64_t submitTsNs{0};
  std::int64_t activeTsNs{0};
  std::int64_t tsNs{0};
  std::uint8_t action{kRecordActionNone};
  std::uint8_t side{kRecordSideNone};
  std::uint8_t type{kRecordTypeNone};
  std::uint8_t status{kRecordStatusNone};
  std::int64_t priceE8{0};
  std::int64_t qtyE8{0};
  bool reduceOnly{false};
};

struct JsonFill {
  std::uint32_t legIndex{0u};
  std::uint64_t orderId{0};
  std::int64_t entryTsNs{0};
  std::int64_t exitTsNs{0};
  std::uint8_t side{kRecordSideNone};
  std::int64_t priceE8{0};
  std::int64_t qtyE8{0};
  std::int64_t realizedPnlE8{0};
  bool reduceOnly{false};
};

struct JsonEquityPoint {
  std::int64_t tsNs{0};
  std::int64_t grossRealizedPnlE8{0};
  std::int64_t realizedPnlE8{0};
  std::int64_t unrealizedPnlE8{0};
  std::int64_t grossTotalPnlE8{0};
  std::int64_t netTotalPnlE8{0};
  std::int64_t totalPnlE8{0};
  std::int64_t feesPaidE8{0};
  std::int64_t walletBalanceE8{0};
  std::int64_t positionQtyE8{0};
  std::uint64_t fillCount{0};
};

struct JsonRiskSnapshot {
  std::int64_t tsNs{0};
  std::int64_t grossRealizedPnlE8{0};
  std::int64_t realizedPnlE8{0};
  std::int64_t unrealizedPnlE8{0};
  std::int64_t grossTotalPnlE8{0};
  std::int64_t netTotalPnlE8{0};
  std::int64_t feesPaidE8{0};
  std::int64_t walletBalanceE8{0};
  std::int64_t positionQtyE8{0};
  std::int64_t entryPriceE8{0};
  std::int64_t bidPriceE8{0};
  std::int64_t askPriceE8{0};
  std::uint64_t fillCount{0};
  std::uint8_t reason{0};
};

struct JsonStrategyIndicatorPoint {
  std::int64_t tsNs{0};
  std::int64_t valueRaw{0};
  std::int64_t auxRaw{0};
  std::uint8_t eventCode{0u};
  std::uint8_t reasonRaw{0u};
  std::uint8_t decisionKind{0u};
  std::uint8_t sideRaw{0u};
};

struct JsonProbeMetric {
  std::int64_t tsNs{0};
  std::uint8_t decisionKind{0u};
  std::uint8_t reasonRaw{0u};
  std::uint8_t sideRaw{0u};
  std::uint32_t orderCount{0u};
  std::uint32_t amendCount{0u};
  std::uint32_t cancelCount{0u};
  std::int64_t bestBidRaw{0};
  std::int64_t bestAskRaw{0};
  std::uint32_t tradeRangeSampleBps{0u};
  std::uint32_t tradeRangeEmaBps{0u};
  std::uint32_t tradeRangeObservedSeconds{0u};
  std::uint64_t rollingPerMinuteRaw{0u};
  std::uint32_t volumeWindowObservedSeconds{0u};
  std::uint8_t tradeRangeReady{0u};
  std::uint8_t volumeWindowReady{0u};
  std::uint8_t hasTrade{0u};
  std::uint8_t hasBookTicker{0u};
  std::uint8_t hasDepth{0u};
  std::uint8_t hasOrderBook{0u};
};

struct OpenOrder {
  hft_trader::core::StrategyOrderState state{};
  std::size_t recordIndex{kNoRecordIndex};
  std::int64_t createdTsNs{0};
  std::int64_t activeTsNs{0};
  bool active{false};
  bool reduceOnly{false};
};

struct PendingAction {
  PendingActionKind kind{PendingActionKind::OrderAck};
  std::size_t slotIndex{kNoRecordIndex};
  std::size_t recordIndex{kNoRecordIndex};
  std::size_t replaceCancelRecordIndex{kNoRecordIndex};
  std::int64_t activeTsNs{0};
  Id targetClientIntentId{};
  Id commandClientIntentId{};
  hft_trader::core::AmendIntent amend{};
};

struct EngineState {
  std::vector<JsonOrder> orders{};
  std::vector<JsonFill> fills{};
  std::vector<JsonEquityPoint> equityPoints{};
  std::vector<JsonRiskSnapshot> riskSnapshots{};
  std::vector<JsonProbeMetric> probeMetrics{};
  std::vector<JsonStrategyIndicatorPoint> strategyIndicator{};
  std::string strategyIndicatorProfile{};
  std::string strategyIndicatorTitle{};
  std::string strategyIndicatorValueLabel{};
  std::string strategyIndicatorAuxLabel{};
  std::string strategyIndicatorUnit{};
  std::vector<PendingAction> pending{};
  std::array<OpenOrder, kMaxOrderStates> open{};
  hft_trader::core::StrategyPositionState position{};
  hft_trader::core::StrategyMarketSnapshot snapshot{};
  hft_trader::core::StrategyMarketSnapshot venueSnapshot{};
  hft_trader::core::LocalOrderBookState orderBook{};
  hft_trader::core::LocalOrderBookState venueOrderBook{};
  hft_trader::core::BboReconstructionState bboReconstruction{};
  hft_trader::core::BboReconstructionState venueBboReconstruction{};
  BacktestInputCoverage inputCoverage{};
  std::uint32_t legIndex{0u};
  ExchangeId exchange{};
  canon::MarketType market{};
  Symbol symbol{};
  std::vector<cxet::composite::UserDataEvent> userEvents{};
  BacktestLatencyRuntime latency{};
  std::size_t deliveredUserEvents{0};
  std::uint64_t fillSequence{0};
  std::uint64_t timerWakeEvents{0};
  std::uint64_t reduceOnlyOrders{0};
  std::uint64_t reduceOnlyFills{0};
  std::int64_t netQtyRaw{0};
  std::int64_t entryPriceE8{0};
  std::int64_t grossRealizedPnlE8{0};
  std::int64_t netRealizedPnlE8{0};
  std::int64_t feesPaidE8{0};
  std::int64_t initialBalanceE8{0};
  std::int64_t walletBalanceE8{0};
  std::int64_t makerFeeBpsE8{0};
  std::int64_t takerFeeBpsE8{0};
  std::int64_t lastRiskSnapshotTsNs{std::numeric_limits<std::int64_t>::min()};
  std::int64_t lastRiskSnapshotUnrealizedPnlE8{0};
  bool hasRiskSnapshot{false};
};

inline const cxet::composite::BookTickerRuntimeV1& effectiveBookTicker(
    const hft_trader::core::StrategyMarketSnapshot& snapshot) noexcept {
  return snapshot.hasReconstructedBbo.raw == hft_trader::core::kAlgoFlagTrue.raw
             ? snapshot.reconstructedBookTicker
             : snapshot.lastBookTicker;
}

struct TimelineEvent {
  std::int64_t tsNs{0};
  std::int64_t deliveryTsNs{0};
  std::int64_t ingestSeq{0};
  std::uint8_t kind{0};
  std::uint8_t legIndex{0};
  std::size_t index{0};
};

std::string quote(std::string_view value);
ExchangeId exchangeFromText(const std::string& value) noexcept;
canon::MarketType marketFromText(const std::string& value) noexcept;
Side sideFromRow(std::int64_t raw) noexcept;

cxet::composite::TradeRuntimeV1 toRuntime(const TradeRow& row) noexcept;
cxet::composite::BookTickerRuntimeV1 toRuntime(const BookTickerRow& row) noexcept;
void toRuntime(const DepthRow& row,
               cxet::composite::OrderBookTapeRuntimeV1& tape,
               cxet::composite::OrderBookTapeSidesRuntimeV1& sides) noexcept;
cxet::composite::OrderBookSnapshot toRuntimeSnapshot(const SnapshotDocument& row) noexcept;
cxet::composite::OrderBookSnapshot toRuntimeSnapshot(const DepthRow& row) noexcept;
bool buildTieredCandleHistory(const std::vector<CandleRow>& rows,
                              cxet::composite::TieredCandleHistory& out) noexcept;

std::int64_t mulPriceQtyE8(std::int64_t px, std::int64_t qty) noexcept;
std::int64_t feeForFillE8(std::int64_t priceE8, std::int64_t qtyE8, std::int64_t feeBpsE8) noexcept;
void initializeAccounting(EngineState& engine, const BacktestRunRequest& request) noexcept;
void updatePosition(EngineState& engine, Side side, std::int64_t priceE8, std::int64_t qtyE8, std::int64_t tsNs, std::int64_t feeBpsE8, std::int64_t& netRealized) noexcept;
std::int64_t unrealizedPnlE8(const EngineState& engine) noexcept;
void appendEquityPoint(EngineState& engine, std::int64_t tsNs);
void appendRiskSnapshot(EngineState& engine, std::int64_t tsNs, std::uint8_t reason);
bool appendRiskSnapshotIfDue(EngineState& engine, std::int64_t tsNs);

std::size_t visibleOrders(const EngineState& engine,
                          hft_trader::core::StrategyOrderState* out,
                          std::size_t capacity) noexcept;
std::int64_t delayedTs(std::int64_t tsNs, std::uint64_t latencyUs) noexcept;
bool fillOrder(EngineState& engine, OpenOrder& order, std::int64_t priceE8, std::int64_t tsNs, bool taker);
void tryFillOnTop(EngineState& engine, std::int64_t tsNs);
void tryFillOnTrade(EngineState& engine, const cxet::composite::TradeRuntimeV1& trade);
void applyDueActions(EngineState& engine, std::int64_t tsNs);
bool forceClosePosition(EngineState& engine, std::int64_t tsNs, std::uint64_t orderLatencyUs);
bool forceClosePosition(EngineState& engine, std::int64_t tsNs, BacktestLatencyRuntime& latency);
void cancelOpenOrders(EngineState& engine, std::int64_t tsNs, std::uint64_t cancelLatencyUs);
void cancelOpenOrders(EngineState& engine, std::int64_t tsNs, BacktestLatencyRuntime& latency);
void applyIntents(EngineState& engine,
                  const hft_trader::core::StrategyIo& io,
                  std::int64_t tsNs,
                  std::uint64_t orderLatencyUs,
                  std::uint64_t amendLatencyUs,
                  std::uint64_t cancelLatencyUs);
void applyIntents(EngineState& engine,
                  const hft_trader::core::StrategyIo& io,
                  std::int64_t tsNs,
                  BacktestLatencyRuntime& latency);

void buildTimeline(const SessionData& session, std::vector<TimelineEvent>& out);
void buildTimeline(const std::vector<SessionData>& sessions, std::vector<TimelineEvent>& out);
Status writeResult(const BacktestRunResult& result, const EngineState& engine) noexcept;

}  // namespace hft_backtest::detail
