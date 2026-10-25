#include "ResearchStorage.hpp"
#include "hft_backtest/SimulationSession.hpp"
#include "hft_backtest/DataStudy.hpp"
#include "hftrec/CorpusContract/CorpusCut.hpp"
#include "hftrec/Service/RecorderService.hpp"
#include "../../Runtime/src/Input/Binary/Internal/BinaryCorpusHelpers.hpp"
#include <algorithm>
#include <charconv>
#include <limits>
#include <map>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>

namespace hft_backtest::research {
namespace {
using namespace hftrec::corpus;
struct OutputCut {std::uint64_t generation{},segments{},rows{},inputCut{},bytes{},inputFingerprint{},sourceCount{};};
std::vector<std::uint8_t> encodeCut(OutputCut cut){detail::Codec c;c.integer(cut.generation);c.integer(cut.segments);c.integer(cut.rows);c.integer(cut.inputCut);c.integer(cut.bytes);c.integer(cut.inputFingerprint);c.integer(cut.sourceCount);detail::Codec framed;auto hash=detail::fingerprint(c.bytes);framed.integer(hash);framed.bytes.insert(framed.bytes.end(),c.bytes.begin(),c.bytes.end());return std::move(framed.bytes);}
OutputCut decodeCut(std::vector<std::uint8_t> bytes){detail::Codec c(true);c.bytes=std::move(bytes);std::uint64_t hash{};c.integer(hash);if(detail::fingerprint(std::span<const std::uint8_t>(c.bytes).subspan(c.offset))!=hash)throw std::runtime_error("output cut checksum mismatch");OutputCut cut;c.integer(cut.generation);c.integer(cut.segments);c.integer(cut.rows);c.integer(cut.inputCut);c.integer(cut.bytes);c.integer(cut.inputFingerprint);c.integer(cut.sourceCount);c.done();return cut;}
std::uint64_t prefixFingerprint(const CorpusCut& cut,std::uint64_t safe,std::uint64_t sources){
 if(sources>cut.sources.size())throw std::runtime_error("restored input source prefix is missing");
 std::uint64_t hash=14695981039346656037ULL;
 auto mix=[&](const auto& value){const auto bytes=std::as_bytes(std::span{&value,1});for(auto b:bytes){hash^=std::to_integer<std::uint8_t>(b);hash*=1099511628211ULL;}};
 mix(cut.manifest.producerEpoch);mix(cut.manifest.replayAnchorReceiveNs);mix(cut.manifest.replayAnchorMonotonicNs);mix(safe);
 for(std::size_t i=0;i<sources;++i)mix(cut.sources[i]);
 for(const auto& block:cut.blocks)if(block.firstMonotonicNs<safe){mix(block.offset);mix(block.segmentNumber);mix(block.shardIndex);mix(block.header);}
 return hash;
}
std::string sourceIdentity(const CorpusCut& cut){return corpusIdentity(cut.manifest);}
struct Variant {
 std::uint64_t id{},lastMetricMonotonic{};
 std::int64_t previousEquity{};bool hasPreviousEquity{},terminal{};
 std::vector<std::uint64_t> returnBins;
 std::unique_ptr<SimulationSession> simulation;
 std::unique_ptr<DataStudySession> study;
};
struct Experiment {
 ExperimentSpec spec;ExperimentStatus status;std::filesystem::path directory,attempt;
 CorpusActiveCursor cursor;std::shared_ptr<const CorpusCut> cut;
 std::unique_ptr<hftrec::service::RecorderServiceClient> recorder;
 std::vector<std::unique_ptr<Variant>> variants;
 std::vector<ResultRow> pending;std::size_t pendingBytes{};
 std::vector<std::uint32_t> maximumDepthCoverageSourcesBySource;
 OutputCut output;
 std::uint64_t targetCut{},restartVisibleCut{},waitingCutGeneration{};
 std::string waitingReason;
 bool initialized{},atBoundary{true},residentReservation{},compatible{true};
 std::string writeError;
};
struct SinkContext {void* service{};Experiment* experiment{};Variant* variant{};};
std::filesystem::path segmentPath(const std::filesystem::path& attempt,std::uint64_t i){return attempt/("segment-"+std::to_string(i)+".bin");}
std::filesystem::path generationPath(const Experiment& e,std::uint64_t generation){return e.directory/("attempt-"+std::to_string(generation));}
std::filesystem::path visibleCutPath(const Experiment& e){
 const auto attempt=generationPath(e,e.status.visibleGeneration);
 const auto name="cut-"+std::to_string(e.status.visibleInputCut);
 const auto final=attempt/(name+"-final.bin");
 return e.status.visibleFinal?final:attempt/(name+".bin");
}
bool gapIntersects(const Experiment& e,const CorpusCut& cut){for(const auto& gap:cut.gaps)if((gap.minimumDroppedReceiveNs==0&&gap.maximumDroppedReceiveNs==0)||gap.maximumDroppedReceiveNs>=e.spec.beginReceiveNs)return true;return false;}
}
struct ResearchService::Impl {
 ServiceConfig config;std::map<std::string,std::unique_ptr<Experiment>> experiments;std::uint64_t diskBytes{};std::size_t schedule{};int lease{-1};bool ready{};
 explicit Impl(ServiceConfig c):config(std::move(c)){}
 ~Impl(){if(lease>=0)::close(lease);}
 bool admit(const ExperimentSpec* newCatalog,const Experiment* activating,std::string& error)const{
  std::uint64_t bytes=16ULL*(config.maxRequestBytes+detail::kMaxWireBytes)+4ULL*detail::kMaxWireBytes;std::size_t variants=0;
  auto charge=[&](std::uint64_t amount){if(amount>config.maxResidentStateBytes||bytes>config.maxResidentStateBytes-amount)return false;bytes+=amount;return true;};
  if(bytes>config.maxResidentStateBytes){error="service bounded control buffers exceed resident byte quota";return false;}
  auto catalog=[&](const ExperimentSpec& spec){std::uint64_t amount=2*detail::encode(spec).size()+sizeof(Experiment)+(16U<<10)+spec.variants.size()*(sizeof(ExperimentVariant)+sizeof(VariantStatus))+spec.fees.size()*sizeof(ScenarioFee);for(const auto& variant:spec.variants)amount+=variant.parameters.size()*sizeof(ParameterValue);return charge(amount);};
  auto compute=[&](const ExperimentSpec& spec){variants+=spec.variants.size();if(!spec.budget.maxStateBytes||spec.variants.size()>config.maxResidentStateBytes/spec.budget.maxStateBytes)return false;if(!charge(spec.budget.maxStateBytes*spec.variants.size())||!charge(spec.budget.maxInputBytes)||!charge(4U<<20))return false;return true;};
  for(const auto& [id,e]:experiments){if(!catalog(e->spec)){error="saved experiment catalog metadata exceeds resident byte quota";return false;}if((e->residentReservation||e.get()==activating)&&!compute(e->spec)){error="service resident state/input/result byte quota exceeded";return false;}}
  if(newCatalog&&!catalog(*newCatalog)){error="saved experiment catalog metadata exceeds resident byte quota";return false;}
  // During restore the candidate is not yet in the catalog.
  if(activating&&!experiments.contains(activating->spec.id)&&!compute(activating->spec)){error="service resident state/input/result byte quota exceeded";return false;}
  if(variants>config.maxResidentVariants){error="service resident variant quota exceeded";return false;}return true;
 }
 void releaseCompute(Experiment& e){
  e.initialized=false;e.residentReservation=false;std::vector<std::unique_ptr<Variant>>{}.swap(e.variants);std::vector<ResultRow>{}.swap(e.pending);e.pendingBytes=0;sinks.erase(&e);e.cursor=CorpusActiveCursor{};e.cut.reset();e.recorder.reset();e.waitingReason.clear();e.waitingCutGeneration=0;
 }
 bool finishInput(Experiment& e){
  for(auto& variant:e.variants)if(variant->simulation && !isOk(variant->simulation->finish())){
   e.writeError=variant->simulation->result().error;return false;
  }
  return flush(e);
 }
 void endInput(Experiment& e){
  if(!finishInput(e)){fence(e,ExperimentState::Failed,e.writeError);return;}
  const bool complete=e.cut&&e.cut->manifest.complete!=0;
  e.status.state=complete?ExperimentState::Stopped:ExperimentState::Incomplete;e.status.backgroundEnabled=false;
  e.status.diagnostic=complete?"input finalized; positions retained without synthetic liquidation":"capture finalized incomplete; native stop reason="+std::to_string(static_cast<unsigned>(e.cut->manifest.stopReason));
  if(!commit(e,e.targetCut,true)){fence(e,ExperimentState::Failed,e.writeError);return;}releaseCompute(e);
 }
 bool write(const std::filesystem::path& p,std::span<const std::uint8_t>b,bool replace,std::string& error){
  std::error_code ec;std::uint64_t old=0;if(replace&&std::filesystem::exists(p,ec))old=std::filesystem::file_size(p,ec);
  if(ec||old>diskBytes){error="invalid durable file accounting";return false;}
  const auto name=p.filename().string();const bool control=name=="state.bin"||name=="output-cut.bin";
  const std::uint64_t reserve=std::uint64_t(config.maxExperiments)*(16U<<10)+std::uint64_t(config.maxResidentVariants)*(64U<<10);
  if(reserve>config.maxServiceOutputBytes){error="finite storage quota cannot reserve control metadata";return false;}
  const auto limit=control?config.maxServiceOutputBytes:config.maxServiceOutputBytes-reserve;
  const auto current=diskBytes-old;if(b.size()>limit||current>limit-b.size()){error="service durable storage quota exceeded";return false;}
  if(!detail::durableWrite(p,b,replace,error))return false;diskBytes=current+b.size();return true;
 }
 bool save(Experiment& e,std::string& error){return write(e.directory/"state.bin",detail::encode(e.status),true,error);}
 void fence(Experiment& e,ExperimentState state,std::string reason){
  e.status.state=state;e.status.backgroundEnabled=false;if(reason.size()>4096)reason.resize(4096);e.status.diagnostic=std::move(reason);
  for(auto& v:e.variants)if(v->simulation)v->simulation->pause();
  try{(void)flush(e);}catch(const std::exception& exception){if(e.status.diagnostic.size()<3000)e.status.diagnostic+="; output fence: "+std::string(exception.what());}
  std::string error;if(!save(e,error)){e.status.diagnostic+="; state persistence failed: "+error;if(e.status.diagnostic.size()>4096)e.status.diagnostic.resize(4096);return;}releaseCompute(e);
 }
 bool flush(Experiment& e){
  if(e.pending.empty())return true;detail::Codec codec;codec.list(e.pending,4096,[&](auto& row){detail::code(codec,row);});const auto hash=detail::fingerprint(codec.bytes);detail::Codec framed;std::uint64_t mutableHash=hash;framed.integer(mutableHash);framed.bytes.insert(framed.bytes.end(),codec.bytes.begin(),codec.bytes.end());codec.bytes=std::move(framed.bytes);
  if(codec.bytes.size()>e.spec.budget.maxOutputBytes||e.status.outputBytes>e.spec.budget.maxOutputBytes-codec.bytes.size()){e.writeError="experiment output byte quota exceeded";return false;}
  if(!write(segmentPath(e.attempt,e.output.segments),codec.bytes,false,e.writeError))return false;
  ++e.output.segments;e.output.rows+=e.pending.size();e.output.bytes+=codec.bytes.size();e.status.outputBytes+=codec.bytes.size();e.pending.clear();e.pendingBytes=0;return true;
 }
 bool emit(Experiment& e,ResultRow row){
  if(e.output.rows+e.pending.size()>=e.spec.budget.maxResultRows){if(!flush(e))return false;e.writeError="experiment result row quota exceeded";return false;}
  const auto bytes=row.payload.size()+row.metric.size()+row.exchange.size()+row.market.size()+row.instrument.size()+256;if(bytes>detail::kMaxWireBytes/2){if(!flush(e))return false;e.writeError="single result row exceeds byte bound";return false;}if(e.pendingBytes+bytes>detail::kMaxWireBytes/2&&!flush(e))return false;if(bytes>e.spec.budget.maxOutputBytes||e.status.outputBytes+e.pendingBytes>e.spec.budget.maxOutputBytes-bytes){if(!flush(e))return false;e.writeError="experiment output byte quota exceeded";return false;}e.pendingBytes+=bytes;
  if(!row.inputCut)row.inputCut=e.targetCut;for(auto& status:e.status.variants)if(status.id==row.variant){++status.rows;break;}e.pending.push_back(std::move(row));if(e.pending.size()>=e.spec.budget.resultBatchRows)return flush(e);return true;
 }
 static bool sink(std::uint32_t source,std::uint64_t monotonic,cxet::StringView stream,cxet::StringView json,void* user)noexcept{
  auto& c=*static_cast<SinkContext*>(user);auto& self=*static_cast<Impl*>(c.service);auto& e=*c.experiment;
  try{ResultRow row;row.variant=c.variant->id;row.source=source;row.inputCut=monotonic;const __int128 realtime=static_cast<__int128>(e.cut->manifest.replayAnchorReceiveNs)+static_cast<__int128>(monotonic)-e.cut->manifest.replayAnchorMonotonicNs;if(realtime<std::numeric_limits<std::int64_t>::min()||realtime>std::numeric_limits<std::int64_t>::max())return false;row.timestampNs=static_cast<std::int64_t>(realtime);qualify(e,row);row.metric.assign(stream.data(),stream.size());row.payload.assign(json.data(),json.size());row.kind=row.metric.find("fill")!=std::string::npos?ResultKind::Fill:ResultKind::Exposure;return self.emit(e,std::move(row));}catch(const std::exception& x){e.writeError=x.what();return false;}
 }
 static void qualify(const Experiment& e,ResultRow& row){for(const auto& source:e.cut->sources)if(source.sourceId==row.source){row.currencyAssetId=source.quoteAssetId;row.exchange.assign(source.venue.data(),source.venueBytes);row.market.assign(source.market.data(),source.marketBytes);row.instrument.assign(source.canonicalSymbol.data(),source.canonicalSymbolBytes);return;}}
 static bool metrics(const BacktestRunResult& r,std::uint64_t monotonic,std::int64_t realtime,std::int64_t equity,std::int64_t cumulativeReturn,std::uint32_t quote,void* user)noexcept{
  auto& context=*static_cast<SinkContext*>(user);auto& self=*static_cast<Impl*>(context.service);auto& e=*context.experiment;auto& v=*context.variant;
  try{if(v.lastMetricMonotonic&&monotonic/e.spec.studyIntervalNs==v.lastMetricMonotonic/e.spec.studyIntervalNs)return true;
   if(quote!=e.spec.valuationQuoteAssetId){e.writeError="admitted valuation currency changed";return false;}
   auto metric=[&](ResultKind kind,const char* name,std::int64_t value,std::uint64_t source=0){ResultRow row;row.kind=kind;row.variant=v.id;row.source=source;row.inputCut=monotonic;row.timestampNs=realtime;row.metric=name;row.value=value;row.currencyAssetId=quote;if(source)qualify(e,row);return self.emit(e,std::move(row));};
   const std::pair<const char*,std::int64_t> financial[]{
    {"equity_e8",equity},{"cumulative_return_bps_e8",cumulativeReturn},{"initial_balance_e8",r.initialBalanceE8},{"wallet_balance_e8",r.walletBalanceE8},
    {"total_pnl_e8",r.totalPnlE8},{"realized_pnl_e8",r.realizedPnlE8},{"unrealized_pnl_e8",r.unrealizedPnlE8},
    {"gross_realized_pnl_e8",r.grossRealizedPnlE8},{"net_realized_pnl_e8",r.netRealizedPnlE8},{"fees_paid_e8",r.feesPaidE8},
    {"max_drawdown_e8",r.maxDrawdownE8},{"mfe_e8",r.mfeE8},{"mae_e8",r.maeE8}};
   for(const auto& [name,value]:financial)if(!metric(ResultKind::Equity,name,value))return false;
   if(!metric(ResultKind::Exposure,"position_qty_e8",r.openPositionQtyE8)||!metric(ResultKind::Scalar,"fills",static_cast<std::int64_t>(r.fills))||!metric(ResultKind::Scalar,"orders",static_cast<std::int64_t>(r.orders))||!metric(ResultKind::Scalar,"events",static_cast<std::int64_t>(r.events)))return false;
   for(const auto& account:r.legs){std::uint64_t sourceId=0;for(const auto& source:e.cut->sources)if(std::string(source.venue.data(),source.venueBytes)==account.exchange&&std::string(source.market.data(),source.marketBytes)==account.market&&(std::string(source.canonicalSymbol.data(),source.canonicalSymbolBytes)==account.symbol||std::string(source.nativeSymbol.data(),source.nativeSymbolBytes)==account.symbol)){sourceId=source.sourceId;break;}if(!sourceId){e.writeError="account result lacks exact native source identity";return false;}if(!metric(ResultKind::Exposure,"source_wallet_component_e8",account.walletBalanceE8,sourceId)||!metric(ResultKind::Exposure,"source_position_qty_e8",account.openPositionQtyE8,sourceId)||!metric(ResultKind::Exposure,"source_fees_paid_e8",account.feesPaidE8,sourceId)||!metric(ResultKind::Exposure,"source_total_pnl_e8",account.totalPnlE8,sourceId))return false;}
   for(const auto& account:v.simulation->accountSummaries()){
    const std::pair<const char*,std::int64_t> fields[]{{"account_initial_balance_e8",account.initialBalanceE8},{"account_wallet_balance_e8",account.walletBalanceE8},{"account_observed_wallet_balance_e8",account.observedWalletBalanceE8},{"account_equity_e8",account.equityE8},{"account_gross_realized_pnl_e8",account.grossRealizedPnlE8},{"account_net_realized_pnl_e8",account.netRealizedPnlE8},{"account_unrealized_pnl_e8",account.unrealizedPnlE8},{"account_fees_paid_e8",account.feesPaidE8}};
    for(const auto& [name,value]:fields){ResultRow row;row.kind=ResultKind::Exposure;row.variant=v.id;row.accountId=account.accountId;row.currencyAssetId=account.quoteAssetId;row.inputCut=monotonic;row.timestampNs=realtime;row.metric=name;row.value=value;for(const auto& source:e.cut->sources)if(source.venueId==account.exchange&&source.marketRaw==account.market){row.exchange.assign(source.venue.data(),source.venueBytes);row.market.assign(source.market.data(),source.marketBytes);break;}if(!self.emit(e,std::move(row)))return false;}
   }
   if(v.hasPreviousEquity){std::int64_t periodReturn{};if(v.simulation->returnSinceEquityBpsE8(v.previousEquity,periodReturn)){
     if(!metric(ResultKind::Series,"period_return_bps_e8",periodReturn))return false;
     const auto bin=static_cast<std::size_t>(std::upper_bound(e.spec.distributionBoundsE8.begin(),e.spec.distributionBoundsE8.end(),periodReturn)-e.spec.distributionBoundsE8.begin());auto& count=v.returnBins[bin];if(count==static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())){e.writeError="period-return histogram count overflow";return false;}++count;ResultRow row;row.kind=ResultKind::Distribution;row.variant=v.id;row.inputCut=monotonic;row.timestampNs=realtime;row.metric="period_return_bps_e8_histogram";row.value=static_cast<std::int64_t>(count);row.auxiliary=static_cast<std::int64_t>(bin);row.currencyAssetId=quote;if(!self.emit(e,std::move(row)))return false;
    }else if(!metric(ResultKind::Scalar,"period_return_unavailable_nonpositive_equity",1))return false;}
   v.previousEquity=equity;v.hasPreviousEquity=true;v.lastMetricMonotonic=monotonic;return true;
  }catch(const std::exception& x){e.writeError=x.what();return false;}
 }
 // Sink contexts have stable addresses for the lifetime of each attempt.
 std::map<Experiment*,std::vector<std::unique_ptr<SinkContext>>> sinks;
 static hftrec::Status approved(void* user,std::uint64_t& generation,std::string& error)noexcept{
  auto& e=*static_cast<Experiment*>(user);generation=0;
  try{if(!e.recorder){e.recorder=std::make_unique<hftrec::service::RecorderServiceClient>();const auto connected=e.recorder->connect(e.spec.recorderRuntimeDirectory,error);if(!hftrec::isOk(connected)){e.recorder.reset();if(connected==hftrec::Status::IoError){error="waiting for Recorder committed cut acknowledgement";return hftrec::Status::Unknown;}return connected;}}
   hftrec::service::RecorderServiceStatus status;const auto result=e.recorder->status(status,error);if(!hftrec::isOk(result)){e.recorder.reset();if(result==hftrec::Status::IoError){error="waiting for Recorder committed cut acknowledgement";return hftrec::Status::Unknown;}return result;}
   if(status.sourceIdentity!=e.spec.sourceIdentity){error="Recorder endpoint capture identity differs from immutable input";return hftrec::Status::CorruptData;}
   generation=status.cutGeneration;if(!generation){error="waiting for Recorder committed cut acknowledgement";return hftrec::Status::Unknown;}return hftrec::Status::Ok;
  }catch(const std::exception& x){error=x.what();return hftrec::Status::IoError;}
 }
 static hftrec::Status compressed(void* user,std::uint64_t generation,std::size_t index,std::vector<std::uint8_t>& bytes,bool& available,std::string& error)noexcept{
  auto& e=*static_cast<Experiment*>(user);available=false;bytes.clear();
  try{if(index>std::numeric_limits<std::uint32_t>::max())return hftrec::Status::OutOfRange;
   if(!e.recorder){e.recorder=std::make_unique<hftrec::service::RecorderServiceClient>();const auto connected=e.recorder->connect(e.spec.recorderRuntimeDirectory,error);if(!hftrec::isOk(connected)){e.recorder.reset();if(connected==hftrec::Status::IoError){error.clear();return hftrec::Status::Ok;}return connected;}}
   std::uint64_t total=0;do{hftrec::service::RecorderBlockChunk chunk;auto status=e.recorder->readBlock(0,static_cast<std::uint32_t>(index),static_cast<std::uint32_t>(bytes.size()),hftrec::service::kRecorderServiceMaximumBlockChunk,chunk,error);
    if(!hftrec::isOk(status)){e.recorder.reset();bytes.clear();if(status==hftrec::Status::IoError){error.clear();return hftrec::Status::Ok;}return status;}
    if(!chunk.available||chunk.archiveFallback){bytes.clear();return hftrec::Status::Ok;}
    if(chunk.cutGeneration<generation||chunk.offset!=bytes.size()||!chunk.totalBytes||chunk.totalBytes>e.spec.budget.maxInputBytes/4||chunk.bytes.empty()||(total&&total!=chunk.totalBytes)||chunk.bytes.size()>chunk.totalBytes-bytes.size()){error="invalid or oversized Recorder RAMfeed chunk";return hftrec::Status::CorruptData;}
    total=chunk.totalBytes;bytes.insert(bytes.end(),chunk.bytes.begin(),chunk.bytes.end());
   }while(bytes.size()<total);available=true;return hftrec::Status::Ok;
  }catch(const std::exception& x){error=x.what();return hftrec::Status::IoError;}
 }
 bool preflightDepthCoverage(Experiment& e,const CorpusCut& cut,std::string& error){
  if(e.spec.kind!=ModuleKind::Strategy)return true;
  CorpusCompressedBlockSource feed;
  if(!e.spec.recorderRuntimeDirectory.empty()){feed.context=&e;feed.approvedGeneration=approved;feed.fetch=compressed;}
  CorpusActiveCursor preflight;
  auto status=preflight.open(e.spec.sourcePath,0,error,{},feed,e.spec.budget.maxInputBytes/2);
  if(!hftrec::isOk(status))return false;
  const auto prefix=preflight.cut();
  if(!prefix || prefix->generation!=cut.generation || prefix->progress.safeMonotonicNs!=cut.progress.safeMonotonicNs ||
     sourceIdentity(*prefix)!=sourceIdentity(cut)){
   error="waiting for stable captured depth coverage preflight";return false;
  }
  std::vector<std::uint32_t> maxima(cut.sources.size());
  BinaryMarketRecord record{};CorpusCursorResult result=CorpusCursorResult::Record;
  do{
   status=preflight.next(record,result,error);if(!hftrec::isOk(status))return false;
   if(result==CorpusCursorResult::Record && record.header.receiveRealtimeNs>=e.spec.beginReceiveNs &&
      !hft_backtest::detail::binary_corpus::includeDepthCoverageMaximum(record,maxima)){
    error="invalid exact captured depth coverage preflight";return false;
   }
  }while(result==CorpusCursorResult::Record);
  e.maximumDepthCoverageSourcesBySource=std::move(maxima);return true;
 }
 bool initialize(Experiment& e,std::string& error){
  if(e.cursor.cut()){if(!hftrec::isOk(e.cursor.refresh(error)))return false;}
  else {CorpusCompressedBlockSource feed;if(!e.spec.recorderRuntimeDirectory.empty()){feed.context=&e;feed.approvedGeneration=approved;feed.fetch=compressed;}if(!hftrec::isOk(e.cursor.open(e.spec.sourcePath,0,error,{},feed,e.spec.budget.maxInputBytes/2)))return false;}
  e.cut=e.cursor.cut();
  if(!e.cut||sourceIdentity(*e.cut)!=e.spec.sourceIdentity){error="input capture identity changed";return false;}
  if(e.waitingCutGeneration==e.cut->generation&&!e.waitingReason.empty()){error=e.waitingReason;return false;}
  if(!validateExperimentSpec(e.spec,config,error))return false;
  auto wait=[&](std::string reason){e.waitingCutGeneration=e.cut->generation;e.waitingReason="waiting for "+std::move(reason);if(e.waitingReason.size()>4096)e.waitingReason.resize(4096);error=e.waitingReason;return false;};
  if(!e.cut->finalized&&e.spec.recorderRuntimeDirectory.empty()){error="active input requires explicit Recorder RAMfeed endpoint";return false;}
  if(e.cut->progress.safeMonotonicNs<=e.cut->manifest.replayAnchorMonotonicNs||!e.cut->manifest.recordCount){if(e.cut->finalized){error="finalized input has no authoritative executable prefix";return false;}return wait("first authoritative executable prefix");}
  if(e.status.visibleGeneration){std::vector<std::uint8_t> bytes;if(!detail::readFile(visibleCutPath(e),bytes,error))return false;const auto prior=decodeCut(std::move(bytes));if(prior.generation!=e.status.visibleGeneration||prior.inputCut!=e.status.visibleInputCut||prefixFingerprint(*e.cut,prior.inputCut,prior.sourceCount)!=prior.inputFingerprint){error="restored immutable input prefix mismatch";return false;}}
  if(e.cut->sources.size()>e.spec.budget.maxSources){error="whole-market source capacity exceeded";return false;}
  if(gapIntersects(e,*e.cut)){error="capture gap intersects selected run";return false;}
  if(e.spec.budget.inputBatchEvents>e.spec.budget.maxInputBytes/4/sizeof(BinaryMarketRecord)){error="input batch exceeds byte quota";return false;}
  if(!preflightDepthCoverage(e,*e.cut,error))return false;
  std::vector<std::unique_ptr<Variant>> prepared;
  std::vector<std::unique_ptr<SinkContext>> preparedSinks;
  for(std::size_t i=0;i<e.spec.variants.size();++i){
   auto variant=std::make_unique<Variant>();variant->id=detail::variantIdentityFromFingerprint(e.status.specFingerprint,i);variant->returnBins.resize(e.spec.distributionBoundsE8.size()+1);
   if(e.spec.kind==ModuleKind::Strategy){
    const auto path=e.directory/("config-"+std::to_string(i)+".json");std::vector<std::uint8_t> bytes;const auto& text=e.spec.variants[i].configText;
    if(!detail::readFile(path,bytes,error)||bytes.size()!=text.size()||!std::equal(bytes.begin(),bytes.end(),text.begin(),[](std::uint8_t byte,char character){return byte==static_cast<std::uint8_t>(character);})){error="immutable expanded configuration artifact mismatch";return false;}
    BacktestRunRequest request;request.corpusPath=e.spec.sourcePath;request.corpusBeginReceiveNs=e.spec.beginReceiveNs;request.configPath=path;request.runId=e.spec.id+"-"+std::to_string(variant->id);request.strategy=e.spec.module;request.indicatorProfile=e.spec.profile;request.latencySeed=e.spec.seed;request.initialBalanceE8=e.spec.initialBalanceE8;request.virtualHost={e.spec.hasStrategyBudget,e.spec.strategyBudgetE8,e.spec.budgetCurrency,e.spec.marginBorrowAllowed};request.writeArtifacts=false;
    request.hasMakerFeeBps=request.hasTakerFeeBps=true;
    for(const auto& fee:e.spec.fees)request.feeSchedules.push_back({fee.exchange,fee.market,fee.makerFeeBpsE8,fee.takerFeeBpsE8});
    auto context=std::make_unique<SinkContext>();context->service=this;context->experiment=&e;context->variant=variant.get();variant->simulation=std::make_unique<SimulationSession>();
    const auto status=variant->simulation->prepare(request,*e.cut,{sink,context.get(),metrics},{e.spec.budget.maxSources,e.spec.budget.maxContexts,e.spec.budget.maxStateBytes,e.maximumDepthCoverageSourcesBySource});
    if(status==Status::NotReady)return wait("native simulation inputs: "+variant->simulation->result().error);
    if(!isOk(status)){error=variant->simulation->result().error;return false;}
    for(const auto& leg:variant->simulation->result().legs){bool found=false;for(const auto& fee:e.spec.fees)if(fee.exchange==leg.exchange&&fee.market==leg.market){found=true;break;}if(!found){error="missing explicit fee scenario for admitted virtual account";return false;}}
    if(variant->simulation->valuationQuoteAssetId()!=e.spec.valuationQuoteAssetId){error="admitted portfolio valuation quote differs from explicit scenario";return false;}
    preparedSinks.push_back(std::move(context));
   }else{variant->study=std::make_unique<DataStudySession>();if(!variant->study->prepare(e.spec,e.spec.variants[i],variant->id,error))return false;}
   prepared.push_back(std::move(variant));
  }
  // Preparation is transactional. Missing active input creates no attempts or
  // output and retries only when another acknowledged Recorder cut appears.
  std::uint64_t next=e.status.generation+1;while(next<=1024&&std::filesystem::exists(generationPath(e,next)))++next;
  if(next>1024){error="attempt generation quota exceeded";return false;}
  e.attempt=generationPath(e,next);if(!std::filesystem::create_directory(e.attempt)){error="cannot create isolated attempt generation";return false;}
  e.status.generation=next;e.output={};e.output.generation=next;e.targetCut=e.cut->progress.safeMonotonicNs;e.restartVisibleCut=e.status.visibleInputCut;e.atBoundary=true;e.status.inputCut=0;
  e.status.variants.clear();for(const auto& variant:prepared)e.status.variants.push_back({variant->id});
  e.variants=std::move(prepared);sinks[&e]=std::move(preparedSinks);e.pending.clear();e.pendingBytes=0;e.waitingCutGeneration=0;e.waitingReason.clear();
  e.initialized=true;e.status.diagnostic.clear();return save(e,error);
 }
 static bool studyOutput(ResultRow row,void* user){auto& context=*static_cast<SinkContext*>(user);return (qualify(*context.experiment,row),static_cast<Impl*>(context.service)->emit(*context.experiment,std::move(row)));}
 bool study(Experiment& e,Variant& v,const BinaryMarketRecord& record){SinkContext context{this,&e,&v};return v.study->observe(record,studyOutput,&context,e.writeError);}
 bool commit(Experiment& e,std::uint64_t safe,bool final=false){
  if(!flush(e))return false;e.output.inputCut=safe;e.output.sourceCount=e.cut->sources.size();e.output.inputFingerprint=prefixFingerprint(*e.cut,safe,e.output.sourceCount);if(!write(e.attempt/("cut-"+std::to_string(safe)+(final?"-final.bin":".bin")),encodeCut(e.output),false,e.writeError)||!write(e.attempt/"output-cut.bin",encodeCut(e.output),true,e.writeError))return false;
  const auto priorInput=e.status.inputCut;const auto priorGeneration=e.status.visibleGeneration;const auto priorVisible=e.status.visibleInputCut;const auto priorFinal=e.status.visibleFinal;
  e.status.inputCut=safe;if(safe>=e.restartVisibleCut){e.status.visibleGeneration=e.status.generation;e.status.visibleInputCut=safe;e.status.visibleFinal=final;}
  if(!save(e,e.writeError)){e.status.inputCut=priorInput;e.status.visibleGeneration=priorGeneration;e.status.visibleInputCut=priorVisible;e.status.visibleFinal=priorFinal;return false;}return true;
 }
 void advance(Experiment& e){
  if(e.status.state!=ExperimentState::Running)return;std::string error;
  if(!e.initialized){if(!initialize(e,error)){if(error.starts_with("waiting for ")){e.status.diagnostic=error;return;}fence(e,error.find("gap")!=std::string::npos?ExperimentState::Incomplete:ExperimentState::Failed,error);return;}}
  if(e.atBoundary){
   if(!hftrec::isOk(e.cursor.refresh(error))){if(error.starts_with("waiting for ")){e.status.diagnostic=error;return;}fence(e,ExperimentState::Failed,error);return;}
   auto cut=e.cursor.cut();if(!cut||sourceIdentity(*cut)!=e.spec.sourceIdentity){fence(e,ExperimentState::Incomplete,"capture identity changed");return;}
   if(gapIntersects(e,*cut)){fence(e,ExperimentState::Incomplete,"capture gap intersects selected input");return;}
   if(cut->sources.size()>e.spec.budget.maxSources){fence(e,ExperimentState::QuotaExceeded,"whole-market source capacity exceeded");return;}
   if(!e.cut||cut->generation!=e.cut->generation){
    if(!preflightDepthCoverage(e,*cut,error)){if(error.starts_with("waiting for ")){e.status.diagnostic=error;return;}fence(e,ExperimentState::Incomplete,error);return;}
    for(auto& variant:e.variants)if(variant->simulation&&!isOk(variant->simulation->updateCut(*cut,e.maximumDepthCoverageSourcesBySource))){fence(e,ExperimentState::Incomplete,variant->simulation->result().error);return;}
   }
   e.cut=std::move(cut);e.targetCut=e.cut->progress.safeMonotonicNs;
   if(e.targetCut==e.status.inputCut){if(e.cut->finalized)endInput(e);return;}
   e.atBoundary=false;
  }
  std::vector<BinaryMarketRecord> records;records.reserve(e.spec.budget.inputBatchEvents);CorpusCursorResult cursorResult=CorpusCursorResult::Record;
  for(std::uint32_t n=0;n<e.spec.budget.inputBatchEvents;++n){BinaryMarketRecord record{};if(!hftrec::isOk(e.cursor.next(record,cursorResult,error))){fence(e,ExperimentState::Incomplete,error);return;}if(cursorResult!=CorpusCursorResult::Record)break;records.push_back(record);}
  const bool boundary=cursorResult!=CorpusCursorResult::Record;
  // Only an exhausted immutable cut closes same-monotonic ties and permits timer settlement.
  const auto safe=boundary?e.targetCut:e.status.inputCut;
  for(std::size_t i=0;i<e.variants.size();++i){auto& v=*e.variants[i];auto& status=e.status.variants[i];if(v.terminal)continue;if(v.simulation){SimulationAdvance progress;if(!isOk(v.simulation->advance(records,safe,progress))){fence(e,e.writeError.empty()?ExperimentState::Failed:ExperimentState::QuotaExceeded,e.writeError.empty()?v.simulation->result().error:e.writeError);return;}const auto& r=v.simulation->result();status.events=r.events;
    if(r.riskStopped){if(!progress.terminalMonotonicNs){fence(e,ExperimentState::Failed,"native risk stop lacks exact input clock evidence");return;}v.terminal=true;status.terminal=status.riskStopped=true;status.terminalMonotonicNs=progress.terminalMonotonicNs;status.inputCut=progress.terminalMonotonicNs;e.status.diagnostic="one or more variants stopped at the native risk boundary";const std::pair<const char*,std::int64_t> facts[]{{"risk_stopped",1},{"risk_stop_equity_e8",r.riskStopEquityE8},{"risk_stop_threshold_equity_e8",r.riskStopThresholdEquityE8}};for(const auto& [name,value]:facts){ResultRow row;row.kind=ResultKind::Scalar;row.variant=v.id;row.inputCut=progress.terminalMonotonicNs;row.timestampNs=r.riskStopTsNs;row.currencyAssetId=e.spec.valuationQuoteAssetId;row.metric=name;row.value=value;if(!emit(e,std::move(row))){fence(e,ExperimentState::QuotaExceeded,e.writeError);return;}}}
    else if(progress.recordsAccepted!=records.size()||progress.paused){fence(e,ExperimentState::Failed,"nonterminal simulation did not accept the exact input batch");return;}
    else status.inputCut=safe;

   }else{for(const auto& record:records){if(!study(e,v,record)){fence(e,ExperimentState::QuotaExceeded,e.writeError);return;}if(record.header.receiveRealtimeNs>=e.spec.beginReceiveNs)++status.events;}}
   if(!records.empty()&&!v.terminal)status.lastReceiveNs=records.back().header.receiveRealtimeNs;
  }
  if(!flush(e)){fence(e,ExperimentState::QuotaExceeded,e.writeError);return;}
  if(std::all_of(e.variants.begin(),e.variants.end(),[](const auto& variant){return variant->terminal;})){
   if(!commit(e,e.targetCut)){fence(e,ExperimentState::Failed,e.writeError);return;}e.status.state=ExperimentState::Stopped;e.status.backgroundEnabled=false;e.status.diagnostic="all variants terminated by native risk policy; inventory retained";if(!save(e,error)){fence(e,ExperimentState::Failed,error);return;}releaseCompute(e);return;
  }
  if(boundary){
   const bool ended=cursorResult==CorpusCursorResult::End&&e.cut->finalized;
   // A separate terminal checkpoint preserves the prior immutable cut even
   // when Recorder finalizes without advancing its authoritative input fence.
   if(ended){endInput(e);return;}
   if(!commit(e,safe)){fence(e,ExperimentState::Failed,e.writeError);return;}
   e.atBoundary=true;
  }
 }
 Response describe(const Experiment& e,const Request& request){
  Response response;response.buildIdentity=config.buildIdentity;response.moduleIdentity=config.moduleIdentity;response.residentByteLimit=config.maxResidentStateBytes;response.residentVariantLimit=config.maxResidentVariants;
  if(!request.limit||request.limit>config.maxResponseRows||request.offset>e.spec.variants.size()){response.error="invalid immutable variant page bounds";return response;}
  response.hasSpecification=true;response.specification=e.spec;response.specification.variants.clear();response.specificationVariantCount=e.spec.variants.size();response.specificationVariantOffset=request.offset;
  std::size_t bytes=detail::encode(response).size();
  for(std::size_t i=static_cast<std::size_t>(request.offset);i<e.spec.variants.size();++i){
   if(response.specification.variants.size()>=request.limit)break;
   const auto& variant=e.spec.variants[i];VariantDescription description;description.id=detail::variantIdentityFromFingerprint(e.status.specFingerprint,i);description.ordinal=i;description.label=e.spec.module+" #"+std::to_string(description.id);
   for(const auto& parameter:variant.parameters){const auto item=" "+parameter.name+"="+parameter.value;if(description.label.size()+item.size()>1000){description.label+=" ...";break;}description.label+=item;}
   std::size_t amount=16+variant.configText.size()+24+description.label.size();for(const auto& parameter:variant.parameters)amount+=16+parameter.name.size()+parameter.value.size();
   if(amount>detail::kMaxWireBytes-bytes)break;
   bytes+=amount;response.specification.variants.push_back(variant);response.variantDescriptions.push_back(std::move(description));
  }
  response.nextOffset=request.offset+response.specification.variants.size();response.more=response.nextOffset<e.spec.variants.size();
  if(response.more&&response.specification.variants.empty()){response.hasSpecification=false;response.error="immutable variant exceeds bounded response schema";return response;}
  response.ok=true;return response;
 }
 Response range(Experiment& e,const Request& request){Response response;if(request.limit==0||request.limit>config.maxResponseRows){response.error="result range limit exceeds bound";return response;}if(!e.status.visibleGeneration){response.ok=true;return response;}
  std::vector<std::uint8_t> bytes;std::string error;const auto attempt=generationPath(e,e.status.visibleGeneration);if(!detail::readFile(visibleCutPath(e),bytes,error)){response.error=error;return response;}auto cut=decodeCut(std::move(bytes));if(cut.generation!=e.status.visibleGeneration||cut.inputCut!=e.status.visibleInputCut||cut.rows>e.spec.budget.maxResultRows||cut.bytes>e.spec.budget.maxOutputBytes)throw std::runtime_error("visible output cut does not match durable experiment state");std::uint64_t matched=0;
  std::size_t responseBytes=0;for(std::uint64_t segment=0;segment<cut.segments;++segment){if(!detail::readFile(segmentPath(attempt,segment),bytes,error)){response.error=error;return response;}detail::Codec c(true);c.bytes=std::move(bytes);std::uint64_t hash{};c.integer(hash);if(detail::fingerprint(std::span<const std::uint8_t>(c.bytes).subspan(c.offset))!=hash)throw std::runtime_error("result segment checksum mismatch");std::vector<ResultRow> rows;c.list(rows,4096,[&](auto& row){detail::code(c,row);});c.done();for(auto& row:rows){if(row.inputCut>=e.status.visibleInputCut||row.kind!=request.resultKind||(request.variant&&row.variant!=request.variant)||row.timestampNs<request.beginNs||(request.endNs&&row.timestampNs>=request.endNs)||(request.maximumInputCut&&row.inputCut>=request.maximumInputCut))continue;if(matched++<request.offset)continue;const auto estimate=row.payload.size()+row.metric.size()+row.exchange.size()+row.market.size()+row.instrument.size()+256;if(response.rows.size()>=request.limit||responseBytes+estimate>detail::kMaxWireBytes/2){response.more=true;response.nextOffset=request.offset+response.rows.size();response.ok=true;return response;}responseBytes+=estimate;response.rows.push_back(std::move(row));}}
  response.nextOffset=request.offset+response.rows.size();response.ok=true;return response;
 }
};
ResearchService::ResearchService(ServiceConfig config):impl_(std::make_unique<Impl>(std::move(config))){}
ResearchService::~ResearchService()=default;
bool ResearchService::restore(std::string& error){auto& s=*impl_;try{
 if(s.lease>=0){error="research service already restored";return false;}
 if(s.config.root.empty()||s.config.buildIdentity.empty()||s.config.moduleIdentity.empty()||!s.config.maxWorkItemsPerStep||s.config.maxWorkItemsPerStep>64||!s.config.maxExperiments||s.config.maxExperiments>4096||s.config.maxResidentVariants>4096||s.config.maxRequestBytes>detail::kMaxWireBytes||s.config.maxResponseRows>4096||s.config.maxResidentStateBytes<(4U<<20)||std::uint64_t(s.config.maxExperiments)*(16U<<10)+std::uint64_t(s.config.maxResidentVariants)*(64U<<10)>s.config.maxServiceOutputBytes){error="invalid research service configuration";return false;}
 if(!s.admit(nullptr,nullptr,error))return false;
 std::error_code directoryError;const bool created=std::filesystem::create_directories(s.config.root,directoryError);if(directoryError){error="cannot create research root";return false;}if(created&&::chmod(s.config.root.c_str(),0700)!=0){error="research root permissions failed";return false;}struct stat st{};if(::lstat(s.config.root.c_str(),&st)!=0||!S_ISDIR(st.st_mode)||st.st_uid!=::geteuid()||(st.st_mode&0077)!=0){error="research root must be an owner-private real directory";return false;}
 s.lease=::open((s.config.root/"service-owner.lease").c_str(),O_RDWR|O_CREAT|O_CLOEXEC|O_NOFOLLOW,0600);if(s.lease<0||::fstat(s.lease,&st)!=0||st.st_uid!=::geteuid()||!S_ISREG(st.st_mode)||st.st_nlink!=1||(st.st_mode&0077)!=0||::flock(s.lease,LOCK_EX|LOCK_NB)!=0){error="research durable owner lease unavailable";return false;}
 s.diskBytes=0;for(const auto& entry:std::filesystem::recursive_directory_iterator(s.config.root)){if(entry.is_symlink()){error="symlink in research store";return false;}if(entry.is_regular_file()){const auto size=entry.file_size();if(size>s.config.maxServiceOutputBytes||s.diskBytes>s.config.maxServiceOutputBytes-size){error="research store exceeds finite service quota";return false;}s.diskBytes+=size;}}
 for(const auto& entry:std::filesystem::directory_iterator(s.config.root)){if(!entry.is_directory()||entry.path().filename().string().starts_with(".creating-"))continue;if(s.experiments.size()>=s.config.maxExperiments){error="experiment catalog quota exceeded";return false;}auto e=std::make_unique<Experiment>();e->directory=entry.path();if(!loadExperimentSpec(e->directory/"spec.bin",e->spec,error))return false;if(entry.path().filename()!=e->spec.id){error="experiment path/spec identity mismatch";return false;}
  std::vector<std::uint8_t> bytes;if(!detail::readFile(e->directory/"state.bin",bytes,error))return false;e->status=detail::decode<ExperimentStatus>(std::move(bytes));if(e->status.id!=e->spec.id||e->status.sourceIdentity!=e->spec.sourceIdentity||e->status.specFingerprint!=detail::fingerprint(detail::encode(e->spec))){error="experiment state/spec mismatch";return false;}
  e->status.outputBytes=0;for(const auto& artifact:std::filesystem::recursive_directory_iterator(e->directory))if(artifact.is_regular_file()&&artifact.path().filename().string().starts_with("segment-")){const auto bytes=artifact.file_size();if(bytes>e->spec.budget.maxOutputBytes||e->status.outputBytes>e->spec.budget.maxOutputBytes-bytes){error="experiment durable output quota exceeded";return false;}e->status.outputBytes+=bytes;}
  if(!detail::validateStoredExperimentSpec(e->spec,error))return false;
  e->compatible=e->spec.buildIdentity==s.config.buildIdentity&&e->spec.moduleIdentity==s.config.moduleIdentity;
  if(!e->compatible){e->status.state=ExperimentState::Failed;e->status.backgroundEnabled=false;e->status.diagnostic="incompatible build/module: saved specification and analytics remain read-only";}
  else if(e->status.backgroundEnabled){
   if(!validateExperimentSpec(e->spec,s.config,error)){e->status.state=ExperimentState::Failed;e->status.backgroundEnabled=false;e->status.diagnostic="restore execution refused: "+error;}
   else {e->status.state=ExperimentState::Running;e->residentReservation=true;}
  }
  if(!s.admit(&e->spec,e->residentReservation?e.get():nullptr,error))return false;
  s.experiments.emplace(e->spec.id,std::move(e));
 }
 s.ready=true;return true;
 }catch(const std::exception&e){error=e.what();return false;}}
