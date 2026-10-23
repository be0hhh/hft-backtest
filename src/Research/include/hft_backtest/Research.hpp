#pragma once

#include <cstdint>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace hft_backtest::research {

inline constexpr std::uint32_t kResearchSchemaVersion = 2;
inline constexpr std::uint32_t kResearchProtocolVersion = 1;
// Wire admission ceiling matches the Recorder corpus owner (checked by service).
inline constexpr std::uint32_t kResearchMaximumSources = 65536;
inline constexpr std::uint32_t kResearchMaximumContexts = 100000;

enum class ModuleKind : std::uint8_t { Strategy, DataStudy };
enum class ExperimentState : std::uint8_t {
  Created, Running, Paused, Stopped, Incomplete, Failed, QuotaExceeded
};
enum class Command : std::uint8_t {
  Catalog, Status, Create, Start, Pause, Resume, Stop, Results, Range, Describe
};
enum class ResultKind : std::uint8_t {
  Scalar, Series, Distribution, Equity, Fill, Exposure
};
std::string parameterKindName(std::uint8_t kind);

struct ParameterValue { std::string name; std::string value; };
struct ExperimentVariant {
  // Complete expanded canonical strategy/module configuration, never a patch.
  std::string configText;
  std::vector<ParameterValue> parameters;
};
struct ScenarioFee {
  std::string exchange;
  std::string market;
  std::int64_t makerFeeBpsE8{};
  std::int64_t takerFeeBpsE8{};
};
struct ResourceBudget {
  std::uint32_t maxVariants{64};
  std::uint32_t maxSources{4096};
  std::uint32_t maxContexts{128};
  std::uint64_t maxStateBytes{256ULL << 20};
  std::uint32_t inputBatchEvents{4096};
  std::uint32_t resultBatchRows{4096};
  std::uint64_t maxInputBytes{64ULL << 20};
  std::uint64_t maxOutputBytes{1ULL << 30};
  std::uint64_t maxResultRows{10000000};
};
struct ExperimentSpec {
  std::uint32_t schemaVersion{kResearchSchemaVersion};
  std::string id;
  ModuleKind kind{ModuleKind::Strategy};
  std::string module;
  std::filesystem::path sourcePath;
  // Active inputs use Recorder's compressed RAMfeed; archive is the same durable
  // capture's catch-up path. Empty is valid for finalized archives only.
  std::filesystem::path recorderRuntimeDirectory;
  // Exact caller-selected corpus identity; never silently follow another capture.
  std::string sourceIdentity;
  std::int64_t beginReceiveNs{};
  std::string profile;
  std::string rawConfigText;
  std::string rawGridText;
  std::vector<ExperimentVariant> variants;
  std::vector<ScenarioFee> fees;
  std::int64_t initialBalanceE8{};
  bool hasStrategyBudget{};
  std::int64_t strategyBudgetE8{};
  std::string budgetCurrency;
  bool marginBorrowAllowed{};
  std::string valuationPolicy{"midprice_e8"};
  std::uint32_t valuationQuoteAssetId{};
  std::uint64_t seed{1};
  std::string buildIdentity;
  std::string moduleIdentity;
  ResourceBudget budget;
  // Fixed receive-clock sample buckets. Histogram units: DataStudy ratio E8;
  // strategy period returns bps E8, produced by the canonical finance owner.
  std::uint64_t studyIntervalNs{1000000000};
  std::vector<std::int64_t> distributionBoundsE8;
};
struct ResultRow {
  ResultKind kind{ResultKind::Scalar};
  std::uint64_t variant{};
  std::uint64_t source{};
  std::uint64_t inputCut{}; // Actual event monotonic coordinate, below output prefix.
  std::uint32_t accountId{};
  std::uint32_t currencyAssetId{};
  std::string exchange;
  std::string market;
  std::string instrument;
  std::int64_t timestampNs{};
  std::string metric;
  std::int64_t value{};
  std::int64_t auxiliary{};
  std::string payload; // Full producer JSON row for fills/exposure and native streams.
};
struct VariantStatus {
  std::uint64_t id{};
  std::uint64_t events{};
  std::uint64_t rows{};
  std::int64_t lastReceiveNs{};
  std::uint64_t inputCut{};
  bool terminal{};
  bool riskStopped{};
  std::uint64_t terminalMonotonicNs{};
};
struct ExperimentStatus {
  std::string id;
  std::uint64_t specFingerprint{};
  std::string sourceIdentity;
  std::filesystem::path sourcePath;
  std::int64_t beginReceiveNs{};
  ExperimentState state{ExperimentState::Created};
  bool backgroundEnabled{};
  std::uint64_t generation{};
  std::uint64_t visibleGeneration{};
  bool visibleFinal{};
  std::uint64_t inputCut{};
  std::uint64_t visibleInputCut{};
  std::uint64_t outputBytes{};
  std::string diagnostic;
  std::vector<VariantStatus> variants;
};
struct ModuleDescriptor {
  struct Parameter {
    std::string name;
    std::string defaultValue;
    std::uint8_t kind{};
    bool required{};
  };
  std::string name;
  ModuleKind kind{ModuleKind::Strategy};
  std::string description;
  std::vector<Parameter> parameters;
  std::vector<std::string> profiles;
};
struct Request {
  std::uint32_t protocolVersion{kResearchProtocolVersion};
  Command command{Command::Status};
  std::string experimentId;
  ExperimentSpec spec;
  ResultKind resultKind{ResultKind::Series};
  std::uint64_t variant{}; // Zero selects all variants.
  std::int64_t beginNs{};
  std::int64_t endNs{}; // Zero has no upper timestamp bound.
  std::uint64_t offset{};
  std::uint64_t maximumInputCut{}; // Exclusive event fence; zero selects latest visible cut.
  std::uint32_t limit{256};
};
struct VariantDescription {
  std::uint64_t id{};
  std::uint64_t ordinal{};
  std::string label;
};
struct Response {
  std::uint32_t protocolVersion{kResearchProtocolVersion};
  bool ok{};
  std::string error;
  std::string buildIdentity;
  std::string moduleIdentity;
  std::uint64_t residentByteLimit{};
  std::uint32_t residentVariantLimit{};
  bool hasSpecification{};
  // Describe returns common immutable fields and an offset/limit variants slice.
  // This slice is never a full-grid Create request without re-expansion.
  ExperimentSpec specification;
  std::uint64_t specificationVariantCount{};
  std::uint64_t specificationVariantOffset{};
  std::vector<VariantDescription> variantDescriptions; // Aligned with the slice.
  std::vector<ModuleDescriptor> catalog;
  std::vector<ExperimentStatus> experiments;
  std::vector<ResultRow> rows;
  std::uint64_t nextOffset{};
  bool more{};
};
struct ServiceConfig {
  std::filesystem::path root;
  std::filesystem::path socketPath;
  std::string buildIdentity;
  std::string moduleIdentity;
  std::uint32_t maxExperiments{128};
  std::uint32_t maxResidentVariants{64};
  std::uint32_t maxWorkItemsPerStep{1};
  std::uint32_t maxRequestBytes{4U << 20};
  std::uint32_t maxResponseRows{4096};
  std::uint64_t maxServiceOutputBytes{16ULL << 30};
  std::uint64_t maxResidentStateBytes{1ULL << 30};
};

