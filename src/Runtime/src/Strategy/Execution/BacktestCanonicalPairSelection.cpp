#include "BacktestCanonicalPairSelection.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <numeric>

namespace hft_backtest {
bool BacktestCanonicalPairSelection::bindCold(const trading_core::StrategyRuntimePolicy& policy,
    std::span<const trading_core::StrategySelectionIdentity> identities,
    std::uint64_t maximumAgeNs, std::string& error) noexcept {
  const auto refuse = [&](const char* text) noexcept {
    try { error = text; } catch (...) { error.clear(); }
    return false;
  };
  if (policy_ || !policy.selection.enabled() || !policy.evaluateCandidate ||
      !policy.selectionIdentityLess || identities.empty() || identities.size() > 100'000u || !maximumAgeNs)
    return refuse("unsupported virtual canonical selection configuration");
  try {
    const auto leaves = std::bit_ceil(identities.size());
    tree_.resize(2u * leaves); ties_.resize(identities.size());
    candidates_.resize(identities.size()); observedAt_.resize(identities.size());
    identities_.assign(identities.begin(), identities.end());
    std::vector<std::uint32_t> ordered(identities.size());
    std::iota(ordered.begin(), ordered.end(), 0u);
    std::sort(ordered.begin(), ordered.end(), [&](auto left, auto right) noexcept {
      if (policy.selectionIdentityLess(identities[left], identities[right])) return true;
      if (policy.selectionIdentityLess(identities[right], identities[left])) return false;
      return left < right;
    });
    for (std::uint32_t rank = 0u; rank < ordered.size(); ++rank) ties_[ordered[rank]] = rank;
  } catch (...) { return refuse("virtual canonical selection storage unavailable"); }
  if (!selection_.bindCold(tree_, static_cast<std::uint32_t>(identities.size()), policy.selection))
    return refuse("canonical selection owner binding failed");
  policy_ = &policy; maximumAgeNs_ = maximumAgeNs;
  return true;
}

bool BacktestCanonicalPairSelection::appendCold(
    std::span<const trading_core::StrategySelectionIdentity> identities,
    std::uint64_t maximumAgeNs, std::string& error) noexcept {
  const auto refuse = [&](const char* text) noexcept {
    try { error = text; } catch (...) { error.clear(); }
    return false;
  };
  if (!policy_ || maximumAgeNs != maximumAgeNs_ || identities.size() < identities_.size() ||
      identities.size() > 100'000u) return refuse("invalid canonical selection membership append");
  for (std::size_t index = 0u; index < identities_.size(); ++index)
    if (identities[index].expiryRealtimeNs != identities_[index].expiryRealtimeNs ||
        identities[index].nativeSymbol != identities_[index].nativeSymbol)
      return refuse("canonical selection append changed an existing candidate identity");
  if (identities.size() == identities_.size()) return true;
  BacktestCanonicalPairSelection replacement;
  if (!replacement.bindCold(*policy_, identities, maximumAgeNs, error)) return false;
  for (std::size_t index = 0u; index < candidates_.size(); ++index) {
    replacement.candidates_[index] = candidates_[index];
    replacement.observedAt_[index] = observedAt_[index];
    const auto& candidate = candidates_[index];
    replacement.selection_.update(static_cast<std::uint32_t>(index), candidate.covered,
        candidate.costE8, candidate.oldestMono, replacement.ties_[index], policy_->selection);
  }
  const bool selected = selection_.status() == trading_core::StrategyInstanceSelection::Status::Selected;
  if (!replacement.selection_.applyCohortDecisionOwner(selection_.status(),
      selected ? selection_.selected() : trading_core::StrategyInstanceSelection::absent,
      selected ? selection_.selectedCostE8() : 0, selection_.startedMonoOwner()))
    return refuse("canonical selection state could not survive membership append");
  // The canonical view borrows replacement.tree_'s allocation, whose address
  // survives this vector swap. Old storage is destroyed after the owner swap.
  tree_.swap(replacement.tree_); ties_.swap(replacement.ties_);
  candidates_.swap(replacement.candidates_); observedAt_.swap(replacement.observedAt_);
  identities_.swap(replacement.identities_); selection_ = replacement.selection_;
  return true;
}

void BacktestCanonicalPairSelection::withdraw(std::uint32_t context, std::uint64_t now) noexcept {
  if (!policy_ || context >= candidates_.size() || !now) return;
  candidates_[context] = {}; observedAt_[context] = now;
  if (selection_.status() == trading_core::StrategyInstanceSelection::Status::Selected &&
      selection_.selected() == context) selection_.invalidateOwner();
  else selection_.update(context, false, 0, 0u, ties_[context], policy_->selection);
}

bool BacktestCanonicalPairSelection::observe(std::uint32_t context,
    const trading_core::StrategyParamStorage& params,
    const trading_core::strategy::PairStrategyInputs& input,
    const trading_core::StrategyContext& cycle, bool eligible) noexcept {
  if (!policy_ || context >= candidates_.size() || !cycle.nowMono.raw) return false;
  auto& candidate = candidates_[context];
  const bool evaluated = policy_->evaluateCandidate(params, input, cycle, eligible,
      maximumAgeNs_, candidate);
  if (!evaluated) candidate = {};
  observedAt_[context] = cycle.nowMono.raw;
  selection_.update(context, evaluated && candidate.covered, candidate.costE8,
      candidate.oldestMono, ties_[context], policy_->selection);
  return evaluated;
}

bool BacktestCanonicalPairSelection::needsFullCut(std::uint64_t now) const noexcept {
  return policy_ && policy_->selection.availableAtDeadline && selection_.startedOwner() &&
      selection_.status() == trading_core::StrategyInstanceSelection::Status::Waiting &&
      now >= selection_.startedMonoOwner() &&
      now - selection_.startedMonoOwner() >= policy_->selection.warmupNs;
}

void BacktestCanonicalPairSelection::finishCut(std::uint64_t now, bool fullReady,
    bool healthy) noexcept {
  if (!policy_) return;
  if (selection_.status() == trading_core::StrategyInstanceSelection::Status::Selected &&
      (!fullReady || !healthy)) { selection_.invalidateOwner(); return; }
  bool current = true;
  if (needsFullCut(now))
    for (const auto observed : observedAt_) current = current && observed == now;
  (void)selection_.observeHealthOwner(fullReady, healthy, now, maximumAgeNs_, policy_->selection, current);
}

TimeNs BacktestCanonicalPairSelection::nextWakeMono() const noexcept {
  if (!policy_ || !selection_.startedOwner() ||
      selection_.status() != trading_core::StrategyInstanceSelection::Status::Waiting) return {};
  const auto started = selection_.startedMonoOwner();
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  return TimeNs{policy_->selection.warmupNs > maximum - started ? maximum :
      started + policy_->selection.warmupNs};
}

bool BacktestCanonicalPairSelection::allows(std::uint32_t context) const noexcept {
  if (!policy_ || context >= candidates_.size() ||
      selection_.status() != trading_core::StrategyInstanceSelection::Status::Selected ||
      selection_.selected() != context) return false;
  const auto& candidate = candidates_[context];
  return candidate.covered && candidate.costE8 > 0 &&
      candidate.costE8 <= policy_->selection.maximumSpotCostE8;
}
}
