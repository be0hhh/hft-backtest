    cxet::testing::Case{"binary_stream_replays_real_membership_without_synthetic_market_frames", +[] {
      BinaryTradeFixture fixture(2u, 199u, 7u, 1u, false, true);
      detail::BinaryCorpusStream stream; std::string error;
      const std::array requests{fixture.request()};
      CXET_CHECK(stream.open(fixture.root, requests, fixture.plan(), error) == hft_backtest::Status::Ok);
      std::vector<detail::TimelineEvent> events; bool available = false;
      for (std::size_t index = 0u; index < 4u; ++index) {
        CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Ok && available && events.size() == 1u);
        const auto& event = events[0];
        if (index == 1u || index == 2u) {
          CXET_CHECK(event.kind == detail::kTimelineKindLifecycle && event.applicationOnly);
          const auto& row = stream.sessions()[0].sourceLifecycle[0];
          CXET_CHECK(row.arrival.frameSequence == 0u && row.arrival.sourceGeneration == 11u);
          CXET_CHECK(row.arrival.sessionEpoch == (index == 1u ? 17u : 18u));
        } else CXET_CHECK(event.kind == detail::kTimelineKindTrade && !event.applicationOnly);
        CXET_CHECK(event.deliveryTsNs == static_cast<std::int64_t>(200u + index * 50u));
      }
      CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Ok && !available);
      auto interval = fixture.request(); interval.beginReceiveNs = 275;
      const std::array intervalRequests{interval};
      CXET_CHECK(stream.open(fixture.root, intervalRequests, fixture.plan(), error) == hft_backtest::Status::Ok);
      CXET_CHECK(!stream.sessions()[0].capturedInitiallyPresent &&
          stream.sessions()[0].capturedDirectoryRevision == 2u);
      CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Ok && available && events.size() == 1u);
      CXET_CHECK(events[0].kind == detail::kTimelineKindLifecycle && events[0].deliveryTsNs == 300);
    }},
    cxet::testing::Case{"administrative_lifecycle_arrival_is_not_a_market_application_frame", +[] {
      hftrec::replay::EventArrival arrival;
      arrival.receiveRealtimeNs = 100; arrival.receiveMonotonicNs = 10;
      arrival.producerEpoch = arrival.sourceGeneration = arrival.shardSequence = 1u;
      arrival.sourceId = 1u;
      arrival.flags = hftrec::replay::EventArrivalSourceLifecycle;
      CXET_CHECK(hftrec::replay::hasCapturedSourceLifecycleArrival(arrival));
      CXET_CHECK(!hftrec::replay::hasCapturedApplicationArrival(arrival));
      arrival.flags |= hftrec::replay::EventArrivalApplicationFrame;
      arrival.frameSequence = arrival.sessionEpoch = 1u;
      CXET_CHECK(!hftrec::replay::hasCapturedSourceLifecycleArrival(arrival));
      CXET_CHECK(!hftrec::replay::hasCapturedApplicationArrival(arrival));
    }},
    cxet::testing::Case{"same_generation_partial_bbo_readd_preserves_trade_and_fences_old_actions", +[] {
      namespace corpus = hftrec::corpus;
      auto engine = std::make_unique<detail::EngineState>();
      engine->capturedSourceManaged = true;
      engine->capturedSourceGeneration = 11u;
      const auto bbo = corpus::binaryMarketChannelBit(corpus::BinaryMarketChannel::BookTicker);
      const auto trade = corpus::binaryMarketChannelBit(corpus::BinaryMarketChannel::Trade);
      engine->capturedActiveChannels = bbo | trade;
      engine->capturedExecutionRequiredChannels = bbo;
      engine->snapshot.hasTrade = trading_core::kAlgoFlagTrue;
      engine->snapshot.lastTrade.ts.raw = 123u;
      engine->snapshot.hasBookTicker = trading_core::kAlgoFlagTrue;
      detail::PendingAction old; old.activeTsNs = 150; old.sourceGeneration = 11u;
      engine->pending.push_back(old);
      engine->open[0].state.valid = true; engine->open[0].sourceGeneration = 11u;
      engine->netQtyRaw = 7;
      detail::SourceLifecycleRow row;
      row.arrival.sourceId = row.payload.source.sourceId = 1u;
      row.arrival.sourceGeneration = row.payload.source.initialSourceGeneration = 11u;
      row.payload.previousSourceGeneration = 11u;
      row.payload.directoryRevision = 1u;
      row.payload.channelMaskBefore = bbo | trade; row.payload.channelMaskAfter = trade;
      row.payload.affectedChannel = static_cast<std::uint8_t>(corpus::BinaryMarketChannel::BookTicker);
      row.payload.kind = corpus::BinaryMarketSourceLifecycleKind::Removed;
      std::string error;
      CXET_CHECK(hft_backtest::run_detail::applyCapturedSourceLifecycle(*engine, row, 200, error));
      CXET_CHECK(engine->snapshot.hasTrade.raw == 1u && engine->snapshot.lastTrade.ts.raw == 123u);
      CXET_CHECK(engine->snapshot.hasBookTicker.raw == 0u && engine->capturedSourceMember);
      row.payload.directoryRevision = 2u;
      row.payload.channelMaskBefore = trade; row.payload.channelMaskAfter = bbo | trade;
      row.payload.kind = corpus::BinaryMarketSourceLifecycleKind::Readded;
      row.arrival.sessionEpoch = 19u;
      CXET_CHECK(hft_backtest::run_detail::applyCapturedSourceLifecycle(*engine, row, 300, error));
      CXET_CHECK(engine->capturedSourceGeneration == 11u && engine->snapshot.lastTrade.ts.raw == 123u);
      CXET_CHECK(hft_backtest::strategy_runtime_detail::nextPendingTs(*engine) == std::numeric_limits<std::int64_t>::max());
      detail::applyDueActions(*engine, 400);
      CXET_CHECK(engine->pending.size() == 1u && engine->open[0].state.valid && engine->netQtyRaw == 7);
      detail::PendingAction current; current.activeTsNs = 410; current.sourceGeneration = 11u;
      current.executionBoundaryEpoch = engine->executionBoundaryEpoch;
      engine->pending.push_back(current);
      CXET_CHECK(hft_backtest::strategy_runtime_detail::nextPendingTs(*engine) == 410);
    }},
    cxet::testing::Case{"venue_wallet_configuration_seeds_account_once_and_explicit_override_wins", +[] {
      BinaryTradeFixture fixture(2u, 199u, 7u);
      const auto configPath = fixture.root / "strategy.ini";
      {
        std::ofstream config(configPath);
        config << "[strategy]\ntype=accounting_fixture\n"
            "[venue.binance_spot]\nsymbols=BTC_USDT\nrate_limits_enabled=false\ninitial_balance_usdt=12.5\n";
      }
      hft_backtest::BacktestStrategyConfig config; std::string error;
      CXET_CHECK(hft_backtest::loadBacktestStrategyConfig(configPath.c_str(), config, error));
      CXET_CHECK(config.venues.size() == 1u && config.venues[0].hasInitialBalance);
      hft_backtest::BacktestRunRequest request; request.initialBalanceE8 = 999;
      CXET_CHECK(hft_backtest::run_detail::virtualAccountInitialBalance(request, config.venues[0], 0u) == 1'250'000'000);
      request.legInitialBalancesE8 = {5};
      CXET_CHECK(hft_backtest::run_detail::virtualAccountInitialBalance(request, config.venues[0], 0u) == 5);
    }},
    cxet::testing::Case{"source_removal_preserves_orders_and_fences_old_generation_pending", +[] {
      auto engine = std::make_unique<detail::EngineState>();
      engine->capturedSourceManaged = true;
      engine->capturedSourceGeneration = 1u;
      detail::PendingAction old;
      old.activeTsNs = 100; old.sourceGeneration = 1u;
      engine->pending.push_back(old);
      engine->open[0].state.valid = true;
      engine->open[0].sourceGeneration = 1u;
      engine->netQtyRaw = 7;
      engine->capturedSourceMember = false;
      CXET_CHECK(hft_backtest::strategy_runtime_detail::nextPendingTs(*engine) == std::numeric_limits<std::int64_t>::max());
      detail::applyDueActions(*engine, 200);
      CXET_CHECK(engine->pending.size() == 1u && engine->open[0].state.valid && engine->netQtyRaw == 7);
      engine->capturedSourceMember = true; engine->capturedSourceGeneration = 2u;
      engine->replayNotBeforeNs = 300;
      CXET_CHECK(hft_backtest::strategy_runtime_detail::nextPendingTs(*engine) == std::numeric_limits<std::int64_t>::max());
      detail::PendingAction current; current.activeTsNs = 310; current.sourceGeneration = 2u;
      engine->pending.push_back(current);
      CXET_CHECK(hft_backtest::strategy_runtime_detail::nextPendingTs(*engine) == 310);
    }},
    cxet::testing::Case{"binary_stream_commits_full_rebase_before_exposing_one_transaction", +[] {
      BinaryTradeFixture fixture(0u, 0u, 0u, 1u, true);
      detail::SessionLoadPlan plan{};
      plan.loadTrades = plan.loadBookTickers = false;
      plan.loadDepths = plan.requireDepths = true;
      detail::BinaryCorpusStream stream; std::string error;
      const std::array requests{fixture.request()};
      CXET_CHECK(stream.open(fixture.root, requests, plan, error) == hft_backtest::Status::Ok);
      std::vector<detail::TimelineEvent> events; bool available = false;
      CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Ok && available);
      CXET_CHECK(events.size() == 2u && stream.sessions()[0].depths.size() == 2u);
      CXET_CHECK(events[0].exchangeTsNs == 199 && events[1].exchangeTsNs == 150 &&
          events[0].deliveryTsNs == events[1].deliveryTsNs &&
          events[0].shardSequence == 1u && events[1].shardSequence == 2u);
      auto engine = std::make_unique<detail::EngineState>();
      CXET_CHECK(engine->strategyDepthBook.prepare());
      CXET_CHECK(engine->orderBook.reserveLevelsPerSide(CountVal{8u}));
      hft_backtest::StrategyInstrumentSpec instrument; instrument.tickSizeRaw = 1;
      for (const auto& event : events)
        hft_backtest::applyMarketEventToSnapshots(*engine, stream.sessions()[0], event, Symbol{}, instrument, true);
      CXET_CHECK(!engine->depthReplayFailed);
      trading_core::LocalOrderBookLevel bid;
      CXET_CHECK(trading_core::orderBook(engine->orderBook).readStoredLevel(
          trading_core::BookSide::Bid, Index{0u}, bid));
      CXET_CHECK(bid.px.raw == 100 && bid.qty.raw == 3);
    }},
    cxet::testing::Case{"overlapping_context_local_ids_do_not_adopt_orders_or_stale_actions", +[] {
      auto engine = std::make_unique<detail::EngineState>();
      prepareCashTradeReplayEngine(*engine);
      hft_backtest::BacktestRateLimitSchedule schedule; schedule.enabled = false;
      std::string quotaError;
      CXET_CHECK(hft_backtest::applyRateLimitSchedule(*engine, schedule, quotaError));
      trading_core::OrderIntent intent{};
      intent.clientIntentId.raw = 7u; intent.orderType = canon::OrderType::Limit;
      intent.side = Side::Buy(); intent.qty.raw = 4; intent.price.raw = 100;
      engine->activeContextIndex = 1u;
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*engine, intent, 10, 20));
      engine->activeContextIndex = 2u;
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*engine, intent, 10, 20));
      detail::applyDueActions(*engine, 20);
      std::array<trading_core::StrategyOrderState, 2> output;
      CXET_CHECK(detail::visibleOrders(*engine, output.data(), output.size()) == 1u);
      CXET_CHECK(detail::execution_detail::findOpenIndex(*engine, Id{7u}) == 1u);
      detail::PendingAction stale;
      stale.contextIndex = 1u; stale.targetClientIntentId.raw = 7u;
      CXET_CHECK(!detail::execution_detail::matchesExpectedOrder(engine->open[1], stale));
      hft_backtest::BacktestRunResult result;
      hft_backtest::populateResultPnl(result, *engine);
      CXET_CHECK(!result.strategyClosed && result.openPositionQtyE8 == 0);
    }},
