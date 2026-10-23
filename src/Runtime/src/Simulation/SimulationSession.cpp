#include "capital/Risk/PortfolioRisk.hpp"
#include "capital/AccountState/AccountState.hpp"
#include "../Run/BacktestUniverse.hpp"
#include "hft_backtest/SimulationSession.hpp"
#include "SimulationInput.hpp"
#include "SimulationOutput.hpp"
#include "SimulationReferences.hpp"
#include "SimulationUniverse.hpp"
#include "../Strategy/Execution/BacktestCanonicalPairSelection.hpp"
#include "../Strategy/Execution/BacktestStrategyIntentLowering.hpp"
#include "trading_core/Strategy/GeneratedStrategyRegistry.hpp"
#include <sstream>
#include "trading_core/Contracts/Numeric/Integer/Bps.hpp"
#include "cxet/Primitives/Buf/CStringPrimitives.hpp"
#include "../Run/Internal/BacktestRunContext.hpp"
#include "../Run/Internal/BacktestRunHelpers.hpp"
#include "../Input/Binary/Internal/BinaryCorpusHelpers.hpp"
#include "../Accounting/BacktestResultAggregation.hpp"
#include "../Engine/BacktestEngineSetup.hpp"
#include "../Config/BacktestRunConfig.hpp"
#include "../Strategy/Diagnostics/BacktestStrategyTrace.hpp"
#include "../Strategy/Inputs/BacktestStrategyWarmup.hpp"
#include "../Strategy/Inputs/BacktestStrategyInstrument.hpp"
#include "../Strategy/Internal/BacktestStrategyRuntimeHelpers.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <set>
#include <map>
#include <tuple>


namespace hft_backtest {
using namespace run_detail;
namespace {
struct UniverseState {
  alignas(trading_core::kStrategyRuntimeStateAlign)
      std::array<unsigned char, trading_core::kStrategyRuntimeStateBytes> state{};
  std::array<detail::EngineState*, trading_core::kStrategyRuntimeMaxLegs> routes{};
  std::vector<StrategyInstrumentSpec> instruments;
  std::unique_ptr<MultiLegCycleScratch> scratch;
  trading_core::SymbolShardConfig shard;
  trading_core::ShardControlState control;
  BacktestStrategyInputPolicy inputPolicy;
  BacktestRunRequest request;
  std::size_t count{};
  std::uint16_t requiredCaptureMask{};
  std::uint64_t metadataVisibleMono{};
  trading_core::DeliveryValuationReference delivery;
  detail::SimulationCentralBank centralBank;
  BacktestCanonicalPairReferences references;

};
const BacktestStrategyVenueConfig* configuredVenue(const BacktestStrategyConfig& config,
    const detail::SessionData& source) {
  for (const auto& venue : config.venues) {
    if (venue.exchange.raw != detail::exchangeFromText(source.exchange).raw ||
        venue.market.raw != detail::marketFromText(source.market).raw) continue;
    for (const auto& symbol : venue.symbols)
      if (source.symbol == symbol.data) return &venue;
  }
  return nullptr;
}

} // namespace
struct SimulationSession::Impl {
  BacktestRunRequest request;
  BacktestRunResult result;
  BacktestStrategyConfig config;
  detail::SessionLoadPlan plan;
  detail::SimulationInput input;
  const trading_core::StrategyDescriptor* strategy{};
  std::vector<UniverseContextSelection> selectedContexts;
  std::vector<detail::EngineState> engines;
  std::vector<StrategyInstrumentSpec> instruments;
  std::vector<std::size_t> accountIndices, accountSources;
  std::vector<SimulationAccountSummary> accountSummaries;
  std::vector<std::unique_ptr<detail::SharedAccountState>> accounts;
  std::vector<std::unique_ptr<UniverseState>> contexts;
  std::vector<std::vector<std::size_t>> subscribers;
  std::array<capital::accounting::StrategyAccountState,trading_core::kStrategyRuntimeMaxLegs> riskAccounts{};
  std::array<capital::risk::PortfolioRiskLegConfig,trading_core::kStrategyRuntimeMaxLegs> riskConfigs{};
  capital::risk::PortfolioRiskState risk{};
  SimulationOutputSink sink;
  SimulationLimits limits;
  std::size_t residentSourceCapacity{};
  std::uint64_t preparedSourceBytes{},preparedContextBytes{};
  std::unique_ptr<BacktestCanonicalPairSelection> selection;
  StrategyIndicatorProfileInfo indicator;
  detail::SimulationReferences capturedReferences;
  std::vector<BacktestCanonicalPairSource> canonicalSources;
  std::vector<std::uint64_t> metadataVisibleMono;
  std::uint64_t referenceGeneration{},referenceBytes{},producerEpoch{},anchorMonotonic{};
  std::int64_t anchorRealtime{},authorityReplay{-1};
  std::uint64_t authorityReferenceGeneration{};

