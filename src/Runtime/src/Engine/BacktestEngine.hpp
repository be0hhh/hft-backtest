#pragma once

#include "hft_backtest/Backtest.hpp"

#include "../Execution/Model/BacktestLatency.hpp"
#include "../Input/Session/SessionLoader.hpp"

#include "cxet/Api/Market/PublicMarketDataTypes.hpp"
#include "trading_core/Algorithm/Market/BboReconstruction.hpp"
#include "trading_core/Algorithm/Market/LocalOrderBook.hpp"
#include "trading_core/Contracts/Intents/OrderState.hpp"
#include "trading_core/Contracts/Intents/PositionState.hpp"
#include "trading_core/Contracts/Strategy/StrategyContract.hpp"
#include "trading_core/Contracts/Strategy/StrategyDecisionTrace.hpp"
#include "trading_core/Needed/FillPositionAccounting.hpp"

#include "cxet/Canon/PositionAndExchange.hpp"
#include "cxet/Canon/Subtypes.hpp"
#include "cxet/Primitives/Composite/BookTicker.hpp"
#include "cxet/Runtime/Market/LocalOrderBook.hpp"
#include "cxet/Primitives/Composite/TieredCandleHistory.hpp"
#include "cxet/Primitives/Composite/Trade.hpp"
#include "cxet/Primitives/Composite/UserDataEvent.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hft_backtest::detail {

class ExecutionDepthProvider;

// Replay owns corpus conversion and scheduling; CXET alone reconstructs depth.
// Full normalized levels are distinct from the bounded strategy measurements.
class ReplayDepthBook final {
 public:
  struct IndexedMutation {
    cxet::market::DepthMutation value{};
    std::uint32_t ordinal{0u};
  };
  [[nodiscard]] bool prepare() noexcept;
  void reset() noexcept;
  [[nodiscard]] bool apply(std::span<const PriceLevelRow> levels,
      std::int64_t exchangeTsNs, const hftrec::replay::EventArrival& arrival,
      bool snapshot, cxet::market::DepthSequence capturedSequence = {},
      std::uint64_t capturedPublicationSequence = 0u) noexcept;
  [[nodiscard]] bool restore(const cxet::runtime::market::LocalOrderBookCheckpointView& checkpoint,
      const hftrec::replay::EventArrival& arrival, bool sourcePosition) noexcept;
  [[nodiscard]] cxet::api::market::PublicMarketOrderBookView view() const noexcept;
  [[nodiscard]] const cxet::runtime::market::LocalOrderBook* book() const noexcept {
    return books_.book(1u);
  }
  [[nodiscard]] const hftrec::replay::EventArrival& arrival() const noexcept { return arrival_; }
  [[nodiscard]] bool sourcePosition() const noexcept { return sourcePosition_; }
 private:
  cxet::runtime::market::LocalOrderBookStore books_{};
  std::unique_ptr<IndexedMutation[]> scratch_{};
  hftrec::replay::EventArrival arrival_{};
  bool sourcePosition_{true};
};


