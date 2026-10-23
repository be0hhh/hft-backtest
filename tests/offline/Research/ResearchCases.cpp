#include "OfflineCase.hpp"
#include "hft_backtest/Research.hpp"
#include "hft_backtest/DataStudy.hpp"
#include "ResearchStorage.hpp"
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <thread>
#include "hftrec/CorpusContract/BinaryMarketCorpusWriter.hpp"
#include <unistd.h>

namespace {
using namespace hft_backtest::research;
struct TemporaryStore {
 std::filesystem::path path=std::filesystem::temp_directory_path()/("cxet-research-case-"+std::to_string(::getpid()));
 TemporaryStore(){std::filesystem::create_directories(path);std::filesystem::permissions(path,std::filesystem::perms::owner_all);}
 ~TemporaryStore(){std::error_code ignored;std::filesystem::remove_all(path,ignored);}
};
ExperimentSpec studySpec(){ExperimentSpec s;s.id="study";s.kind=ModuleKind::DataStudy;s.module="midprice_return_distribution";s.sourcePath="/explicit/corpus";s.sourceIdentity="123:456";s.buildIdentity="build";s.moduleIdentity="module";std::string error;CXET_CHECK(expandExperimentGrid(s,error));return s;}
void exactGridHasFullStableCartesianProduct(){
 ExperimentSpec s;s.rawConfigText="{\"a\":${a},\"b\":${b}}";s.rawGridText="a=1..3..1\nb=0.1,0.2";s.seed=77;std::string error;
 CXET_CHECK(expandExperimentGrid(s,error));CXET_CHECK(s.variants.size()==6);CXET_CHECK(s.variants[0].configText=="{\"a\":1,\"b\":0.1}");CXET_CHECK(s.variants[5].configText=="{\"a\":3,\"b\":0.2}");
 auto id=variantIdentity(s,0);auto copy=s;CXET_CHECK(id==variantIdentity(copy,0));CXET_CHECK(id!=variantIdentity(s,1));CXET_CHECK(s.seed==77);
 s.budget.maxVariants=5;CXET_CHECK(!expandExperimentGrid(s,error));CXET_CHECK(s.variants.size()==6);
}
void gridRejectsMissingPlaceholdersAndWrongRangeDirection(){ExperimentSpec s;s.rawConfigText="{}";s.rawGridText="a=1,2";std::string error;CXET_CHECK(!expandExperimentGrid(s,error));s.rawConfigText="${a}";s.rawGridText="a=1..3..-1";CXET_CHECK(!expandExperimentGrid(s,error));s.budget.maxVariants=4294967295U;s.rawGridText="a=1..100000000..1";CXET_CHECK(!expandExperimentGrid(s,error));auto negative=studySpec();negative.fees.push_back({"x","spot",-1,0});ServiceConfig config;config.buildIdentity="build";config.moduleIdentity="module";CXET_CHECK(!validateExperimentSpec(negative,config,error));CXET_CHECK(error.find("Unsupported")!=std::string::npos);}
void boundedCodecRefusesUnknownProtocolAndTruncation(){
 Request r;r.protocolVersion=99;bool refused=false;try{auto bytes=detail::encode(r);(void)detail::decode<Request>(bytes);}catch(const std::exception&){refused=true;}CXET_CHECK(refused);
 r.protocolVersion=kResearchProtocolVersion;auto bytes=detail::encode(r);bytes.pop_back();refused=false;try{(void)detail::decode<Request>(bytes);}catch(const std::exception&){refused=true;}CXET_CHECK(refused);
}
bool collect(ResultRow row,void* out){static_cast<std::vector<ResultRow>*>(out)->push_back(std::move(row));return true;}
void nativeStudyUsesReceiveIntervalAndRealDistribution(){
 auto spec=studySpec();spec.studyIntervalNs=100;spec.distributionBoundsE8={0,10000000};DataStudySession study;std::string error;CXET_CHECK(study.prepare(spec,spec.variants[0],17,error));std::vector<ResultRow> rows;
 hftrec::corpus::BinaryMarketRecord record;record.header.sourceId=1;record.header.sourceFlags=hftrec::corpus::BinaryMarketSourceBookTickerBidPresent|hftrec::corpus::BinaryMarketSourceBookTickerAskPresent;record.header.channel=hftrec::corpus::BinaryMarketChannel::BookTicker;record.header.payloadBytes=sizeof(hftrec::corpus::BinaryMarketBookTickerPayload);auto* book=hftrec::corpus::binaryMarketPayload<hftrec::corpus::BinaryMarketBookTickerPayload>(&record);
 book->bidPriceRaw=book->askPriceRaw=100;record.header.receiveRealtimeNs=100;record.header.receiveMonotonicNs=100;CXET_CHECK(study.observe(record,collect,&rows,error));CXET_CHECK(rows.empty());
 book->bidPriceRaw=book->askPriceRaw=110;record.header.receiveRealtimeNs=150;record.header.receiveMonotonicNs=150;CXET_CHECK(study.observe(record,collect,&rows,error));CXET_CHECK(rows.empty());
 book->bidPriceRaw=book->askPriceRaw=120;record.header.receiveRealtimeNs=200;record.header.receiveMonotonicNs=200;CXET_CHECK(study.observe(record,collect,&rows,error));CXET_CHECK(rows.size()==2);CXET_CHECK(rows[0].value==20000000);CXET_CHECK(rows[0].variant==17);CXET_CHECK(rows[1].kind==ResultKind::Distribution);CXET_CHECK(rows[1].value==1&&rows[1].auxiliary==2);
}
void immutableCatalogRestoresStoppedWithoutRuntime(){
 TemporaryStore temporary;ServiceConfig config;config.root=temporary.path;config.buildIdentity="build";config.moduleIdentity="module";
 {ResearchService service(config);std::string error;CXET_CHECK(service.restore(error));Request create;create.command=Command::Create;create.spec=studySpec();CXET_CHECK(service.dispatch(create).ok);CXET_CHECK(!service.dispatch(create).ok);Request stop;stop.command=Command::Stop;stop.experimentId="study";CXET_CHECK(service.dispatch(stop).ok);}
 {ResearchService service(config);std::string error;CXET_CHECK(service.restore(error));Request status;status.command=Command::Status;auto reply=service.dispatch(status);CXET_CHECK(reply.ok&&reply.experiments.size()==1);CXET_CHECK(reply.experiments[0].state==ExperimentState::Stopped);CXET_CHECK(!reply.experiments[0].backgroundEnabled);}
}
struct ArchiveFixture {
 TemporaryStore temporary;
 std::filesystem::path root=temporary.path/"corpus";
 hftrec::corpus::BinaryMarketCorpusWriter writer;
 explicit ArchiveFixture(hftrec::corpus::BinaryMarketStopReason reason=hftrec::corpus::BinaryMarketStopReason::Requested){
  using namespace hftrec::corpus;BinaryMarketSource source;source.sourceId=source.canonicalSymbolId=1;source.initialSourceGeneration=1;source.venueId=1;source.marketRaw=2;source.marketKindRaw=1;source.economicBaseAssetId=1;source.quoteAssetId=2;source.priceScale=source.quantityScale=8;source.venue[0]='x';source.venueBytes=1;const std::string market="spot";std::copy(market.begin(),market.end(),source.market.begin());source.marketBytes=market.size();source.canonicalSymbol[0]=source.nativeSymbol[0]='A';source.canonicalSymbolBytes=source.nativeSymbolBytes=1;source.directoryFlags=BinaryMarketDirectoryBookTicker;source.configuredChannelMask=source.availableChannelMask=source.traderReplayChannelMask=1;source.compatibility[0]=BinaryMarketCompatibility::ExactTraderReplay;
  BinaryMarketWriterConfig config;config.root=root;config.sources={&source,1};config.producerEpoch=13;config.maximumBytes=8U<<20;config.segmentTargetBytes=1U<<16;config.targetDurationNs=10000;config.startedReceiveNs=config.startedMonotonicNs=100;config.ringCapacity=1;config.shardCount=1;CXET_CHECK(hftrec::isOk(writer.start(config)));
  for(std::uint64_t i=1;i<=3;++i){BinaryMarketRecord record;auto& h=record.header;h.sourceId=1;h.sourceGeneration=h.sessionEpoch=1;h.eventSequence=h.frameSequence=h.shardSequence=i;h.receiveRealtimeNs=h.receiveMonotonicNs=100+i*100;h.exchangeTimestampNs=static_cast<std::int64_t>(h.receiveMonotonicNs)-1;h.flags=BinaryMarketRecordTraderReplayCompatible;h.sourceFlags=BinaryMarketSourceBookTickerBidPresent|BinaryMarketSourceBookTickerAskPresent;h.payloadBytes=sizeof(BinaryMarketBookTickerPayload);*binaryMarketPayload<BinaryMarketBookTickerPayload>(&record)={static_cast<std::int64_t>(100+i*10),1,static_cast<std::int64_t>(100+i*10),1};CXET_CHECK(hftrec::isOk(writer.append(record)));}
  CorpusProducerProgress progress;progress.producerEpoch=13;progress.safeMonotonicNs=401;progress.shardCount=1;progress.shardSequences[0]=3;progress.directoryRevision=1;CXET_CHECK(hftrec::isOk(writer.updateProgress(progress)));CXET_CHECK(hftrec::isOk(writer.publishCut()));CXET_CHECK(hftrec::isOk(writer.finalize(reason,500)));
 }
 ServiceConfig config(){ServiceConfig config;config.root=temporary.path/"research";config.buildIdentity="build";config.moduleIdentity="module";return config;}
 ExperimentSpec spec(std::uint32_t batch=4096){auto spec=studySpec();spec.sourcePath=root;spec.sourceIdentity=hftrec::corpus::corpusIdentity(writer.latestCut()->manifest);spec.studyIntervalNs=100;spec.budget.inputBatchEvents=batch;return spec;}
};
Response command(ResearchService& service,Command type,const std::string& id="study"){Request request;request.command=type;request.experimentId=id;request.resultKind=ResultKind::Series;return service.dispatch(request);}
void completeArchiveRestartUsesNewGenerationAndSameRows(){
 ArchiveFixture fixture;ResearchService service(fixture.config());std::string error;CXET_CHECK(service.restore(error));Request create;create.command=Command::Create;create.spec=fixture.spec();CXET_CHECK(service.dispatch(create).ok);CXET_CHECK(command(service,Command::Start).ok);service.step();auto first=command(service,Command::Status);CXET_CHECK(first.ok&&first.experiments[0].state==ExperimentState::Stopped);const auto generation=first.experiments[0].visibleGeneration;auto rows=command(service,Command::Range);CXET_CHECK(rows.ok&&rows.rows.size()==2);CXET_CHECK(rows.rows[0].exchange=="x"&&rows.rows[0].market=="spot"&&rows.rows[0].instrument=="A"&&rows.rows[0].currencyAssetId==2);
 auto restarted=command(service,Command::Start);CXET_CHECK(restarted.ok&&restarted.experiments[0].generation>generation&&restarted.experiments[0].visibleGeneration==generation);service.step();auto second=command(service,Command::Status);CXET_CHECK(second.experiments[0].state==ExperimentState::Stopped&&second.experiments[0].visibleGeneration>generation);auto again=command(service,Command::Range);CXET_CHECK(again.ok&&again.rows.size()==rows.rows.size());for(std::size_t i=0;i<rows.rows.size();++i)CXET_CHECK(again.rows[i].value==rows.rows[i].value&&again.rows[i].inputCut==rows.rows[i].inputCut);CXET_CHECK(std::filesystem::exists(fixture.config().root/"study"/("attempt-"+std::to_string(generation))/"segment-0.bin"));
}
void interruptedAttemptReplaysWithoutAppendingOldGeneration(){
 ArchiveFixture fixture;auto config=fixture.config();std::uint64_t oldGeneration{};
 {ResearchService service(config);std::string error;CXET_CHECK(service.restore(error));Request create;create.command=Command::Create;create.spec=fixture.spec(1);CXET_CHECK(service.dispatch(create).ok);auto started=command(service,Command::Start);CXET_CHECK(started.ok);oldGeneration=started.experiments[0].generation;service.step();service.step();CXET_CHECK(command(service,Command::Status).experiments[0].backgroundEnabled);}
 const auto oldSegment=config.root/"study"/("attempt-"+std::to_string(oldGeneration))/"segment-0.bin";CXET_CHECK(std::filesystem::exists(oldSegment));const auto previousBytes=std::filesystem::file_size(oldSegment);
 {ResearchService service(config);std::string error;CXET_CHECK(service.restore(error));for(int n=0;n<5;++n)service.step();auto status=command(service,Command::Status);CXET_CHECK(status.experiments[0].state==ExperimentState::Stopped&&status.experiments[0].visibleGeneration>oldGeneration);auto rows=command(service,Command::Range);CXET_CHECK(rows.ok&&rows.rows.size()==2);CXET_CHECK(std::filesystem::file_size(oldSegment)==previousBytes);}
}
void finalizedCaptureErrorIsIncomplete(){ArchiveFixture fixture(hftrec::corpus::BinaryMarketStopReason::Error);ResearchService service(fixture.config());std::string error;CXET_CHECK(service.restore(error));Request create;create.command=Command::Create;create.spec=fixture.spec();CXET_CHECK(service.dispatch(create).ok);CXET_CHECK(command(service,Command::Start).ok);service.step();const auto status=command(service,Command::Status).experiments[0];CXET_CHECK(status.state==ExperimentState::Incomplete&&!status.backgroundEnabled);CXET_CHECK(status.diagnostic.find("stop reason=")!=std::string::npos);}
void oneRootRefusesConcurrentServiceOwners(){TemporaryStore temporary;ServiceConfig config;config.root=temporary.path;config.buildIdentity="build";config.moduleIdentity="module";ResearchService first(config),second(config);std::string error;CXET_CHECK(first.restore(error));CXET_CHECK(!second.restore(error));CXET_CHECK(error.find("lease")!=std::string::npos);}
void restoreChecksAggregateMemoryReservations(){
 ArchiveFixture fixture;auto config=fixture.config();config.maxResidentStateBytes=2ULL<<30;
 {ResearchService service(config);std::string error;CXET_CHECK(service.restore(error));Request create;create.command=Command::Create;create.spec=fixture.spec();CXET_CHECK(service.dispatch(create).ok);CXET_CHECK(command(service,Command::Start).ok);create.spec.id="second";CXET_CHECK(service.dispatch(create).ok);CXET_CHECK(command(service,Command::Start,"second").ok);}
 config.maxResidentStateBytes=512ULL<<20;ResearchService restored(config);std::string error;CXET_CHECK(!restored.restore(error));CXET_CHECK(error.find("quota")!=std::string::npos);
}
void controlStateReplacementSurvivesDataQuota(){TemporaryStore temporary;ServiceConfig config;config.root=temporary.path;config.buildIdentity="build";config.moduleIdentity="module";config.maxExperiments=1;config.maxResidentVariants=1;config.maxServiceOutputBytes=2U<<20;{ResearchService service(config);std::string error;CXET_CHECK(service.restore(error));Request create;create.command=Command::Create;create.spec=studySpec();create.spec.budget.maxOutputBytes=1U<<20;CXET_CHECK(service.dispatch(create).ok);CXET_CHECK(command(service,Command::Stop).ok);}std::uint64_t occupied=0;for(const auto& entry:std::filesystem::recursive_directory_iterator(config.root))if(entry.is_regular_file())occupied+=entry.file_size();const auto filler=config.root/"preserved-data.bin";{std::ofstream out(filler,std::ios::binary);out.put('x');}std::filesystem::resize_file(filler,config.maxServiceOutputBytes-occupied);{ResearchService service(config);std::string error;CXET_CHECK(service.restore(error));CXET_CHECK(command(service,Command::Stop).ok);CXET_CHECK(command(service,Command::Pause).ok);}ResearchService restored(config);std::string error;CXET_CHECK(restored.restore(error));auto status=command(restored,Command::Status);CXET_CHECK(status.experiments[0].state==ExperimentState::Stopped&&!status.experiments[0].backgroundEnabled);}
void freshProducerClientDescribesSavedFullGridPages(){
 TemporaryStore temporary;ServiceConfig config;config.root=temporary.path/"store";config.socketPath=temporary.path/"research.sock";config.buildIdentity="build";config.moduleIdentity="module";auto specification=studySpec();specification.rawConfigText="native-study=returns";specification.rawGridText="interval_ns=100,200";std::string error;CXET_CHECK(expandExperimentGrid(specification,error));
 {ResearchService creator(config);CXET_CHECK(creator.restore(error));Request create;create.command=Command::Create;create.spec=specification;CXET_CHECK(creator.dispatch(create).ok);}
 ResearchService producer(config);CXET_CHECK(producer.restore(error));UnixResearchServer server(config,producer);CXET_CHECK(server.open(error));
 std::jthread owner([&](std::stop_token stop){std::string pollError;while(!stop.stop_requested())if(!server.poll(5,pollError))return;});
 ResearchClient client(config.socketPath);Request describe;describe.command=Command::Describe;describe.experimentId="study";describe.limit=1;
 const auto first=client.execute(describe);CXET_CHECK(first.ok&&first.hasSpecification&&first.specificationVariantCount==2&&first.specificationVariantOffset==0);CXET_CHECK(first.specification.rawConfigText==specification.rawConfigText&&first.specification.rawGridText==specification.rawGridText);CXET_CHECK(first.specification.variants.size()==1&&first.specification.variants[0].parameters[0].value=="100");CXET_CHECK(first.variantDescriptions.size()==1&&first.variantDescriptions[0].id==variantIdentity(specification,0));CXET_CHECK(first.variantDescriptions[0].label.find("interval_ns=100")!=std::string::npos&&first.more&&first.nextOffset==1);
 describe.offset=first.nextOffset;const auto second=client.execute(describe);CXET_CHECK(second.ok&&second.hasSpecification&&second.specificationVariantOffset==1&&second.specification.variants[0].parameters[0].value=="200"&&!second.more);CXET_CHECK(second.variantDescriptions[0].id==variantIdentity(specification,1));
 Request partial;partial.command=Command::Create;partial.spec=first.specification;partial.spec.id="invalid-partial-clone";CXET_CHECK(!client.execute(partial).ok);
 owner.request_stop();owner.join();
}
struct CounterStudyState {std::uint64_t variant{};std::int64_t count{};};
int counterStudyDestructions=0;
void nativeStudyGenericDescriptorOwnsItsBinding(){
 constexpr DataStudyOutputDescriptor outputs[]{{ResultKind::Series,"offline_counter","count"}};
 const DataStudyDescriptor descriptor{
  "offline_test_counter","offline generic binding fixture",std::uint16_t{1}<<(static_cast<unsigned>(hftrec::corpus::BinaryMarketChannel::Trade)-1),{},outputs,
  +[](const ExperimentSpec&,const ExperimentVariant&)noexcept->std::uint64_t{return sizeof(CounterStudyState);},
  +[](const ExperimentSpec&,const ExperimentVariant&,std::uint64_t id,DataStudyBinding& binding,std::string&)->bool{
   binding.context=new CounterStudyState{id,0};binding.residentBytes=sizeof(CounterStudyState);
   binding.observe=+[](void* raw,const hftrec::corpus::BinaryMarketRecord& record,DataStudyOutput output,void* user,std::string&)->bool{auto& state=*static_cast<CounterStudyState*>(raw);ResultRow row;row.kind=ResultKind::Series;row.variant=state.count?999:0;row.source=record.header.sourceId;row.inputCut=state.count?999999:0;row.timestampNs=state.count?200:0;row.metric="offline_counter";row.value=++state.count;return output(std::move(row),user);};
   binding.destroy=+[](void* raw)noexcept{++counterStudyDestructions;delete static_cast<CounterStudyState*>(raw);};return true;
  }
 };
 counterStudyDestructions=0;auto spec=studySpec();spec.module=descriptor.name;std::string error;std::vector<ResultRow> rows;
 {DataStudySession session;CXET_CHECK(session.prepare(descriptor,spec,spec.variants[0],91,error));hftrec::corpus::BinaryMarketRecord record;record.header.sourceId=1;record.header.receiveMonotonicNs=123;record.header.receiveRealtimeNs=321;record.header.channel=hftrec::corpus::BinaryMarketChannel::BookTicker;CXET_CHECK(session.observe(record,collect,&rows,error)&&rows.empty());record.header.channel=hftrec::corpus::BinaryMarketChannel::Trade;CXET_CHECK(session.observe(record,collect,&rows,error));CXET_CHECK(rows.size()==1&&rows[0].variant==91&&rows[0].metric=="offline_counter"&&rows[0].value==1&&rows[0].inputCut==123&&rows[0].timestampNs==321);record.header.receiveMonotonicNs=124;record.header.receiveRealtimeNs=322;CXET_CHECK(session.observe(record,collect,&rows,error));CXET_CHECK(rows.size()==2&&rows[1].variant==91&&rows[1].inputCut==124&&rows[1].timestampNs==200&&rows[1].value==2);}
 CXET_CHECK(counterStudyDestructions==1);spec.budget.maxStateBytes=1;DataStudySession bounded;CXET_CHECK(!bounded.prepare(descriptor,spec,spec.variants[0],91,error));CXET_CHECK(counterStudyDestructions==1);
}
void completedJobsReleaseComputeWhileAnalyticsStayReadable(){
 ArchiveFixture fixture;auto config=fixture.config();config.maxResidentStateBytes=512ULL<<20;ResearchService service(config);std::string error;CXET_CHECK(service.restore(error));Request create;create.command=Command::Create;create.spec=fixture.spec();CXET_CHECK(service.dispatch(create).ok);CXET_CHECK(command(service,Command::Start).ok);CXET_CHECK(command(service,Command::Pause).ok);create.spec.id="second";CXET_CHECK(service.dispatch(create).ok);CXET_CHECK(!command(service,Command::Start,"second").ok);CXET_CHECK(command(service,Command::Resume).ok);for(int n=0;n<4;++n)service.step();const auto oldStatus=command(service,Command::Status).experiments[0];CXET_CHECK(oldStatus.state==ExperimentState::Stopped);const auto oldRows=command(service,Command::Range);CXET_CHECK(oldRows.ok&&oldRows.rows.size()==2);CXET_CHECK(command(service,Command::Start,"second").ok);for(int n=0;n<4;++n)service.step();CXET_CHECK(command(service,Command::Status,"second").experiments[0].state==ExperimentState::Stopped);const auto retained=command(service,Command::Range);CXET_CHECK(retained.ok&&retained.rows.size()==oldRows.rows.size()&&retained.rows[0].value==oldRows.rows[0].value);CXET_CHECK(command(service,Command::Status).experiments[0].visibleGeneration==oldStatus.visibleGeneration);
}
void incompatibleRemovedModuleKeepsReadOnlyAnalyticsAndFutureJobs(){
 ArchiveFixture fixture;auto config=fixture.config();std::string error;
 {ResearchService service(config);CXET_CHECK(service.restore(error));Request create;create.command=Command::Create;create.spec=fixture.spec();CXET_CHECK(service.dispatch(create).ok);CXET_CHECK(command(service,Command::Start).ok);service.step();CXET_CHECK(command(service,Command::Status).experiments[0].state==ExperimentState::Stopped);}
 // Fixture reproduces a previously supported producer's current wire schema.
 auto historic=fixture.spec();historic.buildIdentity="historical-build";historic.moduleIdentity="historical-module";historic.module="retired_native_study";historic.profile="retired_profile";historic.valuationPolicy="retired_valuation";historic.fees.push_back({"x","spot",-1,0});const auto historicId=variantIdentity(historic,0);const auto directory=config.root/"study";std::vector<std::uint8_t> bytes;CXET_CHECK(detail::readFile(directory/"state.bin",bytes,error));auto state=detail::decode<ExperimentStatus>(std::move(bytes));state.specFingerprint=detail::fingerprint(detail::encode(historic));state.variants[0].id=historicId;state.state=ExperimentState::Running;state.backgroundEnabled=true;
 for(const auto& entry:std::filesystem::recursive_directory_iterator(directory))if(entry.is_regular_file()&&entry.path().filename().string().starts_with("segment-")&&entry.path().extension()==".bin"){
  CXET_CHECK(detail::readFile(entry.path(),bytes,error));detail::Codec decoder(true);decoder.bytes=std::move(bytes);std::uint64_t oldHash{},schema{};decoder.integer(oldHash);decoder.integer(schema);std::vector<ResultRow> rows;decoder.list(rows,4096,[&](auto& row){detail::code(decoder,row);});decoder.done();for(auto& row:rows)row.variant=historicId;detail::Codec payload;payload.integer(schema);payload.list(rows,4096,[&](auto& row){detail::code(payload,row);});detail::Codec framed;auto hash=detail::fingerprint(payload.bytes);framed.integer(hash);framed.bytes.insert(framed.bytes.end(),payload.bytes.begin(),payload.bytes.end());CXET_CHECK(detail::durableWrite(entry.path(),framed.bytes,true,error));
 }
 CXET_CHECK(detail::durableWrite(directory/"spec.bin",detail::encode(historic),true,error));CXET_CHECK(detail::durableWrite(directory/"state.bin",detail::encode(state),true,error));
 ResearchService service(config);CXET_CHECK(service.restore(error));const auto status=command(service,Command::Status).experiments[0];CXET_CHECK(status.state==ExperimentState::Failed&&!status.backgroundEnabled&&status.diagnostic.find("incompatible")!=std::string::npos);auto description=command(service,Command::Describe);CXET_CHECK(description.ok&&description.hasSpecification&&description.specification.module==historic.module&&description.specification.profile==historic.profile);auto rows=command(service,Command::Range);CXET_CHECK(rows.ok&&rows.rows.size()==2&&rows.rows[0].variant==historicId);CXET_CHECK(!command(service,Command::Start).ok);service.step();CXET_CHECK(command(service,Command::Status).experiments[0].generation==state.generation);
 Request create;create.command=Command::Create;create.spec=fixture.spec();create.spec.id="future";CXET_CHECK(service.dispatch(create).ok);CXET_CHECK(command(service,Command::Start,"future").ok);for(int n=0;n<4;++n)service.step();CXET_CHECK(command(service,Command::Status,"future").experiments[0].state==ExperimentState::Stopped);CXET_CHECK(command(service,Command::Range).rows.size()==2);
}
constexpr cxet::testing::Case cases[]{
 {"research_full_grid_stable_identity_and_cap",exactGridHasFullStableCartesianProduct},
 {"research_grid_refuses_missing_placeholder_and_wrong_direction",gridRejectsMissingPlaceholdersAndWrongRangeDirection},
 {"research_bounded_schema_protocol_guards",boundedCodecRefusesUnknownProtocolAndTruncation},
 {"research_native_study_receive_interval_and_distribution",nativeStudyUsesReceiveIntervalAndRealDistribution},
 {"research_immutable_catalog_restore_stopped_without_runtime",immutableCatalogRestoresStoppedWithoutRuntime},
 {"research_complete_archive_restart_new_generation",completeArchiveRestartUsesNewGenerationAndSameRows},
 {"research_interrupted_replay_isolates_old_generation",interruptedAttemptReplaysWithoutAppendingOldGeneration},
 {"research_finalized_capture_error_incomplete",finalizedCaptureErrorIsIncomplete},
 {"research_single_root_single_service_owner",oneRootRefusesConcurrentServiceOwners},
 {"research_restore_checks_aggregate_memory_reservation",restoreChecksAggregateMemoryReservations},
 {"research_control_replacement_at_data_quota",controlStateReplacementSurvivesDataQuota},
 {"research_fresh_client_describes_saved_grid_pages",freshProducerClientDescribesSavedFullGridPages},
 {"research_native_generic_descriptor_binding",nativeStudyGenericDescriptorOwnsItsBinding},
 {"research_completed_jobs_release_compute_keep_analytics",completedJobsReleaseComputeWhileAnalyticsStayReadable},
 {"research_incompatible_removed_module_read_only_history",incompatibleRemovedModuleKeepsReadOnlyAnalyticsAndFutureJobs}
};
}
int main(int argc,char** argv){return cxet::testing::runCases(argc,argv,cases);}
