#include "OfflineCase.hpp"
#include "hft_backtest/SimulationSession.hpp"
#include "hft_backtest/Research.hpp"
#include "hftrec/CorpusContract/BinaryMarketCorpusWriter.hpp"
#include "hftrec/CorpusContract/RecorderReference.hpp"
#include "cxet/Canon/Subtypes.hpp"
#include "cxet/Canon/PositionAndExchange.hpp"
#include "trading_core/Algorithm/Signals/Basis/DeliveryReferenceModel.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {
namespace corpus=hftrec::corpus;
using hft_backtest::SimulationSession;
constexpr std::int64_t E=100'000'000;
constexpr std::uint64_t mono=1'000'000'000;
constexpr std::uint64_t day=20'000u*trading_core::kCivilDayNs-trading_core::kMoscowUtcOffsetNs;
constexpr auto utc=day+mono;

struct Transcript {
  std::vector<std::string> rows;
  std::uint64_t greatestEventMonotonic{};
  static bool write(std::uint32_t source,std::uint64_t time,cxet::StringView stream,cxet::StringView row,void* raw) noexcept {
    try {auto& self=*static_cast<Transcript*>(raw);self.greatestEventMonotonic=std::max(self.greatestEventMonotonic,time);
      self.rows.push_back(std::to_string(source)+":"+std::to_string(time)+":"+
        std::string(stream.data(),stream.size())+":"+std::string(row.data(),row.size()));return true;}catch(...){return false;}
  }
  static bool metric(const hft_backtest::BacktestRunResult& result,std::uint64_t time,std::int64_t,
      std::int64_t equity,std::int64_t returns,std::uint32_t quote,void* raw) noexcept {
    try {auto& self=*static_cast<Transcript*>(raw);self.greatestEventMonotonic=std::max(self.greatestEventMonotonic,time);
      self.rows.push_back("metrics:"+std::to_string(time)+":"+
        std::to_string(result.events)+":"+std::to_string(result.orders)+":"+std::to_string(result.fills)+":"+
        std::to_string(equity)+":"+std::to_string(returns)+":"+std::to_string(quote));return true;}catch(...){return false;}
  }
  hft_backtest::SimulationOutputSink sink(){return {write,this,metric};}
};
struct Fixture {
  std::filesystem::path config=std::filesystem::temp_directory_path()/
      ("simulation-session-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".conf");
  corpus::CorpusCut cut;
  hft_backtest::BacktestRunRequest request;
  Fixture() {
    request.configPath=config;request.runId="deterministic-session";request.initialBalanceE8=10'000*E;
    request.orderLatencyUs=request.cancelLatencyUs=0u;request.hasMakerFeeBps=request.hasTakerFeeBps=true;
    request.rateLimitsEnabled=false;request.writeArtifacts=false;
    request.virtualHost={true,10'000*E,"RUB",false};
    std::ofstream out(config);
    out << "[strategy]\ntype=pair_api_probe\nenabled=true\nsizing.mode=anchor_lots\nsizing.value=1\nsizing.anchor_leg=1\nsizing.quote_currency=1\n"
        << "[venue.binance_spot]\nsymbols=AAA_RUB\nrate_limits_enabled=false\n"
        << "[venue.binance_futures]\nsymbols=AAA_FUT\nrate_limits_enabled=false\n"
        << "[portfolio.pair]\nlegs=binance_spot:AAA_RUB,binance_futures:AAA_FUT\n";
    out.close();
    cut.generation=1;cut.committedMonotonicNs=mono;cut.progress.safeMonotonicNs=mono+1u;
    cut.progress.producerEpoch=cut.manifest.producerEpoch=13u;
    cut.manifest.replayAnchorMonotonicNs=mono;cut.manifest.replayAnchorReceiveNs=utc;
    cut.manifest.startedReceiveNs=utc;cut.manifest.shardCount=cut.progress.shardCount=1u;
    cut.sources.resize(2u);
    std::vector<corpus::RecorderReferenceSnapshot> snapshots;
    for(std::size_t leg=0;leg<2u;++leg) {
      auto& source=cut.sources[leg];source.sourceId=leg+1u;source.canonicalSymbolId=leg+1u;
      source.initialSourceGeneration=1u;source.venueId=canon::kExchangeIdBinance.raw;
      source.marketRaw=leg?canon::kMarketTypeFutures.raw:canon::kMarketTypeSpot.raw;
      source.marketKindRaw=leg?2u:1u;source.assetDomainRaw=2u;
      source.priceScale=source.quantityScale=8u;
      source.tickSizeRaw=1;source.stepSizeRaw=source.canonicalStepSizeRaw=source.executionQuantityStepRaw=E;
      source.contractBaseQtyRaw=source.priceBasisQtyRaw=E;
      source.economicBaseAssetId=source.quoteAssetId=1u;
      source.quantityConversion.canonicalBaseMultiplier=source.quantityConversion.nativeBaseMultiplier=1u;
      source.quantityConversion.kinds[0]=corpus::BinaryMarketQuantityKind::CanonicalBase;
      source.directoryFlags=corpus::BinaryMarketDirectoryBookTicker|corpus::BinaryMarketDirectoryTickSize;
      source.instrumentMetadataFlags=corpus::BinaryMarketInstrumentStepSize|corpus::BinaryMarketInstrumentCanonicalStepSize|
          corpus::BinaryMarketInstrumentExecutionQuantityStep|corpus::BinaryMarketInstrumentContractBaseQty;
      source.configuredChannelMask=source.availableChannelMask=source.traderReplayChannelMask=
          corpus::binaryMarketChannelBit(corpus::BinaryMarketChannel::BookTicker);
      source.compatibility[0]=corpus::BinaryMarketCompatibility::ExactTraderReplay;
      const auto set=[](auto& dst,const char* text){std::copy_n(text,std::strlen(text)+1u,dst.data());};
      set(source.venue,"binance");set(source.market,leg?"futures":"spot");
      set(source.canonicalSymbol,leg?"AAA_FUT":"AAA_RUB");set(source.nativeSymbol,leg?"AAA_FUT":"AAA");
      source.venueBytes=7u;source.marketBytes=leg?7u:4u;source.canonicalSymbolBytes=7u;source.nativeSymbolBytes=leg?7u:3u;
      hft_parser::ipc::InstrumentReferenceRecord record;
      record.product.exchangeRaw=source.venueId;record.product.marketRaw=source.marketRaw;
      record.nativeSymbol.copyFrom(leg?"AAA_FUT":"AAA");record.hasMetadata=1u;
      record.observation.sourceGeneration=record.observation.cacheRevision=1u;
      record.observation.observedMonotonicNs=mono;record.observation.observedRealtimeNs=utc;
      record.observation.freshness=record.observation.provenance=1u;
      auto& row=record.instrument;row.nativeSymbol=record.nativeSymbol;
      row.kind=leg?cxet::api::InstrumentKind::Delivery:cxet::api::InstrumentKind::Spot;
      row.assetClass=leg?cxet::api::InstrumentAssetClass::Future:cxet::api::InstrumentAssetClass::Equity;
      row.assetDomain=cxet::api::InstrumentAssetDomain::TradFi;
      std::copy_n(leg?"RTSX":"MISX",5u,row.executionMic);
      std::copy_n(leg?"future-id":"spot-id",leg?10u:8u,row.providerInstrumentId);
      std::copy_n("RUB",4u,row.quoteAsset.data);row.settleAsset=row.quoteAsset;
      row.expiryRealtimeNs=day+30u*trading_core::kCivilDayNs;
      auto& facts=row.economicFacts;facts.source=cxet::api::InstrumentEconomicSource::NativeCatalog;
      facts.sourceVersion=1u;facts.sourceFingerprint=123u;
      if(!leg) {
        facts.equityClass=cxet::api::InstrumentEquityClass::Common;
        facts.presenceMask=cxet::api::kInstrumentEconomicEquityClass|cxet::api::kInstrumentEconomicCurrencies;
      } else {
        facts.underlyingAssetClass=cxet::api::InstrumentAssetClass::Equity;
        facts.underlyingEquityClass=cxet::api::InstrumentEquityClass::Common;
        facts.term=cxet::api::InstrumentTerm::Dated;facts.settlement=cxet::api::InstrumentSettlement::Physical;
        facts.underlyingNativeSymbol.copyFrom("AAA");
        facts.presenceMask=cxet::api::kInstrumentEconomicUnderlyingAssetClass|cxet::api::kInstrumentEconomicUnderlyingEquityClass|
            cxet::api::kInstrumentEconomicTerm|cxet::api::kInstrumentEconomicSettlement|cxet::api::kInstrumentEconomicUnderlyingIdentity|
            cxet::api::kInstrumentEconomicExpiry|cxet::api::kInstrumentEconomicCurrencies;
      }
      auto& metadata=record.metadata;metadata.nativeSymbol=record.nativeSymbol;metadata.hasNativeSymbol=true;
      metadata.assetClass=row.assetClass;metadata.economicFacts=facts;
      std::copy_n(row.executionMic,sizeof(row.executionMic),metadata.executionMic);
      std::copy_n(row.providerInstrumentId,sizeof(row.providerInstrumentId),metadata.providerInstrumentId);
      std::copy_n("1",2u,metadata.nativeLotBaseQty);metadata.nativeQuantityUnit=cxet::contract::Unit::BaseQuantity;
      std::copy_n("RUB",4u,metadata.quoteCurrency);metadata.hasQuoteCurrency=true;
      auto& financial=metadata.financialFacts;
      financial.buyInitialMarginE8=financial.sellInitialMarginE8=10*E;financial.quantityBasisRaw=E;
      std::copy_n("RUB",4u,financial.currency);financial.presenceMask=cxet::api::kInstrumentFinancialAllMask;
      financial.sourceVersion=financial.sourceFingerprint=1u;
      CXET_CHECK(hft_parser::ipc::validReferenceRecord(record));
      corpus::RecorderReferenceSnapshot snapshot;snapshot.product=record.product;
      snapshot.dataset=hft_parser::ipc::ReferenceDatasetKind::Instruments;
      snapshot.presence=corpus::RecorderReferencePresence::Present;
      snapshot.acquiredMonotonicNs=snapshot.visibleFromMonotonicNs=mono;snapshot.acquiredRealtimeNs=utc;
      snapshot.visibleFromCutGeneration=1u;snapshot.producer.producerEpoch=13u;snapshot.producer.generation=1u;
      auto& prefix=snapshot.producer.snapshot;prefix.product=record.product;prefix.dataset=snapshot.dataset;
      prefix.revision=prefix.lastAttemptMonotonicNs=prefix.lastSuccessMonotonicNs=1u;
      prefix.rowCount=prefix.totalRows=1u;prefix.rowBytes=sizeof(record);
      snapshot.rowBytes.resize(sizeof(record));std::memcpy(snapshot.rowBytes.data(),&record,sizeof(record));
      snapshots.push_back(std::move(snapshot));
    }
    std::string error;CXET_CHECK(corpus::writeRecorderReferences(snapshots,cut.referencePayload,error)==hftrec::Status::Ok);
  }
  void wholeMarket() {
    std::ifstream input(config);std::ostringstream text;text<<input.rdbuf();const auto bytes=text.str();
    const auto portfolio=bytes.find("[portfolio.");
    std::ofstream output(config,std::ios::trunc);output<<bytes.substr(0u,portfolio);
  }
  void appendPair(bool late) {
    std::string error;std::vector<corpus::RecorderReferenceSnapshot> snapshots;
    CXET_CHECK(corpus::readRecorderReferences(cut.referencePayload,snapshots,error)==hftrec::Status::Ok);
    ++cut.generation;const auto visible=late?mono+1'000'000'000u:mono;
    cut.committedMonotonicNs=visible;cut.progress.safeMonotonicNs=visible+1u;
    const auto original=snapshots.size();
    for(std::size_t leg=0;leg<2u;++leg) {
      auto source=cut.sources[leg];source.sourceId=leg+3u;source.canonicalSymbolId=leg+3u;
      source.economicBaseAssetId=2u;source.initiallyPresent=late?0u:1u;
      source.canonicalSymbol={};source.nativeSymbol={};
      std::copy_n(leg?"BBB_FUT":"BBB_RUB",8u,source.canonicalSymbol.data());
      std::copy_n(leg?"BBB_FUT":"BBB",leg?8u:4u,source.nativeSymbol.data());
      cut.sources.push_back(source);
      auto snapshot=snapshots[leg];hft_parser::ipc::InstrumentReferenceRecord first,second;
      CXET_CHECK(snapshot.record(0u,first));second=first;
      second.nativeSymbol.copyFrom(leg?"BBB_FUT":"BBB");second.instrument.nativeSymbol=second.nativeSymbol;
      second.metadata.nativeSymbol=second.nativeSymbol;
      second.instrument.providerInstrumentId[0]='b';second.metadata.providerInstrumentId[0]='b';
      if(leg){second.instrument.economicFacts.underlyingNativeSymbol.copyFrom("BBB");second.metadata.economicFacts=second.instrument.economicFacts;}
      CXET_CHECK(hft_parser::ipc::validReferenceRecord(second));
      snapshot.acquiredMonotonicNs=snapshot.visibleFromMonotonicNs=visible;
      snapshot.acquiredRealtimeNs=utc+visible-mono;snapshot.visibleFromCutGeneration=cut.generation;
      snapshot.producer.generation=cut.generation;auto& prefix=snapshot.producer.snapshot;
      prefix.revision=cut.generation;prefix.totalRows=prefix.rowCount=2u;
      snapshot.rowBytes.resize(2u*sizeof(first));std::memcpy(snapshot.rowBytes.data(),&first,sizeof(first));
      std::memcpy(snapshot.rowBytes.data()+sizeof(first),&second,sizeof(second));
      snapshots.push_back(std::move(snapshot));
    }
    CXET_CHECK(snapshots.size()==original+2u);
    CXET_CHECK(corpus::writeRecorderReferences(snapshots,cut.referencePayload,error)==hftrec::Status::Ok);
  }
  corpus::CorpusCut marginCut(std::int64_t amount,std::uint64_t visible,bool missing=false) const {
    auto out=cut;++out.generation;out.committedMonotonicNs=visible;out.progress.safeMonotonicNs=visible+1u;
    std::string error;std::vector<corpus::RecorderReferenceSnapshot> snapshots;
    CXET_CHECK(corpus::readRecorderReferences(cut.referencePayload,snapshots,error)==hftrec::Status::Ok);
    auto next=snapshots[1];next.acquiredMonotonicNs=next.visibleFromMonotonicNs=visible;
    next.acquiredRealtimeNs=utc+visible-mono;next.visibleFromCutGeneration=out.generation;
    next.producer.generation=out.generation;
    hft_parser::ipc::InstrumentReferenceRecord record;CXET_CHECK(next.record(0u,record));
    if(missing){next.presence=corpus::RecorderReferencePresence::Missing;next.rowBytes.clear();}
    else {
      record.metadata.financialFacts.buyInitialMarginE8=record.metadata.financialFacts.sellInitialMarginE8=amount;
      ++record.metadata.financialFacts.sourceFingerprint;
      std::memcpy(next.rowBytes.data(),&record,sizeof(record));
    }
    snapshots.push_back(std::move(next));
    CXET_CHECK(corpus::writeRecorderReferences(snapshots,out.referencePayload,error)==hftrec::Status::Ok);return out;
  }
  corpus::BinaryMarketRecord added(std::size_t leg,std::uint64_t when,std::uint64_t seq) const {
    auto record=quote(leg,when,seq);auto& header=record.header;
    header.channel=corpus::BinaryMarketChannel::SourceLifecycle;header.sourceFlags=0u;
    header.payloadBytes=sizeof(corpus::BinaryMarketSourceLifecyclePayload);record.payload={};
    auto& payload=*corpus::binaryMarketPayload<corpus::BinaryMarketSourceLifecyclePayload>(&record);
    payload.source=cut.sources[leg];payload.source.initiallyPresent=1u;
    payload.directoryRevision=2u;payload.kind=corpus::BinaryMarketSourceLifecycleKind::Added;
    payload.channelMaskAfter=payload.source.availableChannelMask;return record;
  }
  void cashNativeMargin(bool conflict=false) {
    std::ifstream input(config);std::ostringstream text;text<<input.rdbuf();auto bytes=text.str();
    for(std::size_t position=0u;(position=bytes.find("binance_spot",position))!=std::string::npos;position+=14u)
      bytes.replace(position,12u,"binance_margin");
    if(conflict)bytes="[strategy]\ntype=pair_api_probe\nenabled=true\nsizing.mode=anchor_lots\nsizing.value=1\nsizing.anchor_leg=1\nsizing.quote_currency=1\n"
        "[venue.binance_margin]\nsymbols=AAA_RUB,AAA_FUT\nrate_limits_enabled=false\n"
        "[portfolio.pair]\nlegs=binance_margin:AAA_RUB,binance_margin:AAA_FUT\n";
    {std::ofstream output(config,std::ios::trunc);output<<bytes;}
    std::string error;std::vector<corpus::RecorderReferenceSnapshot> snapshots;
    CXET_CHECK(corpus::readRecorderReferences(cut.referencePayload,snapshots,error)==hftrec::Status::Ok);
    for(std::size_t leg=0;leg<(conflict?2u:1u);++leg) {
      auto& source=cut.sources[leg];source.marketRaw=canon::kMarketTypeMargin.raw;
      source.market={};std::copy_n("margin",7u,source.market.data());source.marketBytes=6u;
      auto& snapshot=snapshots[leg];hft_parser::ipc::InstrumentReferenceRecord row;CXET_CHECK(snapshot.record(0u,row));
      row.product.marketRaw=canon::kMarketTypeMargin.raw;
      snapshot.product=row.product;snapshot.producer.snapshot.product=row.product;
      std::memcpy(snapshot.rowBytes.data(),&row,sizeof(row));
    }
    if(conflict) {
      auto& combined=snapshots[0];combined.producer.snapshot.totalRows=combined.producer.snapshot.rowCount=2u;
      combined.rowBytes.insert(combined.rowBytes.end(),snapshots[1].rowBytes.begin(),snapshots[1].rowBytes.end());snapshots.resize(1u);
    }
    CXET_CHECK(corpus::writeRecorderReferences(snapshots,cut.referencePayload,error)==hftrec::Status::Ok);
  }
  ~Fixture(){std::error_code ec;std::filesystem::remove(config,ec);}
  corpus::BinaryMarketRecord quote(std::size_t leg,std::uint64_t when,std::uint64_t seq) const {
    corpus::BinaryMarketRecord record;auto& h=record.header;
    h.sourceId=leg+1u;h.sourceGeneration=h.sessionEpoch=1u;
    h.receiveMonotonicNs=when;h.receiveRealtimeNs=utc+when-mono;h.exchangeTimestampNs=h.receiveRealtimeNs-100u;
    h.eventSequence=h.frameSequence=h.shardSequence=seq;h.channel=corpus::BinaryMarketChannel::BookTicker;
    h.flags=corpus::BinaryMarketRecordTraderReplayCompatible;
    h.sourceFlags=corpus::BinaryMarketSourceBookTickerBidPresent|corpus::BinaryMarketSourceBookTickerAskPresent;h.payloadBytes=sizeof(corpus::BinaryMarketBookTickerPayload);
    *corpus::binaryMarketPayload<corpus::BinaryMarketBookTickerPayload>(&record)=
        {(leg?110:99)*E,10*E,(leg?111:100)*E,10*E};
    return record;
  }
};

void chunkAndTimestampTiePreserveActorAndMetrics() {
  Fixture fixture;Transcript whole,split;SimulationSession a,b;
  CXET_CHECK(hft_backtest::isOk(a.prepare(fixture.request,fixture.cut,whole.sink())));
  CXET_CHECK(hft_backtest::isOk(b.prepare(fixture.request,fixture.cut,split.sink())));
  const std::array records{fixture.quote(0u,mono+100u,1u),fixture.quote(1u,mono+100u,2u),
      fixture.quote(0u,mono+60'000'000'100u,3u),fixture.quote(1u,mono+60'000'000'100u,4u),
      fixture.quote(0u,mono+60'000'001'100u,5u),fixture.quote(1u,mono+60'000'001'100u,6u)};
  hft_backtest::SimulationAdvance progress;
  CXET_CHECK(hft_backtest::isOk(a.advance(records,mono+60'000'002'100u,progress)));
  for(const auto& record:records)CXET_CHECK(hft_backtest::isOk(b.advance({&record,1u},0u,progress)));
  CXET_CHECK(hft_backtest::isOk(b.advance({},mono+60'000'002'100u,progress)));
  CXET_CHECK(a.result().orders==b.result().orders && a.result().fills==b.result().fills);
  CXET_CHECK(a.result().orders>=2u && a.result().fills>=2u);
  CXET_CHECK(whole.rows==split.rows);
  CXET_CHECK(a.accountSummaries().size()==2u && b.accountSummaries().size()==2u);
  CXET_CHECK(hft_backtest::isOk(a.finish()) && hft_backtest::isOk(b.finish()));
  CXET_CHECK(whole.rows==split.rows);
}
void pausedVariantLeavesIndependentPortfolioUntouched() {
  Fixture fixture;SimulationSession a,b;
  CXET_CHECK(hft_backtest::isOk(a.prepare(fixture.request,fixture.cut)));
  CXET_CHECK(hft_backtest::isOk(b.prepare(fixture.request,fixture.cut)));
  a.pause();const auto record=fixture.quote(0u,mono+100u,1u);hft_backtest::SimulationAdvance progress;
  CXET_CHECK(hft_backtest::isOk(a.advance({&record,1u},0u,progress)) && progress.recordsAccepted==0u && progress.paused);
  CXET_CHECK(hft_backtest::isOk(b.advance({&record,1u},0u,progress)) && progress.recordsAccepted==1u);
  CXET_CHECK(a.result().events==0u && b.result().events==1u);
  a.resume();CXET_CHECK(hft_backtest::isOk(a.advance({&record,1u},0u,progress)) && progress.recordsAccepted==1u);
}
void unsafeAnchorAndChangedPreparedClockAreRefused() {
  Fixture fixture;SimulationSession waiting,ready;
  auto unsafe=fixture.cut;unsafe.progress.safeMonotonicNs=0u;
  CXET_CHECK(waiting.prepare(fixture.request,unsafe)==hft_backtest::Status::NotReady);
  CXET_CHECK(hft_backtest::isOk(ready.prepare(fixture.request,fixture.cut)));
  auto changed=fixture.cut;++changed.generation;++changed.manifest.replayAnchorMonotonicNs;
  CXET_CHECK(ready.updateCut(changed)==hft_backtest::Status::CorruptData);
}
void riskStopBeforeWatermarkNeverRetimesFinalRows() {
  Fixture fixture;
  {std::ofstream out(fixture.config,std::ios::app);out << "[risk]\nenabled=true\nmin_equity_pct=100\n";}
  fixture.request.userDataLatency={1u,0u};
  Transcript transcript;SimulationSession session;
  CXET_CHECK(hft_backtest::isOk(session.prepare(fixture.request,fixture.cut,transcript.sink())));
  const std::array records{fixture.quote(0u,mono+100u,1u),fixture.quote(1u,mono+100u,2u),
      fixture.quote(0u,mono+60'000'000'100u,3u),fixture.quote(1u,mono+60'000'000'100u,4u),
      fixture.quote(0u,mono+60'000'001'100u,5u),fixture.quote(1u,mono+60'000'001'100u,6u)};
  hft_backtest::SimulationAdvance progress;
  const auto future=mono+600'000'000'000u;
  CXET_CHECK(hft_backtest::isOk(session.advance(records,future,progress)));
  CXET_CHECK(session.result().riskStopped && progress.paused && progress.terminalMonotonicNs>0u);
  CXET_CHECK(progress.terminalMonotonicNs<future && !session.result().strategyClosed);
  const auto terminal=progress.terminalMonotonicNs;
  session.resume();
  CXET_CHECK(hft_backtest::isOk(session.advance({},future,progress)) && progress.paused);
  CXET_CHECK(hft_backtest::isOk(session.finish()));
  CXET_CHECK(transcript.greatestEventMonotonic<=terminal);
}
void wholeMarketPairsAndDynamicAppendPreservePortfolio() {
  Fixture all;all.wholeMarket();all.appendPair(false);SimulationSession allSession;
  CXET_CHECK(hft_backtest::isOk(allSession.prepare(all.request,all.cut)));
  CXET_CHECK(allSession.result().universeSources==4u && allSession.result().universeContexts==2u);
  CXET_CHECK(allSession.accountSummaries().size()==2u);
  Fixture growing;growing.wholeMarket();SimulationSession session;
  CXET_CHECK(hft_backtest::isOk(session.prepare(growing.request,growing.cut)));
  hft_backtest::SimulationAdvance progress;
  const std::array initial{growing.quote(0u,mono+100u,1u),growing.quote(1u,mono+100u,2u)};
  CXET_CHECK(hft_backtest::isOk(session.advance(initial,0u,progress)));
  const auto wallet=session.result().walletBalanceE8;
  growing.appendPair(true);CXET_CHECK(hft_backtest::isOk(session.updateCut(growing.cut)));
  CXET_CHECK(session.result().universeContexts==2u && session.result().events==2u);
  CXET_CHECK(session.accountSummaries().size()==2u && session.result().walletBalanceE8==wallet);
  const auto when=mono+1'000'000'000u;
  const std::array addition{growing.added(2u,when,3u),growing.added(3u,when,4u),
      growing.quote(2u,when+100u,5u),growing.quote(3u,when+100u,6u)};
  CXET_CHECK(hft_backtest::isOk(session.advance(addition,when+101u,progress)));
  CXET_CHECK(session.result().events==6u && session.result().orders==0u);
}
void latestMarginChangeAndWithdrawalFenceDelayedActivation() {
  for(bool missing:{false,true}) {
    Fixture fixture;fixture.request.orderLatencyUs=2'000'000u;SimulationSession session;
    CXET_CHECK(hft_backtest::isOk(session.prepare(fixture.request,fixture.cut)));
    const std::array records{fixture.quote(0u,mono+100u,1u),fixture.quote(1u,mono+100u,2u),
        fixture.quote(0u,mono+60'000'000'100u,3u),fixture.quote(1u,mono+60'000'000'100u,4u),
        fixture.quote(0u,mono+60'000'001'100u,5u),fixture.quote(1u,mono+60'000'001'100u,6u)};
    hft_backtest::SimulationAdvance progress;
    CXET_CHECK(hft_backtest::isOk(session.advance(records,mono+60'000'002'100u,progress)));
    CXET_CHECK(session.result().orders==2u && session.result().fills==0u);
    const auto changed=fixture.marginCut(100*E,mono+61'000'000'000u,missing);
    CXET_CHECK(hft_backtest::isOk(session.updateCut(changed)));
    CXET_CHECK(hft_backtest::isOk(session.advance({},mono+64'000'000'000u,progress)));
    CXET_CHECK(session.result().legs.size()==2u && session.result().legs[1].fills==0u);
    CXET_CHECK(session.result().orders==2u);
  }
}
void initialMissingReferenceWaitIsTypedAndRetryable() {
  Fixture fixture;auto missing=fixture.cut;missing.referencePayload.clear();SimulationSession session;
  CXET_CHECK(session.prepare(fixture.request,missing)==hft_backtest::Status::NotReady);
  CXET_CHECK(hft_backtest::isOk(session.prepare(fixture.request,fixture.cut)));
  SimulationSession finalized;missing.finalized=true;
  CXET_CHECK(finalized.prepare(fixture.request,missing)==hft_backtest::Status::Unsupported);
}
void typedCashModelKeepsNativeProductAndRejectsSharedModelConflict() {
  Fixture fixture;fixture.cashNativeMargin();SimulationSession session;
  CXET_CHECK(hft_backtest::isOk(session.prepare(fixture.request,fixture.cut)));
  const std::array records{fixture.quote(0u,mono+100u,1u),fixture.quote(1u,mono+100u,2u),
      fixture.quote(0u,mono+60'000'000'100u,3u),fixture.quote(1u,mono+60'000'000'100u,4u),
      fixture.quote(0u,mono+60'000'001'100u,5u),fixture.quote(1u,mono+60'000'001'100u,6u)};
  hft_backtest::SimulationAdvance progress;
  CXET_CHECK(hft_backtest::isOk(session.advance(records,mono+60'000'002'100u,progress)));
  CXET_CHECK(session.result().legs[0].market=="margin" && session.result().legs[0].openPositionQtyE8==E);
  CXET_CHECK(session.accountSummaries()[0].market==canon::kMarketTypeMargin.raw);
  CXET_CHECK(session.accountSummaries()[0].walletBalanceE8==9'900*E);
  Fixture conflict;conflict.cashNativeMargin(true);SimulationSession rejected;
  CXET_CHECK(rejected.prepare(conflict.request,conflict.cut)==hft_backtest::Status::Unsupported);
  CXET_CHECK(rejected.result().error.find("incompatible captured accounting models")!=std::string::npos);
  Fixture rebate;
  for(bool scheduled:{false,true}) {
    auto request=rebate.request;
    if(scheduled)request.feeSchedules.push_back({"binance","spot",-1,0});
    else request.makerFeeBpsE8=-1;
    SimulationSession unsupported;
    CXET_CHECK(unsupported.prepare(request,rebate.cut)==hft_backtest::Status::Unsupported);
    CXET_CHECK(unsupported.result().error.find("rebate accounting model")!=std::string::npos);
  }
}
void researchEofCommitsFinalSnapshotWithoutLiquidation() {
  using namespace hft_backtest::research;
  Fixture fixture;
  const auto root=fixture.config.string()+"-research";
  struct Cleanup {std::filesystem::path root;~Cleanup(){std::error_code error;std::filesystem::remove_all(root,error);}} cleanup{root};
  corpus::BinaryMarketCorpusWriter writer;
  corpus::BinaryMarketWriterConfig writerConfig;
  writerConfig.root=std::filesystem::path(root)/"corpus";writerConfig.sources=fixture.cut.sources;
  writerConfig.producerEpoch=13u;writerConfig.maximumBytes=16u*1024u*1024u;
  writerConfig.segmentTargetBytes=64u*1024u;writerConfig.targetDurationNs=1'000'000'000'000u;
  writerConfig.startedMonotonicNs=mono;writerConfig.startedReceiveNs=utc;
  writerConfig.ringCapacity=1u;writerConfig.shardCount=1u;
  CXET_CHECK(hftrec::isOk(writer.start(writerConfig)));
  CXET_CHECK(hftrec::isOk(writer.setReferencePayload(fixture.cut.referencePayload)));
  const std::array records{fixture.quote(0u,mono+100u,1u),fixture.quote(1u,mono+100u,2u),
      fixture.quote(0u,mono+60'000'000'100u,3u),fixture.quote(1u,mono+60'000'000'100u,4u),
      fixture.quote(0u,mono+60'000'001'100u,5u),fixture.quote(1u,mono+60'000'001'100u,6u)};
  for(const auto& record:records)CXET_CHECK(hftrec::isOk(writer.append(record)));
  corpus::CorpusProducerProgress progress;progress.producerEpoch=13u;progress.shardCount=1u;
  progress.safeMonotonicNs=records.back().header.receiveMonotonicNs+1u;
  progress.shardSequences[0]=6u;progress.directoryRevision=1u;
  CXET_CHECK(hftrec::isOk(writer.updateProgress(progress)));
  CXET_CHECK(hftrec::isOk(writer.finalize(corpus::BinaryMarketStopReason::Requested,utc+61'000'000'000u)));
  ServiceConfig config;config.root=std::filesystem::path(root)/"store";
  config.buildIdentity="build";config.moduleIdentity="module";
  ResearchService service(config);std::string error;CXET_CHECK(service.restore(error));
  ExperimentSpec spec;spec.id="final-output";spec.module="pair_api_probe";
  spec.sourcePath=writerConfig.root;spec.sourceIdentity=corpus::corpusIdentity(writer.latestCut()->manifest);
  spec.buildIdentity=config.buildIdentity;spec.moduleIdentity=config.moduleIdentity;
  std::ifstream input(fixture.config);std::ostringstream text;text<<input.rdbuf();spec.rawConfigText=text.str();
  spec.initialBalanceE8=fixture.request.initialBalanceE8;spec.hasStrategyBudget=true;
  spec.strategyBudgetE8=10'000*E;spec.budgetCurrency="RUB";spec.valuationQuoteAssetId=1u;
  spec.fees={{"binance","spot",0,0},{"binance","futures",0,0}};
  CXET_CHECK(expandExperimentGrid(spec,error));Request request;request.command=Command::Create;request.spec=spec;
  CXET_CHECK(service.dispatch(request).ok);request.experimentId=spec.id;request.command=Command::Start;
  CXET_CHECK(service.dispatch(request).ok);service.step();request.command=Command::Status;
  const auto status=service.dispatch(request);CXET_CHECK(status.ok&&status.experiments[0].state==ExperimentState::Stopped);
  request.command=Command::Range;request.resultKind=ResultKind::Exposure;request.limit=4096u;
  const auto rows=service.dispatch(request);CXET_CHECK(rows.ok);
  std::size_t finalSnapshots=0u;
  for(const auto& row:rows.rows)if(row.metric=="risk_snapshots"&&row.payload.ends_with(",4]\n")) {
    ++finalSnapshots;CXET_CHECK(row.inputCut<status.experiments[0].visibleInputCut);
  }
  CXET_CHECK(finalSnapshots==2u);
}
constexpr cxet::testing::Case cases[]{
  {"simulation_chunks_ties_preserve_actor_orders_fills_metrics",chunkAndTimestampTiePreserveActorAndMetrics},
  {"simulation_pause_variants_have_independent_accounts",pausedVariantLeavesIndependentPortfolioUntouched},
  {"simulation_requires_frozen_authoritative_receive_anchor",unsafeAnchorAndChangedPreparedClockAreRefused},
  {"simulation_riskstop_freezes_final_rows_before_future_watermark",riskStopBeforeWatermarkNeverRetimesFinalRows},
  {"simulation_wholemarket_pairs_and_dynamic_append_preserve_portfolio",wholeMarketPairsAndDynamicAppendPreservePortfolio},
  {"simulation_latest_margin_and_withdrawal_fence_delayed_activation",latestMarginChangeAndWithdrawalFenceDelayedActivation},
  {"simulation_initial_missing_reference_wait_is_retryable",initialMissingReferenceWaitIsTypedAndRetryable},
  {"simulation_typed_cash_native_product_and_shared_model_conflict",typedCashModelKeepsNativeProductAndRejectsSharedModelConflict},
  {"research_eof_commits_final_snapshot_without_liquidation",researchEofCommitsFinalSnapshotWithoutLiquidation},
};
}
int main(int argc,char** argv){return cxet::testing::runCases(argc,argv,cases);}
