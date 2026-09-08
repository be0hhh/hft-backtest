#include "hft_backtest/StrategyMetadata.hpp"

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

bool metadataHasIndicator(const hft_backtest::StrategyMetadata& metadata, std::string_view id) noexcept {
  for (std::size_t i = 0; i < metadata.indicatorCount; ++i) {
    if (metadata.indicators[i].id && id == metadata.indicators[i].id) return true;
  }
  return false;
}

bool testStaleBasisConvergenceProfileDoesNotCreateMetadata() {
  return expect(
      hft_backtest::findStrategyMetadata("basis_convergence_probe") == nullptr,
      "basis_convergence_probe metadata stays hidden without descriptor");
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
  ok = testStaleBasisConvergenceProfileDoesNotCreateMetadata() && ok;
  ok = testStaleToxicFlowPairProbeProfileDoesNotCreateMetadata() && ok;
  ok = testRecursiveStatArbMetadataIsFixedTwoLegProfile() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
