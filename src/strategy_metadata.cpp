#include "hft_backtest/strategy_metadata.hpp"

#include "hft_trader/runtime/GeneratedStrategyRegistry.hpp"

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace hft_backtest {
namespace {

#ifndef HFT_BACKTEST_TRADER_SOURCE_DIR
#define HFT_BACKTEST_TRADER_SOURCE_DIR "../hft-trader"
#endif

struct ParamTemplateMetadata {
  std::string key{};
  std::uint8_t exclusiveGroup{0u};
  bool defaultActive{true};
};

struct IniTemplate {
  std::string fileName{};
  std::string strategyType{};
  std::string displayNameRu{};
  std::uint8_t minSessionCount{0u};
  std::uint8_t maxSessionCount{0u};
  std::vector<std::pair<std::string, std::string>> indicators{};
  std::vector<std::pair<std::uint8_t, std::string>> paramGroups{};
  std::vector<ParamTemplateMetadata> paramMetadata{};
  std::vector<std::pair<std::string, std::string>> strategyParams{};
  std::size_t recorderLegCount{0u};
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

std::uint8_t parseSessionCount(std::string_view value) noexcept {
  if (!isUnsignedIntegerText(value)) return 0u;
  std::uint32_t out = 0u;
  for (char ch : value) {
    out = out * 10u + static_cast<std::uint32_t>(ch - '0');
    if (out > 255u) return 255u;
  }
  return static_cast<std::uint8_t>(out);
}

std::pair<std::string, std::string> parseIndicator(std::string_view value) {
  std::size_t sep = value.find(':');
  if (sep == std::string_view::npos) sep = value.find(',');
  std::string id{};
  std::string label{};
  if (sep == std::string_view::npos) {
    id = trim(value);
    label = id;
  } else {
    id = trim(value.substr(0u, sep));
    label = trim(value.substr(sep + 1u));
    if (label.empty()) label = id;
  }
  if (id.empty()) return {};
  return {std::move(id), std::move(label)};
}

bool parseBool(std::string_view value, bool fallback) noexcept {
  if (value == "1" || value == "true" || value == "yes" || value == "on") return true;
  if (value == "0" || value == "false" || value == "no" || value == "off") return false;
  return fallback;
}

std::pair<std::uint8_t, std::string> parseParamGroup(std::string_view value) {
  const std::size_t sep = value.find(':');
  if (sep == std::string_view::npos) return {};
  const std::uint8_t id = parseSessionCount(trim(value.substr(0u, sep)));
  std::string key = trim(value.substr(sep + 1u));
  if (id == 0u || key.empty()) return {};
  return {id, std::move(key)};
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

ParamTemplateMetadata& paramTemplate(IniTemplate& ini, const std::string& key) {
  for (ParamTemplateMetadata& metadata : ini.paramMetadata) {
    if (metadata.key == key) return metadata;
  }
  ini.paramMetadata.push_back(ParamTemplateMetadata{});
  ParamTemplateMetadata& out = ini.paramMetadata.back();
  out.key = key;
  return out;
}

const ParamTemplateMetadata* findParamTemplate(const IniTemplate& ini, const std::string& key) noexcept {
  for (const ParamTemplateMetadata& metadata : ini.paramMetadata) {
    if (metadata.key == key) return &metadata;
  }
  return nullptr;
}

bool parseTemplateFile(const std::filesystem::path& path, std::string fileName, IniTemplate& out) {
  out = IniTemplate{};
  out.fileName = std::move(fileName);
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
    } else if (!isTemplateComment && section == "metadata") {
      if (key == "display_name_ru" || key == "display_name") {
        out.displayNameRu = value;
      } else if (key == "min_sessions") {
        out.minSessionCount = parseSessionCount(value);
      } else if (key == "max_sessions") {
        out.maxSessionCount = parseSessionCount(value);
      } else if (key == "indicator") {
        auto indicator = parseIndicator(value);
        if (!indicator.first.empty()) out.indicators.push_back(std::move(indicator));
      } else if (key == "param_group") {
        auto group = parseParamGroup(value);
        if (group.first != 0u) out.paramGroups.push_back(std::move(group));
      }
    } else if (!isTemplateComment && section.rfind("parameter.", 0u) == 0u) {
      const std::string paramKey = section.substr(std::string_view{"parameter."}.size());
      if (!isIniIdentifier(paramKey)) continue;
      ParamTemplateMetadata& param = paramTemplate(out, paramKey);
      if (key == "exclusive_group") {
        param.exclusiveGroup = parseSessionCount(value);
      } else if (key == "default_active") {
        param.defaultActive = parseBool(lower(value), param.defaultActive);
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
  const auto appendDirectory = [&out](const std::filesystem::path& dir, const std::string& prefix) {
    std::error_code ec{};
    std::filesystem::directory_iterator it(dir, ec);
    if (ec) return;
    for (const std::filesystem::directory_entry& entry : it) {
      if (!entry.is_regular_file(ec) || ec) {
        ec.clear();
        continue;
      }
      const std::filesystem::path path = entry.path();
      if (path.extension() != ".ini") continue;
      IniTemplate parsed{};
      const std::string fileName = prefix.empty() ? path.filename().string() : prefix + "/" + path.filename().string();
      if (parseTemplateFile(path, fileName, parsed)) out.push_back(std::move(parsed));
    }
  };
  appendDirectory(root / "strategy_profiles", "strategy_profiles");
  appendDirectory(root, "");
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
                 const IniTemplate& ini,
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
  const ParamTemplateMetadata* templateParam = findParamTemplate(ini, key);
  if (templateParam != nullptr) {
    param.exclusiveGroup = templateParam->exclusiveGroup;
    param.defaultActive = templateParam->defaultActive;
  }
}

void applyTemplateSessionCounts(const IniTemplate& ini, StrategyMetadata& metadata) noexcept {
  std::uint8_t minCount = ini.minSessionCount;
  std::uint8_t maxCount = ini.maxSessionCount;
  if (minCount == 0u && maxCount == 0u) {
    const std::size_t legCount = ini.recorderLegCount == 0u ? 1u : ini.recorderLegCount;
    const std::size_t boundedLegCount = legCount > 255u ? 255u : legCount;
    minCount = static_cast<std::uint8_t>(boundedLegCount);
    maxCount = minCount;
  } else {
    if (minCount == 0u) minCount = maxCount;
    if (maxCount == 0u) maxCount = minCount;
    if (maxCount < minCount) maxCount = minCount;
  }
  metadata.minSessionCount = minCount == 0u ? 1u : minCount;
  metadata.maxSessionCount = maxCount == 0u ? metadata.minSessionCount : maxCount;
}

void appendIndicators(MetadataCache& cache, StrategyMetadata& metadata, const IniTemplate& ini) {
  for (const auto& [id, label] : ini.indicators) {
    if (metadata.indicatorCount >= kStrategyMetadataMaxIndicators) return;
    metadata.indicators[metadata.indicatorCount++] =
        StrategyIndicatorMetadata{keepString(cache, id), keepString(cache, label)};
  }
}

void appendParamGroups(MetadataCache& cache, StrategyMetadata& metadata, const IniTemplate& ini) {
  for (const auto& [id, key] : ini.paramGroups) {
    if (metadata.paramGroupCount >= kStrategyMetadataMaxParamGroups) return;
    metadata.paramGroups[metadata.paramGroupCount++] = StrategyParamGroupMetadata{id, keepString(cache, key)};
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
      if (!ini->displayNameRu.empty()) metadata.displayNameRu = keepString(cache, ini->displayNameRu);
      applyTemplateSessionCounts(*ini, metadata);
      appendIndicators(cache, metadata, *ini);
      for (const auto& [key, value] : ini->strategyParams) appendParam(cache, metadata, *ini, key, value);
      appendParamGroups(cache, metadata, *ini);
    }

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
