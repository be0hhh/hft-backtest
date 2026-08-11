#pragma once

#include "hft_backtest/backtest.hpp"

#include "input/session_loader.hpp"

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