Response ResearchService::dispatch(const Request& request){auto& s=*impl_;Response response;response.buildIdentity=s.config.buildIdentity;response.moduleIdentity=s.config.moduleIdentity;response.residentByteLimit=s.config.maxResidentStateBytes;response.residentVariantLimit=s.config.maxResidentVariants;try{
 if(!s.ready){response.error="research durable owner is not restored";return response;}
 if(request.command==Command::Catalog){response.catalog=researchModuleCatalog();response.ok=true;return response;}
 if(request.command==Command::Create){std::string error;if(!validateExperimentSpec(request.spec,s.config,error)){response.error=error;return response;}if(s.experiments.size()>=s.config.maxExperiments){response.error="experiment catalog quota exceeded";return response;}if(s.experiments.contains(request.spec.id)){response.error="immutable experiment already exists";return response;}
  if(!s.admit(&request.spec,nullptr,error)){response.error=error;return response;}
  auto e=std::make_unique<Experiment>();e->spec=request.spec;const auto finalDirectory=s.config.root/e->spec.id;e->directory=s.config.root/(".creating-"+e->spec.id+"-"+std::to_string(::getpid()));if(std::filesystem::exists(finalDirectory)){response.error="experiment directory already exists";return response;}e->status.id=e->spec.id;e->status.specFingerprint=detail::fingerprint(detail::encode(e->spec));e->status.sourceIdentity=e->spec.sourceIdentity;e->status.sourcePath=e->spec.sourcePath;e->status.beginReceiveNs=e->spec.beginReceiveNs;if(!std::filesystem::create_directory(e->directory)){response.error="experiment directory already exists";return response;}
  if(!s.write(e->directory/"spec.bin",detail::encode(e->spec),false,error)){response.error=error;return response;}if(e->spec.kind==ModuleKind::Strategy)for(std::size_t i=0;i<e->spec.variants.size();++i){const auto& text=e->spec.variants[i].configText;if(!s.write(e->directory/("config-"+std::to_string(i)+".json"),{reinterpret_cast<const std::uint8_t*>(text.data()),text.size()},false,error)){response.error=error;return response;}}if(!s.save(*e,error)){response.error=error;return response;}std::filesystem::rename(e->directory,finalDirectory);e->directory=finalDirectory;{int root=::open(s.config.root.c_str(),O_DIRECTORY|O_RDONLY|O_CLOEXEC);if(root<0||::fsync(root)!=0){if(root>=0)::close(root);response.error="experiment catalog durability failed";return response;}::close(root);}response.experiments.push_back(e->status);s.experiments.emplace(e->spec.id,std::move(e));response.ok=true;return response;
 }
 if(request.command==Command::Status&&request.experimentId.empty()){
  if(!request.limit||request.limit>s.config.maxResponseRows){response.error="status range limit exceeds bound";return response;}std::uint64_t offset=0;std::size_t bytes=0;for(auto& [id,e]:s.experiments){if(offset++<request.offset)continue;const auto estimate=e->status.id.size()+e->status.sourceIdentity.size()+e->status.sourcePath.string().size()+e->status.diagnostic.size()+e->status.variants.size()*64+256;if(response.experiments.size()>=request.limit||bytes+estimate>detail::kMaxWireBytes/2){response.more=true;break;}bytes+=estimate;response.experiments.push_back(e->status);}response.nextOffset=request.offset+response.experiments.size();response.ok=true;return response;
 }
 auto found=s.experiments.find(request.experimentId);if(found==s.experiments.end()){response.error="unknown experiment";return response;}auto& e=*found->second;
 if(request.command==Command::Describe)return s.describe(e,request);
 if(request.command==Command::Results||request.command==Command::Range)return s.range(e,request);
 if(request.command==Command::Status){response.experiments.push_back(e.status);response.ok=true;return response;}
 std::string error;
 if(request.command==Command::Start||request.command==Command::Resume){if(!e.compatible){response.error="saved experiment requires its exact build/module for execution; analytics remain read-only";return response;}if(request.command==Command::Resume&&e.status.state==ExperimentState::Stopped){response.error="stopped attempt requires explicit Start replay";return response;}if(e.status.state==ExperimentState::Incomplete||e.status.state==ExperimentState::Failed||e.status.state==ExperimentState::QuotaExceeded){response.error="fenced immutable experiment requires a new explicit experiment";return response;}if(!validateExperimentSpec(e.spec,s.config,error)){response.error=error;return response;}if(!e.residentReservation){if(!s.admit(nullptr,&e,error)){response.error=error;return response;}e.residentReservation=true;}if(!e.initialized&&!s.initialize(e,error)){if(error.starts_with("waiting for ")){e.status.state=ExperimentState::Running;e.status.backgroundEnabled=true;e.status.diagnostic=error;if(!s.save(e,error)){s.fence(e,ExperimentState::Failed,"start state persistence failed: "+error);response.error=error;return response;}response.experiments.push_back(e.status);response.ok=true;return response;}s.fence(e,error.find("gap")!=std::string::npos?ExperimentState::Incomplete:ExperimentState::Failed,error);response.error=error;return response;}for(auto& v:e.variants)if(v->simulation&&!v->terminal)v->simulation->resume();e.status.state=ExperimentState::Running;e.status.backgroundEnabled=true;}
 else if(request.command==Command::Pause){if(e.status.state==ExperimentState::Failed||e.status.state==ExperimentState::Incomplete||e.status.state==ExperimentState::QuotaExceeded||e.status.state==ExperimentState::Stopped){response.experiments.push_back(e.status);response.ok=true;return response;}e.status.state=ExperimentState::Paused;e.status.backgroundEnabled=false;for(auto& v:e.variants)if(v->simulation)v->simulation->pause();}
 else if(request.command==Command::Stop){if(e.status.state==ExperimentState::Failed||e.status.state==ExperimentState::Incomplete||e.status.state==ExperimentState::QuotaExceeded){if(e.residentReservation){s.fence(e,e.status.state,e.status.diagnostic);if(e.residentReservation){response.error="terminal state persistence is unavailable; compute remains fenced";return response;}}response.experiments.push_back(e.status);response.ok=true;return response;}e.status.state=ExperimentState::Stopped;e.status.backgroundEnabled=false;for(auto& v:e.variants)if(v->simulation)v->simulation->pause();e.status.diagnostic="stopped without synthetic liquidation";}
 else {response.error="unknown research command";return response;}
 if(!s.save(e,error)){if(request.command==Command::Start||request.command==Command::Resume)s.fence(e,ExperimentState::Failed,"start state persistence failed: "+error);response.error=error;return response;}if(request.command==Command::Stop)s.releaseCompute(e);response.experiments.push_back(e.status);response.ok=true;return response;
 }catch(const std::exception&e){response.error=e.what();return response;}}
void ResearchService::step(){auto& s=*impl_;if(!s.ready||s.experiments.empty())return;const auto count=std::min<std::size_t>(s.config.maxWorkItemsPerStep,s.experiments.size());for(std::size_t n=0;n<count;++n){auto iterator=s.experiments.begin();std::advance(iterator,static_cast<std::ptrdiff_t>(s.schedule++%s.experiments.size()));try{s.advance(*iterator->second);}catch(const std::exception&e){s.fence(*iterator->second,ExperimentState::Failed,e.what());}}}
} // namespace hft_backtest::research
