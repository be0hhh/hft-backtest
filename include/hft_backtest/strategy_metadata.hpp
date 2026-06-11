#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

#include "hft_backtest/api.hpp"

namespace hft_backtest {

inline constexpr std::size_t kStrategyMetadataMaxParams = 16u;
inline constexpr std::size_t kStrategyMetadataMaxIndicators = 8u;
inline constexpr std::size_t kStrategyMetadataMaxParamGroups = 8u;

enum class StrategyParamKind : std::uint8_t {
  Text = 0,
  Uint = 1,
  DecimalAmount = 2,
  DecimalQuoteAmount = 3,
};

struct StrategyParamMetadata {
  const char* key{nullptr};
  const char* labelRu{nullptr};
  const char* descriptionRu{nullptr};
  const char* defaultValue{nullptr};
  StrategyParamKind kind{StrategyParamKind::Text};
  std::uint8_t exclusiveGroup{0u};
  bool required{true};
  bool advanced{false};
  bool defaultActive{true};
};


struct StrategyParamGroupMetadata {
  std::uint8_t id{0u};
  const char* key{nullptr};
};

struct StrategyIndicatorMetadata {
  const char* id{nullptr};
  const char* labelRu{nullptr};
};

struct StrategyMetadata {
  const char* id{nullptr};
  const char* displayNameRu{nullptr};
  const char* configTemplateFile{nullptr};
  std::uint8_t minSessionCount{1u};
  std::uint8_t maxSessionCount{1u};
  std::size_t paramCount{0u};
  StrategyParamMetadata params[kStrategyMetadataMaxParams]{};
  std::size_t paramGroupCount{0u};
  StrategyParamGroupMetadata paramGroups[kStrategyMetadataMaxParamGroups]{};
  std::size_t indicatorCount{0u};
  StrategyIndicatorMetadata indicators[kStrategyMetadataMaxIndicators]{};
};

HFT_BACKTEST_API const StrategyMetadata* findStrategyMetadata(std::string_view id) noexcept;
HFT_BACKTEST_API const StrategyMetadata* strategyMetadataAt(std::size_t index) noexcept;
HFT_BACKTEST_API std::size_t strategyMetadataCount() noexcept;

}  // namespace hft_backtest
