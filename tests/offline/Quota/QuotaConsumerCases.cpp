#include "OfflineCase.hpp"
#include "Engine/BacktestEngineSetup.hpp"
#include "Strategy/Execution/BacktestStrategyIntentLowering.hpp"
#include "Strategy/BacktestStrategyRuntime.hpp"

#include <array>
#include <memory>

namespace {
using namespace hft_backtest;

std::unique_ptr<detail::EngineState> fundedCashQuotaEngine() {
  auto engine = std::make_unique<detail::EngineState>();
  const auto nativeProduct = engine->market.raw;
  engine->accountingModel = detail::SimulationAccountingModel::Cash;
  BacktestRunRequest request{}; request.initialBalanceE8 = 100'000'000;
  request.hasMakerFeeBps = request.hasTakerFeeBps = true;
  detail::initializeAccounting(*engine, request);
  CXET_CHECK(engine->market.raw == nativeProduct);
  return engine;
}

BacktestRateLimitSchedule policy() {
  BacktestRateLimitSchedule result{};
  BacktestQuotaBucketConfig bucket{};
  bucket.kind = trading_core::RateLimitBucketKind::Orders;
  bucket.limit = 1;
  bucket.intervalNs = 1000;
  result.buckets.push_back(bucket);
  for (const auto kind : {trading_core::RateLimitActionKind::LimitOrder,
                          trading_core::RateLimitActionKind::CancelOrder}) {
    trading_core::RateLimitActionConfig action{};
    action.action = kind;
    action.costCount = 1;
    action.costs[0] = {bucket.kind, 1};
    result.actions.push_back(action);
  }
  return result;
}

trading_core::OrderIntent order(std::uint64_t id) {
  trading_core::OrderIntent result{};
  result.clientIntentId.raw = id;
  result.orderType = canon::OrderType::Limit;
  result.side = Side::Buy();
  result.price.raw = 100;
  result.qty.raw = 1;
  return result;
}

void submit(detail::EngineState& engine, std::uint64_t id, std::int64_t now) {
  std::array<trading_core::OrderIntent, 1> orders{order(id)};
  trading_core::StrategyIo io{};
  const trading_core::StrategyIoStorage ioStorage{orders.data(), nullptr, nullptr, nullptr, nullptr};
  CXET_CHECK(io.bind(ioStorage, orders.size(), 0u, 0u, 0u, 0u));
  io.orderCount = 1;
  detail::applyIntents(engine, io, now, 1000000, 1000000);
}

void symbolsOnOneVirtualAccountShareOnePermitLedger() {
  auto first = fundedCashQuotaEngine();
  auto second = fundedCashQuotaEngine();
  detail::SharedAccountState account;
  account.accountingModel = detail::SimulationAccountingModel::Cash;
  account.initialBalanceE8 = account.walletBalanceE8 = account.observedWalletBalanceE8 = 100'000'000;
  account.observedInitialized = true;
  first->sharedAccount = second->sharedAccount = &account;
  std::string error;
  CXET_CHECK(applyRateLimitSchedule(*first, policy(), error));
  submit(*first, 1u, 100);
  submit(*second, 1u, 101);
  CXET_CHECK(first->orders.back().status == detail::kRecordStatusPending);
  CXET_CHECK(second->orders.back().status == detail::kRecordStatusRejected);
  CXET_CHECK(second->rateLimitRejectedOrders == 1u);
}

void unsupportedIocDoesNotBecomeRestingOrder() {
  auto engine = fundedCashQuotaEngine();
  std::string error;
  CXET_CHECK(applyRateLimitSchedule(*engine, policy(), error));
  std::array<trading_core::OrderIntent, 1> orders{order(1u)};
  orders[0].limitTimeInForcePolicy = trading_core::LimitTimeInForcePolicy::Ioc;
  trading_core::StrategyIo io{};
  const trading_core::StrategyIoStorage ioStorage{orders.data(), nullptr, nullptr, nullptr, nullptr};
  CXET_CHECK(io.bind(ioStorage, orders.size(), 0u, 0u, 0u, 0u));
  io.orderCount = 1u;
  detail::applyIntents(*engine, io, 100, 1000000, 1000000);
  CXET_CHECK(engine->orders.size() == 1u);
  CXET_CHECK(engine->orders[0].status == detail::kRecordStatusRejected);
  CXET_CHECK(engine->pending.empty());
  for (const auto& active : engine->open) CXET_CHECK(!active.state.valid);
  // Rejection precedes quota reservation: the one allowed GTC still fits.
  submit(*engine, 2u, 101);
  CXET_CHECK(engine->orders.back().status == detail::kRecordStatusPending);
}

void depletedQuotaRefusesOrderAndCancel() {
  auto engine = fundedCashQuotaEngine();
  std::string error;
  CXET_CHECK(!applyRateLimitSchedule(*engine, BacktestRateLimitSchedule{}, error));
  CXET_CHECK(error.find("complete explicit buckets/actions") != std::string::npos);
  auto incomplete = policy();
  incomplete.actions[0].costCount = 0;
  CXET_CHECK(!applyRateLimitSchedule(*engine, incomplete, error));
  CXET_CHECK(applyRateLimitSchedule(*engine, policy(), error));
  submit(*engine, 1, 100);
  CXET_CHECK(engine->open[0].state.valid);
  submit(*engine, 2, 101);
  CXET_CHECK(engine->orders.back().status == detail::kRecordStatusRejected);
  CXET_CHECK(engine->rateLimitRejectedOrders == 1);
  std::array<trading_core::CancelIntent, 1> cancels{};
  cancels[0].clientIntentId.raw = 3;
  cancels[0].targetClientIntentId.raw = 1;
  trading_core::StrategyIo io{};
  const trading_core::StrategyIoStorage ioStorage{nullptr, cancels.data(), nullptr, nullptr, nullptr};
  CXET_CHECK(io.bind(ioStorage, 0u, cancels.size(), 0u, 0u, 0u));
  io.cancelCount = 1;
  detail::applyIntents(*engine, io, 102, 1000000, 1000000);
  CXET_CHECK(engine->orders.back().status == detail::kRecordStatusRejected);
  CXET_CHECK(engine->rateLimitRejectedCancels == 1);
  CXET_CHECK(!engine->open[0].state.cancelPending);
  CXET_CHECK(engine->pending.size() == 1);
  auto disabled = fundedCashQuotaEngine();
  BacktestRateLimitSchedule off{};
  off.enabled = false;
  CXET_CHECK(applyRateLimitSchedule(*disabled, off, error));
  submit(*disabled, 4, 100);
  submit(*disabled, 5, 101);
  CXET_CHECK(disabled->open[0].state.valid && disabled->open[1].state.valid);
  CXET_CHECK(disabled->rateLimitRejectedOrders == 0);
}

void fixedWindowRestoresQuotaExactlyAtBoundary() {
  auto engine = fundedCashQuotaEngine();
  std::string error;
  CXET_CHECK(applyRateLimitSchedule(*engine, policy(), error));
  submit(*engine, 1, 100);
  submit(*engine, 2, 999);
  CXET_CHECK(engine->orders.back().status == detail::kRecordStatusRejected);
  submit(*engine, 3, 1000);
  CXET_CHECK(engine->orders.back().status == detail::kRecordStatusPending);
  CXET_CHECK(engine->open[1].state.clientIntentId.raw == 3);
  CXET_CHECK(engine->rateLimitRejectedOrders == 1);
}

void unsupportedReplaceRefusesWholeCycleWithoutQuotaCharge() {
  auto engine = fundedCashQuotaEngine();
  std::string error;
  CXET_CHECK(applyRateLimitSchedule(*engine, policy(), error));
  std::array<trading_core::StrategyCommandIntent, 2> commands{};
  trading_core::StrategyContext context{};
  const trading_core::StrategyIoStorage ioStorage{nullptr, nullptr, commands.data(), nullptr, nullptr};
  CXET_CHECK(context.io.bind(ioStorage, 0u, 0u, commands.size(), 0u, 0u));
  CXET_CHECK(context.io.pushOrderedOrder(order(1)));
  auto replacement = order(3);
  trading_core::CancelIntent cancel{};
  cancel.clientIntentId.raw = 2;
  cancel.targetClientIntentId.raw = 1;
  cancel.targetOrderType = canon::OrderType::Limit;
  CXET_CHECK(context.io.pushReplace(cancel, replacement));
  CXET_CHECK(!strategy_intent_lowering::validateOutput(context));
  CXET_CHECK(engine->orders.empty() && engine->pending.empty());
  submit(*engine, 4, 100);
  CXET_CHECK(engine->open[0].state.clientIntentId.raw == 4);
  CXET_CHECK(engine->rateLimitRejectedOrders == 0);
}

void observationOutputReplacesUsedBucketsAndClearsMissingCounts() {
  detail::BacktestQuota quota;
  detail::BacktestQuota absent;
  std::string error;
  CXET_CHECK(quota.configure(policy(), error));
  auto book = std::make_unique<trading_core::StrategyRateLimitBook>();
  const trading_core::ExecutionRouteHandle route{3u};
  quota.observationBook(route, canon::kExchangeIdUnknown, canon::kMarketTypeSpot,
                        2u, TimeNs{100u}, *book);
  CXET_CHECK(book->count == 1u && book->actionCount == 2u);
  CXET_CHECK(quota.admit(trading_core::RateLimitActionKind::LimitOrder, TimeNs{100u}).ok());
  quota.observationBook(route, canon::kExchangeIdUnknown, canon::kMarketTypeSpot,
                        2u, TimeNs{101u}, *book);
  CXET_CHECK(book->buckets[0].used == 1 && book->buckets[0].remaining == 0);
  quota.observationBook(route, canon::kExchangeIdUnknown, canon::kMarketTypeSpot,
                        2u, TimeNs{1000u}, *book);
  CXET_CHECK(book->buckets[0].used == 0 && book->buckets[0].remaining == 1);
  CXET_CHECK(book->buckets[0].routeHandle.raw == route.raw && book->buckets[0].apiSlot == 2u);
  CXET_CHECK(book->actionRoutes[0].raw == route.raw && book->actionRoutes[1].raw == route.raw);
  absent.observationBook(route, canon::kExchangeIdUnknown, canon::kMarketTypeSpot,
                         2u, TimeNs{1001u}, *book);
  CXET_CHECK(book->count == 0u && book->actionCount == 0u);
  CXET_CHECK(book->find(route, trading_core::RateLimitBucketKind::Orders) == nullptr);
  CXET_CHECK(book->action(trading_core::RateLimitActionKind::LimitOrder, route) == nullptr);
}

struct RateLimitProbe {
  const trading_core::StrategyRateLimitBook* book{};
  std::uint64_t cycles{};
};

trading_core::StrategyDescriptor rateLimitProbeDescriptor(std::uint8_t legs) {
  trading_core::StrategyDescriptor descriptor;
  descriptor.name = "rate_limit_snapshot_probe";
  descriptor.primaryStream = cxet::api::market::PublicMarketDataStream::BookTicker;
  descriptor.streamCount = 1u;
  descriptor.streams[0] = descriptor.primaryStream;
  descriptor.domainMask = trading_core::StrategyDomainMask::All;
  descriptor.minLegs = descriptor.maxLegs = legs;
  descriptor.universeTopology = legs == 1u
      ? trading_core::StrategyUniverseTopology::SingleSource
      : trading_core::StrategyUniverseTopology::ExplicitGroup;
  descriptor.stateDiscipline = trading_core::StrategyStateDiscipline::Snapshot;
  descriptor.footprint = trading_core::strategyRuntimeFootprint<RateLimitProbe>(0u, 0u, 0u, 0u, 0u);
  descriptor.runCycle = +[](void* state, trading_core::StrategyContext& context) noexcept {
    auto& probe = *static_cast<RateLimitProbe*>(state);
    probe.book = context.state.account.rateLimits;
    ++probe.cycles;
    return true;
  };
  descriptor.captureState = +[](const void* state, trading_core::StrategyStateSnapshot& out) noexcept {
    out = {};
    out.cycles = static_cast<const RateLimitProbe*>(state)->cycles;
  };
  return descriptor;
}

void singleCycleSnapshotIsBorrowedOnlyWhenRequested() {
  auto engine = std::make_unique<detail::EngineState>();
  auto scratch = std::make_unique<SingleLegCycleScratch>();
  RateLimitProbe probe{};
  const auto descriptor = rateLimitProbeDescriptor(1u);
  trading_core::StrategyInstrumentSpec instrument{};
  trading_core::SymbolShardConfig shard{};
  trading_core::ShardControlState control{};
  BacktestRunRequest request{};
  // No configured quota still produces a present, empty requested snapshot.
  engine->rateLimitSnapshotRequested = true;
  CXET_CHECK(runCycle(descriptor, {}, &probe, *engine, instrument, *scratch, shard, control, 100, request, nullptr));
  CXET_CHECK(probe.cycles == 1u && probe.book == &scratch->rateLimitSnapshot);
  CXET_CHECK(probe.book->count == 0u && probe.book->actionCount == 0u);
  CXET_CHECK(!engine->rateLimitSnapshotRequested);
  CXET_CHECK(runCycle(descriptor, {}, &probe, *engine, instrument, *scratch, shard, control, 101, request, nullptr));
  CXET_CHECK(probe.cycles == 2u && probe.book == nullptr);
  std::string error;
  CXET_CHECK(applyRateLimitSchedule(*engine, policy(), error));
  engine->rateLimitSnapshotRequested = true;
  CXET_CHECK(runCycle(descriptor, {}, &probe, *engine, instrument, *scratch, shard, control, 102, request, nullptr));
  CXET_CHECK(probe.book == &scratch->rateLimitSnapshot && probe.book->count == 1u);
  CXET_CHECK(probe.book->buckets[0].remaining == 1);
  CXET_CHECK(runCycle(descriptor, {}, &probe, *engine, instrument, *scratch, shard, control, 103, request, nullptr));
  CXET_CHECK(probe.cycles == 4u && probe.book == nullptr);
}

void portfolioSnapshotPreservesRouteOrderAndIndependentTruncation() {
  auto first = std::make_unique<detail::EngineState>();
  auto second = std::make_unique<detail::EngineState>();
  std::array<detail::EngineState*, 2> owners{first.get(), second.get()};
  const detail::EngineView engines{owners.data(), owners.size()};
  auto schedule = policy();
  for (std::uint8_t slot = 1u; slot < 9u; ++slot) {
    auto bucket = schedule.buckets[0];
    bucket.kind = trading_core::RateLimitBucketKind::Requests;
    bucket.configSlot = slot;
    schedule.buckets.push_back(bucket);
  }
  for (const auto kind : {trading_core::RateLimitActionKind::MarketOrder,
                          trading_core::RateLimitActionKind::ReduceOnlyLimitOrder,
                          trading_core::RateLimitActionKind::ReduceOnlyMarketOrder,
                          trading_core::RateLimitActionKind::WsSubscribe,
                          trading_core::RateLimitActionKind::RestOrderQuery}) {
    auto action = schedule.actions[0];
    action.action = kind;
    schedule.actions.push_back(action);
  }
  std::string error;
  CXET_CHECK(applyRateLimitSchedule(*first, schedule, error));
  CXET_CHECK(applyRateLimitSchedule(*second, schedule, error));
  auto scratch = std::make_unique<MultiLegCycleScratch>();
  std::array<trading_core::StrategyInstrumentSpec, 2> instruments{};
  RateLimitProbe probe{};
  const auto descriptor = rateLimitProbeDescriptor(2u);
  trading_core::SymbolShardConfig shard{};
  trading_core::ShardControlState control{};
  BacktestRunRequest request{};
  second->rateLimitSnapshotRequested = true;
  CXET_CHECK(runCycleMulti(descriptor, {}, &probe, engines, instruments, *scratch, shard, control, 1u, 100, request, nullptr));
  CXET_CHECK(probe.cycles == 1u && probe.book == &scratch->rateLimitSnapshot);
  CXET_CHECK(probe.book->count == trading_core::kMaxStrategyRateLimitViews);
  CXET_CHECK(probe.book->actionCount == trading_core::kMaxRateLimitActions);
  for (std::size_t i = 0u; i < probe.book->count; ++i) {
    CXET_CHECK(probe.book->buckets[i].routeHandle.raw == (i < 9u ? 1u : 2u));
    CXET_CHECK(probe.book->buckets[i].kind == (i == 0u || i == 9u
        ? trading_core::RateLimitBucketKind::Orders : trading_core::RateLimitBucketKind::Requests));
  }
  for (std::size_t i = 0u; i < probe.book->actionCount; ++i) {
    CXET_CHECK(probe.book->actionRoutes[i].raw == (i < 7u ? 1u : 2u));
    CXET_CHECK(probe.book->actions[i].action == schedule.actions[i % 7u].action);
  }
  CXET_CHECK(!first->rateLimitSnapshotRequested && !second->rateLimitSnapshotRequested);
  CXET_CHECK(runCycleMulti(descriptor, {}, &probe, engines, instruments, *scratch, shard, control, 0u, 101, request, nullptr));
  CXET_CHECK(probe.cycles == 2u && probe.book == nullptr);
}

constexpr cxet::testing::Case cases[]{
    cxet::testing::Case{"symbols_on_one_virtual_account_share_one_permit_ledger", symbolsOnOneVirtualAccountShareOnePermitLedger},
    cxet::testing::Case{"quota_unsupported_ioc_refuses_without_charge", unsupportedIocDoesNotBecomeRestingOrder},
    cxet::testing::Case{"quota_depleted_refuses_order_and_cancel", depletedQuotaRefusesOrderAndCancel},
    cxet::testing::Case{"quota_fixed_window_restores_at_boundary", fixedWindowRestoresQuotaExactlyAtBoundary},
    cxet::testing::Case{"quota_unsupported_replace_refuses_without_charge", unsupportedReplaceRefusesWholeCycleWithoutQuotaCharge},
    cxet::testing::Case{"quota_observation_output_replaces_used_buckets_and_clears_missing_counts", observationOutputReplacesUsedBucketsAndClearsMissingCounts},
    cxet::testing::Case{"single_cycle_quota_snapshot_is_borrowed_only_when_requested", singleCycleSnapshotIsBorrowedOnlyWhenRequested},
    cxet::testing::Case{"portfolio_quota_snapshot_preserves_route_order_and_independent_truncation", portfolioSnapshotPreservesRouteOrderAndIndependentTruncation},
};
}

int main(int argc, char** argv) { return cxet::testing::runCases(argc, argv, cases); }
