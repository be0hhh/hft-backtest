#pragma once

#include "hft_backtest/backtest.hpp"

#include "session_loader.hpp"

#include "hft_trader/core/LocalOrderBook.hpp"
#include "hft_trader/core/OrderState.hpp"
#include "hft_trader/core/PositionState.hpp"
#include "hft_trader/core/StrategyContract.hpp"

#include "canon/PositionAndExchange.hpp"
#include "canon/Subtypes.hpp"
#include "primitives/composite/BookTickerRuntimeV1.hpp"
#include "primitives/composite/OrderBookDeltaRuntimeV1.hpp"
#include "primitives/composite/OrderBookSnapshot.hpp"
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

enum class PendingActionKind : std::uint8_t {
  OrderAck = 0,
  CancelAck = 1,
  AmendAck = 2,
};

struct JsonOrder {
  std::uint64_t id{0};
  std::uint64_t targetId{0};
  std::int64_t submitTsNs{0};
  std::int64_t activeTsNs{0};
  std::int64_t tsNs{0};
  std::string action{};
  std::string side{};
  std::string type{};
  std::string status{};
  std::int64_t priceE8{0};
  std::int64_t qtyE8{0};
  bool reduceOnly{false};
};

struct JsonFill {
  std::uint64_t orderId{0};
  std::int64_t entryTsNs{0};
  std::int64_t exitTsNs{0};
  std::string side{};
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
  hft_trader::core::LocalOrderBookState orderBook{};
  BacktestInputCoverage inputCoverage{};
  ExchangeId exchange{};
  Symbol symbol{};
  std::vector<cxet::composite::UserDataEvent> userEvents{};
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

struct TimelineEvent {
  std::int64_t tsNs{0};
  std::int64_t ingestSeq{0};
  std::uint8_t kind{0};
  std::size_t index{0};
};

std::string quote(std::string_view value);
ExchangeId exchangeFromText(const std::string& value) noexcept;
canon::MarketType marketFromText(const std::string& value) noexcept;
Side sideFromRow(std::int64_t raw) noexcept;

cxet::composite::TradeRuntimeV1 toRuntime(const TradeRow& row) noexcept;
cxet::composite::BookTickerRuntimeV1 toRuntime(const BookTickerRow& row) noexcept;
cxet::composite::OrderBookDeltaRuntimeV1 toRuntime(const DepthRow& row) noexcept;
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

std::vector<hft_trader::core::StrategyOrderState> visibleOrders(const EngineState& engine);
std::int64_t delayedTs(std::int64_t tsNs, std::uint64_t latencyUs) noexcept;
bool fillOrder(EngineState& engine, OpenOrder& order, std::int64_t priceE8, std::int64_t tsNs, bool taker);
void tryFillOnTop(EngineState& engine, std::int64_t tsNs);
void tryFillOnTrade(EngineState& engine, const cxet::composite::TradeRuntimeV1& trade);
void applyDueActions(EngineState& engine, std::int64_t tsNs);
bool forceClosePosition(EngineState& engine, std::int64_t tsNs, std::uint64_t orderLatencyUs);
void cancelOpenOrders(EngineState& engine, std::int64_t tsNs, std::uint64_t cancelLatencyUs);
void applyIntents(EngineState& engine,
                  const hft_trader::core::StrategyIo& io,
                  std::int64_t tsNs,
                  std::uint64_t orderLatencyUs,
                  std::uint64_t amendLatencyUs,
                  std::uint64_t cancelLatencyUs);

void buildTimeline(const SessionData& session, std::vector<TimelineEvent>& out);
Status writeResult(const BacktestRunResult& result, const EngineState& engine) noexcept;

}  // namespace hft_backtest::detail
