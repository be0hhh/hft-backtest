#include "ResearchTui.hpp"
#include "Terminal.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <clocale>
#include <sstream>
#include <utility>

namespace hft_backtest::tui {
namespace {
template<class T> bool number(const std::string& text, T& result) {
  T value{}; auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) return false;
  result = value; return true;
}
std::vector<std::string> splitComma(const std::string& text) {
  std::vector<std::string> values; std::istringstream input(text); std::string value;
  while (std::getline(input, value, ',')) { if (value.empty() || values.size() == 1024) return {}; values.push_back(value); }
  if (text.empty() || text.back() == ',') return {};
  return values;
}
void usage() {
  std::fputs("Usage: hft-backtest-tui --research-socket PATH --recorder-runtime DIR [--spec PATH] [--recorder-config PATH]\n"
      "Connects to existing services. No daemon is launched or stopped by terminal lifetime.\n", stdout);
}
bool options(int argc, char** argv, Model& model, std::string& error) {
  for (int index = 1; index < argc; ++index) {
    const std::string name = argv[index];
    if (name == "--help") { usage(); model.exitRequested = true; return true; }
    if (index + 1 == argc) { error = "Missing value for " + name; return false; }
    const std::string value = argv[++index];
    if (name == "--research-socket") model.researchSocket = value;
    else if (name == "--recorder-runtime") model.recorderRuntime = value;
    else if (name == "--spec") model.specPath = value;
    else if (name == "--recorder-config") model.recordingConfigPath = value;
    else { error = "Unknown option: " + name; return false; }
  }
  if (model.researchSocket.empty() || model.recorderRuntime.empty()) { error = "Select both --research-socket PATH and --recorder-runtime DIR explicitly"; return false; }
  if (!model.specPath.empty() && !research::loadExperimentSpec(model.specPath, model.draft, error)) return false;
  if (!model.recordingConfigPath.empty() && !hftrec::isOk(hftrec::service::loadRecorderServiceConfig(model.recordingConfigPath, model.recording, error))) return false;
  return true;
}
class Controller {
 public:
  explicit Controller(Model& model) : model_(model), research_(model.researchSocket) {}
  void refresh() {
    research::Request request; request.command = research::Command::Status;
    model_.status = research_.execute(request);
    model_.researchError = model_.status.ok ? "" : model_.status.error;
    if (model_.status.ok && !model_.status.experiments.empty()) {
      const auto selected = std::find_if(model_.status.experiments.begin(), model_.status.experiments.end(), [&](const auto& job) { return job.id == model_.selectedId; });
      if (selected == model_.status.experiments.end()) { model_.selectedJob = 0; model_.selectedId = model_.status.experiments.front().id; invalidateResults(model_); }
      else model_.selectedJob = static_cast<std::size_t>(selected - model_.status.experiments.begin());
    }
    if (!connected_) {
      model_.recorderError.clear();
      connected_ = hftrec::isOk(recorder_.connect(model_.recorderRuntime, model_.recorderError));
    }
    if (connected_ && !hftrec::isOk(recorder_.status(model_.recorderStatus, model_.recorderError))) { recorder_.disconnect(); connected_ = false; }
    if (connected_) {
      model_.recorderError.clear();
      model_.sourceError.clear();
      const auto status = model_.catalogSourcePath.empty()
          ? recorder_.catalog(model_.sourceOffset, kPageRows, model_.sources, model_.sourceError)
          : recorder_.sessionSources(model_.catalogSourcePath, model_.sourceOffset, kPageRows, model_.sources, model_.sourceError);
      if (!hftrec::isOk(status) && model_.sourceError.empty()) model_.sourceError = "Recorder source catalog unavailable";
    } else model_.sourceError = model_.recorderError;
    if (model_.screen == Screen::Results && !model_.selectedId.empty()) fetchResults();
    // Comparison intentionally retains one explicit common cut until refreshed.
  }
  void archives() {
    model_.sessionsError.clear();
    if (!connected_) { model_.sessionsError = model_.recorderError; return; }
    if (!hftrec::isOk(recorder_.sessions(model_.sessionOffset, 64, model_.sessions, model_.sessionsError)) && model_.sessionsError.empty()) model_.sessionsError = "Recorder archived-session catalog unavailable";
  }
  void catalog() {
    research::Request request; request.command = research::Command::Catalog; model_.catalog = research_.execute(request);
    if (model_.specPath.empty() && model_.draft.buildIdentity.empty() && model_.draft.moduleIdentity.empty()) {
      std::string error; applyServiceIdentity(model_, error);
    }
  }
  void command(const std::string& line) {
    auto parsed = parseCommand(line); std::string error;
    if (!parsed.error.empty()) { model_.message = parsed.error; return; }
    if (parsed.words.empty()) return;
    const auto& words = parsed.words;
    if (words[0] == "set") { if (editDraft(model_, words, error)) model_.message = "Draft updated; result filters apply on refresh/results"; else model_.message = error; return; }
    if (words[0] == "record-set") { model_.message = editRecording(model_, words, error) ? "Recording draft updated; send :record-configure explicitly" : error; return; }
    if (words[0] == "service-identity") {
      if (words.size() != 1) { model_.message = "Use: service-identity"; return; }
      catalog(); model_.message = applyServiceIdentity(model_, error) ? "Draft provenance explicitly selected from the connected research service" : error; return;
    }
    if (parsed.action == Action::Describe) {
      if (words.size() > 2) { model_.message = "Use: describe [EXPERIMENT_ID]"; return; }
      model_.descriptionId = words.size() == 2 ? words[1] : model_.selectedId;
      model_.descriptionOffset = 0; describe(); model_.screen = Screen::Specification; model_.scroll = 0; return;
    }
    if (words[0] == "describe-next") {
      if (words.size() != 1 || !model_.description.ok || !model_.description.hasSpecification || !model_.description.more) { model_.message = "No next saved-specification page; use describe [ID]"; return; }
      model_.descriptionOffset = model_.description.nextOffset;
      describe(); model_.screen = Screen::Specification; model_.scroll = 0; return;
    }
    if (words[0] == "clone") {
      if (words.size() != 2) { model_.message = "Use: clone DISTINCT_NEW_ID (after describe)"; return; }
      if (cloneDescription(model_, words[1], error)) {
        model_.screen = Screen::Experiment; model_.scroll = 0;
        model_.message = "Complete producer grid cloned into a new local draft; existing experiment is unchanged; create explicitly";
      } else model_.message = error;
      return;
    }
    if (words[0] == "range") {
      if (words.size() != 5) error = "Use: range NAME BEGIN END STEP (exact signed decimal; config template must be loaded first)";
      else addParameterRange(model_.draft, words[1], words[2], words[3], words[4], error);
      model_.message = error.empty() ? "Exact range expanded by producer; complete Cartesian grid retained" : error; return;
    }
    if (words[0] == "values") {
      if (words.size() != 3 || words[1].empty() || words[1].find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.") != std::string::npos || splitComma(words[2]).empty()) error = "Use: values NAME LITERAL,LITERAL (JSON literals matching producer parameter type)";
      else if (model_.draft.rawGridText.size() + line.size() > kMaximumConfigBytes) error = "Grid exceeds 1 MiB";
      else {
        std::istringstream grid(model_.draft.rawGridText); std::string definition;
        while (std::getline(grid, definition)) if (definition.rfind(words[1] + "=", 0) == 0) error = "Parameter already defined; reload grid to replace it";
        if (error.empty()) { model_.draft.rawGridText += "\n" + words[1] + "=" + words[2] + "\n"; model_.draft.variants.clear(); }
      }
      model_.message = error.empty() ? "Literal values added; producer validates types on create" : error; return;
    }
    if (words[0] == "venue") {
      if (words.size() == 2 && words[1] == "clear") { model_.recording.venues.clear(); model_.message = "Recording venue draft cleared"; return; }
      if (words.size() != 4 || model_.recording.venues.size() >= 8) error = "Use: venue EXCHANGE MARKET SYMBOL[,SYMBOL] or * (max 8); :venue clear resets";
      else {
        hftrec::service::RecorderVenue venue; venue.exchange = words[1]; venue.market = words[2]; venue.fullUniverse = words[3] == "*";
        if (!venue.fullUniverse) venue.instruments = splitComma(words[3]);
        if (!venue.fullUniverse && venue.instruments.empty()) error = "Explicit nonempty instrument list required";
        else model_.recording.venues.push_back(std::move(venue));
      }
      model_.message = error.empty() ? "Recording venue added to local draft" : error; return;
    }
    if (words[0] == "fee") {
      research::ScenarioFee fee;
      if (words.size() != 5 || !number(words[3], fee.makerFeeBpsE8) || !number(words[4], fee.takerFeeBpsE8) || model_.draft.fees.size() >= 64) error = "Use: fee EXCHANGE MARKET MAKER_BPS_E8 TAKER_BPS_E8 (max 64)";
      else {
        fee.exchange = words[1]; fee.market = words[2];
        auto existing = std::find_if(model_.draft.fees.begin(), model_.draft.fees.end(), [&](const auto& old) { return old.exchange == fee.exchange && old.market == fee.market; });
        if (existing != model_.draft.fees.end()) *existing = std::move(fee); else model_.draft.fees.push_back(std::move(fee));
      }
      model_.message = error.empty() ? "Explicit scenario fee draft updated" : error; return;
    }
    if (words[0] == "bounds") {
      if (words.size() != 2) { model_.message = "Use: bounds SIGNED_E8,SIGNED_E8,... (in increasing order; max 256)"; return; }
      const auto values = splitComma(words[1]); std::vector<std::int64_t> bounds;
      if (values.empty() || values.size() > 256) error = "Bounds need 1..256 explicit signed integers";
      for (const auto& text : values) { std::int64_t value{}; if (!number(text, value)) { error = "Invalid signed E8 distribution boundary"; break; } bounds.push_back(value); }
      if (error.empty() && (!std::is_sorted(bounds.begin(), bounds.end()) || std::adjacent_find(bounds.begin(), bounds.end()) != bounds.end())) error = "Distribution boundaries must increase strictly";
      if (error.empty()) model_.draft.distributionBoundsE8 = std::move(bounds);
      model_.message = error.empty() ? "Distribution boundaries updated" : error; return;
    }
    if (words[0] == "sources-next") {
      if (words.size() != 1) { model_.message = "Use: sources-next"; return; }
      if (model_.sources.nextOffset == 0 || model_.sources.nextOffset <= model_.sourceOffset) { model_.message = "No further source page"; return; }
      model_.sourceOffset = model_.sources.nextOffset; refresh(); model_.screen = Screen::Sources; model_.scroll = 0; return;
    }
    if (words[0] == "sessions-next") {
      if (words.size() != 1) { model_.message = "Use: sessions-next"; return; }
      if (model_.sessions.nextOffset == 0 || model_.sessions.nextOffset <= model_.sessionOffset) { model_.message = "No further archived-session page"; return; }
      model_.sessionOffset = model_.sessions.nextOffset; archives(); model_.screen = Screen::Sources; model_.scroll = 0; return;
    }
    if (words[0] == "session") {
      std::size_t index{};
      if (words.size() != 2 || !number(words[1], index) || index == 0 || index > model_.sessions.sessions.size() || !model_.sessionsError.empty()) { model_.message = "Use: session N (one-based row number on displayed archive page)"; return; }
      const auto& session = model_.sessions.sessions[index - 1];
      if (session.sourceIdentity.empty()) { model_.message = "Session has no authoritative identity"; return; }
      model_.draft.sourcePath = session.sourcePath; model_.draft.sourceIdentity = session.sourceIdentity;
      model_.draft.beginReceiveNs = session.startedReceiveNs;
      model_.draft.recorderRuntimeDirectory = session.active ? model_.recorderRuntime : std::filesystem::path{};
      model_.catalogSourcePath = session.sourcePath; model_.sourceOffset = 0;
      refresh(); model_.screen = Screen::Sources; model_.scroll = 0;
      model_.message = "Draft pinned to archived session; source catalog loaded through Recorder client"; return;
    }
    if (words[0] == "source-current") {
      const auto& status = model_.recorderStatus;
      if (words.size() != 1 || !model_.recorderError.empty() || status.sessionPath.empty() || status.sourceIdentity.empty() || status.startedReceiveNs <= 0 || !status.cutGeneration) { model_.message = "No authoritative published Recorder session; refresh and choose an explicit corpus"; return; }
      model_.draft.sourcePath = status.sessionPath;
      model_.draft.sourceIdentity = status.sourceIdentity;
      model_.draft.beginReceiveNs = status.startedReceiveNs;
      model_.draft.recorderRuntimeDirectory = status.active ? model_.recorderRuntime : std::filesystem::path{};
      model_.catalogSourcePath.clear(); model_.sourceOffset = 0; refresh();
      model_.message = "Draft pinned to the displayed Recorder capture identity; later sessions will not replace it";
      return;
    }
    const auto action = parsed.action;
    const bool optionalId = action == Action::Start || action == Action::Pause || action == Action::Resume || action == Action::Stop || action == Action::Results;
    const bool requiresPath = action == Action::Load || action == Action::Save || action == Action::Compare || action == Action::RecordingLoad;
    if ((optionalId && words.size() > 2) || (requiresPath && words.size() != 2) || (!optionalId && !requiresPath && words.size() != 1)) { model_.message = "Incorrect argument count; press ? for command help"; return; }
    if (optionalId && words.size() == 2) { if (model_.selectedId != words[1]) invalidateResults(model_); model_.selectedId = words[1]; model_.resultOffset = 0; }
    if (action == Action::Quit) { model_.exitRequested = true; return; }
    if (action == Action::Refresh) { model_.sourceOffset = 0; model_.sessionOffset = 0; model_.resultOffset = 0; model_.comparisonOffset = 0; catalog(); refresh(); archives(); if (model_.screen == Screen::Compare) compare(); else if (model_.screen == Screen::Specification) { model_.descriptionOffset = 0; describe(); } else model_.message = "Producer snapshots refreshed"; return; }
    if (action == Action::Load) {
      research::ExperimentSpec spec;
      if (research::loadExperimentSpec(words[1], spec, error)) { model_.draft = std::move(spec); model_.specPath = words[1]; model_.screen = Screen::Experiment; model_.message = "Experiment spec loaded"; }
      else model_.message = error;
      return;
    }
    if (action == Action::RecordingLoad) {
      hftrec::service::RecorderServiceConfig config;
      if (hftrec::isOk(hftrec::service::loadRecorderServiceConfig(words[1], config, error))) { model_.recording = std::move(config); model_.recordingConfigPath = words[1]; model_.screen = Screen::Recording; model_.message = "Recorder config loaded into local draft; configure explicitly"; }
      else model_.message = error;
      return;
    }
    if (action == Action::Save) {
      if (research::saveExperimentSpec(words[1], model_.draft, error)) { model_.specPath = words[1]; model_.message = "Experiment spec saved"; } else model_.message = error;
      return;
    }
    if (action == Action::Create) {
      auto spec = model_.draft;
      if (!research::expandExperimentGrid(spec, error)) { model_.message = error; return; }
      research::Request request; request.command = research::Command::Create; request.spec = std::move(spec);
      auto response = research_.execute(request);
      if (response.ok) { model_.selectedId = model_.draft.id; model_.draft.variants = std::move(request.spec.variants); model_.screen = Screen::Jobs; refresh(); model_.message = "Created " + model_.selectedId + "; start explicitly with s"; }
      else model_.message = response.error;
      return;
    }
    if (action == Action::RecordingConfigure || action == Action::RecordingStart || action == Action::RecordingStop) {
      if (!connected_) { model_.message = "Recorder unavailable: " + model_.recorderError; return; }
      hftrec::Status status;
      if (action == Action::RecordingConfigure) status = recorder_.configure(model_.recording, error);
      else if (action == Action::RecordingStart) status = recorder_.start(error);
      else status = recorder_.requestStop(error);
      model_.message = hftrec::isOk(status) ? (action == Action::RecordingStop ? "Recorder stop requested; status must confirm idle" : "Recorder command accepted") : error;
      refresh(); if (action == Action::RecordingConfigure) archives(); return;
    }
    if (action == Action::Compare) { model_.compareId = words[1]; model_.resultOffset = 0; model_.comparisonOffset = 0; refresh(); compare(); model_.screen = Screen::Compare; model_.scroll = 0; return; }
    if (action == Action::Results) { model_.screen = Screen::Results; model_.resultOffset = 0; fetchResults(); model_.scroll = 0; return; }
    if (action == Action::Next) {
      if (model_.screen == Screen::Specification) { command("describe-next"); return; }
      const bool comparing = model_.screen == Screen::Compare;
      if (!model_.results.more && (!comparing || !model_.comparison.more)) { model_.message = "No further committed result page"; return; }
      model_.resultOffset = model_.results.nextOffset;
      model_.comparisonOffset = model_.comparison.nextOffset;
      fetchResults(comparing ? model_.comparisonCut : 0);
      if (comparing) { auto request = resultRequest(model_, model_.compareId, model_.comparisonCut); request.offset = model_.comparisonOffset; model_.comparison = research_.execute(request); }
      model_.scroll = 0; return;
    }
    research::Request request; request.experimentId = model_.selectedId;
    if (request.experimentId.empty()) { model_.message = "Select a research job first"; return; }
    if (action == Action::Start) request.command = research::Command::Start;
    else if (action == Action::Pause) request.command = research::Command::Pause;
    else if (action == Action::Resume) request.command = research::Command::Resume;
    else if (action == Action::Stop) request.command = research::Command::Stop;
    else return;
    const auto response = research_.execute(request);
    model_.message = response.ok ? "Research command accepted for " + model_.selectedId : response.error;
    refresh();
  }
 private:
  void describe() {
    if (model_.descriptionId.empty()) { model_.description = {}; model_.description.error = "Select an experiment or use describe ID"; model_.message = model_.description.error; return; }
    model_.description = research_.execute(describeRequest(model_));
    model_.message = model_.description.ok && model_.description.hasSpecification
        ? "Loaded immutable producer specification page; editable draft is unchanged"
        : "Describe unavailable: " + (model_.description.error.empty() ? "producer returned no specification" : model_.description.error);
  }
  void fetchResults(std::uint64_t cut = 0) {
    if (model_.selectedId.empty()) { model_.results = {}; model_.results.error = "Select a research job"; return; }
    model_.results = research_.execute(resultRequest(model_, model_.selectedId, cut));
  }
  void compare() {
    std::string error;
    if (!commonInputCut(model_.status, model_.selectedId, model_.compareId, model_.comparisonCut, error)) {
      model_.results = {}; model_.results.error = error; model_.comparison = {}; model_.comparison.error = error; model_.message = error; return;
    }
    fetchResults(model_.comparisonCut);
    auto request = resultRequest(model_, model_.compareId, model_.comparisonCut); request.offset = model_.comparisonOffset;
    model_.comparison = research_.execute(request);
    model_.message = "Comparison frozen at common committed inputcut=" + std::to_string(model_.comparisonCut);
  }
  Model& model_;
  research::ResearchClient research_;
  hftrec::service::RecorderServiceClient recorder_;
  bool connected_{};
};
void moveScreen(Model& model, int delta) {
  int value = static_cast<int>(model.screen) + delta;
  if (value < 0) value = 7; if (value > 7) value = 0;
  model.screen = static_cast<Screen>(value); model.scroll = 0;
}
void revealSelectedJob(Model& model, int columns) {
  const auto lines = render(model, columns);
  const std::string prefix = "> " + model.selectedId + " ";
  for (std::size_t index = 1; index < lines.size(); ++index) if (lines[index].rfind(prefix, 0) == 0) {
    model.scroll = index > 2 ? index - 2 : 0; return;
  }
}
}
int run(int argc, char** argv) {
  std::setlocale(LC_CTYPE, "");
  Model model; std::string error;
  if (!options(argc, argv, model, error)) { std::fprintf(stderr, "%s\n", safeText(error).c_str()); usage(); return 2; }
  if (model.exitRequested) return 0;
  Terminal terminal;
  if (!terminal.open(error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 2; }
  Controller controller(model); controller.catalog(); controller.refresh(); controller.archives();
  auto nextRefresh = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  bool editing{}; std::string command;
  std::vector<std::string> history; std::size_t historyIndex{};
  while (!model.exitRequested && interrupted == 0) {
    std::string prompt = editing ? ":" + command : "q detach | : command | ? help | r refresh | j/k scroll | Tab navigate";
    const auto promptWidth = static_cast<std::size_t>(terminal.viewport().columns - 1);
    if (editing && prompt.size() > promptWidth) {
      auto begin = prompt.size() - (promptWidth - 3);
      while (begin < prompt.size() && (static_cast<unsigned char>(prompt[begin]) & 0xc0U) == 0x80U) ++begin;
      prompt = "..." + prompt.substr(begin);
    }
    const auto viewport = terminal.viewport();
    const auto lines = render(model, viewport.columns);
    const auto remaining = lines.size() > 1 ? lines.size() - 1 : 0;
    const auto capacity = static_cast<std::size_t>(viewport.rows - 4);
    model.scroll = std::min(model.scroll, remaining > capacity ? remaining - capacity : 0);
    terminal.draw(lines, model.message, prompt, model.scroll);
    const auto key = terminal.read(100);
    if (key.kind == KeyKind::Closed) break;
    if (editing) {
      if (key.kind == KeyKind::Escape) { editing = false; command.clear(); }
      else if (key.kind == KeyKind::Backspace && !command.empty()) {
        auto end = command.size() - 1;
        while (end > 0 && (static_cast<unsigned char>(command[end]) & 0xc0U) == 0x80U) --end;
        command.resize(end);
      }
      else if (key.kind == KeyKind::Enter) {
        if (!command.empty() && (history.empty() || history.back() != command)) {
          if (history.size() == 32) history.erase(history.begin());
          history.push_back(command);
        }
        controller.command(command); command.clear(); editing = false;
      }
      else if (key.kind == KeyKind::Up && historyIndex > 0) command = history[--historyIndex];
      else if (key.kind == KeyKind::Down && historyIndex < history.size()) {
        ++historyIndex; command = historyIndex < history.size() ? history[historyIndex] : "";
      }
      else if (key.kind == KeyKind::Character && command.size() < kMaximumCommandBytes) command.push_back(key.character);
    } else {
      if (key.kind == KeyKind::Tab || key.kind == KeyKind::Right) moveScreen(model, 1);
      else if (key.kind == KeyKind::Left) moveScreen(model, -1);
      else if (key.kind == KeyKind::PageUp) model.scroll = model.scroll > capacity ? model.scroll - capacity : 0;
      else if (key.kind == KeyKind::PageDown) model.scroll += capacity;
      else if (key.kind == KeyKind::Home) model.scroll = 0;
      else if (key.kind == KeyKind::End) model.scroll = remaining;
      else if (key.kind == KeyKind::Up && model.screen == Screen::Jobs) { selectJob(model, -1); revealSelectedJob(model, terminal.viewport().columns); }
      else if (key.kind == KeyKind::Down && model.screen == Screen::Jobs) { selectJob(model, 1); revealSelectedJob(model, terminal.viewport().columns); }
      else if (key.kind == KeyKind::Up && model.scroll > 0) --model.scroll;
      else if (key.kind == KeyKind::Down && model.scroll < 65536) ++model.scroll;
      else if (key.kind == KeyKind::Enter && model.screen == Screen::Jobs) controller.command("results");
      else if (key.kind == KeyKind::Character) {
        const char ch = key.character;
        if (ch == ':') { editing = true; historyIndex = history.size(); }
        else if (ch >= '1' && ch <= '7') { model.screen = static_cast<Screen>(ch - '1'); model.scroll = 0; if (model.screen == Screen::Results) controller.command("results"); else if (model.screen == Screen::Specification) controller.command("describe"); }
        else if (ch == '?') { model.screen = Screen::Help; model.scroll = 0; }
        else if (ch == 'q') model.exitRequested = true;
        else if (ch == 'j' && model.scroll < 65536) ++model.scroll;
        else if (ch == 'k' && model.scroll > 0) --model.scroll;
        else if (ch == 'r') controller.command("refresh");
        else if (ch == 'c' && model.screen == Screen::Experiment) controller.command("create");
        else if (ch == 'n' && (model.screen == Screen::Results || model.screen == Screen::Compare)) controller.command("next");
        else if (ch == 'n' && model.screen == Screen::Specification) controller.command("describe-next");
        else if (model.screen == Screen::Jobs) {
          if (ch == 's') controller.command("start"); else if (ch == 'p') controller.command("pause");
          else if (ch == 'u') controller.command("resume"); else if (ch == 'x') controller.command("stop");
        }
      }
    }
    const auto now = std::chrono::steady_clock::now();
    if (!editing && now >= nextRefresh) { controller.refresh(); nextRefresh = now + std::chrono::seconds(2); }
  }
  return 0;
}
} // namespace hft_backtest::tui
