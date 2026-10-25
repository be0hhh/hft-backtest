    cxet::testing::Case{"binary_depth_preserves_native_evidence_and_complete_coverage_tail", +[] {
      auto selection = coverageDepthSelection();
      detail::binary_corpus::BinaryDecodeState decoder{};
      detail::SessionData session{}; std::string error;
      CXET_CHECK(detail::binary_corpus::decodeBinaryMarketRecords(selection, decoder, session, error) == hft_backtest::Status::Ok);
      CXET_CHECK(session.depths.size() == 1u);
      const auto& row = session.depths[0];
      const auto& source = *hftrec::corpus::binaryMarketPayload<hftrec::corpus::BinaryMarketDepthChunkPayload>(&selection.records[0]);
      CXET_CHECK(row.nativeEvidence == source.nativeEvidence && row.capturedSourcePosition);
      CXET_CHECK(row.tsNs == 100 && row.sourceReceiveRealtimeNs == 450u && row.sourceFreshnessMonotonicNs == 550u);
      CXET_CHECK(row.arrival.receiveRealtimeNs == 500 && row.arrival.receiveMonotonicNs == 600u);
      CXET_CHECK(row.capturedCoverage.size() == 6u && row.capturedCoverage.back().sourceId == 15u);
      CXET_CHECK(row.capturedCoverage.back().nativeEvidence == source.nativeEvidence);
      detail::ReplayDepthBook book;
      CXET_CHECK(book.prepare());
      CXET_CHECK(book.apply(row.levels, row.tsNs, row.arrival, row.resetBookBeforeApply,
          row.capturedSequence, row.capturedPublicationSequence, row.capturedSourcePosition, row.nativeEvidence));
      CXET_CHECK(book.sourcePosition() && book.book()->projection().nativeEvidence == source.nativeEvidence);
      auto engine = std::make_unique<detail::EngineState>();
      auto descriptor = singleSourceTopology(); descriptor.orderBookViewLevelsPerSide = 8u;
      detail::SessionLoadPlan loadPlan; loadPlan.loadDepths = true;
      CXET_CHECK(session.maximumCapturedDepthCoverageSources == 6u);
      CXET_CHECK(hft_backtest::run_detail::prepareEngineOrderBookStorage(*engine, &descriptor, loadPlan, &session));
      CXET_CHECK(engine->orderBook.coverageCapacity() == 6u && engine->venueOrderBook.coverageCapacity() == 6u);
      CXET_CHECK(detail::refreshReplayDepthProjection(engine->orderBook, book.view(row.capturedCoverage), Symbol{}));
      CXET_CHECK(engine->orderBook.feedCoverage().size() == 6u &&
          engine->orderBook.feedCoverage().back().nativeEvidence == source.nativeEvidence);
      CXET_CHECK(book.book()->projection().nativeEvidence.updateGuarantee == cxet::api::market::MarketNativeCounterGuarantee::OrderOnly);
      CXET_CHECK(book.book()->projection().nativeEvidence.crossSequenceGuarantee == cxet::api::market::MarketNativeCounterGuarantee::Unavailable);
      detail::TimelineEvent event{}; event.kind = detail::kTimelineKindDepth;
      event.deliveryTsNs = 600; event.sourceGeneration = 11u; event.frameSequence = 9u;
      hft_backtest::recordBacktestStrategySourceDelivery(*engine, event, row.nativeEvidence);
      CXET_CHECK(engine->strategySourceEvidence[cxet::api::market::publicMarketDataStreamIndex(
          cxet::api::market::PublicMarketDataStream::Orderbook)].nativeEvidence == row.nativeEvidence);
      detail::SessionData invalidPlan;
      invalidPlan.maximumCapturedDepthCoverageSources = cxet::market::kMaximumDepthSourceCapacity + 1u;
      CXET_CHECK(!hft_backtest::run_detail::prepareEngineOrderBookStorage(*engine, &descriptor, loadPlan, &invalidPlan));
      CXET_CHECK(engine->orderBook.coverageCapacity() == 6u && engine->venueOrderBook.coverageCapacity() == 6u);
    }},
    cxet::testing::Case{"binary_depth_refuses_partial_or_mismatched_tail_before_publishing", +[] {
      namespace corpus = hftrec::corpus;
      for (unsigned failure = 0u; failure < 5u; ++failure) {
        auto selection = coverageDepthSelection();
        if (failure == 0u) selection.records.pop_back();
        if (failure == 1u) selection.records.back().header.flags &= ~corpus::BinaryMarketRecordDepthTransactionEnd;
        if (failure == 2u) corpus::binaryMarketPayload<corpus::BinaryMarketDepthCoveragePayload>(&selection.records.back())->transactionPartCount = 2u;
        if (failure == 3u) selection.records[0].header.flags |= corpus::BinaryMarketRecordDepthTransactionEnd;
        if (failure == 4u) selection.records.back().header.sourceGeneration += 1u;
        detail::binary_corpus::BinaryDecodeState decoder{};
        detail::SessionData session{}; session.depths.emplace_back();
        session.depths[0].tsNs = 77;
        std::string error;
        CXET_CHECK(detail::binary_corpus::decodeBinaryMarketRecords(selection, decoder, session, error) == hft_backtest::Status::CorruptData);
        CXET_CHECK(session.depths.size() == 1u && session.depths[0].tsNs == 77);
      }
    }},
    cxet::testing::Case{"binary_empty_snapshot_clears_prior_book_after_complete_coverage_transaction", +[] {
      namespace corpus = hftrec::corpus;
      auto populated = coverageDepthSelection();
      detail::binary_corpus::BinaryDecodeState decoder;
      detail::SessionData prior; std::string error;
      CXET_CHECK(detail::binary_corpus::decodeBinaryMarketRecords(populated, decoder, prior, error) == hft_backtest::Status::Ok);
      detail::ReplayDepthBook book; CXET_CHECK(book.prepare());
      const auto apply = [&](const detail::DepthRow& row) {
        return book.apply(row.levels, row.tsNs, row.arrival, row.resetBookBeforeApply,
            row.capturedSequence, row.capturedPublicationSequence, row.capturedSourcePosition, row.nativeEvidence);
      };
      CXET_CHECK(apply(prior.depths[0]));
      trading_core::LocalOrderBookState projection;
      CXET_CHECK(projection.reserveLevelsPerSide(CountVal{8u}, 6u));
      CXET_CHECK(detail::refreshReplayDepthProjection(projection, book.view(prior.depths[0].capturedCoverage), Symbol{}));
      CXET_CHECK(book.view().bidCount == 1u && book.view().askCount == 1u &&
          projection.bids.storedCount.raw == 1u && projection.asks.storedCount.raw == 1u);

      auto empty = populated;
      for (std::size_t index = 0u; index < empty.records.size(); ++index) {
        auto& header = empty.records[index].header;
        header.eventSequence = 8u; header.frameSequence = 10u; header.shardSequence = index + 4u;
        header.exchangeTimestampNs = 110; header.receiveRealtimeNs = 700; header.receiveMonotonicNs = 800u;
      }
      auto& payload = *corpus::binaryMarketPayload<corpus::BinaryMarketDepthChunkPayload>(&empty.records[0]);
      payload.totalLevelCount = payload.levelOffset = payload.levelCount = 0u;
      payload.levels = {};
      payload.sequence.firstUpdateId = payload.sequence.lastUpdateId = 43u;
      detail::SessionData cleared;
      CXET_CHECK(detail::binary_corpus::decodeBinaryMarketRecords(empty, decoder, cleared, error) == hft_backtest::Status::Ok);
      CXET_CHECK(cleared.depths.size() == 1u && cleared.depths[0].levels.empty() && cleared.depths[0].resetBookBeforeApply);
      CXET_CHECK(apply(cleared.depths[0]));
      CXET_CHECK(detail::refreshReplayDepthProjection(projection, book.view(cleared.depths[0].capturedCoverage), Symbol{}));
      CXET_CHECK(book.view().bidCount == 0u && book.view().askCount == 0u &&
          projection.bids.storedCount.raw == 0u && projection.asks.storedCount.raw == 0u);
      CXET_CHECK(projection.feedCoverage().size() == 6u && book.book()->projection().publicationSequence == 8u);
      for (unsigned failure = 0u; failure < 2u; ++failure) {
        auto invalid = empty;
        if (failure == 0u) invalid.records.back().header.flags &= ~corpus::BinaryMarketRecordDepthTransactionEnd;
        else {
          invalid.records[0].header.flags &= ~(corpus::BinaryMarketRecordDepthSnapshot | corpus::BinaryMarketRecordDepthRebase);
          invalid.records[0].header.flags |= corpus::BinaryMarketRecordDepthDelta;
          auto& delta = *corpus::binaryMarketPayload<corpus::BinaryMarketDepthChunkPayload>(&invalid.records[0]);
          delta.frameKind = corpus::BinaryMarketDepthFrameKind::Delta; delta.state = corpus::BinaryMarketDepthState::Healthy;
        }
        detail::binary_corpus::BinaryDecodeState invalidDecoder;
        detail::SessionData refused;
        CXET_CHECK(detail::binary_corpus::decodeBinaryMarketRecords(invalid, invalidDecoder, refused, error) == hft_backtest::Status::CorruptData);
        CXET_CHECK(refused.depths.empty());
      }
    }},
    cxet::testing::Case{"core_composed_cuts_preserve_state_and_recover_only_with_new_complete_publication", +[] {
      namespace corpus = hftrec::corpus;
      using State = cxet::market::DepthSourceState;
      CoreComposedReplayFixture fixture;
      fixture.publish(1u, false);
      CXET_CHECK(fixture.cuts.size() == 1u);
      detail::SimulationInput input;
      input.sourceLegs = {0u}; input.sources.resize(1u); input.sessions.resize(1u);
      auto& source = input.sources[0];
      source.selection = fixture.cuts[0]; source.selection.records.clear();
      source.requestedMask = source.requiredMask = source.selectedMask = corpus::binaryMarketChannelBit(corpus::BinaryMarketChannel::Depth);
      input.sessions[0].binaryMarketCorpus = true;
      input.sessions[0].replayAnchorMonotonicNs = 2'001u; input.sessions[0].replayAnchorReceiveNs = 1'001;
      input.sessions[0].maximumCapturedDepthCoverageSources = 1u;
      auto engine = std::make_unique<detail::EngineState>();
      auto descriptor = singleSourceTopology(); descriptor.orderBookViewLevelsPerSide = 8u;
      detail::SessionLoadPlan loadPlan; loadPlan.loadDepths = true;
      CXET_CHECK(hft_backtest::run_detail::prepareEngineOrderBookStorage(*engine, &descriptor, loadPlan, &input.sessions[0]));
      engine->capturedSourceGeneration = 1u;
      const auto depthBit = corpus::binaryMarketChannelBit(corpus::BinaryMarketChannel::Depth);
      engine->capturedSourceManaged = true;
      engine->capturedActiveChannels = engine->capturedExecutionRequiredChannels = depthBit;
      hft_backtest::StrategyInstrumentSpec instrument; instrument.tickSizeRaw = 1;
      std::string error;
      const auto deliver = [&](const corpus::BinaryMarketSelection& selection) {
        std::vector<detail::TimelineEvent> events;
        for (const auto& record : selection.records) {
          CXET_CHECK(input.append(record, events, error) == hft_backtest::Status::Ok);
          for (const auto& event : events) {
            if (event.kind == detail::kTimelineKindHealth)
              CXET_CHECK(hft_backtest::run_detail::applyCapturedSourceHealth(*engine, input.sessions[0].sourceHealth[event.index], event.deliveryTsNs, error));
            else hft_backtest::applyMarketEventToSnapshots(*engine, input.sessions[0], event, Symbol{}, instrument, true);
          }
        }
        CXET_CHECK(!engine->depthReplayFailed);
      };
      const auto replay = [&](const detail::DepthRow& row) {
        return engine->strategyDepthBook.apply(row.levels, row.tsNs, row.arrival, row.resetBookBeforeApply,
            row.capturedSequence, row.capturedPublicationSequence, row.capturedSourcePosition, row.nativeEvidence,
            row.capturedComposedCut, row.capturedState, row.capturedFailure, row.capturedCoverage);
      };
      deliver(fixture.cuts[0]);
      CXET_CHECK(input.sessions[0].depths[0].capturedComposedCut && engine->strategyDepthBook.book()->state() == State::Partial);
      CXET_CHECK(engine->strategyDepthBook.view().coverage == cxet::api::market::PublicMarketOrderBookCoverage::DeltaOnly);
      const auto retained = input.sessions[0].depths[0];
      CXET_CHECK(engine->venueDepthBook.apply(retained.levels, retained.tsNs, retained.arrival,
          retained.resetBookBeforeApply, retained.capturedSequence, retained.capturedPublicationSequence,
          retained.capturedSourcePosition, retained.nativeEvidence, retained.capturedComposedCut,
          retained.capturedState, retained.capturedFailure, retained.capturedCoverage));
      detail::SourceLifecycleRow marker;
      marker.arrival.sourceId = marker.payload.source.sourceId = 1u;
      marker.arrival.sourceGeneration = marker.payload.source.initialSourceGeneration = marker.payload.previousSourceGeneration = 1u;
      marker.payload.kind = corpus::BinaryMarketSourceLifecycleKind::DirectoryChanged;
      marker.payload.affectedChannel = static_cast<std::uint8_t>(corpus::BinaryMarketChannel::Depth);
      marker.payload.channelMaskBefore = marker.payload.channelMaskAfter = depthBit;
      marker.payload.cause = corpus::BinaryMarketSourceTransitionCause::NativeDegraded;
      CXET_CHECK(hft_backtest::run_detail::applyCapturedSourceLifecycle(*engine, marker, 1'001, error));
      source.unhealthyMask |= depthBit; // The same marker's reader bookkeeping.
      CXET_CHECK(engine->capturedUnhealthyChannels == depthBit && !engine->capturedDepthRecoveryPending &&
          engine->capturedDepthAdministrativeUnhealthy);
      for (const auto* book : {engine->strategyDepthBook.book(), engine->venueDepthBook.book()}) {
        CXET_CHECK(book->state() == State::Partial && book->lastUpdateId() == 1u &&
            book->projection().publicationSequence == 1u && book->bids()[0].qty.raw == 2 &&
            book->projection().sourceGeneration == 1u && book->projection().sessionEpoch == 1u &&
            book->projection().nativeEvidence == retained.nativeEvidence);
      }
      fixture.publish(2u, true); CXET_CHECK(fixture.cuts.size() == 2u);
      deliver(fixture.cuts[1]);
      CXET_CHECK(engine->strategyDepthBook.book()->state() == State::Healthy &&
          engine->capturedUnhealthyChannels == depthBit && engine->capturedDepthAdministrativeUnhealthy &&
          !engine->capturedDepthRecoveryPending);
      marker.payload.cause = corpus::BinaryMarketSourceTransitionCause::NativeRecovered;
      marker.arrival.sourceGeneration = marker.payload.source.initialSourceGeneration = 2u;
      CXET_CHECK(!hft_backtest::run_detail::applyCapturedSourceLifecycle(*engine, marker, 1'003, error));
      marker.arrival.sourceGeneration = marker.payload.source.initialSourceGeneration = 1u;
      CXET_CHECK(hft_backtest::run_detail::applyCapturedSourceLifecycle(*engine, marker, 1'003, error));
      CXET_CHECK(engine->capturedSourceGeneration == 1u && engine->capturedUnhealthyChannels == depthBit &&
          engine->capturedDepthRecoveryPending && engine->strategyDepthBook.book()->projection().publicationSequence == 2u &&
          engine->venueDepthBook.book()->projection().publicationSequence == 1u);
      const std::array<detail::PriceLevelRow, 1u> badDelta{{{99, 9, 0}}};
      CXET_CHECK(!engine->strategyDepthBook.apply(badDelta, 120, input.sessions[0].depths[0].arrival,
          false, {100u, 100u, 99u}, 100u));
      CXET_CHECK(engine->strategyDepthBook.book()->state() == State::Gap);
      fixture.publish(3u, false); CXET_CHECK(fixture.cuts.size() == 3u);
      deliver(fixture.cuts[2]);
      CXET_CHECK(engine->strategyDepthBook.book()->state() == State::Partial && engine->strategyDepthBook.view().ready);
      CXET_CHECK(engine->capturedUnhealthyChannels == 0u && !engine->capturedDepthRecoveryPending &&
          !engine->capturedDepthAdministrativeUnhealthy);
      const auto priorPartial = input.sessions[0].depths[0];
      fixture.gap(4u); CXET_CHECK(fixture.cuts.size() == 4u);
      deliver(fixture.cuts[3]);
      const auto gap = input.sessions[0].depths[0];
      CXET_CHECK(gap.capturedState == State::Gap && gap.capturedFailure == cxet::market::DepthFailure::None);
      CXET_CHECK(gap.capturedCoverage[0].failure == cxet::market::DepthFailure::ExchangeSequenceGap);
      CXET_CHECK(engine->strategyDepthBook.book()->state() == State::Gap && !engine->strategyDepthBook.view().ready &&
          engine->snapshot.hasDepth.raw == trading_core::kAlgoFlagFalse.raw && engine->orderBook.feedCoverage().size() == 1u);
      CXET_CHECK(!replay(priorPartial) && engine->strategyDepthBook.book()->state() == State::Gap);
      detail::SessionData stale;
      CXET_CHECK(detail::binary_corpus::decodeBinaryMarketRecords(fixture.cuts[2], source.decoder, stale, error) == hft_backtest::Status::CorruptData && stale.depths.empty());
      fixture.publish(5u, true); CXET_CHECK(fixture.cuts.size() == 5u);
      deliver(fixture.cuts[4]); // Native Gap recovery requires no administrative marker.
      CXET_CHECK(engine->strategyDepthBook.book()->state() == State::Healthy && engine->strategyDepthBook.view().ready);
      CXET_CHECK(engine->capturedUnhealthyChannels == 0u && !engine->capturedDepthRecoveryPending &&
          !engine->capturedDepthAdministrativeUnhealthy);
      CXET_CHECK(!replay(gap) && engine->strategyDepthBook.book()->state() == State::Healthy);

      auto empty = fixture.cuts[4];
      for (std::size_t index=0u; index<empty.records.size(); ++index) {
        auto& header = empty.records[index].header;
        header.eventSequence = header.frameSequence = 6u; header.shardSequence = index + 11u;
        header.receiveRealtimeNs = 1'006; header.receiveMonotonicNs = 2'006u;
        header.exchangeTimestampNs = 0; header.flags |= corpus::BinaryMarketRecordExchangeTimestampMissing;
        header.sourceFlags = corpus::BinaryMarketSourceExchangeTimestampMissing;
      }
      auto& emptyPayload = *corpus::binaryMarketPayload<corpus::BinaryMarketDepthChunkPayload>(&empty.records[0]);
      emptyPayload.state = corpus::BinaryMarketDepthState::Empty;
      emptyPayload.totalLevelCount = emptyPayload.levelCount = 0u; emptyPayload.levels = {};
      emptyPayload.sequence = {6u, 6u, 0u}; emptyPayload.sourceReceiveRealtimeNs = emptyPayload.sourceFreshnessMonotonicNs = 0u;
      emptyPayload.nativeEvidence = {};
      auto& emptyCoverage = *corpus::binaryMarketPayload<corpus::BinaryMarketDepthCoveragePayload>(&empty.records[1]);
      emptyCoverage.coverage[0] = {}; emptyCoverage.coverage[0].sourceId = 1u; emptyCoverage.coverage[0].maximumLevelsPerSide = 8u;
      deliver(empty);
      CXET_CHECK(engine->strategyDepthBook.book()->state() == State::Empty && !engine->strategyDepthBook.view().ready &&
          engine->strategyDepthBook.view().bidCount == 0u && engine->orderBook.feedCoverage().size() == 1u);
      for (unsigned failure=0u; failure<2u; ++failure) {
        auto invalid = empty;
        if (failure == 0u) {
          auto& p = *corpus::binaryMarketPayload<corpus::BinaryMarketDepthChunkPayload>(&invalid.records[0]);
          p.totalLevelCount = p.levelCount = 1u; p.levels[0] = {99u, 1u, 1u, 1u, 0u, 0u};
        } else {
          invalid.records.resize(1u);
          invalid.records[0].header.flags &= ~(corpus::BinaryMarketRecordDepthHasCoverage | corpus::BinaryMarketRecordDepthSourcePosition);
          invalid.records[0].header.flags |= corpus::BinaryMarketRecordDepthTransactionEnd;
        }
        detail::binary_corpus::BinaryDecodeState invalidDecoder;
        detail::SessionData refused;
        CXET_CHECK(detail::binary_corpus::decodeBinaryMarketRecords(invalid, invalidDecoder, refused, error) == hft_backtest::Status::CorruptData);
        CXET_CHECK(refused.depths.empty());
      }
      BinaryTradeFixture directory(0u, 0u, 0u, 1u, true);
      auto catalog = directory.sources; catalog[0].initialSourceGeneration = 1u;
      corpus::BinaryMarketWriterConfig config;
      config.root = directory.root / "overlay-intervals"; config.sources = catalog; config.producerEpoch = 13u;
      config.maximumBytes = 128u * 1024u; config.segmentTargetBytes = 4096u;
      config.targetDurationNs = 10'000u; config.startedReceiveNs = 1'000;
      config.startedMonotonicNs = 2'000u; config.ringCapacity = 2u; config.shardCount = 1u;
      corpus::BinaryMarketCorpusWriter writer;
      CXET_CHECK(writer.start(config) == hftrec::Status::Ok);
      std::uint64_t shard = 1u;
      const auto writeCut = [&](std::size_t index, std::int64_t receive) {
        for (auto record : fixture.cuts[index].records) {
          record.header.shardSequence = shard++;
          record.header.receiveRealtimeNs = receive;
          record.header.receiveMonotonicNs = static_cast<std::uint64_t>(receive + 1'000);
          CXET_CHECK(writer.append(record) == hftrec::Status::Ok);
        }
      };
      const auto writeMarker = [&](corpus::BinaryMarketSourceTransitionCause cause,
          std::uint64_t revision, std::int64_t receive) {
        corpus::BinaryMarketRecord record; auto& header = record.header;
        header.sourceId = 1u; header.sourceGeneration = header.sessionEpoch = 1u;
        header.eventSequence = revision; header.shardSequence = shard++;
        header.receiveRealtimeNs = receive; header.receiveMonotonicNs = static_cast<std::uint64_t>(receive + 1'000);
        header.channel = corpus::BinaryMarketChannel::SourceLifecycle;
        header.flags = corpus::BinaryMarketRecordSourceLifecycle | corpus::BinaryMarketRecordTraderReplayCompatible |
            corpus::BinaryMarketRecordExchangeTimestampMissing;
        header.payloadBytes = sizeof(corpus::BinaryMarketSourceLifecyclePayload);
        auto& payload = *corpus::binaryMarketPayload<corpus::BinaryMarketSourceLifecyclePayload>(&record);
        payload.source = catalog[0]; payload.directoryRevision = revision; payload.previousSourceGeneration = 1u;
        payload.channelMaskBefore = payload.channelMaskAfter = depthBit;
        payload.kind = corpus::BinaryMarketSourceLifecycleKind::DirectoryChanged;
        payload.affectedChannel = static_cast<std::uint8_t>(corpus::BinaryMarketChannel::Depth); payload.cause = cause;
        CXET_CHECK(writer.append(record) == hftrec::Status::Ok);
      };
      writeCut(0u, 1'001); writeMarker(corpus::BinaryMarketSourceTransitionCause::NativeDegraded, 1u, 1'002);
      writeCut(1u, 1'003); writeMarker(corpus::BinaryMarketSourceTransitionCause::NativeRecovered, 2u, 1'004);
      writeCut(2u, 1'005); writeCut(3u, 1'006); writeCut(4u, 1'007);
      CXET_CHECK(writer.finalize(corpus::BinaryMarketStopReason::Requested, 10'000) == hftrec::Status::Ok);
      detail::SessionLoadPlan intervalPlan; intervalPlan.loadTrades = intervalPlan.loadBookTickers = false;
      intervalPlan.loadDepths = intervalPlan.requireDepths = true;
      for (unsigned phase=0u; phase<3u; ++phase) {
        auto request = directory.request(); request.beginReceiveNs = phase == 0u ? 1'003 : phase == 1u ? 1'005 : 1'007;
        request.endReceiveNs = 1'008;
        const std::array requests{request}; detail::BinaryCorpusStream stream;
        CXET_CHECK(stream.open(config.root, requests, intervalPlan, error) == hft_backtest::Status::Ok);
        const auto& initial = stream.sessions()[0];
        CXET_CHECK(initial.capturedUnhealthyChannels == depthBit);
        CXET_CHECK(initial.capturedDepthAdministrativeUnhealthy == (phase != 2u));
        CXET_CHECK(initial.capturedDepthRecoveryPending == (phase == 1u));
        std::vector<detail::TimelineEvent> events; bool available = false;
        CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Ok && available);
        CXET_CHECK(stream.sessions()[0].depths.size() == 1u &&
            stream.sessions()[0].depths[0].capturedState == (phase == 1u ? State::Partial : State::Healthy));
      }
    }},
