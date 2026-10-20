#include "BacktestQuota.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>

namespace hft_backtest::detail {
namespace {
using namespace trading_core;
using namespace trading_core::execution;
constexpr std::size_t kMaximumBuckets = kMaxStrategyRateLimitViews;
constexpr std::size_t kMaximumReplayPermits = 4096;
// Replay uses one deterministic local account/instrument/session binding per
// ledger. It supplies no native account or live readiness authority.
constexpr ExecutionBinding kReplayBinding{1, 1, 1, 1};

bool knownAction(RateLimitActionKind action) noexcept {
  switch (action) {
    case RateLimitActionKind::LimitOrder:
    case RateLimitActionKind::MarketOrder:
    case RateLimitActionKind::CancelOrder:
    case RateLimitActionKind::ReduceOnlyLimitOrder:
    case RateLimitActionKind::ReduceOnlyMarketOrder:
    case RateLimitActionKind::WsSubscribe:
    case RateLimitActionKind::RestOrderQuery: return true;
    default: return false;
  }
}

PermitAction permitAction(RateLimitActionKind action) noexcept {
  if (action == RateLimitActionKind::CancelOrder) return PermitAction::Cancel;
  if (action == RateLimitActionKind::WsSubscribe ||
      action == RateLimitActionKind::RestOrderQuery) return PermitAction::Auxiliary;
  return PermitAction::Place;
}

std::uint64_t chargeEnd(const QuotaDomainConfig& domain, std::uint64_t now) noexcept {
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  const auto start = domain.window == QuotaWindow::Fixed
      ? now - now % domain.duration : now;
  return domain.duration > maximum - start ? maximum : start + domain.duration;
}
}  // namespace

struct BacktestQuota::Storage {
  std::array<BacktestQuotaBucketConfig, kMaximumBuckets> buckets{};
  std::array<QuotaDomainConfig, kMaximumBuckets> domains{};
  std::array<RateLimitActionConfig, kMaxRateLimitActions> actions{};
  std::array<PermitSlot, kMaximumReplayPermits> slots{};
  std::size_t bucketCount{};
  std::size_t actionCount{};
  PermitLedger ledger;

