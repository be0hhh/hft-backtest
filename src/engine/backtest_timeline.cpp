#include "engine/backtest_engine.hpp"
#include "engine/backtest_replay_clock.hpp"

#include <algorithm>

namespace hft_backtest::detail {
namespace {

bool timelineLess(const TimelineEvent& lhs, const TimelineEvent& rhs) noexcept {
  if (lhs.tsNs != rhs.tsNs) return lhs.tsNs < rhs.tsNs;
  if (lhs.deliveryTsNs != rhs.deliveryTsNs) return lhs.deliveryTsNs < rhs.deliveryTsNs;
  if (lhs.producerEpoch != rhs.producerEpoch) return lhs.producerEpoch < rhs.producerEpoch;
  if (lhs.shardId != rhs.shardId) return lhs.shardId < rhs.shardId;
  if (lhs.shardSequence != rhs.shardSequence) return lhs.shardSequence < rhs.shardSequence;
  if (lhs.eventOrdinal != rhs.eventOrdinal) return lhs.eventOrdinal < rhs.eventOrdinal;
  if (lhs.sourceId != rhs.sourceId) return lhs.sourceId < rhs.sourceId;
  if (lhs.sourceGeneration != rhs.sourceGeneration) return lhs.sourceGeneration < rhs.sourceGeneration;
  if (lhs.sessionEpoch != rhs.sessionEpoch) return lhs.sessionEpoch < rhs.sessionEpoch;
  if (lhs.frameSequence != rhs.frameSequence) return lhs.frameSequence < rhs.frameSequence;
  if (lhs.legIndex != rhs.legIndex) return lhs.legIndex < rhs.legIndex;
  if (lhs.kind != rhs.kind) return lhs.kind < rhs.kind;
  return lhs.index < rhs.index;
}

const hftrec::replay::EventArrival& arrivalFor(const SessionData& session,
                                               std::uint8_t kind,
                                               std::size_t index) noexcept {
  if (kind == kTimelineKindDepth) return session.depths[index].arrival;
  if (kind == kTimelineKindTrade) return session.trades[index].arrival;
  if (kind == kTimelineKindBookTicker) return session.bookTickers[index].arrival;
  if (kind == kTimelineKindPriceLimit) return session.priceLimits[index].arrival;
  if (kind == kTimelineKindLiquidation) return session.liquidations[index].arrival;
  if (kind == kTimelineKindMarkPrice) return session.markPrices[index].arrival;
  if (kind == kTimelineKindIndexPrice) return session.indexPrices[index].arrival;
  if (kind == kTimelineKindFunding) return session.funding[index].arrival;
  return session.trades[index].arrival;
}

std::int64_t exchangeTsFor(const SessionData& session,
                           std::uint8_t kind,
                           std::size_t index) noexcept {
  if (kind == kTimelineKindDepth) return session.depths[index].tsNs;
  if (kind == kTimelineKindTrade) return session.trades[index].tsNs;
  if (kind == kTimelineKindBookTicker) return session.bookTickers[index].tsNs;
  if (kind == kTimelineKindPriceLimit) return session.priceLimits[index].tsNs;
  if (kind == kTimelineKindLiquidation) return session.liquidations[index].tsNs;
  if (kind == kTimelineKindMarkPrice) return session.markPrices[index].tsNs;
  if (kind == kTimelineKindIndexPrice) return session.indexPrices[index].tsNs;
  if (kind == kTimelineKindFunding) return session.funding[index].tsNs;
  return 0;
}

std::int64_t ingestSeqFor(const SessionData& session,
                          std::uint8_t kind,
                          std::size_t index) noexcept {
  if (kind == kTimelineKindDepth) return session.depths[index].ingestSeq;
  if (kind == kTimelineKindTrade) return session.trades[index].ingestSeq;
  if (kind == kTimelineKindBookTicker) return session.bookTickers[index].ingestSeq;
  if (kind == kTimelineKindPriceLimit) return session.priceLimits[index].ingestSeq;
  if (kind == kTimelineKindLiquidation) return session.liquidations[index].ingestSeq;
  if (kind == kTimelineKindMarkPrice) return session.markPrices[index].ingestSeq;
  if (kind == kTimelineKindIndexPrice) return session.indexPrices[index].ingestSeq;
  if (kind == kTimelineKindFunding) return session.funding[index].ingestSeq;
  return 0;
}

TimelineEvent makeEvent(const SessionData& session,
                        std::uint8_t leg,
                        std::uint8_t kind,
                        std::size_t index) noexcept {
  const hftrec::replay::EventArrival& arrival = arrivalFor(session, kind, index);
  const std::int64_t exchangeTsNs = exchangeTsFor(session, kind, index);
  const std::uint64_t anchorMonotonicNs =
      session.replayAnchorMonotonicNs != 0u
          ? session.replayAnchorMonotonicNs
          : session.firstReceiveMonotonicNs;
  const std::int64_t anchorReceiveNs =
      session.replayAnchorReceiveNs > 0
          ? session.replayAnchorReceiveNs
          : session.firstReceiveRealtimeNs;
  const CapturedReplayCoordinate clock = projectCapturedReplayCoordinate(
      CapturedReplayClockAnchor{
          anchorReceiveNs,
          anchorMonotonicNs},
      arrival,
      exchangeTsNs);
  const bool applicationAtomicRebase =
      kind == kTimelineKindDepth &&
      session.depths[index].applicationAtomicRebase;
  const bool applicationOnly =
      kind == kTimelineKindBookTicker &&
      session.bookTickers[index].freshnessOnly;

  TimelineEvent out{};
  out.tsNs = applicationAtomicRebase || applicationOnly
      ? clock.deliveryTsNs
      : clock.venueTsNs;
  out.exchangeTsNs = exchangeTsNs;
  out.deliveryTsNs = clock.deliveryTsNs;
  out.deliveryRealtimeNs = arrival.receiveRealtimeNs;
  out.receiveMonotonicNs = arrival.receiveMonotonicNs;
  out.producerEpoch = arrival.producerEpoch;
  out.sourceGeneration = arrival.sourceGeneration;
  out.sessionEpoch = arrival.sessionEpoch;
  out.frameSequence = arrival.frameSequence;
  out.shardSequence = arrival.shardSequence;
  out.sourceId = arrival.sourceId;
  out.shardId = arrival.shardId;
  out.eventOrdinal = arrival.eventOrdinal;
  out.ingestSeq = ingestSeqFor(session, kind, index);
  out.kind = kind;
  out.legIndex = leg;
  out.venueScheduleAdjusted = !applicationOnly &&
      (clock.venueScheduleAdjusted || applicationAtomicRebase);
  out.applicationAtomicRebase = applicationAtomicRebase;
  out.applicationOnly = applicationOnly;
  out.index = index;
  return out;
}

std::size_t channelSize(const SessionData& session, std::uint8_t kind) noexcept {
  if (kind == kTimelineKindDepth) return session.depths.size();
  if (kind == kTimelineKindTrade) return session.trades.size();
  if (kind == kTimelineKindBookTicker) return session.bookTickers.size();
  if (kind == kTimelineKindPriceLimit) return session.priceLimits.size();
  if (kind == kTimelineKindLiquidation) return session.liquidations.size();
  if (kind == kTimelineKindMarkPrice) return session.markPrices.size();
  if (kind == kTimelineKindIndexPrice) return session.indexPrices.size();
  if (kind == kTimelineKindFunding) return session.funding.size();
  // Open-interest has no current captured-arrival corpus contract. Candles are
  // archive seeds only and must never become strategy-visible live deliveries.
  if (kind == kTimelineKindOpenInterest || kind == kTimelineKindCandle) return 0u;
  return 0u;
}

bool channelSorted(const SessionData& session, std::uint8_t leg, std::uint8_t kind) noexcept {
  if (session.timelineOrderKnown &&
      static_cast<std::size_t>(kind) < session.timelineChannelSorted.size() &&
      session.timelineChannelSorted[static_cast<std::size_t>(kind)]) {
    return true;
  }
  const std::size_t n = channelSize(session, kind);
  if (n < 2u) return true;
  TimelineEvent previous = makeEvent(session, leg, kind, 0u);
  for (std::size_t i = 1u; i < n; ++i) {
    const TimelineEvent current = makeEvent(session, leg, kind, i);
    if (timelineLess(current, previous)) return false;
    previous = current;
  }
  return true;
}

void appendUnsorted(const SessionData& session, std::uint8_t leg, std::vector<TimelineEvent>& out) {
  for (std::uint8_t kind = 0u; kind < kTimelineKindCount; ++kind) {
    const std::size_t n = channelSize(session, kind);
    for (std::size_t i = 0; i < n; ++i) out.push_back(makeEvent(session, leg, kind, i));
  }
}

void sortTimeline(std::vector<TimelineEvent>& out) {
  std::stable_sort(out.begin(), out.end(), timelineLess);
}

struct TimelineCursor {
  const SessionData* session{nullptr};
  std::uint8_t leg{0u};
  std::uint8_t kind{0u};
  std::size_t index{0u};
  std::size_t size{0u};
  TimelineEvent event{};
};

bool cursorDone(const TimelineCursor& cursor) noexcept {
  return cursor.index >= cursor.size;
}

bool cursorHeapLess(const TimelineCursor& lhs, const TimelineCursor& rhs) noexcept {
  return timelineLess(rhs.event, lhs.event);
}

void pushCursor(std::vector<TimelineCursor>& cursors, const SessionData& session, std::uint8_t leg, std::uint8_t kind) {
  const std::size_t n = channelSize(session, kind);
  if (n != 0u) cursors.push_back(TimelineCursor{&session, leg, kind, 0u, n, makeEvent(session, leg, kind, 0u)});
}

bool buildMergedTimeline(const std::vector<const SessionData*>& sessions, std::vector<TimelineEvent>& out) {
  std::size_t capacity = 0u;
  for (std::size_t legIndex = 0u; legIndex < sessions.size(); ++legIndex) {
    if (legIndex > static_cast<std::size_t>(std::numeric_limits<std::uint8_t>::max())) return false;
    const SessionData& session = *sessions[legIndex];
    const std::uint8_t leg = static_cast<std::uint8_t>(legIndex);
    for (std::uint8_t kind = 0u; kind < kTimelineKindCount; ++kind) {
      if (!channelSorted(session, leg, kind)) return false;
      capacity += channelSize(session, kind);
    }
  }

  out.clear();
  out.reserve(capacity);

  std::vector<TimelineCursor> cursors{};
  cursors.reserve(sessions.size() * kTimelineKindCount);
  for (std::size_t legIndex = 0u; legIndex < sessions.size(); ++legIndex) {
    const SessionData& session = *sessions[legIndex];
    const std::uint8_t leg = static_cast<std::uint8_t>(legIndex);
    for (std::uint8_t kind = 0u; kind < kTimelineKindCount; ++kind) pushCursor(cursors, session, leg, kind);
  }

  std::make_heap(cursors.begin(), cursors.end(), cursorHeapLess);
  while (!cursors.empty()) {
    std::pop_heap(cursors.begin(), cursors.end(), cursorHeapLess);
    TimelineCursor cursor = cursors.back();
    cursors.pop_back();
    out.push_back(cursor.event);
    ++cursor.index;
    if (!cursorDone(cursor)) {
      cursor.event = makeEvent(*cursor.session, cursor.leg, cursor.kind, cursor.index);
      cursors.push_back(cursor);
      std::push_heap(cursors.begin(), cursors.end(), cursorHeapLess);
    }
  }
  return out.size() == capacity;
}

void buildFallbackTimeline(const std::vector<const SessionData*>& sessions, std::vector<TimelineEvent>& out) {
  out.clear();
  std::size_t capacity = 0u;
  for (const SessionData* session : sessions) {
    for (std::uint8_t kind = 0u; kind < kTimelineKindCount; ++kind) capacity += channelSize(*session, kind);
  }
  out.reserve(capacity);
  for (std::size_t legIndex = 0u; legIndex < sessions.size(); ++legIndex) {
    appendUnsorted(*sessions[legIndex], static_cast<std::uint8_t>(legIndex), out);
  }
  sortTimeline(out);
}

}  // namespace

void buildTimeline(const SessionData& session, std::vector<TimelineEvent>& out) {
  std::vector<const SessionData*> sessions{&session};
  if (!buildMergedTimeline(sessions, out)) buildFallbackTimeline(sessions, out);
}

void buildTimeline(const std::vector<SessionData>& sessions, std::vector<TimelineEvent>& out) {
  std::vector<const SessionData*> sessionRefs{};
  sessionRefs.reserve(sessions.size());
  for (const SessionData& session : sessions) sessionRefs.push_back(&session);
  if (!buildMergedTimeline(sessionRefs, out)) buildFallbackTimeline(sessionRefs, out);
}

}  // namespace hft_backtest::detail
