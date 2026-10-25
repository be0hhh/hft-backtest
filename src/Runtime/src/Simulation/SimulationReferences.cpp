#include "SimulationReferences.hpp"
#include "cxet/Network/Common/NativeOrderQuantity.hpp"
#include "cxet/Parse/DecimalToScaled.hpp"
#include <algorithm>
#include <cstring>
#include <limits>
namespace hft_backtest::detail {
namespace {
// Strict exact decimal conversion; metadata is cold, bounded fixed text.
bool decimalE8(const char* text,std::size_t capacity,std::int64_t& out,bool optional=false) {
  out=0;if(!text[0])return optional;
  std::size_t bytes=0u;while(bytes<capacity && text[bytes])++bytes;
  return bytes<capacity && cxet::parse::parseUnsignedScaledDecimalStrict(
      cxet::StringView{text,bytes},8u,&out);
}
bool normalizeNative(std::int64_t value,const hftrec::corpus::BinaryMarketSource& source,std::int64_t& out) {
  const auto& conversion=source.quantityConversion;
  if(!conversion.canonicalBaseMultiplier || !conversion.nativeBaseMultiplier || value<0)return false;
  const __int128 numerator=static_cast<__int128>(value)*conversion.nativeBaseMultiplier;
  if(numerator%conversion.canonicalBaseMultiplier)return false;
  const auto result=numerator/conversion.canonicalBaseMultiplier;
  if(result>std::numeric_limits<std::int64_t>::max())return false;
  out=static_cast<std::int64_t>(result);return true;
}
trading_core::DeliveryBasisSnapshotView view(const hft_parser::reference_contract::DeliveryReferenceSnapshot& row,
    trading_core::DeliveryBasisSnapshotKind kind) {
  return {row.points.data(),row.effectiveRealtimeNs,row.fingerprint,row.tradeDateYmd,
    row.updateDateYmd,row.updateTimeMs,row.sessionCalendarDateYmd,row.sessionTradeDateYmd,
    row.sessionEvidenceFlags,0u,row.sessionEvidenceRealtimeNs,row.pointCount,kind,0u};
}
}
Status SimulationReferences::update(const hftrec::corpus::CorpusCut& cut,std::string& error) {
  std::vector<hftrec::corpus::RecorderReferenceSnapshot> next;
  const auto status=hftrec::corpus::readRecorderReferences(cut.referencePayload,next,error);
  if(!hftrec::isOk(status))return binary_corpus::convertStatus(status);
  if(cut.committedMonotonicNs<committedMonotonicNs) {error="reference commit time regressed";return Status::CorruptData;}
  snapshots=std::move(next);committedMonotonicNs=cut.committedMonotonicNs;return Status::Ok;
}
Status SimulationReferences::listing(const hftrec::corpus::BinaryMarketSource& source,
    cxet::api::InstrumentUniverseRow& output,bool active,std::string& error) const {
  using namespace hft_parser::ipc;
  for(const auto& snapshot:snapshots) {
    if(snapshot.dataset!=ReferenceDatasetKind::Instruments || snapshot.product.exchangeRaw!=source.venueId ||
        snapshot.product.marketRaw!=source.marketRaw || snapshot.presence!=hftrec::corpus::RecorderReferencePresence::Present)continue;
    for(std::size_t index=0;index<snapshot.producer.snapshot.totalRows;++index) {
      InstrumentReferenceRecord row;
      if(!snapshot.record(index,row)){error="captured typed listing is malformed";return Status::CorruptData;}
      if(cxet::StringView::fromCString(row.nativeSymbol.data)==cxet::StringView{source.nativeSymbol.data(),source.nativeSymbolBytes}) {
        output=row.instrument;return Status::Ok;
      }
    }
  }
  error="awaiting captured normalized instrument listing";return active?Status::NotReady:Status::Unsupported;
}
bool SimulationReferences::instrumentAvailable(const hftrec::corpus::BinaryMarketSource& source,std::uint64_t nowMono) const {
  using namespace hft_parser::ipc;
  for(auto it=snapshots.rbegin();it!=snapshots.rend();++it) {
    const auto& snapshot=*it;
    if(snapshot.dataset!=ReferenceDatasetKind::Instruments || snapshot.product.exchangeRaw!=source.venueId ||
        snapshot.product.marketRaw!=source.marketRaw || snapshot.visibleFromMonotonicNs>nowMono)continue;
    if(snapshot.presence!=hftrec::corpus::RecorderReferencePresence::Present)return false;
    for(std::size_t index=0;index<snapshot.producer.snapshot.totalRows;++index) {
      InstrumentReferenceRecord record;
      if(!snapshot.record(index,record))return false;
      if(cxet::StringView::fromCString(record.nativeSymbol.data)==
          cxet::StringView{source.nativeSymbol.data(),source.nativeSymbolBytes})
        return record.hasMetadata && (record.observation.freshness&1u) && record.observation.reason==0u;
    }
    return false;
  }
  return false;
}
bool SimulationReferences::instrument(const hftrec::corpus::BinaryMarketSource& source,
    trading_core::StrategyInstrumentSpec& spec,BacktestCanonicalSource& output,
    std::uint64_t& visibleMono,std::string& error,std::uint64_t nowMono) const {
  using namespace hft_parser::ipc;
  const auto native=binary_corpus::fixedText(source.nativeSymbol.data(),source.nativeSymbol.size(),source.nativeSymbolBytes);
  for(std::size_t ordinal=0u;ordinal<snapshots.size();++ordinal) {
    const auto& snapshot=snapshots[nowMono?snapshots.size()-1u-ordinal:ordinal];
    if(snapshot.product.exchangeRaw!=source.venueId || snapshot.product.marketRaw!=source.marketRaw ||
        snapshot.dataset!=ReferenceDatasetKind::Instruments || (nowMono && snapshot.visibleFromMonotonicNs>nowMono))continue;
    if(snapshot.presence!=hftrec::corpus::RecorderReferencePresence::Present) {
      if(nowMono){error="latest visible instrument authority unavailable";return false;}
      continue;
    }
    for(std::size_t i=0;i<snapshot.producer.snapshot.totalRows;++i) {
      InstrumentReferenceRecord record;
      if(!snapshot.record(i,record)){error="captured instrument reference row invalid";return false;}
      if(std::string(record.nativeSymbol.data)!=std::string(native))continue;
      if(!record.hasMetadata || (nowMono && (!(record.observation.freshness&1u) || record.observation.reason))) {
        error="captured instrument lacks current typed native sizing authority";return false;
      }
      spec.expiryUtcNs=record.instrument.expiryRealtimeNs;
      const auto& meta=record.metadata;std::int64_t nativeLot{};
      if(!decimalE8(meta.nativeLotBaseQty,sizeof(meta.nativeLotBaseQty),nativeLot) ||
          !normalizeNative(nativeLot,source,spec.nativeLotBaseQtyRaw) || spec.nativeLotBaseQtyRaw<=0) {
        error="captured native trading lot is unavailable or inexact";return false;
      }
      for(const auto [text,bound]: {std::pair{meta.minQty,&spec.minQtyRaw},std::pair{meta.maxQty,&spec.maxQtyRaw}}) {
        std::int64_t value{};
        if(!decimalE8(text,rawdata::SmallBufLen,value,true) ||
            !cxet::network::common::canonicalNativeQuantityRawExact(value,
              source.quantityConversion.nativeContractBaseQtyRaw,meta.nativeQuantityUnit,
              source.quantityConversion.canonicalBaseMultiplier,source.quantityConversion.nativeBaseMultiplier,
              bound,source.quantityConversion.nativeLotBaseQtyRaw)) {
          error="captured native quantity bound cannot be normalized exactly";return false;
        }
      }
      if(!decimalE8(meta.minNotional,sizeof(meta.minNotional),spec.minNotionalRaw,true) ||
          !decimalE8(meta.maxNotional,sizeof(meta.maxNotional),spec.maxNotionalRaw,true)) {
        error="captured notional limits invalid";return false;
      }
      if(!meta.hasQuoteCurrency) {error="captured quote currency is unavailable";return false;}
      std::copy_n(meta.quoteCurrency,sizeof(spec.quoteCurrency.data),spec.quoteCurrency.data);
      if(!spec.quoteCurrency.valid()) {error="captured quote currency is invalid";return false;}
      if(!trading_core::prepareStrategyInstrumentSpecDerived(spec)) {error="captured sizing grid invalid";return false;}
      output.instrument=record.instrument;
      auto& sizing=output.sizing;
      sizing.grid={spec.stepSizeRaw,spec.executionQuantityStepRaw,spec.qtyStepGrid,spec.executionQuantityGrid};
      sizing.normalizedBasePerLotRaw=spec.contractBaseQtyRaw>0?spec.contractBaseQtyRaw:spec.nativeLotBaseQtyRaw;
      sizing.nativeLotBaseQtyRaw=spec.nativeLotBaseQtyRaw;sizing.priceBasisQtyRaw=spec.priceBasisQtyRaw;
      sizing.quoteCurrency=source.quoteAssetId;
      sizing.minQtyRaw=spec.minQtyRaw;sizing.maxQtyRaw=spec.maxQtyRaw;
      sizing.minNotionalRaw=spec.minNotionalRaw;sizing.maxNotionalRaw=spec.maxNotionalRaw;
      const auto& financial=meta.financialFacts;
      std::int64_t initial{},basis{};
      // The adapter needs both-side conservative authority; asymmetric native
      // margins are selected by the concrete canonical cycle, never guessed.
      if(cxet::api::instrumentInitialMargin(financial,true,initial,basis) &&
          (financial.presenceMask&cxet::api::kInstrumentFinancialSellInitialMargin) &&
          cxet::StringView::fromCString(financial.currency)==cxet::StringView::fromCString(meta.quoteCurrency)) {
        sizing.initialCapitalRaw=std::max(initial,financial.sellInitialMarginE8);
        if(!normalizeNative(basis,source,sizing.capitalBasisQtyRaw)) {error="captured margin basis inexact";return false;}
        sizing.capitalCurrency=source.quoteAssetId;sizing.capitalRequirementKnown=true;
      }
      visibleMono=snapshot.visibleFromMonotonicNs;return true;
    }
    if(nowMono){error="latest visible instrument snapshot withdrew this listing";return false;}
  }
  error="simulation requires a captured typed instrument reference";return false;
}
bool SimulationReferences::centralBank(SimulationCentralBank& owner,std::uint64_t nowMono,std::uint64_t nowUtc,std::string& error) const {
  using namespace hft_parser::ipc;
  for(auto it=snapshots.rbegin();it!=snapshots.rend();++it) {
    const auto& snapshot=*it;
    if(snapshot.dataset!=ReferenceDatasetKind::CentralBankRate ||
        nowMono<snapshot.visibleFromMonotonicNs)continue;
    if(snapshot.presence!=hftrec::corpus::RecorderReferencePresence::Present) {
      error="latest visible captured reference explicitly unavailable";return false;
    }
    if(!snapshot.producer.snapshot.totalRows || snapshot.producer.snapshot.totalRows>kMaximumCentralBankRateProjectionRows) {
      error="captured central-bank curve capacity refused";return false;
    }
    CentralBankRateReferenceRecord first;
    if(!snapshot.record(0u,first))return false;
    const auto activationMono=snapshot.visibleFromMonotonicNs;
    const auto activationUtc=nowUtc>=nowMono-activationMono?nowUtc-(nowMono-activationMono):0u;
    if(owner.fingerprint==first.historyFingerprint && owner.reference.currentActivationRealtimeNs==activationUtc)return true;
    owner.previous=std::move(owner.current);owner.current.clear();
    owner.reference.previousView=owner.reference.currentView;
    owner.reference.previousActivationRealtimeNs=owner.reference.currentActivationRealtimeNs;
    owner.reference.previousObservedRealtimeNs=owner.reference.currentObservedRealtimeNs;
    owner.reference.previousState=owner.reference.currentState;
    for(std::size_t i=0;i<snapshot.producer.snapshot.totalRows;++i) {
      CentralBankRateReferenceRecord row;
      if(!snapshot.record(i,row) || row.historyFingerprint!=first.historyFingerprint) {
        error="captured central-bank curve identity invalid";return false;
      }
      owner.current.push_back({row.effectiveRealtimeNs,row.annualRateQ9});
    }
    const auto state=(first.observation.freshness&1u) && first.observation.reason==0u
        ?trading_core::DeliveryValuationCentralBankState::Ready:trading_core::DeliveryValuationCentralBankState::Stale;
    if(!trading_core::qualifyDeliveryValuationCentralBankFrame(owner.current.data(),
          static_cast<std::uint32_t>(owner.current.size()),first.historyFingerprint,
          first.observation.observedRealtimeNs,state,&owner.reference.currentView)) {
      error="captured central-bank curve failed canonical qualification";return false;
    }
    owner.fingerprint=first.historyFingerprint;owner.reference.currentState=state;
    owner.reference.currentObservedRealtimeNs=first.observation.observedRealtimeNs;
    owner.reference.currentActivationRealtimeNs=activationUtc;return true;
  }
  error="central-bank reference unavailable at recorded receive boundary";return false;
}
bool SimulationReferences::delivery(std::span<const std::size_t> indices,const SimulationInput& input,
    trading_core::DeliveryValuationReference& output,std::uint64_t nowMono,std::uint64_t nowUtc,std::string& error) const {
  using namespace hft_parser::ipc;
  if(indices.size()!=2u)return false;
  for(auto it=snapshots.rbegin();it!=snapshots.rend();++it) {
    const auto& snapshot=*it;
    const auto& future=input.sources[indices[1]].selection.source;
    if(snapshot.dataset!=ReferenceDatasetKind::DeliveryBasis || snapshot.product.exchangeRaw!=future.venueId ||
        snapshot.product.marketRaw!=future.marketRaw || nowMono<snapshot.visibleFromMonotonicNs)continue;
    if(snapshot.presence!=hftrec::corpus::RecorderReferencePresence::Present) {
      error="latest visible captured reference explicitly unavailable";return false;
    }
    for(std::size_t i=0;i<snapshot.producer.snapshot.totalRows;++i) {
      DeliveryBasisReferenceRecord record;
      if(!snapshot.record(i,record)){error="captured delivery reference invalid";return false;}
      std::uint32_t future{},spot{};
      for(auto index:indices) {
        const auto& source=input.sources[index].selection.source;
        const auto native=binary_corpus::fixedText(source.nativeSymbol.data(),source.nativeSymbol.size(),source.nativeSymbolBytes);
        if(source.venueId==record.product.exchangeRaw && source.marketRaw==record.product.marketRaw && native==record.nativeSymbol.data)future=source.sourceId;
        if(native==record.nativeSpotSymbol.data && source.marketRaw==canon::kMarketTypeSpot.raw)spot=source.sourceId;
      }
      if(!future || !spot)continue;
      const auto& value=record.value;
      trading_core::DeliveryValuationReferenceInput qualified;
      qualified.futureSourceId=future;qualified.spotSourceId=spot;
      qualified.curve=view(value.curve,trading_core::DeliveryBasisSnapshotKind::Curve);
      qualified.cashflows=view(value.cashflows,trading_core::DeliveryBasisSnapshotKind::Cashflow);
      qualified.terms={value.lastTradeStartRealtimeNs,value.deliveryStartRealtimeNs,value.termsFingerprint};
      qualified.termsPresent=(value.presentMask&hft_parser::reference_contract::DeliveryReferenceTermsPresent)!=0u;
      qualified.modelPresent=(value.presentMask&(hft_parser::reference_contract::DeliveryReferenceCurvePresent|
          hft_parser::reference_contract::DeliveryReferenceCashflowsPresent))==
          (hft_parser::reference_contract::DeliveryReferenceCurvePresent|hft_parser::reference_contract::DeliveryReferenceCashflowsPresent);
      qualified.observedRealtimeNs=record.observation.observedRealtimeNs;qualified.nowRealtimeNs=nowUtc;
      qualified.observationFresh=(record.observation.freshness&1u)!=0u;
      qualified.providerHealthy=record.observation.reason==0u;
      const auto model=trading_core::qualifyDeliveryValuationReference(qualified);
      const auto activation=nowUtc>=nowMono-snapshot.visibleFromMonotonicNs?
          nowUtc-(nowMono-snapshot.visibleFromMonotonicNs):0u;
      if(output.currentActivationRealtimeNs!=activation || output.current.revision!=model.revision) {
        output.previous=output.current;output.previousActivationRealtimeNs=output.currentActivationRealtimeNs;
        output.current=model;output.currentActivationRealtimeNs=activation;
      }
      return true;
    }
  }
  error="delivery valuation reference is unavailable at this recorded receive boundary";return false;
}
}
