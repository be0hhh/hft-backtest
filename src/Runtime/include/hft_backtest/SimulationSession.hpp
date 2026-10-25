#pragma once

#include "hft_backtest/Backtest.hpp"
#include "hftrec/CorpusContract/CorpusCut.hpp"
#include <memory>
#include <span>
#include "cxet/Primitives/Buf/StringView.hpp"

namespace hft_backtest {

// Synchronous cold output. Views are borrowed only for the callback duration.
// false fences further advancement; the caller must preserve already accepted rows.
using SimulationOutputCallback = bool (*)(std::uint32_t sourceId, std::uint64_t eventMonotonicNs,
    cxet::StringView stream, cxet::StringView jsonLine, void* userData) noexcept;
using SimulationMetricsCallback = bool (*)(const BacktestRunResult&,
    std::uint64_t eventMonotonicNs, std::int64_t eventReceiveRealtimeNs, std::int64_t equityE8,
    std::int64_t cumulativeReturnBpsE8, std::uint32_t valuationQuoteAssetId,
    void* userData) noexcept;
struct SimulationOutputSink {
  SimulationOutputCallback write{};
  void* userData{};
  SimulationMetricsCallback metrics{};
};
struct SimulationLimits {
  std::size_t maximumSources{256u};
  std::size_t maximumContexts{128u};
  std::uint64_t maximumStateBytes{256ull*1024u*1024u};
  // Cold validated prefix maxima, indexed by captured sourceId-1. Copied at
  // prepare; values describe admitted rows and never imply native continuity.
  std::span<const std::uint32_t> maximumDepthCoverageSourcesBySource{};
  std::span<const std::uint16_t> sourceUnhealthyChannelMasksAtBegin{};
  std::span<const std::uint16_t> sourceAdministrativeUnhealthyChannelMasksAtBegin{};
  std::span<const std::uint16_t> sourceRecoveryPendingChannelMasksAtBegin{};
};
struct SimulationAccountSummary {
  std::uint32_t accountId{},quoteAssetId{};
  std::uint16_t exchange{};
  std::uint8_t market{};
  std::int64_t initialBalanceE8{},walletBalanceE8{},observedWalletBalanceE8{},equityE8{};
  std::int64_t grossRealizedPnlE8{},netRealizedPnlE8{},unrealizedPnlE8{},feesPaidE8{};
};
struct SimulationAdvance {
  std::uint64_t recordsAccepted{};
  std::uint64_t eventsProcessed{};
  std::uint64_t safeMonotonicNs{};
  std::uint64_t terminalMonotonicNs{};
  bool paused{};
  bool finished{};
};

// One independent whole-market portfolio/strategy variant. No native sessions.
// Input chunks may split a captured depth transaction; completion is buffered
// within the canonical transaction bound. Source metadata is copied at prepare.
class HFT_BACKTEST_API SimulationSession final {
 public:
  SimulationSession() noexcept;
  ~SimulationSession();
  SimulationSession(const SimulationSession&) = delete;
  SimulationSession& operator=(const SimulationSession&) = delete;
  [[nodiscard]] Status prepare(const BacktestRunRequest& request,
      const hftrec::corpus::CorpusCut& cut, SimulationOutputSink sink = {}, SimulationLimits limits = {}) noexcept;
  [[nodiscard]] Status advance(
      std::span<const hftrec::corpus::BinaryMarketRecord> records,
      // Exclusive authoritative prefix: no future input has receive < safe.
      std::uint64_t safeMonotonicNs, SimulationAdvance& progress) noexcept;
  // Append cold topology/reference facts while preserving all resident actors.
  [[nodiscard]] Status updateCut(const hftrec::corpus::CorpusCut& cut,
      std::span<const std::uint32_t> maximumDepthCoverageSourcesBySource = {}) noexcept;
  void pause() noexcept;
  void resume() noexcept;
  // End input without manufacturing orders, cancellation or position closure.
  [[nodiscard]] Status finish() noexcept;
  [[nodiscard]] std::span<const SimulationAccountSummary> accountSummaries() const noexcept;
  [[nodiscard]] bool returnSinceEquityBpsE8(std::int64_t previousEquity,std::int64_t& out) const noexcept;
  [[nodiscard]] std::uint32_t valuationQuoteAssetId() const noexcept;
  [[nodiscard]] std::int64_t equityE8() const noexcept;
  [[nodiscard]] std::int64_t cumulativeReturnBpsE8() const noexcept;
  [[nodiscard]] const BacktestRunResult& result() const noexcept;
 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};
}  // namespace hft_backtest