constexpr std::size_t kIntentCapacity = 64;
constexpr std::size_t kMaxOrderStates = 256;
constexpr std::size_t kNoRecordIndex = std::numeric_limits<std::size_t>::max();
constexpr std::int64_t kRiskSnapshotIntervalNs = 1000000000ll;
constexpr std::uint8_t kRiskSnapshotReasonStart = 1u;
constexpr std::uint8_t kRiskSnapshotReasonFill = 2u;
constexpr std::uint8_t kRiskSnapshotReasonInterval = 3u;
constexpr std::uint8_t kRiskSnapshotReasonFinal = 4u;
constexpr std::uint8_t kRiskSnapshotReasonLiquidation = 5u;
constexpr std::uint8_t kRiskSnapshotReasonTraderRiskStop = 6u;
constexpr std::uint8_t kTimelineKindDepth = 0u;
constexpr std::uint8_t kTimelineKindTrade = 1u;
constexpr std::uint8_t kTimelineKindBookTicker = 2u;
constexpr std::uint8_t kTimelineKindCount = 3u;
constexpr std::uint8_t kRecordActionNone = 0u;
constexpr std::uint8_t kRecordActionOrder = 1u;
constexpr std::uint8_t kRecordActionCancel = 3u;
constexpr std::uint8_t kRecordSideSell = 0u;
constexpr std::uint8_t kRecordSideNone = 0u;
constexpr std::uint8_t kRecordSideBuy = 1u;
constexpr std::uint8_t kRecordTypeNone = 0u;
constexpr std::uint8_t kRecordTypeLimit = 1u;
constexpr std::uint8_t kRecordTypeMarket = 2u;
constexpr std::uint8_t kRecordTypeStopMarket = 3u;
constexpr std::uint8_t kRecordStatusNone = 0u;
constexpr std::uint8_t kRecordStatusPending = 1u;
constexpr std::uint8_t kRecordStatusAcked = 2u;
constexpr std::uint8_t kRecordStatusActive = kRecordStatusAcked;
constexpr std::uint8_t kRecordStatusFilled = 3u;
constexpr std::uint8_t kRecordStatusCancelled = 4u;
constexpr std::uint8_t kRecordStatusMissing = 5u;
constexpr std::uint8_t kRecordStatusRejected = 6u;
constexpr std::uint8_t kRecordStatusPartialUnfilled = 7u;
constexpr std::uint8_t kFillReasonUnknown = 0u;
constexpr std::uint8_t kFillReasonTop = 1u;
constexpr std::uint8_t kFillReasonTrade = 2u;
constexpr std::uint8_t kFillReasonAggressiveBook = 3u;
constexpr std::uint8_t kFillReasonPassiveQueue = 4u;
constexpr std::uint8_t kFillReasonPassiveBboTouch = 5u;
constexpr std::size_t kFillReasonCount = 6u;
constexpr std::uint8_t kFillLiquidityMaker = 0u;
constexpr std::uint8_t kFillLiquidityTaker = 1u;
constexpr std::size_t kStrategyIndicatorFeatureCapacity = 10u;

enum class PendingActionKind : std::uint8_t {
  OrderAck = 0,
  CancelAck = 1,
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
  bool postOnly{false};
  std::int64_t triggerPriceE8{0};
  std::uint8_t triggerSource{static_cast<std::uint8_t>(canon::OrderTriggerSource::Last)};
  std::int64_t triggerSourcePriceE8{0};
  std::int64_t triggerTsNs{0};
  bool service{false};
  bool warmup{false};
  bool serviceAutounwind{false};
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
  std::uint8_t fillReason{kFillReasonUnknown};
  std::uint8_t liquidity{kFillLiquidityMaker};
  std::int64_t orderQtyE8{0};
  std::int64_t cumulativeFilledQtyE8{0};
  std::int64_t remainingQtyE8{0};
  std::int64_t avgPriceE8{0};
  std::int64_t bookLevelQtyE8{0};
  std::int64_t bookVisibleExecutableQtyE8{0};
  std::int64_t bookConsumedPctE8{0};
  std::int64_t queueAheadBeforeE8{0};
  std::int64_t queueAheadAfterE8{0};
  std::uint32_t chunkIndex{0};
  std::uint32_t chunkCount{0};
  std::int64_t executionQtyE8{0};
  std::int64_t executionAvgPriceE8{0};
  std::int64_t referencePriceE8{0};
  std::int64_t slippageE8{0};
  std::int64_t slippageBpsE8{0};
  std::int64_t executionBookConsumedPctE8{0};
};

