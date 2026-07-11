#include "hft_backtest/strategy_metadata.hpp"

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

bool expect(bool value, std::string_view message) {
  if (value) return true;
  std::cerr << "FAIL: " << message << "\n";
  return false;
}

bool metadataHasParam(const hft_backtest::StrategyMetadata& metadata, std::string_view key) noexcept {
  for (std::size_t i = 0; i < metadata.paramCount; ++i) {
    if (metadata.params[i].key && key == metadata.params[i].key) return true;
  }
  return false;
}

bool metadataDisplayNameIs(const hft_backtest::StrategyMetadata& metadata, std::string_view expected) noexcept {
  return metadata.displayNameRu != nullptr && expected == metadata.displayNameRu;
}

bool metadataHasIndicator(const hft_backtest::StrategyMetadata& metadata, std::string_view id) noexcept {
  for (std::size_t i = 0; i < metadata.indicatorCount; ++i) {
    if (metadata.indicators[i].id && id == metadata.indicators[i].id) return true;
  }
  return false;
}

bool testBasisConvergenceMetadataRequiresTwoSessions() {
  const hft_backtest::StrategyMetadata* metadata = hft_backtest::findStrategyMetadata("basis_convergence_probe");
  return expect(metadata != nullptr, "basis_convergence_probe metadata is exported") &&
         expect(metadata->configTemplateFile != nullptr &&
                    std::string_view{metadata->configTemplateFile} == "strategy_profiles/basis_convergence_probe.ini",
                "basis_convergence_probe metadata discovers strategy profile") &&
         expect(metadataDisplayNameIs(*metadata, "Базис spot/futures"),
                "basis convergence display name comes from ini metadata") &&
         expect(metadata->minSessionCount == 2u, "basis convergence probe requires spot and futures sessions") &&
         expect(metadata->maxSessionCount == 2u, "basis convergence probe allows exactly two sessions") &&
         expect(!metadataHasParam(*metadata, "source"), "basis convergence metadata keeps source auto-resolved") &&
         expect(!metadataHasParam(*metadata, "timeframe_sec"), "basis convergence metadata keeps timeframe defaulted") &&
         expect(!metadataHasParam(*metadata, "expiry_utc_ns"), "basis convergence metadata keeps expiry auto-resolved");
}

bool testStaleToxicFlowPairProbeProfileDoesNotCreateMetadata() {
  return expect(hft_backtest::findStrategyMetadata("toxic_flow_pair_probe") == nullptr,
                "toxic_flow_pair_probe metadata stays hidden without descriptor");
}

bool testRecursiveStatArbMetadataIsFixedTwoLegProfile() {
  const hft_backtest::StrategyMetadata* metadata =
      hft_backtest::findStrategyMetadata("recursive_stat_arb_taker");
  return expect(metadata != nullptr, "recursive_stat_arb_taker metadata is exported") &&
         expect(metadata->minSessionCount == 2u && metadata->maxSessionCount == 2u,
                "recursive stat arb requires exactly two sessions") &&
         expect(metadataHasParam(*metadata, "amount_qty"), "recursive stat arb exposes operator sizing") &&
         expect(metadataHasParam(*metadata, "max_quote_age_ms"), "recursive stat arb exposes freshness safety") &&
         expect(!metadataHasParam(*metadata, "lambda"), "recursive stat arb keeps RLS policy out of config") &&
         expect(metadataHasIndicator(*metadata, "recursive_stat_arb"),
                "recursive stat arb declares its JSONL indicator profile");
}

}  // namespace

int main() {
  bool ok = true;
  ok = testBasisConvergenceMetadataRequiresTwoSessions() && ok;
  ok = testStaleToxicFlowPairProbeProfileDoesNotCreateMetadata() && ok;
  ok = testRecursiveStatArbMetadataIsFixedTwoLegProfile() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
