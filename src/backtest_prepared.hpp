#pragma once

#include "backtest_engine.hpp"

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
