#pragma once
#include "trading_core/Strategy/Descriptor/StrategyInstanceSelection.hpp"
#include "trading_core/Strategy/Descriptor/StrategyRuntimePolicy.hpp"
#include "trading_core/Strategy/Orders/Execution/PairStrategyInputs.hpp"

#include <vector>

namespace hft_backtest {
// One virtual portfolio owns one policy election over its admitted contexts.
// Cold storage is bounded by the canonical 100000-candidate selection limit.
class BacktestCanonicalPairSelection final {
 public:
  [[nodiscard]] bool bindCold(const trading_core::StrategyRuntimePolicy&,
      std::span<const trading_core::StrategySelectionIdentity>,
      std::uint64_t maximumAgeNs, std::string& error) noexcept;
  // Append membership at a cold owner boundary. Existing indices, absolute
  // collection start, winner and actor state survive; no election restart.
  [[nodiscard]] bool appendCold(std::span<const trading_core::StrategySelectionIdentity>,
      std::uint64_t maximumAgeNs, std::string& error) noexcept;
  void withdraw(std::uint32_t context, std::uint64_t now) noexcept;
  [[nodiscard]] bool observe(std::uint32_t context,
      const trading_core::StrategyParamStorage&,
      const trading_core::strategy::PairStrategyInputs&,
      const trading_core::StrategyContext&, bool eligible) noexcept;
  // At the deadline every candidate must be freshly re-evaluated against this
  // owner cut, including membership/continuity withdrawals and retained quotes.
  void finishCut(std::uint64_t now, bool fullReady, bool healthy) noexcept;
  [[nodiscard]] bool needsFullCut(std::uint64_t now) const noexcept;
  [[nodiscard]] TimeNs nextWakeMono() const noexcept;
  [[nodiscard]] bool allows(std::uint32_t context) const noexcept;
  [[nodiscard]] const trading_core::StrategyRuntimePolicy* policy() const noexcept { return policy_; }
  [[nodiscard]] trading_core::StrategyInstanceSelection::Status status() const noexcept {
    return selection_.status();
  }
 private:
  const trading_core::StrategyRuntimePolicy* policy_{};
  trading_core::StrategyInstanceSelection selection_{};
  std::vector<trading_core::StrategyInstanceSelection::Node> tree_{};
  std::vector<std::uint32_t> ties_{};
  std::vector<trading_core::StrategySelectionCandidate> candidates_{};
  std::vector<std::uint64_t> observedAt_{};
  // Source-owned immutable symbol bytes outlive this selection owner.
  std::vector<trading_core::StrategySelectionIdentity> identities_{};
  std::uint64_t maximumAgeNs_{};
};
}