// These files use the same bounded schema as the producer-owned local client.
bool saveExperimentSpec(const std::filesystem::path&, const ExperimentSpec&, std::string& error);
bool loadExperimentSpec(const std::filesystem::path&, ExperimentSpec&, std::string& error);
bool validateExperimentSpec(const ExperimentSpec&, const ServiceConfig&, std::string& error);
bool expandExperimentGrid(ExperimentSpec&, std::string& error);
std::uint64_t variantIdentity(const ExperimentSpec&, std::size_t ordinal);
std::vector<ModuleDescriptor> researchModuleCatalog();

class ResearchClient {
 public:
  explicit ResearchClient(std::filesystem::path socketPath);
  Response execute(const Request&) const;
 private:
  std::filesystem::path socketPath_;
};

class ResearchService {
 public:
  explicit ResearchService(ServiceConfig);
  ~ResearchService();
  ResearchService(const ResearchService&) = delete;
  ResearchService& operator=(const ResearchService&) = delete;
  bool restore(std::string& error);
  Response dispatch(const Request&);
  // Advances each scheduled owner with a finite event/result budget. No UI owner.
  void step();
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class UnixResearchServer {
 public:
  UnixResearchServer(ServiceConfig, ResearchService&);
  ~UnixResearchServer();
  bool open(std::string& error);
  bool poll(int timeoutMs, std::string& error);
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

int runResearchDaemon(const ServiceConfig&);
} // namespace hft_backtest::research
