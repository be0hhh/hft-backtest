#pragma once

#include "../../Engine/BacktestEngine.hpp"
#include "BinaryCorpusLoader.hpp"
#include <memory>
#include <span>

namespace hft_backtest::detail {

// Cold/offline replay owner. Storage follows selected sources and the largest
// current native transaction, never the selected corpus duration.
class BinaryCorpusStream final {
 public:
  BinaryCorpusStream() noexcept;
  ~BinaryCorpusStream();
  BinaryCorpusStream(const BinaryCorpusStream&) = delete;
  BinaryCorpusStream& operator=(const BinaryCorpusStream&) = delete;
  [[nodiscard]] Status open(const std::filesystem::path& path,
      std::span<const BacktestCorpusSourceRequest> sources,
      const SessionLoadPlan& plan, std::string& error, bool allowQuietSources = false) noexcept;
  [[nodiscard]] Status next(std::vector<TimelineEvent>& events, bool& available,
      std::string& error) noexcept;
  [[nodiscard]] const std::vector<SessionData>& sessions() const noexcept;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace hft_backtest::detail
