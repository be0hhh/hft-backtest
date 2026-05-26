#include "backtest_engine.hpp"

#include <algorithm>

namespace hft_backtest::detail {

void buildTimeline(const SessionData& session, std::vector<TimelineEvent>& out) {
  out.clear();
  out.reserve(session.depths.size() + session.trades.size() + session.bookTickers.size());
  for (std::size_t i = 0; i < session.depths.size(); ++i) out.push_back(TimelineEvent{session.depths[i].tsNs, session.depths[i].tsNs, session.depths[i].ingestSeq, 0, 0, i});
  for (std::size_t i = 0; i < session.trades.size(); ++i) out.push_back(TimelineEvent{session.trades[i].tsNs, session.trades[i].tsNs, session.trades[i].ingestSeq, 1, 0, i});
  for (std::size_t i = 0; i < session.bookTickers.size(); ++i) out.push_back(TimelineEvent{session.bookTickers[i].tsNs, session.bookTickers[i].tsNs, session.bookTickers[i].ingestSeq, 2, 0, i});
  std::stable_sort(out.begin(), out.end(), [](const TimelineEvent& lhs, const TimelineEvent& rhs) noexcept {
    if (lhs.tsNs != rhs.tsNs) return lhs.tsNs < rhs.tsNs;
    if (lhs.ingestSeq != rhs.ingestSeq) return lhs.ingestSeq < rhs.ingestSeq;
    if (lhs.legIndex != rhs.legIndex) return lhs.legIndex < rhs.legIndex;
    return lhs.kind < rhs.kind;
  });
}

void buildTimeline(const std::vector<SessionData>& sessions, std::vector<TimelineEvent>& out) {
  out.clear();
  std::size_t capacity = 0;
  for (const SessionData& session : sessions) capacity += session.depths.size() + session.trades.size() + session.bookTickers.size();
  out.reserve(capacity);
  for (std::size_t legIndex = 0; legIndex < sessions.size(); ++legIndex) {
    const SessionData& session = sessions[legIndex];
    const std::uint8_t leg = static_cast<std::uint8_t>(legIndex);
    for (std::size_t i = 0; i < session.depths.size(); ++i) out.push_back(TimelineEvent{session.depths[i].tsNs, session.depths[i].tsNs, session.depths[i].ingestSeq, 0, leg, i});
    for (std::size_t i = 0; i < session.trades.size(); ++i) out.push_back(TimelineEvent{session.trades[i].tsNs, session.trades[i].tsNs, session.trades[i].ingestSeq, 1, leg, i});
    for (std::size_t i = 0; i < session.bookTickers.size(); ++i) out.push_back(TimelineEvent{session.bookTickers[i].tsNs, session.bookTickers[i].tsNs, session.bookTickers[i].ingestSeq, 2, leg, i});
  }
  std::stable_sort(out.begin(), out.end(), [](const TimelineEvent& lhs, const TimelineEvent& rhs) noexcept {
    if (lhs.tsNs != rhs.tsNs) return lhs.tsNs < rhs.tsNs;
    if (lhs.ingestSeq != rhs.ingestSeq) return lhs.ingestSeq < rhs.ingestSeq;
    if (lhs.legIndex != rhs.legIndex) return lhs.legIndex < rhs.legIndex;
    return lhs.kind < rhs.kind;
  });
}

}  // namespace hft_backtest::detail
