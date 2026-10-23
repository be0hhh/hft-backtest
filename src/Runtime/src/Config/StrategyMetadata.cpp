#include "hft_backtest/StrategyMetadata.hpp"

#include "hft_backtest/generated/StrategyMetadata.hpp"
#include "trading_core/Strategy/GeneratedStrategyRegistry.hpp"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <string_view>

namespace hft_backtest {
namespace {

static_assert(generated::kMaximumParams == kStrategyMetadataMaxParams);
static_assert(generated::kMaximumIndicators == kStrategyMetadataMaxIndicators);
static_assert(generated::kMaximumParamGroups == kStrategyMetadataMaxParamGroups);

struct MetadataCache final {
  std::array<StrategyMetadata, generated::kMaximumStrategies> strategies{};
  std::size_t count{0u};
};

[[noreturn]] void failClosedMetadataContract() noexcept {
  std::abort();
}

[[nodiscard]] bool unsignedInteger(std::string_view value) noexcept {
  if (value.empty()) return false;
  for (const char ch : value) {
    if (ch < '0' || ch > '9') return false;
  }
  return true;
}

[[nodiscard]] StrategyParamKind inferParamKind(std::string_view key,
                                               std::string_view value) noexcept {
  if (key.find("qty") != std::string_view::npos) {
    return key.find("amount") != std::string_view::npos
        ? StrategyParamKind::DecimalQuoteAmount
        : StrategyParamKind::DecimalAmount;
  }
  if (value.find('.') != std::string_view::npos ||
      value.find('-') != std::string_view::npos) {
    return StrategyParamKind::DecimalQuoteAmount;
  }
  return unsignedInteger(value) ? StrategyParamKind::Uint
                                : StrategyParamKind::Text;
}

void applyTemplate(const generated::GeneratedStrategyTemplate& source,
                   StrategyMetadata& destination) noexcept {
  if (source.paramCount > kStrategyMetadataMaxParams ||
      source.paramGroupCount > kStrategyMetadataMaxParamGroups ||
      source.indicatorCount > kStrategyMetadataMaxIndicators ||
      source.fileName == nullptr || source.strategyType == nullptr ||
      source.displayName == nullptr || source.minSessionCount == 0u ||
      source.maxSessionCount < source.minSessionCount) {
    failClosedMetadataContract();
  }

  destination.displayNameRu = source.displayName;
  destination.configTemplateFile = source.fileName;
  destination.minSessionCount = source.minSessionCount;
  destination.maxSessionCount = source.maxSessionCount;
  destination.paramCount = source.paramCount;
  for (std::size_t i = 0u; i < source.paramCount; ++i) {
    const auto& param = source.params[i];
    if (param.key == nullptr || param.defaultValue == nullptr) {
      failClosedMetadataContract();
    }
    destination.params[i] = StrategyParamMetadata{
        .key = param.key,
        .labelRu = param.key,
        .descriptionRu = "",
        .defaultValue = param.defaultValue,
        .kind = inferParamKind(param.key, param.defaultValue),
        .exclusiveGroup = param.exclusiveGroup,
        .required = true,
        .advanced = false,
        .defaultActive = param.defaultActive,
    };
  }

  destination.paramGroupCount = source.paramGroupCount;
  for (std::size_t i = 0u; i < source.paramGroupCount; ++i) {
    const auto& group = source.paramGroups[i];
    if (group.id == 0u || group.key == nullptr) failClosedMetadataContract();
    destination.paramGroups[i] = StrategyParamGroupMetadata{group.id, group.key};
  }

  destination.indicatorCount = source.indicatorCount;
  for (std::size_t i = 0u; i < source.indicatorCount; ++i) {
    const auto& indicator = source.indicators[i];
    if (indicator.id == nullptr || indicator.label == nullptr) {
      failClosedMetadataContract();
    }
    destination.indicators[i] =
        StrategyIndicatorMetadata{indicator.id, indicator.label};
  }
}

[[nodiscard]] MetadataCache buildMetadataCache() noexcept {
  MetadataCache cache{};
  // Profile discovery does not confer replay support: runtime admission checks
  // each canonical descriptor and the recorded inputs it requires.
  for (const auto& source : generated::kStrategyTemplates) {
    if (source.strategyType == nullptr) failClosedMetadataContract();
    const trading_core::StrategyDescriptor* descriptor =
        trading_core::findStrategyDescriptor(source.strategyType);
    if (descriptor == nullptr || descriptor->name == nullptr ||
        descriptor->name[0] == '\0' ||
        !trading_core::strategyDescriptorContractValid(*descriptor) ||
        cache.count == cache.strategies.size()) {
      failClosedMetadataContract();
    }

    StrategyMetadata& metadata = cache.strategies[cache.count++];
    metadata.id = descriptor->name;
    metadata.displayNameRu = descriptor->name;
    metadata.minSessionCount = 1u;
    metadata.maxSessionCount = 1u;
    applyTemplate(source, metadata);
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
