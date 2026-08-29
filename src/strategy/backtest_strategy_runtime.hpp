#pragma once

#include "hft_backtest/backtest.hpp"

#include "config/backtest_strategy_config.hpp"
#include "engine/backtest_engine.hpp"
#include "input/session_loader.hpp"
#include "strategy/backtest_strategy_input_freshness.hpp"

#include "trading_core/core/control/ControlState.hpp"
#include "trading_core/core/market/LocalOrderBook.hpp"
#include "trading_core/core/strategy/StrategyContract.hpp"
#include "trading_core/api/descriptor/StrategyDescriptor.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace hft_backtest {

using StrategyInstrumentSpec = trading_core::StrategyInstrumentSpec;

enum class RunUntilBoundary : std::uint8_t {
  Inclusive,
  Exclusive,
};

struct SingleLegCycleScratch {
  std::array<trading_core::OrderIntent, detail::kIntentCapacity> orderBuf{};
  std::array<trading_core::CancelIntent, detail::kIntentCapacity> cancelBuf{};
  std::array<trading_core::StrategyDesiredOrderSlot,
             trading_core::kStrategyRuntimeMaxDesiredOrders> desiredOrderBuf{};
  std::array<trading_core::StrategyCommandIntent,
             trading_core::kStrategyRuntimeMaxCommands +
                 2u * trading_core::kStrategyRuntimeMaxDesiredOrders>
      commandBuf{};
  std::array<trading_core::StrategyTransactionIntent,
             trading_core::kStrategyRuntimeMaxTransactions> transactionBuf{};
  std::array<trading_core::StrategyOrderState, detail::kMaxOrderStates> orderStorage{};
  std::array<trading_core::StrategyOrderPathWarmupView, 1> warmupViews{};
};

struct MultiLegCycleScratch {
  std::array<trading_core::OrderIntent, detail::kIntentCapacity> orderBuf{};
  std::array<trading_core::CancelIntent, detail::kIntentCapacity> cancelBuf{};
  std::array<trading_core::StrategyDesiredOrderSlot,
             trading_core::kStrategyRuntimeMaxDesiredOrders> desiredOrderBuf{};
  std::array<trading_core::StrategyCommandIntent,
             trading_core::kStrategyRuntimeMaxCommands +
                 2u * trading_core::kStrategyRuntimeMaxDesiredOrders>
      commandBuf{};
  std::array<trading_core::StrategyTransactionIntent,
             trading_core::kStrategyRuntimeMaxTransactions> transactionBuf{};
  std::array<trading_core::StrategyMarketSnapshot, trading_core::kStrategyRuntimeMaxLegs> snapshots{};
  std::array<trading_core::StrategyCandleSnapshot, trading_core::kStrategyRuntimeMaxLegs> candles{};
  std::array<std::array<trading_core::StrategyOrderState, detail::kMaxOrderStates>, trading_core::kStrategyRuntimeMaxLegs> orderStorage{};
  std::array<trading_core::StrategyOrderStateView, trading_core::kStrategyRuntimeMaxLegs> orderViews{};
  std::array<trading_core::StrategyOrderPathWarmupView, trading_core::kStrategyRuntimeMaxLegs> warmupViews{};
  std::array<trading_core::StrategyPositionState, trading_core::kStrategyRuntimeMaxLegs> positions{};
  std::array<trading_core::StrategyAccountState, trading_core::kStrategyRuntimeMaxLegs> accounts{};
  std::array<StrategyInstrumentSpec, trading_core::kStrategyRuntimeMaxLegs> instruments{};
  std::array<canon::MarketType, trading_core::kStrategyRuntimeMaxLegs> markets{};
  std::array<std::uint64_t, trading_core::kStrategyRuntimeMaxLegs> revisions{};
  std::size_t legCount{0u};

  void reset(std::size_t count) noexcept;
  void refreshLeg(std::size_t leg,
                  const std::vector<detail::EngineState>& engines,
                  const std::vector<StrategyInstrumentSpec>& legInstruments) noexcept;
  void refreshDirty(const std::vector<detail::EngineState>& engines,
                    const std::vector<StrategyInstrumentSpec>& legInstruments) noexcept;
};

