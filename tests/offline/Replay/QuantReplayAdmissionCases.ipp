    cxet::testing::Case{"service_unwind_without_genuine_opposite_price_retains_spot_and_futures_exposure", +[] {
      const std::array scenarios{
          std::pair{canon::kMarketTypeSpot, detail::SimulationAccountingModel::Cash},
          std::pair{canon::kMarketTypeFutures, detail::SimulationAccountingModel::Linear}};
      for (const auto [market, model] : scenarios) {
        SpotFixture fixture; auto engine = fixture.engine(0u, market, model);
        const auto probe = SpotFixture::intent(212u, Side::Buy(), SpotFixture::q, canon::OrderType::Limit, 50 * SpotFixture::q);
        CXET_CHECK(detail::execution_detail::submitOrderIntent(*engine, probe, 10, 20, true, 0u, false)); detail::applyDueActions(*engine, 20);
        engine->venueSnapshot.lastBookTicker.bid.px.raw = 0;
        CXET_CHECK(detail::applyAcceptedOrderFill(*engine, engine->open[0], 50 * SpotFixture::q, SpotFixture::q, 0, 30, true, {}));
        CXET_CHECK(engine->netQtyRaw == SpotFixture::q && engine->serviceExposureOpen && engine->fills.size() == 1u);
        if (model == detail::SimulationAccountingModel::Linear) {
          CXET_CHECK(engine->simulationOrders->collateral.venueUnavailable);
          CXET_CHECK(!engine->simulationEvidenceFailed);
        }
        const auto cash = (market.raw == canon::kMarketTypeSpot.raw ? 50 : 100) * SpotFixture::q;
        CXET_CHECK(fixture.account.walletBalanceE8 == cash);
        SpotFixture::deliverAll(*engine);
        CXET_CHECK(engine->position.netQty.raw == SpotFixture::q && hft_backtest::accountStateFromEngine(*engine).walletBalanceE8 == cash);
      }
    }},
    cxet::testing::Case{"warmup_ack_is_unknown_until_correlated_private_delivery", +[] {
      SpotFixture fixture; auto engine = fixture.engine(0u);
      const auto probe = SpotFixture::intent(213u, Side::Buy(), SpotFixture::q, canon::OrderType::Limit, 50 * SpotFixture::q);
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*engine, probe, 10, 20, true, 0u, false));
      const auto submitted = hft_backtest::strategy_runtime_detail::backtestWarmupView(*engine);
      CXET_CHECK(submitted.warmupInFlight && submitted.lastAckMono.raw == 0u && !submitted.routeHot);
      detail::applyDueActions(*engine, 20);
      const auto activated = hft_backtest::strategy_runtime_detail::backtestWarmupView(*engine);
      CXET_CHECK(activated.warmupInFlight && activated.lastAckMono.raw == 0u && !activated.routeHot);
      const auto* record = trading_core::strategy::canonicalOrderEvidence(detail::simulationOrderEvidenceView(*engine), probe.clientIntentId, 1u);
      CXET_CHECK(record && record->placeSettled && !record->venueObserved);
      CXET_CHECK(engine->userEvents.size() == 1u && engine->userEventDeliveryTsNs[0] > 20);
      CXET_CHECK(!detail::deliverSimulationOrderEvent(*engine, 0u, engine->userEventDeliveryTsNs[0] - 1));
      CXET_CHECK(detail::deliverSimulationOrderEvent(*engine, 0u, engine->userEventDeliveryTsNs[0]));
      const auto received = hft_backtest::strategy_runtime_detail::backtestWarmupView(*engine);
      CXET_CHECK(!received.warmupInFlight && received.routeHot && record->venueObserved);
      CXET_CHECK(received.lastAckMono.raw == static_cast<std::uint64_t>(engine->userEventDeliveryTsNs[0]));
      CXET_CHECK(detail::fillOrderQty(*engine, engine->open[0], 50 * SpotFixture::q, SpotFixture::q, 2000, true, {}));
      CXET_CHECK(engine->netQtyRaw == 0 && engine->userEvents.size() == 4u);
      CXET_CHECK(!hft_backtest::strategy_runtime_detail::backtestWarmupView(*engine).serviceExposureOpen);
      CXET_CHECK(detail::deliverSimulationOrderEvent(*engine, 1u, engine->userEventDeliveryTsNs[1]));
      CXET_CHECK(engine->position.netQty.raw == SpotFixture::q &&
          hft_backtest::strategy_runtime_detail::backtestWarmupView(*engine).serviceExposureOpen);
      CXET_CHECK(detail::deliverSimulationOrderEvent(*engine, 2u, engine->userEventDeliveryTsNs[2]));
      CXET_CHECK(hft_backtest::strategy_runtime_detail::backtestWarmupView(*engine).serviceExposureOpen);
      CXET_CHECK(detail::deliverSimulationOrderEvent(*engine, 3u, engine->userEventDeliveryTsNs[3]));
      CXET_CHECK(engine->position.netQty.raw == 0 &&
          !hft_backtest::strategy_runtime_detail::backtestWarmupView(*engine).serviceExposureOpen);
    }},
    cxet::testing::Case{"whole_market_available_recorded_only_required_source_is_not_a_quiet_pause", +[] {
      QuantityFixtureConfig config; config.wholeUniverseSources = config.recordedOnlyRequiredSource = true;
      QuantFixture fixture(0, true, 1u, config); const auto result = fixture.load("LTC_USDT");
      CXET_CHECK(result.status == hft_backtest::Status::Unsupported && result.error.find("replay") != std::string::npos);
    }},
    cxet::testing::Case{"whole_market_nonhealth_required_row_without_replay_compatibility_refuses", +[] {
      QuantityFixtureConfig config; config.wholeUniverseSources = config.noncompatibleRequiredRow = true;
      QuantFixture fixture(0, true, 1u, config); const auto result = fixture.load("LTC_USDT");
      CXET_CHECK(result.status == hft_backtest::Status::Unsupported && result.error.find("replay") != std::string::npos);
    }},
