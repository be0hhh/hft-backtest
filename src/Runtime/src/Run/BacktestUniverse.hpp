#pragma once
#include "../Strategy/BacktestStrategyRuntime.hpp"
#include "hftrec/CorpusContract/BinaryMarketCorpusFormat.hpp"

namespace hft_backtest::run_detail {
struct UniverseContextSelection {
  std::vector<std::size_t> sourceIndices;
  std::uint32_t primaryLegIndex{};
  BacktestTradeMode tradeMode{BacktestTradeMode::AllLegs};
};
[[nodiscard]] std::int64_t virtualAccountInitialBalance(const BacktestRunRequest& request,
    const BacktestStrategyVenueConfig& venue, std::size_t sourceIndex) noexcept;
[[nodiscard]] bool discoverUniverseContexts(const trading_core::StrategyDescriptor& descriptor,
    const BacktestStrategyConfig& config, const std::vector<detail::SessionData>& sources,
    std::vector<UniverseContextSelection>& contexts, std::string& error);
[[nodiscard]] bool materializeUniverseSymbolsFromCorpus(BacktestStrategyConfig& config,
    std::span<const hftrec::corpus::BinaryMarketSource> catalog, std::string& error);
[[nodiscard]] bool applyCapturedSourceHealth(detail::EngineState& engine,
    const detail::SourceHealthRow& row, std::int64_t replayTsNs, std::string& error);
[[nodiscard]] bool applyCapturedSourceLifecycle(detail::EngineState& engine,
    const detail::SourceLifecycleRow& row, std::int64_t replayTsNs, std::string& error);
void runBinaryUniverse(BacktestRunResult& result, const BacktestRunRequest& request,
    const BacktestStrategyConfig& config, const trading_core::StrategyDescriptor& descriptor,
    BacktestProgressCallback progress, void* userData) noexcept;
}
