#pragma once

#include "backtest_engine.hpp"

#include <cstdint>
#include <filesystem>

namespace hft_backtest::detail {

struct ExecutionDepthProviderImpl;

class ExecutionDepthProvider {
 public:
  ExecutionDepthProvider() noexcept = default;
  ExecutionDepthProvider(const ExecutionDepthProvider&) = delete;
  ExecutionDepthProvider& operator=(const ExecutionDepthProvider&) = delete;
  ExecutionDepthProvider(ExecutionDepthProvider&&) = delete;
  ExecutionDepthProvider& operator=(ExecutionDepthProvider&&) = delete;
  ~ExecutionDepthProvider();

  [[nodiscard]] bool open(const std::filesystem::path& sessionPath,
                          const Symbol& symbol,
                          const hft_trader::core::LocalOrderBookConfig& config,
                          const hft_trader::core::LocalOrderBookState& initialBook) noexcept;
  [[nodiscard]] bool advanceTo(std::int64_t tsNs) noexcept;

  [[nodiscard]] const hft_trader::core::LocalOrderBookState& book() const noexcept { return book_; }
  [[nodiscard]] bool ready() const noexcept;
  [[nodiscard]] std::uint64_t rowsRead() const noexcept { return rowsRead_; }

 private:
  ExecutionDepthProviderImpl* impl_{nullptr};
  hft_trader::core::LocalOrderBookState book_{};
  Symbol symbol_{};
  hft_trader::core::LocalOrderBookConfig config_{};
  std::uint64_t rowsRead_{0};
  bool available_{false};
};

}  // namespace hft_backtest::detail
