#include "hft_backtest/strategy_metadata.hpp"

#include <cstdlib>
#include <cstdint>
#include <cstddef>
#include <string_view>

namespace {

bool expect(bool condition, const char* message) {
  if (!condition) {
    (void)message;
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

bool testEwmaEdgeMakerDiscoversNestedMakerProfile() {
  const hft_backtest::StrategyMetadata* metadata = hft_backtest::findStrategyMetadata("ewma_edge_maker");
  return expect(metadata != nullptr, "ewma_edge_maker metadata is exported") &&
         expect(metadata->configTemplateFile != nullptr &&
                    std::string_view{metadata->configTemplateFile} ==
                        "strategy_profiles/makers/ewma_edge_maker.ini",
                "ewma_edge_maker metadata discovers nested maker profile") &&
         expect(metadata->minSessionCount == 1u, "ewma_edge_maker uses one session") &&
         expect(metadata->maxSessionCount == 1u, "ewma_edge_maker allows exactly one session") &&
         expect(!metadataHasParam(*metadata, "ewma_alpha_pct"), "ewma metadata hides ewma_alpha_pct") &&
         expect(metadataHasParam(*metadata, "center_window_ms"), "ewma metadata contains center_window_ms") &&
         expect(metadataHasParam(*metadata, "center_fast_window_ms"), "ewma metadata contains center_fast_window_ms") &&
         expect(metadataHasParam(*metadata, "vol_window_ms"), "ewma metadata contains vol_window_ms") &&
         expect(metadataHasParam(*metadata, "range_vol_mult_bps"), "ewma metadata contains range_vol_mult_bps") &&
         expect(metadataHasParam(*metadata, "shock_range_mult_bps"), "ewma metadata contains shock_range_mult_bps") &&
         expect(metadataHasParam(*metadata, "min_range_ticks"), "ewma metadata contains min_range_ticks") &&
         expect(metadataHasParam(*metadata, "entry_edge_bps"), "ewma metadata contains entry_edge_bps") &&
         expect(metadataHasParam(*metadata, "exit_edge_bps"), "ewma metadata contains exit_edge_bps") &&
         expect(metadataHasParam(*metadata, "trigger_edge_bps"), "ewma metadata contains trigger_edge_bps") &&
         expect(!metadataHasParam(*metadata, "ewma_window_ms"), "ewma metadata hides old ewma_window_ms") &&
         expect(!metadataHasParam(*metadata, "warmup_ms"), "ewma metadata hides old warmup_ms") &&
         expect(!metadataHasParam(*metadata, "ladder_step_bps"), "ewma metadata hides ladder_step_bps") &&
         expect(!metadataHasParam(*metadata, "max_ladder_levels"), "ewma metadata hides max_ladder_levels") &&
         expect(!metadataHasParam(*metadata, "max_quote_age_ms"), "ewma metadata hides max_quote_age_ms") &&
         expect(metadataHasParam(*metadata, "amount_qty"), "ewma metadata contains amount_qty") &&
         expect(metadataHasParam(*metadata, "order_qty"), "ewma metadata contains order_qty") &&
         expect(!metadataHasParam(*metadata, "ewma_smoothing_bps"), "ewma metadata hides old ewma_smoothing_bps") &&
         expect(metadataParamDefaultIs(*metadata, "center_window_ms", "5000"), "ewma center window default comes from ini") &&
         expect(metadataParamDefaultIs(*metadata, "center_fast_window_ms", "350"), "ewma fast center default comes from ini") &&
         expect(metadataParamDefaultIs(*metadata, "vol_window_ms", "4000"), "ewma vol window default comes from ini") &&
         expect(metadataParamDefaultIs(*metadata, "range_vol_mult_bps", "20000"), "ewma range multiplier default comes from ini") &&
         expect(metadataParamDefaultIs(*metadata, "shock_range_mult_bps", "10000"), "ewma shock multiplier default comes from ini") &&
         expect(metadataParamDefaultIs(*metadata, "min_range_ticks", "3"), "ewma min range default comes from ini") &&
         expect(metadataParamDefaultIs(*metadata, "amount_qty", "10"), "ewma amount_qty default comes from ini") &&
         expect(metadataParamDefaultIs(*metadata, "order_qty", "1"), "ewma order_qty default comes from commented ini") &&
         expect(metadataParamGroupIs(*metadata, "amount_qty", 1u, true),
                "ewma amount_qty is active sizing default") &&
         expect(metadataParamGroupIs(*metadata, "order_qty", 1u, false),
                "ewma order_qty is inactive sizing option");
}

}  // namespace

int main() {
  return testEwmaEdgeMakerDiscoversNestedMakerProfile() ? EXIT_SUCCESS : EXIT_FAILURE;
}