  explicit Storage(std::size_t count) noexcept
      : bucketCount(count), ledger(std::span(domains).first(count), slots) {}
};

BacktestQuota::BacktestQuota() noexcept = default;
BacktestQuota::~BacktestQuota() = default;
BacktestQuota::BacktestQuota(BacktestQuota&&) noexcept = default;
BacktestQuota& BacktestQuota::operator=(BacktestQuota&&) noexcept = default;

bool BacktestQuota::configure(const BacktestRateLimitSchedule& schedule,
                             std::string& error) {
  // Reconfiguration is cold startup only. Refuse a second enabled authority
  // rather than silently dropping charged permits or resetting a live window.
  if (storage_) {
    error = "backtest quota authority is already configured";
    return false;
  }
  if (!schedule.enabled) {
    disabled_ = true;
    return true;
  }
  disabled_ = false;
  const auto fail = [&error](const char* reason) {
    error = reason;
    return false;
  };
  if (schedule.buckets.empty() || schedule.buckets.size() > kMaximumBuckets ||
      schedule.actions.empty() || schedule.actions.size() > kMaxRateLimitActions)
    return fail("enabled backtest quota requires complete explicit buckets/actions");
  std::unique_ptr<Storage> storage(new (std::nothrow) Storage(schedule.buckets.size()));
  if (!storage) return fail("backtest quota storage allocation failed");
  for (std::size_t i = 0; i < schedule.buckets.size(); ++i) {
    const auto& bucket = schedule.buckets[i];
    if (!bucket.enabled || bucket.limit <= 0 || bucket.intervalNs == 0 ||
        bucket.kind < RateLimitBucketKind::RequestWeight ||
        bucket.kind > RateLimitBucketKind::ReduceOnlyOrders ||
        bucket.scope > RateLimitScopeKind::Connection || bucket.configSlot >= kMaximumBuckets ||
        (bucket.windowMode != QuotaWindow::Fixed && bucket.windowMode != QuotaWindow::Sliding))
      return fail("invalid explicit backtest quota bucket");
    for (std::size_t j = 0; j < i; ++j)
      if (storage->buckets[j].kind == bucket.kind &&
          storage->buckets[j].configSlot == bucket.configSlot)
        return fail("duplicate explicit backtest quota bucket slot");
    storage->buckets[i] = bucket;
    storage->domains[i] = {static_cast<std::uint32_t>(i + 1), bucket.windowMode,
                          bucket.intervalNs, static_cast<std::uint64_t>(bucket.limit), 0};
  }
  for (const auto& action : schedule.actions) {
    if (!action.enabled || !knownAction(action.action) || action.costCount == 0 ||
        action.costCount > action.costs.size())
      return fail("invalid or unsupported explicit backtest quota action");
    for (std::size_t i = 0; i < storage->actionCount; ++i)
      if (storage->actions[i].action == action.action)
        return fail("duplicate explicit backtest quota action");
    std::size_t count = 0;
    for (std::size_t c = 0; c < action.costCount; ++c) {
      const auto& cost = action.costs[c];
      if (cost.cost <= 0) return fail("backtest quota action cost must be positive");
      for (std::size_t previous = 0; previous < c; ++previous)
        if (action.costs[previous].bucket == cost.bucket)
          return fail("duplicate backtest quota action bucket cost");
      bool found = false;
      for (std::size_t d = 0; d < storage->bucketCount; ++d) {
        if (storage->buckets[d].kind != cost.bucket) continue;
        if (cost.cost > storage->buckets[d].limit)
          return fail("backtest quota action cost exceeds bucket limit");
        found = true;
        ++count;
      }
      if (!found) return fail("backtest quota action references missing bucket");
    }
    if (count > kMaxPermitCosts)
      return fail("backtest quota action exceeds canonical permit cost capacity");
    storage->actions[storage->actionCount++] = action;
  }
  if (!storage->ledger.prepareCold(0)) return fail("backtest quota ledger preparation failed");
  storage_ = std::move(storage);
  return true;
}

PermitDecision BacktestQuota::admit(RateLimitActionKind action, TimeNs now) noexcept {
  if (!knownAction(action)) return {PermitRefusal::Invalid, 0};
  if (disabled_) return {};
  if (!storage_) return {PermitRefusal::Unprepared, 0};
  if (now.raw == std::numeric_limits<std::uint64_t>::max()) return {PermitRefusal::Invalid, 0};
  const RateLimitActionConfig* configured = nullptr;
  for (std::size_t i = 0; i < storage_->actionCount; ++i)
    if (storage_->actions[i].action == action) configured = &storage_->actions[i];
  if (!configured) return {PermitRefusal::Invalid, 0};
  PermitRequest request{};
  request.binding = kReplayBinding;
  request.action = permitAction(action);
  request.expiresAt = now.raw + 1;
  for (std::size_t c = 0; c < configured->costCount; ++c)
    for (std::size_t d = 0; d < storage_->bucketCount; ++d)
      if (storage_->buckets[d].kind == configured->costs[c].bucket)
        request.costs[request.costCount++] = {storage_->domains[d].id,
            static_cast<std::uint64_t>(configured->costs[c].cost)};
  PermitBatch batch{};
  const auto issued = storage_->ledger.issueCold(std::span(&request, 1), now.raw, batch);
  if (!issued.ok()) return issued;
  const auto consumed = storage_->ledger.consume(batch.tokens[0], kReplayBinding, request.action, now.raw);
  if (!consumed.ok()) {
    static_cast<void>(storage_->ledger.releasePrepared(batch.tokens[0]));
    return consumed;
  }
  // Submission to the deterministic simulator is the modeled venue attempt.
  // Subsequent ACK/fill rejection never refunds its attempted quota charge.
  if (!storage_->ledger.settle(batch.tokens[0], AttemptDisposition::Attempted) ||
      !storage_->ledger.advanceCold(now.raw)) return {PermitRefusal::Invalid, 0};
  return {};
}

void BacktestQuota::observationBook(ExecutionRouteHandle route,
    ExchangeId exchange, canon::MarketType market, std::uint8_t apiSlot, TimeNs now,
    StrategyRateLimitBook& result) noexcept {
  result.count = 0u;
  result.actionCount = 0u;
  if (!storage_ || !storage_->ledger.advanceCold(now.raw)) return;
  result.actionCount = static_cast<std::uint8_t>(storage_->actionCount);
  for (std::size_t i = 0; i < storage_->actionCount; ++i) {
    result.actions[i] = storage_->actions[i];
    result.actionRoutes[i] = route;
  }
  for (std::size_t d = 0; d < storage_->bucketCount; ++d) {
    auto& view = result.buckets[result.count++];
    view = RateLimitBucketView{};
    const auto& metadata = storage_->buckets[d];
    const auto& domain = storage_->domains[d];
    view.routeHandle = route;
    view.exchange = exchange;
    view.market = market;
    view.apiSlot = apiSlot;
    view.kind = metadata.kind;
    view.scope = metadata.scope;
    view.intervalNs = domain.duration;
    view.limit = metadata.limit;
    view.recvTime = now;
    view.valid = true;
    view.resetMono.raw = chargeEnd(domain, now.raw);
    for (const auto& slot : storage_->slots) {
      for (std::size_t c = 0; c < slot.request.costCount; ++c) {
        if (!(slot.activeCostMask & (1u << c)) || slot.request.costs[c].domainId != domain.id) continue;
        view.used += static_cast<std::int64_t>(slot.request.costs[c].weight);
        const auto end = chargeEnd(domain, slot.consumedAt);
        if (domain.window == QuotaWindow::Sliding && end < view.resetMono.raw)
          view.resetMono.raw = end;
      }
    }
    view.remaining = view.limit - view.used;
    view.exhausted = view.remaining <= 0;
  }
}

}  // namespace hft_backtest::detail
