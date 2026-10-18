#pragma once
#include "trading_core/Execution/Orders.hpp"
#include "trading_core/Contracts/State/Orders/OrderEvidenceView.hpp"
#include "trading_core/Contracts/Intents/Intent.hpp"
#include "trading_core/Contracts/Intents/OrderState.hpp"
#include <array>
#include <optional>
namespace hft_backtest::detail {
struct EngineState;
struct OpenOrder;
struct AggressiveFillPlan;
inline constexpr std::size_t kSimulationOrderEvidenceCapacity = 1024u;
inline constexpr unsigned char kSimulationServiceOrder = 1u;
inline constexpr unsigned char kSimulationWarmupOrder = 2u;
inline constexpr unsigned char kSimulationWarmupUnwindOrder = 4u;
struct SpotOrderReservation {
  std::int64_t estimatedPriceE8{}, venuePriceE8{}, observedQuoteE8{}, venueQuoteE8{}, observedBaseE8{}, venueBaseE8{};
  std::int64_t feeBpsE8{};
};
struct SimulationOrderEvidenceOwner {
  std::array<trading_core::execution::OrderRecord, kSimulationOrderEvidenceCapacity> records{};
  std::array<trading_core::execution::OrderIndexSlot, kSimulationOrderEvidenceCapacity> index{};
  std::array<trading_core::StrategyOrderEvidence, kSimulationOrderEvidenceCapacity> identities{};
  std::array<trading_core::StrategyOrderState, kSimulationOrderEvidenceCapacity> localStates{};
  std::array<unsigned char, kSimulationOrderEvidenceCapacity> serviceFlags{};
  std::array<SpotOrderReservation, kSimulationOrderEvidenceCapacity> spotReservations{};
  std::optional<trading_core::execution::OrderStore> store{};
  trading_core::execution::ExecutionBinding binding{};
  std::uint64_t nextCommandCounter{1u};
  bool ready{false};
};
[[nodiscard]] bool prepareSimulationOrderEvidence(EngineState&) noexcept;
[[nodiscard]] trading_core::StrategyOrderEvidenceView simulationOrderEvidenceView(const EngineState&) noexcept;
[[nodiscard]] bool installSimulationOrderIntent(EngineState&, const trading_core::OrderIntent&) noexcept;
[[nodiscard]] bool markSimulationWarmupIntent(EngineState&, Id, bool unwind) noexcept;
[[nodiscard]] bool settleSimulationOrderSubmit(EngineState&, Id, bool sent) noexcept;
[[nodiscard]] std::uint64_t simulationCommandId(const EngineState&, Id, std::uint32_t contextIndex) noexcept;
[[nodiscard]] bool beginSimulationCancel(EngineState&, Id target, Id cancelLocalId = {}) noexcept;
[[nodiscard]] bool deliverSimulationOrderEvent(EngineState&, std::size_t index, std::int64_t nowNs) noexcept;
[[nodiscard]] bool reserveSpotObservedIntent(EngineState&, const trading_core::OrderIntent&) noexcept;
[[nodiscard]] bool reserveSpotVenueOrder(EngineState&, const OpenOrder&) noexcept;
[[nodiscard]] bool spotVenueFillAffordable(const EngineState&, const OpenOrder&,
    std::int64_t priceE8, std::int64_t qtyE8, std::int64_t feeBpsE8) noexcept;
[[nodiscard]] bool spotVenueFillPlanAffordable(const EngineState&, const OpenOrder&,
    const AggressiveFillPlan&, std::int64_t feeBpsE8) noexcept;
void updateSpotVenueReservation(EngineState&, const OpenOrder&, std::int64_t remainingQtyE8) noexcept;
}
