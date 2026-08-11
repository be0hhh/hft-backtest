#include "strategy/backtest_strategy_trace.hpp"

#include "hft_backtest/strategy_metadata.hpp"
#include "hft_trader/api/StrategyApi.hpp"
#include "hft_trader/core/math/PriceBasis.hpp"

#include <algorithm>
#include <limits>
#include <string>

namespace hft_backtest {
namespace {

std::size_t boundedReserve(std::size_t events, std::size_t divisor, std::size_t base) noexcept {
  return events == 0u ? base : events / divisor + base;
}

constexpr std::size_t kInitialOrderReserve = 4096u;
constexpr std::size_t kInitialEventRecordReserve = 1024u;
constexpr std::size_t kInitialRiskReserve = 64u;
constexpr std::size_t kInitialRiskReserveCap = 65536u;
constexpr std::size_t kInitialTraceReserveCap = 262144u;

bool hasWallDrop(const hft_trader::core::StrategyDecisionTrace& trace) noexcept {
  for (std::size_t side = 0; side < hft_trader::core::kStrategyDecisionWallSideCount; ++side) {
    for (std::size_t rank = 0; rank < hft_trader::core::kStrategyDecisionWallRankCount; ++rank) {
      if (trace.wallDropped[side][rank] != 0u) return true;
    }
  }
  return false;
}

std::uint8_t indicatorEventCode(const hft_trader::core::StrategyDecisionTrace& trace) noexcept {
  if (hasWallDrop(trace)) return 2u;
  if (trace.decisionKind == static_cast<std::uint8_t>(hft_trader::core::StrategyDecisionKind::Skip) && trace.reasonRaw != 0u) return 4u;
  if (trace.decisionKind == static_cast<std::uint8_t>(hft_trader::core::StrategyDecisionKind::Enter) ||
      trace.decisionKind == static_cast<std::uint8_t>(hft_trader::core::StrategyDecisionKind::Close) ||
      trace.bidTriggered != 0u || trace.askTriggered != 0u) return 1u;
  return 0u;
}

std::int64_t strategyIndicatorSecond(std::int64_t tsNs) noexcept {
  constexpr std::int64_t nsPerSecond = 1000000000ll;
  return tsNs >= 0 ? tsNs / nsPerSecond : (tsNs - (nsPerSecond - 1ll)) / nsPerSecond;
}

std::int64_t absI64(std::int64_t value) noexcept {
  if (value >= 0) return value;
  if (value == std::numeric_limits<std::int64_t>::min()) return std::numeric_limits<std::int64_t>::max();
  return -value;
}

detail::JsonStrategySpreadPoint makeStrategySpreadPoint(std::int64_t tsNs,
                                                        const hft_trader::core::StrategyDecisionTrace& trace) noexcept {
  detail::JsonStrategySpreadPoint row{};
  row.tsNs = tsNs;
  row.spreadBpsE8 = trace.arbSpreadBpsE8;
  row.emaBpsE8 = trace.arbEmaBpsE8;
  row.deviationBpsE8 = trace.arbDeviationBpsE8;
  row.costBandBpsE8 = trace.arbCostBandBpsE8;
  row.edgeAfterCostBpsE8 = trace.arbEdgeAfterCostBpsE8;
  row.direction = trace.arbDirection;
  row.decisionKind = trace.decisionKind;
  row.reasonRaw = trace.reasonRaw;
  return row;
}

void recordStrategySpreadDiagnostics(detail::JsonStrategySpreadDiagnostics& out,
                                     const hft_trader::core::StrategyDecisionTrace& trace) noexcept {
  if (out.rows == 0u) {
    out.minEdgeAfterCostBpsE8 = trace.arbEdgeAfterCostBpsE8;
    out.maxEdgeAfterCostBpsE8 = trace.arbEdgeAfterCostBpsE8;
  } else {
    out.minEdgeAfterCostBpsE8 = std::min(out.minEdgeAfterCostBpsE8, trace.arbEdgeAfterCostBpsE8);
    out.maxEdgeAfterCostBpsE8 = std::max(out.maxEdgeAfterCostBpsE8, trace.arbEdgeAfterCostBpsE8);
  }
  out.maxAbsDeviationBpsE8 = std::max(out.maxAbsDeviationBpsE8, absI64(trace.arbDeviationBpsE8));
  if (trace.decisionKind < hft_trader::core::kStrategyDecisionKindCount &&
      trace.reasonRaw < hft_trader::core::kStrategyDecisionReasonCapacity) {
    ++out.decisionCounts[trace.decisionKind][trace.reasonRaw];
  }
  ++out.rows;
}

}  // namespace

bool resolveStrategyIndicatorProfile(std::string_view id, StrategyIndicatorProfileInfo& out) noexcept {
  out = StrategyIndicatorProfileInfo{};
  if (id.empty() || id == "none") return true;
  if (id == "trend_score") {
    out = StrategyIndicatorProfileInfo{"trend_score", "Trend score", "Score", "Raw", "score"};
    return true;
  }
  if (id == "toxic_flow") {
    out = StrategyIndicatorProfileInfo{"toxic_flow", "Toxic flow", "Score", "Raw", "score"};
    return true;
  }
  if (id == "toxic_flow_pair") {
    out = StrategyIndicatorProfileInfo{"toxic_flow_pair", "Toxic flow A/B", "A", "B", "score"};
    return true;
  }
  if (id == "volatility_bps") {
    out = StrategyIndicatorProfileInfo{"volatility_bps", "Volatility", "EMA", "Sample", "bps"};
    return true;
  }
  if (id == "recursive_stat_arb") {
    out = StrategyIndicatorProfileInfo{"recursive_stat_arb",
                                       "Recursive stat-arb",
                                       "Residual",
                                       "Entry threshold",
                                       "bps_e8"};
    return true;
  }
  return false;
}

bool strategyDeclaresIndicator(std::string_view strategy, std::string_view profile) noexcept {
  if (profile.empty()) return true;
  const hft_backtest::StrategyMetadata* metadata = hft_backtest::findStrategyMetadata(strategy);
  if (metadata == nullptr) return false;
  for (std::size_t i = 0; i < metadata->indicatorCount && i < hft_backtest::kStrategyMetadataMaxIndicators; ++i) {
    const hft_backtest::StrategyIndicatorMetadata& indicator = metadata->indicators[i];
    if (indicator.id != nullptr && profile == std::string_view{indicator.id}) return true;
  }
  return false;
}

bool strategyIndicatorEnabled(const detail::EngineState& engine) noexcept {
  return !engine.strategyIndicatorProfile.empty();
}

void configureStrategyIndicator(detail::EngineState& engine, const StrategyIndicatorProfileInfo& profile) {
  engine.strategyIndicatorProfile = std::string{profile.id};
  engine.strategyIndicatorTitle = std::string{profile.title};
  engine.strategyIndicatorValueLabel = std::string{profile.valueLabel};
  engine.strategyIndicatorAuxLabel = std::string{profile.auxLabel};
  engine.strategyIndicatorUnit = std::string{profile.unit};
}

bool emitsArbSpreadTrace(const hft_trader::runtime::StrategyDescriptor& descriptor) noexcept {
  return descriptor.emitsArbSpreadTrace;
}

bool emitsPriceRangeTrace(const hft_trader::runtime::StrategyDescriptor& descriptor) noexcept {
  return descriptor.emitsPriceRangeTrace;
}

void reserveEngineStorage(detail::EngineState& engine,
                          std::size_t events,
                          std::uint64_t replayDurationNs,
                          bool captureIndicatorTrace,
                          bool captureRangeTrace,
                          bool captureSpreadRows) {
  engine.pending.reserve(detail::kIntentCapacity);
  engine.orders.reserve(kInitialOrderReserve);
  engine.fills.reserve(kInitialEventRecordReserve);
  engine.rateLimitUsage.reserve(kInitialEventRecordReserve);
  engine.userEvents.reserve(kInitialEventRecordReserve);
  engine.userEventDeliveryTsNs.reserve(kInitialEventRecordReserve);
  engine.equityPoints.reserve(kInitialEventRecordReserve);
  const std::uint64_t intervalRows = replayDurationNs /
      static_cast<std::uint64_t>(detail::kRiskSnapshotIntervalNs) + 8u;
  const std::size_t riskReserve = static_cast<std::size_t>(
      std::min<std::uint64_t>(intervalRows, static_cast<std::uint64_t>(kInitialRiskReserveCap)));
  engine.riskSnapshots.reserve(std::max(kInitialRiskReserve, riskReserve));
  const std::size_t traceReserve = std::min(events + 8u, kInitialTraceReserveCap);
  if (captureIndicatorTrace) engine.strategyIndicator.reserve(std::min(boundedReserve(events, 64u, 64u), kInitialTraceReserveCap));
  if (captureRangeTrace) engine.strategyRange.reserve(traceReserve);
  if (captureSpreadRows) engine.strategySpread.reserve(traceReserve);
  if (captureSpreadRows) engine.executionQuality.reserve(traceReserve);
}

void appendStrategyIndicator(detail::EngineState& engine,
                             std::int64_t tsNs,
                             const hft_trader::core::StrategyDecisionTrace& trace) {
  if (!strategyIndicatorEnabled(engine)) return;
  detail::JsonStrategyIndicatorPoint row{};
  row.tsNs = tsNs;
  if (engine.strategyIndicatorProfile == "trend_score") {
    row.valueRaw = static_cast<std::int64_t>(trace.trendScoreRaw);
    row.auxRaw = static_cast<std::int64_t>(trace.trendRawScore);
    row.featureRaw[0] = static_cast<std::int64_t>(trace.trendFastScore);
    row.featureRaw[1] = static_cast<std::int64_t>(trace.trendMediumScore);
    row.featureRaw[2] = static_cast<std::int64_t>(trace.trendSlowScore);
    row.featureRaw[3] = static_cast<std::int64_t>(trace.trendAccelerationScore);
    row.featureRaw[4] = static_cast<std::int64_t>(trace.trendConfidencePct);
    row.featureRaw[5] = static_cast<std::int64_t>(trace.trendNormalMoveBps);
  } else if (engine.strategyIndicatorProfile == "toxic_flow") {
    row.valueRaw = static_cast<std::int64_t>(trace.toxicFlowScoreRaw);
    row.auxRaw = static_cast<std::int64_t>(trace.toxicFlowRawScore);
    row.featureRaw[0] = static_cast<std::int64_t>(trace.toxicFlowPressureScore);
    row.featureRaw[1] = static_cast<std::int64_t>(trace.toxicFlowTouchScore);
    row.featureRaw[2] = static_cast<std::int64_t>(trace.toxicFlowFollowScore);
    row.featureRaw[3] = static_cast<std::int64_t>(trace.toxicFlowImbalanceScore);
    row.featureRaw[4] = static_cast<std::int64_t>(trace.toxicFlowDepthDropScore);
    row.featureRaw[5] = static_cast<std::int64_t>(trace.toxicFlowBidDepthDropScore);
    row.featureRaw[6] = static_cast<std::int64_t>(trace.toxicFlowAskDepthDropScore);
    row.featureRaw[7] = static_cast<std::int64_t>(trace.toxicFlowBuyPressureScore);
    row.featureRaw[8] = static_cast<std::int64_t>(trace.toxicFlowSellPressureScore);
    row.featureRaw[9] = static_cast<std::int64_t>(trace.toxicFlowConfidencePct);
  } else if (engine.strategyIndicatorProfile == "toxic_flow_pair") {
    row.valueRaw = static_cast<std::int64_t>(trace.toxicFlowScoreRaw);
    row.auxRaw = static_cast<std::int64_t>(trace.toxicFlowRawScore);
  } else if (engine.strategyIndicatorProfile == "volatility_bps") {
    row.valueRaw = static_cast<std::int64_t>(trace.volatilityBps);
    row.auxRaw = static_cast<std::int64_t>(trace.volatilitySampleBps);
    row.featureRaw[0] = static_cast<std::int64_t>(trace.volatilityFastBps);
    row.featureRaw[1] = static_cast<std::int64_t>(trace.volatilityBaselineBps);
    row.featureRaw[2] = static_cast<std::int64_t>(trace.volatilityShockBps);
    row.featureRaw[3] = static_cast<std::int64_t>(trace.volatilityQuoteMoveBps);
    row.featureRaw[4] = static_cast<std::int64_t>(trace.volatilitySpreadBps);
    row.featureRaw[5] = static_cast<std::int64_t>(trace.volatilityConfidencePct);
    row.featureRaw[6] = static_cast<std::int64_t>(trace.volatilityQuoteConfidencePct);
  } else if (engine.strategyIndicatorProfile == "recursive_stat_arb") {
    row.valueRaw = hft_trader::core::saturatingSubI64(trace.recursiveResidualBpsE8,
                                                     trace.recursiveMeanBpsE8);
    row.auxRaw = trace.recursiveEntryThresholdBpsE8;
    row.featureRaw[0] = trace.recursiveRlsBetaQ32;
    row.featureRaw[1] = trace.recursiveRlsInterceptQ32;
    row.featureRaw[2] = trace.recursiveMeanBpsE8;
    row.featureRaw[3] = trace.recursiveSigmaBpsE8;
    row.featureRaw[4] = trace.recursiveCusumUpBpsE8;
    row.featureRaw[5] = trace.recursiveCusumDownBpsE8;
    row.featureRaw[6] = static_cast<std::int64_t>(trace.recursiveFlowLeg0Bps);
    row.featureRaw[7] = static_cast<std::int64_t>(trace.recursiveFlowLeg1Bps);
    row.featureRaw[8] = static_cast<std::int64_t>(trace.recursivePairFlowBps);
    row.featureRaw[9] = static_cast<std::int64_t>(trace.recursiveModelSamples);
  }
  row.eventCode = indicatorEventCode(trace);
  row.reasonRaw = trace.reasonRaw;
  row.decisionKind = trace.decisionKind;
  row.sideRaw = trace.sideRaw;
  if ((engine.strategyIndicatorProfile == "trend_score" || engine.strategyIndicatorProfile == "toxic_flow" ||
       engine.strategyIndicatorProfile == "toxic_flow_pair" || engine.strategyIndicatorProfile == "volatility_bps" ||
       engine.strategyIndicatorProfile == "recursive_stat_arb") &&
      !engine.strategyIndicator.empty() &&
      strategyIndicatorSecond(engine.strategyIndicator.back().tsNs) == strategyIndicatorSecond(row.tsNs)) {
    engine.strategyIndicator.back() = row;
    return;
  }
  engine.strategyIndicator.push_back(row);
}

void appendStrategyRange(detail::EngineState& engine,
                         std::int64_t tsNs,
                         const hft_trader::core::StrategyDecisionTrace& trace) {
  if (trace.rangeReady == 0u || trace.rangeLowRaw <= 0 || trace.rangeHighRaw <= trace.rangeLowRaw) return;
  detail::JsonStrategyRangePoint row{};
  row.tsNs = tsNs;
  row.lowRaw = trace.rangeLowRaw;
  row.midRaw = trace.rangeMidRaw;
  row.highRaw = trace.rangeHighRaw;
  engine.strategyRange.push_back(row);
}

void appendStrategySpread(detail::EngineState& engine,
                          std::int64_t tsNs,
                          const hft_trader::core::StrategyDecisionTrace& trace) {
  if (trace.arbReady == 0u) return;
  engine.strategySpread.push_back(makeStrategySpreadPoint(tsNs, trace));
}

void recordStrategySpread(detail::EngineState& engine,
                          std::int64_t tsNs,
                          const hft_trader::core::StrategyDecisionTrace& trace,
                          bool captureRows) {
  if (trace.arbReady == 0u) return;
  recordStrategySpreadDiagnostics(engine.strategySpreadDiagnostics, trace);
  if (captureRows) engine.strategySpread.push_back(makeStrategySpreadPoint(tsNs, trace));
}

void recordInputCoverage(detail::EngineState& engine) noexcept {
  ++engine.inputCoverage.strategyCycles;
  if (engine.snapshot.hasTrade.raw == hft_trader::core::kAlgoFlagTrue.raw) ++engine.inputCoverage.cyclesWithTrade;
  if (engine.snapshot.hasBookTicker.raw == hft_trader::core::kAlgoFlagTrue.raw) ++engine.inputCoverage.cyclesWithBookTicker;
  if (engine.snapshot.hasPriceLimit.raw == hft_trader::core::kAlgoFlagTrue.raw) ++engine.inputCoverage.cyclesWithPriceLimit;
  if (engine.snapshot.hasDepth.raw == hft_trader::core::kAlgoFlagTrue.raw) ++engine.inputCoverage.cyclesWithDepth;
  if (engine.snapshot.hasOrderBook.raw == hft_trader::core::kAlgoFlagTrue.raw && engine.snapshot.orderBook) ++engine.inputCoverage.cyclesWithOrderBook;
}

void recordExecutionQualityActions(
    detail::EngineState& engine,
    std::int64_t tsNs,
    const hft_trader::core::StrategyExecutionTraceBatch& batch) {
  for (std::size_t i = 0u; i < batch.count; ++i) {
    const hft_trader::core::StrategyExecutionActionTrace& source = batch.actions[i];
    if (!source.valid || source.actionId.raw == 0u) continue;
    detail::JsonExecutionQualityAction row{};
    row.actionId = source.actionId.raw;
    row.positionCycleId = source.positionCycleId.raw;
    row.transactionId = source.transactionId.raw;
    row.pairKey = source.pairKey;
    row.decisionTsNs = tsNs;
    row.decisionEdgeAfterCostBpsE8 = source.decisionEdgeAfterCostBpsE8;
    row.requiredEdgeBpsE8 = source.requiredEdgeBpsE8;
    row.fairBpsE8 = source.fairBpsE8;
    row.costBandBpsE8 = source.costBandBpsE8;
    row.quoteDecisionEdgeAfterCostBpsE8 = source.quoteDecisionEdgeAfterCostBpsE8;
    row.pairIndex = source.pairIndex;
    row.pairSlot = source.pairSlot;
    row.phaseRaw = static_cast<std::uint8_t>(source.phase);
    row.anchorRaw = static_cast<std::uint8_t>(source.anchor);
    row.statusRaw = static_cast<std::uint8_t>(source.status);
    row.directionRaw = source.directionRaw;
    row.bandSideRaw = source.bandSideRaw;
    row.legCount = source.legCount < 2u ? source.legCount : 2u;
    for (std::uint8_t leg = 0u; leg < row.legCount; ++leg) {
      row.legs[leg].clientIntentId = source.legs[leg].clientIntentId.raw;
      row.legs[leg].legIndex = source.legs[leg].legIndex;
      row.legs[leg].sideRaw = source.legs[leg].side.raw;
      row.legs[leg].expectedQtyE8 = source.legs[leg].expectedQty.raw;
      row.legs[leg].decisionPriceE8 = source.legs[leg].decisionPrice.raw;
    }
    if (source.anchor == hft_trader::core::StrategyExecutionAnchor::MakerCommitment && row.legCount > 0u) {
      for (auto it = engine.executionQuality.rbegin(); it != engine.executionQuality.rend(); ++it) {
        if (it->anchorRaw != static_cast<std::uint8_t>(hft_trader::core::StrategyExecutionAnchor::MakerQuote) ||
            it->legCount == 0u ||
            it->legs[0].clientIntentId != row.legs[0].clientIntentId) continue;
        row.quoteDecisionEdgeAfterCostBpsE8 = it->decisionEdgeAfterCostBpsE8;
        break;
      }
    }
    if (source.anchor == hft_trader::core::StrategyExecutionAnchor::MakerCommitment &&
        row.legCount > 0u && row.legs[0].expectedQtyE8 > 0 && row.legs[0].decisionPriceE8 > 0) {
      row.legs[0].filledQtyE8 = row.legs[0].expectedQtyE8;
      row.legs[0].fillVwapE8 = row.legs[0].decisionPriceE8;
      row.legs[0].firstFillTsNs = tsNs;
      row.legs[0].lastFillTsNs = tsNs;
    }
    engine.executionQuality.push_back(row);
  }
}

namespace {

void updateExecutionQualityLeg(detail::JsonExecutionQualityLeg& leg,
                               std::int64_t fillQty,
                               std::int64_t fillPrice,
                               std::int64_t tsNs) noexcept {
  if (fillQty <= 0 || fillPrice <= 0) return;
  const std::int64_t priorQty = leg.filledQtyE8;
  const std::int64_t totalQty = priorQty > std::numeric_limits<std::int64_t>::max() - fillQty
      ? std::numeric_limits<std::int64_t>::max()
      : priorQty + fillQty;
  if (totalQty <= 0) return;
  const __int128 weighted = static_cast<__int128>(leg.fillVwapE8) * priorQty +
                            static_cast<__int128>(fillPrice) * fillQty;
  const __int128 average = weighted / totalQty;
  leg.fillVwapE8 = average > std::numeric_limits<std::int64_t>::max()
      ? std::numeric_limits<std::int64_t>::max()
      : static_cast<std::int64_t>(average);
  leg.filledQtyE8 = totalQty;
  if (leg.firstFillTsNs == 0 || tsNs < leg.firstFillTsNs) leg.firstFillTsNs = tsNs;
  if (tsNs > leg.lastFillTsNs) leg.lastFillTsNs = tsNs;
}

void finishExecutionQualityAction(detail::JsonExecutionQualityAction& row) noexcept {
  if (row.anchorRaw == static_cast<std::uint8_t>(hft_trader::core::StrategyExecutionAnchor::MakerQuote)) return;
  if (row.legCount != 2u) return;
  for (std::uint8_t i = 0u; i < 2u; ++i) {
    if (row.legs[i].expectedQtyE8 <= 0 || row.legs[i].filledQtyE8 < row.legs[i].expectedQtyE8 ||
        row.legs[i].fillVwapE8 <= 0) return;
  }
  const detail::JsonExecutionQualityLeg* buy = row.legs[0].sideRaw == Side::Buy().raw
      ? &row.legs[0]
      : (row.legs[1].sideRaw == Side::Buy().raw ? &row.legs[1] : nullptr);
  const detail::JsonExecutionQualityLeg* sell = row.legs[0].sideRaw == Side::Sell().raw
      ? &row.legs[0]
      : (row.legs[1].sideRaw == Side::Sell().raw ? &row.legs[1] : nullptr);
  if (!buy || !sell || buy->fillVwapE8 <= 0 || sell->fillVwapE8 <= 0) return;
  const std::int64_t spread = hft_trader::core::bpsE8(
      hft_trader::core::saturatingSubI64(sell->fillVwapE8, buy->fillVwapE8),
      buy->fillVwapE8);
  const std::int64_t deviation = hft_trader::core::saturatingSubI64(spread, row.fairBpsE8);
  row.fillEdgeAfterCostBpsE8 = hft_trader::core::saturatingSubI64(deviation, row.costBandBpsE8);
  row.edgeLossBpsE8 = hft_trader::core::saturatingSubI64(
      row.decisionEdgeAfterCostBpsE8, row.fillEdgeAfterCostBpsE8);
  const std::int64_t firstFill = std::min(row.legs[0].firstFillTsNs, row.legs[1].firstFillTsNs);
  const std::int64_t lastFill = std::max(row.legs[0].lastFillTsNs, row.legs[1].lastFillTsNs);
  row.completedTsNs = lastFill;
  row.decisionToFirstFillNs = firstFill >= row.decisionTsNs ? firstFill - row.decisionTsNs : 0;
  row.decisionToLastFillNs = lastFill >= row.decisionTsNs ? lastFill - row.decisionTsNs : 0;
  row.fillLegSkewNs = row.legs[0].lastFillTsNs >= row.legs[1].lastFillTsNs
      ? row.legs[0].lastFillTsNs - row.legs[1].lastFillTsNs
      : row.legs[1].lastFillTsNs - row.legs[0].lastFillTsNs;
  row.statusRaw = static_cast<std::uint8_t>(hft_trader::core::StrategyExecutionStatus::Completed);
}

}  // namespace

void recordExecutionQualityFill(
    detail::EngineState& ledger,
    const cxet::composite::UserDataEvent& event,
    std::uint32_t legIndex,
    std::int64_t deliveryTsNs,
    const hft_trader::core::StrategyInstrumentSpec& instrument) noexcept {
  Id clientIntentId{};
  if (!hft_trader::strategy::clientIntentId(event, clientIntentId) || clientIntentId.raw == 0u) return;
  if (hft_trader::strategy::isUserOrderRejected(event) || hft_trader::strategy::isUserOrderCanceled(event)) {
    for (auto it = ledger.executionQuality.rbegin(); it != ledger.executionQuality.rend(); ++it) {
      for (std::uint8_t leg = 0u; leg < it->legCount; ++leg) {
        if (it->legs[leg].clientIntentId != clientIntentId.raw) continue;
        it->statusRaw = static_cast<std::uint8_t>(
            it->legs[leg].filledQtyE8 > 0
                ? hft_trader::core::StrategyExecutionStatus::Partial
                : hft_trader::core::StrategyExecutionStatus::Rejected);
        return;
      }
    }
    return;
  }
  if (!hft_trader::strategy::isUserFillEvent(event)) return;
  Amount qty{};
  const bool hasLastFilledQty = cxet::composite::userDataFieldPresent(
      event, cxet::composite::UserDataField::LastFilledQty);
  const bool hasFilledQty = cxet::composite::userDataFieldPresent(
      event, cxet::composite::UserDataField::FilledQty);
  if (hasLastFilledQty && event.lastFilledQty.raw > 0) {
    qty = event.lastFilledQty;
  } else if (hasFilledQty) {
    qty = event.filledQty;
  }
  Price price = hft_trader::strategy::userOrderFillPrice(event);
  price = hft_trader::core::nativeToUnderlyingPrice(price, instrument);
  if (qty.raw <= 0 || price.raw <= 0) return;
  for (auto it = ledger.executionQuality.rbegin(); it != ledger.executionQuality.rend(); ++it) {
    if (it->statusRaw == static_cast<std::uint8_t>(hft_trader::core::StrategyExecutionStatus::Completed)) continue;
    for (std::uint8_t leg = 0u; leg < it->legCount; ++leg) {
      if (it->legs[leg].clientIntentId != clientIntentId.raw || it->legs[leg].legIndex != legIndex) continue;
      updateExecutionQualityLeg(it->legs[leg], qty.raw, price.raw, deliveryTsNs);
      finishExecutionQualityAction(*it);
      return;
    }
  }
}

void finalizeExecutionQuality(detail::EngineState& engine) noexcept {
  for (detail::JsonExecutionQualityAction& row : engine.executionQuality) {
    if (row.anchorRaw == static_cast<std::uint8_t>(hft_trader::core::StrategyExecutionAnchor::MakerQuote)) continue;
    if (row.statusRaw == static_cast<std::uint8_t>(hft_trader::core::StrategyExecutionStatus::Completed) ||
        row.statusRaw == static_cast<std::uint8_t>(hft_trader::core::StrategyExecutionStatus::Rejected)) continue;
    bool anyFill = false;
    for (std::uint8_t leg = 0u; leg < row.legCount; ++leg) anyFill = anyFill || row.legs[leg].filledQtyE8 > 0;
    row.statusRaw = static_cast<std::uint8_t>(
        anyFill ? hft_trader::core::StrategyExecutionStatus::Partial
                : hft_trader::core::StrategyExecutionStatus::Uncertain);
  }
}

void calculateExecutionQualityDeadlines(
    detail::EngineState& engine,
    const detail::SessionData* sessions,
    const hft_trader::core::StrategyInstrumentSpec* instruments,
    std::size_t legCount) noexcept {
  if (!sessions || !instruments || legCount == 0u) return;
  for (detail::JsonExecutionQualityAction& action : engine.executionQuality) {
    if (action.anchorRaw == static_cast<std::uint8_t>(hft_trader::core::StrategyExecutionAnchor::MakerQuote) ||
        action.legCount != 2u || action.decisionTsNs <= 0) continue;
    const detail::JsonExecutionQualityLeg* buy = action.legs[0].sideRaw == Side::Buy().raw
        ? &action.legs[0]
        : (action.legs[1].sideRaw == Side::Buy().raw ? &action.legs[1] : nullptr);
    const detail::JsonExecutionQualityLeg* sell = action.legs[0].sideRaw == Side::Sell().raw
        ? &action.legs[0]
        : (action.legs[1].sideRaw == Side::Sell().raw ? &action.legs[1] : nullptr);
    if (!buy || !sell || buy->legIndex >= legCount || sell->legIndex >= legCount) continue;
    const std::vector<detail::BookTickerRow>& buyRows = sessions[buy->legIndex].bookTickers;
    const std::vector<detail::BookTickerRow>& sellRows = sessions[sell->legIndex].bookTickers;
    if (buyRows.empty() || sellRows.empty()) continue;
    const auto initialIndex = [&](const std::vector<detail::BookTickerRow>& rows) noexcept -> std::size_t {
      const auto it = std::upper_bound(
          rows.begin(), rows.end(), action.decisionTsNs,
          [](std::int64_t ts, const detail::BookTickerRow& row) noexcept { return ts < row.tsNs; });
      if (it == rows.begin()) return rows.size();
      return static_cast<std::size_t>((it - rows.begin()) - 1);
    };
    std::size_t buyIndex = initialIndex(buyRows);
    std::size_t sellIndex = initialIndex(sellRows);
    if (buyIndex >= buyRows.size() || sellIndex >= sellRows.size()) continue;
    std::int64_t buyAsk = buyRows[buyIndex].askPriceE8;
    std::int64_t sellBid = sellRows[sellIndex].bidPriceE8;
    ++buyIndex;
    ++sellIndex;
    const std::int64_t stopTs = action.completedTsNs > action.decisionTsNs
        ? action.completedTsNs
        : std::numeric_limits<std::int64_t>::max();
    while ((buyIndex < buyRows.size() || sellIndex < sellRows.size()) &&
           (action.thresholdDeadlineNs < 0 || action.zeroEdgeDeadlineNs < 0)) {
      const std::int64_t buyTs = buyIndex < buyRows.size()
          ? buyRows[buyIndex].tsNs
          : std::numeric_limits<std::int64_t>::max();
      const std::int64_t sellTs = sellIndex < sellRows.size()
          ? sellRows[sellIndex].tsNs
          : std::numeric_limits<std::int64_t>::max();
      const std::int64_t ts = std::min(buyTs, sellTs);
      if (ts == std::numeric_limits<std::int64_t>::max() || ts > stopTs) break;
      if (buyTs == ts) buyAsk = buyRows[buyIndex++].askPriceE8;
      if (sellTs == ts) sellBid = sellRows[sellIndex++].bidPriceE8;
      Price buyPrice{buyAsk};
      Price sellPrice{sellBid};
      buyPrice = hft_trader::core::nativeToUnderlyingPrice(buyPrice, instruments[buy->legIndex]);
      sellPrice = hft_trader::core::nativeToUnderlyingPrice(sellPrice, instruments[sell->legIndex]);
      if (buyPrice.raw <= 0 || sellPrice.raw <= 0) continue;
      const std::int64_t spread = hft_trader::core::bpsE8(
          hft_trader::core::saturatingSubI64(sellPrice.raw, buyPrice.raw), buyPrice.raw);
      const std::int64_t edge = hft_trader::core::saturatingSubI64(
          hft_trader::core::saturatingSubI64(spread, action.fairBpsE8),
          action.costBandBpsE8);
      const std::int64_t elapsed = ts >= action.decisionTsNs ? ts - action.decisionTsNs : 0;
      if (action.thresholdDeadlineNs < 0 && edge < action.requiredEdgeBpsE8) {
        action.thresholdDeadlineNs = elapsed;
      }
      if (action.zeroEdgeDeadlineNs < 0 && edge <= 0) action.zeroEdgeDeadlineNs = elapsed;
    }
  }
}

}  // namespace hft_backtest
