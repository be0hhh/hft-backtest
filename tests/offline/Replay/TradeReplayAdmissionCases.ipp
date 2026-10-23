    cxet::testing::Case{"discoverable_profiles_do_not_certify_missing_capture_authority", +[] {
      CXET_CHECK(hft_backtest::strategyMetadataCount() > 0u);
      CXET_CHECK(hft_backtest::strategyMetadataAt(0u) != nullptr);
      BinaryTradeFixture fixture(2u, 199u, 7u);
      for (const auto name : {"delivery_sum_signal", "pair_api_probe", "unknown_strategy"}) {
        CXET_CHECK((hft_backtest::findStrategyMetadata(name) == nullptr) == (std::string{name}=="unknown_strategy"));
        const auto path = fixture.root / "unsupported.ini";
        {
          std::ofstream config(path);
          config << "[strategy]\ntype=" << name
                 << "\n[venue.binance_spot]\nsymbols=BTC_USDT\nrate_limits_enabled=false\n";
        }
        hft_backtest::BacktestRunRequest request{};
        request.configPath = path; request.corpusPath = fixture.root;
        request.corpusBeginReceiveNs = 100; request.corpusEndReceiveNs = 10'000;
        request.writeArtifacts = false;
        const auto result = hft_backtest::runBacktest(request);
        CXET_CHECK(result.status == hft_backtest::Status::Unsupported);
        CXET_CHECK(!result.error.empty());
        CXET_CHECK(result.orders == 0u && result.fills == 0u && result.inputCoverage.strategyCycles == 0u);
      }
    }},
    cxet::testing::Case{"backtest_api_guard_rejects_old_result_layout",+[] {
      CXET_CHECK(hft_backtest::kBacktestApiVersion==8u);
      CXET_CHECK(hft_backtest::hft_backtest_api_version()==hft_backtest::kBacktestApiVersion);
      CXET_CHECK(hft_backtest::hft_backtest_api_version()!=7u);
      hft_backtest::BacktestRunRequest legacy;legacy.sessionPath="/explicit/legacy-session";
      legacy.configPath="/explicit/legacy-config";legacy.writeArtifacts=false;
      const auto refused=hft_backtest::runBacktest(legacy);
      CXET_CHECK(refused.status==hft_backtest::Status::Unsupported);
      CXET_CHECK(refused.error.find("typed financial accounting model")!=std::string::npos);
      CXET_CHECK(refused.orders==0u&&refused.fills==0u);
    }},

    cxet::testing::Case{"universe_refuses_mixed_or_unknown_quote_currency_before_strategy", +[] {
      std::vector<detail::SessionData> sources(2u);
      for(std::size_t i=0u;i<sources.size();++i) {
        sources[i].exchange=i==0u?"Binance":"Bybit";sources[i].market="Futures";
        sources[i].symbol=i==0u?"BTC_USDT":"ETH_USDC";sources[i].quoteAssetId=i+1u;
      }
      hft_backtest::BacktestStrategyConfig config;
      std::vector<hft_backtest::run_detail::UniverseContextSelection> contexts;std::string error;
      CXET_CHECK(!hft_backtest::run_detail::discoverUniverseContexts(singleSourceTopology(),
          config,sources,contexts,error) && contexts.empty() && !error.empty());
      sources[1].quoteAssetId=0u;error.clear();
      CXET_CHECK(!hft_backtest::run_detail::discoverUniverseContexts(singleSourceTopology(),
          config,sources,contexts,error));
      sources[1].quoteAssetId=sources[0].quoteAssetId;error.clear();
      CXET_CHECK(hft_backtest::run_detail::discoverUniverseContexts(singleSourceTopology(),
          config,sources,contexts,error) && contexts.size()==2u);
    }},
