#include "engine/backtest_engine.hpp"
#include "strategy/backtest_strategy_intent_lowering.hpp"
#include "strategy/backtest_strategy_runtime.hpp"

#include "trading_core/api/execution_state/UserEventIdentity.hpp"
#include "trading_core/api/order/StrategyTransactionValidation.hpp"
#include "trading_core/strategy/StrategyMarketData.hpp"

#include <array>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

using namespace hft_backtest;
using namespace hft_backtest::detail;
using hft_backtest::strategy_intent_lowering::StrategyTransactionAdmission;

constexpr std::int64_t kBidRaw = 10'000'000'000ll;
constexpr std::int64_t kAskRaw = 10'100'000'000ll;
constexpr std::int64_t kQtyRaw = 100'000'000ll;

bool expect(bool value, std::string_view message) {
  if (value) return true;
  std::cerr << "FAIL: " << message << '\n';
  return false;
}

void setTop(EngineState& engine) noexcept {
  cxet::composite::BookTickerRuntimeV1 top{};
  top.bid.px.raw = kBidRaw;
  top.bid.qty.raw = kQtyRaw;
  top.ask.px.raw = kAskRaw;
  top.ask.qty.raw = kQtyRaw;
  cxet::composite::BboReconstructionView reconstructed{};
  reconstructed.bookTicker = top;
  reconstructed.source =
      cxet::composite::kBboReconstructionSourceBookTicker;
  reconstructed.valid = cxet::composite::kBboReconstructionFlagTrue;
  reconstructed.bidPresent = cxet::composite::kBboReconstructionFlagTrue;
  reconstructed.askPresent = cxet::composite::kBboReconstructionFlagTrue;
  trading_core::copyBookTickerSnapshot(engine.snapshot, top);
  trading_core::copyBookTickerSnapshot(engine.venueSnapshot, top);
  trading_core::copyReconstructedBboSnapshot(
      engine.snapshot, reconstructed);
  trading_core::copyReconstructedBboSnapshot(
      engine.venueSnapshot, reconstructed);
  // The transaction test observes terminal simulator admission in the same
  // cycle.  A zero limit-order latency makes the strict post-only ack due at
  // tsNs instead of leaving the record Pending under the production default.
  engine.latency.config.limitOrder = {};
  engine.latency.config.cancelOrder = {};
}

trading_core::StrategyTransactionIntent transaction(
    std::uint64_t transactionId,
    std::uint32_t routeRaw,
    std::uint64_t clientIntentId,
    std::uint64_t touchedLegMask) noexcept {
  trading_core::StrategyTransactionIntent out{};
  out.transactionId.raw = transactionId;
  out.touchedLegMask = touchedLegMask;
  out.semantic = trading_core::StrategyOrderSemantic::EntryAdd;
  out.legCount = 1u;
  out.legs[0].routeHandle =
      trading_core::ExecutionRouteHandle{routeRaw};
  out.legs[0].clientIntentId.raw = clientIntentId;
  out.legs[0].expectedFillQty.raw = kQtyRaw;
  out.legs[0].signedDeltaRaw = kQtyRaw;
  return out;
}

trading_core::OrderIntent marketOrder(
    const trading_core::StrategyTransactionIntent& owner,
    Side side) noexcept {
  trading_core::OrderIntent out{};
  out.transactionId = owner.transactionId;
  out.clientIntentId = owner.legs[0].clientIntentId;
  out.routeHandle = owner.legs[0].routeHandle;
  out.semantic = owner.semantic;
  out.side = side;
  out.orderType = canon::OrderType::Market;
  out.qty = owner.legs[0].expectedFillQty;
  return out;
}

