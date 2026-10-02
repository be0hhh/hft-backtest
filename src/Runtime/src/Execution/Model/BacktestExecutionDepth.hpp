#pragma once

#include "../../Engine/BacktestEngine.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>

namespace hft_backtest::detail {

struct ExecutionDepthProviderImpl;
class ExecutionDepthCheckpointCache;

[[nodiscard]] std::shared_ptr<ExecutionDepthCheckpointCache> makeExecutionDepthCheckpointCache();

enum class ExecutionDepthOpenStatus : std::uint8_t {
  Absent = 0,
  Ready = 1,
  Failed = 2,
};

class ExecutionDepthProvider {
 public:
  ExecutionDepthProvider() noexcept = default;
  ExecutionDepthProvider(const ExecutionDepthProvider&) = delete;
  ExecutionDepthProvider& operator=(const ExecutionDepthProvider&) = delete;
  ExecutionDepthProvider(ExecutionDepthProvider&&) = delete;
  ExecutionDepthProvider& operator=(ExecutionDepthProvider&&) = delete;
  ~ExecutionDepthProvider();

  [[nodiscard]] ExecutionDepthOpenStatus open(const std::filesystem::path& sessionPath,
                                              const Symbol& symbol,
                                              const trading_core::LocalOrderBookConfig& config,
                                              const trading_core::LocalOrderBookState& initialBook,
                                              std::shared_ptr<ExecutionDepthCheckpointCache> cache = {}) noexcept;
  [[nodiscard]] bool advanceTo(std::int64_t tsNs) noexcept;

  [[nodiscard]] const trading_core::LocalOrderBookState& book() const noexcept { return book_; }
  [[nodiscard]] bool ready() const noexcept;
  [[nodiscard]] std::uint64_t rowsRead() const noexcept { return rowsRead_; }
  [[nodiscard]] std::uint64_t readNs() const noexcept { return readNs_; }
  [[nodiscard]] bool failed() const noexcept { return failed_; }
  [[nodiscard]] const char* failureReason() const noexcept { return failureReason_; }

 private:
  ExecutionDepthProviderImpl* impl_{nullptr};
  trading_core::LocalOrderBookState book_{};
  Symbol symbol_{};
  trading_core::LocalOrderBookConfig config_{};
  std::uint64_t rowsRead_{0};
  std::uint64_t readNs_{0};
  bool available_{false};
  bool failed_{false};
  const char* failureReason_{""};
  std::shared_ptr<ExecutionDepthCheckpointCache> cache_{};
};

}  // namespace hft_backtest::detail
