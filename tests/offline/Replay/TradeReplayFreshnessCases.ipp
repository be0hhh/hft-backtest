    cxet::testing::Case{"reconstruction_freshness_uses_accepted_trade_and_refuses_old_quote", +[] {
      auto engine = std::make_unique<detail::EngineState>();
      CXET_CHECK(trading_core::configureBboReconstruction(engine->bboReconstruction, Price{1}));
      detail::SessionData session{};
      detail::BookTickerRow quote{};
      quote.bidPriceE8=100; quote.askPriceE8=105;
      quote.bidQtyE8=quote.askQtyE8=10; quote.tsNs=100;
      session.bookTickers.push_back(quote);
      detail::TradeRow trade{};
      trade.priceE8=104; trade.qtyE8=1; trade.side=0; trade.tsNs=200;
      session.trades.push_back(trade);
      detail::TimelineEvent event{};
      event.kind=detail::kTimelineKindBookTicker;
      event.tsNs=event.deliveryTsNs=100;
      hft_backtest::StrategyInstrumentSpec instrument{};
      hft_backtest::applyMarketEventToSnapshots(*engine,session,event,Symbol{},instrument,true);
      event.kind=detail::kTimelineKindTrade;
      event.tsNs=event.deliveryTsNs=200;
      hft_backtest::applyMarketEventToSnapshots(*engine,session,event,Symbol{},instrument,true);
      CXET_CHECK(engine->snapshot.reconstructedBookTickerRecvMono.raw==200u);
      hft_backtest::BacktestStrategyInputPolicy policy{};
      policy.requiredStreamReadyMask=cxet::api::market::publicMarketDataStreamReadyBit(
          cxet::api::market::PublicMarketDataStream::BookTicker);
      policy.maxAgeNs=50u;
      const detail::ConstEngineView engines{engine.get(),1u};
      CXET_CHECK(hft_backtest::backtestSourceFreshLegMask(policy,engines,1u,225)==1u);
      const auto revision=engine->contextRevision;
      session.bookTickers[0].tsNs=150;
      event.kind=detail::kTimelineKindBookTicker;
      event.tsNs=150; event.deliveryTsNs=240;
      for (const bool freshnessOnly : {false,true}) {
        session.bookTickers[0].freshnessOnly=freshnessOnly;
        hft_backtest::applyMarketEventToSnapshots(*engine,session,event,Symbol{},instrument,true);
        CXET_CHECK(engine->snapshot.lastBookTickerRecvMono.raw==100u &&
            engine->snapshot.reconstructedBookTickerRecvMono.raw==200u &&
            engine->contextRevision==revision);
        CXET_CHECK(detail::strategySourceDeliveryTimestamp(*engine,
            cxet::api::market::PublicMarketDataStream::BookTicker)==100);
      }
      CXET_CHECK(hft_backtest::backtestSourceFreshLegMask(policy,engines,1u,251)==0u);
    }},
    cxet::testing::Case{"replay_depth_refresh_does_not_reapply_older_reconstructed_bbo", +[] {
      for (const bool strategyVisible : {false, true}) {
        for (const std::int64_t tick : {0ll, 1ll}) {
          auto engine = std::make_unique<detail::EngineState>();
          auto& book = strategyVisible ? engine->orderBook : engine->venueOrderBook;
          auto& depthBook = strategyVisible ? engine->strategyDepthBook : engine->venueDepthBook;
          auto& snapshot = strategyVisible ? engine->snapshot : engine->venueSnapshot;
          auto& bbo = strategyVisible ? engine->bboReconstruction : engine->venueBboReconstruction;
          CXET_CHECK(trading_core::configureBboReconstruction(bbo, Price{1}));
          CXET_CHECK(book.reserveLevelsPerSide(CountVal{8u}));
          CXET_CHECK(depthBook.prepare());
          detail::SessionData session{};
          detail::BookTickerRow ticker{};
          ticker.bidPriceE8 = 100; ticker.bidQtyE8 = 2;
          ticker.askPriceE8 = 101; ticker.askQtyE8 = 3; ticker.tsNs = 100;
          session.bookTickers.push_back(ticker);
          detail::DepthRow depth{};
          depth.tsNs = 200; depth.resetBookBeforeApply = true;
          depth.levels = {{99, 4, 0}, {102, 5, 1}};
          depth.arrival.receiveMonotonicNs = 200u;
          session.depths.push_back(depth);
          hft_backtest::StrategyInstrumentSpec instrument{};
          instrument.tickSizeRaw = tick;
          detail::TimelineEvent event{};
          event.kind = detail::kTimelineKindBookTicker;
          event.tsNs = event.deliveryTsNs = 100;
          hft_backtest::applyMarketEventToSnapshots(
              *engine, session, event, Symbol{}, instrument, strategyVisible);
          CXET_CHECK(snapshot.hasReconstructedBbo.raw == trading_core::kAlgoFlagTrue.raw);
          CXET_CHECK(snapshot.reconstructedBookTicker.bid.px.raw == 100 &&
              snapshot.reconstructedBookTicker.ask.px.raw == 101);
          event.kind = detail::kTimelineKindDepth;
          event.tsNs = event.deliveryTsNs = 200;
          hft_backtest::applyMarketEventToSnapshots(
              *engine, session, event, Symbol{}, instrument, strategyVisible);
          CXET_CHECK(!engine->depthReplayFailed && book.ts.raw == 200u);
          CXET_CHECK(book.bids.storedCount.raw == 1u && book.asks.storedCount.raw == 1u);
          trading_core::LocalOrderBookLevel bid{}, ask{};
          const auto view = trading_core::orderBook(book);
          CXET_CHECK(view.readStoredLevel(trading_core::BookSide::Bid, Index{0u}, bid));
          CXET_CHECK(view.readStoredLevel(trading_core::BookSide::Ask, Index{0u}, ask));
          CXET_CHECK(bid.px.raw == 99 && bid.qty.raw == 4 && ask.px.raw == 102 && ask.qty.raw == 5);
          for (const auto* side : {&book.bids, &book.asks}) {
            CXET_CHECK(side->origins[0].sourceId == 0u &&
                side->origins[0].exchangeTimestampNs == 0u &&
                side->origins[0].freshnessMonotonicNs == 0u);
          }
          if (tick > 0) {
            CXET_CHECK(book.config.tickSizeRaw == tick &&
                book.config.tickGrid.step.divisor == static_cast<std::uint64_t>(tick));
          } else {
            CXET_CHECK(book.config.tickSizeRaw == 0 && book.config.tickGrid.step.divisor == 0u);
            CXET_CHECK(book.bids.format == trading_core::LocalOrderBookStorageFormat::Sparse &&
                book.asks.format == trading_core::LocalOrderBookStorageFormat::Sparse);
          }
        }
      }
    }},
