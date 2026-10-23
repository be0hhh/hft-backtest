    cxet::testing::Case{"native_channel_close_preserves_membership_and_fences_execution", +[] {
      namespace corpus=hftrec::corpus;
      auto engine=std::make_unique<detail::EngineState>();
      const auto bbo=corpus::binaryMarketChannelBit(corpus::BinaryMarketChannel::BookTicker);
      engine->capturedSourceManaged=true;
      engine->capturedSourceMember=true;engine->capturedSourceGeneration=11u;
      engine->capturedExecutionRequiredChannels=engine->capturedActiveChannels=bbo;
      engine->snapshot.hasBookTicker=trading_core::kAlgoFlagTrue;
      engine->pending.push_back({});engine->pending[0].sourceGeneration=11u;
      engine->open[0].state.valid=true;engine->netQtyRaw=7;
      detail::SourceLifecycleRow row;
      row.arrival.sourceId=row.payload.source.sourceId=1u;
      row.arrival.sourceGeneration=row.payload.source.initialSourceGeneration=row.payload.previousSourceGeneration=11u;
      row.payload.channelMaskBefore=row.payload.channelMaskAfter=bbo;row.payload.directoryRevision=1u;
      row.payload.kind=corpus::BinaryMarketSourceLifecycleKind::DirectoryChanged;
      row.payload.affectedChannel=1u;row.payload.cause=corpus::BinaryMarketSourceTransitionCause::NativeClosed;
      std::string error;
      CXET_CHECK(hft_backtest::run_detail::applyCapturedSourceLifecycle(*engine,row,200,error));
      CXET_CHECK(engine->capturedSourceMember && engine->capturedActiveChannels==bbo && engine->capturedUnhealthyChannels==bbo);
      detail::applyDueActions(*engine,250);
      CXET_CHECK(engine->pending.size()==1u && engine->open[0].state.valid && engine->netQtyRaw==7);
      row.payload.directoryRevision=2u;row.payload.cause=corpus::BinaryMarketSourceTransitionCause::NativeRecovered;
      CXET_CHECK(hft_backtest::run_detail::applyCapturedSourceLifecycle(*engine,row,300,error));
      CXET_CHECK(engine->capturedUnhealthyChannels==0u && engine->snapshot.hasBookTicker.raw==0u && engine->replayNotBeforeNs==300);
    }},
    cxet::testing::Case{"native_depth_gap_without_replay_compatibility_is_exact_health_state", +[] {
      namespace corpus = hftrec::corpus;
      BinaryTradeFixture fixture(0u, 0u, 0u, 1u, true, false, false, false, true);
      detail::SessionLoadPlan plan{}; plan.loadTrades = plan.loadBookTickers = false;
      plan.loadDepths = plan.requireDepths = true;
      detail::BinaryCorpusStream stream; std::string error;
      const std::array requests{fixture.request()};
      CXET_CHECK(stream.open(fixture.root, requests, plan, error) == hft_backtest::Status::Ok);
      std::vector<detail::TimelineEvent> events; bool available = false;
      CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Ok && available && events.size() == 2u);
      CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Ok && available && events.size() == 1u);
      CXET_CHECK(events[0].kind == detail::kTimelineKindHealth && events[0].applicationOnly && stream.sessions()[0].depths.empty());
      const auto& row = stream.sessions()[0].sourceHealth[0];
      const auto& payload = *corpus::binaryMarketPayload<corpus::BinaryMarketDepthChunkPayload>(&row.record);
      CXET_CHECK(row.unhealthy && payload.state == corpus::BinaryMarketDepthState::Gap && payload.failure == 7u);
      CXET_CHECK((row.record.header.flags & corpus::BinaryMarketRecordTraderReplayCompatible) == 0u);
      CXET_CHECK(row.record.header.receiveMonotonicNs == 250u && row.record.header.sourceGeneration == 11u);
      auto interval = fixture.request(); interval.beginReceiveNs = 225;
      const std::array intervalRequests{interval};
      CXET_CHECK(stream.open(fixture.root, intervalRequests, plan, error) == hft_backtest::Status::Ok);
      CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Ok && available && events.size() == 1u);
      CXET_CHECK(events[0].kind == detail::kTimelineKindHealth && stream.sessions()[0].depths.empty());
    }},
    cxet::testing::Case{"observed_health_pauses_required_lane_and_keeps_existing_execution_state", +[] {
      namespace corpus = hftrec::corpus;
      auto engine = std::make_unique<detail::EngineState>();
      auto unaffected = std::make_unique<detail::EngineState>();
      engine->capturedSourceManaged = true; engine->capturedSourceGeneration = 11u;
      const auto trade = corpus::binaryMarketChannelBit(corpus::BinaryMarketChannel::Trade);
      engine->capturedExecutionRequiredChannels = engine->capturedActiveChannels = trade;
      engine->snapshot.hasTrade = trading_core::kAlgoFlagTrue; engine->snapshot.lastTrade.ts.raw = 100u;
      CXET_CHECK(trading_core::configureBboReconstruction(engine->bboReconstruction,Price{1}));
      cxet::composite::BookTicker quote{};quote.ts.raw=90u;
      quote.bid.px.raw=100;quote.ask.px.raw=105;quote.bid.qty.raw=quote.ask.qty.raw=10;
      CXET_CHECK(trading_core::applyBboReconstructionBookTicker(engine->bboReconstruction,quote,
          trading_core::BboReconstructionTimestampOrigin::Exchange));
      cxet::composite::Trade seller{};seller.ts.raw=100u;
      CXET_CHECK(seller.setValues(Price{104},Amount{1},cxet::composite::TradeInitiatorSide::Seller));
      CXET_CHECK(trading_core::applyBboReconstructionKnownInitiatorTrade(engine->bboReconstruction,seller,
          trading_core::BboReconstructionTimestampOrigin::Exchange));
      engine->venueBboReconstruction=engine->bboReconstruction;
      for (auto* snapshot:{&engine->snapshot,&engine->venueSnapshot})
        trading_core::copyReconstructedBboSnapshot(*snapshot,trading_core::bboReconstructionView(engine->bboReconstruction),TimeNs{100});
      engine->pending.push_back({}); engine->pending[0].sourceGeneration = 11u;
      engine->open[0].state.valid = true; engine->open[0].sourceGeneration = 11u;
      engine->netQtyRaw = 7;
      unaffected->snapshot.hasTrade = trading_core::kAlgoFlagTrue;
      detail::SourceHealthRow row;
      row.record.header.channel = corpus::BinaryMarketChannel::Trade;
      row.record.header.sourceGeneration = row.arrival.sourceGeneration = 11u;
      row.record.header.sourceFlags = corpus::BinaryMarketSourceSequenceGap;
      row.unhealthy = true;
      std::string error;
      CXET_CHECK(hft_backtest::run_detail::applyCapturedSourceHealth(*engine, row, 200, error));
      CXET_CHECK(engine->capturedUnhealthyChannels == trade && engine->snapshot.hasTrade.raw == 0u);
      CXET_CHECK(engine->snapshot.reconstructedBookTicker.bid.px.raw==104 &&
          engine->snapshot.reconstructedBookTickerRecvMono.raw==100 && engine->reconstructionTradeRebasePending);
      CXET_CHECK(engine->capturedDirectoryRevision == 0u && engine->executionBoundaryEpoch == 1u);
      CXET_CHECK(hft_backtest::strategy_runtime_detail::nextPendingTs(*engine) == std::numeric_limits<std::int64_t>::max());
      detail::applyDueActions(*engine, 200);
      CXET_CHECK(engine->pending.size() == 1u && engine->open[0].state.valid && engine->netQtyRaw == 7);
      row.unhealthy = false; row.record.header.sourceFlags = 0u;
      CXET_CHECK(hft_backtest::run_detail::applyCapturedSourceHealth(*engine, row, 300, error));
      CXET_CHECK(engine->capturedUnhealthyChannels == 0u && engine->snapshot.hasTrade.raw == 0u);
      CXET_CHECK(engine->venueSnapshot.reconstructedBookTicker.bid.px.raw==104 &&
          engine->venueSnapshot.reconstructedBookTickerRecvMono.raw==100 && engine->venueReconstructionTradeRebasePending);
      CXET_CHECK(engine->executionBoundaryEpoch == 2u && engine->replayNotBeforeNs == 300);
      CXET_CHECK(engine->pending.size() == 1u && engine->open[0].state.valid && engine->netQtyRaw == 7);
      CXET_CHECK(unaffected->snapshot.hasTrade.raw == 1u && unaffected->executionBoundaryEpoch == 0u);
    }},
    cxet::testing::Case{"binary_observed_health_is_typed_pause_and_recovery_uses_real_payload", +[] {
      BinaryTradeFixture fixture(2u, 199u, 7u, 1u, false, false, true);
      detail::BinaryCorpusStream stream; std::string error;
      const std::array requests{fixture.request()};
      CXET_CHECK(stream.open(fixture.root, requests, fixture.plan(), error) == hft_backtest::Status::Ok);
      std::vector<detail::TimelineEvent> events; bool available = false;
      CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Ok && available && events.size() == 1u);
      CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Ok && available && events.size() == 1u);
      CXET_CHECK(events[0].kind == detail::kTimelineKindHealth && events[0].applicationOnly && stream.sessions()[0].trades.empty());
      CXET_CHECK(stream.sessions()[0].sourceHealth[0].record.header.sourceFlags == hftrec::corpus::BinaryMarketSourceStale);
      CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Ok && available && events.size() == 2u);
      CXET_CHECK(events[0].kind == detail::kTimelineKindHealth && !stream.sessions()[0].sourceHealth[0].unhealthy);
      CXET_CHECK(events[1].kind == detail::kTimelineKindTrade && stream.sessions()[0].trades[0].priceE8 == 1);
      auto interval = fixture.request(); interval.beginReceiveNs = 275;
      const std::array intervalRequests{interval};
      CXET_CHECK(stream.open(fixture.root, intervalRequests, fixture.plan(), error) == hft_backtest::Status::Ok);
      CXET_CHECK(stream.sessions()[0].capturedUnhealthyChannels == hftrec::corpus::binaryMarketChannelBit(hftrec::corpus::BinaryMarketChannel::Trade));
    }},
    cxet::testing::Case{"missing_capture_gap_boundary_still_refuses_complete_replay_interval", +[] {
      BinaryTradeFixture fixture(2u, 199u, 7u, 1u, false, false, false, true);
      detail::BinaryCorpusStream stream; std::string error;
      const std::array requests{fixture.request()};
      CXET_CHECK(stream.open(fixture.root, requests, fixture.plan(), error) == hft_backtest::Status::CorruptData);
    }},
