#include "ResearchTui.hpp"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <cwchar>
#include <utility>

namespace hft_backtest::tui {
namespace {
template<class T> bool number(const std::string& value, T& output) {
  T parsed{}; const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size()) return false;
  output = parsed; return true;
}
bool boolean(const std::string& value, bool& output) {
  if (value == "true") { output = true; return true; }
  if (value == "false") { output = false; return true; }
  return false;
}
bool readConfig(const std::string& path, std::string& output, std::string& error) {
  std::ifstream input(path, std::ios::binary);
  if (!input) { error = "Cannot open config file: " + path; return false; }
  std::string contents(kMaximumConfigBytes + 1, '\0');
  input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
  const auto count = input.gcount();
  if (count > static_cast<std::streamsize>(kMaximumConfigBytes) || input.bad()) { error = "Config file exceeds 1 MiB or read failed"; return false; }
  contents.resize(static_cast<std::size_t>(count)); output = std::move(contents); return true;
}
bool parameterName(const std::string& name) {
  if (name.empty() || name.size() > 128) return false;
  for (unsigned char ch : name) if (!std::isalnum(ch) && ch != '_' && ch != '.') return false;
  return true;
}
}
std::string safeText(const std::string& text, std::size_t maximum) {
  std::string output; output.reserve(std::min(text.size(), maximum));
  std::size_t columns{};
  for (std::size_t index = 0; index < text.size() && columns < maximum && output.size() / 4 < maximum;) {
    const auto c = static_cast<unsigned char>(text[index]);
    std::uint32_t code = c; std::size_t bytes = 1;
    if (c >= 0xc2 && c <= 0xdf) { code = c & 31U; bytes = 2; }
    else if (c >= 0xe0 && c <= 0xef) { code = c & 15U; bytes = 3; }
    else if (c >= 0xf0 && c <= 0xf4) { code = c & 7U; bytes = 4; }
    bool valid = c < 128 || bytes > 1;
    if (index + bytes > text.size()) valid = false;
    if (valid && bytes > 1) for (std::size_t offset = 1; offset < bytes; ++offset) {
      const auto next = static_cast<unsigned char>(text[index + offset]);
      if ((next & 0xc0U) != 0x80U) { valid = false; break; }
      code = (code << 6U) | (next & 63U);
    }
    if (valid && ((bytes == 2 && code < 0x80) || (bytes == 3 && code < 0x800) || (bytes == 4 && code < 0x10000) || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff))) valid = false;
    if (!valid || code < 32 || (code >= 0x7f && code <= 0x9f) || (code >= 0x202a && code <= 0x202e) || (code >= 0x2066 && code <= 0x2069)) {
      output += '?'; ++columns; index += valid ? bytes : 1; continue;
    }
    const int measured = ::wcwidth(static_cast<wchar_t>(code));
    const auto width = static_cast<std::size_t>(measured < 0 ? 1 : measured);
    if (columns + width > maximum) break;
    output.append(text, index, bytes); index += bytes; columns += width;
  }
  return output;
}
ParsedCommand parseCommand(const std::string& line) {
  ParsedCommand result;
  if (line.size() > kMaximumCommandBytes) { result.error = "Command exceeds 4096 bytes"; return result; }
  std::istringstream input(line);
  std::string word;
  for (;;) {
    input >> std::ws;
    if (input.eof()) break;
    if (!(input >> std::quoted(word))) { result.error = "Unclosed quote or invalid command"; return result; }
    result.words.push_back(word);
    if (result.words.size() > 32) { result.error = "Too many command arguments"; return result; }
  }
  if (result.words.empty()) return result;
  const auto& name = result.words.front();
  if (name == "refresh") result.action = Action::Refresh;
  else if (name == "create") result.action = Action::Create;
  else if (name == "start") result.action = Action::Start;
  else if (name == "pause") result.action = Action::Pause;
  else if (name == "resume") result.action = Action::Resume;
  else if (name == "stop") result.action = Action::Stop;
  else if (name == "record-configure") result.action = Action::RecordingConfigure;
  else if (name == "record-start") result.action = Action::RecordingStart;
  else if (name == "record-stop") result.action = Action::RecordingStop;
  else if (name == "results") result.action = Action::Results;
  else if (name == "next") result.action = Action::Next;
  else if (name == "compare") result.action = Action::Compare;
  else if (name == "describe") result.action = Action::Describe;
  else if (name == "load") result.action = Action::Load;
  else if (name == "save") result.action = Action::Save;
  else if (name == "record-load") result.action = Action::RecordingLoad;
  else if (name == "quit") result.action = Action::Quit;
  else if (name != "set" && name != "record-set" && name != "range" && name != "values" && name != "venue" && name != "sources-next" && name != "source-current" && name != "sessions-next" && name != "session" && name != "service-identity" && name != "describe-next" && name != "clone" && name != "fee" && name != "bounds") result.error = "Unknown command; press ? for help";
  return result;
}
bool addParameterRange(research::ExperimentSpec& spec, const std::string& name,
                       const std::string& begin, const std::string& end,
                       const std::string& step, std::string& error) {
  if (!parameterName(name) || begin.size() > 64 || end.size() > 64 || step.size() > 64 ||
      begin.find_first_of("=,\r\n") != std::string::npos || end.find_first_of("=,\r\n") != std::string::npos || step.find_first_of("=,\r\n") != std::string::npos) { error = "Range needs a valid name and bounded numeric cells"; return false; }
  if (spec.rawGridText.size() > kMaximumConfigBytes - 4096) { error = "Grid config is full"; return false; }
  // Refuse duplicate definitions rather than quietly replacing earlier choices.
  std::istringstream existing(spec.rawGridText); std::string line;
  while (std::getline(existing, line)) if (line.rfind(name + "=", 0) == 0) { error = "Parameter already has a range; reload grid to replace it"; return false; }
  auto candidate = spec;
  if (!candidate.rawGridText.empty() && candidate.rawGridText.back() != '\n') candidate.rawGridText += '\n';
  candidate.rawGridText += name + "=" + begin + ".." + end + ".." + step + '\n';
  // Exact decimal parsing, inclusive ranges, expansion and quota semantics have
  // one producer owner. The UI only assembles that owner's textual request.
  if (!research::expandExperimentGrid(candidate, error)) return false;
  spec = std::move(candidate); return true;
}
bool editDraft(Model& model, const std::vector<std::string>& words, std::string& error) {
  if (words.size() != 3) { error = "Use: set FIELD VALUE (quote paths containing spaces)"; return false; }
  const auto& key = words[1]; const auto& value = words[2]; auto& spec = model.draft;
  bool valid = true;
  if (key == "id") spec.id = value;
  else if (key == "module") spec.module = value;
  else if (key == "kind") { if (value == "strategy") spec.kind = research::ModuleKind::Strategy; else if (value == "study") spec.kind = research::ModuleKind::DataStudy; else valid = false; }
  else if (key == "source") spec.sourcePath = value;
  else if (key == "recorder-runtime") spec.recorderRuntimeDirectory = value;
  else if (key == "source-identity") spec.sourceIdentity = value;
  else if (key == "profile") spec.profile = value;
  else if (key == "build-identity") spec.buildIdentity = value;
  else if (key == "module-identity") spec.moduleIdentity = value;
  else if (key == "valuation") spec.valuationPolicy = value;
  else if (key == "valuation-quote-asset") valid = number(value, spec.valuationQuoteAssetId);
  else if (key == "strategy-budget-enabled") valid = boolean(value, spec.hasStrategyBudget);
  else if (key == "strategy-budget-e8") { std::int64_t amount{}; valid = number(value, amount) && amount >= 0; if (valid) spec.strategyBudgetE8 = amount; }
  else if (key == "budget-currency") spec.budgetCurrency = value;
  else if (key == "margin-borrow") valid = boolean(value, spec.marginBorrowAllowed);
  else if (key == "config") { valid = readConfig(value, spec.rawConfigText, error); if (valid) spec.variants.clear(); }
  else if (key == "grid") { valid = readConfig(value, spec.rawGridText, error); if (valid) spec.variants.clear(); }
  else if (key == "begin") { std::int64_t time{}; valid = number(value, time) && time >= 0; if (valid) spec.beginReceiveNs = time; }
  else if (key == "seed") valid = number(value, spec.seed);
  else if (key == "balance-e8") valid = number(value, spec.initialBalanceE8);
  else if (key == "study-interval-ns") valid = number(value, spec.studyIntervalNs);
  else if (key == "max-variants") valid = number(value, spec.budget.maxVariants);
  else if (key == "max-sources") valid = number(value, spec.budget.maxSources);
  else if (key == "max-contexts") valid = number(value, spec.budget.maxContexts);
  else if (key == "state-bytes") valid = number(value, spec.budget.maxStateBytes);
  else if (key == "input-batch") valid = number(value, spec.budget.inputBatchEvents);
  else if (key == "result-batch") valid = number(value, spec.budget.resultBatchRows);
  else if (key == "input-bytes") valid = number(value, spec.budget.maxInputBytes);
  else if (key == "output-bytes") valid = number(value, spec.budget.maxOutputBytes);
  else if (key == "result-rows") valid = number(value, spec.budget.maxResultRows);
  else if (key == "variant") valid = number(value, model.variant);
  else if (key == "result-begin") { std::int64_t time{}; valid = number(value, time) && time >= 0 && (model.endNs == 0 || time < model.endNs); if (valid) model.beginNs = time; }
  else if (key == "result-end") { std::int64_t time{}; valid = number(value, time) && time >= 0 && (time == 0 || time > model.beginNs); if (valid) model.endNs = time; }
  else if (key == "catalog-begin") { std::int64_t time{}; valid = number(value, time) && time >= 0 && (model.catalogEndNs == 0 || time < model.catalogEndNs); if (valid) model.catalogBeginNs = time; }
  else if (key == "catalog-end") { std::int64_t time{}; valid = number(value, time) && time >= 0 && (time == 0 || time > model.catalogBeginNs); if (valid) model.catalogEndNs = time; }
  else if (key == "result-kind") {
    if (value == "equity") model.resultKind = research::ResultKind::Equity;
    else if (value == "fills") model.resultKind = research::ResultKind::Fill;
    else if (value == "distribution") model.resultKind = research::ResultKind::Distribution;
    else if (value == "series") model.resultKind = research::ResultKind::Series;
    else if (value == "exposure") model.resultKind = research::ResultKind::Exposure;
    else if (value == "scalar") model.resultKind = research::ResultKind::Scalar;
    else valid = false;
  } else { error = "Unknown experiment field: " + key; return false; }
  if (!valid && error.empty()) error = "Invalid value for " + key;
  if (valid && (key == "variant" || key == "result-begin" || key == "result-end" || key == "result-kind")) invalidateResults(model);
  return valid;
}
bool editRecording(Model& model, const std::vector<std::string>& words, std::string& error) {
  if (words.size() != 3) { error = "Use: record-set FIELD VALUE"; return false; }
  auto& config = model.recording; const auto& key = words[1]; const auto& value = words[2]; bool valid = true;
  if (key == "workspace") config.workspaceRoot = value;
  else if (key == "parser-template") config.parserTemplate = value;
  else if (key == "env") config.envPath = value;
  else if (key == "output") config.outputRoot = value;
  else if (key == "channels") valid = number(value, config.channelMask);
  else if (key == "duration") valid = number(value, config.durationSec);
  else if (key == "maximum-bytes") valid = number(value, config.maximumBytes);
  else if (key == "segment-bytes") valid = number(value, config.segmentBytes);
  else if (key == "retained-bytes") valid = number(value, config.retainedCompressedBytes);
  else if (key == "reference-bytes") valid = number(value, config.maximumReferenceBytes);
  else if (key == "queue-records") valid = number(value, config.writerQueueRecords);
  else { error = "Unknown recording field: " + key; return false; }
  if (!valid) error = "Invalid value for " + key;
  return valid;
}
bool applyServiceIdentity(Model& model, std::string& error) {
  if (!model.catalog.ok || model.catalog.buildIdentity.empty() || model.catalog.moduleIdentity.empty()) {
    error = "Service identities unavailable; refresh Catalog before selecting provenance"; return false;
  }
  model.draft.buildIdentity = model.catalog.buildIdentity;
  model.draft.moduleIdentity = model.catalog.moduleIdentity;
  return true;
}
research::Request describeRequest(const Model& model) {
  research::Request request;
  request.command = research::Command::Describe;
  request.experimentId = model.descriptionId;
  request.offset = model.descriptionOffset;
  // Configurations may be large. Keep the owner response page small; the
  // producer may return fewer entries according to its independent byte quota.
  request.limit = 4;
  return request;
}
bool cloneDescription(Model& model, const std::string& newId, std::string& error) {
  const auto& description = model.description;
  if (!description.ok || !description.hasSpecification || newId.empty() || newId == description.specification.id) {
    error = "Describe an existing experiment, then use clone with a distinct new ID"; return false;
  }
  auto candidate = description.specification;
  candidate.id = newId;
  // Describe contains only a page of existing variants. Reconstruct the full
  // grid from common immutable inputs through the producer, never the slice.
  candidate.variants.clear();
  if (!research::expandExperimentGrid(candidate, error)) return false;
  if (candidate.variants.size() != description.specificationVariantCount) {
    error = "Describe common inputs do not reconstruct the complete saved grid"; return false;
  }
  model.draft = std::move(candidate); model.specPath.clear();
  return true;
}
void selectJob(Model& model, int direction) {
  if (model.status.experiments.empty()) return;
  if (direction < 0 && model.selectedJob > 0) --model.selectedJob;
  if (direction > 0 && model.selectedJob + 1 < model.status.experiments.size()) ++model.selectedJob;
  model.selectedJob = std::min(model.selectedJob, model.status.experiments.size() - 1);
  model.selectedId = model.status.experiments[model.selectedJob].id;
  invalidateResults(model);
}
void invalidateResults(Model& model) {
  model.resultOffset = 0; model.comparisonOffset = 0; model.comparisonCut = 0;
  model.results = {}; model.comparison = {};
  model.results.error = "Selection changed; request results or compare again";
  model.comparison.error = model.results.error;
}
research::Request resultRequest(const Model& model, const std::string& id, std::uint64_t maximumCut) {
  research::Request request;
  request.command = research::Command::Range; request.experimentId = id;
  request.resultKind = model.resultKind; request.variant = model.variant;
  request.beginNs = model.beginNs; request.endNs = model.endNs;
  request.offset = model.resultOffset; request.limit = kPageRows;
  request.maximumInputCut = maximumCut;
  return request;
}
bool commonInputCut(const research::Response& response, const std::string& first, const std::string& second,
                    std::uint64_t& cut, std::string& error) {
  const research::ExperimentStatus* a{}; const research::ExperimentStatus* b{};
  for (const auto& status : response.experiments) { if (status.id == first) a = &status; if (status.id == second) b = &status; }
  if (!response.ok || !a || !b) { error = "Refresh job status: both experiments must be available"; return false; }
  if (first == second) { error = "Select two different experiments"; return false; }
  if (a->sourceIdentity.empty() || a->sourceIdentity != b->sourceIdentity || a->beginReceiveNs != b->beginReceiveNs) {
    error = "Comparison requires the same exact source identity and replay begin time"; return false;
  }
  cut = std::min(a->visibleInputCut, b->visibleInputCut);
  if (cut == 0) { error = "No common committed inputcut is available"; return false; }
  return true;
}
} // namespace hft_backtest::tui
