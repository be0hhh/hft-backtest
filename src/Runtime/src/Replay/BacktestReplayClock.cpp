#include "BacktestReplayClock.hpp"

#include <limits>

namespace hft_backtest::detail {

CapturedReplayCoordinate projectCapturedReplayCoordinate(
    const CapturedReplayClockAnchor& anchor,
    const hftrec::replay::EventArrival& arrival,
    std::int64_t exchangeTsNs) noexcept {
  CapturedReplayCoordinate out{};

  const __int128 offset = static_cast<__int128>(anchor.firstReceiveRealtimeNs) -
      static_cast<__int128>(anchor.firstReceiveMonotonicNs);
  const __int128 deliveryWide = static_cast<__int128>(arrival.receiveMonotonicNs) + offset;
  out.deliveryTsNs = deliveryWide > 0 &&
          deliveryWide <= std::numeric_limits<std::int64_t>::max()
      ? static_cast<std::int64_t>(deliveryWide)
      : arrival.receiveRealtimeNs;

  out.venueScheduleAdjusted = exchangeTsNs <= 0 ||
      (arrival.flags & hftrec::replay::EventArrivalExchangeTimestampMissing) != 0u;
  out.venueTsNs = out.deliveryTsNs;
  if (!out.venueScheduleAdjusted) {
    const __int128 observedTransit = static_cast<__int128>(arrival.receiveRealtimeNs) -
        static_cast<__int128>(exchangeTsNs);
    const __int128 venueWide = static_cast<__int128>(out.deliveryTsNs) - observedTransit;
    if (venueWide > 0 && venueWide <= out.deliveryTsNs) {
      out.venueTsNs = static_cast<std::int64_t>(venueWide);
    } else {
      out.venueScheduleAdjusted = true;
    }
  }
  return out;
}

}  // namespace hft_backtest::detail