struct FillExecutionMetadata {
  std::uint8_t fillReason{kFillReasonUnknown};
  std::int64_t avgPriceE8{0};
  std::int64_t bookLevelQtyE8{0};
  std::int64_t bookVisibleExecutableQtyE8{0};
  std::int64_t bookConsumedPctE8{0};
  std::int64_t queueAheadBeforeE8{0};
  std::int64_t queueAheadAfterE8{0};
  std::uint32_t chunkIndex{0};
  std::uint32_t chunkCount{0};
  std::int64_t executionQtyE8{0};
  std::int64_t executionAvgPriceE8{0};
  std::int64_t referencePriceE8{0};
  std::int64_t slippageE8{0};
  std::int64_t slippageBpsE8{0};
  std::int64_t executionBookConsumedPctE8{0};
};

struct AggressiveFillChunk {
  std::int64_t priceE8{0};
  std::int64_t qtyE8{0};
  std::int64_t avgPriceE8{0};
  std::int64_t bookLevelQtyE8{0};
  std::int64_t bookVisibleExecutableQtyE8{0};
  std::int64_t bookConsumedPctE8{0};
  std::uint32_t chunkIndex{0};
  std::uint32_t chunkCount{0};
  std::int64_t executionQtyE8{0};
  std::int64_t executionAvgPriceE8{0};
  std::int64_t referencePriceE8{0};
  std::int64_t slippageE8{0};
  std::int64_t slippageBpsE8{0};
  std::int64_t executionBookConsumedPctE8{0};
};

struct AggressiveFillPlan {
  std::array<AggressiveFillChunk, rawdata::kMaxOrderbookLevels> chunks{};
  std::size_t count{0u};
  std::int64_t requestedQtyE8{0};
  std::int64_t filledQtyE8{0};
  std::int64_t remainingQtyE8{0};
  std::int64_t bookTsNs{0};
  std::int64_t referencePriceE8{0};
  std::int64_t executionAvgPriceE8{0};
  std::int64_t slippageE8{0};
  std::int64_t slippageBpsE8{0};
  std::int64_t executionBookConsumedPctE8{0};
  bool visibleDepthCapped{false};
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
  std::array<std::int64_t, kStrategyIndicatorFeatureCapacity> featureRaw{};
};

struct JsonStrategyRangePoint {
  std::int64_t tsNs{0};
  std::int64_t lowRaw{0};
  std::int64_t midRaw{0};
  std::int64_t highRaw{0};
};

struct JsonStrategySpreadPoint {
  std::int64_t tsNs{0};
  std::int64_t spreadBpsE8{0};
  std::int64_t emaBpsE8{0};
  std::int64_t deviationBpsE8{0};
  std::int64_t costBandBpsE8{0};
  std::int64_t edgeAfterCostBpsE8{0};
  std::uint8_t direction{0u};
  std::uint8_t decisionKind{0u};
  std::uint8_t reasonRaw{0u};
};

struct JsonStrategySpreadDiagnostics {
  std::uint64_t rows{0u};
  std::int64_t minEdgeAfterCostBpsE8{0};
  std::int64_t maxEdgeAfterCostBpsE8{0};
  std::int64_t maxAbsDeviationBpsE8{0};
  std::array<std::array<std::uint64_t, trading_core::kStrategyDecisionReasonCapacity>,
             trading_core::kStrategyDecisionKindCount> decisionCounts{};
};

struct JsonExecutionQualityLeg {
  std::uint64_t clientIntentId{0u};
  std::uint32_t legIndex{0u};
  std::uint8_t sideRaw{0u};
  std::int64_t expectedQtyE8{0};
  std::int64_t filledQtyE8{0};
  std::int64_t decisionPriceE8{0};
  std::int64_t fillVwapE8{0};
  std::int64_t firstFillTsNs{0};
  std::int64_t lastFillTsNs{0};
};

