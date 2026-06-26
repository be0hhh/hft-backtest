#pragma once

#include "hft_backtest/backtest.hpp"

#include "engine/backtest_engine.hpp"
#include "input/session_loader.hpp"

#include "hft_trader/core/control/ControlState.hpp"
#include "hft_trader/core/market/LocalOrderBook.hpp"
#include "hft_trader/core/strategy/StrategyContract.hpp"
#include "hft_trader/runtime/config/RuntimeConfig.hpp"
#include "hft_trader/runtime/runner/StrategyDescriptor.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hft_backtest {

using StrategyInstrumentSpec = hft_trader::runtime::StrategyInstrumentSpec;

enum class RunUntilBoundary : std::uint8_t {
  Inclusive,
  Exclusive,
};

struct SingleLegCycleScratch {
  std::array<hft_trader::core::OrderIntent, detail::kIntentCapacity> orderBuf{};
  std::array<hft_trader::core::CancelIntent, detail::kIntentCapacity> cancelBuf{};
  std::array<hft_trader::core::StrategyCommandIntent, detail::kIntentCapacity> commandBuf{};
  std::array<hft_trader::core::StrategyOrderState, detail::kMaxOrderStates> orderStorage{};
};

struct MultiLegCycleScratch {
  std::array<hft_trader::core::OrderIntent, detail::kIntentCapacity> orderBuf{};
  std::array<hft_trader::core::CancelIntent, detail::kIntentCapacity> cancelBuf{};
  std::array<hft_trader::core::StrategyCommandIntent, detail::kIntentCapacity> commandBuf{};
  std::array<hft_trader::core::StrategyMarketSnapshot, hft_trader::core::kStrategyRuntimeMaxLegs> snapshots{};
  std::array<hft_trader::core::StrategyCandleSnapshot, hft_trader::core::kStrategyRuntimeMaxLegs> candles{};
  std::array<std::array<hft_trader::core::StrategyOrderState, detail::kMaxOrderStates>, hft_trader::core::kStrategyRuntimeMaxLegs> orderStorage{};
  std::array<hft_trader::core::StrategyOrderStateView, hft_trader::core::kStrategyRuntimeMaxLegs> orderViews{};
  std::array<hft_trader::core::StrategyPositionState, hft_trader::core::kStrategyRuntimeMaxLegs> positions{};
  std::array<hft_trader::core::StrategyAccountState, hft_trader::core::kStrategyRuntimeMaxLegs> accounts{};
  std::array<StrategyInstrumentSpec, hft_trader::core::kStrategyRuntimeMaxLegs> instruments{};
  std::array<std::uint64_t, hft_trader::core::kStrategyRuntimeMaxLegs> revisions{};
  std::size_t legCount{0u};

  void reset(std::size_t count) noexcept;
  void refreshLeg(std::size_t leg,
                  const std::vector<detail::EngineState>& engines,
                  const std::vector<StrategyInstrumentSpec>& legInstruments) noexcept;
  void refreshDirty(const std::vector<detail::EngineState>& engines,
                    const std::vector<StrategyInstrumentSpec>& legInstruments) noexcept;
};

[[nodiscard]] hft_trader::core::StrategyAccountState accountStateFromEngine(
    const detail::EngineState& engine) noexcept;
[[nodiscard]] std::size_t boundedRiskLegCount(std::size_t count) noexcept;
void refreshRiskAccountsFromEngines(
    const std::vector<detail::EngineState>& engines,
    std::array<hft_trader::core::StrategyAccountState, hft_trader::core::kStrategyRuntimeMaxLegs>& out) noexcept;
void resolveRiskVenuesForSessions(
    const hft_trader::runtime::HftRuntimeConfig& cfg,
    const std::vector<detail::SessionData>& sessions,
    std::array<const hft_trader::runtime::VenueRuntimeConfig*, hft_trader::core::kStrategyRuntimeMaxLegs>& out) noexcept;
[[nodiscard]] bool forceCloseForTraderRisk(detail::EngineState& engine, std::int64_t tsNs);
[[nodiscard]] std::int64_t portfolioEquityE8(const std::vector<detail::EngineState>& engines,
                                             std::int64_t initialBalanceE8) noexcept;
[[nodiscard]] TimeNs timeNsFromI64(std::int64_t value) noexcept;

