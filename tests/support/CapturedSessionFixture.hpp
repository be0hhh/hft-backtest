#pragma once

#include "hftrec/corpus_contract/EventRows.hpp"
#include "hftrec/corpus_contract/SessionManifest.hpp"

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace hft_backtest::test_support {

inline constexpr std::int64_t kReceiveRealtimeBaseNs =
    1'900'000'000'000'000'000LL;
inline constexpr std::uint64_t kReceiveMonotonicBaseNs =
    900'000'000'000'000'000ULL;

struct CapturedChannelCounts {
  std::uint64_t trades{0u};
  std::uint64_t bookTickers{0u};
  std::uint64_t priceLimits{0u};
  std::uint64_t depth{0u};
  std::uint64_t candles{0u};
};

inline hftrec::replay::EventArrival applicationArrival(
    std::uint64_t sequence,
    std::int64_t exchangeTsNs) noexcept {
  hftrec::replay::EventArrival out{};
  out.receiveRealtimeNs = kReceiveRealtimeBaseNs +
      static_cast<std::int64_t>(sequence * 1'000u);
  out.receiveMonotonicNs = kReceiveMonotonicBaseNs + sequence * 1'000u;
  out.producerEpoch = 1u;
  out.sourceGeneration = 1u;
  out.sessionEpoch = 1u;
  out.frameSequence = sequence;
  out.shardSequence = sequence;
  out.sourceId = 1u;
  out.flags = hftrec::replay::EventArrivalApplicationFrame;
  if (exchangeTsNs > out.receiveRealtimeNs) {
    out.flags |= hftrec::replay::EventArrivalExchangeAheadOfReceive;
  }
  return out;
}

inline std::string arrivalTail(std::uint64_t sequence,
                               std::int64_t exchangeTsNs) {
  const hftrec::replay::EventArrival arrival =
      applicationArrival(sequence, exchangeTsNs);
  return std::to_string(arrival.receiveRealtimeNs) + "," +
      std::to_string(arrival.receiveMonotonicNs) + "," +
      std::to_string(arrival.producerEpoch) + "," +
      std::to_string(arrival.sourceGeneration) + "," +
      std::to_string(arrival.sessionEpoch) + "," +
      std::to_string(arrival.frameSequence) + "," +
      std::to_string(arrival.shardSequence) + "," +
      std::to_string(arrival.sourceId) + ",0,0," +
      std::to_string(arrival.flags);
}

inline std::string capturedManifestJson(
    std::string exchange,
    std::string market,
    std::string symbol,
    const CapturedChannelCounts& counts) {
  hftrec::capture::SessionManifest manifest{};
  manifest.sessionId = "backtest-captured-fixture";
  manifest.exchange = std::move(exchange);
  manifest.market = std::move(market);
  manifest.symbols = {std::move(symbol)};
  manifest.sessionStatus = "complete";
  manifest.startedAtNs = kReceiveRealtimeBaseNs;
  manifest.endedAtNs = kReceiveRealtimeBaseNs + 1'000'000'000LL;
  manifest.structurallyLoadable = true;
  manifest.sessionHealth = hftrec::SessionHealth::Clean;
  manifest.exactReplayEligible = true;

  manifest.tradesEnabled = counts.trades != 0u;
  manifest.tradesCount = counts.trades;
  if (manifest.tradesEnabled) {
    manifest.tradesRuntime.state = "live";
    manifest.tradesRuntime.required = true;
  }
  manifest.bookTickerEnabled = counts.bookTickers != 0u;
  manifest.bookTickerCount = counts.bookTickers;
  if (manifest.bookTickerEnabled) {
    manifest.bookTickerRuntime.state = "live";
    manifest.bookTickerRuntime.required = true;
  }
  manifest.priceLimitEnabled = counts.priceLimits != 0u;
  manifest.priceLimitCount = counts.priceLimits;
  if (manifest.priceLimitEnabled) {
    manifest.priceLimitRuntime.state = "live";
    manifest.priceLimitRuntime.required = true;
  }
  manifest.orderbookEnabled = counts.depth != 0u;
  manifest.depthCount = counts.depth;
  if (manifest.orderbookEnabled) {
    manifest.depthRuntime.state = "live";
    manifest.depthRuntime.required = true;
  }
  manifest.candlesEnabled = counts.candles != 0u;
  manifest.candlesCount = counts.candles;

  manifest.arrivalClock.capturedRows = counts.trades + counts.bookTickers +
      counts.priceLimits + counts.depth;
  manifest.arrivalClock.historicalRows = counts.candles;
  manifest.arrivalClock.firstReceiveRealtimeNs =
      kReceiveRealtimeBaseNs + 1'000;
  manifest.arrivalClock.lastReceiveRealtimeNs =
      kReceiveRealtimeBaseNs + 1'000'000'000LL;
  manifest.arrivalClock.firstReceiveMonotonicNs =
      kReceiveMonotonicBaseNs + 1'000u;
  manifest.arrivalClock.lastReceiveMonotonicNs =
      kReceiveMonotonicBaseNs + 1'000'000'000u;
  return hftrec::capture::renderManifestJson(manifest);
}

inline std::string bookTickerRow(
    std::int64_t bid,
    std::int64_t bidQty,
    std::int64_t ask,
    std::int64_t askQty,
    std::int64_t exchangeTsNs,
    std::uint64_t sequence,
    std::string_view symbol,
    std::string_view exchange,
    std::string_view market = "futures") {
  return "[" + std::to_string(sequence) + "," + std::to_string(bid) +
      "," + std::to_string(bidQty) + "," + std::to_string(ask) + "," +
      std::to_string(askQty) + "," + std::to_string(exchangeTsNs) +
      ",\"" + std::string{symbol} + "\",\"" + std::string{exchange} +
      "\",\"" + std::string{market} + "\"," +
      std::to_string(sequence) + "," + std::to_string(sequence) + "," +
      arrivalTail(sequence, exchangeTsNs) + "]\n";
}

inline std::string tradeRow(
    std::int64_t price,
    std::int64_t qty,
    std::int64_t side,
    std::int64_t exchangeTsNs,
    std::uint64_t sequence,
    std::string_view symbol,
    std::string_view exchange,
    std::string_view market = "futures") {
  return "[" + std::to_string(price) + "," + std::to_string(qty) + "," +
      std::to_string(side) + "," + std::to_string(exchangeTsNs) + "," +
      std::to_string(sequence) + "," + std::to_string(sequence) + "," +
      std::to_string(sequence) + "," + std::to_string(price) +
      ",0,\"" + std::string{symbol} + "\",\"" + std::string{exchange} +
      "\",\"" + std::string{market} + "\"," +
      std::to_string(sequence) + "," + std::to_string(sequence) + "," +
      arrivalTail(sequence, exchangeTsNs) + "]\n";
}

inline std::string priceLimitRow(
    std::int64_t exchangeTsNs,
    std::int64_t buyLimit,
    std::int64_t sellLimit,
    std::uint64_t sequence) {
  return "[" + std::to_string(exchangeTsNs) + "," +
      std::to_string(buyLimit) + "," + std::to_string(sellLimit) +
      ",1," + std::to_string(sequence) + "," +
      std::to_string(sequence) + "," +
      arrivalTail(sequence, exchangeTsNs) + "]\n";
}

struct DepthLevel {
  std::int64_t price{0};
  std::int64_t qty{0};
  std::int64_t side{0};
};

inline std::uint64_t taggedDepthTimestamp(std::int64_t exchangeTsNs) noexcept {
  return (1ULL << 63u) | static_cast<std::uint64_t>(exchangeTsNs);
}

inline std::string depthTapeRow(
    std::int64_t exchangeTsNs,
    std::uint64_t sequence,
    std::initializer_list<DepthLevel> levels) {
  std::string out = "[" + std::to_string(sequence) + "," +
      std::to_string(taggedDepthTimestamp(exchangeTsNs)) + "," +
      std::to_string(sequence) + "," + std::to_string(sequence) + "," +
      arrivalTail(sequence, exchangeTsNs);
  for (const DepthLevel& level : levels) {
    out += "," + std::to_string(level.price) + "," +
        std::to_string(level.qty);
  }
  out += "]\n";
  return out;
}

inline std::string depthSidecarRow(
    std::int64_t exchangeTsNs,
    std::uint64_t sequence,
    std::initializer_list<DepthLevel> levels) {
  std::string out = "[" + std::to_string(sequence) + "," +
      std::to_string(taggedDepthTimestamp(exchangeTsNs));
  auto it = levels.begin();
  while (it != levels.end()) {
    const std::int64_t side = it->side;
    std::uint64_t count = 0u;
    while (it != levels.end() && it->side == side) {
      ++count;
      ++it;
    }
    out += "," + std::to_string(side) + "," + std::to_string(count);
  }
  out += "]\n";
  return out;
}

inline constexpr std::string_view historicalArrivalTail() noexcept {
  return "0,0,0,0,0,0,0,0,0,0,2";
}

inline std::string candleRow(std::int64_t tier,
                             std::int64_t exchangeTsNs,
                             std::int64_t high,
                             std::int64_t low,
                             std::int64_t quoteAmount,
                             std::uint64_t sequence,
                             std::string_view symbol,
                             std::string_view exchange,
                             std::string_view timeframe) {
  return "[" + std::to_string(tier) + "," +
      std::to_string(exchangeTsNs) + "," + std::to_string(high) + "," +
      std::to_string(high) + "," + std::to_string(low) + "," +
      std::to_string(low) + ",0," + std::to_string(quoteAmount) +
      ",0,\"" + std::string{exchange} + "\",\"futures\",\"" +
      std::string{symbol} + "\",\"" + std::string{timeframe} +
      "\",60000000000," + std::to_string(sequence) + "," +
      std::to_string(sequence) + "," +
      std::string{historicalArrivalTail()} + "]\n";
}

}  // namespace hft_backtest::test_support
