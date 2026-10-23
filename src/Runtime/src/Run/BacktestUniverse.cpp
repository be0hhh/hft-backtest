#include "BacktestUniverse.hpp"
#include "hft_backtest/SimulationSession.hpp"
#include "../Config/BacktestRunConfig.hpp"
#include "../Input/Binary/Internal/BinaryCorpusHelpers.hpp"
#include "../Results/Internal/BacktestResultWriterInternal.hpp"
#include "Internal/BacktestRunHelpers.hpp"
#include <array>
#include <fstream>
#include <map>
#include <limits>

namespace hft_backtest::run_detail {
namespace {
// Duration-independent file owners. The same result callback is used by the
// continuous research service; archive replay supplies only another input feed.
struct ArchiveOutput {
  std::filesystem::path root;
  std::map<std::uint32_t,std::size_t> sourceLegs;
  std::map<std::string,std::uint64_t> counts;
  static bool write(std::uint32_t source,std::uint64_t,cxet::StringView stream,cxet::StringView row,void* raw) noexcept {
    try {
      auto& owner=*static_cast<ArchiveOutput*>(raw);
      const auto key=std::string(stream.data(),stream.size());
      auto emit=[&](const std::string& name) {
        std::error_code error;
        const auto path=owner.root/(name+".jsonl");
        std::filesystem::create_directories(path.parent_path(),error);
        if(error)return false;
        std::ofstream output(path,std::ios::out|(owner.counts.contains(name)?std::ios::app:std::ios::trunc));
        output.write(row.data(),static_cast<std::streamsize>(row.size()));
        output.flush();if(!output.good())return false;
        ++owner.counts[name];return true;
      };
      const auto leg=owner.sourceLegs.find(source);
      return leg!=owner.sourceLegs.end() && emit(key) && emit("legs/"+std::to_string(leg->second)+"/"+key);
    } catch(...) {return false;}
  }
};
}
void runBinaryUniverse(BacktestRunResult& result,const BacktestRunRequest& request,
    const BacktestStrategyConfig&,const trading_core::StrategyDescriptor&,
    BacktestProgressCallback progress,void* userData) noexcept {
  try {
    namespace corpus=hftrec::corpus;
    std::string error;
    corpus::BinaryMarketCorpusCursor cursor;
    corpus::BinaryMarketSelectionRequest selection;
    selection.root=request.corpusPath;selection.allSources=true;
    selection.channelMask=0xffu;selection.requireTraderReplayCompatibility=false;
    selection.beginReceiveNs=request.corpusBeginReceiveNs;
    selection.endReceiveNs=request.corpusEndReceiveNs;
    if(!request.corpusSources.empty()) {
      selection.beginReceiveNs=request.corpusSources.front().beginReceiveNs;
      selection.endReceiveNs=request.corpusSources.front().endReceiveNs;
      for(const auto& source:request.corpusSources)
        if(source.beginReceiveNs!=selection.beginReceiveNs || source.endReceiveNs!=selection.endReceiveNs) {
          result.status=Status::InvalidArgument;result.error="one portfolio requires one captured interval";return;
        }
    }
    corpus::BinaryMarketSelection metadata;
    const auto opened=cursor.open(selection,metadata,error);
    if(!hftrec::isOk(opened)){result.status=detail::binary_corpus::convertStatus(opened);result.error=error;return;}
    std::shared_ptr<const corpus::CorpusCut> durable;
    const auto cutStatus=corpus::readCorpusCut(request.corpusPath,durable,error);
    corpus::CorpusCut cut;
    if(hftrec::isOk(cutStatus) && durable)cut=*durable;
    else {
      const auto catalog=corpus::BinaryMarketCorpusReader{}.catalog(request.corpusPath,cut.manifest,cut.sources,error);
      if(!hftrec::isOk(catalog)){result.status=detail::binary_corpus::convertStatus(catalog);result.error=error;return;}
    }
    if(!metadata.sourcesAtBegin.empty())cut.sources=metadata.sourcesAtBegin;
    if(!cut.progress.safeMonotonicNs && cut.manifest.complete) {
      cut.progress.safeMonotonicNs=cut.manifest.replayAnchorMonotonicNs;
      cut.progress.producerEpoch=cut.manifest.producerEpoch;
      cut.finalized=true;
    }
    ArchiveOutput output;
    output.root=result.resultPath;
    SimulationSession session;
    const SimulationOutputSink sink=request.writeArtifacts?SimulationOutputSink{ArchiveOutput::write,&output}:SimulationOutputSink{};
    const auto prepared=session.prepare(request,cut,sink);
    if(!isOk(prepared)){result=session.result();return;}
    for(std::size_t leg=0;leg<session.result().legs.size();++leg) {
      const auto& admitted=session.result().legs[leg];
      for(const auto& source:cut.sources)
        if(detail::binary_corpus::fixedText(source.venue.data(),source.venue.size(),source.venueBytes)==admitted.exchange &&
           detail::binary_corpus::fixedText(source.market.data(),source.market.size(),source.marketBytes)==admitted.market &&
           detail::binary_corpus::fixedText(source.canonicalSymbol.data(),source.canonicalSymbol.size(),source.canonicalSymbolBytes)==admitted.symbol)
          output.sourceLegs[source.sourceId]=leg;
    }
    std::array<corpus::BinaryMarketRecord,256u> batch;
    std::size_t size=0u;bool available=true;
    std::uint64_t lastMono=0u;
    while(available) {
      const auto read=cursor.next(batch[size],available,error);
      if(!hftrec::isOk(read)){result=session.result();result.status=detail::binary_corpus::convertStatus(read);result.error=error;return;}
      if(available){lastMono=batch[size].header.receiveMonotonicNs;++size;}
      if(size==batch.size() || !available) {
        SimulationAdvance advanced;
        const auto status=session.advance({batch.data(),size},available?0u:(lastMono==std::numeric_limits<std::uint64_t>::max()?lastMono:lastMono+1u),advanced);
        if(!isOk(status)){result=session.result();return;}
        size=0u;
        if(advanced.paused)break;
        if(!reportProgress(session.result().events,0u,"running",progress,userData)) {
          session.pause();result=session.result();result.status=Status::Cancelled;result.error="cancelled";return;
        }
      }
    }
    (void)session.finish();result=session.result();
    if(request.writeArtifacts) {
      auto projection=std::make_unique<detail::EngineState>();
      projection->emittedFills=result.fills;projection->emittedOrders=result.orders;
      projection->emittedUserEvents=result.userEvents;
      projection->emittedEquityPoints=output.counts["equity"];projection->emittedRiskSnapshots=output.counts["risk_snapshots"];
      projection->emittedStrategyIndicator=output.counts["strategy_indicator"];projection->emittedStrategyRange=output.counts["strategy_range"];
      projection->emittedStrategySpread=output.counts["strategy_spread"];projection->emittedExecutionQuality=output.counts["execution_quality"];
      projection->emittedRateLimitUsage=output.counts["rate_limit_usage"];
      if(!detail::result_writer::writeRunConfig(result.resultPath,result.configPath) ||
          !detail::result_writer::writeManifest(result.resultPath/"manifest.json",result,*projection,nullptr,output.counts["order_lifetimes"])) {
        result.status=Status::IoError;result.error="archive result manifest write failed";
      }
    }
    (void)reportProgress(result.events,result.events,"complete",progress,userData);
  } catch(...) {result.status=Status::IoError;result.error="archive simulation feed allocation failed";}
}
}
