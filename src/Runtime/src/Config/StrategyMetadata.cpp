#include "hft_backtest/StrategyMetadata.hpp"

#include "cxet/Primitives/Buf/CStringPrimitives.hpp"
#include "trading_core/Strategy/GeneratedStrategyRegistry.hpp"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <string_view>

namespace hft_backtest {
namespace {

inline constexpr std::size_t kMaximumStrategies = 64u;

struct MetadataCache final {
  std::array<StrategyMetadata, kMaximumStrategies> strategies{};
  std::size_t count{0u};
};

[[noreturn]] void failClosedMetadataContract() noexcept {
  std::abort();
}

[[nodiscard]] MetadataCache buildMetadataCache() noexcept {
  MetadataCache cache{};
  const auto count = trading_core::strategyDescriptorCount();
  if (count > cache.strategies.size()) failClosedMetadataContract();
  // Discovery projects the selected canonical provider. Replay admission still
  // validates each descriptor's capabilities against the captured inputs.
  for (std::size_t i = 0u; i < count; ++i) {
    const auto* descriptor = trading_core::strategyDescriptorAt(i);
    if (descriptor == nullptr || descriptor->name == nullptr ||
        descriptor->name[0] == '\0' ||
        !trading_core::strategyDescriptorContractValid(*descriptor)) {
      failClosedMetadataContract();
    }
    for (std::size_t previous = 0u; previous < cache.count; ++previous) {
      if (cxet::bytescan::hftStrcmp(cache.strategies[previous].id,
                                  descriptor->name) == 0)
        failClosedMetadataContract();
    }
    StrategyMetadata& metadata = cache.strategies[cache.count++];
    metadata.id = metadata.displayNameRu = descriptor->name;
    metadata.configTemplateFile = nullptr;
    metadata.minSessionCount = descriptor->minLegs;
    metadata.maxSessionCount = descriptor->maxLegs;
    metadata.paramCount = metadata.paramGroupCount = metadata.indicatorCount = 0u;
    // Descriptors own static names and exact leg bounds, but declare no INI
    // template, UI parameter defaults or indicator profiles. Those stay absent.
  }
  return cache;
}

[[nodiscard]] const MetadataCache& metadataCache() noexcept {
  static const MetadataCache cache = buildMetadataCache();
  return cache;
}

}  // namespace

const StrategyMetadata* findStrategyMetadata(std::string_view id) noexcept {
  const MetadataCache& cache = metadataCache();
  for (std::size_t i = 0u; i < cache.count; ++i) {
    const StrategyMetadata& metadata = cache.strategies[i];
    if (metadata.id != nullptr && id == std::string_view{metadata.id}) {
      return &metadata;
    }
  }
  return nullptr;
}

const StrategyMetadata* strategyMetadataAt(std::size_t index) noexcept {
  const MetadataCache& cache = metadataCache();
  return index < cache.count ? &cache.strategies[index] : nullptr;
}

std::size_t strategyMetadataCount() noexcept {
  return metadataCache().count;
}

}  // namespace hft_backtest
