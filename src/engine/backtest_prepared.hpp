#pragma once

#include "engine/backtest_engine.hpp"
#include "execution/backtest_execution_depth.hpp"

#include <filesystem>
#include <vector>

namespace hft_backtest::detail {

struct PreparedBacktestInput {
  bool valid{false};
  bool timelineValid{false};
  bool hasExplicitSessions{false};
  SessionLoadPlan loadPlan{};
  std::vector<std::filesystem::path> sessionPaths{};
  std::vector<SessionData> sessions{};
  std::vector<TimelineEvent> timeline{};
  std::vector<std::shared_ptr<ExecutionDepthCheckpointCache>> depthCaches{};
};

class PreparedBacktestScope {
 public:
  explicit PreparedBacktestScope(PreparedBacktestInput* input) noexcept;
  PreparedBacktestScope(const PreparedBacktestScope&) = delete;
  PreparedBacktestScope& operator=(const PreparedBacktestScope&) = delete;
  ~PreparedBacktestScope();

 private:
  PreparedBacktestInput* previous_{nullptr};
};

PreparedBacktestInput* activePreparedBacktestInput() noexcept;

}  // namespace hft_backtest::detail
