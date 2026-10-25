#include "SimulationUniverse.hpp"
#include <map>
#include <string>
namespace hft_backtest::detail {
Status requireSimulationInstrument(const SimulationReferences& references,
    const hftrec::corpus::BinaryMarketSource& source,bool active,std::string& error) {
  using namespace hft_parser::ipc;
  for(const auto& snapshot:references.snapshots) {
    if(snapshot.product.exchangeRaw!=source.venueId || snapshot.product.marketRaw!=source.marketRaw ||
        snapshot.dataset!=ReferenceDatasetKind::Instruments ||
        snapshot.presence!=hftrec::corpus::RecorderReferencePresence::Present)continue;
    for(std::size_t index=0;index<snapshot.producer.snapshot.totalRows;++index) {
      InstrumentReferenceRecord record;
      if(!snapshot.record(index,record)){error="captured instrument reference is malformed";return Status::CorruptData;}
      if(cxet::StringView::fromCString(record.nativeSymbol.data)!=cxet::StringView{source.nativeSymbol.data(),source.nativeSymbolBytes})continue;
      if(record.hasMetadata)return Status::Ok;
    }
  }
  error="awaiting captured typed instrument reference metadata";
  return active?Status::NotReady:Status::Unsupported;
}
Status discoverSimulationUniverse(const trading_core::StrategyDescriptor& descriptor,
    const SimulationInput& input,std::span<const BacktestCanonicalSource> metadata,
    std::size_t maximumContexts,std::vector<run_detail::UniverseContextSelection>& contexts,std::string& error) {
  using namespace trading_core;
  contexts.clear();
  if(descriptor.minLegs!=2u || descriptor.maxLegs!=2u || metadata.size()!=input.sessions.size()) {
    error="canonical whole-market pair topology is unavailable";return Status::Unsupported;
  }
  using Key=std::pair<std::uint32_t,std::string>;
  std::map<Key,std::vector<std::size_t>> native,position,canonical;
  const auto text=[](const auto& value){const auto view=cxet::StringView::fromCString(value.data);return std::string(view.data(),view.size());};
  const auto now=static_cast<std::uint64_t>(input.sources.front().selection.manifest.replayAnchorReceiveNs);
  const auto quoteAllowed=[&](const auto& row) {
    return !descriptor.runtimePolicy || descriptor.runtimePolicy->requiredQuoteCurrency.empty() ||
        cxet::StringView::fromCString(row.quoteAsset.data)==descriptor.runtimePolicy->requiredQuoteCurrency;
  };
  for(std::size_t index=0;index<metadata.size();++index) {
    const auto& row=metadata[index].instrument;
    if(!quoteAllowed(row) || evaluateStrategyInstrumentEligibility(descriptor.instrumentRequirements,row,now)!=StrategyInstrumentEligibilityReason::None ||
        evaluateStrategyInstrumentEligibility(descriptor.pairRequirements.first,row,now)!=StrategyInstrumentEligibilityReason::None)continue;
    const auto quote=input.sessions[index].quoteAssetId;
    if(row.nativeSymbol.data[0])native[{quote,text(row.nativeSymbol)}].push_back(index);
    if(row.economicFacts.positionIdentity.data[0])position[{quote,text(row.economicFacts.positionIdentity)}].push_back(index);
    canonical[{quote,std::to_string(input.sessions[index].canonicalInstrumentId)}].push_back(index);
  }
  const bool exact=(static_cast<unsigned>(descriptor.pairRequirements.facts)&
      static_cast<unsigned>(StrategyPairRequirement::ExactUnderlyingListing))!=0u;
  if(!exact && descriptor.universeTopology!=StrategyUniverseTopology::SameCanonicalCrossVenue) {
    error="descriptor has no declared bounded whole-market pairing identity";return Status::Unsupported;
  }
  std::uint64_t comparisons=0u;
  const std::uint64_t joinBudget=static_cast<std::uint64_t>(maximumContexts)*16u+metadata.size()*4u;
  for(std::size_t second=0;second<metadata.size();++second) {
    const auto& row=metadata[second].instrument;
    if(!quoteAllowed(row) || evaluateStrategyInstrumentEligibility(descriptor.instrumentRequirements,row,now)!=StrategyInstrumentEligibilityReason::None ||
        evaluateStrategyInstrumentEligibility(descriptor.pairRequirements.second,row,now)!=StrategyInstrumentEligibilityReason::None)continue;
    const auto quote=input.sessions[second].quoteAssetId;
    const std::vector<std::size_t>* bucket=nullptr;
    if(exact) {
      const auto& underlying=row.economicFacts;
      if(underlying.underlyingNativeSymbol.data[0]) {
        const auto found=native.find({quote,text(underlying.underlyingNativeSymbol)});
        if(found!=native.end())bucket=&found->second;
      } else if(underlying.underlyingPositionIdentity.data[0]) {
        const auto found=position.find({quote,text(underlying.underlyingPositionIdentity)});
        if(found!=position.end())bucket=&found->second;
      }
    } else {
      const auto found=canonical.find({quote,std::to_string(input.sessions[second].canonicalInstrumentId)});
      if(found!=canonical.end())bucket=&found->second;
    }
    if(!bucket)continue;
    for(const auto first:*bucket) {
      if(++comparisons>joinBudget){error="bounded canonical eligibility join capacity exhausted";return Status::Unsupported;}
      if(first==second || evaluateStrategyPairEligibility(descriptor.pairRequirements,metadata[first].instrument,row,now)!=StrategyInstrumentEligibilityReason::None)continue;
      if(!exact && input.sessions[first].exchange==input.sessions[second].exchange)continue;
      if(contexts.size()>=maximumContexts){error="whole-market eligible context capacity exhausted";return Status::Unsupported;}
      contexts.push_back({{first,second}});
    }
  }
  if(contexts.empty()){error="recording has no currently eligible canonical strategy pairs";return Status::NotReady;}
  return Status::Ok;
}
}
