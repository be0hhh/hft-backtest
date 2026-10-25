#include "ResearchStorage.hpp"
#include "hft_backtest/StrategyMetadata.hpp"
#include "hft_backtest/DataStudy.hpp"
#include <algorithm>

namespace hft_backtest::research {
static_assert(kResearchMaximumSources==hftrec::corpus::kBinaryMarketMaximumSources);
namespace detail {
bool validateStoredExperimentSpec(const ExperimentSpec& s,std::string& error){
 auto refuse=[&](const char* text){error=text;return false;};
 if(s.id.empty()||s.id.size()>128||!std::all_of(s.id.begin(),s.id.end(),[](unsigned char x){return (x>='a'&&x<='z')||(x>='A'&&x<='Z')||(x>='0'&&x<='9')||x=='_'||x=='-';}))return refuse("invalid experiment id");
 if(s.sourcePath.string().find('\0')!=std::string::npos||s.recorderRuntimeDirectory.string().find('\0')!=std::string::npos)return refuse("embedded NUL in immutable input path");
 if(s.sourcePath.empty()||s.sourceIdentity.empty()||s.beginReceiveNs<0)return refuse("explicit source identity/start required");
 if(s.buildIdentity.empty()||s.moduleIdentity.empty()||s.module.empty())return refuse("stored module/build provenance is incomplete");
 if(static_cast<unsigned>(s.kind)>static_cast<unsigned>(ModuleKind::DataStudy))return refuse("unknown stored module kind");
 if(!s.budget.maxVariants||s.budget.maxVariants>4096||s.variants.empty()||s.variants.size()>s.budget.maxVariants)return refuse("invalid stored full grid bound");
 if(!s.budget.maxContexts||s.budget.maxContexts>kResearchMaximumContexts||!s.budget.maxStateBytes||!s.budget.maxSources||s.budget.maxSources>kResearchMaximumSources||!s.budget.inputBatchEvents||s.budget.inputBatchEvents>65536||!s.budget.resultBatchRows||s.budget.resultBatchRows>4096||!s.budget.maxInputBytes||s.budget.maxInputBytes>(1ULL<<30)||!s.budget.maxOutputBytes||!s.budget.maxResultRows)return refuse("invalid stored resource bounds");
 ExperimentSpec expanded=s;expanded.budget.maxVariants=static_cast<std::uint32_t>(s.variants.size());std::string expansionError;
 if(!expandExperimentGrid(expanded,expansionError))return refuse("invalid stored full grid expansion");
 if(expanded.variants.size()!=s.variants.size())return refuse("persisted variants do not cover full grid");
 for(std::size_t i=0;i<s.variants.size();++i){
  if(expanded.variants[i].configText!=s.variants[i].configText||expanded.variants[i].parameters.size()!=s.variants[i].parameters.size())return refuse("expanded grid configuration mismatch");
  for(std::size_t j=0;j<s.variants[i].parameters.size();++j)if(expanded.variants[i].parameters[j].name!=s.variants[i].parameters[j].name||expanded.variants[i].parameters[j].value!=s.variants[i].parameters[j].value)return refuse("expanded grid parameter mismatch");
 }
 try{(void)encode(s);}catch(const std::exception& exception){error=exception.what();return false;}
 return true;
}
} // namespace detail
bool validateExperimentSpec(const ExperimentSpec& s,const ServiceConfig& c,std::string& error){
 if(!detail::validateStoredExperimentSpec(s,error))return false;
 auto refuse=[&](const char* text){error=text;return false;};
 if(s.buildIdentity!=c.buildIdentity||s.moduleIdentity!=c.moduleIdentity)return refuse("exact build/module provenance mismatch");
 if(s.valuationPolicy!="midprice_e8")return refuse("unsupported valuation policy");
 if(s.variants.size()>c.maxResidentVariants||s.budget.maxStateBytes>c.maxResidentStateBytes||s.variants.size()>c.maxResidentStateBytes/s.budget.maxStateBytes)return refuse("resident variant state memory quota exceeded");
 if(s.budget.maxOutputBytes>c.maxServiceOutputBytes)return refuse("output byte budget exceeds service quota");
 for(const auto& fee:s.fees)if(fee.makerFeeBpsE8<0||fee.takerFeeBpsE8<0)return refuse("Unsupported scenario fee rebate: canonical finance supports nonnegative costs only");
 if(s.kind==ModuleKind::Strategy){
  const auto* metadata=findStrategyMetadata(s.module);if(!metadata)return refuse("unknown native strategy");
  if(!s.profile.empty()&&s.profile!="none"){bool found=false;for(std::size_t i=0;i<metadata->indicatorCount;++i)if(metadata->indicators[i].id&&s.profile==metadata->indicators[i].id)found=true;if(!found)return refuse("profile is not declared by selected native strategy");}
  for(const auto& variant:s.variants)if(variant.configText.empty())return refuse("expanded strategy config required");
  if(s.fees.empty())return refuse("explicit scenario fees required");
  for(std::size_t i=0;i<s.fees.size();++i){
   if(s.fees[i].exchange.empty()||s.fees[i].market.empty())return refuse("fee schedule requires exact venue/market");
   for(std::size_t j=0;j<i;++j)if(s.fees[i].exchange==s.fees[j].exchange&&s.fees[i].market==s.fees[j].market)return refuse("duplicate scenario fee schedule");
  }
  if(!s.valuationQuoteAssetId)return refuse("explicit valuation quote asset required");
  if(s.initialBalanceE8<=0)return refuse("explicit positive virtual account capital required");
 }else if(!findDataStudyDescriptor(s.module))return refuse("unknown native study");
 if(!s.studyIntervalNs||s.distributionBoundsE8.size()>256||!std::is_sorted(s.distributionBoundsE8.begin(),s.distributionBoundsE8.end())||std::adjacent_find(s.distributionBoundsE8.begin(),s.distributionBoundsE8.end())!=s.distributionBoundsE8.end())return refuse("invalid study interval/distribution boundaries");
 try{if(detail::encode(s).size()>detail::kMaxWireBytes-(16U<<10))return refuse("immutable specification exceeds Describe response headroom");}catch(const std::exception& exception){error=exception.what();return false;}
 return true;
}
std::vector<ModuleDescriptor> researchModuleCatalog(){
 std::vector<ModuleDescriptor> out;
 for(std::size_t i=0;i<strategyMetadataCount();++i){const auto* metadata=strategyMetadataAt(i);if(!metadata||!metadata->id)continue;ModuleDescriptor descriptor;descriptor.name=metadata->id;descriptor.kind=ModuleKind::Strategy;descriptor.description=metadata->displayNameRu?metadata->displayNameRu:metadata->id;for(std::size_t j=0;j<metadata->paramCount;++j){const auto& parameter=metadata->params[j];descriptor.parameters.push_back({parameter.key?parameter.key:"",parameter.defaultValue?parameter.defaultValue:"",static_cast<std::uint8_t>(parameter.kind),parameter.required});}descriptor.profiles.push_back("none");for(std::size_t j=0;j<metadata->indicatorCount;++j)if(metadata->indicators[j].id)descriptor.profiles.push_back(metadata->indicators[j].id);out.push_back(std::move(descriptor));}
 for(std::size_t i=0;i<dataStudyDescriptorCount();++i){const auto* study=dataStudyDescriptorAt(i);ModuleDescriptor descriptor;descriptor.name=study->name;descriptor.kind=ModuleKind::DataStudy;descriptor.description=study->description;for(const auto& parameter:study->parameters)descriptor.parameters.push_back({parameter.name?parameter.name:"",parameter.defaultValue?parameter.defaultValue:"",parameter.kind,parameter.required});descriptor.profiles.push_back("none");out.push_back(std::move(descriptor));}
 return out;
}
} // namespace hft_backtest::research
