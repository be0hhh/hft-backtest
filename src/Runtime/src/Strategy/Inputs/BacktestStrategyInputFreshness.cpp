#include "BacktestStrategyInputFreshness.hpp"

#include "trading_core/Strategy/StrategyInputFreshness.hpp"
#include "trading_core/Strategy/StrategyMarketData.hpp"

namespace hft_backtest {
namespace {

[[nodiscard]] TimeNs backtestMonoFromTs(std::int64_t tsNs) noexcept {
  TimeNs out{};
  if (tsNs > 0) out.raw = static_cast<std::uint64_t>(tsNs);
  return out;
}

[[nodiscard]] bool timelinePublicMarketStream(
    std::uint8_t kind,
    cxet::api::market::PublicMarketDataStream& out) noexcept {
  using Stream = cxet::api::market::PublicMarketDataStream;
  switch (kind) {
    case detail::kTimelineKindDepth: out = Stream::Orderbook; return true;
    case detail::kTimelineKindTrade: out = Stream::Trades; return true;
    case detail::kTimelineKindBookTicker: out = Stream::BookTicker; return true;
    default: return false;
  }
}

}  // namespace

BacktestStrategyInputPolicy resolveBacktestStrategyInputPolicy(
    const trading_core::StrategyDescriptor& descriptor,
    const trading_core::StrategyParamStorage& params) noexcept {
  BacktestStrategyInputPolicy out{};
  out.requiredStreamReadyMask =
      trading_core::strategy::descriptorRequiredFreshnessStreamMask(
          descriptor);
  const std::uint32_t maxAgeMs = descriptor.inputFreshnessMaxAgeMs
      ? descriptor.inputFreshnessMaxAgeMs(params)
      : trading_core::strategy::kDefaultStrategyInputFreshnessMaxAgeMs;
  out.maxAgeNs =
      trading_core::strategy::strategyInputFreshnessMaxAgeNs(maxAgeMs);
  return out;
}

bool backtestStrategySnapshotReady(
    const BacktestStrategyInputPolicy& inputPolicy,
    const trading_core::StrategyMarketSnapshot& snapshot) noexcept {
  for (std::uint8_t raw = 1u;
       raw <= cxet::api::market::kPublicMarketDataStreamCount;
       ++raw) {
    const auto stream = static_cast<
        cxet::api::market::PublicMarketDataStream>(raw);
    if ((inputPolicy.requiredStreamReadyMask &
         cxet::api::market::publicMarketDataStreamReadyBit(stream)) == 0u) {
      continue;
    }
    if (!trading_core::strategy::snapshotHasRequiredStream(
            snapshot, stream)) return false;
  }
  return true;
}

std::uint64_t backtestSourceFreshLegMask(
    const BacktestStrategyInputPolicy& inputPolicy,
    const detail::EngineState* engines,
    std::size_t legCount,
    std::int64_t tsNs) noexcept {
  if (!engines || legCount == 0u) return 0u;
  const TimeNs nowMono = backtestMonoFromTs(tsNs);
  std::uint64_t out = 0u;
  for (std::size_t i = 0u; i < legCount && i < 64u; ++i) {
    bool legFresh = true;
    for (std::uint8_t raw = 1u;
         raw <= cxet::api::market::kPublicMarketDataStreamCount;
         ++raw) {
      const auto stream = static_cast<
          cxet::api::market::PublicMarketDataStream>(raw);
      if ((inputPolicy.requiredStreamReadyMask &
           cxet::api::market::publicMarketDataStreamReadyBit(stream)) == 0u) {
        continue;
      }
      const TimeNs recvMono = backtestMonoFromTs(
          detail::strategySourceDeliveryTimestamp(engines[i], stream));
      if (!trading_core::strategy::strategyInputSourceTimestampFresh(
              recvMono, nowMono, inputPolicy.maxAgeNs)) {
        legFresh = false;
        break;
      }
    }
    if (legFresh) out |= (std::uint64_t{1} << i);
  }
  return out;
}

void recordBacktestStrategySourceDelivery(
    detail::EngineState& engine,
    const detail::TimelineEvent& event) noexcept {
  cxet::api::market::PublicMarketDataStream stream{};
  if (!timelinePublicMarketStream(event.kind, stream)) return;
  detail::setStrategySourceDeliveryTimestamp(
      engine, stream,
      event.deliveryTsNs > 0 ? event.deliveryTsNs : event.tsNs);
}

}  // namespace hft_backtest
