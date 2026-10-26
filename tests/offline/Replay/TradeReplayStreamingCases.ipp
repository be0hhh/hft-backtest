    cxet::testing::Case{"binary_stream_long_interval_does_not_accumulate_rows_or_sort_exchange_time", +[] {
      BinaryTradeFixture fixture(2u, 199u, 7u, 16u);
      detail::BinaryCorpusStream stream;
      std::string error;
      const std::array requests{fixture.request()};
      CXET_CHECK(stream.open(fixture.root, requests, fixture.plan(), error) == hft_backtest::Status::Ok);
      std::vector<detail::TimelineEvent> events;
      bool available = false;
      for (std::uint32_t row = 0u; row < 16u; ++row) {
        CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Ok && available);
        CXET_CHECK(events.size() == 1u && events[0].exchangeTsNs == 199 - row &&
            events[0].receiveMonotonicNs == 200u + row &&
            events[0].tsNs == events[0].deliveryTsNs);
        CXET_CHECK(stream.sessions()[0].trades.size() == 1u);
      }
      CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Ok && !available);
    }},
    cxet::testing::Case{"simulated_event_identity_survives_terminal_slot_and_native_text", +[] {
      auto engine = std::make_unique<detail::EngineState>();
      prepareCashTradeReplayEngine(*engine);
      hft_backtest::BacktestRateLimitSchedule schedule; schedule.enabled = false;
      std::string quotaError;
      CXET_CHECK(hft_backtest::applyRateLimitSchedule(*engine, schedule, quotaError));
      trading_core::OrderIntent order;
      order.clientIntentId.raw = 1'000'000'000'007u;
      order.orderType=canon::OrderType::Limit;order.side=Side::Buy();order.qty.raw=1;order.price.raw=100;
      CXET_CHECK(detail::prepareSimulationOrderEvidence(*engine));
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*engine,order,10,20));
      detail::applyDueActions(*engine,20);
      CXET_CHECK(engine->userEvents.size()==1u);
      const auto& ack = engine->userEvents[0];
      CXET_CHECK(ack.quantityUnit == cxet::contract::Unit::BaseQuantity &&
          ack.quantityDenomination == cxet::contract::QuantityDenomination::Canonical);
      CXET_CHECK(cxet::composite::userDataFieldPresent(ack, cxet::composite::UserDataField::QuantityUnit) &&
          cxet::composite::userDataFieldPresent(ack, cxet::composite::UserDataField::QuantityDenomination));
      CXET_CHECK(detail::deliverSimulationOrderEvent(*engine,0u,engine->userEventDeliveryTsNs[0]));
      trading_core::CancelIntent cancel;cancel.clientIntentId.raw=2u;cancel.targetClientIntentId=order.clientIntentId;
      CXET_CHECK(detail::execution_detail::submitCancelIntent(*engine,cancel,30,40));
      detail::applyDueActions(*engine,40);
      CXET_CHECK(engine->userEvents.size()==2u);
      const auto& canceled = engine->userEvents[1];
      CXET_CHECK(canceled.quantityUnit == cxet::contract::Unit::BaseQuantity &&
          canceled.quantityDenomination == cxet::contract::QuantityDenomination::Canonical);
      CXET_CHECK(cxet::composite::userDataFieldPresent(canceled, cxet::composite::UserDataField::QuantityUnit) &&
          cxet::composite::userDataFieldPresent(canceled, cxet::composite::UserDataField::QuantityDenomination));
      CXET_CHECK(detail::deliverSimulationOrderEvent(*engine,1u,engine->userEventDeliveryTsNs[1]));
      const auto event=engine->userEvents[1];
      trading_core::StrategyContext context{};
      context.userEvent = &engine->userEvents[1];
      context.hasUserEvent = true;
      context.userEventAdmittedSemanticMask = cxet::composite::userDataSemanticBit(
          cxet::composite::UserDataSemantic::OrderTransition);
      hft_backtest::bindBacktestUserEventIdentity(context, *engine);
      trading_core::strategy::RuntimeClientOrderId admitted{};
      CXET_CHECK(trading_core::strategy::admittedStrategyEventIdentity(context, admitted));
      CXET_CHECK(admitted.clientIntentId.raw == order.clientIntentId.raw && admitted.generation == 1u);
      engine->executionQuality.emplace_back();
      auto& quality=engine->executionQuality.back();
      quality.legCount=1u;quality.legs[0].clientIntentId=order.clientIntentId.raw;
      quality.statusRaw=static_cast<std::uint8_t>(trading_core::StrategyExecutionStatus::Uncertain);
      hft_backtest::recordExecutionQualityFill(*engine,context,0u,40,{});
      CXET_CHECK(quality.statusRaw==static_cast<std::uint8_t>(trading_core::StrategyExecutionStatus::Rejected));
      quality.statusRaw=static_cast<std::uint8_t>(trading_core::StrategyExecutionStatus::Uncertain);
      auto foreign = event;
      foreign.eventId.raw += 1u;
      context.userEvent = &foreign;
      hft_backtest::bindBacktestUserEventIdentity(context, *engine);
      CXET_CHECK(!trading_core::strategy::admittedStrategyEventIdentity(context, admitted));
      hft_backtest::recordExecutionQualityFill(*engine,context,0u,40,{});
      CXET_CHECK(quality.statusRaw==static_cast<std::uint8_t>(trading_core::StrategyExecutionStatus::Uncertain));
    }},
    cxet::testing::Case{"binary_stream_retains_only_current_transaction_and_exchange_time", +[] {
      BinaryTradeFixture fixture(2u, 199u, 7u);
      detail::BinaryCorpusStream stream;
      std::string error;
      const std::array requests{fixture.request()};
      CXET_CHECK(stream.open(fixture.root, requests, fixture.plan(), error) == hft_backtest::Status::Ok);
      CXET_CHECK(stream.sessions().size() == 1u && stream.sessions()[0].trades.empty());
      std::vector<detail::TimelineEvent> events;
      bool available = false;
      CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Ok && available);
      CXET_CHECK(events.size() == 1u && events[0].exchangeTsNs == 199 &&
          events[0].tsNs == events[0].deliveryTsNs && events[0].receiveMonotonicNs == 200u);
      const auto& rows = stream.sessions()[0].trades;
      CXET_CHECK(rows.size() == 1u && rows[0].tsNs == 199 && rows[0].nativeIdentity.nativeSecond == 7u);
      CXET_CHECK(stream.next(events, available, error) == hft_backtest::Status::Ok && !available);
      CXET_CHECK(events.empty() && stream.sessions()[0].trades.empty());
    }},
    cxet::testing::Case{"replay_refuses_canonical_order_evidence_state", +[] {
      const auto& supported = trading_core::kDeliverySumSignalStrategyDescriptor;
      CXET_CHECK(trading_core::strategyDescriptorContractValid(supported));
      CXET_CHECK(hft_backtest::strategy_intent_lowering::footprintSupported(supported));
      // Replay admits canonical execution only with its prepared dispatch seam.
      // The public actor contract alone does not prove simulation support.
      auto descriptor = supported;
      descriptor.commitPreparedDispatch = nullptr;
      CXET_CHECK(trading_core::strategyDescriptorContractValid(descriptor));
      CXET_CHECK(!hft_backtest::strategy_intent_lowering::footprintSupported(descriptor));
      for (const auto stream : {cxet::api::market::PublicMarketDataStream::PriceLimit,
                               cxet::api::market::PublicMarketDataStream::Funding}) {
        descriptor = supported;
        descriptor.streamCount = 2u; descriptor.streams[1] = stream;
        descriptor.optionalStreamMask = 0u;
        detail::SessionLoadPlan plan{}; std::string error;
        CXET_CHECK(!hft_backtest::buildSessionLoadPlan(descriptor, plan, error));
        descriptor.optionalStreamMask = 2u;
        CXET_CHECK(hft_backtest::buildSessionLoadPlan(descriptor, plan, error));
        descriptor.legInputRequirementCount = descriptor.maxLegs;
        descriptor.legInputRequirements[0].required = stream == cxet::api::market::PublicMarketDataStream::PriceLimit
            ? trading_core::kStrategyInputPriceLimit : trading_core::kStrategyInputFunding;
        CXET_CHECK(!hft_backtest::buildSessionLoadPlan(descriptor, plan, error));
      }
      descriptor = supported;
      descriptor.legInputRequirementCount = descriptor.maxLegs;
      descriptor.legInputRequirements[0].required = trading_core::kStrategyInputCandle;
      detail::SessionLoadPlan plan{}; std::string error;
      CXET_CHECK(!hft_backtest::buildSessionLoadPlan(descriptor, plan, error));
    }},
