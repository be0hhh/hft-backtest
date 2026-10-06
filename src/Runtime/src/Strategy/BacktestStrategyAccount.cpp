#include "trading_core/Algorithm/Math/Integer/Saturating.hpp"
#include "BacktestStrategyRuntime.hpp"

#include "../Config/BacktestRunConfig.hpp"
#include "Execution/BacktestStrategyIntentLowering.hpp"
#include "Diagnostics/BacktestStrategyTrace.hpp"

#include "trading_core/Strategy/Context/StrategyContextView.hpp"
#include "trading_core/Strategy/Orders/WarmupIntents.hpp"
#include "trading_core/Strategy/StrategyInputFreshness.hpp"
#include "trading_core/Strategy/StrategyMarketData.hpp"

#include <algorithm>
#include <limits>
#include <string>

#include "Internal/BacktestStrategyRuntimeHelpers.hpp"

namespace hft_backtest::strategy_runtime_detail {

const BacktestStrategyVenueConfig* venueByName(
    const BacktestStrategyConfig& cfg,
    const std::string& name) noexcept {
  for (const BacktestStrategyVenueConfig& candidate : cfg.venues) {
    if (candidate.name == name) return &candidate;
  }
  return nullptr;
}

bool venueMatchesSession(const BacktestStrategyVenueConfig& venue,
                         const detail::SessionData& session) noexcept {
  if (venue.exchange.raw != detail::exchangeFromText(session.exchange).raw) return false;
  if (venue.market.raw != detail::marketFromText(session.market).raw) return false;
  for (const Symbol& symbol : venue.symbols) {
    if (session.symbol == symbol.data) return true;
  }
  return false;
}

const BacktestStrategyVenueConfig* venueForSession(
    const BacktestStrategyConfig& cfg,
    const detail::SessionData& session) noexcept {
  for (const BacktestStrategyVenueConfig& candidate : cfg.venues) {
    if (venueMatchesSession(candidate, session)) return &candidate;
  }
  return nullptr;
}

void cancelOpenOrdersForTraderRiskClose(detail::EngineState& engine, std::int64_t tsNs) noexcept {
  engine.pending.clear();
  for (detail::OpenOrder& order : engine.open) {
    if (order.state.valid && order.recordIndex < engine.orders.size()) {
      engine.orders[order.recordIndex].status = detail::kRecordStatusCancelled;
      engine.orders[order.recordIndex].tsNs = tsNs;
    }
    order = detail::OpenOrder{};
  }
}

}  // namespace hft_backtest::strategy_runtime_detail