  std::string error;
  std::int64_t initialBalance{}, schedulerFloor{}, now{};
  std::uint64_t safeMonotonicNs{}, lastRecordMonotonicNs{};
  bool paused{}, finished{},activeInput{},wholeMarketPairs{};
  std::int64_t pnlPeak{},drawdown{},mfe{},mae{};
  std::vector<detail::TimelineEvent> transaction;
  void reject(Status status,std::string message) {result.status=status;result.error=std::move(message);}
  Status prepare(const BacktestRunRequest& requested,const hftrec::corpus::CorpusCut& cut,SimulationOutputSink output,SimulationLimits bounds) {
    request=requested;sink=output;limits=bounds;request.writeArtifacts=false;
    activeInput=!cut.finalized && !cut.manifest.complete;
    if(request.makerFeeBpsE8<0 || request.takerFeeBpsE8<0 ||
        std::any_of(request.feeSchedules.begin(),request.feeSchedules.end(),[](const auto& fee) {
          return fee.makerFeeBpsE8<0 || fee.takerFeeBpsE8<0;
        })) {
      reject(Status::Unsupported,"negative maker/taker fees require an unsupported rebate accounting model");return result.status;
    }
    if(request.virtualHost.marginBorrowAllowed) {
      reject(Status::Unsupported,"simulation has no captured borrowing or credit authority");return result.status;
    }
    if(!cut.progress.safeMonotonicNs || !cut.manifest.replayAnchorMonotonicNs || cut.manifest.replayAnchorReceiveNs<=0 ||
        !cut.manifest.producerEpoch || (cut.progress.producerEpoch && cut.progress.producerEpoch!=cut.manifest.producerEpoch)) {
      reject(activeInput?Status::NotReady:Status::Unsupported,"simulation awaits an authoritative prefix with a frozen captured clock anchor");return result.status;
    }
    producerEpoch=cut.manifest.producerEpoch;anchorMonotonic=cut.manifest.replayAnchorMonotonicNs;
    anchorRealtime=cut.manifest.replayAnchorReceiveNs;
    if(!limits.maximumSources || !limits.maximumContexts || limits.maximumSources>hftrec::corpus::kBinaryMarketMaximumSources) {
      reject(Status::InvalidArgument,"invalid declared simulation capacities");return result.status;
    }
    if(!request.latencySeed)request.latencySeed=hashRunSeed(request.runId.empty()?"simulation":request.runId);
    if(cut.referencePayload.size()>limits.maximumStateBytes) {
      reject(Status::Unsupported,"captured reference history exceeds simulation byte budget");return result.status;
    }
    referenceBytes=cut.referencePayload.size();
    const auto refStatus=capturedReferences.update(cut,error);
    if(!isOk(refStatus)){reject(refStatus,error);return result.status;}
    referenceGeneration=cut.generation;
    result.runId=request.runId.empty()?makeRunId():request.runId;
    result.requestId=request.requestId.empty()?result.runId:request.requestId;
    result.sessionPath=request.corpusPath;result.configPath=request.configPath;
    result.resultPath=resultPathFor(request,result.runId);
    if (!loadBacktestStrategyConfig(request.configPath.string().c_str(),config,error)) {
      reject(Status::InvalidArgument,error);return result.status;
    }
    const auto& descriptorRef=trading_core::findStrategyDescriptor(config.strategyType);
    strategy=descriptorRef;
    if (!strategy || !strategy->resolveParams || !strategy->initState || !strategy->runCycle ||
        !strategy_intent_lowering::footprintSupported(*strategy)) {
      reject(Status::Unsupported,"strategy has no canonical simulation adapter");return result.status;
    }
    const auto& descriptor=*strategy;
    std::vector<BacktestRateLimitSchedule> explicitSchedules=std::move(request.rateLimitSchedules);
    for(const auto& venue:config.venues) {
      const auto separator=venue.name.rfind('_');
      request.rateLimitSchedules.push_back({venue.name.substr(0,separator),venue.name.substr(separator+1),
          venue.rateLimitBuckets,venue.rateLimitActions,venue.rateLimitsEnabled});
    }
    request.rateLimitSchedules.insert(request.rateLimitSchedules.end(),explicitSchedules.begin(),explicitSchedules.end());
    result.strategy=request.strategy.empty()?config.strategyType:request.strategy;
    if (result.strategy!=config.strategyType) {reject(Status::ConfigMismatch,"strategy does not match config");return result.status;}
    if (!buildSessionLoadPlan(descriptor,plan,error)) {reject(Status::Unsupported,error);return result.status;}
    wholeMarketPairs=request.corpusSources.empty() && config.portfolios.empty() &&
        descriptor.stateDiscipline==trading_core::StrategyStateDiscipline::CanonicalEvidence;
    if(request.corpusSources.empty()) {
      const auto status=materializeSymbols(cut);
      if(!isOk(status)){reject(status,error);return result.status;}
    }
    std::vector<BacktestCorpusSourceRequest> selections;
    for (const auto& captured:cut.sources) {
      detail::SessionData meta;
      meta.exchange=detail::binary_corpus::fixedText(captured.venue.data(),captured.venue.size(),captured.venueBytes);
      meta.market=detail::binary_corpus::fixedText(captured.market.data(),captured.market.size(),captured.marketBytes);
      meta.symbol=detail::binary_corpus::fixedText(captured.canonicalSymbol.data(),captured.canonicalSymbol.size(),captured.canonicalSymbolBytes);
      if(!configuredVenue(config,meta)) continue;
      if (!request.corpusSources.empty() && std::none_of(request.corpusSources.begin(),request.corpusSources.end(),[&](const auto& selected){
        return selected.exchange==meta.exchange && selected.market==meta.market && selected.canonicalSymbol==meta.symbol;
      })) continue;
      selections.push_back({meta.exchange,meta.market,meta.symbol,0,0});
    }
    if(selections.empty()) {reject(activeInput?Status::NotReady:Status::ConfigMismatch,"no configured captured sources");return result.status;}
    if(wholeMarketPairs) {
      const auto status=selectWholeMarketSources(cut,selections);if(!isOk(status)){reject(status,error);return result.status;}
    }
    const auto inputStatus=input.prepare(cut,selections,plan,error);
    if(!isOk(inputStatus)) {reject(inputStatus,error);return result.status;}
    wholeMarketPairs=request.corpusSources.empty() && config.portfolios.empty() &&
        descriptor.stateDiscipline==trading_core::StrategyStateDiscipline::CanonicalEvidence;
    if(wholeMarketPairs)for(const auto& source:input.sources) {
      const auto authority=detail::requireSimulationInstrument(capturedReferences,source.selection.source,activeInput,error);
      if(!isOk(authority)){reject(authority,error);return result.status;}
    }
    if(wholeMarketPairs) {
      canonicalSources.resize(input.sessions.size());metadataVisibleMono.resize(input.sessions.size());
      for(std::size_t source=0;source<input.sessions.size();++source) {
        StrategyInstrumentSpec spec;
        const auto status=prepareBacktestStrategyInstrument(input.sessions[source].instrument,spec,error);
        if(!isOk(status) || !capturedReferences.instrument(input.sources[source].selection.source,spec,
            canonicalSources[source],metadataVisibleMono[source],error)) {
          reject(isOk(status)?Status::Unsupported:status,error);return result.status;
        }
      }
      const auto discovered=detail::discoverSimulationUniverse(descriptor,input,canonicalSources,limits.maximumContexts,selectedContexts,error);
      if(!isOk(discovered)){reject(discovered==Status::NotReady && !activeInput?Status::Unsupported:discovered,error);return result.status;}
      std::vector<unsigned char> used(input.sessions.size());
      for(const auto& context:selectedContexts)for(auto source:context.sourceIndices)used[source]=1u;
      std::vector<std::size_t> remap(used.size());std::vector<BacktestCorpusSourceRequest> participating;
      for(std::size_t source=0;source<used.size();++source)if(used[source]) {
        remap[source]=participating.size();const auto& session=input.sessions[source];
        participating.push_back({session.exchange,session.market,session.symbol,0,0});
      }
      for(auto& context:selectedContexts)for(auto& source:context.sourceIndices)source=remap[source];
      detail::SimulationInput filtered;
      const auto status=filtered.prepare(cut,participating,plan,error);
      if(!isOk(status)){reject(status,error);return result.status;}
      input=std::move(filtered);
    } else if(!discoverUniverseContexts(descriptor,config,input.sessions,selectedContexts,error)) {
      reject(activeInput?Status::NotReady:Status::ConfigMismatch,error);return result.status;
    }
    // Explicit groups admit only their actual member sources/accounts too.
    if(!wholeMarketPairs) {
      std::vector<unsigned char> used(input.sessions.size());
      for(const auto& context:selectedContexts)for(auto source:context.sourceIndices)used[source]=1u;
      std::vector<std::size_t> remap(used.size());std::vector<BacktestCorpusSourceRequest> participating;
      for(std::size_t source=0;source<used.size();++source)if(used[source]) {
        remap[source]=participating.size();const auto& session=input.sessions[source];
        participating.push_back({session.exchange,session.market,session.symbol,0,0});
      }
      if(participating.size()!=input.sessions.size()) {
        for(auto& context:selectedContexts)for(auto& source:context.sourceIndices)source=remap[source];
        detail::SimulationInput filtered;const auto status=filtered.prepare(cut,participating,plan,error);
        if(!isOk(status)){reject(status,error);return result.status;}input=std::move(filtered);
      }
    }
    const auto& sources=input.sessions;
    constexpr std::uint64_t maximumEnvelope=3ull*1024u*1024u*1024u;
    if(!limits.maximumStateBytes || limits.maximumStateBytes>maximumEnvelope) {
      reject(Status::InvalidArgument,"invalid simulation resident state byte budget");return result.status;
    }
    preparedSourceBytes=sizeof(detail::EngineState)+sizeof(detail::SimulationOrderEvidenceOwner)+
        sizeof(detail::PendingAction)*detail::kMaxOrderStates*4u+
        (sizeof(cxet::composite::UserDataEvent)+sizeof(detail::JsonFill)+sizeof(detail::JsonEquityPoint)+
         sizeof(detail::JsonRiskSnapshot)+sizeof(detail::JsonStrategyIndicatorPoint)+sizeof(detail::JsonStrategyRangePoint)+
         sizeof(detail::JsonStrategySpreadPoint)+sizeof(detail::JsonExecutionQualityAction)+sizeof(detail::JsonRateLimitUsagePoint)+
         4u*sizeof(std::uint64_t)+sizeof(std::uint32_t)+2u)*detail::kMaxOrderStates*16u+
        sizeof(detail::JsonOrder)*detail::kMaxOrderStates*8u;
    preparedContextBytes=sizeof(UniverseState)+sizeof(MultiLegCycleScratch)+sizeof(BacktestCanonicalPairAdapter);
    auto prototype=std::make_unique<detail::EngineState>();
    if(!prepareEngineOrderBookStorage(*prototype,&descriptor,plan)) {reject(Status::IoError,"simulation depth memory planning failed");return result.status;}
    preparedSourceBytes+=prototype->orderBook.storageBytes()+prototype->venueOrderBook.storageBytes()+prototype->executionLiquidity.storageBytes();
    // Descriptor context storage is allocated as topology grows. The raw engine
    // vector reserves a bounded address-stable bank before actors borrow it.
    if(selectedContexts.size()>limits.maximumContexts ||
        selectedContexts.size()>limits.maximumStateBytes/preparedContextBytes) {
      reject(Status::Unsupported,"simulation context state exceeds declared byte budget");return result.status;
    }
    const auto nonResidentSourceBytes=preparedSourceBytes-sizeof(detail::EngineState);
    const auto actualOther=referenceBytes+selectedContexts.size()*preparedContextBytes+sources.size()*nonResidentSourceBytes;
    if(actualOther>=limits.maximumStateBytes) {reject(Status::Unsupported,"simulation bounded queues exceed state byte budget");return result.status;}
    const auto remaining=limits.maximumStateBytes-actualOther;
    residentSourceCapacity=std::min<std::size_t>(limits.maximumSources,remaining/preparedSourceBytes);
    if(sources.size()>residentSourceCapacity) {reject(Status::Unsupported,"simulation source state exceeds declared byte budget");return result.status;}
    engines.reserve(residentSourceCapacity);instruments.reserve(residentSourceCapacity);
    canonicalSources.reserve(residentSourceCapacity);metadataVisibleMono.reserve(residentSourceCapacity);
    engines.resize(sources.size());instruments.resize(sources.size());
    canonicalSources.resize(sources.size());metadataVisibleMono.resize(sources.size());
    accountIndices.resize(sources.size());subscribers.resize(sources.size());
    if(!resolveStrategyIndicatorProfile(request.indicatorProfile,indicator)) {reject(Status::InvalidArgument,"unknown indicator profile");return result.status;}
    for (std::size_t i=0;i<sources.size();++i) if(!isOk(initializeSource(i)))return result.status;
    if (accounts.size() > trading_core::kStrategyRuntimeMaxLegs) {
      reject(Status::Unsupported, "virtual account risk domain exceeds canonical account bound"); return result.status;
    }
    for (std::size_t i=0;i<selectedContexts.size();++i) if(!isOk(initializeContext(i)))return result.status;
    if(strategy->runtimePolicy && strategy->runtimePolicy->selection.enabled()) {
      selection=std::make_unique<BacktestCanonicalPairSelection>();
      std::vector<trading_core::StrategySelectionIdentity> identities;
      for(const auto& selected:selectedContexts) {
        const auto source=selected.sourceIndices.back();
        const auto& facts=canonicalSources[source].instrument;
        identities.push_back({facts.expiryRealtimeNs,cxet::StringView::fromCString(facts.nativeSymbol.data)});
      }
      if(!selection->bindCold(*strategy->runtimePolicy,identities,contexts.front()->inputPolicy.maxAgeNs,error)) {
        reject(Status::Unsupported,error);return result.status;
      }
      for(std::size_t i=0;i<contexts.size();++i)if(contexts[i]->scratch->canonicalPair &&
          !contexts[i]->scratch->canonicalPair->bindSelectionOwner(*selection,static_cast<std::uint32_t>(i))) {
        reject(Status::Unsupported,"canonical selection context binding failed");return result.status;
      }
    }
    refreshAccounts();
    for (std::size_t a = 0; a < accounts.size(); ++a)
      riskConfigs[a] = makeBacktestPortfolioRiskLegConfig(configuredVenue(config, sources[accountSources[a]]));
    risk = capital::risk::makePortfolioRiskState(makeBacktestPortfolioRiskConfig(config.risk),
        riskAccounts.data(), accounts.size(), riskConfigs.data());

    result.status=Status::Ok;result.error.clear();
    result.universeSources=sources.size();result.universeContexts=contexts.size();result.virtualAccounts=accounts.size();
    result.initialBalanceE8=result.walletBalanceE8=initialBalance;
    result.replayClock.venueSchedule="captured_arrival_no_future_lookahead";
    summarize();return result.status;
  }
  Status selectWholeMarketSources(const hftrec::corpus::CorpusCut& cut,std::vector<BacktestCorpusSourceRequest>& selections) {
    detail::SimulationInput candidates;std::vector<BacktestCanonicalPairSource> metadata;
    std::vector<std::size_t> selectionIndices;
    using CatalogKey=std::tuple<std::string,std::string,std::string>;
    std::map<CatalogKey,const hftrec::corpus::BinaryMarketSource*> catalog;
    for(const auto& source:cut.sources)
      catalog.emplace(CatalogKey{
          std::string(detail::binary_corpus::fixedText(source.venue.data(),source.venue.size(),source.venueBytes)),
          std::string(detail::binary_corpus::fixedText(source.market.data(),source.market.size(),source.marketBytes)),
          std::string(detail::binary_corpus::fixedText(source.canonicalSymbol.data(),source.canonicalSymbol.size(),source.canonicalSymbolBytes))},&source);
    for(std::size_t index=0;index<selections.size();++index) {
      const auto& selected=selections[index];
      const auto entry=catalog.find({selected.exchange,selected.market,selected.canonicalSymbol});
      if(entry==catalog.end()){error="whole-market source disappeared from immutable catalog";return Status::CorruptData;}
      const auto* found=entry->second;
      BacktestCanonicalPairSource row;
      const auto status=capturedReferences.listing(*found,row.instrument,activeInput,error);
      if(!isOk(status))return status;
      auto& source=candidates.sources.emplace_back();source.selection.source=*found;source.selection.manifest=cut.manifest;
      auto& session=candidates.sessions.emplace_back();session.exchange=selected.exchange;session.market=selected.market;
      session.symbol=selected.canonicalSymbol;session.quoteAssetId=found->quoteAssetId;session.canonicalInstrumentId=found->canonicalSymbolId;
      metadata.push_back(row);selectionIndices.push_back(index);
    }
    std::vector<UniverseContextSelection> eligible;
    const auto status=detail::discoverSimulationUniverse(*strategy,candidates,metadata,limits.maximumContexts,eligible,error);
    if(!isOk(status))return status==Status::NotReady && !activeInput?Status::Unsupported:status;
    std::vector<unsigned char> used(metadata.size());for(const auto& context:eligible)for(auto source:context.sourceIndices)used[source]=1u;
    std::vector<BacktestCorpusSourceRequest> admitted;
    for(std::size_t source=0;source<used.size();++source)if(used[source])admitted.push_back(selections[selectionIndices[source]]);
    selections=std::move(admitted);return Status::Ok;
  }
  Status materializeSymbols(const hftrec::corpus::CorpusCut& cut) {
    if(!wholeMarketPairs || !strategy->runtimePolicy || strategy->runtimePolicy->requiredQuoteCurrency.empty())
      return materializeUniverseSymbolsFromCorpus(config,cut.sources,error)?Status::Ok:Status::ConfigMismatch;
    using Key=std::tuple<std::uint16_t,std::uint8_t,std::string>;
    std::map<Key,std::string> currencies;
    for(const auto& snapshot:capturedReferences.snapshots) {
      if(snapshot.dataset!=hft_parser::ipc::ReferenceDatasetKind::Instruments ||
          snapshot.presence!=hftrec::corpus::RecorderReferencePresence::Present)continue;
      for(std::size_t index=0;index<snapshot.producer.snapshot.totalRows;++index) {
        hft_parser::ipc::InstrumentReferenceRecord row;
        if(!snapshot.record(index,row)){error="captured instrument currency metadata malformed";return Status::CorruptData;}
        const auto native=cxet::StringView::fromCString(row.nativeSymbol.data);
        const auto quote=cxet::StringView::fromCString(row.instrument.quoteAsset.data);
        currencies.try_emplace({snapshot.product.exchangeRaw,snapshot.product.marketRaw,std::string(native.data(),native.size())},
            std::string(quote.data(),quote.size()));
      }
    }
    std::uint32_t quoteId=0u;
    for(const auto& source:cut.sources) {
      if(std::none_of(config.venues.begin(),config.venues.end(),[&](const auto& venue){
          return venue.exchange.raw==source.venueId && venue.market.raw==source.marketRaw;
      }))continue;
      const auto native=detail::binary_corpus::fixedText(source.nativeSymbol.data(),source.nativeSymbol.size(),source.nativeSymbolBytes);
      const auto found=currencies.find({source.venueId,source.marketRaw,std::string(native)});
      if(found==currencies.end() || cxet::StringView{found->second.data(),found->second.size()}!=strategy->runtimePolicy->requiredQuoteCurrency)continue;
      if(!source.quoteAssetId || (quoteId && quoteId!=source.quoteAssetId)) {
        error="captured native currency maps to conflicting normalized quote identities";return Status::CorruptData;
      }
      quoteId=source.quoteAssetId;
    }
    if(!quoteId){error="awaiting captured instruments in the strategy declared quote currency";return activeInput?Status::NotReady:Status::Unsupported;}
    std::vector<hftrec::corpus::BinaryMarketSource> eligible;
    for(const auto& source:cut.sources)if(source.quoteAssetId==quoteId)eligible.push_back(source);
    return materializeUniverseSymbolsFromCorpus(config,eligible,error)?Status::Ok:Status::ConfigMismatch;
  }
  Status initializeSource(std::size_t i) {
    const auto& sources=input.sessions;const auto& descriptor=*strategy;

      const auto& source = sources[i];
      const auto* venue = configuredVenue(config, source);
      if (!venue) { reject(Status::ConfigMismatch, "selected source is outside configured strategy universe"); return result.status; }
      auto& engine = engines[i];
      engine.streamingOutput = true;
      auto& instrument = instruments[i];
      const auto authority=detail::requireSimulationInstrument(capturedReferences,input.sources[i].selection.source,activeInput,error);
      if(!isOk(authority)){reject(authority,error);return result.status;}
      const auto preparedInstrument = prepareBacktestStrategyInstrument(source.instrument, instrument, error);
      if (!isOk(preparedInstrument)) { reject(preparedInstrument, error); return result.status; }
      {
        if(!capturedReferences.instrument(input.sources[i].selection.source,instrument,canonicalSources[i],metadataVisibleMono[i],error)) {
          reject(Status::Unsupported,error);return result.status;
        }
      }
      const auto& facts=canonicalSources[i].instrument;
      const bool cash=facts.kind==cxet::api::InstrumentKind::Spot;
      const bool linear=(facts.kind==cxet::api::InstrumentKind::Delivery || facts.kind==cxet::api::InstrumentKind::Perpetual) &&
          facts.quoteAsset.data[0] && cxet::StringView::fromCString(facts.quoteAsset.data)==cxet::StringView::fromCString(facts.settleAsset.data) &&
          instrument.contractBaseQtyRaw>0;
      if(!cash && !linear) {reject(Status::Unsupported,"captured economic quantity/settlement has no supported accounting model");return result.status;}
      engine.accountingModel=cash?detail::SimulationAccountingModel::Cash:detail::SimulationAccountingModel::Linear;
      engine.capturedSourceManaged = true;
      engine.capturedSourceMember = source.capturedInitiallyPresent;
      engine.capturedSourceGeneration = source.capturedSourceGeneration;
      engine.capturedDirectoryRevision = source.capturedDirectoryRevision;
      engine.capturedUnhealthyChannels = source.capturedUnhealthyChannels;
      engine.capturedActiveChannels = source.capturedInitiallyPresent ? source.capturedAvailableChannels : 0u;
      engine.exchange = detail::exchangeFromText(source.exchange);
      engine.market = detail::marketFromText(source.market);
      engine.symbol.copyFrom(source.symbol.c_str());
      engine.legIndex = static_cast<std::uint32_t>(i);
      engine.tickSizeE8 = instrument.tickSizeRaw;
      engine.makerFillMinCrossTicks = request.makerFillMinCrossTicks;
      if (!prepareEngineOrderBookStorage(engine, &descriptor, plan)) {
        reject(Status::IoError, "failed to prepare source depth owner"); return result.status;
      }
      engine.venueOrderBookExecutionEnabled = plan.loadDepths;
      if (plan.loadDepths) engine.depthExecutionMode = BacktestDepthExecutionMode::Depth;
      (void)trading_core::configureBboReconstruction(engine.bboReconstruction, Price{instrument.tickSizeRaw});
      (void)trading_core::configureBboReconstruction(engine.venueBboReconstruction, Price{instrument.tickSizeRaw});
      configureStrategyIndicator(engine, indicator);
      detail::initLatencyRuntime(engine.latency, resolveLatencyConfig(request, source.exchange, source.market));
      auto zeroBalance = request;
      zeroBalance.initialBalanceE8 = 0;
      detail::initializeAccounting(engine, zeroBalance);
      auto fee=resolveFeeSchedule(request,source.exchange,source.market);
      const bool exactFee=(request.hasMakerFeeBps && request.hasTakerFeeBps) ||
          std::any_of(request.feeSchedules.begin(),request.feeSchedules.end(),[&](const auto& row){
            return row.exchange==source.exchange && row.market==source.market;
          });
      if(!exactFee) {reject(Status::Unsupported,"virtual execution requires an explicit maker/taker fee policy");return result.status;}
      if(fee.makerFeeBpsE8<0 || fee.takerFeeBpsE8<0) {
        reject(Status::Unsupported,"negative maker/taker fees require an unsupported rebate accounting model");return result.status;
      }
      canonicalSources[i].feesKnown=exactFee;engine.feePolicyKnown=exactFee;
      engine.makerFeeBpsE8 = fee.makerFeeBpsE8;
      engine.takerFeeBpsE8 = fee.takerFeeBpsE8;
      std::size_t account = accounts.size();
      for (std::size_t a = 0; a < accountSources.size(); ++a) {
        const auto owner = accountSources[a];
        if (engines[owner].exchange.raw == engine.exchange.raw &&
            engines[owner].market.raw == engine.market.raw) { account = a; break; }
      }
      if(!source.quoteAssetId || (!accounts.empty() && accounts.front()->quoteAssetId!=source.quoteAssetId)) {
        reject(Status::Unsupported,"admitted virtual accounts use different quote assets; no captured valuation conversion model");return result.status;
      }
      const auto balance = virtualAccountInitialBalance(request, *venue, i);
      if (account == accounts.size()) {
        accounts.push_back(std::make_unique<detail::SharedAccountState>());
        accountSources.push_back(i);
        accounts.back()->quoteAssetId = source.quoteAssetId;
        accounts.back()->accountingModel=engine.accountingModel;
        accounts.back()->initialBalanceE8 = accounts.back()->walletBalanceE8 = balance;
        accounts.back()->observedWalletBalanceE8 = balance;
        accounts.back()->observedInitialized = true;
        engine.initialBalanceE8 = engine.walletBalanceE8 = balance;
        engine.observedWalletBalanceE8 = balance;
        initialBalance = trading_core::saturatingAddI64(initialBalance, balance);
        const auto policy = resolveRateLimitSchedule(request, source.exchange, source.market);
        if (!accounts.back()->quotas.configure(policy, error)) { reject(Status::InvalidArgument, error); return result.status; }
        result.rateLimitSchedules.push_back(policy);
        result.rateLimitsEnabled=result.rateLimitsEnabled || policy.enabled;
        result.strictRateLimitRejects=request.strictRateLimitRejects && result.rateLimitsEnabled;

      } else if (!request.legInitialBalancesE8.empty() && accounts[account]->initialBalanceE8 != balance) {
        reject(Status::InvalidArgument, "sources on one virtual account disagree on initial balance"); return result.status;
      }
      if(accounts[account]->accountingModel!=engine.accountingModel) {
        reject(Status::Unsupported,"one virtual account mixes incompatible captured accounting models");return result.status;
      }
      if (source.quoteAssetId == 0u || accounts[account]->quoteAssetId != source.quoteAssetId) {
        reject(Status::Unsupported, "virtual account requires one exact captured quote asset"); return result.status;
      }
      accountIndices[i] = account;
      engine.sharedAccount = accounts[account].get();
      const auto& capital=canonicalSources[i].sizing;
      if(capital.capitalRequirementKnown && !detail::configureSimulationDerivativeCollateral(engine,
          capital.initialCapitalRaw,capital.capitalBasisQtyRaw,capital.capitalCurrency)) {
        reject(Status::Unsupported,"captured derivative initial margin authority rejected");return result.status;
      }

    return Status::Ok;
  }
  Status initializeContext(std::size_t i) {
    const auto& sources=input.sessions;const auto& descriptor=*strategy;

      const auto& selected = selectedContexts[i];
      auto context = std::make_unique<UniverseState>();
      context->count = selected.sourceIndices.size();
      context->scratch = std::make_unique<MultiLegCycleScratch>();
      context->scratch->reset(context->count);
      // Only cycle policy is copied; folder selections and schedules stay
      // with their single cold owner rather than growing once per context.
      context->request.marketOrderLatency = request.marketOrderLatency;
      context->request.limitOrderLatency = request.limitOrderLatency;
      context->request.cancelOrderLatency = request.cancelOrderLatency;
      context->request.userDataLatency = request.userDataLatency;
      context->request.orderLatencyUs = request.orderLatencyUs;
      context->request.cancelLatencyUs = request.cancelLatencyUs;
      context->request.rateLimitsEnabled = request.rateLimitsEnabled;
      context->request.strictRateLimitRejects = request.strictRateLimitRejects;
      context->request.captureStrategySpread = request.captureStrategySpread;
      context->request.tradeMode = selected.tradeMode;
      context->request.primaryLegIndex = request.hasPrimaryLegIndex ?
          request.primaryLegIndex : selected.primaryLegIndex;
      if (request.hasPrimaryLegIndex) context->request.tradeMode = request.tradeMode;
      if (context->request.primaryLegIndex >= context->count) {
        reject(Status::InvalidArgument, "primary leg index is outside one strategy context"); return result.status;
      }
      context->request.hasPrimaryLegIndex = true;
      context->shard.shardId.raw = static_cast<std::uint32_t>(i + 1);
      context->shard.strategyId.raw = 1;
      context->shard.symbolId.raw = static_cast<std::uint32_t>(selected.sourceIndices[0] + 1);
      context->shard.symbol = engines[selected.sourceIndices[0]].symbol;
      context->control.shardId = context->shard.shardId;
      for (auto source : selected.sourceIndices) {
        context->routes[context->instruments.size()] = &engines[source];
        context->instruments.push_back(instruments[source]);
        subscribers[source].push_back(i);
      }
      trading_core::StrategyParamStorage params;
      const auto* venue = configuredVenue(config, sources[selected.sourceIndices[0]]);
      if (!descriptor.resolveParams(makeBacktestStrategyParamContext(config, *venue), params, error)) {
        reject(Status::InvalidArgument, error); return result.status;
      }
      context->inputPolicy = resolveBacktestStrategyInputPolicy(descriptor, params);
      if (plan.requireTrades) context->requiredCaptureMask |= hftrec::corpus::binaryMarketChannelBit(hftrec::corpus::BinaryMarketChannel::Trade);
      if (plan.requireBookTickers) context->requiredCaptureMask |= hftrec::corpus::binaryMarketChannelBit(hftrec::corpus::BinaryMarketChannel::BookTicker);
      if (plan.requireDepths) context->requiredCaptureMask |= hftrec::corpus::binaryMarketChannelBit(hftrec::corpus::BinaryMarketChannel::Depth);
      for (auto source : selected.sourceIndices)
        engines[source].capturedExecutionRequiredChannels |= context->requiredCaptureMask;
      if (!descriptor.initState(context->state.data(), params, context->instruments[0]) ||
          !seedTradeWarmupFromSession(descriptor, context->state.data(), params, sources[selected.sourceIndices[0]], error)) {
        reject(Status::Unsupported, error.empty() ? "strategy has unsupported binary warmup" : error); return result.status;
      }
      if(strategy->stateDiscipline==trading_core::StrategyStateDiscipline::CanonicalEvidence) {
        context->scratch->canonicalPair=std::make_unique<BacktestCanonicalPairAdapter>();
        std::array<BacktestCanonicalPairSource,trading_core::kStrategyRuntimeMaxLegs> metadata;
        for(std::size_t leg=0;leg<selected.sourceIndices.size();++leg) {
          const auto source=selected.sourceIndices[leg];
          metadata[leg]=canonicalSources[source];
          context->metadataVisibleMono=std::max(context->metadataVisibleMono,metadataVisibleMono[source]);
          if(leg<2u)context->references.sourceIds[leg]=input.sessions[source].capturedSourceId;
          context->routes[leg]->activeContextIndex=static_cast<std::uint32_t>(i+1u);
        }
        trading_core::StrategyRuntimeConfiguration host;
        host.exchange=context->routes[0]->exchange.raw;host.ownerCount=static_cast<std::uint32_t>(accounts.size());
        host.independentOwner=true;host.strategyCount=1u;
        host.budgetConfigured=request.virtualHost.hasStrategyBudget;host.amountBudget=request.virtualHost.hasStrategyBudget;
        host.budgetValueRaw=request.virtualHost.strategyBudgetE8;
        host.budgetCurrency={request.virtualHost.budgetCurrency.data(),request.virtualHost.budgetCurrency.size()};
        host.marginBorrowAllowed=request.virtualHost.marginBorrowAllowed;
        if(!context->scratch->canonicalPair->bindCold(descriptor,params,context->state.data(),context->instruments,
             {metadata.data(),selected.sourceIndices.size()},detail::EngineView(context->routes.data(),context->count),
             static_cast<std::uint64_t>(input.sources[0].selection.manifest.replayAnchorReceiveNs),error,host)) {
          reject(Status::Unsupported,error);return result.status;
        }
        context->references.delivery=&context->delivery;
        context->references.centralBank=&context->centralBank.reference;
        context->scratch->pairReferences=&context->references;
      }
      contexts.push_back(std::move(context));

    return Status::Ok;
  }
  Status updateCut(const hftrec::corpus::CorpusCut& cut) {
    if(finished || result.riskStopped || !isOk(result.status) || !input.transaction.empty())return Status::InvalidArgument;
    if(cut.manifest.producerEpoch!=producerEpoch || cut.manifest.replayAnchorMonotonicNs!=anchorMonotonic ||
        cut.manifest.replayAnchorReceiveNs!=anchorRealtime || (cut.progress.producerEpoch && cut.progress.producerEpoch!=producerEpoch)) {
      reject(Status::CorruptData,"simulation producer/clock anchor changed after preparation");return result.status;
    }
    if(cut.generation<referenceGeneration){reject(Status::CorruptData,"simulation cut generation regressed");return result.status;}
    activeInput=!cut.finalized && !cut.manifest.complete;
    if(cut.referencePayload.size()+residentSourceCapacity*sizeof(detail::EngineState)+
        input.sessions.size()*(preparedSourceBytes-sizeof(detail::EngineState))+contexts.size()*preparedContextBytes>limits.maximumStateBytes) {
      reject(Status::Unsupported,"captured reference history exceeds resident simulation byte budget");return result.status;
    }
    referenceBytes=cut.referencePayload.size();
    const auto status=capturedReferences.update(cut,error);
    if(!isOk(status)){reject(status,error);return result.status;}
    if(request.corpusSources.empty()) {
      const auto status=materializeSymbols(cut);if(!isOk(status)){reject(status,error);return result.status;}
    }
    std::vector<BacktestCorpusSourceRequest> selections;
    for(const auto& source:cut.sources) {
      detail::SessionData metadata;
      metadata.exchange=detail::binary_corpus::fixedText(source.venue.data(),source.venue.size(),source.venueBytes);
      metadata.market=detail::binary_corpus::fixedText(source.market.data(),source.market.size(),source.marketBytes);
      metadata.symbol=detail::binary_corpus::fixedText(source.canonicalSymbol.data(),source.canonicalSymbol.size(),source.canonicalSymbolBytes);
      if(!configuredVenue(config,metadata))continue;
      if(!request.corpusSources.empty() && std::none_of(request.corpusSources.begin(),request.corpusSources.end(),[&](const auto& selected){
          return selected.exchange==metadata.exchange && selected.market==metadata.market && selected.canonicalSymbol==metadata.symbol;
      }))continue;
      selections.push_back({metadata.exchange,metadata.market,metadata.symbol,0,0});
    }
    if(wholeMarketPairs) {
      const auto status=selectWholeMarketSources(cut,selections);if(!isOk(status)){reject(status,error);return result.status;}
      for(const auto& retained:input.sessions)
        if(std::none_of(selections.begin(),selections.end(),[&](const auto& selected){
            return selected.exchange==retained.exchange && selected.market==retained.market && selected.canonicalSymbol==retained.symbol;
        }))selections.push_back({retained.exchange,retained.market,retained.symbol,0,0});
    }
    if(selections.size()>residentSourceCapacity || referenceBytes+residentSourceCapacity*sizeof(detail::EngineState)+
        selections.size()*(preparedSourceBytes-sizeof(detail::EngineState))+contexts.size()*preparedContextBytes>limits.maximumStateBytes) {
      reject(Status::Unsupported,"declared source/state byte capacity exceeded by captured topology");return result.status;
    }
    const auto prior=input.sessions.size();const auto priorAccounts=accounts.size();
    const auto appended=input.prepare(cut,selections,plan,error);
    if(!isOk(appended)){reject(appended,error);return result.status;}
    const auto count=input.sessions.size();
    engines.resize(count);instruments.resize(count);canonicalSources.resize(count);metadataVisibleMono.resize(count);
    subscribers.resize(count);accountIndices.resize(count);
    for(std::size_t source=prior;source<count;++source)if(!isOk(initializeSource(source)))return result.status;
    if(accounts.size()>trading_core::kStrategyRuntimeMaxLegs){reject(Status::Unsupported,"canonical account risk capacity exhausted");return result.status;}
    const auto priorContexts=contexts.size();
    std::vector<UniverseContextSelection> candidates;
    if(wholeMarketPairs) {
      const auto discovered=detail::discoverSimulationUniverse(*strategy,input,canonicalSources,limits.maximumContexts,candidates,error);
      if(!isOk(discovered)){reject(discovered,error);return result.status;}
    } else if(!discoverUniverseContexts(*strategy,config,input.sessions,candidates,error)) {
      reject(Status::ConfigMismatch,error);return result.status;
    }
    for(auto& candidate:candidates) {
      if(std::any_of(selectedContexts.begin(),selectedContexts.end(),[&](const auto& current){
          return current.sourceIndices==candidate.sourceIndices;
      }))continue;
      if(selectedContexts.size()>=limits.maximumContexts ||
          referenceBytes+(selectedContexts.size()+1u)*preparedContextBytes+residentSourceCapacity*sizeof(detail::EngineState)+
          input.sessions.size()*(preparedSourceBytes-sizeof(detail::EngineState))>limits.maximumStateBytes) {
        reject(Status::Unsupported,"declared strategy context/state byte capacity exhausted");return result.status;
      }
      selectedContexts.push_back(std::move(candidate));
      if(!isOk(initializeContext(selectedContexts.size()-1u)))return result.status;
    }
    if(selection && contexts.size()!=priorContexts) {
      std::vector<trading_core::StrategySelectionIdentity> identities;
      for(const auto& candidate:selectedContexts) {
        const auto source=candidate.sourceIndices.back();const auto& facts=canonicalSources[source].instrument;
        identities.push_back({facts.expiryRealtimeNs,cxet::StringView::fromCString(facts.nativeSymbol.data)});
      }
      if(!selection->appendCold(identities,contexts.front()->inputPolicy.maxAgeNs,error)) {
        reject(Status::Unsupported,error);return result.status;
      }
      for(std::size_t index=priorContexts;index<contexts.size();++index)
        if(!contexts[index]->scratch->canonicalPair->bindSelectionOwner(*selection,static_cast<std::uint32_t>(index))) {
          reject(Status::Unsupported,"appended canonical selection binding failed");return result.status;
        }
    }
    if(accounts.size()!=priorAccounts) {
      // Canonical local/simulation risk derives baselines from each unchanged
      // explicit initialBalance, so adding an account never rebases old PnL.
      refreshAccounts();
      for(std::size_t account=0;account<accounts.size();++account)
        riskConfigs[account]=makeBacktestPortfolioRiskLegConfig(configuredVenue(config,input.sessions[accountSources[account]]));
      risk=capital::risk::makePortfolioRiskState(makeBacktestPortfolioRiskConfig(config.risk),riskAccounts.data(),accounts.size(),riskConfigs.data());
    }
    referenceGeneration=cut.generation;
    result.universeSources=count;result.universeContexts=contexts.size();result.virtualAccounts=accounts.size();
    summarize();return result.status;
  }
  std::uint64_t monotonicFromReplay(std::int64_t replay) const {
    if(input.sessions.empty())return 0u;
    const auto& source=input.sessions.front();
    if(replay<source.replayAnchorReceiveNs)return 0u;
    const auto delta=static_cast<std::uint64_t>(replay-source.replayAnchorReceiveNs);
    return source.replayAnchorMonotonicNs+delta;
  }
  bool evaluateRisk(std::int64_t replay) {
    refreshAccounts();
    const auto evaluation=capital::risk::evaluatePortfolioRisk(risk,riskAccounts.data(),accounts.size(),timeNsFromI64(replay));
    if(!evaluation.breached)return false;
    for(auto& engine:engines) {
      engine.riskStopped=true;engine.riskStopTsNs=replay;
      engine.riskStopEquityE8=evaluation.equityE8;
      engine.riskStopThresholdEquityE8=evaluation.thresholdEquityE8;
    }
    paused=true;
    result.riskStopped=true;result.riskStopTsNs=replay;
    result.riskStopEquityE8=evaluation.equityE8;result.riskStopThresholdEquityE8=evaluation.thresholdEquityE8;
    return true;
  }
  void refreshFinancialAuthority(std::int64_t replay) {
    if(authorityReplay==replay && authorityReferenceGeneration==referenceGeneration)return;
    const auto currentMono=monotonicFromReplay(replay);
    for(std::size_t source=0;source<engines.size();++source) {
      auto candidate=instruments[source];BacktestCanonicalPairSource current;std::uint64_t visible{};
      bool qualified=capturedReferences.instrument(input.sources[source].selection.source,candidate,current,visible,error,currentMono);
      const auto& bound=canonicalSources[source].sizing;
      const auto same=[](const auto& left,const auto& right) {
        return cxet::StringView::fromCString(left.data)==cxet::StringView::fromCString(right.data);
      };
      qualified=qualified && engines[source].feePolicyKnown && candidate.tickSizeRaw==instruments[source].tickSizeRaw &&
          candidate.executionQuantityStepRaw==instruments[source].executionQuantityStepRaw &&
          candidate.nativeLotBaseQtyRaw==instruments[source].nativeLotBaseQtyRaw &&
          same(candidate.quoteCurrency,instruments[source].quoteCurrency) &&
          same(current.instrument.quoteAsset,canonicalSources[source].instrument.quoteAsset) &&
          same(current.instrument.settleAsset,canonicalSources[source].instrument.settleAsset) &&
          same(candidate.quoteCurrency,current.instrument.quoteAsset) &&
          cxet::api::instrumentEconomicFactPresent(current.instrument.economicFacts,cxet::api::kInstrumentEconomicCurrencies) &&
          current.sizing.capitalRequirementKnown && bound.capitalRequirementKnown &&
          current.sizing.initialCapitalRaw==bound.initialCapitalRaw && current.sizing.capitalBasisQtyRaw==bound.capitalBasisQtyRaw &&
          current.sizing.capitalCurrency==bound.capitalCurrency;
      detail::setSimulationDerivativeAuthorityCurrent(engines[source],qualified);
    }
    authorityReplay=replay;authorityReferenceGeneration=referenceGeneration;
  }
  bool prepareContextReferences(std::size_t index,std::int64_t replay) {
    auto& context=*contexts[index];
    refreshFinancialAuthority(replay);
    for(std::size_t leg=0;leg<std::min<std::size_t>(2u,context.count);++leg)
      context.references.quoteReceiveUtc[leg]=context.routes[leg]->reconstructedQuoteReceiveUtc;
    if(!context.scratch->canonicalPair || !strategy->runtimePolicy || !strategy->runtimePolicy->requiresPairSignal)return true;
    const auto mono=monotonicFromReplay(replay);
    const auto utc=replayUtcFromMonotonic(replay,context.routes[0]->replayUtcOffsetNs).raw;
    if(!capturedReferences.delivery(selectedContexts[index].sourceIndices,input,context.delivery,mono,utc,error) ||
        !capturedReferences.centralBank(context.centralBank,mono,utc,error)) {
      reject(Status::Unsupported,error);return false;
    }
    return true;
  }
  void refreshAccounts() {
      for (auto& engine : engines) {
        detail::refreshAccountPositionValue(engine, false);
        detail::refreshAccountPositionValue(engine, true);
      }
      for (std::size_t a = 0; a < accounts.size(); ++a)
        riskAccounts[a] = accountStateFromEngine(engines[accountSources[a]]);
    }
  detail::EngineView selectContext(std::size_t i) {
      auto& context = *contexts[i];
      for (std::size_t leg = 0; leg < context.count; ++leg)
        context.routes[leg]->activeContextIndex = static_cast<std::uint32_t>(i + 1);
      return detail::EngineView(context.routes.data(), context.count);
    }
  bool contextActive(std::size_t i) {
      const auto& context = *contexts[i];
      if(monotonicFromReplay(schedulerFloor)<context.metadataVisibleMono)return false;
      for (std::size_t leg = 0u; leg < context.count; ++leg) {
        const auto& source = *context.routes[leg];
        const auto sourceIndex=selectedContexts[i].sourceIndices[leg];
        if(!capturedReferences.instrumentAvailable(input.sources[sourceIndex].selection.source,monotonicFromReplay(schedulerFloor)))return false;
        if (!source.capturedSourceMember ||
            (context.requiredCaptureMask & ~source.capturedActiveChannels) != 0u ||
            (context.requiredCaptureMask & source.capturedUnhealthyChannels) != 0u ||
            !backtestStrategySnapshotReady(context.inputPolicy, source.snapshot)) return false;
      }
      return true;
    }
  bool until(std::int64_t ts, RunUntilBoundary boundary) {
    const auto& descriptor=*strategy;
    if(selection && now>0 && ts>now)selection->finishCut(static_cast<std::uint64_t>(now),!result.riskStopped,!result.riskStopped);
      for (const auto& engine : engines) if (engine.simulationEvidenceFailed) {
        reject(Status::Unsupported, "canonical simulated order evidence capacity or identity refused"); return false;
      }
      for (std::uint32_t guard = 0u; guard < 1'000'000u; ++guard) {
        std::size_t selected = contexts.size();
        auto due = std::numeric_limits<std::int64_t>::max();
        std::size_t privateSource = engines.size();
        auto privateDue = std::numeric_limits<std::int64_t>::max();
        for (std::size_t source = 0u; source < engines.size(); ++source) {
          const auto& engine = engines[source];
          const auto head = engine.deliveredUserEvents;
          if (head >= engine.userEvents.size()) continue;
          if (head >= engine.userEventDeliveryTsNs.size() || head >= engine.userEventContextIndices.size()) {
            reject(Status::CorruptData, "simulated private source prefix metadata missing"); return false;
          }
          const auto eligible = std::max({engine.userEventDeliveryTsNs[head], engine.replayNotBeforeNs, schedulerFloor});
          if (eligible < privateDue || (eligible == privateDue &&
              (privateSource == engines.size() || engine.userEventDeliveryTsNs[head] <
               engines[privateSource].userEventDeliveryTsNs[engines[privateSource].deliveredUserEvents]))) {
            privateDue = eligible; privateSource = source;
          }
        }
        for (std::size_t i = 0; i < contexts.size(); ++i) {
          const bool active=contextActive(i);
          const auto engines = selectContext(i);
          const auto& context = *contexts[i];
          const auto next = std::min({
              result.riskStopped || !active ? std::numeric_limits<std::int64_t>::max() :
                  (context.scratch->canonicalPair && context.scratch->canonicalPair->nextWakeMono().raw
                    ? static_cast<std::int64_t>(context.scratch->canonicalPair->nextWakeMono().raw)
                    : strategy_runtime_detail::nextWakeTs(descriptor, context.state.data())),
              strategy_runtime_detail::nextPendingTs(engines),
              strategy_runtime_detail::nextUserEventDeliveryTs(engines),
              strategy_runtime_detail::nextRateLimitCooldownTs(engines)});
          auto floor = schedulerFloor;
          for (const auto& source : engines) floor = std::max(floor, source.replayNotBeforeNs);
          const auto eligible = std::max(next, floor);
          if (eligible < due) { due = eligible; selected = i; }
        }
        if (privateSource < engines.size() && privateDue <= due &&
            privateDue <= ts && !(boundary == RunUntilBoundary::Exclusive && privateDue == ts)) {
          auto& engine = engines[privateSource];
          const auto head = engine.deliveredUserEvents;
          const auto ownerIndex = engine.userEventContextIndices[head];
          if (!ownerIndex || ownerIndex > contexts.size()) {
            reject(Status::CorruptData, "simulated private event has no admitted context owner"); return false;
          }
          const auto event = engine.userEvents[head];
          const auto contextIndex = static_cast<std::size_t>(ownerIndex - 1u);
          const auto& indices = selectedContexts[contextIndex].sourceIndices;
          const auto found = std::find(indices.begin(), indices.end(), privateSource);
          if (found == indices.end()) { reject(Status::CorruptData, "private event source is foreign to context"); return false; }
          schedulerFloor = privateDue;
          engine.replayNotBeforeNs = std::max(engine.replayNotBeforeNs, privateDue);
          engine.activeContextIndex = ownerIndex;
          refreshFinancialAuthority(privateDue);
          if (!detail::deliverSimulationOrderEvent(engine, head, privateDue)) {
            reject(Status::CorruptData, "simulated private event correlation or accounting refused"); return false;
          }
          const bool riskBreached=evaluateRisk(privateDue);
          if (!engine.userEventServiceFlags[head] &&
              ((!riskBreached && contextActive(contextIndex)) || contexts[contextIndex]->scratch->canonicalPair)) {
            auto& context = *contexts[contextIndex];
            refreshAccounts();
            if(!prepareContextReferences(contextIndex,privateDue))return false;
            if (!runCycleMulti(descriptor, context.inputPolicy, context.state.data(),
                  selectContext(contextIndex), context.instruments, *context.scratch, context.shard,
                  context.control, static_cast<std::uint32_t>(found - indices.begin()), privateDue,
                  context.request, &event)) {
              const auto evidenceFailed = std::any_of(engines.begin(), engines.end(),
                  [](const auto& source) { return source.simulationEvidenceFailed; });
              reject(evidenceFailed ? Status::Unsupported : Status::InvalidArgument,
                  evidenceFailed ? "canonical simulated order evidence capacity or identity refused" :
                      "canonical strategy rejected private delivery context"); return false;
            }
          }
          now=std::max(now,privateDue);
          if(!publishEvent())return false;
          if(paused)return true;
          continue;
        }
        if (selected == contexts.size() || due > ts ||
            (boundary == RunUntilBoundary::Exclusive && due == ts)) return true;
        if(selection && selection->needsFullCut(static_cast<std::uint64_t>(due))) {
          schedulerFloor=due;
          for(std::size_t i=0;i<contexts.size();++i) {
            if(!contextActive(i)) {selection->withdraw(static_cast<std::uint32_t>(i),static_cast<std::uint64_t>(due));continue;}
            auto& candidate=*contexts[i];
            if(!prepareContextReferences(i,due) || !runCycleMulti(descriptor,candidate.inputPolicy,candidate.state.data(),
                selectContext(i),candidate.instruments,*candidate.scratch,candidate.shard,candidate.control,0u,due,candidate.request,nullptr)) {
              reject(Status::InvalidArgument,"canonical candidate reevaluation failed");return false;
            }
          }
          selection->finishCut(static_cast<std::uint64_t>(due),!result.riskStopped,!result.riskStopped);
        }
        auto& context = *contexts[selected];
        schedulerFloor = due;
        for (auto* source : context.routes) if (source)
          source->replayNotBeforeNs = std::max(source->replayNotBeforeNs, due);
        const auto routes=selectContext(selected);
        const auto pending=strategy_runtime_detail::nextPendingTs(routes);
        const auto cooldown=strategy_runtime_detail::nextRateLimitCooldownTs(routes);
        if(pending<=due) {
          refreshFinancialAuthority(due);
          for(auto& engine:routes)detail::applyDueActions(engine,due);
          (void)evaluateRisk(due);
        } else {
          if(cooldown<=due)strategy_runtime_detail::clearRateLimitCooldown(routes,due);
          (void)evaluateRisk(due);
          if(!result.riskStopped) {
            if(!prepareContextReferences(selected,due))return false;
            ++routes[0].timerWakeEvents;
            if(!runCycleMulti(descriptor,context.inputPolicy,context.state.data(),routes,context.instruments,
                *context.scratch,context.shard,context.control,0u,due,context.request,nullptr)) {
              reject(Status::InvalidArgument,"canonical strategy timer cycle rejected");return false;
            }
          }
        }
        now=std::max(now,due);
        if(!publishEvent())return false;
        if(paused)return true;
      }
      return false;
    }

