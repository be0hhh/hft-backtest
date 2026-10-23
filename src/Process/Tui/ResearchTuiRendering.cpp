#include "ResearchTui.hpp"
#include <algorithm>
#include <sstream>

namespace hft_backtest::tui {
namespace {
std::string stateName(research::ExperimentState state) {
  switch (state) {
    case research::ExperimentState::Created: return "created";
    case research::ExperimentState::Running: return "running";
    case research::ExperimentState::Paused: return "paused";
    case research::ExperimentState::Stopped: return "stopped";
    case research::ExperimentState::Incomplete: return "incomplete";
    case research::ExperimentState::Failed: return "failed";
    case research::ExperimentState::QuotaExceeded: return "quota-exceeded";
  }
  return "unknown";
}
std::string resultName(research::ResultKind kind) {
  switch (kind) {
    case research::ResultKind::Scalar: return "scalar";
    case research::ResultKind::Series: return "series";
    case research::ResultKind::Distribution: return "distribution";
    case research::ResultKind::Equity: return "equity";
    case research::ResultKind::Fill: return "fills";
    case research::ResultKind::Exposure: return "exposure";
  }
  return "unknown";
}
void appendCurves(std::vector<std::string>& lines, const research::Response& response, int columns) {
  // Display-only integer coordinate mapping. Never derive equity, PnL or
  // drawdown in the UI; each point is an actual producer result row.
  std::vector<const research::ResultRow*> groups;
  for (const auto& candidate : response.rows) {
    if (candidate.kind != research::ResultKind::Equity && candidate.kind != research::ResultKind::Series) continue;
    const auto sameSeries = [&](const research::ResultRow& row) {
      return row.kind == candidate.kind && row.variant == candidate.variant && row.source == candidate.source && row.metric == candidate.metric &&
          row.accountId == candidate.accountId && row.currencyAssetId == candidate.currencyAssetId && row.exchange == candidate.exchange && row.market == candidate.market && row.instrument == candidate.instrument;
    };
    if (std::any_of(groups.begin(), groups.end(), [&](const auto* first) { return sameSeries(*first); })) continue;
    if (groups.size() == 8) { lines.push_back("More curves exist on this page; select a variant for detail."); break; }
    groups.push_back(&candidate);
    std::vector<const research::ResultRow*> points;
    for (const auto& row : response.rows) if (sameSeries(row)) points.push_back(&row);
    if (points.size() < 2) continue;
    std::int64_t minimum = points.front()->value, maximum = minimum;
    for (const auto* point : points) { minimum = std::min(minimum, point->value); maximum = std::max(maximum, point->value); }
    const auto rank = [](std::int64_t value) { return static_cast<std::uint64_t>(value) ^ (std::uint64_t{1} << 63U); };
    const auto low = rank(minimum), difference = rank(maximum) - low;
    const auto width = static_cast<std::size_t>(std::clamp(columns - 4, 10, 80));
    std::vector<std::string> plot(6, std::string(width, ' '));
    for (std::size_t index = 0; index < points.size(); ++index) {
      const auto value = rank(points[index]->value);
      std::size_t y = difference == 0 ? 2 : 0;
      if (difference != 0) for (std::size_t level = 1; level <= 5; ++level) {
        const auto threshold = low + (difference / 5) * level + ((difference % 5) * level) / 5;
        if (value >= threshold) y = level;
      }
      const auto x = index * (width - 1) / (points.size() - 1);
      plot[5 - y][x] = '*';
    }
    lines.push_back("Page curve " + candidate.metric + " variant=" + std::to_string(candidate.variant) + " source=" + std::to_string(candidate.source));
    lines.push_back("account=" + std::to_string(candidate.accountId) + " currency-asset=" + std::to_string(candidate.currencyAssetId) + " " + candidate.exchange + "/" + candidate.market + "/" + candidate.instrument);
    lines.push_back("min=" + std::to_string(minimum) + " max=" + std::to_string(maximum) + " receive_ns=" + std::to_string(points.front()->timestampNs) + ".." + std::to_string(points.back()->timestampNs));
    for (const auto& row : plot) lines.push_back("|" + row + "|");
  }
}
void appendRows(std::vector<std::string>& lines, const research::Response& response, int columns) {
  if (!response.ok) { lines.push_back("Unavailable: " + response.error); return; }
  if (response.rows.empty()) { lines.push_back("No committed rows in this selection. No sample data is generated."); return; }
  appendCurves(lines, response, columns);
  lines.push_back("variant source inputcut receive_ns metric value auxiliary (producer integer units)");
  for (const auto& row : response.rows) {
    lines.push_back(std::to_string(row.variant) + " " + std::to_string(row.source) + " " +
        std::to_string(row.inputCut) + " " + std::to_string(row.timestampNs) + " " + row.metric + " " +
        std::to_string(row.value) + " " + std::to_string(row.auxiliary));
    lines.push_back("  account=" + std::to_string(row.accountId) + " currency-asset=" + std::to_string(row.currencyAssetId) + " " + row.exchange + "/" + row.market + "/" + row.instrument);
    if (row.kind == research::ResultKind::Distribution && row.value > 0) {
      const auto bar = static_cast<std::size_t>(std::min<std::int64_t>(row.value, std::max(columns - 20, 1)));
      lines.push_back("  value " + std::string(bar, '#') + (row.value > static_cast<std::int64_t>(bar) ? "+" : ""));
    }
    if (!row.payload.empty()) lines.push_back("  " + row.payload);
  }
  lines.push_back(response.more ? "More rows available: n / :next" : "End of committed result page.");
}
void recording(std::vector<std::string>& lines, const Model& model) {
  const auto& status = model.recorderStatus; const auto& config = model.recording;
  lines.push_back("RECORDING  producer runtime=" + model.recorderRuntime.string());
  if (!model.recorderError.empty()) lines.push_back("Unavailable: " + model.recorderError);
  else {
    lines.push_back(std::string("Capture: ") + (status.active ? "ACTIVE" : "idle") + (status.stopping ? " STOP REQUESTED" : "") +
        " configured=" + (status.configured ? "yes" : "no") + " complete=" + (status.complete ? "yes" : "no"));
    lines.push_back("Session: " + status.sessionPath.string());
    lines.push_back("Records=" + std::to_string(status.records) + " sources=" + std::to_string(status.sourceCount) +
        " capture-gaps=" + std::to_string(status.gaps) + " bytes=" + std::to_string(status.projectedBytes));
    lines.push_back("Epoch=" + std::to_string(status.producerEpoch) + " cut=" + std::to_string(status.cutGeneration) +
        " safe_monotonic_ns=" + std::to_string(status.safeMonotonicNs) + " loss-epoch=" + std::to_string(status.lossEpoch));
    if (!status.error.empty()) lines.push_back("Producer: " + status.error);
  }
  lines.push_back("LOCAL CONFIG DRAFT file=" + model.recordingConfigPath.string() + " (send with :record-configure before :record-start)");
  lines.push_back("Workspace=" + config.workspaceRoot.string());
  lines.push_back("Parser template=" + config.parserTemplate.string() + " env path=" + config.envPath.string());
  lines.push_back("Output=" + config.outputRoot.string() + " channels=" + std::to_string(config.channelMask) + " duration_s=" + std::to_string(config.durationSec));
  lines.push_back("Maximum bytes=" + std::to_string(config.maximumBytes) + " segment bytes=" + std::to_string(config.segmentBytes));
  lines.push_back("Compressed cache bytes=" + std::to_string(config.retainedCompressedBytes) + " writer records=" + std::to_string(config.writerQueueRecords));
  lines.push_back("Maximum reference bytes=" + std::to_string(config.maximumReferenceBytes));
  for (const auto& venue : config.venues) {
    std::string line = venue.exchange + "/" + venue.market + (venue.fullUniverse ? " FULL UNIVERSE" : " instruments:");
    for (const auto& instrument : venue.instruments) line += " " + instrument;
    lines.push_back(line);
  }
  if (config.venues.empty()) lines.push_back("No venue selected: :venue EXCHANGE MARKET INSTRUMENT[,INSTRUMENT] or *");
  lines.push_back("Explicit stop: :record-stop. Closing this UI leaves recording and research services running.");
}
void sources(std::vector<std::string>& lines, const Model& model) {
  lines.push_back("CORPUS / SOURCE CATALOG  selected=" + (model.catalogSourcePath.empty() ? model.recorderStatus.sessionPath.string() : model.catalogSourcePath.string()));
  lines.push_back("ARCHIVE total=" + std::to_string(model.sessions.totalSessions) + " offset=" + std::to_string(model.sessionOffset) + " (:sessions-next; :session N selects)");
  lines.push_back("Time filter receive_ns=" + std::to_string(model.catalogBeginNs) + ".." + std::to_string(model.catalogEndNs) + " (0 end unbounded; filter applies to displayed page)");
  if (!model.sessionsError.empty()) lines.push_back("Archive unavailable: " + model.sessionsError);
  else for (std::size_t index = 0; index < model.sessions.sessions.size(); ++index) {
    const auto& session = model.sessions.sessions[index];
    if (session.endedReceiveNs != 0 && session.endedReceiveNs < model.catalogBeginNs) continue;
    if (model.catalogEndNs != 0 && session.startedReceiveNs >= model.catalogEndNs) continue;
    lines.push_back(std::to_string(index + 1) + ". " + session.sourcePath.string() + (session.active ? " ACTIVE" : "") + (session.complete ? " complete" : " incomplete/active"));
    lines.push_back("   identity=" + session.sourceIdentity + " receive_ns=" + std::to_string(session.startedReceiveNs) + ".." + std::to_string(session.endedReceiveNs) + " sources=" + std::to_string(session.sourceCount) + " cut=" + std::to_string(session.cutGeneration));
  }
  if (!model.sourceError.empty()) { lines.push_back("Source directory unavailable: " + model.sourceError); return; }
  lines.push_back("Recorder cut=" + std::to_string(model.sources.cutGeneration) + " sources=" + std::to_string(model.sources.totalSources) +
      " offset=" + std::to_string(model.sourceOffset) + " (:sources-next / r)");
  lines.push_back("Current live capture safe_monotonic_ns=" + std::to_string(model.recorderStatus.safeMonotonicNs) + "; draft begin_receive_ns=" + std::to_string(model.draft.beginReceiveNs));
  lines.push_back("Current capture identity=" + model.recorderStatus.sourceIdentity +
      " started_receive_ns=" + std::to_string(model.recorderStatus.startedReceiveNs) + " started_monotonic_ns=" + std::to_string(model.recorderStatus.startedMonotonicNs));
  lines.push_back("source venue market canonical-symbol price-scale qty-scale configured/available/replay masks");
  for (const auto& source : model.sources.sources) {
    const std::string venue(source.venue.data(), std::min<std::size_t>(source.venueBytes, source.venue.size()));
    const std::string market(source.market.data(), std::min<std::size_t>(source.marketBytes, source.market.size()));
    const std::string symbol(source.canonicalSymbol.data(), std::min<std::size_t>(source.canonicalSymbolBytes, source.canonicalSymbol.size()));
    lines.push_back(std::to_string(source.sourceId) + " " + venue + " " + market + " " + symbol + " " +
        std::to_string(source.priceScale) + " " + std::to_string(source.quantityScale) + " " +
        std::to_string(source.configuredChannelMask) + "/" + std::to_string(source.availableChannelMask) + "/" + std::to_string(source.traderReplayChannelMask));
  }
  if (model.sources.sources.empty()) lines.push_back("No published sources at this Recorder cut.");
  lines.push_back("Select corpus explicitly: :set source PATH ; :set source-identity EXACT_PRODUCER_ID ; :set begin NS");
  lines.push_back(":source-current explicitly pins the displayed Recorder capture into the draft (including its exact identity).");
  lines.push_back("Source identity is never inferred from another active capture; archived corpus paths are accepted explicitly.");
}
void experiment(std::vector<std::string>& lines, const Model& model) {
  const auto& spec = model.draft;
  lines.push_back("EXPERIMENT DRAFT  file=" + model.specPath.string());
  lines.push_back("id=" + spec.id + " module=" + spec.module + " profile=" + spec.profile + " kind=" + (spec.kind == research::ModuleKind::Strategy ? "strategy" : "study"));
  lines.push_back("Source=" + spec.sourcePath.string() + " identity=" + spec.sourceIdentity + " begin_ns=" + std::to_string(spec.beginReceiveNs));
  lines.push_back("Recorder RAM-feed runtime=" + spec.recorderRuntimeDirectory.string() + " (empty only for finalized archives)");
  lines.push_back("Balance_e8=" + std::to_string(spec.initialBalanceE8) + " valuation=" + spec.valuationPolicy + " seed=" + std::to_string(spec.seed));
  lines.push_back("Valuation quote asset ID=" + std::to_string(spec.valuationQuoteAssetId) + " (explicit for strategies; currencies stay qualified)");
  lines.push_back(std::string("Virtual host: strategy-budget-enabled=") + (spec.hasStrategyBudget ? "true" : "false") +
      " budget_e8=" + std::to_string(spec.strategyBudgetE8) + " currency=" + spec.budgetCurrency + " margin-borrow=" + (spec.marginBorrowAllowed ? "true" : "false"));
  lines.push_back("Build=" + spec.buildIdentity + " module identity=" + spec.moduleIdentity);
  if (model.catalog.ok) {
    lines.push_back("Connected service build=" + model.catalog.buildIdentity + " module=" + model.catalog.moduleIdentity);
    lines.push_back("Global resident byte limit=" + std::to_string(model.catalog.residentByteLimit) + " variant limit=" + std::to_string(model.catalog.residentVariantLimit));
    if (spec.buildIdentity != model.catalog.buildIdentity || spec.moduleIdentity != model.catalog.moduleIdentity) lines.push_back("PROVENANCE MISMATCH: review the saved spec; :service-identity explicitly selects this service's identities");
  }
  for (const auto& fee : spec.fees) lines.push_back("Fee " + fee.exchange + "/" + fee.market + " maker_bps_e8=" + std::to_string(fee.makerFeeBpsE8) + " taker_bps_e8=" + std::to_string(fee.takerFeeBpsE8));
  if (spec.fees.empty() && spec.kind == research::ModuleKind::Strategy) lines.push_back("Explicit scenario fees required: :fee EXCHANGE MARKET MAKER_BPS_E8 TAKER_BPS_E8");
  std::string bounds = "Distribution bounds E8:"; for (auto bound : spec.distributionBoundsE8) bounds += " " + std::to_string(bound); lines.push_back(bounds);
  lines.push_back("Maximum variants=" + std::to_string(spec.budget.maxVariants) + " sources=" + std::to_string(spec.budget.maxSources) +
      " input batch=" + std::to_string(spec.budget.inputBatchEvents) + " result batch=" + std::to_string(spec.budget.resultBatchRows));
  lines.push_back("Maximum contexts/variant=" + std::to_string(spec.budget.maxContexts) + " state bytes/variant=" + std::to_string(spec.budget.maxStateBytes));
  lines.push_back("Input bytes=" + std::to_string(spec.budget.maxInputBytes) + " output bytes=" + std::to_string(spec.budget.maxOutputBytes) + " rows=" + std::to_string(spec.budget.maxResultRows));
  lines.push_back("Config bytes=" + std::to_string(spec.rawConfigText.size()) + " expanded variants=" + std::to_string(spec.variants.size()) + " study interval_ns=" + std::to_string(spec.studyIntervalNs));
  lines.push_back("Grid (producer expands complete configurations; every variant has an independent portfolio):");
  std::istringstream grid(spec.rawGridText); std::string line;
  while (std::getline(grid, line) && lines.size() < 128) lines.push_back("  " + line);
  lines.push_back("MODULE CATALOG / TYPED PARAMETERS (producer-owned)");
  if (!model.catalog.ok) lines.push_back("Unavailable: " + model.catalog.error);
  for (const auto& module : model.catalog.catalog) {
    if (lines.size() >= 4096) { lines.push_back("Catalog display capped at 4096 lines."); break; }
    lines.push_back(module.name + " -- " + module.description);
    std::string profiles = "  profiles:"; for (const auto& profile : module.profiles) profiles += " " + profile; lines.push_back(profiles);
    for (const auto& parameter : module.parameters) {
      if (lines.size() >= 4096) break;
      lines.push_back("  " + parameter.name + " kind=" + research::parameterKindName(parameter.kind) + " default=" + parameter.defaultValue + (parameter.required ? " required" : ""));
    }
  }
  lines.push_back("c creates only. s starts selected job. :load/:save PATH preserve the complete producer-owned spec.");
}
void jobs(std::vector<std::string>& lines, const Model& model) {
  lines.push_back("JOBS  research socket=" + model.researchSocket.string());
  if (!model.researchError.empty()) lines.push_back("Unavailable: " + model.researchError);
  if (model.status.experiments.empty()) lines.push_back("No jobs loaded. Set up an experiment (3) and create it (c).");
  for (const auto& status : model.status.experiments) {
    lines.push_back((status.id == model.selectedId ? "> " : "  ") + status.id + " " + stateName(status.state) +
        (status.backgroundEnabled ? " scheduled" : " unscheduled") + " cut=" + std::to_string(status.inputCut) + " visible=" + std::to_string(status.visibleInputCut));
    lines.push_back("  generation=" + std::to_string(status.generation) + " visible-generation=" + std::to_string(status.visibleGeneration) +
        " output-bytes=" + std::to_string(status.outputBytes) + " source=" + status.sourceIdentity);
    if (model.recorderError.empty() && status.sourceIdentity == model.recorderStatus.sourceIdentity && status.inputCut != 0 && model.recorderStatus.safeMonotonicNs >= status.inputCut)
      lines.push_back("  input lag_ns=" + std::to_string(model.recorderStatus.safeMonotonicNs - status.inputCut) + " (capture safe cut minus processed cut)");
    else lines.push_back("  input lag unavailable for this source/cut; progress remains job-local");
    if (!status.diagnostic.empty()) lines.push_back("  " + status.diagnostic);
    if (status.id == model.selectedId) for (const auto& variant : status.variants) {
      lines.push_back("  portfolio/variant=" + std::to_string(variant.id) + " events=" + std::to_string(variant.events) +
          " rows=" + std::to_string(variant.rows) + " last_receive_ns=" + std::to_string(variant.lastReceiveNs));
      lines.push_back("    inputcut=" + std::to_string(variant.inputCut) + " terminal=" + (variant.terminal ? "true" : "false") +
          " risk-stopped=" + (variant.riskStopped ? "true" : "false") + " terminal_monotonic_ns=" + std::to_string(variant.terminalMonotonicNs));
    }
  }
  lines.push_back("Up/Down selects; s start; p pause; u resume; x stop (only selected research job).");
  lines.push_back("Capture loss and subscriber lag are independent. Per-job inputcut is processing progress; visible cut is committed output.");
}
void appendConfiguration(std::vector<std::string>& lines, const std::string& text, int columns) {
  const auto width = static_cast<std::size_t>(std::max(columns - 4, 10));
  std::istringstream input(text); std::string line;
  while (std::getline(input, line)) {
    if (lines.size() >= 4096) break;
    if (line.empty()) { lines.push_back("  "); continue; }
    for (std::size_t offset = 0; offset < line.size() && lines.size() < 4096;) {
      auto end = std::min(line.size(), offset + width);
      while (end < line.size() && end > offset && (static_cast<unsigned char>(line[end]) & 0xc0U) == 0x80U) --end;
      if (end == offset) end = std::min(line.size(), offset + width);
      lines.push_back("  " + line.substr(offset, end - offset)); offset = end;
    }
  }
}
void specification(std::vector<std::string>& lines, const Model& model, int columns) {
  const auto& response = model.description;
  lines.push_back("SAVED SPECIFICATION (READ ONLY) " + model.descriptionId);
  if (!response.ok || !response.hasSpecification) { lines.push_back("Unavailable: " + response.error); return; }
  const auto& spec = response.specification;
  lines.push_back("id=" + spec.id + " module=" + spec.module + " profile=" + spec.profile + " seed=" + std::to_string(spec.seed));
  lines.push_back("Source=" + spec.sourcePath.string() + " identity=" + spec.sourceIdentity + " begin_receive_ns=" + std::to_string(spec.beginReceiveNs));
  lines.push_back("Build=" + spec.buildIdentity + " module identity=" + spec.moduleIdentity);
  lines.push_back("Balance_e8=" + std::to_string(spec.initialBalanceE8) + " valuation=" + spec.valuationPolicy + " quote-asset=" + std::to_string(spec.valuationQuoteAssetId));
  lines.push_back(std::string("Virtual host: budget-enabled=") + (spec.hasStrategyBudget ? "true" : "false") + " budget_e8=" + std::to_string(spec.strategyBudgetE8) +
      " currency=" + spec.budgetCurrency + " margin-borrow=" + (spec.marginBorrowAllowed ? "true" : "false"));
  lines.push_back("Recorder runtime=" + spec.recorderRuntimeDirectory.string() + " study interval_ns=" + std::to_string(spec.studyIntervalNs));
  lines.push_back("Maximum variants=" + std::to_string(spec.budget.maxVariants) + " sources=" + std::to_string(spec.budget.maxSources) + " contexts/variant=" + std::to_string(spec.budget.maxContexts) + " state bytes/variant=" + std::to_string(spec.budget.maxStateBytes));
  lines.push_back("Input/result batches=" + std::to_string(spec.budget.inputBatchEvents) + "/" + std::to_string(spec.budget.resultBatchRows) +
      " input/output bytes=" + std::to_string(spec.budget.maxInputBytes) + "/" + std::to_string(spec.budget.maxOutputBytes) + " result rows=" + std::to_string(spec.budget.maxResultRows));
  for (const auto& fee : spec.fees) lines.push_back("Fee " + fee.exchange + "/" + fee.market + " maker_bps_e8=" + std::to_string(fee.makerFeeBpsE8) + " taker_bps_e8=" + std::to_string(fee.takerFeeBpsE8));
  std::string bounds = "Distribution bounds E8:"; for (auto bound : spec.distributionBoundsE8) bounds += " " + std::to_string(bound);
  appendConfiguration(lines, bounds, columns);
  lines.push_back("Grid variants total=" + std::to_string(response.specificationVariantCount) + " page offset=" + std::to_string(response.specificationVariantOffset) +
      " loaded=" + std::to_string(spec.variants.size()) + " (this response is a slice, not a complete Create spec)");
  lines.push_back("Next page: n / :describe-next. :clone NEW_ID rebuilds the complete producer grid into a separate editable draft.");
  lines.push_back("Immutable original grid:"); appendConfiguration(lines, spec.rawGridText, columns);
  for (std::size_t index = 0; index < spec.variants.size() && lines.size() < 4096; ++index) {
    const auto& variant = spec.variants[index];
    if (index < response.variantDescriptions.size()) {
      const auto& descriptor = response.variantDescriptions[index];
      lines.push_back("VARIANT " + std::to_string(descriptor.id) + " ordinal=" + std::to_string(descriptor.ordinal) + " label=" + descriptor.label);
    } else lines.push_back("VARIANT slice index=" + std::to_string(index) + " (producer label unavailable)");
    for (const auto& parameter : variant.parameters) {
      if (lines.size() >= 4096) break;
      std::string kind = "producer value";
      if (model.catalog.ok && model.catalog.buildIdentity == spec.buildIdentity && model.catalog.moduleIdentity == spec.moduleIdentity)
        for (const auto& module : model.catalog.catalog) if (module.name == spec.module)
          for (const auto& metadata : module.parameters) if (metadata.name == parameter.name) kind = research::parameterKindName(metadata.kind);
      lines.push_back("  " + parameter.name + " [" + kind + "]=" + parameter.value);
    }
    lines.push_back("Immutable complete expanded configuration bytes=" + std::to_string(variant.configText.size()));
    appendConfiguration(lines, variant.configText, columns);
  }
  if (lines.size() < 4096) { lines.push_back("Immutable common configuration template bytes=" + std::to_string(spec.rawConfigText.size())); appendConfiguration(lines, spec.rawConfigText, columns); }
  if (lines.size() >= 4096) lines.push_back("Display preview capped at 4096 lines; page response remains intact. Widen terminal or select the next variant page.");
  else lines.push_back(response.more ? "More saved variants: n / :describe-next" : "End of saved variant page.");
}
void help(std::vector<std::string>& lines) {
  lines.insert(lines.end(), {
    "HELP / WORKFLOW  UI connects to existing independent producer processes.",
    "1 Recording | 2 Corpus/Sources | 3 Draft | 4 Jobs | 5 Results | 6 Compare | 7 Saved | ? Help",
    "Tab/Left/Right tabs; j/k or PgUp/PgDown scroll (Up/Down select job on Jobs); r refresh; q detaches.",
    "Press : to enter a command. Enter submits; Esc cancels; quote paths with spaces.",
    "Recording: :record-set FIELD VALUE ; :venue EXCHANGE MARKET SYMBOL[,SYMBOL] (or *)",
    "Recording fields: workspace parser-template env output channels duration maximum-bytes",
    "                 segment-bytes retained-bytes reference-bytes queue-records",
    ":record-load PATH loads a producer-owned bounded recording profile into the local draft.",
    ":record-configure -> :record-start -> :record-stop (asynchronous stop; status confirms idle)",
    "Corpus: :sources-next pages current source directory; :source-current pins this capture; r resets page.",
    "Archive: :sessions-next ; :session N pins displayed session and loads its sources via Recorder.",
    "Archive filters: :set catalog-begin NS ; :set catalog-end NS (0 unbounded); page-local metadata filter.",
    "Experiment: :load PATH loads complete spec; :save PATH saves; :set FIELD VALUE edits draft.",
    "Fields: id kind(strategy|study) module profile source source-identity recorder-runtime begin seed balance-e8",
    "        config(FILE) grid(FILE) build-identity module-identity valuation study-interval-ns",
    "        valuation-quote-asset strategy-budget-enabled(true|false) strategy-budget-e8 budget-currency",
    "        margin-borrow(true|false); explicit virtual-host facts are checked by the canonical strategy owner.",
    ":service-identity explicitly selects build/module provenance from a fresh producer Catalog response.",
    "New unsaved drafts use reported service identities; loaded specs retain their original provenance.",
    "Budgets: max-variants max-sources max-contexts state-bytes input-batch result-batch input-bytes output-bytes result-rows",
    "state-bytes/max-contexts bound each variant; connected service enforces its separate global resident limit.",
    "Config/grid files are bounded at 1 MiB; grid: NAME=VALUE,VALUE, using ${NAME} in config.",
    ":range NAME BEGIN END STEP delegates exact decimal inclusive ranges to producer; load config first.",
    ":values NAME VALUE,VALUE adds explicit literals matching the producer parameter type.",
    ":fee EXCHANGE MARKET MAKER_BPS_E8 TAKER_BPS_E8 ; :bounds SIGNED_E8,SIGNED_E8,...",
    "Select canonical module/profile from the displayed producer catalog; complete config remains explicit.",
    ":create expands producer grid and creates job. :start [ID] :pause [ID] :resume [ID] :stop [ID]",
    "Jobs own independent portfolios, scheduling and seeds. Pause/stop never stop recording.",
    ":describe [ID] reads saved immutable parameters/config from the service, leaving the editable draft unchanged.",
    "Saved variants are paginated: n / :describe-next; :clone NEW_ID re-expands the full grid into a separate draft.",
    "Results: :set result-kind equity|fills|distribution|series|exposure|scalar ; :results [ID]",
    "Filters: :set variant ID (0 all) ; :set result-begin NS ; :set result-end NS (0 unbounded)",
    "Equity/drawdown metrics use producer integer units. Fill/exposure payloads show real producer JSON.",
    "n / :next pages results; r refresh resets page. j/k scroll; payloads are safely clipped to width.",
    ":compare OTHER_ID loads selected job and OTHER_ID below a common exclusive committed inputcut, same corpus/begin.",
    "Compare labels actual event inputcuts; events at/above the frozen upper cut are excluded. n pages both queries.",
    "Failure is shown explicitly; there are no fabricated charts, sample portfolios or native-session fallbacks.",
    "q, :quit, SIGINT and SIGTERM detach this terminal only. Services keep running."
  });
}
}
std::vector<std::string> render(const Model& model, int columns) {
  std::vector<std::string> lines;
  lines.reserve(512);
  lines.push_back("RESEARCH [1 Record][2 Corpus][3 Draft][4 Jobs][5 Results][6 Compare][7 Saved][?]");
  switch (model.screen) {
    case Screen::Recording: recording(lines, model); break;
    case Screen::Sources: sources(lines, model); break;
    case Screen::Experiment: experiment(lines, model); break;
    case Screen::Jobs: jobs(lines, model); break;
    case Screen::Results:
      lines.push_back("RESULTS " + model.selectedId + " kind=" + resultName(model.resultKind) + " variant=" + std::to_string(model.variant) + " offset=" + std::to_string(model.resultOffset));
      lines.push_back("Receive range_ns=" + std::to_string(model.beginNs) + ".." + std::to_string(model.endNs) + " (0 end = unbounded)");
      appendRows(lines, model.results, columns); break;
    case Screen::Compare:
      lines.push_back("COMPARE " + model.selectedId + " / " + model.compareId + " exclusive upper inputcut=" + std::to_string(model.comparisonCut));
      lines.push_back("Same exact source identity and replay start; producer rows only. Common inputcut is not wall-clock time.");
      lines.push_back("A: " + model.selectedId); appendRows(lines, model.results, columns);
      lines.push_back("B: " + model.compareId); appendRows(lines, model.comparison, columns); break;
    case Screen::Help: help(lines); break;
    case Screen::Specification: specification(lines, model, columns); break;
  }
  return lines;
}
} // namespace hft_backtest::tui