struct JsonExecutionQualityAction {
  std::uint64_t actionId{0u};
  std::uint64_t positionCycleId{0u};
  std::uint64_t transactionId{0u};
  std::uint64_t pairKey{0u};
  std::int64_t decisionTsNs{0};
  std::int64_t completedTsNs{0};
  std::int64_t decisionEdgeAfterCostBpsE8{0};
  std::int64_t fillEdgeAfterCostBpsE8{0};
  std::int64_t edgeLossBpsE8{0};
  std::int64_t requiredEdgeBpsE8{0};
  std::int64_t fairBpsE8{0};
  std::int64_t costBandBpsE8{0};
  std::int64_t quoteDecisionEdgeAfterCostBpsE8{0};
  std::int64_t decisionToFirstFillNs{0};
  std::int64_t decisionToLastFillNs{0};
  std::int64_t fillLegSkewNs{0};
  std::int64_t thresholdDeadlineNs{-1};
  std::int64_t zeroEdgeDeadlineNs{-1};
  JsonExecutionQualityLeg legs[2]{};
  std::uint32_t pairIndex{0u};
  std::uint32_t pairSlot{0u};
  std::uint8_t phaseRaw{0u};
  std::uint8_t anchorRaw{0u};
  std::uint8_t statusRaw{0u};
  std::uint8_t directionRaw{0u};
  std::uint8_t bandSideRaw{0u};
  std::uint8_t legCount{0u};
};

struct JsonRateLimitUsagePoint {
  std::uint32_t legIndex{0u};
  std::int64_t tsNs{0};
  std::int64_t minRemainingPctE4{0};
  std::int64_t remaining{0};
  std::int64_t limit{0};
  trading_core::RateLimitBucketKind bucketKind{trading_core::RateLimitBucketKind::Unknown};
  std::uint64_t intervalNs{0u};
  bool exhausted{false};
};

struct OpenOrder {
  trading_core::StrategyOrderState state{};
  trading_core::ExecutionRouteHandle routeHandle{trading_core::kDefaultExecutionRouteHandle};
  std::size_t recordIndex{kNoRecordIndex};
  std::int64_t createdTsNs{0};
  std::int64_t activeTsNs{0};
  std::int64_t filledQtyRaw{0};
  std::int64_t queueAheadQtyRaw{0};
  std::int64_t lastAggressiveBookTsNs{0};
  bool active{false};
  bool reduceOnly{false};
  bool postOnly{false};
  bool queueInitialized{false};
  bool stopTriggered{false};
  bool service{false};
  bool warmup{false};
  bool serviceAutounwind{false};
  bool warmupCancelAfterAck{false};
  bool warmupCancelRequested{false};
  std::int64_t triggerSourcePriceRaw{0};
  std::int64_t triggerTsNs{0};
  std::int64_t warmupCancelTimeoutTsNs{0};
};

struct PendingAction {
  PendingActionKind kind{PendingActionKind::OrderAck};
  std::size_t slotIndex{kNoRecordIndex};
  std::size_t recordIndex{kNoRecordIndex};
  std::int64_t activeTsNs{0};
  std::uint64_t submitSequence{0u};
  Id targetClientIntentId{};
  Id commandClientIntentId{};
};

struct ReconstructionSourceIdentity {
  std::uint64_t producerEpoch{0u};
  std::uint64_t sourceGeneration{0u};
  std::uint64_t sessionEpoch{0u};
  std::uint32_t sourceId{0u};
  bool initialized{false};
};

