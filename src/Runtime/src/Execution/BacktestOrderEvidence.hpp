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
  bool observedReducing{}, venueReducing{};
  Side collateralSide{};
};
struct SimulationDerivativeCollateral {
  std::int64_t initialMarginE8{}, basisQuantityE8{};
  std::uint32_t currency{};
  std::int64_t observedProtectedE8{}, venueProtectedE8{};
  bool known{}, authorityCurrent{}, observedUnavailable{}, venueUnavailable{};
};
struct SimulationOrderEvidenceOwner {
  std::array<trading_core::execution::OrderRecord, kSimulationOrderEvidenceCapacity> records{};
  std::array<trading_core::execution::OrderIndexSlot, kSimulationOrderEvidenceCapacity> index{};
  std::array<trading_core::StrategyOrderEvidence, kSimulationOrderEvidenceCapacity> identities{};
  std::array<trading_core::StrategyOrderState, kSimulationOrderEvidenceCapacity> localStates{};
  std::array<unsigned char, kSimulationOrderEvidenceCapacity> serviceFlags{};
  std::array<SpotOrderReservation, kSimulationOrderEvidenceCapacity> spotReservations{};
  SimulationDerivativeCollateral collateral{};
  std::array<std::uint64_t, kSimulationOrderEvidenceCapacity> deliveredCuts{};
  std::array<trading_core::execution::OwnedInventoryQuantity, 64u> retiredOwned{};
  std::optional<trading_core::execution::OrderStore> store{};
  trading_core::execution::ExecutionBinding binding{};
  std::uint64_t nextCommandCounter{1u};
  std::uint64_t deliveryCut{0u};
  bool ready{false};
};
// Called only at an owner scheduler boundary, after all actors consumed the
// delivered private/account cut and committed their dispatch. The callback
// proves every actor for this context has released the exact intent and that
// no retained dispatch feedback borrows it; terminal status alone is no proof.
struct SimulationEvidenceRetirementProof {
  void* owner{};
  bool (*released)(void*, std::uint32_t contextIndex, Id,
                   std::uint64_t deliveredCut) noexcept{};
};
[[nodiscard]] std::size_t reclaimSimulationOrderEvidence(
    EngineState&, SimulationEvidenceRetirementProof) noexcept;
[[nodiscard]] bool prepareSimulationOrderEvidence(EngineState&) noexcept;
[[nodiscard]] bool configureSimulationDerivativeCollateral(EngineState&,
    std::int64_t initialMarginE8, std::int64_t basisQuantityE8,
    std::uint32_t currency) noexcept;
void setSimulationDerivativeAuthorityCurrent(EngineState&, bool current) noexcept;
void refreshSimulationDerivativeCollateral(EngineState&, bool observed) noexcept;
[[nodiscard]] bool reserveDerivativeOrder(EngineState&, SpotOrderReservation&,
    Side, std::int64_t quantity, std::int64_t price, bool observed) noexcept;
[[nodiscard]] bool derivativeVenueFillAffordable(const EngineState&,
    const SpotOrderReservation&, const OpenOrder&, std::int64_t price,
    std::int64_t quantity, std::int64_t actualFeeE8) noexcept;
[[nodiscard]] bool derivativeVenueFillPlanAffordable(const EngineState&,
    const SpotOrderReservation&, const OpenOrder&, const AggressiveFillPlan&,
    std::int64_t feeBpsE8) noexcept;
void updateDerivativeOrderReservation(EngineState&, SpotOrderReservation&,
    Side, std::int64_t remaining, bool observed) noexcept;
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
    std::int64_t priceE8, std::int64_t qtyE8, std::int64_t actualFeeE8) noexcept;
[[nodiscard]] bool spotVenueFillPlanAffordable(const EngineState&, const OpenOrder&,
    const AggressiveFillPlan&, std::int64_t feeBpsE8) noexcept;
void updateSpotVenueReservation(EngineState&, const OpenOrder&, std::int64_t remainingQtyE8) noexcept;
}
