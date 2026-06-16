#include "backtest_engine.hpp"

#include <algorithm>
#include <limits>

namespace hft_backtest::detail {
namespace {

bool timelineLess(const TimelineEvent& lhs, const TimelineEvent& rhs) noexcept {
  if (lhs.tsNs != rhs.tsNs) return lhs.tsNs < rhs.tsNs;
  if (lhs.ingestSeq != rhs.ingestSeq) return lhs.ingestSeq < rhs.ingestSeq;
  if (lhs.legIndex != rhs.legIndex) return lhs.legIndex < rhs.legIndex;
  return lhs.kind < rhs.kind;
}

TimelineEvent makeEvent(const SessionData& session, std::uint8_t leg, std::uint8_t kind, std::size_t index) noexcept {
  if (kind == kTimelineKindDepth) return TimelineEvent{session.depths[index].tsNs, session.depths[index].tsNs, session.depths[index].ingestSeq, kind, leg, index};
  if (kind == kTimelineKindTrade) return TimelineEvent{session.trades[index].tsNs, session.trades[index].tsNs, session.trades[index].ingestSeq, kind, leg, index};
  if (kind == kTimelineKindBookTicker) return TimelineEvent{session.bookTickers[index].tsNs, session.bookTickers[index].tsNs, session.bookTickers[index].ingestSeq, kind, leg, index};
  if (kind == kTimelineKindPriceLimit) return TimelineEvent{session.priceLimits[index].tsNs, session.priceLimits[index].tsNs, session.priceLimits[index].ingestSeq, kind, leg, index};
  if (kind == kTimelineKindLiquidation) return TimelineEvent{session.liquidations[index].tsNs, session.liquidations[index].tsNs, session.liquidations[index].ingestSeq, kind, leg, index};
  if (kind == kTimelineKindMarkPrice) return TimelineEvent{session.markPrices[index].tsNs, session.markPrices[index].tsNs, session.markPrices[index].ingestSeq, kind, leg, index};
  if (kind == kTimelineKindIndexPrice) return TimelineEvent{session.indexPrices[index].tsNs, session.indexPrices[index].tsNs, session.indexPrices[index].ingestSeq, kind, leg, index};
  if (kind == kTimelineKindFunding) return TimelineEvent{session.funding[index].tsNs, session.funding[index].tsNs, session.funding[index].ingestSeq, kind, leg, index};
  if (kind == kTimelineKindOpenInterest) return TimelineEvent{session.openInterest[index].tsNs, session.openInterest[index].tsNs, session.openInterest[index].ingestSeq, kind, leg, index};
  return TimelineEvent{session.priceLimits[index].tsNs, session.priceLimits[index].tsNs, session.priceLimits[index].ingestSeq, kind, leg, index};
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
  if (kind == kTimelineKindOpenInterest) return session.openInterest.size();
  return session.priceLimits.size();
}

bool channelSorted(const SessionData& session, std::uint8_t leg, std::uint8_t kind) noexcept {
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
};

bool cursorDone(const TimelineCursor& cursor) noexcept {
  return cursor.index >= cursor.size;
}

TimelineEvent cursorEvent(const TimelineCursor& cursor) noexcept {
  return makeEvent(*cursor.session, cursor.leg, cursor.kind, cursor.index);
}

void pushCursor(std::vector<TimelineCursor>& cursors, const SessionData& session, std::uint8_t leg, std::uint8_t kind) {
  const std::size_t n = channelSize(session, kind);
  if (n != 0u) cursors.push_back(TimelineCursor{&session, leg, kind, 0u, n});
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

  while (out.size() < capacity) {
    std::size_t best = cursors.size();
    TimelineEvent bestEvent{};
    for (std::size_t i = 0u; i < cursors.size(); ++i) {
      if (cursorDone(cursors[i])) continue;
      const TimelineEvent candidate = cursorEvent(cursors[i]);
      if (best == cursors.size() || timelineLess(candidate, bestEvent)) {
        best = i;
        bestEvent = candidate;
      }
    }
    if (best == cursors.size()) return false;
    out.push_back(bestEvent);
    ++cursors[best].index;
  }
  return true;
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