struct EngineState {
  std::vector<JsonOrder> orders{};
  std::vector<JsonFill> fills{};
  std::vector<JsonEquityPoint> equityPoints{};
  std::vector<JsonRiskSnapshot> riskSnapshots{};
  std::vector<JsonStrategyIndicatorPoint> strategyIndicator{};
  std::vector<JsonStrategyRangePoint> strategyRange{};
  std::vector<JsonStrategySpreadPoint> strategySpread{};
  JsonStrategySpreadDiagnostics strategySpreadDiagnostics{};
  std::vector<JsonExecutionQualityAction> executionQuality{};
  std::vector<JsonRateLimitUsagePoint> rateLimitUsage{};
  std::string strategyIndicatorProfile{};
  std::string strategyIndicatorTitle{};
  std::string strategyIndicatorValueLabel{};
  std::string strategyIndicatorAuxLabel{};
  std::string strategyIndicatorUnit{};
  std::vector<PendingAction> pending{};
  std::uint64_t nextPendingSubmitSequence{1u};
  std::array<OpenOrder, kMaxOrderStates> open{};
  trading_core::StrategyPositionState position{};
  trading_core::StrategyMarketSnapshot snapshot{};
  trading_core::StrategyCandleSnapshot candle{};
  trading_core::StrategyMarketSnapshot venueSnapshot{};
  trading_core::LocalOrderBookState orderBook{};
  trading_core::LocalOrderBookState venueOrderBook{};
  ReplayDepthBook strategyDepthBook{};
  ReplayDepthBook venueDepthBook{};
  bool depthReplayFailed{false};
  trading_core::LocalOrderBookState topExecutionOverlayScratch{};
  std::vector<AggressiveFillPlan> topExecutionFillPlanScratch{};
  ExecutionDepthProvider* executionDepthProvider{nullptr};
  bool venueOrderBookExecutionEnabled{false};
  BacktestDepthExecutionMode depthExecutionMode{BacktestDepthExecutionMode::BboNoDepth};
  std::uint64_t depthFallbackFills{0};
  std::int64_t firstDepthFallbackTsNs{0};
  std::string depthFallbackReason{};
  trading_core::BboReconstructionState bboReconstruction{};
  trading_core::BboReconstructionState venueBboReconstruction{};
  ReconstructionSourceIdentity reconstructionBookTickerIdentity{};
  ReconstructionSourceIdentity reconstructionTradeIdentity{};
  ReconstructionSourceIdentity venueReconstructionBookTickerIdentity{};
  ReconstructionSourceIdentity venueReconstructionTradeIdentity{};
  std::uint64_t contextRevision{1u};
  std::array<std::int64_t,
             cxet::api::market::kPublicMarketDataStreamCount>
      strategySourceDeliveryTsNs{};
  // Difference between the captured realtime clock and the replay monotonic
  // coordinate at the most recently delivered market-data boundary.
  std::int64_t replayUtcOffsetNs{0};
  std::int64_t lastReplayMonotonicNs{0};
  std::int64_t firstReplayDeliveryNs{0};
  std::int64_t lastReplayDeliveryNs{0};
  std::int64_t firstVenueScheduleNs{0};
  std::int64_t lastVenueScheduleNs{0};
  std::uint64_t venueScheduleAdjustedEvents{0u};
  BacktestInputCoverage inputCoverage{};
  std::uint32_t legIndex{0u};
  ExchangeId exchange{};
  canon::MarketType market{};
  Symbol symbol{};
  std::vector<cxet::composite::UserDataEvent> userEvents{};
  std::vector<std::int64_t> userEventDeliveryTsNs{};
  BacktestLatencyRuntime latency{};
  trading_core::RateLimitState rateLimits{};
  std::uint64_t rateLimitRejectedOrders{0};
  std::uint64_t rateLimitRejectedCancels{0};
  std::int64_t firstRateLimitRejectTsNs{0};
  std::int64_t lastRateLimitRejectTsNs{0};
  std::int64_t rateLimitCooldownUntilTsNs{0};
  std::int64_t rateLimitProblemUntilTsNs{0};
  std::size_t deliveredUserEvents{0};
  std::uint64_t userEventSequence{0};
  std::uint64_t timerWakeEvents{0};
  TimeNs lastOrderPathHotMono{};
  std::uint64_t nextServiceClientIntentRaw{0x8000000000000000ull};
  std::uint8_t lastWarmupSkipReason{0u};
  trading_core::StrategyProblemEvent pendingProblem{};
  bool hasPendingProblem{false};
  bool rateLimitSnapshotRequested{false};
  bool warmupInFlight{false};
  bool serviceExposureOpen{false};
  std::uint64_t reduceOnlyOrders{0};
  std::uint64_t reduceOnlyFills{0};
  std::int64_t tickSizeE8{0};
  std::uint32_t makerFillMinCrossTicks{0};
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
  std::int64_t liquidationTsNs{0};
  std::int64_t liquidationEquityE8{0};
  std::int64_t riskStopTsNs{0};
  std::int64_t riskStopEquityE8{0};
  std::int64_t riskStopThresholdEquityE8{0};
  bool hasRiskSnapshot{false};
  bool liquidated{false};
  bool riskStopped{false};
};

