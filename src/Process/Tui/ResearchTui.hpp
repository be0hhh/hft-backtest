#pragma once

#include "hft_backtest/Research.hpp"
#include "hftrec/Service/RecorderService.hpp"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace hft_backtest::tui {
enum class Screen : std::uint8_t { Recording, Sources, Experiment, Jobs, Results, Compare, Specification, Help };
enum class Action : std::uint8_t { None, Refresh, Create, Start, Pause, Resume, Stop, RecordingConfigure, RecordingStart, RecordingStop, Results, Next, Compare, Describe, Load, Save, RecordingLoad, Quit };
struct ParsedCommand { Action action{Action::None}; std::vector<std::string> words; std::string error; };
// All UI allocations are cold. Pages are replaced, never accumulated. Command
// lines and files are bounded; producer contracts own their independent quotas.
inline constexpr std::size_t kMaximumCommandBytes = 4096;
inline constexpr std::size_t kMaximumConfigBytes = 1U << 20;
inline constexpr std::uint32_t kPageRows = 128;
struct Model {
  Screen screen{Screen::Jobs};
  std::filesystem::path researchSocket, recorderRuntime, specPath, recordingConfigPath;
  research::ExperimentSpec draft;
  hftrec::service::RecorderServiceConfig recording;
  hftrec::service::RecorderServiceStatus recorderStatus;
  hftrec::service::RecorderCatalog sources;
  hftrec::service::RecorderSessionCatalog sessions;
  std::filesystem::path catalogSourcePath;
  research::Response catalog, status, results, comparison, description;
  std::string selectedId, compareId, descriptionId, message{"Press ? for help; : enters a command."};
  std::string researchError, recorderError, sourceError, sessionsError;
  std::size_t selectedJob{}, scroll{};
  research::ResultKind resultKind{research::ResultKind::Equity};
  std::uint64_t variant{}, resultOffset{}, comparisonOffset{}, comparisonCut{}, descriptionOffset{};
  std::int64_t beginNs{}, endNs{}, catalogBeginNs{}, catalogEndNs{};
  std::uint32_t sourceOffset{}, sessionOffset{};
  bool exitRequested{};
};
ParsedCommand parseCommand(const std::string& line);
bool editDraft(Model&, const std::vector<std::string>& words, std::string& error);
bool editRecording(Model&, const std::vector<std::string>& words, std::string& error);
bool applyServiceIdentity(Model&, std::string& error);
bool cloneDescription(Model&, const std::string& newId, std::string& error);
research::Request describeRequest(const Model&);
bool addParameterRange(research::ExperimentSpec&, const std::string& name,
                       const std::string& begin, const std::string& end,
                       const std::string& step, std::string& error);
research::Request resultRequest(const Model&, const std::string& id, std::uint64_t maximumCut = 0);
bool commonInputCut(const research::Response&, const std::string&, const std::string&,
                    std::uint64_t& cut, std::string& error);
void selectJob(Model&, int direction);
void invalidateResults(Model&);
std::string safeText(const std::string& text, std::size_t maximum = 4096);
std::vector<std::string> render(const Model&, int columns);
int run(int argc, char** argv);
} // namespace hft_backtest::tui
