    cxet::testing::Case{"whole_market_corpus_loads_configured_and_added_symbols", +[] {
      QuantityFixtureConfig config; config.wholeUniverseSources = true;
      QuantFixture fixture(0, true, 1u, config);
      const auto first = fixture.load(), added = fixture.load("ETH_USDT");
      CXET_CHECK(first.status == hft_backtest::Status::Ok && added.status == hft_backtest::Status::Ok);
      CXET_CHECK(first.session.symbol == "BTC_USDT" && added.session.symbol == "ETH_USDT");
      CXET_CHECK(first.session.bookTickers.size() == 1u && added.session.bookTickers.size() == 1u);
    }},
    cxet::testing::Case{"legacy_single_session_cannot_infer_market_quantity_unit_from_order_grid", +[] {
      detail::InstrumentMetadata metadata{}; metadata.lotSizeE8 = 100; metadata.tickSizeE8 = 1;
      trading_core::StrategyInstrumentSpec instrument{}; std::string error;
      CXET_CHECK(hft_backtest::prepareBacktestStrategyInstrument(metadata, instrument, error) == hft_backtest::Status::Unsupported);
      CXET_CHECK(error.find("execution quantity") != std::string::npos);
    }},
    cxet::testing::Case{"linear_swap_accepts_exact_recorded_native_base_quantity_authority", +[] {
      for (const auto& exchange : canon::exchangeRegistryEntries())
        CXET_CHECK(detail::exchangeFromText(exchange.canonicalName).raw == exchange.id.raw);
      CXET_CHECK(detail::exchangeFromText("bingx").raw == canon::kExchangeIdBingx.raw);
      CXET_CHECK(detail::exchangeFromText("Bingx").raw == canon::kExchangeIdUnknown.raw);
      CXET_CHECK(detail::exchangeFromText(std::string{"bingx\0suffix", 12u}).raw == canon::kExchangeIdUnknown.raw);
      QuantityFixtureConfig config; config.marketKind = 2u;
      config.marketRaw = canon::kMarketTypeFutures.raw; config.venue = "bingx";
      config.conversion.kinds.fill(corpus::BinaryMarketQuantityKind::NativeBase);
      QuantFixture fixture(100'000'000, true, 1u, config); const auto result = fixture.load();
      CXET_CHECK(result.status == hft_backtest::Status::Ok);
      CXET_CHECK(result.session.bookTickers.size() == 1u && result.session.bookTickers[0].bidQtyE8 == config.qtyRaw);
    }},
    cxet::testing::Case{"quantity_authority_change_refuses_instead_of_reinterpreting_retained_state", +[] {
      QuantityFixtureConfig config; config.conversion.kinds.fill(corpus::BinaryMarketQuantityKind::NativeBase);
      config.changeConversion = true; QuantFixture fixture(0, true, 1u, config);
      detail::SessionLoadPlan plan; plan.loadTrades = false; plan.requireBookTickers = true;
      const std::array<hft_backtest::BacktestCorpusSourceRequest, 1> requests{
          hft_backtest::BacktestCorpusSourceRequest{"binance", "spot", "BTC_USDT", 100, 10'000}};
      detail::BinaryCorpusStream stream; std::string error; std::vector<detail::TimelineEvent> events; bool available = false;
      CXET_CHECK(stream.open(fixture.root, requests, plan, error) == hft_backtest::Status::Ok);
      CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Ok && available);
      CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Unsupported && !available);
      CXET_CHECK(error.find("quantity") != std::string::npos);
    }},
    cxet::testing::Case{"different_venue_base_and_contract_channels_project_equal_canonical_quantities", +[] {
      for (const bool contract : {false, true}) {
        QuantityFixtureConfig config; config.marketKind = 2u; config.allChannels = true;
        config.venue = contract ? "okx" : "binance";
        config.conversion.kinds.fill(contract ? corpus::BinaryMarketQuantityKind::NativeContract : corpus::BinaryMarketQuantityKind::NativeBase);
        config.conversion.canonicalBaseMultiplier = 2u; config.conversion.nativeBaseMultiplier = 1u;
        config.conversion.nativeContractBaseQtyRaw = contract ? 50'000'000 : 0;
        config.qtyRaw = contract ? 2'400'000'000 : 1'200'000'000;
        QuantFixture fixture(0, true, 1u, config);
        detail::SessionLoadPlan plan; plan.loadDepths = true;
        plan.requireBookTickers = plan.requireTrades = plan.requireDepths = true;
        const std::array<hft_backtest::BacktestCorpusSourceRequest, 1> requests{
            hft_backtest::BacktestCorpusSourceRequest{std::string(config.venue), "futures", "BTC_USDT", 100, 10'000}};
        detail::BinaryCorpusStream stream; std::string error; std::vector<detail::TimelineEvent> events; bool available = false;
        const auto openStatus = stream.open(fixture.root, requests, plan, error);
        if (openStatus != hft_backtest::Status::Ok)
          std::fprintf(stderr, "quantity replay open: contract=%d status=%u error=%s\n",
              static_cast<int>(contract), static_cast<unsigned>(openStatus), error.c_str());
        CXET_CHECK(openStatus == hft_backtest::Status::Ok);
        CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Ok && available);
        CXET_CHECK(stream.sessions()[0].bookTickers[0].bidQtyE8 == 600'000'000 && stream.sessions()[0].bookTickers[0].askQtyE8 == 600'000'000);
        CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Ok && available);
        CXET_CHECK(stream.sessions()[0].trades[0].qtyE8 == 600'000'000);
        CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Ok && available);
        CXET_CHECK(stream.sessions()[0].depths[0].levels[0].qtyE8 == 600'000'000 && stream.sessions()[0].depths[0].levels[1].qtyE8 == 600'000'000);
        corpus::BinaryMarketSelectionRequest native; native.root = fixture.root; native.exchange = std::string(config.venue);
        native.market = "futures"; native.canonicalSymbol = "BTC_USDT"; native.beginReceiveNs = 100; native.endReceiveNs = 10'000;
        native.channelMask = corpus::binaryMarketChannelBit(corpus::BinaryMarketChannel::BookTicker);
        corpus::BinaryMarketSelection selected;
        CXET_CHECK(corpus::BinaryMarketCorpusReader{}.select(native, selected, error) == hftrec::Status::Ok);
        CXET_CHECK(corpus::binaryMarketPayload<corpus::BinaryMarketBookTickerPayload>(&selected.records[0])->bidQtyRaw == config.qtyRaw);

      }
    }},
    cxet::testing::Case{"canonical_spot_quantities_are_not_packaged_twice", +[] {
      QuantityFixtureConfig config; config.conversion.canonicalBaseMultiplier = 1000u; config.conversion.nativeBaseMultiplier = 1u;
      QuantFixture fixture(0, true, 1u, config);
      detail::SessionLoadPlan plan; plan.loadTrades = false; plan.requireBookTickers = true;
      const std::array<hft_backtest::BacktestCorpusSourceRequest, 1> requests{
          hft_backtest::BacktestCorpusSourceRequest{"binance", "spot", "BTC_USDT", 100, 10'000}};
      detail::BinaryCorpusStream stream; std::string error; std::vector<detail::TimelineEvent> events; bool available = false;
      CXET_CHECK(stream.open(fixture.root, requests, plan, error) == hft_backtest::Status::Ok);
      CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Ok && available);
      CXET_CHECK(stream.sessions()[0].bookTickers[0].bidQtyE8 == config.qtyRaw);
    }},
    cxet::testing::Case{"native_lot_quantity_scales_then_uses_proven_native_lot_factor", +[] {
      corpus::BinaryMarketSource source{}; source.quantityScale = 6u;
      source.quantityConversion.kinds.fill(corpus::BinaryMarketQuantityKind::NativeLot);
      source.quantityConversion.canonicalBaseMultiplier = 2u; source.quantityConversion.nativeBaseMultiplier = 3u;
      source.quantityConversion.nativeLotBaseQtyRaw = 200'000'000;
      std::int64_t out = -1;
      CXET_CHECK(detail::binary_corpus::canonicalMarketQuantityToE8(source, corpus::BinaryMarketChannel::Trade, std::int64_t{1'000'000}, out));
      CXET_CHECK(out == 300'000'000);
    }},
    cxet::testing::Case{"unknown_selected_lane_quantity_authority_refuses_before_strategy_admission", +[] {
      QuantityFixtureConfig config; config.conversion = {};
      QuantFixture fixture(0, true, 1u, config); const auto result = fixture.load();
      CXET_CHECK(result.status == hft_backtest::Status::Unsupported && result.error.find("quantity") != std::string::npos);
    }},
    cxet::testing::Case{"quantity_projection_refuses_invalid_kind_missing_factors_and_contract_authority", +[] {
      corpus::BinaryMarketSource source{}; source.quantityScale = 8u;
      source.quantityConversion.kinds.fill(corpus::BinaryMarketQuantityKind::NativeContract);
      std::int64_t out = -7;
      CXET_CHECK(!detail::binary_corpus::canonicalMarketQuantityToE8(source, corpus::BinaryMarketChannel::BookTicker, std::int64_t{100'000'000}, out));
      source.quantityConversion.canonicalBaseMultiplier = source.quantityConversion.nativeBaseMultiplier = 1u;
      CXET_CHECK(!detail::binary_corpus::canonicalMarketQuantityToE8(source, corpus::BinaryMarketChannel::BookTicker, std::int64_t{100'000'000}, out));
      source.quantityConversion.nativeContractBaseQtyRaw = 100'000'000;
      source.quantityConversion.kinds[0] = static_cast<corpus::BinaryMarketQuantityKind>(255u);
      CXET_CHECK(!detail::binary_corpus::canonicalMarketQuantityToE8(source, corpus::BinaryMarketChannel::BookTicker, std::int64_t{100'000'000}, out));
      CXET_CHECK(out == -7);
    }},
    cxet::testing::Case{"nonexact_quantity_division_rejects_selected_frame_without_rounded_copy", +[] {
      QuantityFixtureConfig config; config.conversion.kinds.fill(corpus::BinaryMarketQuantityKind::NativeBase);
      config.conversion.canonicalBaseMultiplier = 3u; config.qtyRaw = 1;
      QuantFixture fixture(0, true, 1u, config);
      detail::SessionLoadPlan plan; plan.loadTrades = false; plan.requireBookTickers = true;
      const std::array<hft_backtest::BacktestCorpusSourceRequest, 1> requests{
          hft_backtest::BacktestCorpusSourceRequest{"binance", "spot", "BTC_USDT", 100, 10'000}};
      detail::BinaryCorpusStream stream; std::string error; std::vector<detail::TimelineEvent> events; bool available = true;
      CXET_CHECK(stream.open(fixture.root, requests, plan, error) == hft_backtest::Status::Ok);
      CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::CorruptData && !available && events.empty());
    }},
    cxet::testing::Case{"quantity_projection_overflow_refuses_without_output_mutation", +[] {
      corpus::BinaryMarketSource source{}; source.quantityScale = 8u;
      source.quantityConversion.kinds.fill(corpus::BinaryMarketQuantityKind::NativeBase);
      source.quantityConversion.canonicalBaseMultiplier = 1u; source.quantityConversion.nativeBaseMultiplier = 2u;
      std::int64_t out = -9;
      CXET_CHECK(!detail::binary_corpus::canonicalMarketQuantityToE8(source, corpus::BinaryMarketChannel::Depth,
          std::numeric_limits<std::int64_t>::max(), out));
      CXET_CHECK(out == -9);
    }},
