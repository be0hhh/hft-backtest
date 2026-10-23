#include "hft_backtest/Research.hpp"
#include <charconv>
#include <cstdio>
#include <limits>
#include <string>

namespace {
bool positive(const std::string& text,std::uint64_t& value){
 const auto [end,error]=std::from_chars(text.data(),text.data()+text.size(),value);
 return error==std::errc{}&&end==text.data()+text.size()&&value!=0;
}
}
int main(int argc,char** argv){
 hft_backtest::research::ServiceConfig config;
 for(int i=1;i<argc;++i){
  const std::string option(argv[i]);
  if(option=="--help"){
   std::puts("hft-backtestd --root PATH --socket PATH --build-id ID --module-id ID\n"
             "  [--max-state-bytes N] [--max-resident-variants N]\n"
             "  [--max-output-bytes N] [--max-experiments N] [--work-items-per-step N (1..64)]");return 0;
  }
  if(i+1>=argc){std::fprintf(stderr,"missing value for %s\n",argv[i]);return 2;}
  const std::string value(argv[++i]);
  if(option=="--root")config.root=value;
  else if(option=="--socket")config.socketPath=value;
  else if(option=="--build-id")config.buildIdentity=value;
  else if(option=="--module-id")config.moduleIdentity=value;
  else if(option=="--max-state-bytes"||option=="--max-resident-variants"||option=="--max-output-bytes"||option=="--max-experiments"||option=="--work-items-per-step"){
   std::uint64_t number{};if(!positive(value,number)){std::fprintf(stderr,"%s requires a positive decimal integer\n",option.c_str());return 2;}
   if(option=="--max-state-bytes")config.maxResidentStateBytes=number;
   else if(option=="--max-output-bytes")config.maxServiceOutputBytes=number;
   else {if(number>std::numeric_limits<std::uint32_t>::max()){std::fprintf(stderr,"%s exceeds its integer bound\n",option.c_str());return 2;}const auto bounded=static_cast<std::uint32_t>(number);if(option=="--max-resident-variants")config.maxResidentVariants=bounded;else if(option=="--max-experiments")config.maxExperiments=bounded;else config.maxWorkItemsPerStep=bounded;}
  }else{std::fprintf(stderr,"unknown research option %s\n",option.c_str());return 2;}
 }
 if(config.root.empty()||config.socketPath.empty()||config.buildIdentity.empty()||config.moduleIdentity.empty()){
  std::fputs("explicit root, socket, build-id and module-id are required\n",stderr);return 2;
 }
 return hft_backtest::research::runResearchDaemon(config);
}
