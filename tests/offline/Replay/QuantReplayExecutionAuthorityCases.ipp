    cxet::testing::Case{"captured_canonical_and_executable_steps_do_not_use_native_contract_lot", +[] {
      QuantFixture fixture(0, true);
      detail::SessionLoadPlan plan; plan.loadTrades = false; plan.requireBookTickers = true;
      const std::array<hft_backtest::BacktestCorpusSourceRequest, 1> requests{
          hft_backtest::BacktestCorpusSourceRequest{"binance", "spot", "BTC_USDT", 100, 10'000}};
      detail::BinaryCorpusStream stream; std::string error;
      CXET_CHECK(stream.open(fixture.root, requests, plan, error) == hft_backtest::Status::Ok);
      CXET_CHECK(stream.sessions()[0].instrument.lotSizeE8 == 2);
      CXET_CHECK(stream.sessions()[0].instrument.executionQuantityStepE8 == 4);
      trading_core::StrategyInstrumentSpec instrument{};
      CXET_CHECK(hft_backtest::prepareBacktestStrategyInstrument(stream.sessions()[0].instrument, instrument, error) == hft_backtest::Status::Ok);
      CXET_CHECK(instrument.stepSizeRaw == 2 && instrument.executionQuantityStepRaw == 4);
    }},
    cxet::testing::Case{"native_lot_without_execution_quantity_authority_refuses_before_strategy_init", +[] {
      QuantFixture fixture; const auto result = fixture.load();
      CXET_CHECK(result.status == hft_backtest::Status::Ok);
      trading_core::StrategyInstrumentSpec instrument{}; std::string error;
      CXET_CHECK(hft_backtest::prepareBacktestStrategyInstrument(result.session.instrument, instrument, error) == hft_backtest::Status::Unsupported);
      CXET_CHECK(error.find("execution quantity") != std::string::npos);
    }},
    cxet::testing::Case{"nonunit_native_price_basis_refuses_e8_accounting_before_admission", +[] {
      QuantFixture fixture(200'000'000, true); const auto result = fixture.load();
      CXET_CHECK(result.status == hft_backtest::Status::Ok);
      trading_core::StrategyInstrumentSpec instrument{}; std::string error;
      CXET_CHECK(hft_backtest::prepareBacktestStrategyInstrument(result.session.instrument, instrument, error) == hft_backtest::Status::Unsupported);
      CXET_CHECK(error.find("price basis") != std::string::npos);
    }},
    cxet::testing::Case{"user_data_jitter_keeps_source_order_and_deterministic_ties", +[] {
      auto first = std::make_unique<detail::EngineState>();
      auto repeat = std::make_unique<detail::EngineState>();
      detail::BacktestLatencyConfig config; config.seed = 7u; config.userData = {1000u, 1000u};
      detail::initLatencyRuntime(first->latency, config);
      detail::initLatencyRuntime(repeat->latency, config);
      for (std::uint64_t i = 0u; i < 128u; ++i) {
        cxet::composite::UserDataEvent event{}; event.eventId.raw = i + 1u;
        event.eventTime.raw = event.updateTime.raw = 100u + i;
        Id intent{}; intent.raw = i + 11u;
        detail::execution_detail::pushUserEvent(*first, event, 100 + i, intent, 1u);
        detail::execution_detail::pushUserEvent(*repeat, event, 100 + i, intent, 1u);
        CXET_CHECK(first->userEventDeliveryTsNs[i] == repeat->userEventDeliveryTsNs[i]);
        if (i != 0u) CXET_CHECK(first->userEventDeliveryTsNs[i] >= first->userEventDeliveryTsNs[i - 1u]);
        CXET_CHECK(first->userEvents[i].eventId.raw == i + 1u && first->userEvents[i].updateTime.raw == 100u + i);
        CXET_CHECK(first->userEventClientIntentIds[i].raw == i + 11u && first->userEventContextIndices[i] == 1u);
      }
    }},
