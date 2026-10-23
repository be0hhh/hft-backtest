#include "ResearchStorage.hpp"
#include <algorithm>
#include <sstream>
#include <charconv>
#include <limits>
namespace hft_backtest::research {
std::string parameterKindName(std::uint8_t kind){switch(kind){case 0:return "text";case 1:return "unsigned integer";case 2:return "decimal amount";case 3:return "decimal quote amount";default:return "unknown";}}
namespace {
bool fixedDecimal(std::string text,std::int64_t& result){bool negative=false;if(!text.empty()&&(text.front()=='-'||text.front()=='+')){negative=text.front()=='-';text.erase(0,1);}if(text.empty())return false;__int128 value=0;unsigned decimals=0;bool point=false,digit=false;for(char c:text){if(c=='.'&&!point){point=true;continue;}if(c<'0'||c>'9')return false;digit=true;if(point&&++decimals>8)return false;value=value*10+(c-'0');if(value>static_cast<__int128>(std::numeric_limits<std::int64_t>::max())+1)return false;}if(!digit)return false;while(decimals++<8)value*=10;if(negative)value=-value;if(value<std::numeric_limits<std::int64_t>::min()||value>std::numeric_limits<std::int64_t>::max())return false;result=static_cast<std::int64_t>(value);return true;}
std::string decimalText(std::int64_t value){const bool negative=value<0;const auto absolute=negative?std::uint64_t(-(value+1))+1:std::uint64_t(value);std::string out=(negative?"-":"")+std::to_string(absolute/100000000);auto tail=absolute%100000000;if(tail){auto fraction=std::to_string(tail+100000000).substr(1);while(fraction.back()=='0')fraction.pop_back();out+='.';out+=fraction;}return out;}
bool gridValues(std::string text,std::size_t maximum,std::vector<std::string>& values,std::string& error){auto range=text.find("..");if(range==std::string::npos){if(text.empty()||text.front()==','||text.back()==','||text.find(",,")!=std::string::npos){error="empty grid value";return false;}std::istringstream cells{std::string(text)};std::string value;while(std::getline(cells,value,',')){if(value.empty()||values.size()>=maximum){error="empty or oversized grid values";return false;}values.push_back(value);}if(values.empty()){error="empty grid values";return false;}return true;}
 auto stepAt=text.find("..",range+2);if(stepAt==std::string::npos||text.find("..",stepAt+2)!=std::string::npos){error="range requires start..stop..step";return false;}std::int64_t begin{},end{},step{};if(!fixedDecimal(text.substr(0,range),begin)||!fixedDecimal(text.substr(range+2,stepAt-range-2),end)||!fixedDecimal(text.substr(stepAt+2),step)||!step||(begin<end&&step<0)||(begin>end&&step>0)){error="invalid exact decimal grid range";return false;}
 for(__int128 n=begin;step>0?n<=end:n>=end;n+=step){if(values.size()>=maximum){error="full grid range exceeds variant quota";return false;}values.push_back(decimalText(static_cast<std::int64_t>(n)));}return !values.empty();}
}

bool saveExperimentSpec(const std::filesystem::path& p,const ExperimentSpec& s,std::string& error){try{return detail::durableWrite(p,detail::encode(s),false,error);}catch(const std::exception& e){error=e.what();return false;}}
bool loadExperimentSpec(const std::filesystem::path& p,ExperimentSpec& s,std::string& error){try{std::vector<std::uint8_t>b;if(!detail::readFile(p,b,error))return false;s=detail::decode<ExperimentSpec>(std::move(b));return true;}catch(const std::exception&e){error=e.what();return false;}}
std::uint64_t variantIdentity(const ExperimentSpec& s,std::size_t ordinal){return detail::variantIdentityFromFingerprint(detail::fingerprint(detail::encode(s)),ordinal);}
bool expandExperimentGrid(ExperimentSpec& s,std::string& error) {
 if(!s.budget.maxVariants||s.budget.maxVariants>4096||s.rawConfigText.size()>(1U<<20)||s.rawGridText.size()>(1U<<20)){error="grid schema bounds exceeded";return false;}
 std::vector<ExperimentVariant> result{{s.rawConfigText,{}}};std::istringstream lines(s.rawGridText);std::string line;std::vector<std::string> keys;
 while(std::getline(lines,line)){
  if(line.empty())continue;auto split=line.find('=');if(split==std::string::npos||split==0){error="grid requires name=value,value lines";return false;}
  std::string name=line.substr(0,split);if(name.size()>256||keys.size()>=64){error="grid parameter bound exceeded";return false;}if(std::find(keys.begin(),keys.end(),name)!=keys.end()){error="duplicate grid parameter";return false;}keys.push_back(name);
  std::vector<std::string> values;if(!gridValues(line.substr(split+1),s.budget.maxVariants,values,error))return false;
  if(std::any_of(values.begin(),values.end(),[](const auto& value){return value.size()>4096;})||values.empty()||result.size()>s.budget.maxVariants/values.size()){error="full grid exceeds variant quota";return false;}
  std::vector<ExperimentVariant> expanded;std::size_t totalBytes=0;for(const auto& prior:result)for(const auto& item:values){auto variant=prior;variant.parameters.push_back({name,item});const std::string token="${"+name+"}";std::size_t at=0;bool found=false;while((at=variant.configText.find(token,at))!=std::string::npos){if(item.size()>token.size()&&variant.configText.size()>(1U<<20)-(item.size()-token.size())){error="expanded configuration exceeds byte limit";return false;}variant.configText.replace(at,token.size(),item);at+=item.size();found=true;}if(!found&&s.kind==ModuleKind::Strategy){error="grid parameter absent from full config template";return false;}totalBytes+=variant.configText.size();for(const auto& p:variant.parameters)totalBytes+=p.name.size()+p.value.size()+32;if(totalBytes>detail::kMaxWireBytes){error="full expanded grid exceeds message byte limit";return false;}expanded.push_back(std::move(variant));}result=std::move(expanded);
 }
 for(const auto& variant:result)if(variant.configText.find("${")!=std::string::npos){error="unexpanded configuration parameter";return false;}
 s.variants=std::move(result);return true;
}
} // namespace hft_backtest::research