  bool applyTransaction() {
    const auto& descriptor=*strategy;
    const auto& sources=input.sessions;
      const auto first = transaction.front();
      if(request.corpusBeginReceiveNs>0 && first.deliveryRealtimeNs<request.corpusBeginReceiveNs) {
        for(const auto& event:transaction) {
          auto& engine=engines[event.legIndex];
          if(event.kind==detail::kTimelineKindLifecycle) {
            if(!applyCapturedSourceLifecycle(engine,sources[event.legIndex].sourceLifecycle[event.index],event.deliveryTsNs,error)) {
              reject(Status::CorruptData,error);return false;
            }
          } else if(event.kind==detail::kTimelineKindHealth) {
            if(!applyCapturedSourceHealth(engine,sources[event.legIndex].sourceHealth[event.index],event.deliveryTsNs,error)) {
              reject(Status::CorruptData,error);return false;
            }
          }
        }
        return true;
      }
      if (first.deliveryTsNs < now || !until(first.deliveryTsNs, RunUntilBoundary::Exclusive)) {
        if (isOk(result.status)) reject(Status::CorruptData, "universe delivery or strategy timer order rejected");
        return false;
      }
      now = first.deliveryTsNs;
      schedulerFloor = now;
      for (auto& engine : engines) applyCapturedDeliveryClock(engine, first);
      refreshFinancialAuthority(now);
      for (const auto& event : transaction) {
        auto& engine = engines[event.legIndex];
        if (event.kind == detail::kTimelineKindHealth) {
          if (!applyCapturedSourceHealth(engine, sources[event.legIndex].sourceHealth[event.index], now, error)) {
            reject(Status::CorruptData, error); return false;
          }
        } else if (event.kind == detail::kTimelineKindLifecycle) {
          const auto& row = sources[event.legIndex].sourceLifecycle[event.index];
          auto& instrument = instruments[event.legIndex];
          trading_core::StrategyInstrumentSpec candidate;
          const auto preparedInstrument = prepareBacktestStrategyInstrument(
              sources[event.legIndex].instrument, candidate, error);
          if (!isOk(preparedInstrument)) { reject(preparedInstrument, error); return false; }
          BacktestCanonicalPairSource typedCandidate;std::uint64_t visible{};
          if(!capturedReferences.instrument(input.sources[event.legIndex].selection.source,candidate,typedCandidate,visible,error)) {
            reject(Status::Unsupported,error);return false;
          }
          if (candidate.tickSizeRaw != instrument.tickSizeRaw ||
              candidate.stepSizeRaw != instrument.stepSizeRaw ||
              candidate.executionQuantityStepRaw != instrument.executionQuantityStepRaw ||
              candidate.contractBaseQtyRaw != instrument.contractBaseQtyRaw ||
              candidate.priceBasisQtyRaw != instrument.priceBasisQtyRaw ||
              cxet::bytescan::hftStrncmp(candidate.quoteCurrency.data, instrument.quoteCurrency.data,
                  cxet::composite::AccountCurrencyCode::capacity) != 0 ||
              candidate.quoteToPositionLimitE8 != instrument.quoteToPositionLimitE8 ||
              candidate.expiryUtcNs != instrument.expiryUtcNs) {
            reject(Status::Unsupported, "strategy has no canonical instrument rebind for changed captured metadata"); return false;
          }
          if (!applyCapturedSourceLifecycle(engine, row, now, error)) {
            reject(Status::CorruptData, error); return false;
          }
          instrument = candidate;
          engine.tickSizeE8 = instrument.tickSizeRaw;
          engine.bboReconstruction.tickSize = Price{instrument.tickSizeRaw};
          engine.venueBboReconstruction.tickSize = Price{instrument.tickSizeRaw};
          for (auto contextIndex : subscribers[event.legIndex]) {
            auto& context = *contexts[contextIndex];
            const auto& indices = selectedContexts[contextIndex].sourceIndices;
            const auto local = std::find(indices.begin(), indices.end(), event.legIndex) - indices.begin();
            context.instruments[local] = instrument;
            context.scratch->revisions[local] = 0u;
          }
        } else {
        if (!event.applicationOnly)
          applyMarketEventToSnapshots(engine, sources[event.legIndex], event,
              engine.symbol, instruments[event.legIndex], false);
        applyMarketEventToSnapshots(engine, sources[event.legIndex], event,
            engine.symbol, instruments[event.legIndex], true);
        }
        includeTimelineClock(result.replayClock, event);
        includeTimelineClock(engine, event);
        if (event.venueScheduleAdjusted && !event.applicationOnly) {
          ++result.replayClock.venueScheduleAdjustedEvents;
          ++engine.venueScheduleAdjustedEvents;
        }
        ++result.events;
        if (event.kind == detail::kTimelineKindTrade) ++result.trades;
        else if (event.kind == detail::kTimelineKindBookTicker) ++result.bookTickers;
        else if (event.kind == detail::kTimelineKindDepth) ++result.depths;
        if (event.exchangeTsNs > 0 && (result.firstTsNs == 0 || event.exchangeTsNs < result.firstTsNs))
          result.firstTsNs = event.exchangeTsNs;
        result.lastTsNs = std::max(result.lastTsNs, event.exchangeTsNs);
        if (engine.depthReplayFailed) { reject(Status::CorruptData, "captured depth transaction rejected"); return false; }
        if (engine.simulationEvidenceFailed) {
          reject(Status::Unsupported, "canonical simulated order evidence capacity or identity refused"); return false;
        }
      }
      if (!isOk(result.status)) return false;
      const bool riskBreached=evaluateRisk(now);
      if (!riskBreached && !transaction.back().applicationOnly) {
        for (auto i : subscribers[first.legIndex]) {
          if (!contextActive(i)) continue;
          auto& context = *contexts[i];
          const auto& indices = selectedContexts[i].sourceIndices;
          const auto leg = std::find(indices.begin(), indices.end(), first.legIndex) - indices.begin();
          refreshAccounts();
          if(!prepareContextReferences(i,now))return false;
          if (!runCycleMulti(descriptor, context.inputPolicy, context.state.data(),
                selectContext(i), context.instruments, *context.scratch, context.shard,
                context.control, static_cast<std::uint32_t>(leg), now, context.request, nullptr)) {
            const auto evidenceFailed = std::any_of(engines.begin(), engines.end(),
                [](const auto& engine) { return engine.simulationEvidenceFailed; });
            reject(evidenceFailed ? Status::Unsupported : Status::InvalidArgument,
                evidenceFailed ? "canonical simulated order evidence capacity or identity refused" :
                    "canonical strategy rejected universe context"); return false;
          }
        }
      }
      if (!isOk(result.status)) {
        if (isOk(result.status)) reject(Status::InvalidArgument, "universe user event loop did not settle");
        return false;
      }
      refreshAccounts();
      (void)evaluateRisk(now);
      if (request.strictRateLimitRejects &&
          rateLimitRejectedCommands(engines[0], &engines) != 0u) {
        reject(Status::InvalidArgument, "strict shared-account quota rejection"); return false;
      }

    for(auto& engine:engines) (void)detail::appendRiskSnapshotIfDue(engine,now);
    return publishEvent();
  }
  bool publishEvent() {
    for(const auto& engine:engines)if(engine.simulationEvidenceFailed) {
      reject(Status::Unsupported,"bounded simulation output/execution capacity exhausted");return false;
    }
    summarize();
    if(!flush())return false;
    if(sink.metrics && !sink.metrics(result,monotonicFromReplay(now),
          static_cast<std::int64_t>(replayUtcFromMonotonic(now,engines.empty()?0:engines.front().replayUtcOffsetNs).raw),
          portfolioEquityE8(engines,initialBalance),trading_core::bpsE8(result.totalPnlE8,initialBalance),
          accounts.empty()?0u:accounts.front()->quoteAssetId,sink.userData)) {
      reject(Status::IoError,"simulation metrics sink refused output");return false;
    }
    return true;
  }
  bool flush() {
    for (std::size_t i=0u;i<engines.size();++i) {
      if(!detail::simulation_output::compact(engines[i],input.sessions[i].capturedSourceId,monotonicFromReplay(now),sink)) {
        reject(Status::IoError,"simulation incremental result sink refused output");return false;
      }
    }
    for(std::size_t source=0;source<engines.size();++source) {
      struct Proof {Impl* owner;std::size_t source;};
      Proof proof{this,source};
      (void)detail::reclaimSimulationOrderEvidence(engines[source],{&proof,
          [](void* raw,std::uint32_t context,Id intent,std::uint64_t cut) noexcept {
            auto& proof=*static_cast<Proof*>(raw);auto& owner=*proof.owner;
            if(!context || context>owner.contexts.size())return false;
            const auto index=static_cast<std::size_t>(context-1u);
            const auto& indices=owner.selectedContexts[index].sourceIndices;
            const auto found=std::find(indices.begin(),indices.end(),proof.source);
            if(found==indices.end())return false;
            const auto& scratch=*owner.contexts[index]->scratch;
            if(scratch.canonicalPair)return scratch.canonicalPair->released(
                static_cast<std::size_t>(found-indices.begin()),context,intent,cut);
            // Snapshot state owns no retained canonical execution references;
            // completed runCycle has consumed this cut before flush.
            return true;
          }});
    }
    return true;
  }
  void summarize() {
    const auto& sources=input.sessions;
    populatePortfolioResultPnl(result,sources,engines,initialBalance);
    accountSummaries.assign(accounts.size(),{});
    for(std::size_t index=0;index<accounts.size();++index) {
      const auto& account=*accounts[index];const auto& engine=engines[accountSources[index]];
      accountSummaries[index]={static_cast<std::uint32_t>(index+1u),account.quoteAssetId,
          engine.exchange.raw,engine.market.raw,account.initialBalanceE8,account.walletBalanceE8,
          account.observedWalletBalanceE8,trading_core::saturatingAddI64(account.walletBalanceE8,account.positionEquityValueE8)};
    }
    for(std::size_t source=0;source<engines.size();++source) {
      auto& account=accountSummaries[accountIndices[source]];const auto& engine=engines[source];
      account.grossRealizedPnlE8=trading_core::saturatingAddI64(account.grossRealizedPnlE8,engine.grossRealizedPnlE8);
      account.netRealizedPnlE8=trading_core::saturatingAddI64(account.netRealizedPnlE8,engine.netRealizedPnlE8);
      account.unrealizedPnlE8=trading_core::saturatingAddI64(account.unrealizedPnlE8,detail::unrealizedPnlE8(engine));
      account.feesPaidE8=trading_core::saturatingAddI64(account.feesPaidE8,engine.feesPaidE8);
    }
    pnlPeak=std::max(pnlPeak,result.totalPnlE8);
    drawdown=std::max(drawdown,trading_core::saturatingSubI64(pnlPeak,result.totalPnlE8));
    mfe=std::max(mfe,result.totalPnlE8);mae=std::min(mae,result.totalPnlE8);
    result.maxDrawdownE8=drawdown;result.mfeE8=mfe;result.maeE8=mae;
    result.totalPnlCurveE8.clear();
  }
};

SimulationSession::SimulationSession() noexcept=default;
SimulationSession::~SimulationSession()=default;
Status SimulationSession::prepare(const BacktestRunRequest& request,const hftrec::corpus::CorpusCut& cut,SimulationOutputSink sink,SimulationLimits limits) noexcept {
  if(impl_ && impl_->result.status!=Status::NotReady)return Status::InvalidArgument;
  try {impl_=std::make_unique<Impl>();return impl_->prepare(request,cut,sink,limits);}
  catch (...) {if(impl_) {impl_->result.status=Status::IoError;impl_->result.error="simulation owner allocation failed";}return Status::IoError;}
}
Status SimulationSession::advance(std::span<const hftrec::corpus::BinaryMarketRecord> records,
    std::uint64_t safe,SimulationAdvance& progress) noexcept {
  progress={};
  if(!impl_ || impl_->finished) return Status::InvalidArgument;
  auto& owner=*impl_;
  progress.paused=owner.paused;
  if(owner.paused || !isOk(owner.result.status)) return owner.result.status;
  try {
    if(safe<owner.safeMonotonicNs) {owner.reject(Status::CorruptData,"simulation completeness watermark regressed");return owner.result.status;}
    for(const auto& record:records) {
      if(owner.paused)break;
      if(owner.input.transaction.empty() && record.header.receiveMonotonicNs>=owner.anchorMonotonic &&
          record.header.receiveMonotonicNs-owner.anchorMonotonic<=static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()-owner.anchorRealtime)) {
        const auto boundary=owner.anchorRealtime+static_cast<std::int64_t>(record.header.receiveMonotonicNs-owner.anchorMonotonic);
        if((owner.request.corpusBeginReceiveNs<=0 || record.header.receiveRealtimeNs>=owner.request.corpusBeginReceiveNs) &&
            !owner.until(boundary,RunUntilBoundary::Exclusive)) {
          if(isOk(owner.result.status))owner.reject(Status::InvalidArgument,"simulation scheduler failed before input acceptance");
          return owner.result.status;
        }
        if(owner.paused)break;
      }
      if(record.header.receiveMonotonicNs<owner.lastRecordMonotonicNs ||
          (owner.safeMonotonicNs && record.header.receiveMonotonicNs<owner.safeMonotonicNs)) {
        owner.reject(Status::CorruptData,"record arrived after its committed simulation prefix");return owner.result.status;
      }
      owner.lastRecordMonotonicNs=record.header.receiveMonotonicNs;
      const auto status=owner.input.append(record,owner.transaction,owner.error);
      if(!isOk(status)) {owner.reject(status,owner.error);return status;}
      ++progress.recordsAccepted;
      if(!owner.transaction.empty() && !owner.applyTransaction()) return owner.result.status;
    }
    if(!owner.paused && !owner.input.transaction.empty() && safe>owner.input.transaction.front().header.receiveMonotonicNs) {
      owner.reject(Status::CorruptData,"completeness watermark splits application transaction");return owner.result.status;
    }
    // A recorded receive watermark has the same monotonic domain as records.
    // Project through the immutable captured clock anchor, never exchange time.
    if(!owner.paused && safe && !owner.engines.empty()) {
      const auto& source=owner.input.sessions.front();
      const auto anchor=source.replayAnchorMonotonicNs;
      const auto realtime=source.replayAnchorReceiveNs;
      if(!anchor || realtime<=0 || safe<anchor || safe-anchor>static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()-realtime)) {
        owner.reject(Status::CorruptData,"simulation watermark lacks captured receive clock anchor");return owner.result.status;
      }
      const auto until=realtime+static_cast<std::int64_t>(safe-anchor);
      if((owner.request.corpusBeginReceiveNs<=0 || until>=owner.request.corpusBeginReceiveNs) &&
          !owner.until(until,RunUntilBoundary::Exclusive)) {
        if(isOk(owner.result.status)) owner.reject(Status::InvalidArgument,"simulation timer/private loop did not settle");return owner.result.status;
      }
    }
    if(!owner.paused)owner.safeMonotonicNs=safe;
    owner.summarize();
    if(!owner.flush()) return owner.result.status;
    progress.eventsProcessed=owner.result.events;progress.safeMonotonicNs=owner.safeMonotonicNs;progress.paused=owner.paused;
    progress.terminalMonotonicNs=owner.result.riskStopped?owner.monotonicFromReplay(owner.result.riskStopTsNs):0u;
    return owner.result.status;
  } catch (...) {owner.reject(Status::IoError,"simulation bounded advancement failed");return owner.result.status;}
}
Status SimulationSession::updateCut(const hftrec::corpus::CorpusCut& cut) noexcept {
  if(!impl_)return Status::InvalidArgument;
  try {return impl_->updateCut(cut);}catch(...) {impl_->reject(Status::IoError,"simulation topology update allocation failed");return impl_->result.status;}
}
void SimulationSession::pause() noexcept {if(impl_) impl_->paused=true;}
void SimulationSession::resume() noexcept {if(impl_ && !impl_->finished && isOk(impl_->result.status)) impl_->paused=false;}
Status SimulationSession::finish() noexcept {
  if(!impl_) return Status::InvalidArgument;
  auto& owner=*impl_;
  if(owner.finished || !isOk(owner.result.status)) return owner.result.status;
  if(!owner.input.transaction.empty()) {owner.reject(Status::CorruptData,"simulation ended within captured transaction");return owner.result.status;}
  try {
    owner.summarize();
    for(auto& engine:owner.engines) {
      detail::appendEquityPoint(engine,owner.now);
      detail::appendRiskSnapshot(engine,owner.now,detail::kRiskSnapshotReasonFinal);
    }
    if(!owner.flush()) return owner.result.status;
    for(std::size_t source=0;source<owner.engines.size();++source)
      if(!detail::simulation_output::finishOpen(owner.engines[source],owner.input.sessions[source].capturedSourceId,
          owner.monotonicFromReplay(owner.now),owner.now,owner.sink)) {
        owner.reject(Status::IoError,"simulation final retained output refused");return owner.result.status;
      }
    owner.finished=true;owner.paused=false;
    return owner.result.status;
  } catch (...) {owner.reject(Status::IoError,"simulation final output failed");return owner.result.status;}
}
std::span<const SimulationAccountSummary> SimulationSession::accountSummaries() const noexcept {
  return impl_?std::span<const SimulationAccountSummary>{impl_->accountSummaries}:std::span<const SimulationAccountSummary>{};
}
bool SimulationSession::returnSinceEquityBpsE8(std::int64_t previous,std::int64_t& out) const noexcept {
  out=0;if(!impl_ || previous<=0)return false;
  out=trading_core::bpsE8(trading_core::saturatingSubI64(equityE8(),previous),previous);return true;
}
std::uint32_t SimulationSession::valuationQuoteAssetId() const noexcept {
  return impl_ && !impl_->accounts.empty()?impl_->accounts.front()->quoteAssetId:0u;
}
std::int64_t SimulationSession::equityE8() const noexcept {
  return impl_?portfolioEquityE8(impl_->engines,impl_->initialBalance):0;
}
std::int64_t SimulationSession::cumulativeReturnBpsE8() const noexcept {
  return impl_?trading_core::bpsE8(impl_->result.totalPnlE8,impl_->initialBalance):0;
}
const BacktestRunResult& SimulationSession::result() const noexcept {
  static const BacktestRunResult empty{};return impl_?impl_->result:empty;
}
} // namespace hft_backtest
