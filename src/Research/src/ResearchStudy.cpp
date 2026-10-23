#include "ResearchStudyModules.hpp"
#include <array>
#include <exception>
#include <limits>
#include <algorithm>

namespace hft_backtest::research {
namespace {
constexpr std::array registry{&kMidpriceReturnStudyDescriptor};
}
const DataStudyDescriptor* dataStudyDescriptorAt(std::size_t i)noexcept{return i<registry.size()?registry[i]:nullptr;}
std::size_t dataStudyDescriptorCount()noexcept{return registry.size();}
const DataStudyDescriptor* findDataStudyDescriptor(const std::string& name)noexcept{
 for(const auto* descriptor:registry)if(descriptor&&descriptor->name&&name==descriptor->name)return descriptor;
 return nullptr;
}
struct DataStudySession::Impl {
 struct OutputRule{ResultKind kind;std::string metric;};
 DataStudyBinding binding;
 std::vector<OutputRule> outputs;
 std::uint16_t inputChannels{};
 std::uint64_t variant{};
 void reset()noexcept{if(binding.destroy)binding.destroy(binding.context);binding={};inputChannels=0;variant=0;outputs.clear();}
 ~Impl(){reset();}
};
DataStudySession::DataStudySession():impl_(std::make_unique<Impl>()){}
DataStudySession::~DataStudySession()=default;
bool DataStudySession::prepare(const ExperimentSpec& spec,const ExperimentVariant& variant,std::uint64_t id,std::string& error){
 const auto* descriptor=findDataStudyDescriptor(spec.module);if(!descriptor){error="unknown native DataStudy module";return false;}
 return prepare(*descriptor,spec,variant,id,error);
}
bool DataStudySession::prepare(const DataStudyDescriptor& descriptor,const ExperimentSpec& spec,const ExperimentVariant& variant,std::uint64_t id,std::string& error){
 auto& s=*impl_;s.reset();
 if(!descriptor.name||spec.module!=descriptor.name||!descriptor.prepare||!descriptor.requiredStateBytes||descriptor.parameters.size()>64||descriptor.outputs.size()>64||(descriptor.inputChannels&~std::uint16_t{0x1ff})!=0){error="invalid native DataStudy descriptor";return false;}
 std::vector<Impl::OutputRule> outputs;std::uint64_t overhead=sizeof(Impl)+4096;
 for(const auto& output:descriptor.outputs){if(!output.metric||!output.unit||static_cast<unsigned>(output.kind)>static_cast<unsigned>(ResultKind::Exposure)){error="invalid declared DataStudy output";return false;}std::size_t size=0;while(size<=256&&output.metric[size])++size;if(!size||size>256){error="DataStudy output name bound exceeded";return false;}overhead+=sizeof(Impl::OutputRule)+size+1;}
 for(std::size_t i=0;i<variant.parameters.size();++i){const auto& parameter=variant.parameters[i];if(std::none_of(descriptor.parameters.begin(),descriptor.parameters.end(),[&](const auto& declared){return declared.name&&parameter.name==declared.name;})){error="undeclared native DataStudy parameter";return false;}for(std::size_t j=0;j<i;++j)if(variant.parameters[j].name==parameter.name){error="duplicate native DataStudy parameter";return false;}}
 const auto bound=descriptor.requiredStateBytes(spec,variant);if(bound==std::numeric_limits<std::uint64_t>::max()||overhead>spec.budget.maxStateBytes||bound>spec.budget.maxStateBytes-overhead){error="DataStudy declared state exceeds selected memory budget";return false;}
 outputs.reserve(descriptor.outputs.size());for(const auto& output:descriptor.outputs)outputs.push_back({output.kind,output.metric});
 DataStudyBinding prepared;
 try{if(!descriptor.prepare(spec,variant,id,prepared,error)){if(prepared.destroy)prepared.destroy(prepared.context);return false;}}
 catch(const std::exception& exception){if(prepared.destroy)prepared.destroy(prepared.context);error=exception.what();return false;}
 if(!prepared.observe||!prepared.destroy||prepared.residentBytes>bound){if(prepared.destroy)prepared.destroy(prepared.context);error="native DataStudy binding violates its state/callback declaration";return false;}
 s.binding=prepared;s.inputChannels=descriptor.inputChannels;s.variant=id;s.outputs=std::move(outputs);return true;
}
bool DataStudySession::observe(const hftrec::corpus::BinaryMarketRecord& record,DataStudyOutput output,void* user,std::string& error){
 auto& s=*impl_;if(!s.binding.observe||!output){error="DataStudy has no prepared binding/output sink";return false;}
 const auto channel=static_cast<unsigned>(record.header.channel);if(!channel||channel>9){error="invalid DataStudy input channel";return false;}
 if((s.inputChannels&(std::uint16_t{1}<<(channel-1)))==0)return true;
 struct OutputContext{Impl* state;DataStudyOutput downstream;void* user;std::string* error;std::uint64_t monotonic{};std::int64_t receiveRealtime{};bool failed{};};OutputContext context{&s,output,user,&error,record.header.receiveMonotonicNs,record.header.receiveRealtimeNs};
 const bool accepted=s.binding.observe(s.binding.context,record,+[](ResultRow row,void* raw){auto& context=*static_cast<OutputContext*>(raw);if(context.failed)return false;if(std::none_of(context.state->outputs.begin(),context.state->outputs.end(),[&](const auto& declared){return declared.kind==row.kind&&declared.metric==row.metric;})){*context.error="DataStudy emitted an undeclared output";context.failed=true;return false;}row.variant=context.state->variant;row.inputCut=context.monotonic;if(!row.timestampNs)row.timestampNs=context.receiveRealtime;if(!context.downstream(std::move(row),context.user)){context.failed=true;return false;}return true;},&context,error);return accepted&&!context.failed;
}
} // namespace hft_backtest::research