inline void setStrategySourceDeliveryTimestamp(
    EngineState& engine,
    cxet::api::market::PublicMarketDataStream stream,
    std::int64_t timestampNs) noexcept {
  const std::size_t index =
      cxet::api::market::publicMarketDataStreamIndex(stream);
  if (index < engine.strategySourceDeliveryTsNs.size()) {
    engine.strategySourceDeliveryTsNs[index] = timestampNs;
  }
}

[[nodiscard]] inline std::int64_t strategySourceDeliveryTimestamp(
    const EngineState& engine,
    cxet::api::market::PublicMarketDataStream stream) noexcept {
  const std::size_t index =
      cxet::api::market::publicMarketDataStreamIndex(stream);
  return index < engine.strategySourceDeliveryTsNs.size()
      ? engine.strategySourceDeliveryTsNs[index]
      : 0;
}

inline void bumpContextRevision(EngineState& engine) noexcept {
  ++engine.contextRevision;
  if (engine.contextRevision == 0u) engine.contextRevision = 1u;
}

inline const cxet::composite::BookTicker& effectiveBookTicker(
    const trading_core::StrategyMarketSnapshot& snapshot) noexcept {
  return snapshot.hasReconstructedBbo.raw == trading_core::kAlgoFlagTrue.raw
             ? snapshot.reconstructedBookTicker
             : snapshot.lastBookTicker;
}

struct TimelineEvent {
  // Replay-monotonic venue scheduling coordinate. It is derived from the
  // captured monotonic receive clock and the exchange timestamp, then clamped
  // to delivery when venue-clock skew would put the event after its arrival.
  std::int64_t tsNs{0};
  std::int64_t exchangeTsNs{0};
  // Replay-monotonic application-frame delivery coordinate. This preserves
  // captured CLOCK_MONOTONIC deltas while being anchored near UTC for artifacts.
  std::int64_t deliveryTsNs{0};
  std::int64_t deliveryRealtimeNs{0};
  std::uint64_t receiveMonotonicNs{0u};
  std::uint64_t producerEpoch{0u};
  std::uint64_t sourceGeneration{0u};
  std::uint64_t sessionEpoch{0u};
  std::uint64_t frameSequence{0u};
  std::uint64_t shardSequence{0u};
  std::uint32_t sourceId{0u};
  std::uint16_t shardId{0u};
  std::uint16_t eventOrdinal{0u};
  std::int64_t ingestSeq{0};
  std::uint8_t kind{0};
  std::uint8_t legIndex{0};
  bool venueScheduleAdjusted{false};
  bool applicationAtomicRebase{false};
  // Captured liveness evidence without a new venue state. It is delivered on
  // the application plane only and does not directly run a strategy cycle.
  bool applicationOnly{false};
  std::size_t index{0};
};

std::string quote(std::string_view value);
ExchangeId exchangeFromText(const std::string& value) noexcept;
canon::MarketType marketFromText(const std::string& value) noexcept;
Side sideFromRow(std::int64_t raw) noexcept;

