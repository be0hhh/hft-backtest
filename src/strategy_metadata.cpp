#include "hft_backtest/strategy_metadata.hpp"

#include "hft_trader/runtime/GeneratedStrategyRegistry.hpp"

#include <cctype>
#include <cstddef>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace hft_backtest {
namespace {

#ifndef HFT_BACKTEST_TRADER_SOURCE_DIR
#define HFT_BACKTEST_TRADER_SOURCE_DIR "../hft-trader"
#endif

struct IniTemplate {
  std::string fileName{};
  std::string strategyType{};
  std::vector<std::pair<std::string, std::string>> strategyParams{};
  std::size_t recorderLegCount{1u};
};

struct MetadataCache {
  std::deque<std::string> strings{};
  std::vector<StrategyMetadata> strategies{};
};

std::string trim(std::string_view text) {
  std::size_t begin = 0u;
  while (begin < text.size() && std::isspace(static_cast<unsigned char>(text[begin])) != 0) ++begin;
  std::size_t end = text.size();
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1u])) != 0) --end;
  return std::string{text.substr(begin, end - begin)};
}

std::string lower(std::string text) {
  for (char& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  return text;
}

std::string stripComment(std::string_view line) {
  const std::size_t hash = line.find('#');
  if (hash == std::string_view::npos) return std::string{line};
  return std::string{line.substr(0u, hash)};
}

std::string commentedTemplateValue(std::string_view line) {
  std::string text = trim(line);
  if (text.empty() || text.front() != '#') return {};
  text.erase(text.begin());
  return trim(text);
}

bool isStrategyParamKey(std::string_view key) noexcept {
  return key != "type" && key != "enabled" && key != "inputs";
}

bool isIniIdentifier(std::string_view key) noexcept {
  if (key.empty()) return false;
  for (char ch : key) {
    const bool valid = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_';
    if (!valid) return false;
  }
  return true;
}

bool isUnsignedIntegerText(std::string_view value) noexcept {
  if (value.empty()) return false;
  for (char ch : value) {
    if (ch < '0' || ch > '9') return false;
  }
  return true;
}

StrategyParamKind inferParamKind(std::string_view key, std::string_view value) noexcept {
  if (key.find("qty") != std::string_view::npos) {
    return key.find("amount") != std::string_view::npos ? StrategyParamKind::DecimalQuoteAmount
                                                        : StrategyParamKind::DecimalAmount;
  }
  if (value.find('.') != std::string_view::npos || value.find('-') != std::string_view::npos) {
    return StrategyParamKind::DecimalQuoteAmount;
  }
  return isUnsignedIntegerText(value) ? StrategyParamKind::Uint : StrategyParamKind::Text;
}

std::size_t commaSeparatedCount(std::string_view text) noexcept {
  std::size_t count = 0u;
  bool hasToken = false;
  for (char ch : text) {
    if (ch == ',') {
      if (hasToken) ++count;
      hasToken = false;
      continue;
    }
    if (std::isspace(static_cast<unsigned char>(ch)) == 0) hasToken = true;
  }
  if (hasToken) ++count;
  return count;
}

bool parseTemplateFile(const std::filesystem::path& path, IniTemplate& out) {
  out = IniTemplate{};
  out.fileName = path.filename().string();
  std::ifstream in(path);
  if (!in) return false;

  std::string section{};
  std::string line{};
  while (std::getline(in, line)) {
    const std::string templateComment = commentedTemplateValue(line);
    const bool isTemplateComment = !templateComment.empty();
    const std::string clean = isTemplateComment ? templateComment : trim(stripComment(line));
    if (clean.empty()) continue;
    if (clean.front() == '[' && clean.back() == ']') {
      section = lower(trim(std::string_view{clean}.substr(1u, clean.size() - 2u)));
      continue;
    }

    const std::size_t eq = clean.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = lower(trim(std::string_view{clean}.substr(0u, eq)));
    const std::string value = trim(std::string_view{clean}.substr(eq + 1u));
    if (section == "strategy") {
      if (!isTemplateComment && key == "type") {
        out.strategyType = value;
      } else if (isIniIdentifier(key) && isStrategyParamKey(key)) {
        out.strategyParams.push_back({key, value});
      }
    } else if (!isTemplateComment && section == "portfolio.recorder" && key == "legs") {
      const std::size_t legCount = commaSeparatedCount(value);
      if (legCount > out.recorderLegCount) out.recorderLegCount = legCount;
    }
  }
  return !out.strategyType.empty();
}

std::vector<IniTemplate> loadTemplates() {
  std::vector<IniTemplate> out{};
  const std::filesystem::path root{HFT_BACKTEST_TRADER_SOURCE_DIR};
  std::error_code ec{};
  std::filesystem::directory_iterator it(root, ec);
  if (ec) return out;
  for (const std::filesystem::directory_entry& entry : it) {
    if (!entry.is_regular_file(ec) || ec) {
      ec.clear();
      continue;
    }
    const std::filesystem::path path = entry.path();
    if (path.extension() != ".ini") continue;
    IniTemplate parsed{};
    if (parseTemplateFile(path, parsed)) out.push_back(std::move(parsed));
  }
  return out;
}

const IniTemplate* templateForStrategy(const std::vector<IniTemplate>& templates, std::string_view strategy) noexcept {
  for (const IniTemplate& candidate : templates) {
    if (candidate.strategyType == strategy) return &candidate;
  }
  return nullptr;
}

const char* keepString(MetadataCache& cache, std::string value) {
  cache.strings.push_back(std::move(value));
  return cache.strings.back().c_str();
}

void appendParam(MetadataCache& cache,
                 StrategyMetadata& metadata,
                 const std::string& key,
                 const std::string& value) {
  if (metadata.paramCount >= kStrategyMetadataMaxParams) return;
  for (std::size_t i = 0u; i < metadata.paramCount; ++i) {
    if (metadata.params[i].key != nullptr && key == metadata.params[i].key) return;
  }
  StrategyParamMetadata& param = metadata.params[metadata.paramCount++];
  param.key = keepString(cache, key);
  param.labelRu = param.key;
  param.descriptionRu = "";
  param.defaultValue = keepString(cache, value);
  param.kind = inferParamKind(key, value);
  if (key == "amount_qty" || key == "order_qty") {
    param.exclusiveGroup = 1u;
    param.defaultActive = key == "amount_qty";
  }
}

void applySizingGroupIfNeeded(StrategyMetadata& metadata) noexcept {
  bool hasAmount = false;
  bool hasOrder = false;
  for (std::size_t i = 0u; i < metadata.paramCount; ++i) {
    if (metadata.params[i].key == nullptr) continue;
    if (std::string_view{metadata.params[i].key} == "amount_qty") hasAmount = true;
    if (std::string_view{metadata.params[i].key} == "order_qty") hasOrder = true;
  }
  if (!hasAmount || !hasOrder || metadata.paramGroupCount >= kStrategyMetadataMaxParamGroups) return;
  metadata.paramGroups[metadata.paramGroupCount++] = StrategyParamGroupMetadata{1u, "sizing_mode"};
}

void applyStrategySpecificMetadata(StrategyMetadata& metadata) noexcept {
  if (metadata.id == nullptr) return;
  const std::string_view id{metadata.id};
  if (id == "spread_maker1and2") {
    metadata.displayNameRu = "Спред-мейкер 1/2";
  } else if (id == "stat_arb_band_ladder") {
    metadata.displayNameRu = "Стат-арбитраж band ladder";
  } else if (id == "trend_probe") {
    metadata.displayNameRu = "Диагностика тренда";
    metadata.indicatorCount = 1u;
    metadata.indicators[0] = StrategyIndicatorMetadata{"trend_score", "Trend score"};
  } else if (id == "toxic_flow_probe") {
    metadata.displayNameRu = "Диагностика toxic flow";
    metadata.indicatorCount = 1u;
    metadata.indicators[0] = StrategyIndicatorMetadata{"toxic_flow", "Toxic flow"};
  } else if (id == "volatility_probe") {
    metadata.displayNameRu = "Диагностика волатильности";
    metadata.indicatorCount = 1u;
    metadata.indicators[0] = StrategyIndicatorMetadata{"volatility_bps", "Volatility"};
  }
}

MetadataCache buildMetadataCache() {
  MetadataCache cache{};
  const std::vector<IniTemplate> templates = loadTemplates();
  const std::size_t count = hft_trader::runtime::strategyDescriptorCount();
  cache.strategies.reserve(count);
  for (std::size_t i = 0u; i < count; ++i) {
    const hft_trader::runtime::StrategyDescriptor* descriptor = hft_trader::runtime::strategyDescriptorAt(i);
    if (descriptor == nullptr || descriptor->name == nullptr || descriptor->name[0] == '\0') continue;

    StrategyMetadata metadata{};
    metadata.id = keepString(cache, descriptor->name);
    metadata.displayNameRu = metadata.id;
    metadata.minSessionCount = 1u;
    metadata.maxSessionCount = 1u;

    const IniTemplate* ini = templateForStrategy(templates, descriptor->name);
    if (ini != nullptr) {
      metadata.configTemplateFile = keepString(cache, ini->fileName);
      const std::size_t legCount = ini->recorderLegCount == 0u ? 1u : ini->recorderLegCount;
      const std::size_t boundedLegCount = legCount > 255u ? 255u : legCount;
      metadata.minSessionCount = static_cast<std::uint8_t>(boundedLegCount);
      metadata.maxSessionCount = static_cast<std::uint8_t>(boundedLegCount);
      for (const auto& [key, value] : ini->strategyParams) appendParam(cache, metadata, key, value);
      applySizingGroupIfNeeded(metadata);
    }

    applyStrategySpecificMetadata(metadata);
    cache.strategies.push_back(metadata);
  }
  return cache;
}

const MetadataCache& metadataCache() noexcept {
  static const MetadataCache cache = [] {
    try {
      return buildMetadataCache();
    } catch (...) {
      return MetadataCache{};
    }
  }();
  return cache;
}

}  // namespace

const StrategyMetadata* findStrategyMetadata(std::string_view id) noexcept {
  const MetadataCache& cache = metadataCache();
  for (const StrategyMetadata& metadata : cache.strategies) {
    if (metadata.id != nullptr && id == std::string_view{metadata.id}) return &metadata;
  }
  return nullptr;
}

const StrategyMetadata* strategyMetadataAt(std::size_t index) noexcept {
  const MetadataCache& cache = metadataCache();
  return index < cache.strategies.size() ? &cache.strategies[index] : nullptr;
}

std::size_t strategyMetadataCount() noexcept {
  return metadataCache().strategies.size();
}

}  // namespace hft_backtest
