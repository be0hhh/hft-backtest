#pragma once
#include "../Input/Binary/Internal/BinaryCorpusHelpers.hpp"
#include "../Engine/BacktestEngine.hpp"
#include "hftrec/CorpusContract/CorpusCut.hpp"
namespace hft_backtest::detail {
class SimulationInput final {
 public:
  struct Source {
    hftrec::corpus::BinaryMarketSelection selection;
    binary_corpus::BinaryDecodeState decoder;
    std::uint16_t requestedMask{}, requiredMask{}, selectedMask{}, unhealthyMask{};
  };
  std::vector<SessionData> sessions;
  std::vector<Source> sources;
  std::vector<std::size_t> sourceLegs;
  std::vector<hftrec::corpus::BinaryMarketRecord> transaction;
  std::size_t transactionLeg{};
  std::size_t priorLeg{std::numeric_limits<std::size_t>::max()};
  [[nodiscard]] Status prepare(const hftrec::corpus::CorpusCut&,
      std::span<const BacktestCorpusSourceRequest>,const SessionLoadPlan&,std::string&);
  [[nodiscard]] Status append(const hftrec::corpus::BinaryMarketRecord&,
      std::vector<TimelineEvent>&,std::string&);
 private:
  [[nodiscard]] Status decode(std::vector<TimelineEvent>&,std::string&);
};
}
