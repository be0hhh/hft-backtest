#pragma once

#include "cxet/Canon/PositionAndExchange.hpp"
#include "cxet/Primitives/Buf/SmallBuf.hpp"
#include "cxet/Primitives/Raw/Counts/ExchangeId.hpp"
#include "cxet/Primitives/Raw/Counts/Id.hpp"

#include <cstddef>
#include <cstdint>

namespace hft_backtest::detail {

inline constexpr std::uint64_t kBacktestClientIntentIdWrap =
    1000000000000ull;

[[nodiscard]] constexpr Id normalizeBacktestClientIntentId(Id id) noexcept {
  if (id.raw == 0u) return Id{1u};
  if (id.raw >= kBacktestClientIntentIdWrap) {
    return Id{1u + (id.raw % (kBacktestClientIntentIdWrap - 1u))};
  }
  return id;
}

[[nodiscard]] inline bool makeBacktestClientOrderId(
    ExchangeId exchange,
    Id clientIntentId,
    SmallBuf& out, bool normalizeLocalIntent = true) noexcept {
  out = SmallBuf{};
  if (normalizeLocalIntent) clientIntentId = normalizeBacktestClientIntentId(clientIntentId);
  else if (!clientIntentId.raw) return false;
  std::size_t pos = 0u;
  const char prefix =
      exchange.raw == canon::kExchangeIdGate.raw ? 't' : 'h';
  constexpr char kMiddle[] = "-o-";
  if (SmallBuf::capacity <= 7u) return false;
  out.data[pos++] = prefix;
  for (char c : kMiddle) {
    if (c == '\0') break;
    if (pos + 1u >= SmallBuf::capacity) return false;
    out.data[pos++] = c;
  }

  char digits[20]{};
  std::size_t digitCount = 0u;
  std::uint64_t value = clientIntentId.raw;
  do {
    digits[digitCount++] = static_cast<char>('0' + value % 10u);
    value /= 10u;
  } while (value != 0u && digitCount < sizeof(digits));
  if (pos + digitCount + 2u >= SmallBuf::capacity) return false;
  while (digitCount != 0u) out.data[pos++] = digits[--digitCount];
  out.data[pos++] = '-';
  out.data[pos++] = '1';
  out.data[pos] = '\0';
  return true;
}

}  // namespace hft_backtest::detail