cxet::composite::Trade toRuntime(const TradeRow& row) noexcept;
cxet::composite::BookTicker toRuntime(const BookTickerRow& row) noexcept;
bool buildTieredCandleHistory(const std::vector<CandleRow>& rows,
                              cxet::composite::TieredCandleHistory& out) noexcept;

void initializeAccounting(EngineState& engine, const BacktestRunRequest& request) noexcept;
[[nodiscard]] trading_core::FillPositionAccountingResult updatePosition(
    EngineState& engine,
    Side side,
    std::int64_t priceE8,
    std::int64_t qtyE8,
    std::int64_t tsNs,
    std::int64_t feeBpsE8) noexcept;
std::int64_t unrealizedPnlE8(const EngineState& engine) noexcept;
std::int64_t equityE8(const EngineState& engine) noexcept;
void appendEquityPoint(EngineState& engine, std::int64_t tsNs);
void appendRiskSnapshot(EngineState& engine, std::int64_t tsNs, std::uint8_t reason);
bool appendRiskSnapshotIfDue(EngineState& engine, std::int64_t tsNs);
void hardLiquidate(EngineState& engine, std::int64_t tsNs, std::int64_t equityE8) noexcept;
void stopForTraderRisk(EngineState& engine,
                       std::int64_t tsNs,
                       std::int64_t equityE8,
                       std::int64_t thresholdEquityE8) noexcept;

std::size_t visibleOrders(const EngineState& engine,
                          trading_core::StrategyOrderState* out,
                          std::size_t capacity) noexcept;
std::int64_t delayedTs(std::int64_t tsNs, std::uint64_t latencyUs) noexcept;
bool fillOrder(EngineState& engine, OpenOrder& order, std::int64_t priceE8, std::int64_t tsNs, bool taker);
bool fillOrder(EngineState& engine,
               OpenOrder& order,
               std::int64_t priceE8,
               std::int64_t tsNs,
               bool taker,
               const FillExecutionMetadata& metadata);
bool fillOrderQty(EngineState& engine,
                  OpenOrder& order,
                  std::int64_t priceE8,
                  std::int64_t requestedQty,
                  std::int64_t tsNs,
                  bool taker,
                  const FillExecutionMetadata& metadata);
void tryFillOnTop(EngineState& engine,
                  std::int64_t venueTsNs,
                  bool venueBookTickerUpdate);
void tryFillOnTrade(EngineState& engine,
                    const cxet::composite::Trade& trade,
                    std::int64_t venueTsNs);
void applyDueActions(EngineState& engine, std::int64_t tsNs);
void applyDueActionsBefore(EngineState& engine, std::int64_t tsNs);
bool forceClosePosition(EngineState& engine, std::int64_t tsNs, std::uint64_t orderLatencyUs);
bool forceClosePosition(EngineState& engine, std::int64_t tsNs, BacktestLatencyRuntime& latency);
void cancelOpenOrders(EngineState& engine, std::int64_t tsNs, std::uint64_t cancelLatencyUs);
void cancelOpenOrders(EngineState& engine, std::int64_t tsNs, BacktestLatencyRuntime& latency);
void applyIntents(EngineState& engine,
                  const trading_core::StrategyIo& io,
                  std::int64_t tsNs,
                  std::uint64_t orderLatencyUs,
                  std::uint64_t cancelLatencyUs);
void applyIntents(EngineState& engine,
                  const trading_core::StrategyIo& io,
                  std::int64_t tsNs,
                  BacktestLatencyRuntime& latency);
void appendRateLimitUsageSnapshot(EngineState& engine, std::int64_t tsNs);

void buildTimeline(const SessionData& session, std::vector<TimelineEvent>& out);
void buildTimeline(const std::vector<SessionData>& sessions, std::vector<TimelineEvent>& out);
Status writeResult(BacktestRunResult& result,
                   const EngineState& engine,
                   const std::vector<EngineState>* legEngines = nullptr) noexcept;

}  // namespace hft_backtest::detail
