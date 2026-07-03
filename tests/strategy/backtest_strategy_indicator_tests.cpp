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

}  // namespace

int main() {
  bool ok = true;
  ok = testBasisConvergenceMetadataRequiresTwoSessions() && ok;
  ok = testStaleToxicFlowPairProbeProfileDoesNotCreateMetadata() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
