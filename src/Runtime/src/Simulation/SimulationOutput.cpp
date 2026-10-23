#include "SimulationOutput.hpp"
#include "../Results/Internal/BacktestResultWriterInternal.hpp"
#include <algorithm>
#include <sstream>
namespace hft_backtest::detail::simulation_output {
void writeRow(std::ostream& out,const JsonFill& row,const EngineState& engine) {

    out << '[' << row.orderId
        << ',' << row.entryTsNs
        << ',' << row.exitTsNs
        << ',' << static_cast<unsigned>(row.side)
        << ',' << row.priceE8
        << ',' << row.qtyE8
        << ',' << row.realizedPnlE8
        << ',' << (row.reduceOnly ? 1 : 0)
        << ',' << row.legIndex
        << ',' << static_cast<unsigned>(row.fillReason)
        << ',' << static_cast<unsigned>(row.liquidity)
        << ',' << row.orderQtyE8
        << ',' << row.cumulativeFilledQtyE8
        << ',' << row.remainingQtyE8
        << ',' << row.avgPriceE8
        << ',' << row.bookLevelQtyE8
        << ',' << row.bookVisibleExecutableQtyE8
        << ',' << row.bookConsumedPctE8
        << ',' << row.queueAheadBeforeE8
        << ',' << row.queueAheadAfterE8
        << ',' << row.chunkIndex
        << ',' << row.chunkCount
        << ',' << row.executionQtyE8
        << ',' << row.executionAvgPriceE8
        << ',' << row.referencePriceE8
        << ',' << row.slippageE8
        << ',' << row.slippageBpsE8
        << ',' << row.executionBookConsumedPctE8
        << ',' << row.contextIndex
        << "]\n";
  
}
void writeRow(std::ostream& out,const JsonRiskSnapshot& row,const EngineState& engine) {

    out << '[' << row.tsNs
        << ',' << row.grossRealizedPnlE8
        << ',' << row.realizedPnlE8
        << ',' << row.unrealizedPnlE8
        << ',' << row.grossTotalPnlE8
        << ',' << row.netTotalPnlE8
        << ',' << row.feesPaidE8
        << ',' << row.walletBalanceE8
        << ',' << row.positionQtyE8
        << ',' << row.entryPriceE8
        << ',' << row.bidPriceE8
        << ',' << row.askPriceE8
        << ',' << row.fillCount
        << ',' << static_cast<unsigned>(row.reason)
        << "]\n";
  
}
void writeRow(std::ostream& out,const JsonStrategyIndicatorPoint& row,const EngineState& engine) {
  const auto featureCount=result_writer::strategyIndicatorFeatureCount(engine.strategyIndicatorProfile);

    out << '[' << row.tsNs
        << ',' << row.valueRaw
        << ',' << row.auxRaw
        << ',' << static_cast<unsigned>(row.eventCode)
        << ',' << static_cast<unsigned>(row.reasonRaw)
        << ',' << static_cast<unsigned>(row.decisionKind)
        << ',' << static_cast<unsigned>(row.sideRaw);
    for (std::size_t i = 0u; i < featureCount && i < row.featureRaw.size(); ++i) {
      out << ',' << row.featureRaw[i];
    }
    out << "]\n";
  
}
void writeRow(std::ostream& out,const JsonStrategyRangePoint& row,const EngineState& engine) {

    out << '[' << row.tsNs
        << ',' << row.lowRaw
        << ',' << row.midRaw
        << ',' << row.highRaw
        << "]\n";
  
}
void writeRow(std::ostream& out,const JsonStrategySpreadPoint& row,const EngineState& engine) {

    out << '[' << row.tsNs
        << ',' << row.spreadBpsE8
        << ',' << row.emaBpsE8
        << ',' << row.deviationBpsE8
        << ',' << row.costBandBpsE8
        << ',' << row.edgeAfterCostBpsE8
        << ',' << static_cast<unsigned>(row.direction)
        << ',' << static_cast<unsigned>(row.decisionKind)
        << ',' << static_cast<unsigned>(row.reasonRaw)
        << "]\n";
  
}
void writeRow(std::ostream& out,const JsonExecutionQualityAction& row,const EngineState& engine) {

    std::uint8_t status = row.statusRaw;
    if (row.anchorRaw != static_cast<std::uint8_t>(trading_core::StrategyExecutionAnchor::MakerQuote) &&
        (status == static_cast<std::uint8_t>(trading_core::StrategyExecutionStatus::Proposed) ||
        status == static_cast<std::uint8_t>(trading_core::StrategyExecutionStatus::Accepted))) {
      const bool anyFill = row.legs[0].filledQtyE8 > 0 || row.legs[1].filledQtyE8 > 0;
      status = static_cast<std::uint8_t>(anyFill
          ? trading_core::StrategyExecutionStatus::Partial
          : trading_core::StrategyExecutionStatus::Uncertain);
    }
    out << '[' << row.actionId
        << ',' << row.positionCycleId
        << ',' << row.transactionId
        << ',' << row.pairKey
        << ',' << row.decisionTsNs
        << ',' << row.completedTsNs
        << ',' << static_cast<unsigned>(row.phaseRaw)
        << ',' << static_cast<unsigned>(row.anchorRaw)
        << ',' << static_cast<unsigned>(status)
        << ',' << static_cast<unsigned>(row.directionRaw)
        << ',' << static_cast<unsigned>(row.bandSideRaw)
        << ',' << row.decisionEdgeAfterCostBpsE8
        << ',' << row.fillEdgeAfterCostBpsE8
        << ',' << row.edgeLossBpsE8
        << ',' << row.requiredEdgeBpsE8
        << ',' << row.fairBpsE8
        << ',' << row.costBandBpsE8
        << ',' << row.quoteDecisionEdgeAfterCostBpsE8
        << ',' << row.decisionToFirstFillNs
        << ',' << row.decisionToLastFillNs
        << ',' << row.fillLegSkewNs
        << ',' << row.thresholdDeadlineNs
        << ',' << row.zeroEdgeDeadlineNs
        << ',' << row.pairIndex
        << ',' << row.pairSlot;
    for (const JsonExecutionQualityLeg& leg : row.legs) {
      out << ',' << leg.legIndex
          << ',' << leg.clientIntentId
          << ',' << static_cast<unsigned>(leg.sideRaw)
          << ',' << leg.expectedQtyE8
          << ',' << leg.filledQtyE8
          << ',' << leg.decisionPriceE8
          << ',' << leg.fillVwapE8
          << ',' << leg.firstFillTsNs
          << ',' << leg.lastFillTsNs;
    }
    out << "]\n";
  
}
void writeRow(std::ostream& out,const JsonRateLimitUsagePoint& row,const EngineState& engine) {

    out << '[' << row.tsNs
        << ',' << row.legIndex
        << ',' << row.minRemainingPctE4
        << ',' << row.remaining
        << ',' << row.limit
        << ',' << static_cast<unsigned>(row.bucketKind)
        << ',' << row.intervalNs
        << ',' << (row.exhausted ? 1 : 0)
        << "]\n";
  
}
void writeRow(std::ostream& out,const JsonEquityPoint& row,const EngineState&) {
  result_writer::writeEquityRow(out,row);
}
void writeRow(std::ostream& out,const JsonOrder& r,const EngineState&) {
  out << '[' << r.id << ',' << r.targetId << ',' << r.tsNs << ',' << r.activeTsNs << ','
      << static_cast<unsigned>(r.action) << ',' << static_cast<unsigned>(r.side) << ','
      << static_cast<unsigned>(r.type) << ',' << static_cast<unsigned>(r.status) << ','
      << r.priceE8 << ',' << r.qtyE8 << ',' << r.legIndex << ',' << r.contextIndex << "]\n";
}
bool emitLifetime(const EngineState& engine,const JsonOrder& order,std::uint32_t sourceId,
    std::uint64_t eventMono,SimulationOutputSink sink,bool openEnded=false,std::int64_t finalTs=0) {
  if(!sink.write || order.action!=kRecordActionOrder ||
      (order.type!=kRecordTypeLimit && order.type!=kRecordTypeStopMarket) ||
      !result_writer::isEffectiveOrderStatus(order.status))return true;
  const auto price=order.type==kRecordTypeStopMarket?order.triggerPriceE8:order.priceE8;
  if(order.activeTsNs<=0 || price<=0 || order.qtyE8<=0)return true;
  result_writer::OrderLifetimeIndexes indexes;
  result_writer::buildOrderLifetimeIndexes(engine,indexes);
  const result_writer::OrderLifetimeKey key{static_cast<std::uint64_t>(order.legIndex)<<32u | order.contextIndex,order.id};
  const auto* fill=result_writer::lifetimeFill(indexes,key);
  auto end=result_writer::lifetimeCommandEndTs(indexes,key,order.activeTsNs);
  if(fill && (!end || fill->exitTsNs<end))end=fill->exitTsNs;
  if(openEnded)end=finalTs;
  if(end<=order.activeTsNs)return true;
  const auto line="["+std::to_string(order.activeTsNs)+","+std::to_string(end)+","+std::to_string(price)+","+
      std::to_string(order.qtyE8)+","+std::to_string(order.side)+","+(openEnded?"1":"0")+","+
      std::to_string(order.legIndex)+","+std::to_string(order.type)+"]\n";
  return sink.write(sourceId,eventMono,"order_lifetimes",{line.data(),line.size()},sink.userData);
}
bool finishOpen(const EngineState& engine,std::uint32_t sourceId,std::uint64_t eventMono,std::int64_t finalTs,SimulationOutputSink sink) {
  if(!sink.write)return true;
  for(const auto& row:engine.orders) {
    std::ostringstream out;writeRow(out,row,engine);const auto bytes=out.str();
    if(!sink.write(sourceId,eventMono,"orders",{bytes.data(),bytes.size()},sink.userData) ||
        !emitLifetime(engine,row,sourceId,eventMono,sink,true,finalTs))return false;
  }
  return true;
}
bool compact(EngineState& engine,std::uint32_t sourceId,std::uint64_t eventMono,SimulationOutputSink sink) {
  const auto emit=[&](cxet::StringView stream,const auto& row) {
    if(!sink.write) return true;
    std::ostringstream out;writeRow(out,row,engine);
    const auto bytes=out.str();return sink.write(sourceId,eventMono,stream,{bytes.data(),bytes.size()},sink.userData);
  };
  const auto drain=[&](cxet::StringView stream,auto& rows,std::uint64_t* count=nullptr) {
    for(const auto& row:rows) if(!emit(stream,row)) return false;
    if(count)*count+=rows.size();rows.clear();return true;
  };
  std::vector<std::size_t> remap(engine.orders.size(),kNoRecordIndex);
  std::vector<unsigned char> keep(engine.orders.size());
  for(const auto& order:engine.open) if(order.state.valid && order.recordIndex<keep.size()) keep[order.recordIndex]=1u;
  for(const auto& pending:engine.pending) if(pending.recordIndex<keep.size()) keep[pending.recordIndex]=1u;
  std::size_t write=0u;
  for(std::size_t index=0;index<engine.orders.size();++index) {
    if(keep[index]) {remap[index]=write;if(write!=index) engine.orders[write]=engine.orders[index];++write;}
    else {
      if(!emit("orders",engine.orders[index]) || !emitLifetime(engine,engine.orders[index],sourceId,eventMono,sink))return false;
      ++engine.emittedOrders;
    }
  }
  engine.orders.resize(write);
  for(auto& order:engine.open) if(order.recordIndex<remap.size()) order.recordIndex=remap[order.recordIndex];
  for(auto& pending:engine.pending) if(pending.recordIndex<remap.size()) pending.recordIndex=remap[pending.recordIndex];
  const auto fills=engine.fills.size();
  if(!drain("fills",engine.fills))return false;
  engine.emittedFills+=fills;
  if(!drain("equity",engine.equityPoints,&engine.emittedEquityPoints) || !drain("risk_snapshots",engine.riskSnapshots,&engine.emittedRiskSnapshots) ||
     !drain("strategy_indicator",engine.strategyIndicator,&engine.emittedStrategyIndicator) || !drain("strategy_range",engine.strategyRange,&engine.emittedStrategyRange) ||
     !drain("strategy_spread",engine.strategySpread,&engine.emittedStrategySpread) || !drain("rate_limit_usage",engine.rateLimitUsage,&engine.emittedRateLimitUsage)) return false;
  // Execution quality remains mutable until completion; preserve only in-flight rows.
  bool outputOk=true;
  auto completed=std::remove_if(engine.executionQuality.begin(),engine.executionQuality.end(),[&](const auto& row){
    if(!row.completedTsNs) return false;
    if(!emit("execution_quality",row)){outputOk=false;return false;}++engine.emittedExecutionQuality;return true;
  });
  engine.executionQuality.erase(completed,engine.executionQuality.end());
  if(!outputOk || engine.executionQuality.size()>kMaxOrderStates*4u)return false;
  const auto consumed=engine.deliveredUserEvents;
  if(consumed>engine.userEvents.size())return false;
  for(std::size_t i=0;i<consumed;++i) {
    if(i>=engine.userEventConsumed.size() || !engine.userEventConsumed[i])return false;
    if(sink.write) {
      const auto& row=engine.userEvents[i];
      const auto line="["+std::to_string(engine.userEventCommandIds[i])+","+
          std::to_string(engine.userEventDeliveryTsNs[i])+","+std::to_string(row.filledQty.raw)+"]\n";
      if(!sink.write(sourceId,eventMono,"user_events",{line.data(),line.size()},sink.userData))return false;
    }
  }
  const auto erasePrefix=[&](auto& rows){rows.erase(rows.begin(),rows.begin()+static_cast<std::ptrdiff_t>(consumed));};
  erasePrefix(engine.userEvents);erasePrefix(engine.userEventDeliveryTsNs);erasePrefix(engine.userEventClientIntentIds);
  erasePrefix(engine.userEventCommandIds);erasePrefix(engine.userEventGenerations);erasePrefix(engine.userEventContextIndices);
  erasePrefix(engine.userEventConsumed);erasePrefix(engine.userEventServiceFlags);
  engine.emittedUserEvents+=consumed;engine.deliveredUserEvents=0u;
  engine.lastDeliveredUserEvent=std::numeric_limits<std::size_t>::max();
  return true;
}
} // namespace hft_backtest::detail::simulation_output
