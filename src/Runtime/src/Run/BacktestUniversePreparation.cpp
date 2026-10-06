#include "BacktestUniverse.hpp"
#include "../Config/BacktestRunConfig.hpp"
#include "../Config/Strategy/Internal/BacktestStrategyConfigHelpers.hpp"
#include <algorithm>
#include <set>

namespace hft_backtest::run_detail {
bool materializeUniverseSymbolsFromCorpus(BacktestStrategyConfig& config,
    std::span<const hftrec::corpus::BinaryMarketSource> catalog, std::string& error) {
  if (catalog.empty() || catalog.size()>hftrec::corpus::kBinaryMarketMaximumSources) {
    error="whole market requires a bounded recorded source catalog";return false;
  }
  const auto matches=[](const auto& venue,const auto& source) {
    return venue.exchange.raw==source.venueId && venue.market.raw==source.marketRaw;
  };
  const auto symbolText=[](const auto& source) {
    return std::string_view(source.canonicalSymbol.data(),source.canonicalSymbolBytes);
  };
  std::set<std::pair<std::uint16_t,std::uint8_t>> products;
  for (const auto& venue:config.venues)
    if (!products.emplace(venue.exchange.raw,venue.market.raw).second) {
      error="whole market requires one configured venue policy per exchange/product";return false;
    }
  std::uint32_t quote=0u;
  bool matchedSeed=false;
  for (const auto& source:catalog) {
    if (!hftrec::corpus::validBinaryMarketText(source.canonicalSymbol,source.canonicalSymbolBytes) ||
        std::find(source.canonicalSymbol.begin(),
                  source.canonicalSymbol.begin()+source.canonicalSymbolBytes,'\0')!=
            source.canonicalSymbol.begin()+source.canonicalSymbolBytes) {
      error="recorded source symbol is invalid";return false;
    }
    for (const auto& venue:config.venues) {
      if (!matches(venue,source)) continue;
      for (const auto& seed:venue.symbols) {
        if (std::string_view(seed.data)!=symbolText(source)) continue;
        if (!source.quoteAssetId || (quote && quote!=source.quoteAssetId)) {
          error="configured seed symbols do not prove one common recorded quote asset";return false;
        }
        quote=source.quoteAssetId;matchedSeed=true;
      }
    }
  }
  if (!matchedSeed) {
    for (const auto& source:catalog) {
      if (std::none_of(config.venues.begin(),config.venues.end(),[&](const auto& venue){return matches(venue,source);})) continue;
      if (!source.quoteAssetId || (quote && quote!=source.quoteAssetId)) {
        error="whole market quote is ambiguous; select a recorded seed symbol or explicit sources";return false;
      }
      quote=source.quoteAssetId;
    }
  }
  if (!quote) {error="recording contains no configured market product";return false;}
  // Cold storage has at most one Symbol per catalog row: product policies are
  // unique, and no source outside the proven quote is materialized.
  std::vector<std::vector<Symbol>> expanded(config.venues.size());
  std::size_t total=0u;
  for (const auto& source:catalog) {
    if (source.quoteAssetId!=quote) continue;
    for (std::size_t venue=0u;venue<config.venues.size();++venue) {
      if (!matches(config.venues[venue],source)) continue;
      Symbol symbol{};const std::string text(symbolText(source));
      if (strategy_config::cryptoExchangeRequiresLocalSymbols(config.venues[venue].exchange) &&
          !strategy_config::validLocalCryptoSymbol(text.c_str())) {
        error="recorded source symbol violates the canonical strategy config grammar";return false;
      }
      if (!symbol.copyFrom(text.c_str())) {error="recorded symbol exceeds the canonical config capacity";return false;}
      expanded[venue].push_back(symbol);++total;break;
    }
  }
  if (!total) {error="recording has no symbols in the common quote asset";return false;}
  for (std::size_t venue=0u;venue<config.venues.size();++venue)
    if (!expanded[venue].empty()) config.venues[venue].symbols=std::move(expanded[venue]);
  return true;
}
std::int64_t virtualAccountInitialBalance(const BacktestRunRequest& request,
    const BacktestStrategyVenueConfig& venue, std::size_t sourceIndex) noexcept {
  if (!request.legInitialBalancesE8.empty()) return legInitialBalanceE8(request, sourceIndex);
  return venue.hasInitialBalance ? venue.initialBalanceE8 : request.initialBalanceE8;
}
bool discoverUniverseContexts(const trading_core::StrategyDescriptor& descriptor,
    const BacktestStrategyConfig& config, const std::vector<detail::SessionData>& sources,
    std::vector<UniverseContextSelection>& contexts, std::string& error) {
  contexts.clear();
  std::uint32_t quoteAssetId=0u;
  for (const auto& source : sources) {
    if (source.quoteAssetId==0u || (quoteAssetId!=0u && quoteAssetId!=source.quoteAssetId)) {
      error="one replay run requires one proven common quote asset; currency conversion is unavailable";
      return false;
    }
    quoteAssetId=source.quoteAssetId;
  }
  const auto append = [&](UniverseContextSelection selection) {
    if (selection.sourceIndices.size() < descriptor.minLegs ||
        selection.sourceIndices.size() > descriptor.maxLegs ||
        selection.sourceIndices.size() > trading_core::kStrategyRuntimeMaxLegs) {
      error = "strategy context leg count exceeds its canonical topology bound";
      return false;
    }
    for (auto index : selection.sourceIndices) {
      const auto domain = sources[index].assetDomainRaw == 1u ? trading_core::StrategyDomainMask::Crypto :
          sources[index].assetDomainRaw == 2u ? trading_core::StrategyDomainMask::TradFi : trading_core::StrategyDomainMask::None;
      if (domain == trading_core::StrategyDomainMask::None ?
          descriptor.domainMask != trading_core::StrategyDomainMask::All :
          !trading_core::strategyDomainMaskContains(descriptor.domainMask, domain)) {
        error = "strategy context source has an incompatible asset domain"; return false;
      }
    }
    if (selection.tradeMode == BacktestTradeMode::PrimaryOnly && selection.sourceIndices.size() > 1u &&
        !descriptor.supportsReferenceOnlyLegs) {
      error = "strategy does not support reference-only context legs"; return false;
    }
    contexts.push_back(std::move(selection));
    return true;
  };
  using Topology = trading_core::StrategyUniverseTopology;
  if (descriptor.universeTopology == Topology::SingleSource) {
    for (std::size_t source = 0u; source < sources.size(); ++source)
      if (!append({{source}})) return false;
  } else if (descriptor.universeTopology == Topology::SameCanonicalCrossVenue) {
    std::set<std::uint32_t> visited;
    for (const auto& first : sources) {
      if (!first.canonicalInstrumentId || !visited.insert(first.canonicalInstrumentId).second) continue;
      UniverseContextSelection group;
      for (std::size_t i = 0u; i < sources.size(); ++i)
        if (sources[i].canonicalInstrumentId == first.canonicalInstrumentId) group.sourceIndices.push_back(i);
      const bool distinct = std::any_of(group.sourceIndices.begin(), group.sourceIndices.end(),
          [&](auto i) { return sources[i].exchange != first.exchange; });
      if (!distinct || group.sourceIndices.size() < descriptor.minLegs) continue;
      if (!append(std::move(group))) return false;
    }
  } else if (descriptor.universeTopology == Topology::ExplicitGroup) {
    for (const auto& configured : config.portfolios) {
      UniverseContextSelection group;
      group.primaryLegIndex = configured.primaryLegIndex;
      group.tradeMode = configured.tradeMode == BacktestPortfolioTradeMode::Primary
          ? BacktestTradeMode::PrimaryOnly : BacktestTradeMode::AllLegs;
      bool complete = true;
      for (const auto& leg : configured.legs) {
        const auto venue = std::find_if(config.venues.begin(), config.venues.end(),
            [&](const auto& candidate) { return candidate.name == leg.venue; });
        if (venue == config.venues.end()) { complete = false; break; }
        const auto source = std::find_if(sources.begin(), sources.end(), [&](const auto& candidate) {
          return candidate.symbol == leg.symbol.data &&
              detail::exchangeFromText(candidate.exchange).raw == venue->exchange.raw &&
              detail::marketFromText(candidate.market).raw == venue->market.raw;
        });
        if (source == sources.end()) { complete = false; break; }
        const auto index = static_cast<std::size_t>(source - sources.begin());
        if (std::find(group.sourceIndices.begin(), group.sourceIndices.end(), index) != group.sourceIndices.end()) {
          error = "explicit backtest group repeats the same source"; return false;
        }
        group.sourceIndices.push_back(index);
      }
      if (!complete) continue;
      if (group.primaryLegIndex >= group.sourceIndices.size() || !append(std::move(group))) return false;
    }
  } else {
    error = "unsupported canonical strategy universe topology"; return false;
  }
  if (contexts.empty()) {
    error = "recording folder has no complete configured strategy context"; return false;
  }
  return true;
}
}
