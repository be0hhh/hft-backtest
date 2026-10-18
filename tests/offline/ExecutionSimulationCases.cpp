#include "OfflineCase.hpp"
#include "Engine/BacktestEngine.hpp"
#include "Engine/BacktestEngineSetup.hpp"
#include "Execution/BacktestOrderEvidence.hpp"
#include "Execution/Internal/BacktestExecutionHelpers.hpp"
#include "Strategy/BacktestStrategyRuntime.hpp"

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

  explicit ExecutionFixture(bool depth = true) {
    auto& e = *state;
    e.market = canon::kMarketTypeFutures;
    e.exchange = detail::exchangeFromText("binance");
    e.symbol.copyFrom("BTC_USDT");
    e.activeContextIndex = 1u;
    hft_backtest::BacktestRateLimitSchedule schedule{}; schedule.enabled = false;
    std::string error;
    CXET_CHECK(hft_backtest::applyRateLimitSchedule(e, schedule, error));
    hft_backtest::BacktestRunRequest request{};
    request.initialBalanceE8 = 1'000'000 * q;
    request.makerFeeBpsE8 = request.takerFeeBpsE8 = 0;
    detail::initializeAccounting(e, request);
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
};

const cxet::testing::Case cases[]{
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
