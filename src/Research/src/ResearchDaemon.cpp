#include "hft_backtest/Research.hpp"
#include <csignal>
#include <cstdio>
#include <exception>

namespace hft_backtest::research {
namespace {volatile std::sig_atomic_t shutdownRequested=0;void stopSignal(int){shutdownRequested=1;}}
int runResearchDaemon(const ServiceConfig& config){
 try{ResearchService service(config);std::string error;if(!service.restore(error)){std::fprintf(stderr,"backtest research restore: %s\n",error.c_str());return 1;}
  UnixResearchServer server(config,service);if(!server.open(error)){std::fprintf(stderr,"backtest research endpoint: %s\n",error.c_str());return 1;}
  shutdownRequested=0;const auto oldInt=std::signal(SIGINT,stopSignal);const auto oldTerm=std::signal(SIGTERM,stopSignal);
  int result=0;while(!shutdownRequested){if(!server.poll(20,error)){std::fprintf(stderr,"backtest research control: %s\n",error.c_str());result=1;break;}service.step();}
  std::signal(SIGINT,oldInt);std::signal(SIGTERM,oldTerm);return result;
 }catch(const std::exception& e){std::fprintf(stderr,"backtest research daemon: %s\n",e.what());return 1;}
}
} // namespace hft_backtest::research
