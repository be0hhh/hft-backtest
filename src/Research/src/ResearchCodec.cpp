#include "ResearchStorage.hpp"

namespace hft_backtest::research::detail {
void code(Codec& c,ExperimentSpec& s) {
 c.integer(s.schemaVersion); c.text(s.id,128); c.integer(s.kind); c.text(s.module,256);
 std::string path=s.sourcePath.string(); c.text(path,4096); if(c.reading)s.sourcePath=path;
 path=s.recorderRuntimeDirectory.string();c.text(path,4096);if(c.reading)s.recorderRuntimeDirectory=path;
 c.text(s.sourceIdentity,256); c.integer(s.beginReceiveNs); c.text(s.profile,256);
 c.text(s.rawConfigText); c.text(s.rawGridText);
 c.list(s.variants,4096,[&](auto& v){c.text(v.configText);c.list(v.parameters,64,[&](auto& p){c.text(p.name,256);c.text(p.value,4096);});});
 c.list(s.fees,256,[&](auto& f){c.text(f.exchange,128);c.text(f.market,128);c.integer(f.makerFeeBpsE8);c.integer(f.takerFeeBpsE8);});
 c.integer(s.initialBalanceE8);c.integer(s.hasStrategyBudget);c.integer(s.strategyBudgetE8);c.text(s.budgetCurrency,128);c.integer(s.marginBorrowAllowed);c.text(s.valuationPolicy,256);c.integer(s.valuationQuoteAssetId);c.integer(s.seed);c.text(s.buildIdentity,1024);c.text(s.moduleIdentity,1024);
 c.integer(s.budget.maxVariants);c.integer(s.budget.maxSources);c.integer(s.budget.maxContexts);c.integer(s.budget.maxStateBytes);c.integer(s.budget.inputBatchEvents);c.integer(s.budget.resultBatchRows);
 c.integer(s.budget.maxInputBytes);c.integer(s.budget.maxOutputBytes);c.integer(s.budget.maxResultRows);
 c.integer(s.studyIntervalNs);c.list(s.distributionBoundsE8,256,[&](auto& v){c.integer(v);});
 if(static_cast<unsigned>(s.kind)>static_cast<unsigned>(ModuleKind::DataStudy))throw std::runtime_error("unknown module kind");
 if(s.schemaVersion!=kResearchSchemaVersion)throw std::runtime_error("research schema mismatch");
}
void code(Codec& c,ExperimentStatus& s) {
 std::uint64_t schema=kResearchSchemaVersion;c.integer(schema);if(schema!=kResearchSchemaVersion)throw std::runtime_error("research status schema mismatch");
 c.text(s.id,128);c.integer(s.specFingerprint);c.text(s.sourceIdentity,256);std::string path=s.sourcePath.string();c.text(path,4096);if(c.reading)s.sourcePath=path;c.integer(s.beginReceiveNs);c.integer(s.state);c.integer(s.backgroundEnabled);c.integer(s.generation);c.integer(s.visibleGeneration);c.integer(s.visibleFinal);
 c.integer(s.inputCut);c.integer(s.visibleInputCut);c.integer(s.outputBytes);c.text(s.diagnostic,4096);
 c.list(s.variants,4096,[&](auto& v){c.integer(v.id);c.integer(v.events);c.integer(v.rows);c.integer(v.lastReceiveNs);c.integer(v.inputCut);c.integer(v.terminal);c.integer(v.riskStopped);c.integer(v.terminalMonotonicNs);});
 if(static_cast<unsigned>(s.state)>static_cast<unsigned>(ExperimentState::QuotaExceeded))throw std::runtime_error("unknown experiment state");
}
void code(Codec& c,ResultRow& r) { c.integer(r.kind);c.integer(r.variant);c.integer(r.source);c.integer(r.inputCut);c.integer(r.accountId);c.integer(r.currencyAssetId);c.text(r.exchange,128);c.text(r.market,128);c.text(r.instrument,256);c.integer(r.timestampNs);c.text(r.metric,256);c.integer(r.value);c.integer(r.auxiliary);c.text(r.payload);if(static_cast<unsigned>(r.kind)>static_cast<unsigned>(ResultKind::Exposure))throw std::runtime_error("unknown result kind"); }
void code(Codec& c,Request& r) {
 c.integer(r.protocolVersion);c.integer(r.command);c.text(r.experimentId,128);code(c,r.spec);c.integer(r.resultKind);c.integer(r.variant);
 c.integer(r.beginNs);c.integer(r.endNs);c.integer(r.offset);c.integer(r.maximumInputCut);c.integer(r.limit);
 if(r.protocolVersion!=kResearchProtocolVersion)throw std::runtime_error("research protocol mismatch");
 if(static_cast<unsigned>(r.resultKind)>static_cast<unsigned>(ResultKind::Exposure))throw std::runtime_error("unknown result kind");
 if(static_cast<unsigned>(r.command)>static_cast<unsigned>(Command::Describe))throw std::runtime_error("unknown research command");
}
void code(Codec& c,Response& r) {
 c.integer(r.protocolVersion);c.integer(r.ok);c.text(r.error,4096);c.text(r.buildIdentity,1024);c.text(r.moduleIdentity,1024);c.integer(r.residentByteLimit);c.integer(r.residentVariantLimit);c.integer(r.hasSpecification);if(r.hasSpecification){code(c,r.specification);c.integer(r.specificationVariantCount);c.integer(r.specificationVariantOffset);c.list(r.variantDescriptions,4096,[&](auto& v){c.integer(v.id);c.integer(v.ordinal);c.text(v.label,1024);});if(r.specification.variants.size()!=r.variantDescriptions.size())throw std::runtime_error("specification page identity count mismatch");}
 c.list(r.catalog,4096,[&](auto& d){c.text(d.name,256);c.integer(d.kind);c.text(d.description,4096);c.list(d.parameters,64,[&](auto& p){c.text(p.name,256);c.text(p.defaultValue,4096);c.integer(p.kind);c.integer(p.required);});c.list(d.profiles,64,[&](auto& p){c.text(p,256);});});
 c.list(r.experiments,4096,[&](auto& e){code(c,e);});c.list(r.rows,4096,[&](auto& row){code(c,row);});c.integer(r.nextOffset);c.integer(r.more);
 if(r.protocolVersion!=kResearchProtocolVersion)throw std::runtime_error("research protocol mismatch");
}
} // namespace hft_backtest::research::detail
