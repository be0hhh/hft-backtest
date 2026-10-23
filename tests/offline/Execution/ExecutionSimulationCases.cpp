#include "OfflineCase.hpp"
#include "Engine/BacktestEngine.hpp"
#include "Engine/BacktestEngineSetup.hpp"
#include "Execution/BacktestOrderEvidence.hpp"
#include "Execution/Internal/BacktestExecutionHelpers.hpp"
#include "Strategy/BacktestStrategyRuntime.hpp"
#include "trading_core/Strategy/Orders/Evidence/OrderDecisionCursor.hpp"

#include <algorithm>
#include <array>
#include <initializer_list>
#include <limits>
#include <memory>
#include <span>
#include <string>

namespace {
namespace detail = hft_backtest::detail;
constexpr std::int64_t q = 100'000'000;

struct ExecutionFixture {
  std::unique_ptr<detail::EngineState> state = std::make_unique<detail::EngineState>();
  std::int64_t nextTs{101};

  explicit ExecutionFixture(bool depth = true, bool collateral = true, std::uint32_t leg = 0u,
      detail::SimulationAccountingModel model = detail::SimulationAccountingModel::Linear,
      canon::MarketType product = canon::kMarketTypeFutures) {
    auto& e = *state;
    e.market = product;
    e.accountingModel = model;
    e.exchange = detail::exchangeFromText("binance");
    e.symbol.copyFrom("BTC_USDT");
    e.activeContextIndex = 1u;
    e.legIndex = leg;
    hft_backtest::BacktestRateLimitSchedule schedule{}; schedule.enabled = false;
    std::string error;
    CXET_CHECK(hft_backtest::applyRateLimitSchedule(e, schedule, error));
    hft_backtest::BacktestRunRequest request{};
    request.initialBalanceE8 = 1'000'000 * q;
    request.makerFeeBpsE8 = request.takerFeeBpsE8 = 0;
    request.hasMakerFeeBps = request.hasTakerFeeBps = true;
    detail::initializeAccounting(e, request);
    if (collateral) CXET_CHECK(detail::configureSimulationDerivativeCollateral(e, 10 * q, q, 1u));
    CXET_CHECK(!e.simulationEvidenceFailed);
    CXET_CHECK(e.executionLiquidity.prepare(16u));
    CXET_CHECK(e.venueOrderBook.reserveLevelsPerSide(CountVal{16u}, 1u));
    CXET_CHECK(e.venueDepthBook.prepare());
    e.venueOrderBookExecutionEnabled = depth;
    e.depthExecutionMode = depth ? hft_backtest::BacktestDepthExecutionMode::Depth
                                 : hft_backtest::BacktestDepthExecutionMode::BboNoDepth;
    top(99 * q, 101 * q, 10 * q);
    if (depth) publish({detail::PriceLevelRow{99 * q, 10 * q, 0},
                        detail::PriceLevelRow{101 * q, 10 * q, 1}}, true, 100);
  }

  void top(std::int64_t bid, std::int64_t ask, std::int64_t qty) {
    auto& snapshot = state->venueSnapshot;
    snapshot.hasBookTicker = trading_core::kAlgoFlagTrue;
    snapshot.lastBookTicker.bid.px.raw = bid;
    snapshot.lastBookTicker.ask.px.raw = ask;
    snapshot.lastBookTicker.bid.qty.raw = snapshot.lastBookTicker.ask.qty.raw = qty;
    state->snapshot = snapshot;
  }

  void publish(std::initializer_list<detail::PriceLevelRow> levels, bool snapshot,
               std::int64_t ts) {
    const std::span<const detail::PriceLevelRow> rows{levels.begin(), levels.size()};
    hftrec::replay::EventArrival arrival{};
    arrival.sourceId = 1u; arrival.sourceGeneration = 1u;
    arrival.producerEpoch = arrival.sessionEpoch = 1u;
    arrival.receiveMonotonicNs = ts;
    CXET_CHECK(state->venueDepthBook.apply(rows, ts, arrival, snapshot));
    const auto source = state->venueDepthBook.view();
    CXET_CHECK(detail::refreshReplayDepthProjection(state->venueOrderBook, source, Symbol{}));
    CXET_CHECK(trading_core::orderBook(state->venueOrderBook).initialized() ==
        (source.bidCount != 0u || source.askCount != 0u));
    state->executionLiquidity.publish(rows, snapshot);
    // Keep BBO and depth consistent; these cases isolate consumption semantics.
    for (const auto& row : rows) {
      auto& top = state->venueSnapshot.lastBookTicker;
      if (row.side == 0 && row.priceE8 == top.bid.px.raw) top.bid.qty.raw = row.qtyE8;
      if (row.side == 1 && row.priceE8 == top.ask.px.raw) top.ask.qty.raw = row.qtyE8;
    }
  }

  static trading_core::OrderIntent intent(std::uint64_t id, std::int64_t qty,
      canon::OrderType type = canon::OrderType::Market, std::int64_t price = 0) {
    trading_core::OrderIntent out{};
    out.clientIntentId.raw = id; out.side = Side::Buy();
    out.orderType = type; out.price.raw = price; out.qty.raw = qty;
    return out;
  }

  void place(const trading_core::OrderIntent& order) {
    const auto ts = nextTs++;
    CXET_CHECK(detail::execution_detail::submitOrderIntent(*state, order, ts, ts));
    detail::applyDueActions(*state, ts);
    CXET_CHECK(!state->simulationEvidenceFailed);
  }

  detail::OpenOrder& open(std::uint64_t id) {
    for (auto& order : state->open)
      if (order.state.valid && order.state.clientIntentId.raw == id) return order;
    CXET_CHECK(false);
    return state->open[0];
  }

  void maker(std::uint64_t id, std::int64_t qty, std::int64_t price = 99 * q) {
    auto order = intent(id, qty, canon::OrderType::Limit, price);
    order.limitTimeInForcePolicy = trading_core::LimitTimeInForcePolicy::PostOnlyStrict;
    place(order);
  }

  void trade(std::int64_t qty, std::int64_t price = 99 * q) {
    detail::TradeRow row{};
    row.priceE8 = price; row.qtyE8 = qty; row.side = 0; row.tsNs = nextTs++;
    cxet::composite::Trade value{};
    CXET_CHECK(detail::toRuntime(row, value));
    CXET_CHECK(value.initiatorSide() == cxet::composite::TradeInitiatorSide::Seller);
    detail::tryFillOnTrade(*state, value, row.tsNs);
  }

