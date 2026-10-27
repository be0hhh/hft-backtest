#pragma once

#include "capital/Sizing/PreparedPairSizingTypes.hpp"

#include "../../Engine/BacktestEngine.hpp"
#include "trading_core/Strategy/StrategyInstance.hpp"
#include "trading_core/Strategy/Orders/Execution/PairStrategyInputs.hpp"
#include "trading_core/Strategy/Orders/Execution/StrategyExecution.hpp"
#include "trading_core/Algorithm/Signals/Basis/DeliveryValuation.hpp"

#include <array>
#include <memory>
#include <string>
#include <vector>

namespace hft_backtest {
class BacktestCanonicalPairSelection;

// Captured typed evidence; symbol spelling never supplies economic identity,
// native lots, settlement or currency conversion authority.
struct BacktestCanonicalSource final {
  cxet::api::InstrumentUniverseRow instrument{};
  capital::sizing::PairSizingLeg sizing{};
  bool feesKnown{};
};

// Same-owner immutable cut. The caller retains the underlying validated curve
// storage for the current lexical cycle. Nothing here is copied into the actor.
struct BacktestCanonicalPairReferences final {
  const trading_core::DeliveryValuationReference* delivery{};
  const trading_core::DeliveryValuationCentralBankReference* centralBank{};
  std::array<std::uint32_t, 2u> sourceIds{};
  std::array<TimeNs, 2u> quoteReceiveUtc{};
};

class BacktestCanonicalExecutionAdapter final {
 public:
  [[nodiscard]] bool bindCold(const trading_core::StrategyDescriptor&,
      const trading_core::StrategyParamStorage&, void* initializedState,
      std::span<const trading_core::StrategyInstrumentSpec>,
      std::span<const BacktestCanonicalSource>, detail::EngineView,
      std::uint64_t nowUtc, std::string& error,
      const trading_core::StrategyRuntimeConfiguration& configuration = {}) noexcept;
  [[nodiscard]] bool prepareCycle(trading_core::StrategyContext&,
      detail::EngineView, const BacktestCanonicalPairReferences&,
      trading_core::strategy::PairStrategyInputs&) noexcept;
  // A false canonical callback can mean idle/no output. Only protocol and
  // output validation failures stop replay.
  [[nodiscard]] bool decideAndExecute(trading_core::StrategyContext&) noexcept;
  [[nodiscard]] bool dispatchAndCommit(trading_core::StrategyContext&,
      detail::EngineView, std::int64_t tsNs) noexcept;
  [[nodiscard]] bool released(std::size_t leg, std::uint32_t contextIndex, Id,
      std::uint64_t deliveredCut) const noexcept;
  [[nodiscard]] Span<trading_core::StrategyCommandResult> commandResults() noexcept {
    return {commandResults_.data(), commandResults_.size()};
  }
  [[nodiscard]] bool bound() const noexcept { return descriptor_ != nullptr; }
  [[nodiscard]] bool bindSelectionOwner(BacktestCanonicalPairSelection&, std::uint32_t index) noexcept;
  [[nodiscard]] TimeNs nextWakeMono() const noexcept;
  [[nodiscard]] trading_core::StrategyResult result() const noexcept { return result_; }

 private:
  [[nodiscard]] bool bindGenericCold(const trading_core::StrategyDescriptor&,
      const trading_core::StrategyParamStorage&, void*,
      std::span<const trading_core::StrategyInstrumentSpec>,
      std::span<const BacktestCanonicalSource>, detail::EngineView,
      std::uint64_t, std::string&) noexcept;
  trading_core::StrategyInstance instance_{};
  trading_core::StrategyRetainedInstrumentState retained_{};
  struct PairResources {
    trading_core::strategy::PairExecutionPool pool{};
    std::array<trading_core::strategy::PairExecutionPool::Row, 64u> rows{};
  };
  std::unique_ptr<PairResources> pairResources_{};
  std::vector<BacktestCanonicalSource> sources_{};
  std::vector<trading_core::strategy::StrategyExecutionLeg> legs_{};
  std::vector<trading_core::strategy::StrategyExecutionSlot> slots_{};
  std::vector<trading_core::strategy::StrategyExecutionLegGate> gates_{};
  std::vector<trading_core::StrategyLegInputView> inputViews_{};
  std::vector<trading_core::OrderCommissionSchedule> feeSchedules_{};
  std::uint64_t feeQualificationMono_{};
  std::uint64_t marketMaximumAgeNs_{};
  std::vector<trading_core::strategy::StrategyDispatchObservation> feedback_{};
  std::vector<trading_core::StrategyCommandResult> commandResults_{};
  std::size_t feedbackCount_{};
  std::size_t resultCount_{};
  capital::sizing::PreparedPairLotsSizing preparedLots_{};
  trading_core::StrategyResolvedPairParameters parameters_{};
  trading_core::StrategyParamStorage params_{};
  const trading_core::StrategyDescriptor* descriptor_{};
  BacktestCanonicalPairSelection* selection_{};
  std::uint32_t selectionIndex_{};
  std::vector<std::uint64_t> consumedCuts_{};
  std::array<std::uint64_t, 2u> privateFillCuts_{};
  std::array<std::uint64_t, 2u> finalFillCommands_{};
  std::uint32_t filledOrders_{};
  trading_core::StrategyResult result_{};
  std::uint32_t contextIndex_{};
  bool cyclePrepared_{}, dispatchCommitted_{}, pairPolicy_{};
};

} // namespace hft_backtest
