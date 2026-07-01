#include "strategy/backtest_strategy_trace.hpp"

#include "hft_backtest/strategy_metadata.hpp"

#include <string>

namespace hft_backtest {
namespace {

std::size_t boundedReserve(std::size_t events, std::size_t divisor, std::size_t base) noexcept {
  return events == 0u ? base : events / divisor + base;
}

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

void reserveEngineStorage(detail::EngineState& engine,
                          std::size_t events,
                          bool captureIndicatorTrace,
                          bool captureRangeTrace,
                          bool captureSpreadTrace) {
  const std::size_t tradeRows = boundedReserve(events, 16u, 64u);
  engine.pending.reserve(detail::kIntentCapacity);
  engine.orders.reserve(tradeRows);
  engine.fills.reserve(tradeRows);
  engine.rateLimitUsage.reserve(tradeRows);
  engine.userEvents.reserve(tradeRows);
  engine.userEventDeliveryTsNs.reserve(tradeRows);
  engine.equityPoints.reserve(boundedReserve(events, 64u, 16u));
  engine.riskSnapshots.reserve(boundedReserve(events, 1024u, 8u));
  if (captureIndicatorTrace) engine.strategyIndicator.reserve(boundedReserve(events, 64u, 64u));
  if (captureRangeTrace) engine.strategyRange.reserve(events + 8u);
  if (captureSpreadTrace) engine.strategySpread.reserve(events + 8u);
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
  }
  row.eventCode = indicatorEventCode(trace);
  row.reasonRaw = trace.reasonRaw;
  row.decisionKind = trace.decisionKind;
  row.sideRaw = trace.sideRaw;
  if ((engine.strategyIndicatorProfile == "trend_score" || engine.strategyIndicatorProfile == "toxic_flow" ||
       engine.strategyIndicatorProfile == "toxic_flow_pair" || engine.strategyIndicatorProfile == "volatility_bps") &&
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
  engine.strategySpread.push_back(row);
}

void recordInputCoverage(detail::EngineState& engine) noexcept {
  ++engine.inputCoverage.strategyCycles;
  if (engine.snapshot.hasTrade.raw == hft_trader::core::kAlgoFlagTrue.raw) ++engine.inputCoverage.cyclesWithTrade;
  if (engine.snapshot.hasBookTicker.raw == hft_trader::core::kAlgoFlagTrue.raw) ++engine.inputCoverage.cyclesWithBookTicker;
  if (engine.snapshot.hasPriceLimit.raw == hft_trader::core::kAlgoFlagTrue.raw) ++engine.inputCoverage.cyclesWithPriceLimit;
  if (engine.snapshot.hasDepth.raw == hft_trader::core::kAlgoFlagTrue.raw) ++engine.inputCoverage.cyclesWithDepth;
  if (engine.snapshot.hasOrderBook.raw == hft_trader::core::kAlgoFlagTrue.raw && engine.snapshot.orderBook) ++engine.inputCoverage.cyclesWithOrderBook;
}

}  // namespace hft_backtest