  std::int64_t filled(std::uint64_t id) const {
    std::int64_t total{};
    for (const auto& fill : state->fills) if (fill.orderId == id) total += fill.qtyE8;
    return total;
  }
  void deliverPrivate() {
    while (state->deliveredUserEvents < state->userEvents.size()) {
      const auto index = state->deliveredUserEvents;
      const auto now = std::max(nextTs++, state->userEventDeliveryTsNs[index]);
      CXET_CHECK(detail::deliverSimulationOrderEvent(*state, index, now));
    }
  }
};

const cxet::testing::Case cases[]{
    {"terminal_risk_mutation_uses_resident_order_indexes", +[] {
      for(const bool liquidate:{false,true}) {
        auto engine=std::make_unique<detail::EngineState>();
        engine->accountingModel=detail::SimulationAccountingModel::Cash;
        engine->orders.resize(1u);engine->orders[0].status=detail::kRecordStatusNone;
        engine->emittedOrders=10u;engine->open[0].state.valid=true;engine->open[0].recordIndex=1u;
        engine->open[1].state.valid=true;engine->open[1].recordIndex=0u;
        if(liquidate)detail::hardLiquidate(*engine,100,0);
        else detail::stopForTraderRisk(*engine,100,0,0);
        CXET_CHECK(engine->orders[0].status==detail::kRecordStatusCancelled);
        CXET_CHECK(!engine->open[0].state.valid&&!engine->open[1].state.valid);
      }
    }},
    {"direct_accounting_quantity_overflow_retains_wallet_position_and_fault", +[] {
      for (const auto side : {Side::Buy(), Side::Sell()}) {
        auto engine = std::make_unique<detail::EngineState>();
        engine->accountingModel = detail::SimulationAccountingModel::Linear;
        engine->netQtyRaw = side.raw == Side::Buy().raw ? INT64_MAX : INT64_MIN;
        engine->entryPriceE8 = 1;
        engine->walletBalanceE8 = 17; engine->grossRealizedPnlE8 = 3;
        engine->netRealizedPnlE8 = 4; engine->feesPaidE8 = 5;
        detail::SharedAccountState shared{}; shared.accountingModel = engine->accountingModel;
        shared.walletBalanceE8 = 19; shared.revision = 7; engine->sharedAccount = &shared;
        const auto before = engine->netQtyRaw;
        const auto result = detail::updatePositionActualFee(*engine, side, 1, 1, 1, 1);
        CXET_CHECK(!result.applied && engine->simulationEvidenceFailed);
        CXET_CHECK(engine->netQtyRaw == before && engine->entryPriceE8 == 1 && engine->walletBalanceE8 == 17);
        CXET_CHECK(engine->grossRealizedPnlE8 == 3 && engine->netRealizedPnlE8 == 4 && engine->feesPaidE8 == 5);
        CXET_CHECK(shared.walletBalanceE8 == 19 && shared.revision == 7);
        const auto opposite = side.raw == Side::Buy().raw ? Side::Sell() : Side::Buy();
        CXET_CHECK(!detail::updatePosition(*engine, opposite, 1, 1, 2, 0).applied);
        CXET_CHECK(engine->netQtyRaw == before && engine->walletBalanceE8 == 17 && engine->fills.empty() && engine->userEvents.empty());
      }
    }},
    {"direct_accounting_exact_signed_quantity_boundaries_remain_valid", +[] {
      for (const auto side : {Side::Buy(), Side::Sell()}) {
        auto engine = std::make_unique<detail::EngineState>();
        engine->accountingModel = detail::SimulationAccountingModel::Linear;
        const auto boundary = side.raw == Side::Buy().raw ? INT64_MAX : INT64_MIN;
        engine->netQtyRaw = side.raw == Side::Buy().raw ? boundary - 1 : boundary + 1;
        engine->entryPriceE8 = 1; engine->walletBalanceE8 = 17;
        const auto result = detail::updatePositionActualFee(*engine, side, 1, 1, 1, 0);
        CXET_CHECK(result.applied && !engine->simulationEvidenceFailed && result.position.netQtyRaw == boundary);
        CXET_CHECK(engine->netQtyRaw == boundary && engine->walletBalanceE8 == 17 && engine->feesPaidE8 == 0);
      }
      auto cash = std::make_unique<detail::EngineState>();
      cash->accountingModel = detail::SimulationAccountingModel::Cash;
      cash->walletBalanceE8 = INT64_MAX;
      const auto bought = detail::updatePositionActualFee(*cash, Side::Buy(), INT64_MAX, q, 1, 0);
      CXET_CHECK(bought.applied && cash->netQtyRaw == q && cash->walletBalanceE8 == 0 && !cash->simulationEvidenceFailed);
      const auto sold = detail::updatePositionActualFee(*cash, Side::Sell(), INT64_MAX, q, 2, 0);
      CXET_CHECK(sold.applied && cash->netQtyRaw == 0 && cash->walletBalanceE8 == INT64_MAX && !cash->simulationEvidenceFailed);
      ExecutionFixture f(false, false, 0u, detail::SimulationAccountingModel::Cash, canon::kMarketTypeSpot);
      f.state->walletBalanceE8 = f.state->observedWalletBalanceE8 = INT64_MAX;
      f.top(INT64_MAX, INT64_MAX, q);
      f.place(ExecutionFixture::intent(1u, q));
      CXET_CHECK(f.state->fills.size() == 1u && f.state->netQtyRaw == q && f.state->walletBalanceE8 == 0);
      f.deliverPrivate();
      CXET_CHECK(f.state->position.netQty.raw == q && f.state->observedWalletBalanceE8 == 0 && !f.state->simulationEvidenceFailed);
    }},
    {"accepted_fill_quantity_overflow_publishes_no_partial_fill_and_refuses_next_command", +[] {
      for (const auto side : {Side::Buy(), Side::Sell()}) {
        ExecutionFixture f(false);
        auto& engine = *f.state;
        engine.netQtyRaw = side.raw == Side::Buy().raw ? INT64_MAX : INT64_MIN;
        engine.entryPriceE8 = 1;
        detail::OpenOrder order{}; order.state.valid = order.active = true;
        order.state.side = side; order.state.qty.raw = q;
        const auto position = engine.netQtyRaw, wallet = engine.walletBalanceE8;
        const auto revision = engine.contextRevision;
        CXET_CHECK(!detail::applyAcceptedOrderFill(engine, order, 1, q, 7, 200, true, {}));
        CXET_CHECK(engine.simulationEvidenceFailed && engine.netQtyRaw == position && engine.entryPriceE8 == 1);
        CXET_CHECK(engine.walletBalanceE8 == wallet && engine.feesPaidE8 == 0 && engine.contextRevision == revision);
        CXET_CHECK(order.state.valid && order.filledQtyRaw == 0 && order.filledNotionalRaw == 0);
        CXET_CHECK(engine.fills.empty() && engine.equityPoints.empty() && engine.riskSnapshots.empty() && engine.userEvents.empty());
        CXET_CHECK(!detail::execution_detail::submitOrderIntent(engine, ExecutionFixture::intent(2u, q), 201, 201));
        detail::applyDueActions(engine, 201);
        CXET_CHECK(engine.orders.empty() && engine.pending.empty() && engine.userEvents.empty() && engine.netQtyRaw == position);
      }
    }},
    {"private_quantity_overflow_preserves_canonical_delivery_cut_and_latches", +[] {
      for (const auto side : {Side::Buy(), Side::Sell()}) {
        ExecutionFixture f(false);
        const auto price = side.raw == Side::Buy().raw ? 99 * q : 103 * q;
        auto intent = ExecutionFixture::intent(1u, q, canon::OrderType::Limit, price);
        intent.side = side; intent.limitTimeInForcePolicy = trading_core::LimitTimeInForcePolicy::PostOnlyStrict;
        f.place(intent); f.deliverPrivate();
        CXET_CHECK(detail::fillOrderQty(*f.state, f.open(1u), price, q, f.nextTs++, false, {}));
        auto& engine = *f.state;
        engine.position.netQty.raw = side.raw == Side::Buy().raw ? INT64_MAX : INT64_MIN;
        engine.position.entryPrice.raw = 1;
        const auto position = engine.position.netQty.raw, wallet = engine.observedWalletBalanceE8;
        const auto revision = engine.contextRevision;
        const auto delivered = engine.deliveredUserEvents;
        const auto replayTime = engine.lastReplayMonotonicNs;
        const auto* record = trading_core::strategy::canonicalOrderEvidence(
            detail::simulationOrderEvidenceView(engine), Id{1u}, 1u);
        CXET_CHECK(record && record->cumulativeQtyE8 == 0 && !record->terminal());
        CXET_CHECK(!detail::deliverSimulationOrderEvent(engine, delivered, engine.userEventDeliveryTsNs[delivered]));
        CXET_CHECK(engine.simulationEvidenceFailed && engine.position.netQty.raw == position && engine.position.entryPrice.raw == 1);
        CXET_CHECK(engine.observedWalletBalanceE8 == wallet && engine.contextRevision == revision && engine.lastReplayMonotonicNs == replayTime);
        CXET_CHECK(record->cumulativeQtyE8 == 0 && !record->terminal() && engine.deliveredUserEvents == delivered && !engine.userEventConsumed[delivered]);
        engine.position.netQty.raw = 0;
        CXET_CHECK(!detail::deliverSimulationOrderEvent(engine, delivered, engine.userEventDeliveryTsNs[delivered]));
        CXET_CHECK(engine.observedWalletBalanceE8 == wallet && record->cumulativeQtyE8 == 0 && !engine.userEventConsumed[delivered]);
      }
    }},
    {"depth_fok_quantity_overflow_keeps_all_prior_chunks_uncommitted", +[] {
      ExecutionFixture f(true, false, 0u, detail::SimulationAccountingModel::Cash, canon::kMarketTypeSpot);
      f.publish({detail::PriceLevelRow{99 * q, q, 0},
                 detail::PriceLevelRow{101 * q, q, 1},
                 detail::PriceLevelRow{102 * q, q, 1}}, true, 100);
      auto stop = ExecutionFixture::intent(1u, 2 * q, canon::OrderType::Stop, 102 * q);
      stop.triggerPrice.raw = 103 * q; stop.triggerSource = canon::OrderTriggerSource::Last;
      stop.triggeredTimeInForce = canon::TimeInForce::FOK;
      f.place(stop); f.open(1u).stopTriggered = true;
      f.state->netQtyRaw = INT64_MAX - q; f.state->entryPriceE8 = 1;
      const auto wallet = f.state->walletBalanceE8;
      const auto events = f.state->userEvents.size();
      detail::tryFillOnTop(*f.state, f.nextTs++, false);
      CXET_CHECK(f.state->simulationEvidenceFailed && f.state->netQtyRaw == INT64_MAX - q && f.state->walletBalanceE8 == wallet);
      CXET_CHECK(f.state->fills.empty() && f.state->userEvents.size() == events && f.open(1u).filledQtyRaw == 0);
      CXET_CHECK(f.state->executionLiquidity.available(trading_core::BookSide::Ask, 101 * q, q) == q &&
          f.state->executionLiquidity.available(trading_core::BookSide::Ask, 102 * q, q) == q);
    }},
    {"typed_cash_on_native_margin_product_reserves_and_settles_principal", +[] {
      ExecutionFixture f(false, false, 0u, detail::SimulationAccountingModel::Cash, canon::kMarketTypeMargin);
      f.state->walletBalanceE8 = f.state->observedWalletBalanceE8 = 1'000 * q;
      f.maker(1u, 2 * q);
      CXET_CHECK(f.state->market.raw == canon::kMarketTypeMargin.raw);
      CXET_CHECK(f.state->simulationOrders->binding.productId == static_cast<std::uint32_t>(canon::kMarketTypeMargin.raw) + 1u);
      CXET_CHECK(f.state->observedReservedQuoteE8 == 198 * q && f.state->venueReservedQuoteE8 == 198 * q);
      detail::FillExecutionMetadata metadata{}; metadata.fillReason = detail::kFillReasonPassiveQueue;
      CXET_CHECK(detail::fillOrderQty(*f.state, f.open(1u), 99 * q, q, f.nextTs++, false, metadata));
      CXET_CHECK(f.state->walletBalanceE8 == 901 * q && f.state->venueReservedQuoteE8 == 99 * q);
      CXET_CHECK(f.state->observedWalletBalanceE8 == 1'000 * q && f.state->observedReservedQuoteE8 == 198 * q);
      f.deliverPrivate();
      CXET_CHECK(f.state->observedWalletBalanceE8 == 901 * q && f.state->observedReservedQuoteE8 == 99 * q);
      CXET_CHECK(f.state->position.netQty.raw == q && f.state->netQtyRaw == q);
      trading_core::CancelIntent cancel{}; cancel.clientIntentId.raw = 11u; cancel.targetClientIntentId.raw = 1u;
      const auto now = f.nextTs++;
      CXET_CHECK(detail::execution_detail::submitCancelIntent(*f.state, cancel, now, now));
      detail::applyDueActions(*f.state, now); f.deliverPrivate();
      CXET_CHECK(f.state->venueReservedQuoteE8 == 0 && f.state->observedReservedQuoteE8 == 0);
      CXET_CHECK(f.state->walletBalanceE8 == 901 * q && f.state->observedWalletBalanceE8 == 901 * q);
      CXET_CHECK(!f.state->simulationOrders->collateral.known && !f.state->simulationEvidenceFailed);
      CXET_CHECK(f.state->market.raw == canon::kMarketTypeMargin.raw);
    }},
    {"unknown_accounting_model_never_falls_back_from_native_spot_product", +[] {
      ExecutionFixture f(false, false, 0u, detail::SimulationAccountingModel::Unknown, canon::kMarketTypeSpot);
      CXET_CHECK(!detail::execution_detail::submitOrderIntent(*f.state,
          ExecutionFixture::intent(1u, q), 101, 101));
      CXET_CHECK(f.state->fills.empty() && f.state->pending.empty());
      CXET_CHECK(f.state->orders.back().status == detail::kRecordStatusRejected);
      CXET_CHECK(f.state->observedReservedQuoteE8 == 0 && f.state->venueReservedQuoteE8 == 0);
    }},
    {"accepted_derivative_fill_survives_missing_mark_and_fences_later_matching", +[] {
      ExecutionFixture f(false);
      f.maker(1u, q);
      f.state->venueSnapshot.lastBookTicker.bid.px.raw = 0;
      CXET_CHECK(detail::applyAcceptedOrderFill(*f.state, f.open(1u), 99 * q, q, 0, f.nextTs++, false, {}));
      CXET_CHECK(f.state->netQtyRaw == q && f.state->fills.size() == 1u);
      CXET_CHECK(f.state->simulationOrders->collateral.venueUnavailable);
      CXET_CHECK(f.state->simulationOrders->collateral.venueProtectedE8 >= 10 * q);
      const auto now = f.nextTs++;
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*f.state,
          ExecutionFixture::intent(2u, q, canon::OrderType::Limit, 98 * q), now, now));
      detail::applyDueActions(*f.state, now);
      CXET_CHECK(f.state->orders.back().status == detail::kRecordStatusRejected);
      CXET_CHECK(f.state->netQtyRaw == q && f.state->fills.size() == 1u);
      f.deliverPrivate();
      CXET_CHECK(!f.state->simulationEvidenceFailed && f.state->position.netQty.raw == q);
    }},
    {"accepted_cash_fill_records_actual_deficit_without_authorizing_new_risk", +[] {
      ExecutionFixture f(false, false, 0u, detail::SimulationAccountingModel::Cash, canon::kMarketTypeMargin);
      f.maker(1u, q);
      f.state->walletBalanceE8 = f.state->observedWalletBalanceE8 = 0;
      CXET_CHECK(detail::applyAcceptedOrderFill(*f.state, f.open(1u), 99 * q, q, 0, f.nextTs++, false, {}));
      CXET_CHECK(f.state->walletBalanceE8 == -99 * q && f.state->netQtyRaw == q);
      f.deliverPrivate();
      CXET_CHECK(f.state->observedWalletBalanceE8 == -99 * q && f.state->position.netQty.raw == q);
      const auto now = f.nextTs++;
      CXET_CHECK(!detail::execution_detail::submitOrderIntent(*f.state,
          ExecutionFixture::intent(2u, q), now, now));
      CXET_CHECK(!f.state->simulationEvidenceFailed && f.state->netQtyRaw == q);
    }},
    {"unknown_fee_policy_refuses_cash_and_linear_risk_but_delivers_actual_fee_fact", +[] {
      for (const auto model : {detail::SimulationAccountingModel::Cash, detail::SimulationAccountingModel::Linear}) {
        ExecutionFixture f(false, model == detail::SimulationAccountingModel::Linear, 0u, model, canon::kMarketTypeMargin);
        f.state->feePolicyKnown = false;
        CXET_CHECK(!detail::execution_detail::submitOrderIntent(*f.state,
            ExecutionFixture::intent(1u, q), 101, 101));
        CXET_CHECK(f.state->fills.empty() && f.state->pending.empty());
        CXET_CHECK(f.state->orders.back().status == detail::kRecordStatusRejected);
        CXET_CHECK(f.state->market.raw == canon::kMarketTypeMargin.raw);
      }
      ExecutionFixture f(false, false, 0u, detail::SimulationAccountingModel::Cash, canon::kMarketTypeMargin);
      f.state->takerFeeBpsE8 = 100'000'000;
      f.maker(1u, q);
      f.state->feePolicyKnown = false;
      constexpr std::int64_t actualFee = 12'345;
      CXET_CHECK(detail::applyAcceptedOrderFill(*f.state, f.open(1u), 99 * q, q, actualFee, f.nextTs++, true, {}));
      CXET_CHECK(f.state->feesPaidE8 == actualFee);
      const auto settledCash = f.state->walletBalanceE8;
      f.state->feePolicyKnown = false;
      const auto now = f.nextTs++;
      CXET_CHECK(!detail::execution_detail::submitOrderIntent(*f.state,
          ExecutionFixture::intent(2u, q), now, now));
      f.deliverPrivate();
      CXET_CHECK(f.state->observedWalletBalanceE8 == settledCash && f.state->position.netQty.raw == q);
      CXET_CHECK(!f.state->feePolicyKnown && !f.state->simulationEvidenceFailed);
    }},
    {"derivative_zero_cash_refuses_before_send", +[] {
      ExecutionFixture f(false);
      f.state->walletBalanceE8 = f.state->observedWalletBalanceE8 = 0;
      CXET_CHECK(!detail::execution_detail::submitOrderIntent(*f.state,
          ExecutionFixture::intent(1u, q), 101, 101));
      CXET_CHECK(f.state->pending.empty() && f.state->fills.empty());
      CXET_CHECK(f.state->orders.back().status == detail::kRecordStatusRejected);
      const auto view = detail::simulationOrderEvidenceView(*f.state);
      const auto* row = trading_core::strategy::canonicalOrderEvidenceIdentity(view, Id{1u}, 1u);
      CXET_CHECK(row && row->record && row->record->placeSettled);
      CXET_CHECK(row->record->placeSend == trading_core::execution::SendDisposition::NotAttempted);
      CXET_CHECK(f.state->observedReservedQuoteE8 == 0 && f.state->venueReservedQuoteE8 == 0);
    }},
    {"derivative_missing_margin_authority_refuses_new_risk", +[] {
      ExecutionFixture f(false, false);
      CXET_CHECK(!detail::execution_detail::submitOrderIntent(*f.state,
          ExecutionFixture::intent(1u, q), 101, 101));
      CXET_CHECK(f.state->pending.empty() && f.state->fills.empty());
      CXET_CHECK(f.state->orders.back().status == detail::kRecordStatusRejected);
      CXET_CHECK(!f.state->simulationOrders->collateral.known);
    }},
    {"derivative_admission_reserves_known_execution_loss_before_send", +[] {
      ExecutionFixture f(false);
      f.state->walletBalanceE8 = f.state->observedWalletBalanceE8 = 10 * q;
      CXET_CHECK(!detail::execution_detail::submitOrderIntent(*f.state,
          ExecutionFixture::intent(1u, q), 101, 101));
      CXET_CHECK(f.state->pending.empty() && f.state->fills.empty());
      CXET_CHECK(f.state->orders.back().status == detail::kRecordStatusRejected);
      CXET_CHECK(f.state->observedReservedQuoteE8 == 0);
    }},
    {"derivative_changed_latest_margin_fences_final_new_risk_admission", +[] {
      ExecutionFixture f(false);
      const auto latestMargin = 100 * q;
      detail::setSimulationDerivativeAuthorityCurrent(*f.state,
          f.state->simulationOrders->collateral.initialMarginE8 == latestMargin);
      CXET_CHECK(detail::configureSimulationDerivativeCollateral(*f.state, 10 * q, q, 1u));
      CXET_CHECK(!f.state->simulationOrders->collateral.authorityCurrent);
      CXET_CHECK(!detail::execution_detail::submitOrderIntent(*f.state,
          ExecutionFixture::intent(1u, q), 101, 101));
      CXET_CHECK(f.state->pending.empty() && f.state->fills.empty());
      CXET_CHECK(f.state->simulationOrders->collateral.initialMarginE8 == 10 * q);
    }},
    {"derivative_withdrawn_authority_rechecked_at_venue_and_private_delivery_continues", +[] {
      ExecutionFixture f(false);
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*f.state,
          ExecutionFixture::intent(1u, q, canon::OrderType::Limit, 99 * q), 101, 500));
      CXET_CHECK(f.state->observedReservedQuoteE8 == 10 * q);
      detail::setSimulationDerivativeAuthorityCurrent(*f.state, false);
      detail::applyDueActions(*f.state, 500);
      CXET_CHECK(f.state->netQtyRaw == 0 && f.state->fills.empty());
      CXET_CHECK(f.state->orders.back().status == detail::kRecordStatusRejected);
      f.deliverPrivate();
      CXET_CHECK(!f.state->simulationEvidenceFailed && f.state->observedReservedQuoteE8 == 0);
      CXET_CHECK(!f.state->simulationOrders->collateral.authorityCurrent);
    }},
    {"derivative_partial_fill_cancel_retains_only_owned_position_collateral", +[] {
      ExecutionFixture f(false);
      f.state->walletBalanceE8 = f.state->observedWalletBalanceE8 = 50 * q;
      f.maker(1u, 4 * q);
      CXET_CHECK(f.state->observedReservedQuoteE8 == 40 * q && f.state->venueReservedQuoteE8 == 40 * q);
      detail::FillExecutionMetadata metadata{}; metadata.fillReason = detail::kFillReasonPassiveQueue;
      CXET_CHECK(detail::fillOrderQty(*f.state, f.open(1u), 99 * q, 2 * q, f.nextTs++, false, metadata));
      CXET_CHECK(f.state->venueReservedQuoteE8 == 20 * q);
      CXET_CHECK(f.state->simulationOrders->collateral.venueProtectedE8 == 20 * q);
      CXET_CHECK(f.state->observedReservedQuoteE8 == 40 * q);
      trading_core::CancelIntent cancel{}; cancel.clientIntentId.raw = 11u; cancel.targetClientIntentId.raw = 1u;
      const auto now = f.nextTs++;
      CXET_CHECK(detail::execution_detail::submitCancelIntent(*f.state, cancel, now, now));
      detail::applyDueActions(*f.state, now);
      CXET_CHECK(f.state->venueReservedQuoteE8 == 0);
      CXET_CHECK(f.state->simulationOrders->collateral.venueProtectedE8 == 20 * q);
      f.deliverPrivate();
      CXET_CHECK(f.state->observedReservedQuoteE8 == 0);
      CXET_CHECK(f.state->simulationOrders->collateral.observedProtectedE8 == 20 * q);
      CXET_CHECK(!detail::execution_detail::submitOrderIntent(*f.state,
          ExecutionFixture::intent(2u, 4 * q), f.nextTs, f.nextTs));
      CXET_CHECK(f.state->simulationOrders->collateral.observedProtectedE8 == 20 * q);
    }},
    {"derivative_known_reduction_remains_serviceable_without_free_cash", +[] {
      ExecutionFixture f(false);
      f.place(ExecutionFixture::intent(1u, q)); f.deliverPrivate();
      CXET_CHECK(f.state->netQtyRaw == q && f.state->position.netQty.raw == q);
      f.state->walletBalanceE8 = f.state->observedWalletBalanceE8 = 0;
      auto close = ExecutionFixture::intent(2u, q); close.side = Side::Sell();
      f.place(close);
      CXET_CHECK(f.state->netQtyRaw == 0);
      CXET_CHECK(f.state->simulationOrders->collateral.venueProtectedE8 == 0);
      CXET_CHECK(!f.state->simulationEvidenceFailed);
    }},
    {"derivative_sources_share_one_account_collateral_ceiling", +[] {
      ExecutionFixture first(false, true, 0u), second(false, true, 1u);
      detail::SharedAccountState account{}; account.quoteAssetId = 1u;
      account.accountingModel = detail::SimulationAccountingModel::Linear;
      account.initialBalanceE8 = account.walletBalanceE8 = account.observedWalletBalanceE8 = 30 * q;
      account.observedInitialized = true;
      first.state->sharedAccount = second.state->sharedAccount = &account;
      hft_backtest::BacktestRateLimitSchedule schedule{}; schedule.enabled = false;
      std::string error; CXET_CHECK(hft_backtest::applyRateLimitSchedule(*first.state, schedule, error));
      first.maker(1u, 2 * q);
      CXET_CHECK(account.observedReservedQuoteE8 == 20 * q);
      CXET_CHECK(!detail::execution_detail::submitOrderIntent(*second.state,
          ExecutionFixture::intent(1u, 2 * q, canon::OrderType::Limit, 99 * q), 101, 101));
      CXET_CHECK(account.observedReservedQuoteE8 == 20 * q);
      CXET_CHECK(second.state->pending.empty() && second.state->fills.empty());
    }},
    {"execution_liquidity_capacity_refuses_new_consumption_and_reuses_fixed_storage", +[] {
      detail::ExecutionLiquidity ledger;
      CXET_CHECK(ledger.prepare(2u));
      const auto bytes = ledger.storageBytes();
      CXET_CHECK(bytes > 0u);
      CXET_CHECK(ledger.consume(trading_core::BookSide::Ask, 101 * q, q));
      CXET_CHECK(ledger.consume(trading_core::BookSide::Ask, 102 * q, q));
      CXET_CHECK(!ledger.canConsume(trading_core::BookSide::Ask, 103 * q, q));
      CXET_CHECK(!ledger.consume(trading_core::BookSide::Ask, 103 * q, q));
      CXET_CHECK(ledger.available(trading_core::BookSide::Ask, 101 * q, 2 * q) == q);
      const std::array<detail::PriceLevelRow, 1u> update{{{101 * q, 2 * q, 1}}};
      ledger.publish(update, false);
      CXET_CHECK(ledger.consume(trading_core::BookSide::Ask, 103 * q, q));
      CXET_CHECK(ledger.storageBytes() == bytes);
      ledger.publish({}, true);
      CXET_CHECK(ledger.available(trading_core::BookSide::Ask, 102 * q, 2 * q) == 2 * q);
      CXET_CHECK(ledger.storageBytes() == bytes);
    }},
    {"execution_liquidity_overflow_refuses_without_changing_consumed_state", +[] {
      detail::ExecutionLiquidity ledger;
      CXET_CHECK(ledger.prepare(1u));
      const auto maximum = std::numeric_limits<std::int64_t>::max();
      CXET_CHECK(ledger.consume(trading_core::BookSide::Bid, q, maximum));
      CXET_CHECK(!ledger.canConsume(trading_core::BookSide::Bid, q, 1));
      CXET_CHECK(!ledger.consume(trading_core::BookSide::Bid, q, 1));
      CXET_CHECK(ledger.available(trading_core::BookSide::Bid, q, maximum) == 0);
      CXET_CHECK(ledger.available(trading_core::BookSide::Ask, q, maximum) == maximum);
    }},
    {"depth_two_eight_unit_orders_share_ten_units_without_bbo_completion", +[] {
      ExecutionFixture f;
      f.place(ExecutionFixture::intent(1u, 8 * q));
      f.place(ExecutionFixture::intent(2u, 8 * q));
      CXET_CHECK(f.filled(1u) == 8 * q && f.filled(2u) == 2 * q);
      CXET_CHECK(f.state->netQtyRaw == 10 * q && f.state->depthFallbackFills == 0u);
      for (const auto& fill : f.state->fills)
        CXET_CHECK(fill.fillReason == detail::kFillReasonAggressiveBook);
    }},
    {"depth_repeated_matching_and_new_activation_do_not_refill_consumed_levels", +[] {
      ExecutionFixture f;
      f.place(ExecutionFixture::intent(1u, 10 * q));
      for (unsigned i = 0; i < 3u; ++i) detail::tryFillOnTop(*f.state, f.nextTs++, true);
      f.place(ExecutionFixture::intent(2u, q));
      CXET_CHECK(f.filled(1u) == 10 * q && f.filled(2u) == 0);
      CXET_CHECK(f.state->netQtyRaw == 10 * q);
    }},
    {"depth_unrelated_delta_preserves_consumption_but_same_timestamp_price_delta_refreshes", +[] {
      ExecutionFixture f;
      f.place(ExecutionFixture::intent(1u, 10 * q));
      f.publish({detail::PriceLevelRow{98 * q, 3 * q, 0}}, false, 100);
      f.place(ExecutionFixture::intent(2u, q));
      CXET_CHECK(f.filled(2u) == 0);
      f.publish({detail::PriceLevelRow{101 * q, 4 * q, 1}}, false, 100);
      f.place(ExecutionFixture::intent(3u, 6 * q));
      CXET_CHECK(f.filled(3u) == 4 * q && f.state->netQtyRaw == 14 * q);
    }},
    {"partial_market_with_depth_is_terminal_without_top_up_on_bbo", +[] {
      ExecutionFixture f;
      f.place(ExecutionFixture::intent(1u, 12 * q));
      CXET_CHECK(f.filled(1u) == 10 * q);
      detail::tryFillOnTop(*f.state, f.nextTs++, true);
      CXET_CHECK(f.filled(1u) == 10 * q && f.state->depthFallbackFills == 0u);
      for (const auto& order : f.state->open) CXET_CHECK(!order.state.valid);
      const auto& canceled = f.state->userEvents.back();
      CXET_CHECK(canceled.orderStatus == canon::OrderStatus::Canceled);
      CXET_CHECK(canceled.avgPrice.raw == 101 * q);
      CXET_CHECK(cxet::composite::userDataFieldPresent(canceled, cxet::composite::UserDataField::AvgPrice));
    }},
    {"accepted_depth_erase_stays_finite_despite_retained_positive_bbo", +[] {
      for (const bool removeBoth : {false, true}) {
        ExecutionFixture f;
        if (removeBoth) {
          f.publish({detail::PriceLevelRow{99 * q, 0, 0},
                     detail::PriceLevelRow{101 * q, 0, 1}}, false, 100);
        } else {
          f.publish({detail::PriceLevelRow{101 * q, 0, 1}}, false, 100);
        }
        const auto source = f.state->venueDepthBook.view();
        CXET_CHECK(source.ready && source.askCount == 0u);
        CXET_CHECK(source.bidCount == (removeBoth ? 0u : 1u));
        CXET_CHECK(f.state->venueOrderBook.asks.storedCount.raw == 0u);
        CXET_CHECK(detail::refreshReplayDepthProjection(f.state->venueOrderBook, source, Symbol{}));
        auto rejected = source;
        rejected.ready = false;
        CXET_CHECK(!detail::refreshReplayDepthProjection(f.state->venueOrderBook, rejected, Symbol{}));
        // Retained quotes are not a new market publication and cannot restore erased depth.
        f.top(99 * q, 101 * q, 10 * q);
        f.place(ExecutionFixture::intent(1u, q));
        CXET_CHECK(f.state->fills.empty() && f.state->netQtyRaw == 0);
        CXET_CHECK(!f.state->simulationEvidenceFailed && f.state->depthFallbackReason.empty());
        CXET_CHECK(f.state->depthFallbackFills == 0u);
        CXET_CHECK(f.state->depthExecutionMode == hft_backtest::BacktestDepthExecutionMode::Depth);
        CXET_CHECK(f.state->venueOrderBook.asks.storedCount.raw == 0u);
      }
    }},
    {"venue_trade_and_partial_bbo_publications_cannot_resurrect_erased_depth_ask", +[] {
      ExecutionFixture f;
      CXET_CHECK(trading_core::configureBboReconstruction(f.state->venueBboReconstruction, Price{q}));
      detail::SessionData session{};
      detail::BookTickerRow full{};
      full.bidPriceE8 = 99 * q; full.askPriceE8 = 101 * q;
      full.bidQtyE8 = full.askQtyE8 = 10 * q; full.tsNs = 110;
      session.bookTickers.push_back(full);
      detail::DepthRow erase{};
      erase.tsNs = 120; erase.levels = {{101 * q, 0, 1}};
      erase.arrival.sourceId = erase.arrival.sourceGeneration = 1u;
      erase.arrival.producerEpoch = erase.arrival.sessionEpoch = 1u;
      erase.arrival.receiveMonotonicNs = 120u;
      session.depths.push_back(erase);
      detail::TradeRow seller{};
      seller.priceE8 = 99 * q; seller.qtyE8 = q; seller.side = 0; seller.tsNs = 130;
      session.trades.push_back(seller);
      auto bidOnly = full;
      bidOnly.bidQtyE8 = 9 * q; bidOnly.askPresent = false; bidOnly.tsNs = 140;
      session.bookTickers.push_back(bidOnly);
      hft_backtest::StrategyInstrumentSpec instrument{}; instrument.tickSizeRaw = q;
      const auto apply = [&](std::uint8_t kind, std::size_t index, std::int64_t ts) {
        detail::TimelineEvent event{};
        event.kind = kind; event.index = index; event.tsNs = event.deliveryTsNs = ts;
        event.sourceId = event.sourceGeneration = 1u;
        event.producerEpoch = event.sessionEpoch = 1u;
        event.receiveMonotonicNs = ts;
        hft_backtest::applyMarketEventToSnapshots(*f.state, session, event, Symbol{}, instrument, false);
        CXET_CHECK(!f.state->depthReplayFailed && !f.state->simulationEvidenceFailed);
      };
      apply(detail::kTimelineKindBookTicker, 0u, 110);
      apply(detail::kTimelineKindDepth, 0u, 120);
      apply(detail::kTimelineKindTrade, 0u, 130);
      CXET_CHECK(f.state->venueOrderBook.asks.storedCount.raw == 0u);
      f.nextTs = 135;
      f.place(ExecutionFixture::intent(1u, q));
      CXET_CHECK(f.state->fills.empty() && f.state->depthFallbackFills == 0u);
      apply(detail::kTimelineKindBookTicker, 1u, 140);
      CXET_CHECK(f.state->venueSnapshot.reconstructedBookTicker.ask.px.raw == 101 * q);
      CXET_CHECK(f.state->venueOrderBook.asks.storedCount.raw == 0u);
      f.nextTs = 145;
      f.place(ExecutionFixture::intent(2u, q));
      CXET_CHECK(f.state->fills.empty() && f.state->depthFallbackFills == 0u);
      CXET_CHECK(!f.state->depthReplayFailed && !f.state->simulationEvidenceFailed);
    }},
    {"recorded_trade_through_consumes_better_depth_and_print_quantity_before_taker", +[] {
      ExecutionFixture f;
      f.publish({detail::PriceLevelRow{99 * q, 10 * q, 0},
                 detail::PriceLevelRow{98 * q, 3 * q, 0},
                 detail::PriceLevelRow{101 * q, 10 * q, 1}}, true, 100);
      f.trade(2 * q, 98 * q);
      CXET_CHECK(f.state->executionLiquidity.available(trading_core::BookSide::Bid, 99 * q, 10 * q) == 0);
      CXET_CHECK(f.state->executionLiquidity.available(trading_core::BookSide::Bid, 98 * q, 3 * q) == q);
      auto sell = ExecutionFixture::intent(1u, 10 * q);
      sell.side = Side::Sell();
      f.place(sell);
      CXET_CHECK(f.filled(1u) == q && f.state->fills.size() == 1u);
      CXET_CHECK(f.state->fills.back().priceE8 == 98 * q);
      CXET_CHECK(f.state->fills.back().fillReason == detail::kFillReasonAggressiveBook);
      CXET_CHECK(f.state->depthFallbackFills == 0u && !f.state->simulationEvidenceFailed);
    }},
    {"triggered_depth_fok_failure_does_not_spend_visible_liquidity", +[] {
      ExecutionFixture f;
      auto stop = ExecutionFixture::intent(1u, 12 * q, canon::OrderType::Stop, 101 * q);
      stop.triggerPrice.raw = 102 * q;
      stop.triggerSource = canon::OrderTriggerSource::Last;
      stop.triggeredTimeInForce = canon::TimeInForce::FOK;
      f.place(stop);
      f.open(1u).stopTriggered = true;
      detail::tryFillOnTop(*f.state, f.nextTs++, false);
      CXET_CHECK(f.filled(1u) == 0);
      f.place(ExecutionFixture::intent(2u, 10 * q));
      CXET_CHECK(f.filled(2u) == 10 * q);
    }},
    {"reduce_only_fok_cannot_partially_close_smaller_position_on_depth_or_bbo", +[] {
      for (const bool depth : {true, false}) {
        ExecutionFixture f(depth);
        f.publish({detail::PriceLevelRow{99 * q, 10 * q, 0},
                   detail::PriceLevelRow{101 * q, 8 * q, 1},
                   detail::PriceLevelRow{102 * q, 4 * q, 1}}, true, 100);
        f.state->netQtyRaw = f.state->position.netQty.raw = -10 * q;
        f.state->entryPriceE8 = f.state->position.entryPrice.raw = 100 * q;
        auto stop = ExecutionFixture::intent(1u, 12 * q, canon::OrderType::Stop, 102 * q);
        stop.positionReduction = trading_core::PositionReductionIntent::Reduce;
        stop.triggerPrice.raw = 103 * q;
        stop.triggerSource = canon::OrderTriggerSource::Last;
        stop.triggeredTimeInForce = canon::TimeInForce::FOK;
        f.place(stop);
        f.open(1u).stopTriggered = true;
        const auto wallet = f.state->walletBalanceE8;
        detail::tryFillOnTop(*f.state, f.nextTs++, false);
        CXET_CHECK(f.state->fills.empty() && f.state->netQtyRaw == -10 * q);
        CXET_CHECK(f.state->walletBalanceE8 == wallet && !f.state->simulationEvidenceFailed);
        CXET_CHECK(f.state->orders.back().status == detail::kRecordStatusPartialUnfilled);
        CXET_CHECK(f.state->userEvents.back().orderStatus == canon::OrderStatus::Canceled);
        CXET_CHECK(f.state->executionLiquidity.available(trading_core::BookSide::Ask, 101 * q, 8 * q) == 8 * q);
        CXET_CHECK(f.state->executionLiquidity.available(trading_core::BookSide::Ask, 102 * q, 4 * q) == 4 * q);
      }
    }},
    {"depth_fok_preflights_total_ledger_capacity_before_spending_first_chunk", +[] {
      ExecutionFixture f;
      CXET_CHECK(f.state->executionLiquidity.prepare(2u));
      f.publish({detail::PriceLevelRow{99 * q, q, 0},
                 detail::PriceLevelRow{101 * q, q, 1},
                 detail::PriceLevelRow{102 * q, q, 1}}, true, 100);
      CXET_CHECK(f.state->executionLiquidity.consume(trading_core::BookSide::Ask, 100 * q, q));
      auto stop = ExecutionFixture::intent(1u, 2 * q, canon::OrderType::Stop, 102 * q);
      stop.triggerPrice.raw = 103 * q;
      stop.triggerSource = canon::OrderTriggerSource::Last;
      stop.triggeredTimeInForce = canon::TimeInForce::FOK;
      f.place(stop);
      f.open(1u).stopTriggered = true;
      const auto wallet = f.state->walletBalanceE8;
      detail::tryFillOnTop(*f.state, f.nextTs++, false);
      CXET_CHECK(f.state->fills.empty() && f.state->netQtyRaw == 0);
      CXET_CHECK(f.state->walletBalanceE8 == wallet && !f.state->simulationEvidenceFailed);
      CXET_CHECK(f.state->orders.back().status == detail::kRecordStatusPartialUnfilled);
      CXET_CHECK(f.state->userEvents.back().orderStatus == canon::OrderStatus::Canceled);
      CXET_CHECK(f.state->executionLiquidity.available(trading_core::BookSide::Ask, 101 * q, q) == q);
      CXET_CHECK(f.state->executionLiquidity.available(trading_core::BookSide::Ask, 102 * q, q) == q);
      CXET_CHECK(f.state->executionLiquidity.available(trading_core::BookSide::Ask, 100 * q, 2 * q) == q);
    }},
    {"bbo_no_depth_explicitly_fills_full_quantity_even_with_zero_displayed_size", +[] {
      ExecutionFixture f(false);
      f.top(99 * q, 101 * q, 0);
      f.place(ExecutionFixture::intent(1u, 80 * q));
      f.place(ExecutionFixture::intent(2u, 80 * q, canon::OrderType::Limit, 101 * q));
      CXET_CHECK(f.filled(1u) == 80 * q && f.filled(2u) == 80 * q);
      CXET_CHECK(f.state->depthFallbackFills == 2u);
      for (const auto& fill : f.state->fills)
        CXET_CHECK(fill.priceE8 == 101 * q && fill.fillReason == detail::kFillReasonTop);
    }},
    {"resting_maker_requires_trade_and_bbo_touch_never_executes_it", +[] {
      ExecutionFixture f(false);
      f.maker(1u, 4 * q);
      f.top(98 * q, 99 * q, 100 * q);
      detail::tryFillOnTop(*f.state, f.nextTs++, true);
      CXET_CHECK(f.filled(1u) == 0);
      f.trade(2 * q);
      CXET_CHECK(f.filled(1u) == 2 * q);
      CXET_CHECK(f.state->fills.back().liquidity == detail::kFillLiquidityMaker);
    }},
    {"ordinary_gtc_limit_rests_after_initial_evaluation_and_ignores_later_bbo_cross", +[] {
      ExecutionFixture f(false);
      f.place(ExecutionFixture::intent(1u, 4 * q, canon::OrderType::Limit, 99 * q));
      CXET_CHECK(f.filled(1u) == 0);
      f.top(98 * q, 99 * q, 100 * q);
      detail::tryFillOnTop(*f.state, f.nextTs++, true);
      CXET_CHECK(f.filled(1u) == 0 && f.state->depthFallbackFills == 0u);
      f.trade(2 * q);
      CXET_CHECK(f.filled(1u) == 2 * q);
      CXET_CHECK(f.state->fills.back().liquidity == detail::kFillLiquidityMaker);
    }},
    {"triggered_bbo_ioc_and_fok_fill_full_quantity_without_displayed_size_cap", +[] {
      for (const auto tif : {canon::TimeInForce::IOC, canon::TimeInForce::FOK}) {
        ExecutionFixture f(false);
        auto stop = ExecutionFixture::intent(1u, 80 * q, canon::OrderType::Stop, 101 * q);
        stop.triggerPrice.raw = 102 * q;
        stop.triggerSource = canon::OrderTriggerSource::Last;
        stop.triggeredTimeInForce = tif;
        f.place(stop);
        CXET_CHECK(f.filled(1u) == 0);
        f.open(1u).stopTriggered = true;
        f.top(99 * q, 101 * q, 0);
        detail::tryFillOnTop(*f.state, f.nextTs++, false);
        CXET_CHECK(f.filled(1u) == 80 * q && f.state->depthFallbackFills == 1u);
        CXET_CHECK(f.state->fills.back().priceE8 == 101 * q);
      }
    }},
    {"maker_trade_spends_common_external_queue_once_then_one_shared_trade_volume", +[] {
      ExecutionFixture f;
      f.maker(1u, 8 * q); f.maker(2u, 8 * q);
      f.trade(14 * q);
      CXET_CHECK(f.filled(1u) == 4 * q && f.filled(2u) == 0);
      f.trade(8 * q);
      CXET_CHECK(f.filled(1u) == 8 * q && f.filled(2u) == 4 * q);
      CXET_CHECK(f.open(2u).queueAheadQtyRaw == 0);
    }},
    {"new_maker_queue_uses_liquidity_remaining_after_prior_recorded_trade", +[] {
      ExecutionFixture f;
      f.trade(6 * q);
      CXET_CHECK(f.state->fills.empty());
      f.maker(1u, 4 * q);
      CXET_CHECK(f.open(1u).queueAheadQtyRaw == 4 * q);
      f.trade(8 * q);
      CXET_CHECK(f.filled(1u) == 4 * q && f.state->fills.size() == 1u);
      trading_core::LocalOrderBookLevel bid{};
      CXET_CHECK(trading_core::orderBook(f.state->venueOrderBook).readStoredLevel(
          trading_core::BookSide::Bid, Index{0u}, bid));
      CXET_CHECK(bid.px.raw == 99 * q && bid.qty.raw == 10 * q);
      CXET_CHECK(f.state->executionLiquidity.available(trading_core::BookSide::Bid, 99 * q, 10 * q) == 0);
      CXET_CHECK(f.state->depthFallbackFills == 0u && !f.state->simulationEvidenceFailed);
    }},
    {"maker_fifo_uses_activation_order_after_cancel_and_slot_reuse", +[] {
      ExecutionFixture f(false);
      f.nextTs = 101;
      f.maker(1u, 4 * q);
      f.nextTs = 101;
      f.maker(2u, 4 * q);
      trading_core::CancelIntent cancel{};
      cancel.clientIntentId.raw = 11u; cancel.targetClientIntentId.raw = 1u;
      f.nextTs = 101;
      const auto ts = f.nextTs++;
      CXET_CHECK(detail::execution_detail::submitCancelIntent(*f.state, cancel, ts, ts));
      detail::applyDueActions(*f.state, ts);
      f.nextTs = 101;
      f.maker(3u, 4 * q);
      CXET_CHECK(f.open(2u).activeTsNs == 101 && f.open(3u).activeTsNs == 101);
      CXET_CHECK(f.open(2u).activationSequence < f.open(3u).activationSequence);
      f.trade(5 * q);
      CXET_CHECK(f.filled(1u) == 0 && f.filled(2u) == 4 * q && f.filled(3u) == q);
    }},
    {"cumulative_average_spans_fill_events_and_matches_private_report", +[] {
      ExecutionFixture f(false);
      f.maker(1u, 5 * q);
      auto& order = f.open(1u);
      detail::FillExecutionMetadata metadata{}; metadata.fillReason = detail::kFillReasonPassiveQueue;
      CXET_CHECK(detail::fillOrderQty(*f.state, order, 90 * q, 2 * q, f.nextTs++, false, metadata));
      CXET_CHECK(f.state->fills.back().avgPriceE8 == 90 * q);
      CXET_CHECK(detail::fillOrderQty(*f.state, order, 95 * q, 3 * q, f.nextTs++, false, metadata));
      CXET_CHECK(f.state->fills.back().avgPriceE8 == 93 * q);
      CXET_CHECK(f.state->fills.back().cumulativeFilledQtyE8 == 5 * q);
      CXET_CHECK(f.state->userEvents.back().avgPrice.raw == 93 * q);
      CXET_CHECK(f.state->userEvents.back().filledQty.raw == 5 * q);
    }},
};
}

int main(int argc, char** argv) { return cxet::testing::runCases(argc, argv, cases); }