void applyMarketEventToSnapshots(detail::EngineState& engine,
                                 const detail::SessionData& session,
                                 const detail::TimelineEvent& event,
                                 const Symbol& selectedSymbol,
                                 const StrategyInstrumentSpec& instrument,
                                 bool strategyVisible);
void applyMarketEventToSnapshots(detail::EngineState& engine,
                                 const std::vector<detail::SessionData>& sessions,
                                 const detail::TimelineEvent& event,
                                 const Symbol& selectedSymbol,
                                 const StrategyInstrumentSpec& instrument,
                                 bool strategyVisible);
void applyMarketEventToSnapshots(std::vector<detail::EngineState>& engines,
                                 const std::vector<detail::SessionData>& sessions,
                                 const detail::TimelineEvent& event,
                                 const std::vector<Symbol>& symbols,
                                 const std::vector<StrategyInstrumentSpec>& instruments,
                                 bool strategyVisible);

[[nodiscard]] bool runCycle(const hft_trader::runtime::StrategyDescriptor& descriptor,
                            void* state,
                            detail::EngineState& engine,
                            const StrategyInstrumentSpec& instrument,
                            SingleLegCycleScratch& scratch,
                            const hft_trader::core::SymbolShardConfig& shard,
                            const hft_trader::core::ShardControlState& control,
                            std::int64_t tsNs,
                            const BacktestRunRequest& request,
                            const cxet::composite::UserDataEvent* userEvent);
[[nodiscard]] bool runCycleMulti(const hft_trader::runtime::StrategyDescriptor& descriptor,
                                 void* state,
                                 std::vector<detail::EngineState>& engines,
                                 const std::vector<StrategyInstrumentSpec>& legInstruments,
                                 MultiLegCycleScratch& scratch,
                                 const hft_trader::core::SymbolShardConfig& shard,
                                 const hft_trader::core::ShardControlState& control,
                                 std::uint32_t eventLegIndex,
                                 std::int64_t tsNs,
                                 const BacktestRunRequest& request,
                                 const cxet::composite::UserDataEvent* userEvent);
[[nodiscard]] bool runUntil(const hft_trader::runtime::StrategyDescriptor& descriptor,
                            void* state,
                            detail::EngineState& engine,
                            const StrategyInstrumentSpec& instrument,
                            SingleLegCycleScratch& scratch,
                            const hft_trader::core::SymbolShardConfig& shard,
                            const hft_trader::core::ShardControlState& control,
                            std::int64_t untilTs,
                            const BacktestRunRequest& request,
                            RunUntilBoundary boundary = RunUntilBoundary::Inclusive);
[[nodiscard]] bool runUntilMulti(const hft_trader::runtime::StrategyDescriptor& descriptor,
                                 void* state,
                                 std::vector<detail::EngineState>& engines,
                                 const std::vector<StrategyInstrumentSpec>& legInstruments,
                                 MultiLegCycleScratch& scratch,
                                 const hft_trader::core::SymbolShardConfig& shard,
                                 const hft_trader::core::ShardControlState& control,
                                 std::int64_t untilTs,
                                 const BacktestRunRequest& request,
                                 RunUntilBoundary boundary = RunUntilBoundary::Inclusive);
[[nodiscard]] bool settleAfterReplay(const hft_trader::runtime::StrategyDescriptor& descriptor,
                                     void* state,
                                     detail::EngineState& engine,
                                     const StrategyInstrumentSpec& instrument,
                                     SingleLegCycleScratch& scratch,
                                     const hft_trader::core::SymbolShardConfig& shard,
                                     const hft_trader::core::ShardControlState& control,
                                     std::int64_t lastTsNs,
                                     const BacktestRunRequest& request);
[[nodiscard]] bool settleAfterReplayMulti(const hft_trader::runtime::StrategyDescriptor& descriptor,
                                          void* state,
                                          std::vector<detail::EngineState>& engines,
                                          const std::vector<StrategyInstrumentSpec>& legInstruments,
                                          MultiLegCycleScratch& scratch,
                                          const hft_trader::core::SymbolShardConfig& shard,
                                          const hft_trader::core::ShardControlState& control,
                                          std::int64_t lastTsNs,
                                          const BacktestRunRequest& request);

}  // namespace hft_backtest
