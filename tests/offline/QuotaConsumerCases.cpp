#include "OfflineCase.hpp"
#include "Engine/BacktestEngineSetup.hpp"
#include "Strategy/Execution/BacktestStrategyIntentLowering.hpp"

#include <array>
#include <memory>

namespace {
using namespace hft_backtest;

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
  io.orderOut = {orders.data(), orders.size()};
  io.orderCount = 1;
  detail::applyIntents(engine, io, now, 1000000, 1000000);
}

void symbolsOnOneVirtualAccountShareOnePermitLedger() {
  auto first = std::make_unique<detail::EngineState>();
  auto second = std::make_unique<detail::EngineState>();
  detail::SharedAccountState account;
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
  auto engine = std::make_unique<detail::EngineState>();
  std::string error;
  CXET_CHECK(applyRateLimitSchedule(*engine, policy(), error));
  std::array<trading_core::OrderIntent, 1> orders{order(1u)};
  orders[0].limitTimeInForcePolicy = trading_core::LimitTimeInForcePolicy::Ioc;
  trading_core::StrategyIo io{};
  io.orderOut = {orders.data(), orders.size()};
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
  auto engine = std::make_unique<detail::EngineState>();
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
  io.cancelOut = {cancels.data(), cancels.size()};
  io.cancelCount = 1;
  detail::applyIntents(*engine, io, 102, 1000000, 1000000);
  CXET_CHECK(engine->orders.back().status == detail::kRecordStatusRejected);
  CXET_CHECK(engine->rateLimitRejectedCancels == 1);
  CXET_CHECK(!engine->open[0].state.cancelPending);
  CXET_CHECK(engine->pending.size() == 1);
  auto disabled = std::make_unique<detail::EngineState>();
  BacktestRateLimitSchedule off{};
  off.enabled = false;
  CXET_CHECK(applyRateLimitSchedule(*disabled, off, error));
  submit(*disabled, 4, 100);
  submit(*disabled, 5, 101);
  CXET_CHECK(disabled->open[0].state.valid && disabled->open[1].state.valid);
  CXET_CHECK(disabled->rateLimitRejectedOrders == 0);
}

void fixedWindowRestoresQuotaExactlyAtBoundary() {
  auto engine = std::make_unique<detail::EngineState>();
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
  auto engine = std::make_unique<detail::EngineState>();
  std::string error;
  CXET_CHECK(applyRateLimitSchedule(*engine, policy(), error));
  std::array<trading_core::StrategyCommandIntent, 2> commands{};
  trading_core::StrategyContext context{};
  context.io.commandOut = {commands.data(), commands.size()};
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

constexpr cxet::testing::Case cases[]{
    cxet::testing::Case{"symbols_on_one_virtual_account_share_one_permit_ledger", symbolsOnOneVirtualAccountShareOnePermitLedger},
    cxet::testing::Case{"quota_unsupported_ioc_refuses_without_charge", unsupportedIocDoesNotBecomeRestingOrder},
    cxet::testing::Case{"quota_depleted_refuses_order_and_cancel", depletedQuotaRefusesOrderAndCancel},
    cxet::testing::Case{"quota_fixed_window_restores_at_boundary", fixedWindowRestoresQuotaExactlyAtBoundary},
    cxet::testing::Case{"quota_unsupported_replace_refuses_without_charge", unsupportedReplaceRefusesWholeCycleWithoutQuotaCharge},
};
}

int main(int argc, char** argv) { return cxet::testing::runCases(argc, argv, cases); }
