    cxet::testing::Case{"universe_single_source_context_bound_does_not_cap_catalog_at_fifty", +[] {
      std::vector<detail::SessionData> sources(60u);
      for (std::size_t i = 0; i < sources.size(); ++i) {
        sources[i].exchange = "Binance"; sources[i].market = "Futures";
        sources[i].symbol = "S" + std::to_string(i);
        sources[i].quoteAssetId=1u;
      }
      hft_backtest::BacktestStrategyConfig config;
      std::vector<hft_backtest::run_detail::UniverseContextSelection> contexts;
      std::string error;
      CXET_CHECK(hft_backtest::run_detail::discoverUniverseContexts(
          singleSourceTopology(), config, sources, contexts, error));
      CXET_CHECK(contexts.size() == 60u);
      for (std::size_t i = 0; i < contexts.size(); ++i)
        CXET_CHECK(contexts[i].sourceIndices.size() == 1u && contexts[i].sourceIndices[0] == i);
    }},
    cxet::testing::Case{"whole_market_materializes_new_symbols_from_proven_seed_quote", +[] {
      using Source=hftrec::corpus::BinaryMarketSource;
      const auto row=[](const char* symbol,std::uint32_t quote) {
        Source source{};source.venueId=1u;source.marketRaw=1u;source.quoteAssetId=quote;
        std::string text=symbol;source.canonicalSymbolBytes=text.size();
        std::copy(text.begin(),text.end(),source.canonicalSymbol.begin());return source;
      };
      const std::array catalog{row("AAA_USDT",2u),row("NEW_USDT",2u),row("OTHER_USDC",3u)};
      hft_backtest::BacktestStrategyConfig config;
      config.strategyType="topology_fixture";
      hft_backtest::BacktestStrategyVenueConfig venue;
      venue.name="Binance_Futures";venue.exchange.raw=1u;venue.market.raw=1u;
      venue.initialBalanceE8=123;venue.hasInitialBalance=true;
      Symbol seed{};CXET_CHECK(seed.copyFrom("AAA_USDT"));venue.symbols.push_back(seed);
      config.venues.push_back(venue);std::string error;
      CXET_CHECK(hft_backtest::run_detail::materializeUniverseSymbolsFromCorpus(config,catalog,error));
      CXET_CHECK(config.venues[0].symbols.size()==2u);
      CXET_CHECK(std::string_view(config.venues[0].symbols[1].data)=="NEW_USDT");
      CXET_CHECK(config.venues[0].initialBalanceE8==123 && config.strategyType=="topology_fixture");
    }},
    cxet::testing::Case{"whole_market_ambiguous_quote_refuses_without_partial_config_change", +[] {
      std::array<hftrec::corpus::BinaryMarketSource,2u> catalog{};
      for (std::size_t i=0;i<catalog.size();++i) {
        catalog[i].venueId=1u;catalog[i].marketRaw=1u;catalog[i].quoteAssetId=i+2u;
        const std::string symbol=i==0u?"AAA_USDT":"BBB_USDC";
        catalog[i].canonicalSymbolBytes=symbol.size();
        std::copy(symbol.begin(),symbol.end(),catalog[i].canonicalSymbol.begin());
      }
      hft_backtest::BacktestStrategyConfig config;hft_backtest::BacktestStrategyVenueConfig venue;
      venue.name="Binance_Futures";venue.exchange.raw=1u;venue.market.raw=1u;
      Symbol seed{};CXET_CHECK(seed.copyFrom("MISSING_USDT"));venue.symbols.push_back(seed);config.venues.push_back(venue);
      std::string error;
      CXET_CHECK(!hft_backtest::run_detail::materializeUniverseSymbolsFromCorpus(config,catalog,error));
      CXET_CHECK(config.venues[0].symbols.size()==1u && std::string_view(config.venues[0].symbols[0].data)=="MISSING_USDT");
      config.venues[0].symbols.clear();
      for (const auto& row:catalog) {Symbol symbol{};CXET_CHECK(symbol.copyFrom(row.canonicalSymbol.data()));config.venues[0].symbols.push_back(symbol);}
      CXET_CHECK(!hft_backtest::run_detail::materializeUniverseSymbolsFromCorpus(config,catalog,error));
      config.venues[0].symbols.resize(1u);
      catalog[1].quoteAssetId=catalog[0].quoteAssetId;
      catalog[1].canonicalSymbol[3]='\0';
      CXET_CHECK(!hft_backtest::run_detail::materializeUniverseSymbolsFromCorpus(config,catalog,error));
      CXET_CHECK(config.venues[0].symbols.size()==1u && std::string_view(config.venues[0].symbols[0].data)=="AAA_USDT");
      catalog[1].canonicalSymbol.fill('\0');
      const std::string malformed="BROKEN";
      catalog[1].canonicalSymbolBytes=malformed.size();
      std::copy(malformed.begin(),malformed.end(),catalog[1].canonicalSymbol.begin());
      CXET_CHECK(!hft_backtest::run_detail::materializeUniverseSymbolsFromCorpus(config,catalog,error));
      CXET_CHECK(config.venues[0].symbols.size()==1u && std::string_view(config.venues[0].symbols[0].data)=="AAA_USDT");
    }},
    cxet::testing::Case{"universe_contexts_borrow_same_source_and_share_wallet", +[] {
      auto first = std::make_unique<detail::EngineState>();
      auto second = std::make_unique<detail::EngineState>();
      first->accountingModel = second->accountingModel = detail::SimulationAccountingModel::Cash;
      detail::SharedAccountState account{};
      account.accountingModel = detail::SimulationAccountingModel::Cash;
      account.initialBalanceE8 = account.walletBalanceE8 = 1000;
      account.observedWalletBalanceE8=1000;account.observedInitialized=true;
      first->sharedAccount = second->sharedAccount = &account;
      first->initialBalanceE8 = first->walletBalanceE8 = 1000;
      std::array<detail::EngineState*, 2> routes{second.get(), first.get()};
      detail::EngineView view(routes.data(), routes.size());
      view[0].netQtyRaw = 7;
      CXET_CHECK(second->netQtyRaw == 7 && &view[1] == first.get());
      const auto filled = detail::updatePosition(*first, Side::Buy(), 100, 100'000'000, 1, 100'000'000);
      CXET_CHECK(filled.applied && account.walletBalanceE8 == first->walletBalanceE8);
      const auto sameAccount = hft_backtest::accountStateFromEngine(*second);
      CXET_CHECK(sameAccount.initialBalanceE8 == 1000 && sameAccount.walletBalanceE8 == 1000);
    }},
