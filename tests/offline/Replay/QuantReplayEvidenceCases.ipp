    cxet::testing::Case{"service_identity_exhaustion_refuses_without_namespace_reset", +[] {
      auto engine = std::make_unique<detail::EngineState>();
      engine->nextServiceClientIntentRaw = std::numeric_limits<std::uint64_t>::max();
      CXET_CHECK(hft_backtest::strategy_runtime_detail::allocateBacktestServiceClientIntentId(*engine).raw == std::numeric_limits<std::uint64_t>::max());
      CXET_CHECK(hft_backtest::strategy_runtime_detail::allocateBacktestServiceClientIntentId(*engine).raw == 0u);
      CXET_CHECK(hft_backtest::strategy_runtime_detail::allocateBacktestServiceClientIntentId(*engine).raw == 0u);
    }},
    cxet::testing::Case{"simulation_evidence_capacity_refuses_without_terminal_eviction", +[] {
      auto engine = std::make_unique<detail::EngineState>(); engine->activeContextIndex = 1u;
      prepareCashReplayEngine(*engine);
      CXET_CHECK(detail::prepareSimulationOrderEvidence(*engine));
      trading_core::OrderIntent intent; intent.orderType = canon::OrderType::Limit;
      intent.limitTimeInForcePolicy = trading_core::LimitTimeInForcePolicy::Ioc;
      intent.side = Side::Buy(); intent.qty.raw = 4; intent.price.raw = 100;
      for (std::size_t i = 1u; i <= detail::kSimulationOrderEvidenceCapacity + 1u; ++i) {
        intent.clientIntentId.raw = i;
        CXET_CHECK(!detail::execution_detail::submitOrderIntent(*engine, intent, i, i + 1));
        if (engine->simulationEvidenceFailed) break;
      }
      CXET_CHECK(engine->simulationEvidenceFailed);
      const auto view = detail::simulationOrderEvidenceView(*engine);
      const auto* first = trading_core::strategy::canonicalOrderEvidence(view, Id{1u}, 1u);
      CXET_CHECK(first && first->terminal() && first->request.identity.owner.strategyInstanceId == 2u);
    }},
    cxet::testing::Case{"service_feedback_updates_canonical_store_without_strategy_delivery", +[] {
      auto engine = std::make_unique<detail::EngineState>(); engine->activeContextIndex = 1u;
      prepareCashReplayEngine(*engine);
      hft_backtest::BacktestRateLimitSchedule schedule; schedule.enabled = false;
      std::string quotaError;
      CXET_CHECK(hft_backtest::applyRateLimitSchedule(*engine, schedule, quotaError));
      CXET_CHECK(detail::prepareSimulationOrderEvidence(*engine));
      trading_core::OrderIntent intent; intent.clientIntentId.raw = 31u; intent.service = true;
      intent.orderType = canon::OrderType::Limit; intent.side = Side::Buy(); intent.qty.raw = 4; intent.price.raw = 100;
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*engine, intent, 10, 20));
      detail::applyDueActions(*engine, 20);
      CXET_CHECK(engine->userEvents.size() == 1u && engine->userEventServiceFlags[0] == 1u);
      const auto view = detail::simulationOrderEvidenceView(*engine);
      const auto* record = trading_core::strategy::canonicalOrderEvidence(view, intent.clientIntentId, 1u);
      CXET_CHECK(record && !record->venueObserved);
      CXET_CHECK(detail::deliverSimulationOrderEvent(*engine, 0u, engine->userEventDeliveryTsNs[0]));
      CXET_CHECK(record->venueObserved && !record->terminal());
      trading_core::StrategyContext ctx; ctx.hasUserEvent = true; ctx.userEvent = &engine->userEvents[0];
      hft_backtest::bindBacktestUserEventIdentity(ctx, *engine);
      CXET_CHECK(ctx.userEventGeneration == 0u && ctx.userEventClientIntentId.raw == 0u);
    }},
    cxet::testing::Case{"simulated_cancel_terminal_evidence_is_final_only_at_private_delivery", +[] {
      auto engine = std::make_unique<detail::EngineState>();
      prepareCashReplayEngine(*engine);
      hft_backtest::BacktestRateLimitSchedule schedule; schedule.enabled = false;
      std::string quotaError;
      CXET_CHECK(hft_backtest::applyRateLimitSchedule(*engine, schedule, quotaError));
      engine->activeContextIndex = 1u;
      CXET_CHECK(detail::prepareSimulationOrderEvidence(*engine));
      detail::BacktestLatencyConfig latency; latency.userData = {1u, 0u};
      detail::initLatencyRuntime(engine->latency, latency);
      trading_core::OrderIntent intent; intent.clientIntentId.raw = 41u;
      intent.orderType = canon::OrderType::Limit; intent.side = Side::Buy();
      intent.qty.raw = 4; intent.price.raw = 100;
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*engine, intent, 10, 20));
      auto view = detail::simulationOrderEvidenceView(*engine);
      const auto* placed = trading_core::strategy::canonicalOrderEvidence(view, intent.clientIntentId, 1u);
      CXET_CHECK(placed && placed->placeSettled && !placed->venueObserved && !placed->terminal());
      detail::applyDueActions(*engine, 20);
      CXET_CHECK(engine->userEvents.size() == 1u);
      CXET_CHECK(!placed->venueObserved);
      CXET_CHECK(detail::deliverSimulationOrderEvent(*engine, 0u, engine->userEventDeliveryTsNs[0]));
      CXET_CHECK(placed->venueObserved && !placed->terminal());
      trading_core::CancelIntent cancel; cancel.clientIntentId.raw = 42u; cancel.targetClientIntentId = intent.clientIntentId;
      CXET_CHECK(detail::execution_detail::submitCancelIntent(*engine, cancel, 1030, 1040));
      detail::applyDueActions(*engine, 1040);
      CXET_CHECK(engine->userEvents.size() == 2u && !placed->terminal());
      CXET_CHECK(!placed->terminal());
      CXET_CHECK(detail::deliverSimulationOrderEvent(*engine, 1u, engine->userEventDeliveryTsNs[1]));
      CXET_CHECK(placed->terminal());
    }},
    cxet::testing::Case{"simulated_evidence_refuses_foreign_context_source_owner_and_generation", +[] {
      auto engine = std::make_unique<detail::EngineState>();
      prepareCashReplayEngine(*engine);
      hft_backtest::BacktestRateLimitSchedule schedule; schedule.enabled = false;
      std::string quotaError;
      CXET_CHECK(hft_backtest::applyRateLimitSchedule(*engine, schedule, quotaError));
      auto foreign = std::make_unique<detail::EngineState>(); foreign->legIndex = 1u;
      engine->activeContextIndex = foreign->activeContextIndex = 1u;
      CXET_CHECK(detail::prepareSimulationOrderEvidence(*engine) && detail::prepareSimulationOrderEvidence(*foreign));
      trading_core::OrderIntent intent; intent.clientIntentId.raw = 77u;
      intent.orderType = canon::OrderType::Limit; intent.side = Side::Buy(); intent.qty.raw = 4; intent.price.raw = 100;
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*engine, intent, 10, 20));
      auto view = detail::simulationOrderEvidenceView(*engine);
      CXET_CHECK(trading_core::strategy::canonicalOrderEvidence(view, intent.clientIntentId, 1u));
      auto otherContext = view; otherContext.contextIndex = 2u;
      CXET_CHECK(!trading_core::strategy::canonicalOrderEvidence(otherContext, intent.clientIntentId, 1u));
      auto otherOwner = view; ++otherOwner.owner.strategyInstanceId;
      CXET_CHECK(!trading_core::strategy::canonicalOrderEvidence(otherOwner, intent.clientIntentId, 1u));
      CXET_CHECK(!trading_core::strategy::canonicalOrderEvidence(view, intent.clientIntentId, 2u));
      auto otherSource = detail::simulationOrderEvidenceView(*foreign);
      otherSource.rows = view.rows; otherSource.binding = view.binding;
      CXET_CHECK(!trading_core::strategy::canonicalOrderEvidence(otherSource, intent.clientIntentId, 1u));
    }},
