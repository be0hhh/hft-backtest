#include "OfflineCase.hpp"
#include "ResearchTui.hpp"
#include <limits>
#include <string>

namespace {
namespace ui = hft_backtest::tui;
namespace research = hft_backtest::research;

void quotedPathsRemainOneArgument() {
  const auto command = ui::parseCommand("load \"/data/research run/spec.bin\"");
  CXET_CHECK(command.error.empty());
  CXET_CHECK(command.action == ui::Action::Load);
  CXET_CHECK(command.words.size() == 2);
  CXET_CHECK(command.words[1] == "/data/research run/spec.bin");
  CXET_CHECK(ui::parseCommand("describe research-a").action == ui::Action::Describe);
}
void malformedQuotesAreRejected() {
  const auto command = ui::parseCommand("set config \"/data/incomplete");
  CXET_CHECK(!command.error.empty());
}
void detachAndProducerStopAreDistinct() {
  CXET_CHECK(ui::parseCommand("quit").action == ui::Action::Quit);
  CXET_CHECK(ui::parseCommand("stop research-a").action == ui::Action::Stop);
  CXET_CHECK(ui::parseCommand("record-stop").action == ui::Action::RecordingStop);
  CXET_CHECK(ui::parseCommand("record-start").action == ui::Action::RecordingStart);
  CXET_CHECK(ui::parseCommand("record-configure").action == ui::Action::RecordingConfigure);
}
void oversizedCommandsDoNotDispatch() {
  const auto command = ui::parseCommand(std::string(ui::kMaximumCommandBytes + 1, 'a'));
  CXET_CHECK(!command.error.empty());
  CXET_CHECK(command.action == ui::Action::None);
}
void exactRangesUseProducerExpansion() {
  research::ExperimentSpec spec; spec.kind = research::ModuleKind::DataStudy;
  std::string error;
  CXET_CHECK(ui::addParameterRange(spec, "threshold", "0.1", "0.3", "0.1", error));
  CXET_CHECK(spec.rawGridText == "threshold=0.1..0.3..0.1\n");
  CXET_CHECK(spec.variants.size() == 3);
  CXET_CHECK(spec.variants[0].parameters[0].value == "0.1");
  CXET_CHECK(spec.variants[2].parameters[0].value == "0.3");
}
void refusedRangesPreserveDraft() {
  research::ExperimentSpec spec; spec.kind = research::ModuleKind::DataStudy; spec.budget.maxVariants = 2;
  std::string error;
  CXET_CHECK(!ui::addParameterRange(spec, "interval_ns", "1", "10", "1", error));
  CXET_CHECK(spec.rawGridText.empty());
  CXET_CHECK(spec.variants.empty());
  error.clear();
  CXET_CHECK(!ui::addParameterRange(spec, "interval_ns", "1", "2", "0", error));
  CXET_CHECK(spec.rawGridText.empty());
  spec.budget.maxVariants = std::numeric_limits<std::uint32_t>::max(); error.clear();
  CXET_CHECK(!ui::addParameterRange(spec, "interval_ns", "1", "2", "1", error));
  CXET_CHECK(spec.rawGridText.empty() && spec.variants.empty());
}
research::Response comparableJobs() {
  research::Response response; response.ok = true;
  research::ExperimentStatus first, second;
  first.id = "first"; first.sourceIdentity = "13:100"; first.beginReceiveNs = 100; first.visibleInputCut = 500;
  second = first; second.id = "second"; second.visibleInputCut = 300;
  response.experiments.push_back(first); response.experiments.push_back(second);
  return response;
}
void comparisonUsesCommonCommittedInputCut() {
  auto response = comparableJobs(); std::uint64_t cut{}; std::string error;
  CXET_CHECK(ui::commonInputCut(response, "first", "second", cut, error));
  CXET_CHECK(cut == 300);
  response.experiments[0].inputCut = 999;
  CXET_CHECK(ui::commonInputCut(response, "first", "second", cut, error));
  CXET_CHECK(cut == 300);
}
void comparisonRefusesDifferentCorpusOrStart() {
  auto response = comparableJobs(); std::uint64_t cut{}; std::string error;
  response.experiments[1].sourceIdentity = "other:100";
  CXET_CHECK(!ui::commonInputCut(response, "first", "second", cut, error));
  response.experiments[1].sourceIdentity = response.experiments[0].sourceIdentity;
  response.experiments[1].beginReceiveNs = 101;
  CXET_CHECK(!ui::commonInputCut(response, "first", "second", cut, error));
  response.experiments[1].beginReceiveNs = 100; response.experiments[1].visibleInputCut = 0;
  CXET_CHECK(!ui::commonInputCut(response, "first", "second", cut, error));
}
void resultQueryCarriesAllFences() {
  ui::Model model; model.resultKind = research::ResultKind::Fill;
  model.variant = 77; model.beginNs = 100; model.endNs = 200; model.resultOffset = 19;
  const auto request = ui::resultRequest(model, "first", 300);
  CXET_CHECK(request.command == research::Command::Range);
  CXET_CHECK(request.experimentId == "first");
  CXET_CHECK(request.resultKind == research::ResultKind::Fill);
  CXET_CHECK(request.variant == 77 && request.beginNs == 100 && request.endNs == 200);
  CXET_CHECK(request.offset == 19 && request.limit == ui::kPageRows);
  CXET_CHECK(request.maximumInputCut == 300);
  std::string error;
  CXET_CHECK(!ui::editDraft(model, {"set", "result-end", "99"}, error));
  CXET_CHECK(model.endNs == 200);
  error.clear();
  CXET_CHECK(!ui::editDraft(model, {"set", "catalog-begin", "-1"}, error));
  CXET_CHECK(model.catalogBeginNs == 0);
  error.clear();
  CXET_CHECK(ui::editDraft(model, {"set", "valuation-quote-asset", "7"}, error));
  CXET_CHECK(model.draft.valuationQuoteAssetId == 7);
  CXET_CHECK(ui::editDraft(model, {"set", "strategy-budget-enabled", "true"}, error));
  CXET_CHECK(ui::editDraft(model, {"set", "strategy-budget-e8", "200000000"}, error));
  CXET_CHECK(ui::editDraft(model, {"set", "budget-currency", "USDT"}, error));
  CXET_CHECK(ui::editDraft(model, {"set", "margin-borrow", "false"}, error));
  CXET_CHECK(model.draft.hasStrategyBudget && model.draft.strategyBudgetE8 == 200000000);
  CXET_CHECK(model.draft.budgetCurrency == "USDT" && !model.draft.marginBorrowAllowed);
  CXET_CHECK(!ui::editDraft(model, {"set", "strategy-budget-enabled", "automatic"}, error));
  CXET_CHECK(model.draft.hasStrategyBudget);
  error.clear();
  CXET_CHECK(!ui::editDraft(model, {"set", "strategy-budget-e8", "-1"}, error));
  CXET_CHECK(model.draft.strategyBudgetE8 == 200000000);
  error.clear();
  CXET_CHECK(ui::editDraft(model, {"set", "max-contexts", "16"}, error));
  CXET_CHECK(ui::editDraft(model, {"set", "state-bytes", "1048576"}, error));
  CXET_CHECK(model.draft.budget.maxContexts == 16 && model.draft.budget.maxStateBytes == 1048576);
  model.draft.buildIdentity = "old-build"; model.draft.moduleIdentity = "old-module";
  model.catalog.ok = false; error.clear();
  CXET_CHECK(!ui::applyServiceIdentity(model, error));
  CXET_CHECK(model.draft.buildIdentity == "old-build" && model.draft.moduleIdentity == "old-module");
  model.catalog.ok = true; model.catalog.buildIdentity = "service-build"; model.catalog.moduleIdentity = "service-module";
  error.clear();
  CXET_CHECK(ui::applyServiceIdentity(model, error));
  CXET_CHECK(model.draft.buildIdentity == "service-build" && model.draft.moduleIdentity == "service-module");
  model.descriptionId = "saved"; model.descriptionOffset = 4;
  const auto describe = ui::describeRequest(model);
  CXET_CHECK(describe.command == research::Command::Describe && describe.experimentId == "saved");
  CXET_CHECK(describe.offset == 4 && describe.limit == 4);
}
void selectionInvalidatesOldResults() {
  ui::Model model; model.status = comparableJobs(); model.selectedId = "first";
  model.resultOffset = 128; model.comparisonCut = 300;
  model.results.ok = true; model.results.rows.push_back({});
  ui::selectJob(model, 1);
  CXET_CHECK(model.selectedId == "second");
  CXET_CHECK(model.resultOffset == 0 && model.comparisonCut == 0);
  CXET_CHECK(model.results.rows.empty() && !model.results.ok);
  std::string error;
  CXET_CHECK(ui::editDraft(model, {"set", "result-kind", "exposure"}, error));
  CXET_CHECK(model.resultKind == research::ResultKind::Exposure);
}
void producerTextCannotInjectTerminalControls() {
  const std::string text = "metric\033[2J\r\nvalue\x7f";
  const auto safe = ui::safeText(text);
  CXET_CHECK(safe.find('\033') == std::string::npos);
  CXET_CHECK(safe.find('\r') == std::string::npos);
  CXET_CHECK(safe.find('\n') == std::string::npos);
  CXET_CHECK(safe.find('\x7f') == std::string::npos);
  CXET_CHECK(ui::safeText("abcdef", 3) == "abc");
  CXET_CHECK(ui::safeText("\xc0\xaf").find('\xc0') == std::string::npos);
}
void resultsRenderOnlyLoadedProducerRows() {
  ui::Model model; model.screen = ui::Screen::Results; model.results.error = "producer unavailable";
  auto lines = ui::render(model, 100); std::string text;
  for (const auto& line : lines) text += line + '\n';
  CXET_CHECK(text.find("Unavailable: producer unavailable") != std::string::npos);
  CXET_CHECK(text.find("Page curve") == std::string::npos);
  model.results.ok = true; model.results.error.clear();
  research::ResultRow row; row.kind = research::ResultKind::Equity; row.variant = 42; row.metric = "producer_equity_e8";
  row.value = std::numeric_limits<std::int64_t>::min(); model.results.rows.push_back(row);
  row.value = std::numeric_limits<std::int64_t>::max(); model.results.rows.push_back(row);
  lines = ui::render(model, 100); text.clear();
  for (const auto& line : lines) text += line + '\n';
  CXET_CHECK(text.find("Page curve producer_equity_e8 variant=42") != std::string::npos);
  CXET_CHECK(text.find(std::to_string(std::numeric_limits<std::int64_t>::min())) != std::string::npos);
  CXET_CHECK(text.find(std::to_string(std::numeric_limits<std::int64_t>::max())) != std::string::npos);
  row.currencyAssetId = 9; row.accountId = 99; row.value = 1; model.results.rows.push_back(row);
  row.value = 2; model.results.rows.push_back(row);
  lines = ui::render(model, 100); text.clear();
  for (const auto& line : lines) text += line + '\n';
  const auto firstCurve = text.find("Page curve producer_equity_e8 variant=42");
  const auto secondCurve = text.find("Page curve producer_equity_e8 variant=42", firstCurve + 1);
  CXET_CHECK(firstCurve != std::string::npos && secondCurve != std::string::npos);
  CXET_CHECK(text.find("account=99 currency-asset=9") != std::string::npos);
  model.screen = ui::Screen::Specification; model.descriptionId = "saved"; model.draft.id = "unchanged-draft";
  model.description.ok = true; model.description.hasSpecification = true;
  model.description.specification.id = "saved"; model.description.specification.kind = research::ModuleKind::DataStudy;
  model.description.specification.module = "midprice_return_distribution";
  model.description.specification.rawGridText = "interval_ns=1,2,3\n";
  model.description.specification.variants.resize(1);
  model.description.specification.variants[0].parameters.push_back({"interval_ns", "1"});
  model.description.variantDescriptions.push_back({901, 0, "interval_ns=1"});
  model.description.specificationVariantCount = 3; model.description.more = true;
  lines = ui::render(model, 100); text.clear();
  for (const auto& line : lines) text += line + '\n';
  CXET_CHECK(text.find("SAVED SPECIFICATION (READ ONLY) saved") != std::string::npos);
  CXET_CHECK(text.find("VARIANT 901 ordinal=0 label=interval_ns=1") != std::string::npos);
  CXET_CHECK(text.find("slice, not a complete Create spec") != std::string::npos);
  CXET_CHECK(model.draft.id == "unchanged-draft" && model.description.specification.variants.size() == 1);
  std::string error;
  CXET_CHECK(!ui::cloneDescription(model, "saved", error));
  CXET_CHECK(model.draft.id == "unchanged-draft");
  error.clear();
  CXET_CHECK(ui::cloneDescription(model, "new-draft", error));
  CXET_CHECK(model.draft.id == "new-draft" && model.draft.variants.size() == 3);
  CXET_CHECK(model.description.specification.id == "saved" && model.description.specification.variants.size() == 1);
}
constexpr cxet::testing::Case cases[]{
    {"research_tui_quoted_paths_preserve_argument", quotedPathsRemainOneArgument},
    {"research_tui_malformed_quotes_refuse", malformedQuotesAreRejected},
    {"research_tui_detach_differs_from_producer_stop", detachAndProducerStopAreDistinct},
    {"research_tui_oversized_command_refuses", oversizedCommandsDoNotDispatch},
    {"research_tui_exact_ranges_use_producer_expansion", exactRangesUseProducerExpansion},
    {"research_tui_refused_ranges_preserve_draft", refusedRangesPreserveDraft},
    {"research_tui_compare_common_committed_cut", comparisonUsesCommonCommittedInputCut},
    {"research_tui_compare_refuses_mismatched_provenance", comparisonRefusesDifferentCorpusOrStart},
    {"research_tui_result_query_retains_all_fences", resultQueryCarriesAllFences},
    {"research_tui_selection_invalidates_old_results", selectionInvalidatesOldResults},
    {"research_tui_terminal_controls_are_sanitized", producerTextCannotInjectTerminalControls},
    {"research_tui_results_use_only_real_rows", resultsRenderOnlyLoadedProducerRows},
};
}
int main(int argc, char** argv) { return cxet::testing::runCases(argc, argv, cases); }