[[nodiscard]] trading_core::StrategyAccountState accountStateFromEngine(
    const detail::EngineState& engine) noexcept;
[[nodiscard]] std::size_t boundedRiskLegCount(std::size_t count) noexcept;
void refreshRiskAccountsFromEngines(
    const std::vector<detail::EngineState>& engines,
    std::array<trading_core::StrategyAccountState, trading_core::kStrategyRuntimeMaxLegs>& out) noexcept;
void resolveRiskVenuesForSessions(
    const BacktestStrategyConfig& cfg,
    const std::vector<detail::SessionData>& sessions,
    std::array<const BacktestStrategyVenueConfig*, trading_core::kStrategyRuntimeMaxLegs>& out) noexcept;
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

[[nodiscard]] bool runCycle(const trading_core::StrategyDescriptor& descriptor,
                            const BacktestStrategyInputPolicy& inputPolicy,
                            void* state,
                            detail::EngineState& engine,
                            const StrategyInstrumentSpec& instrument,
                            SingleLegCycleScratch& scratch,
                            const trading_core::SymbolShardConfig& shard,
                            const trading_core::ShardControlState& control,
                            std::int64_t tsNs,
                            const BacktestRunRequest& request,
                            const cxet::composite::UserDataEvent* userEvent);
[[nodiscard]] bool runCycleMulti(const trading_core::StrategyDescriptor& descriptor,
                                 const BacktestStrategyInputPolicy& inputPolicy,
                                 void* state,
                                 std::vector<detail::EngineState>& engines,
                                 const std::vector<StrategyInstrumentSpec>& legInstruments,
                                 MultiLegCycleScratch& scratch,
                                 const trading_core::SymbolShardConfig& shard,
                                 const trading_core::ShardControlState& control,
                                 std::uint32_t eventLegIndex,
                                 std::int64_t tsNs,
                                 const BacktestRunRequest& request,
                                 const cxet::composite::UserDataEvent* userEvent);
[[nodiscard]] bool runUntil(const trading_core::StrategyDescriptor& descriptor,
                            const BacktestStrategyInputPolicy& inputPolicy,
                            void* state,
                            detail::EngineState& engine,
                            const StrategyInstrumentSpec& instrument,
                            SingleLegCycleScratch& scratch,
                            const trading_core::SymbolShardConfig& shard,
                            const trading_core::ShardControlState& control,
                            std::int64_t untilTs,
                            const BacktestRunRequest& request,
                            RunUntilBoundary boundary = RunUntilBoundary::Inclusive);
[[nodiscard]] bool runUntilMulti(const trading_core::StrategyDescriptor& descriptor,
                                 const BacktestStrategyInputPolicy& inputPolicy,
                                 void* state,
                                 std::vector<detail::EngineState>& engines,
                                 const std::vector<StrategyInstrumentSpec>& legInstruments,
                                 MultiLegCycleScratch& scratch,
                                 const trading_core::SymbolShardConfig& shard,
                                 const trading_core::ShardControlState& control,
                                 std::int64_t untilTs,
                                 const BacktestRunRequest& request,
                                 RunUntilBoundary boundary = RunUntilBoundary::Inclusive);
[[nodiscard]] bool settleAfterReplay(const trading_core::StrategyDescriptor& descriptor,
                                     const BacktestStrategyInputPolicy& inputPolicy,
                                     void* state,
                                     detail::EngineState& engine,
                                     const StrategyInstrumentSpec& instrument,
                                     SingleLegCycleScratch& scratch,
                                     const trading_core::SymbolShardConfig& shard,
                                     const trading_core::ShardControlState& control,
                                     std::int64_t lastTsNs,
                                     const BacktestRunRequest& request);
[[nodiscard]] bool settleAfterReplayMulti(const trading_core::StrategyDescriptor& descriptor,
                                          const BacktestStrategyInputPolicy& inputPolicy,
                                          void* state,
                                          std::vector<detail::EngineState>& engines,
                                          const std::vector<StrategyInstrumentSpec>& legInstruments,
                                          MultiLegCycleScratch& scratch,
                                          const trading_core::SymbolShardConfig& shard,
                                          const trading_core::ShardControlState& control,
                                          std::int64_t lastTsNs,
                                          const BacktestRunRequest& request);

}  // namespace hft_backtest