namespace hft_backtest {
using strategy_intent_lowering::applyToLegs;
using strategy_intent_lowering::applyToSingleLeg;
using strategy_intent_lowering::applyTransactionCycle;
using strategy_intent_lowering::bindTransactionLegs;
using strategy_intent_lowering::configuredDefaultExecutionRoute;
using strategy_intent_lowering::configuredPrimaryLegIndex;
using strategy_intent_lowering::footprintSupported;
using strategy_intent_lowering::validateOutput;
using strategy_intent_lowering::routeLegIndex;
using strategy_intent_lowering::routeTradable;
using strategy_intent_lowering::transactionAdmission;
using strategy_runtime_detail::cancelOpenOrdersForTraderRiskClose;
using strategy_runtime_detail::venueByName;
using strategy_runtime_detail::venueForSession;
using strategy_runtime_detail::venueMatchesSession;

trading_core::StrategyAccountState accountStateFromEngine(const detail::EngineState& engine) noexcept {
  trading_core::StrategyAccountState out{};
  out.initialBalanceE8 = engine.sharedAccount ? engine.sharedAccount->initialBalanceE8 : engine.initialBalanceE8;
  out.walletBalanceE8 = engine.sharedAccount ? (engine.sharedAccount->observedInitialized
      ? engine.sharedAccount->observedWalletBalanceE8 : engine.sharedAccount->initialBalanceE8) : engine.observedWalletBalanceE8;
  out.availableBalanceE8 = out.walletBalanceE8;
  if (engine.market.raw == canon::kMarketTypeSpot.raw) {
    const auto held = engine.sharedAccount ? engine.sharedAccount->observedReservedQuoteE8 : engine.observedReservedQuoteE8;
    out.availableBalanceE8 = held > out.walletBalanceE8 ? 0 : out.walletBalanceE8 - held;
  }
  out.equityE8 = engine.sharedAccount ? trading_core::saturatingAddI64(
      out.walletBalanceE8, engine.sharedAccount->observedPositionEquityValueE8) : trading_core::saturatingAddI64(
          out.walletBalanceE8, detail::positionEquityValueE8(engine, true));
  out.makerFeeBpsE8 = engine.makerFeeBpsE8;
  out.takerFeeBpsE8 = engine.takerFeeBpsE8;
  out.hasBalance = out.initialBalanceE8 != 0 || out.walletBalanceE8 != 0;
  out.hasFees = engine.makerFeeBpsE8 != 0 || engine.takerFeeBpsE8 != 0;
  return out;
}

std::size_t boundedRiskLegCount(std::size_t count) noexcept {
  return count < trading_core::kStrategyRuntimeMaxLegs
      ? count
      : trading_core::kStrategyRuntimeMaxLegs;
}

void refreshRiskAccountsFromEngines(
    detail::ConstEngineView engines,
    std::array<trading_core::StrategyAccountState, trading_core::kStrategyRuntimeMaxLegs>& out) noexcept {
  out.fill(trading_core::StrategyAccountState{});
  const std::size_t count = boundedRiskLegCount(engines.size());
  for (std::size_t i = 0u; i < count; ++i) out[i] = accountStateFromEngine(engines[i]);
}

void resolveRiskVenuesForSessions(
    const BacktestStrategyConfig& cfg,
    const std::vector<detail::SessionData>& sessions,
    std::array<const BacktestStrategyVenueConfig*, trading_core::kStrategyRuntimeMaxLegs>& out) noexcept {
  out.fill(nullptr);
  const std::size_t count = boundedRiskLegCount(sessions.size());
  for (const BacktestPortfolioConfig& portfolio : cfg.portfolios) {
    if (portfolio.legs.size() != count) continue;
    bool matches = true;
    for (std::size_t i = 0u; i < count; ++i) {
      const BacktestStrategyVenueConfig* venue = venueByName(cfg, portfolio.legs[i].venue);
      if (!venue || !venueMatchesSession(*venue, sessions[i]) || sessions[i].symbol != portfolio.legs[i].symbol.data) {
        matches = false;
        break;
      }
    }
    if (!matches) continue;
    for (std::size_t i = 0u; i < count; ++i) out[i] = venueByName(cfg, portfolio.legs[i].venue);
    return;
  }
  for (std::size_t i = 0u; i < count; ++i) out[i] = venueForSession(cfg, sessions[i]);
}

bool forceCloseForTraderRisk(detail::EngineState& engine, std::int64_t tsNs) {
  cancelOpenOrdersForTraderRiskClose(engine, tsNs);
  if (engine.netQtyRaw == 0) return true;
  const std::uint64_t closeLatencyUs =
      detail::sampleLatencyUs(engine.latency, detail::BacktestLatencyChannel::MarketOrder);
  return detail::forceClosePosition(engine, tsNs, closeLatencyUs);
}

std::int64_t portfolioEquityE8(detail::ConstEngineView engines,
                               std::int64_t initialBalanceE8) noexcept {
  std::int64_t out = initialBalanceE8;
  for (const detail::EngineState& engine : engines) {
    out = trading_core::saturatingAddI64(
        out,
        trading_core::saturatingAddI64(
            engine.netRealizedPnlE8, detail::unrealizedPnlE8(engine)));
  }
  return out;
}


}  // namespace hft_backtest
