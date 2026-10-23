    cxet::testing::Case{"drawdown_empty_single_and_portfolio_keep_zero_baseline", +[] {
      auto engine = std::make_unique<detail::EngineState>();
      const auto single = detail::calculateExcursionMetrics(*engine);
      CXET_CHECK(single.maxDrawdownE8 == 0 && single.mfeE8 == 0 && single.maeE8 == 0);
      const auto empty = detail::calculatePortfolioExcursionMetrics({});
      CXET_CHECK(empty.maxDrawdownE8 == 0 && empty.mfeE8 == 0 && empty.maeE8 == 0);
      const std::vector<detail::EngineState> engines(2u);
      const auto portfolio = detail::calculatePortfolioExcursionMetrics(engines);
      CXET_CHECK(portfolio.maxDrawdownE8 == 0 && portfolio.mfeE8 == 0 && portfolio.maeE8 == 0);
    }},
    cxet::testing::Case{"drawdown_first_loss_counts_from_zero_on_equity_and_risk_paths", +[] {
      for (const bool risk : {false, true}) {
        auto engine = std::make_unique<detail::EngineState>();
        for (const auto pnl : {-7, -2, 4, -3}) {
          if (risk) {
            detail::JsonRiskSnapshot row{}; row.netTotalPnlE8 = pnl;
            engine->riskSnapshots.push_back(row);
          } else {
            detail::JsonEquityPoint row{}; row.netTotalPnlE8 = pnl;
            engine->equityPoints.push_back(row);
          }
        }
        hft_backtest::BacktestRunResult result{};
        hft_backtest::populateResultPnl(result, *engine);
        CXET_CHECK(result.maxDrawdownE8 == 7 && result.mfeE8 == 4 && result.maeE8 == -7);
      }
    }},
    cxet::testing::Case{"drawdown_extreme_first_loss_saturates_without_signed_negation", +[] {
      auto engine = std::make_unique<detail::EngineState>();
      detail::JsonRiskSnapshot row{}; row.netTotalPnlE8 = std::numeric_limits<std::int64_t>::min();
      engine->riskSnapshots.push_back(row);
      const auto metrics = detail::calculateExcursionMetrics(*engine);
      CXET_CHECK(metrics.maxDrawdownE8 == std::numeric_limits<std::int64_t>::max());
      CXET_CHECK(metrics.mfeE8 == 0 && metrics.maeE8 == std::numeric_limits<std::int64_t>::min());
    }},
    cxet::testing::Case{"portfolio_drawdown_uses_zero_baseline_and_latest_same_timestamp_leg_rows", +[] {
      for (const bool risk : {false, true}) {
        std::vector<detail::EngineState> engines(2u);
        const auto append = [&](std::size_t leg, std::int64_t ts, std::int64_t pnl) {
          if (risk) {
            detail::JsonRiskSnapshot row{}; row.tsNs = ts; row.netTotalPnlE8 = pnl;
            engines[leg].riskSnapshots.push_back(row);
          } else {
            detail::JsonEquityPoint row{}; row.tsNs = ts; row.netTotalPnlE8 = pnl;
            engines[leg].equityPoints.push_back(row);
          }
        };
        append(0u, 10, -100); append(0u, 10, -7); append(1u, 10, 2);
        append(0u, 20, 8); append(1u, 20, -2);
        append(1u, 30, -10);
        hft_backtest::BacktestRunResult result{};
        hft_backtest::populatePortfolioResultPnl(result, {}, engines, 0);
        CXET_CHECK(result.maxDrawdownE8 == 8 && result.mfeE8 == 6 && result.maeE8 == -5);
        // The first synchronized point alone must include the initial loss.
        if (risk) {
          engines[0].riskSnapshots.resize(2u); engines[1].riskSnapshots.resize(1u);
        } else {
          engines[0].equityPoints.resize(2u); engines[1].equityPoints.resize(1u);
        }
        const auto first = detail::calculatePortfolioExcursionMetrics(engines);
        CXET_CHECK(first.maxDrawdownE8 == 5 && first.mfeE8 == 0 && first.maeE8 == -5);
      }
    }},
    cxet::testing::Case{"bbo_optimism_warning_requires_actual_fills_including_no_depth_mode", +[] {
      auto engine = std::make_unique<detail::EngineState>();
      engine->depthExecutionMode = hft_backtest::BacktestDepthExecutionMode::BboNoDepth;
      hft_backtest::BacktestRunResult result{};
      hft_backtest::appendDepthExecutionWarnings(result, *engine);
      CXET_CHECK(result.warnings.empty());
      engine->depthFallbackFills = 1u;
      hft_backtest::appendDepthExecutionWarnings(result, *engine);
      CXET_CHECK(result.warnings.size() == 1u);
      CXET_CHECK(result.warnings[0].find("optimistic BBO") != std::string::npos);
      CXET_CHECK(result.warnings[0].find("without displayed quantity limits") != std::string::npos);
      hft_backtest::appendDepthExecutionWarnings(result, *engine);
      CXET_CHECK(result.warnings.size() == 1u);
    }},
    cxet::testing::Case{"depth_failure_diagnostic_survives_without_fills_and_keeps_reason", +[] {
      auto engine = std::make_unique<detail::EngineState>();
      engine->depthExecutionMode = hft_backtest::BacktestDepthExecutionMode::BboFallback;
      engine->depthFallbackReason = "missing execution row";
      hft_backtest::BacktestRunResult result{};
      hft_backtest::appendDepthExecutionWarnings(result, *engine);
      CXET_CHECK(result.warnings.size() == 1u);
      CXET_CHECK(result.warnings[0].find("declared execution depth failed") != std::string::npos);
      CXET_CHECK(result.warnings[0].find(engine->depthFallbackReason) != std::string::npos);
      CXET_CHECK(result.warnings[0].find("optimistic BBO") == std::string::npos);
      engine->depthFallbackFills = 2u;
      hft_backtest::appendDepthExecutionWarnings(result, *engine);
      CXET_CHECK(result.warnings.size() == 2u && result.warnings[1].find("optimistic BBO") != std::string::npos);
    }},
    cxet::testing::Case{"portfolio_bbo_warning_identifies_only_legs_with_actual_top_fills", +[] {
      auto aggregate = std::make_unique<detail::EngineState>();
      std::vector<detail::EngineState> legs(2u);
      legs[0].depthExecutionMode = hft_backtest::BacktestDepthExecutionMode::BboNoDepth;
      legs[1].depthExecutionMode = hft_backtest::BacktestDepthExecutionMode::Depth;
      legs[1].depthFallbackFills = 1u;
      hft_backtest::BacktestRunResult result{};
      hft_backtest::appendDepthExecutionWarnings(result, *aggregate, &legs);
      CXET_CHECK(result.warnings.size() == 1u && result.warnings[0].find("leg 1: optimistic BBO") == 0u);
    }},
