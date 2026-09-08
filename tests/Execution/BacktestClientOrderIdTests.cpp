#include "../../src/Runtime/src/Execution/BacktestClientOrderId.hpp"

#include "trading_core/Execution/ExecutionState/UserEventIdentity.hpp"
#include "cxet/Primitives/Buf/StringView.hpp"

#include <cstdint>
#include <iostream>

namespace {

using namespace hft_backtest::detail;

[[nodiscard]] Id clientIntentId(std::uint64_t raw) noexcept {
  Id out{};
  out.raw = raw;
  return out;
}

[[nodiscard]] bool expect(bool value, const char* message) {
  if (!value) std::cerr << "FAIL: " << message << '\n';
  return value;
}

[[nodiscard]] bool clientOrderIdEquals(const SmallBuf& value,
                                       cxet::StringView expected) noexcept {
  return cxet::StringView::fromCString(value.data) == expected;
}

[[nodiscard]] bool canonicalParserReadsOrderId(
    const SmallBuf& value,
    std::uint64_t expectedClientIntentId) noexcept {
  trading_core::strategy::RuntimeClientOrderId parsed{};
  return trading_core::strategy::parseRuntimeClientOrderId(value, parsed) &&
      parsed.valid &&
      parsed.action ==
          trading_core::strategy::RuntimeClientOrderAction::Order &&
      parsed.clientIntentId.raw == expectedClientIntentId &&
      parsed.generation == 1u;
}

[[nodiscard]] bool testBacktestClientOrderIdParity() {
  SmallBuf out{};
  if (!makeBacktestClientOrderId(
          canon::kExchangeIdBinance, clientIntentId(123u), out) ||
      !expect(clientOrderIdEquals(out, "h-o-123-1"),
              "normal client id bytes must match the canonical runtime form") ||
      !expect(canonicalParserReadsOrderId(out, 123u),
              "canonical trading-core parser must read the normal id")) {
    return false;
  }
  if (!makeBacktestClientOrderId(
          canon::kExchangeIdGate, clientIntentId(42u), out) ||
      !expect(clientOrderIdEquals(out, "t-o-42-1"),
              "Gate client id bytes must use the t prefix") ||
      !expect(canonicalParserReadsOrderId(out, 42u),
              "canonical trading-core parser must read the Gate id")) {
    return false;
  }
  if (!makeBacktestClientOrderId(
          canon::kExchangeIdBinance, clientIntentId(0u), out) ||
      !expect(clientOrderIdEquals(out, "h-o-1-1"),
              "zero client intent id must normalize to one")) {
    return false;
  }
  return makeBacktestClientOrderId(
             canon::kExchangeIdBinance,
             clientIntentId(kBacktestClientIntentIdWrap), out) &&
      expect(clientOrderIdEquals(out, "h-o-2-1"),
             "wrapped client intent id must preserve runtime normalization") &&
      expect(canonicalParserReadsOrderId(out, 2u),
             "canonical parser must observe the normalized wrapped id");
}

}  // namespace

int main() {
  return testBacktestClientOrderIdParity() ? 0 : 1;
}
