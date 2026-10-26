#pragma once
#include "SimulationInput.hpp"
#include "../Strategy/Execution/BacktestCanonicalPairAdapter.hpp"
#include "hftrec/CorpusContract/RecorderReference.hpp"
#include "trading_core/Algorithm/Signals/Basis/DeliveryValuationReference.hpp"
namespace hft_backtest::detail {
struct SimulationCentralBank final {
  trading_core::DeliveryValuationCentralBankReference reference;
  std::vector<trading_core::CentralBankRatePoint> current, previous;
  std::uint64_t fingerprint{};
};
struct SimulationReferences final {
  std::vector<hftrec::corpus::RecorderReferenceSnapshot> snapshots;
  std::uint64_t committedMonotonicNs{};
  [[nodiscard]] Status update(const hftrec::corpus::CorpusCut&,std::string&);
  [[nodiscard]] Status listing(const hftrec::corpus::BinaryMarketSource&,cxet::api::InstrumentUniverseRow&,bool active,std::string&) const;
  [[nodiscard]] bool instrumentAvailable(const hftrec::corpus::BinaryMarketSource&,std::uint64_t nowMono) const;
  [[nodiscard]] bool instrument(const hftrec::corpus::BinaryMarketSource&,
      trading_core::StrategyInstrumentSpec&,BacktestCanonicalSource&,std::uint64_t& visibleMono,std::string&,std::uint64_t nowMono=0u) const;
  [[nodiscard]] bool centralBank(SimulationCentralBank&,std::uint64_t nowMono,std::uint64_t nowUtc,std::string&) const;
  [[nodiscard]] bool delivery(std::span<const std::size_t>,const SimulationInput&,
      trading_core::DeliveryValuationReference&,std::uint64_t nowMono,std::uint64_t nowUtc,std::string&) const;
};
}
