#include "backtest_engine.hpp"

#include <algorithm>

namespace hft_backtest::detail {

void buildTimeline(const SessionData& session, std::vector<TimelineEvent>& out) {
  out.clear();
  for (std::size_t i = 0; i < session.depths.size(); ++i) out.push_back(TimelineEvent{session.depths[i].tsNs, session.depths[i].tsNs, session.depths[i].ingestSeq, 0, i});
  for (std::size_t i = 0; i < session.trades.size(); ++i) out.push_back(TimelineEvent{session.trades[i].tsNs, session.trades[i].tsNs, session.trades[i].ingestSeq, 1, i});
  for (std::size_t i = 0; i < session.bookTickers.size(); ++i) out.push_back(TimelineEvent{session.bookTickers[i].tsNs, session.bookTickers[i].tsNs, session.bookTickers[i].ingestSeq, 2, i});
  std::stable_sort(out.begin(), out.end(), [](const TimelineEvent& lhs, const TimelineEvent& rhs) noexcept {
    if (lhs.tsNs != rhs.tsNs) return lhs.tsNs < rhs.tsNs;
    if (lhs.ingestSeq != rhs.ingestSeq) return lhs.ingestSeq < rhs.ingestSeq;
    return lhs.kind < rhs.kind;
  });
}

}  // namespace hft_backtest::detail
