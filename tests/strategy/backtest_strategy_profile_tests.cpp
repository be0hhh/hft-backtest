#include "hft_backtest/strategy_metadata.hpp"

#include <cstdlib>
#include <cstdint>
#include <cstddef>
#include <iostream>
#include <string_view>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << "\n";
    return false;
  }
  return true;
}

bool metadataHasParam(const hft_backtest::StrategyMetadata& metadata, std::string_view key) noexcept {
  for (std::size_t i = 0; i < metadata.paramCount; ++i) {
    const hft_backtest::StrategyParamMetadata& param = metadata.params[i];
    if (param.key != nullptr && key == param.key) return true;
  }
  return false;
}

bool metadataParamDefaultIs(const hft_backtest::StrategyMetadata& metadata,
                            std::string_view key,
                            std::string_view expected) noexcept {
  for (std::size_t i = 0; i < metadata.paramCount; ++i) {
    const hft_backtest::StrategyParamMetadata& param = metadata.params[i];
    if (param.key != nullptr && key == param.key) {
      return param.defaultValue != nullptr && expected == param.defaultValue;
    }
  }
  return false;
}

bool metadataParamGroupIs(const hft_backtest::StrategyMetadata& metadata,
                          std::string_view key,
                          std::uint8_t group,
                          bool defaultActive) noexcept {
  for (std::size_t i = 0; i < metadata.paramCount; ++i) {
    const hft_backtest::StrategyParamMetadata& param = metadata.params[i];
    if (param.key != nullptr && key == param.key) {
      return param.exclusiveGroup == group && param.defaultActive == defaultActive;
    }
  }
  return false;
}

bool testStatArbPairwiseTakerDiscoversAllPairsProfile() {
  const hft_backtest::StrategyMetadata* metadata = hft_backtest::findStrategyMetadata("stat_arb_pairwise_taker");
  return expect(metadata != nullptr, "stat_arb_pairwise_taker metadata is exported") &&
         expect(metadata->configTemplateFile != nullptr &&
                    std::string_view{metadata->configTemplateFile} ==
                        "strategy_profiles/stat_arb_pairwise_taker.ini",
                "stat_arb_pairwise_taker metadata discovers all-pairs profile") &&
         expect(metadata->minSessionCount == 2u, "stat_arb_pairwise_taker requires at least two sessions") &&
         expect(metadata->maxSessionCount == 50u, "stat_arb_pairwise_taker allows the GUI multileg cap") &&
         expect(metadataHasParam(*metadata, "ema_window_ms"), "pairwise metadata contains ema_window_ms") &&
         expect(metadataHasParam(*metadata, "entry_edge_bps"), "pairwise metadata contains entry_edge_bps") &&
         expect(metadataHasParam(*metadata, "ladder_step_bps"), "pairwise metadata contains ladder_step_bps") &&
         expect(metadataHasParam(*metadata, "exit_edge_bps"), "pairwise metadata contains exit_edge_bps") &&
         expect(metadataHasParam(*metadata, "max_ladder_levels"), "pairwise metadata contains max_ladder_levels") &&
         expect(!metadataHasParam(*metadata, "max_active_pairs"), "pairwise metadata hides old max_active_pairs") &&
         expect(!metadataHasParam(*metadata, "max_pair_entries_per_cycle"),
                "pairwise metadata hides old max_pair_entries_per_cycle") &&
         expect(!metadataHasParam(*metadata, "full_scan_budget_pairs"),
                "pairwise metadata hides old full_scan_budget_pairs") &&
         expect(!metadataHasParam(*metadata, "ewma_alpha_pct"), "pairwise metadata hides old ewma_alpha_pct") &&
         expect(!metadataHasParam(*metadata, "center_window_ms"), "pairwise metadata hides unrelated center_window_ms") &&
         expect(!metadataHasParam(*metadata, "vol_window_ms"), "pairwise metadata hides unrelated vol_window_ms") &&
         expect(!metadataHasParam(*metadata, "trigger_edge_bps"), "pairwise metadata hides unrelated trigger_edge_bps") &&
         expect(metadataHasParam(*metadata, "amount_qty"), "pairwise metadata contains amount_qty") &&
         expect(metadataHasParam(*metadata, "order_qty"), "pairwise metadata contains order_qty") &&
         expect(metadataParamDefaultIs(*metadata, "ema_window_ms", "5000"), "pairwise ema window default comes from ini") &&
         expect(metadataParamDefaultIs(*metadata, "entry_edge_bps", "10"), "pairwise entry edge default comes from ini") &&
         expect(metadataParamDefaultIs(*metadata, "ladder_step_bps", "2"), "pairwise ladder step default comes from ini") &&
         expect(metadataParamDefaultIs(*metadata, "exit_edge_bps", "5"), "pairwise exit edge default comes from ini") &&
         expect(metadataParamDefaultIs(*metadata, "max_ladder_levels", "3"),
                "pairwise ladder levels default comes from ini") &&
         expect(metadataParamDefaultIs(*metadata, "amount_qty", "10"), "pairwise amount_qty default comes from ini") &&
         expect(metadataParamDefaultIs(*metadata, "order_qty", "0.001"),
                "pairwise order_qty default comes from commented ini") &&
         expect(metadataParamGroupIs(*metadata, "amount_qty", 1u, true),
                "pairwise amount_qty is active sizing default") &&
         expect(metadataParamGroupIs(*metadata, "order_qty", 1u, false),
                "pairwise order_qty is inactive sizing option");
}

}  // namespace

int main() {
  return testStatArbPairwiseTakerDiscoversAllPairsProfile() ? EXIT_SUCCESS : EXIT_FAILURE;
}