struct ReportCapture final {
  std::array<trading_core::StrategyEmitReport, 3u> reports{};
  std::size_t count{0u};
  std::uint64_t nextClientIntentId{9'000u};
};

bool captureReport(
    void* rawState,
    const trading_core::StrategyEmitReport& report) noexcept {
  auto& capture = *static_cast<ReportCapture*>(rawState);
  if (capture.count >= capture.reports.size()) return false;
  capture.reports[capture.count++] = report;
  return true;
}

bool replayTransaction(
    void*, const trading_core::StrategyTransactionIntent&) noexcept {
  return true;
}

Id allocateClientId(void* rawState) noexcept {
  auto& capture = *static_cast<ReportCapture*>(rawState);
  Id out{};
  out.raw = capture.nextClientIntentId++;
  return out;
}

struct AsyncRejectProbeState final {
  bool proposalEmitted{false};
  bool reportApplied{false};
  bool rejectionObserved{false};
  bool completionEmitted{false};
};

trading_core::StrategyTransactionIntent asyncRejectTransaction(
    trading_core::StrategyTransactionRecordKind kind) noexcept {
  trading_core::StrategyTransactionIntent out =
      transaction(700u, 1u, 701u, 0x1u);
  out.recordKind = kind;
  return out;
}

bool asyncRejectRunCycle(
    void* rawState, trading_core::StrategyContext& context) noexcept {
  auto& state = *static_cast<AsyncRejectProbeState*>(rawState);
  if (context.hasUserEvent && context.userEvent &&
      trading_core::strategy::isUserOrderRejected(*context.userEvent)) {
    state.rejectionObserved = true;
    state.completionEmitted = context.io.pushTransaction(
        asyncRejectTransaction(
            trading_core::StrategyTransactionRecordKind::Rejected));
    return state.completionEmitted;
  }
  if (state.proposalEmitted) return true;

  const trading_core::StrategyTransactionIntent proposal =
      asyncRejectTransaction(
          trading_core::StrategyTransactionRecordKind::Proposed);
  trading_core::OrderIntent order = marketOrder(proposal, Side::Buy());
  order.orderType = canon::OrderType::Limit;
  order.price.raw = kAskRaw;
  order.limitTimeInForcePolicy =
      trading_core::LimitTimeInForcePolicy::PostOnlyStrict;
  if (!context.io.pushTransaction(proposal) ||
      !context.io.pushOrder(order)) {
    return false;
  }
  state.proposalEmitted = true;
  return true;
}

void asyncRejectCaptureState(
    const void*, trading_core::StrategyStateSnapshot& out) noexcept {
  out = trading_core::StrategyStateSnapshot{};
}

void asyncRejectRollback(
    void*, const trading_core::StrategyStateSnapshot&) noexcept {}

bool asyncRejectApplyReport(
    void* rawState,
    const trading_core::StrategyEmitReport& report) noexcept {
  auto& state = *static_cast<AsyncRejectProbeState*>(rawState);
  state.reportApplied = report.transactionId.raw == 700u &&
      report.allAccepted && report.anyAccepted &&
      report.legCount == 1u && report.legs[0].attempted &&
      report.legs[0].accepted &&
      report.legs[0].clientIntentId.raw == 701u;
  return state.reportApplied;
}

std::uint64_t asyncRejectFinalizeRecoveryReplay(void*) noexcept {
  return 0u;
}

std::size_t asyncRejectRecoveryTargets(
    const void*, std::int64_t* out, std::size_t capacity) noexcept {
  if (out) {
    for (std::size_t index = 0u; index < capacity; ++index) out[index] = 0;
  }
  return capacity;
}

void asyncRejectQuarantine(void*, std::uint64_t) noexcept {}
void asyncRejectCompleteRecovery(void*, std::uint64_t, bool) noexcept {}

Id asyncRejectAllocateClientId(void*) noexcept {
  return Id{7'999u};
}

trading_core::StrategyDescriptor asyncRejectDescriptor() noexcept {
  trading_core::StrategyDescriptor descriptor{};
  descriptor.name = "async_reject_fail_stop_probe";
  descriptor.streamCount = 1u;
  descriptor.streams[0] =
      cxet::api::market::PublicMarketDataStream::BookTicker;
  descriptor.footprint =
      trading_core::strategyRuntimeFootprint<AsyncRejectProbeState>(
          1u, 0u, 0u, 0u, 1u);
  descriptor.runCycle = &asyncRejectRunCycle;
  descriptor.captureState = &asyncRejectCaptureState;
  descriptor.rollbackCapacityCycle = &asyncRejectRollback;
  descriptor.restoreAfterOrderFailure = &asyncRejectRollback;
  descriptor.applyEmitReport = &asyncRejectApplyReport;
  descriptor.replayTransaction = &replayTransaction;
  descriptor.finalizeRecoveryReplay =
      &asyncRejectFinalizeRecoveryReplay;
  descriptor.recoveryTargets = &asyncRejectRecoveryTargets;
  descriptor.quarantineLegs = &asyncRejectQuarantine;
  descriptor.completeRecovery = &asyncRejectCompleteRecovery;
  descriptor.allocateClientId = &asyncRejectAllocateClientId;
  descriptor.internalOrderFailureRollback = true;
  return descriptor;
}

bool testAdmissionMasksArePerTransactionAndPropagateOverlap() {
  trading_core::StrategyMarketSnapshot snapshots[2]{};
  trading_core::StrategyTransactionIntent transactions[2]{
      transaction(100u, 1u, 101u, 0x1u),
      transaction(200u, 2u, 201u, 0x2u)};
  trading_core::OrderIntent orders[2]{
      marketOrder(transactions[0], Side::Buy()),
      marketOrder(transactions[1], Side::Sell())};
  trading_core::StrategyContext context{};
  context.marketData = {snapshots, 2u};
  context.tradableLegMask = 0x3u;
  context.io.orderOut = Span<trading_core::OrderIntent>(orders, 2u);
  context.io.transactionOut =
      Span<trading_core::StrategyTransactionIntent>(transactions, 2u);
  context.io.orderCount = 2u;
  context.io.transactionCount = 2u;
  if (!expect(trading_core::validStrategyTransactionalIoShape(context.io),
              "disjoint admission fixture has a valid transactional IO shape") ||
      !expect(trading_core::validStrategyTransactionBindings(context),
              "disjoint admission fixture has valid transaction bindings")) return false;

  BacktestRunRequest primaryOnly{};
  primaryOnly.tradeMode = BacktestTradeMode::PrimaryOnly;
  primaryOnly.hasPrimaryLegIndex = true;
  primaryOnly.primaryLegIndex = 0u;
  const StrategyTransactionAdmission disjoint =
      hft_backtest::strategy_intent_lowering::transactionAdmission(
          context, 2u, primaryOnly, false);

  transactions[0].touchedLegMask = 0x3u;
  transactions[0].legCount = 2u;
  transactions[0].legs[1].routeHandle =
      trading_core::ExecutionRouteHandle{2u};
  transactions[0].legs[1].signedDeltaRaw = -kQtyRaw;
  const StrategyTransactionAdmission overlapping =
      hft_backtest::strategy_intent_lowering::transactionAdmission(
          context, 2u, primaryOnly, false);

  transactions[0] = transaction(100u, 1u, 101u, 0x1u);
  const StrategyTransactionAdmission globallyBlocked =
      hft_backtest::strategy_intent_lowering::transactionAdmission(
          context, 2u, BacktestRunRequest{}, true);

  return expect(disjoint.localFaultMask == 0x2u,
                "PrimaryOnly blocks only the non-primary proposal") &&
         expect(disjoint.deferredMask == 0u,
                "disjoint primary proposal remains admitted") &&
         expect(overlapping.localFaultMask == 0x2u,
                "the directly filtered proposal remains a local fault") &&
         expect(overlapping.deferredMask == 0x1u,
                "an overlapping otherwise-valid proposal is deferred") &&
         expect(globallyBlocked.localFaultMask == 0x3u &&
                    globallyBlocked.deferredMask == 0u,
                "global order block faults each proposal independently");
}

bool testTerminalRejectStopsTransactionAndDefersOnlyOverlap() {
  std::array<EngineState, 2u> engines{};
  setTop(engines[0]);
  setTop(engines[1]);
  trading_core::StrategyMarketSnapshot snapshots[2]{
      engines[0].snapshot, engines[1].snapshot};
  trading_core::StrategyTransactionIntent transactions[3]{
      transaction(300u, 1u, 301u, 0x1u),
      transaction(400u, 1u, 401u, 0x1u),
      transaction(500u, 2u, 501u, 0x2u)};
  trading_core::OrderIntent orders[3]{
      marketOrder(transactions[0], Side::Buy()),
      marketOrder(transactions[1], Side::Sell()),
      marketOrder(transactions[2], Side::Buy())};
  orders[0].orderType = canon::OrderType::Limit;
  orders[0].price.raw = kAskRaw;
  orders[0].limitTimeInForcePolicy =
      trading_core::LimitTimeInForcePolicy::PostOnlyStrict;
  orders[2].orderType = canon::OrderType::Limit;
  orders[2].price.raw = kBidRaw;

  trading_core::StrategyContext context{};
  context.marketData = {snapshots, 2u};
  context.tradableLegMask = 0x3u;
  context.io.orderOut = Span<trading_core::OrderIntent>(orders, 3u);
  context.io.transactionOut =
      Span<trading_core::StrategyTransactionIntent>(transactions, 3u);
  context.io.orderCount = 3u;
  context.io.transactionCount = 3u;
  if (!expect(trading_core::validStrategyTransactionalIoShape(context.io),
              "terminal-reject fixture has a valid transactional IO shape") ||
      !expect(trading_core::validStrategyTransactionBindings(context),
              "terminal-reject fixture has valid transaction bindings")) return false;

  trading_core::StrategyDescriptor descriptor{};
  descriptor.applyEmitReport = &captureReport;
  descriptor.replayTransaction = &replayTransaction;
  descriptor.allocateClientId = &allocateClientId;
  ReportCapture capture{};
  const bool applied =
      hft_backtest::strategy_intent_lowering::applyTransactionCycle(
          descriptor,
          &capture,
          context,
          Span<EngineState>(engines.data(), engines.size()),
          1'000,
          StrategyTransactionAdmission{});

  return expect(!applied && capture.count == 3u,
                "failed cycle stops after all proposal outcomes are reported") &&
         expect(capture.reports[0].localFault &&
                    capture.reports[0].legCount == 1u &&
                    capture.reports[0].legs[0].attempted &&
                    !capture.reports[0].legs[0].accepted,
                "first terminal rejection is recorded") &&
         expect(capture.reports[1].deferred &&
                    !capture.reports[1].localFault &&
                    capture.reports[1].legCount == 1u &&
                    !capture.reports[1].legs[0].attempted,
                "overlapping later proposal is not attempted") &&
         expect(capture.reports[2].allAccepted &&
                    capture.reports[2].anyAccepted,
                "disjoint later proposal remains executable") &&
         expect(engines[0].orders.size() == 1u &&
                    engines[0].orders[0].id == 301u &&
                    engines[0].orders[0].status == kRecordStatusRejected,
                "overlapping proposal creates no simulated effect") &&
         expect(engines[1].orders.size() == 1u &&
                    engines[1].orders[0].id == 501u &&
                    engines[1].orders[0].status ==
                        kRecordStatusActive,
                "disjoint accepted order is outside failed-lineage cleanup");
}

bool testPartialAcceptedTransactionSchedulesCleanup() {
  std::array<EngineState, 2u> engines{};
  setTop(engines[0]);
  setTop(engines[1]);
  trading_core::StrategyMarketSnapshot snapshots[2]{
      engines[0].snapshot, engines[1].snapshot};
  trading_core::StrategyTransactionIntent proposal =
      transaction(600u, 1u, 601u, 0x3u);
  proposal.legCount = 2u;
  proposal.legs[1].routeHandle =
      trading_core::ExecutionRouteHandle{2u};
  proposal.legs[1].clientIntentId.raw = 602u;
  proposal.legs[1].expectedFillQty.raw = kQtyRaw;
  proposal.legs[1].signedDeltaRaw = -kQtyRaw;
  trading_core::OrderIntent orders[2]{
      marketOrder(proposal, Side::Buy()),
      marketOrder(proposal, Side::Sell())};
  orders[0].orderType = canon::OrderType::Limit;
  orders[0].price.raw = kBidRaw;
  orders[1].clientIntentId = proposal.legs[1].clientIntentId;
  orders[1].routeHandle = proposal.legs[1].routeHandle;
  orders[1].orderType = canon::OrderType::Limit;
  orders[1].price.raw = kBidRaw;
  orders[1].limitTimeInForcePolicy =
      trading_core::LimitTimeInForcePolicy::PostOnlyStrict;

  trading_core::StrategyContext context{};
  context.marketData = {snapshots, 2u};
  context.tradableLegMask = 0x3u;
  context.io.orderOut = Span<trading_core::OrderIntent>(orders, 2u);
  context.io.transactionOut =
      Span<trading_core::StrategyTransactionIntent>(&proposal, 1u);
  context.io.orderCount = 2u;
  context.io.transactionCount = 1u;
  if (!expect(trading_core::validStrategyTransactionalIoShape(context.io),
              "partial-admission fixture has a valid transactional IO shape") ||
      !expect(trading_core::validStrategyTransactionBindings(context),
              "partial-admission fixture has valid transaction bindings")) return false;

  trading_core::StrategyDescriptor descriptor{};
  descriptor.applyEmitReport = &captureReport;
  descriptor.replayTransaction = &replayTransaction;
  descriptor.allocateClientId = &allocateClientId;
  ReportCapture capture{};
  const bool applied =
      hft_backtest::strategy_intent_lowering::applyTransactionCycle(
          descriptor,
          &capture,
          context,
          Span<EngineState>(engines.data(), engines.size()),
          2'000,
          StrategyTransactionAdmission{});

  return expect(!applied && capture.count == 1u,
                "partial transaction reports before fail-stop") &&
         expect(capture.reports[0].localFault &&
                    capture.reports[0].anyAccepted &&
                    !capture.reports[0].allAccepted,
                "later terminal rejection preserves accepted-leg evidence") &&
         expect(engines[0].orders.size() == 2u &&
                    engines[0].orders[0].id == 601u &&
                    engines[0].orders[0].status ==
                        kRecordStatusCancelled &&
                    engines[0].orders[1].action ==
                        kRecordActionCancel &&
                    engines[0].orders[1].targetId == 601u,
                "accepted resting leg receives compensating cancel") &&
         expect(engines[1].orders.size() == 1u &&
                    engines[1].orders[0].id == 602u &&
                    engines[1].orders[0].status ==
                        kRecordStatusRejected,
                "second leg remains the terminal rejection");
}

bool testAsyncRejectedTransactionCompletionFailsBacktestClosed() {
  constexpr std::int64_t kStartNs = 10'000'000ll;
  EngineState engine{};
  engine.exchange = canon::kExchangeIdBinance;
  engine.market = canon::kMarketTypeFutures;
  setTop(engine);
  // setTop makes immediate admission deterministic for the focused lowering
  // tests above. Restore the product default here: a strict PostOnly order is
  // accepted as pending, then rejected asynchronously 1000 us later.
  engine.latency = BacktestLatencyRuntime{};
  engine.lastOrderPathHotMono.raw =
      static_cast<std::uint64_t>(kStartNs - 1ll);
  setStrategySourceDeliveryTimestamp(
      engine,
      cxet::api::market::PublicMarketDataStream::BookTicker,
      kStartNs);

  StrategyInstrumentSpec instrument{};
  instrument.tickSizeRaw = 100'000'000ll;
  instrument.stepSizeRaw = 100'000'000ll;
  instrument.tickGrid =
      trading_core::prepareFastStepGrid(instrument.tickSizeRaw);
  instrument.qtyStepGrid =
      trading_core::prepareFastStepGrid(instrument.stepSizeRaw);
  trading_core::StrategyDescriptor descriptor = asyncRejectDescriptor();
  if (!expect(trading_core::strategyDescriptorContractValid(descriptor),
              "async-reject probe descriptor satisfies the current strategy contract")) return false;
  const BacktestStrategyInputPolicy inputPolicy =
      resolveBacktestStrategyInputPolicy(
          descriptor, trading_core::StrategyParamStorage{});
  AsyncRejectProbeState state{};
  SingleLegCycleScratch scratch{};
  trading_core::SymbolShardConfig shard{};
  trading_core::ShardControlState control{};
  BacktestRunRequest request{};

  trading_core::StrategyTransactionIntent proposal =
      asyncRejectTransaction(
          trading_core::StrategyTransactionRecordKind::Proposed);
  trading_core::OrderIntent order = marketOrder(proposal, Side::Buy());
  order.orderType = canon::OrderType::Limit;
  order.price.raw = kAskRaw;
  order.limitTimeInForcePolicy =
      trading_core::LimitTimeInForcePolicy::PostOnlyStrict;
  trading_core::StrategyContext context{};
  context.marketData = {&engine.snapshot, 1u};
  context.tradableLegMask = 0x1u;
  context.io.orderOut = Span<trading_core::OrderIntent>(&order, 1u);
  context.io.transactionOut =
      Span<trading_core::StrategyTransactionIntent>(&proposal, 1u);
  context.io.orderCount = 1u;
  context.io.transactionCount = 1u;
  state.proposalEmitted = true;

  // Admission/lowering is the behavior under test here. Bypass the separate
  // order-path warmup gate, then let runUntil consume the real asynchronous
  // rejection and strategy completion record.
  const bool admitted =
      hft_backtest::strategy_intent_lowering::applyTransactionCycle(
          descriptor, &state, context,
          Span<EngineState>(&engine, 1u), kStartNs,
          StrategyTransactionAdmission{});
  if (!expect(admitted, "async-reject proposal cycle is admitted") ||
      !expect(!engine.pending.empty(),
              "async-reject proposal creates pending simulator work")) return false;
  const std::int64_t rejectTsNs = engine.pending.front().activeTsNs;
  const bool continued = runUntil(
      descriptor, inputPolicy, &state, engine, instrument, scratch,
      shard, control, rejectTsNs, request);

  return expect(!continued,
                "asynchronous rejected completion fail-stops runUntil") &&
         expect(state.proposalEmitted && state.reportApplied,
                "pending admission report is delivered before rejection") &&
         expect(state.rejectionObserved && state.completionEmitted,
                "strategy completion output is consumed before fail-stop") &&
         expect(engine.deliveredUserEvents == 1u,
                "asynchronous rejection evidence is delivered once") &&
         expect(engine.orders.size() == 1u &&
                    engine.orders[0].status == kRecordStatusRejected,
                "default-latency strict PostOnly order reaches rejection");
}

}  // namespace

int main() {
  bool ok = true;
  ok = testAdmissionMasksArePerTransactionAndPropagateOverlap() && ok;
  ok = testTerminalRejectStopsTransactionAndDefersOnlyOverlap() && ok;
  ok = testPartialAcceptedTransactionSchedulesCleanup() && ok;
  ok = testAsyncRejectedTransactionCompletionFailsBacktestClosed() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
