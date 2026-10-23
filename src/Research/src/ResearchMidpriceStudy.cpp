#include "ResearchStudyModules.hpp"
#include <algorithm>
#include <charconv>
#include <limits>

namespace hft_backtest::research {
namespace {
struct Sample {std::int64_t mid{};std::uint64_t monotonic{};};
struct MidpriceState {
 std::vector<Sample> samples;
 std::vector<std::uint64_t> bins;
 std::vector<std::int64_t> bounds;
 std::uint64_t interval{},variant{};
 std::int64_t begin{};
};
std::uint64_t requiredState(const ExperimentSpec& spec,const ExperimentVariant&)noexcept{
 if(!spec.budget.maxSources||spec.budget.maxSources>kResearchMaximumSources||spec.distributionBoundsE8.size()>256)return std::numeric_limits<std::uint64_t>::max();
 const auto slots=std::uint64_t(spec.budget.maxSources)+1;
 return sizeof(MidpriceState)+slots*sizeof(Sample)+slots*(spec.distributionBoundsE8.size()+1)*sizeof(std::uint64_t)+spec.distributionBoundsE8.size()*sizeof(std::int64_t)+4096;
}
bool observe(void* raw,const hftrec::corpus::BinaryMarketRecord& record,DataStudyOutput output,void* user,std::string& error){
 using namespace hftrec::corpus;auto& s=*static_cast<MidpriceState*>(raw);
 if(record.header.receiveRealtimeNs<s.begin)return true;
 if(record.header.sourceId==0||record.header.sourceId>=s.samples.size()){error="DataStudy source capacity exceeded";return false;}
 if(record.header.channel==BinaryMarketChannel::SourceLifecycle){s.samples[record.header.sourceId]={};return true;}
 if((record.header.sourceFlags&(BinaryMarketSourceStale|BinaryMarketSourceDegraded|BinaryMarketSourceSequenceGap))!=0){s.samples[record.header.sourceId]={};return true;}
 const auto present=BinaryMarketSourceBookTickerBidPresent|BinaryMarketSourceBookTickerAskPresent;if((record.header.sourceFlags&present)!=present)return true;
 const auto* payload=binaryMarketPayload<BinaryMarketBookTickerPayload>(&record);if(record.header.payloadBytes!=sizeof(*payload)){error="invalid book ticker payload";return false;}
 if(payload->bidPriceRaw<=0||payload->askPriceRaw<payload->bidPriceRaw)return true;
 const auto mid=payload->bidPriceRaw+(payload->askPriceRaw-payload->bidPriceRaw)/2;auto& previous=s.samples[record.header.sourceId];const auto time=record.header.receiveRealtimeNs;
 if(previous.monotonic&&record.header.receiveMonotonicNs/s.interval==previous.monotonic/s.interval)return true;
 if(previous.mid){const __int128 scaled=static_cast<__int128>(mid-previous.mid)*100000000/previous.mid;if(scaled<std::numeric_limits<std::int64_t>::min()||scaled>std::numeric_limits<std::int64_t>::max()){error="DataStudy return overflow";return false;}
  ResultRow row;row.kind=ResultKind::Series;row.variant=s.variant;row.source=record.header.sourceId;row.inputCut=record.header.receiveMonotonicNs;row.timestampNs=time;row.metric="midprice_return_e8";row.value=static_cast<std::int64_t>(scaled);if(!output(row,user)){if(error.empty())error="DataStudy result sink refused";return false;}
  const auto bin=static_cast<std::size_t>(std::upper_bound(s.bounds.begin(),s.bounds.end(),row.value)-s.bounds.begin());auto& count=s.bins[record.header.sourceId*(s.bounds.size()+1)+bin];if(count==static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())){error="DataStudy histogram overflow";return false;}++count;row.kind=ResultKind::Distribution;row.metric="midprice_return_histogram";row.value=static_cast<std::int64_t>(count);row.auxiliary=static_cast<std::int64_t>(bin);if(!output(std::move(row),user)){if(error.empty())error="DataStudy result sink refused";return false;}
 }
 previous={mid,record.header.receiveMonotonicNs};return true;
}
bool prepare(const ExperimentSpec& spec,const ExperimentVariant& variant,std::uint64_t id,DataStudyBinding& binding,std::string& error){
 const auto bound=requiredState(spec,variant);if(bound==std::numeric_limits<std::uint64_t>::max()||bound>spec.budget.maxStateBytes){error="DataStudy state exceeds selected memory budget";return false;}
 auto state=std::make_unique<MidpriceState>();auto& s=*state;s.interval=spec.studyIntervalNs;s.variant=id;s.begin=spec.beginReceiveNs;s.bounds=spec.distributionBoundsE8;bool hasInterval=false;
 for(const auto& p:variant.parameters){if(p.name!="interval_ns"||hasInterval){error="unsupported or repeated DataStudy parameter: "+p.name;return false;}hasInterval=true;const auto [end,ec]=std::from_chars(p.value.data(),p.value.data()+p.value.size(),s.interval);if(ec!=std::errc{}||end!=p.value.data()+p.value.size()||!s.interval){error="invalid study interval_ns";return false;}}
 if(!s.interval||!std::is_sorted(s.bounds.begin(),s.bounds.end())||std::adjacent_find(s.bounds.begin(),s.bounds.end())!=s.bounds.end()){error="invalid DataStudy interval/distribution bounds";return false;}
 const auto slots=std::size_t(spec.budget.maxSources)+1;s.samples.assign(slots,{});s.bins.assign(slots*(s.bounds.size()+1),0);
 binding.context=state.release();binding.observe=observe;binding.destroy=+[](void* value)noexcept{delete static_cast<MidpriceState*>(value);};binding.residentBytes=bound;return true;
}
constexpr DataStudyParameter parameters[]{{"interval_ns","1000000000",1,true}};
constexpr DataStudyOutputDescriptor outputs[]{
 {ResultKind::Series,"midprice_return_e8","ratio_e8"},
 {ResultKind::Distribution,"midprice_return_histogram","count"}
};
constexpr std::uint16_t inputChannels=(std::uint16_t{1}<<(static_cast<unsigned>(hftrec::corpus::BinaryMarketChannel::BookTicker)-1))|(std::uint16_t{1}<<(static_cast<unsigned>(hftrec::corpus::BinaryMarketChannel::SourceLifecycle)-1));
}
const DataStudyDescriptor kMidpriceReturnStudyDescriptor{
 "midprice_return_distribution","First valid BBO in each fixed receive-monotonic bucket: integer midprice returns and histogram; no trading",
 inputChannels,parameters,outputs,requiredState,prepare
};
} // namespace hft_backtest::research
