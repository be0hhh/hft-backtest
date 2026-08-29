#pragma once

#include "hftrec/corpus_contract/EventRows.hpp"

#include <cstdint>

namespace hft_backtest::detail {

struct CapturedReplayClockAnchor {
  std::int64_t firstReceiveRealtimeNs{0};
  std::uint64_t firstReceiveMonotonicNs{0};
};

struct CapturedReplayCoordinate {
  std::int64_t deliveryTsNs{0};
  std::int64_t venueTsNs{0};
  bool venueScheduleAdjusted{false};
};

[[nodiscard]] CapturedReplayCoordinate projectCapturedReplayCoordinate(
    const CapturedReplayClockAnchor& anchor,
    const hftrec::replay::EventArrival& arrival,
    std::int64_t exchangeTsNs) noexcept;

}  // namespace hft_backtest::detail
