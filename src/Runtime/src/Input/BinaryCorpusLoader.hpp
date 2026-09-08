#pragma once

#include "hft_backtest/Backtest.hpp"

#include "SessionLoader.hpp"

#include <filesystem>
#include <string>

namespace hft_backtest::detail {

[[nodiscard]] Status loadBinaryMarketCorpus(
    const std::filesystem::path& corpusPath,
    const BacktestCorpusSourceRequest& sourceRequest,
    const SessionLoadPlan& plan,
    SessionData& output,
    std::string& error) noexcept;

}  // namespace hft_backtest::detail
