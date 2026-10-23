    cxet::testing::Case{"shared_account_private_credit_precedes_later_debit_across_reversed_source_indices", +[] {
      SpotFixture fixture; auto debit = fixture.engine(0u); auto credit = fixture.engine(1u);
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*credit, SpotFixture::intent(220u, Side::Buy(), SpotFixture::q / 2), 10, 20));
      detail::applyDueActions(*credit, 20); SpotFixture::deliverAll(*credit);
      CXET_CHECK(fixture.account.observedWalletBalanceE8 == 70 * SpotFixture::q);
      SpotFixture::top(*credit, 70 * SpotFixture::q, 71 * SpotFixture::q);
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*credit, SpotFixture::intent(221u, Side::Sell(), SpotFixture::q / 2), 30, 40));
      detail::applyDueActions(*credit, 40);
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*debit, SpotFixture::intent(222u, Side::Buy(), SpotFixture::q / 2), 30, 40));
      detail::applyDueActions(*debit, 40);
      const auto creditHead = credit->deliveredUserEvents;
      CXET_CHECK(credit->userEvents[creditHead].updateTime.raw == debit->userEvents[0].updateTime.raw);
      CXET_CHECK(credit->userEventDeliveryTsNs[creditHead + 1] < debit->userEventDeliveryTsNs[0]);
      CXET_CHECK(fixture.account.walletBalanceE8 == 75 * SpotFixture::q);
      CXET_CHECK(fixture.account.observedWalletBalanceE8 == 70 * SpotFixture::q);
      CXET_CHECK(hft_backtest::accountStateFromEngine(*debit).availableBalanceE8 == 40 * SpotFixture::q);
      SpotFixture::deliverAll(*credit);
      CXET_CHECK(fixture.account.observedWalletBalanceE8 == 105 * SpotFixture::q);
      SpotFixture::deliverAll(*debit);
      CXET_CHECK(fixture.account.observedWalletBalanceE8 == 75 * SpotFixture::q);
    }},
    cxet::testing::Case{"spot_rejects_naked_ask_without_negative_base_inventory", +[] {
      SpotFixture fixture; auto engine = fixture.engine(0u);
      const auto sell = SpotFixture::intent(201u, Side::Sell());
      (void)detail::execution_detail::submitOrderIntent(*engine, sell, 10, 20); detail::applyDueActions(*engine, 20);
      CXET_CHECK(engine->fills.empty() && engine->netQtyRaw == 0 && fixture.account.walletBalanceE8 == 100 * SpotFixture::q);
      SpotFixture::deliverAll(*engine); CXET_CHECK(engine->position.netQty.raw == 0);
    }},
    cxet::testing::Case{"spot_two_symbols_cannot_spend_shared_cash_twice", +[] {
      SpotFixture fixture; auto btc = fixture.engine(0u); auto eth = fixture.engine(1u);
      (void)detail::execution_detail::submitOrderIntent(*btc, SpotFixture::intent(202u, Side::Buy()), 10, 20);
      CXET_CHECK(hft_backtest::accountStateFromEngine(*eth).availableBalanceE8 == 40 * SpotFixture::q);
      (void)detail::execution_detail::submitOrderIntent(*eth, SpotFixture::intent(203u, Side::Buy()), 10, 20);
      detail::applyDueActions(*btc, 20); detail::applyDueActions(*eth, 20);
      CXET_CHECK(btc->fills.size() + eth->fills.size() == 1u && fixture.account.walletBalanceE8 == 40 * SpotFixture::q);
      SpotFixture::deliverAll(*btc); SpotFixture::deliverAll(*eth);
      CXET_CHECK(hft_backtest::accountStateFromEngine(*eth).walletBalanceE8 == 40 * SpotFixture::q);
    }},
    cxet::testing::Case{"spot_partial_fill_and_cancel_release_observed_reserve_only_at_private_terminal", +[] {
      SpotFixture fixture; auto engine = fixture.engine(0u); SpotFixture::top(*engine, 79 * SpotFixture::q, 80 * SpotFixture::q);
      const auto buy = SpotFixture::intent(204u, Side::Buy(), SpotFixture::q, canon::OrderType::Limit, 60 * SpotFixture::q);
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*engine, buy, 10, 20)); detail::applyDueActions(*engine, 20);
      SpotFixture::deliverAll(*engine);
      CXET_CHECK(detail::fillOrderQty(*engine, engine->open[0], 60 * SpotFixture::q, SpotFixture::q / 2, 30, false, {}));
      CXET_CHECK(fixture.account.walletBalanceE8 == 70 * SpotFixture::q);
      CXET_CHECK(hft_backtest::accountStateFromEngine(*engine).walletBalanceE8 == 100 * SpotFixture::q);
      SpotFixture::deliverAll(*engine);
      CXET_CHECK(hft_backtest::accountStateFromEngine(*engine).walletBalanceE8 == 70 * SpotFixture::q);
      CXET_CHECK(hft_backtest::accountStateFromEngine(*engine).availableBalanceE8 == 40 * SpotFixture::q);
      trading_core::CancelIntent cancel{}; cancel.clientIntentId.raw = 205u; cancel.targetClientIntentId = buy.clientIntentId;
      CXET_CHECK(detail::execution_detail::submitCancelIntent(*engine, cancel, 40, 50)); detail::applyDueActions(*engine, 50);
      CXET_CHECK(hft_backtest::accountStateFromEngine(*engine).availableBalanceE8 == 40 * SpotFixture::q);
      SpotFixture::deliverAll(*engine);
      CXET_CHECK(hft_backtest::accountStateFromEngine(*engine).availableBalanceE8 == 70 * SpotFixture::q && engine->position.netQty.raw == SpotFixture::q / 2);
      std::vector<detail::EngineState> sources; sources.push_back(std::move(*engine));
      hft_backtest::BacktestRunResult result{};
      hft_backtest::populatePortfolioResultPnl(result, {}, sources, 100 * SpotFixture::q);
      CXET_CHECK(result.walletBalanceE8 == 70 * SpotFixture::q && result.netRealizedPnlE8 == 0);
      const auto markedPnl = 19 * SpotFixture::q / 2;
      CXET_CHECK(result.totalPnlE8 == markedPnl && result.unrealizedPnlE8 == markedPnl);
      CXET_CHECK(!result.totalPnlCurveE8.empty() && result.totalPnlCurveE8.back() == markedPnl);
      CXET_CHECK(sources[0].equityPoints.back().unrealizedPnlE8 == markedPnl);
      auto aggregate = std::make_unique<detail::EngineState>();
      hft_backtest::populateAggregateArtifactEngine(sources, 100 * SpotFixture::q, 50, *aggregate);
      CXET_CHECK(aggregate->walletBalanceE8 == 70 * SpotFixture::q);
      CXET_CHECK(aggregate->equityPoints.back().totalPnlE8 == markedPnl);
      CXET_CHECK(aggregate->riskSnapshots.back().unrealizedPnlE8 == markedPnl &&
          aggregate->riskSnapshots.back().netTotalPnlE8 == markedPnl);
    }},
    cxet::testing::Case{"spot_buy_sell_conserves_principal_equity_and_realized_pnl", +[] {
      SpotFixture fixture; auto engine = fixture.engine(0u);
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*engine, SpotFixture::intent(206u, Side::Buy()), 10, 20)); detail::applyDueActions(*engine, 20);
      CXET_CHECK(fixture.account.walletBalanceE8 == 40 * SpotFixture::q && detail::equityE8(*engine) == 100 * SpotFixture::q);
      SpotFixture::deliverAll(*engine);
      engine->capturedSourceGeneration = 11u;
      detail::SourceHealthRow pause{}; pause.unhealthy = true;
      pause.record.header.channel = corpus::BinaryMarketChannel::BookTicker;
      pause.record.header.sourceGeneration = 11u;
      std::string error;
      CXET_CHECK(hft_backtest::run_detail::applyCapturedSourceHealth(*engine, pause, 25, error));
      auto sibling = fixture.engine(1u);
      CXET_CHECK(engine->venueSnapshot.hasBookTicker.raw == trading_core::kAlgoFlagFalse.raw &&
          engine->snapshot.hasBookTicker.raw == trading_core::kAlgoFlagFalse.raw);
      CXET_CHECK(engine->netQtyRaw == SpotFixture::q && engine->position.netQty.raw == SpotFixture::q);
      CXET_CHECK(detail::equityE8(*engine) == 100 * SpotFixture::q);
      CXET_CHECK(hft_backtest::accountStateFromEngine(*sibling).equityE8 == 100 * SpotFixture::q);
      SpotFixture::top(*engine, 70 * SpotFixture::q, 71 * SpotFixture::q);
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*engine, SpotFixture::intent(207u, Side::Sell()), 30, 40)); detail::applyDueActions(*engine, 40);
      CXET_CHECK(fixture.account.walletBalanceE8 == 110 * SpotFixture::q && engine->netQtyRaw == 0 && engine->netRealizedPnlE8 == 10 * SpotFixture::q);
      CXET_CHECK(hft_backtest::accountStateFromEngine(*engine).walletBalanceE8 == 40 * SpotFixture::q);
      SpotFixture::deliverAll(*engine);
      CXET_CHECK(hft_backtest::accountStateFromEngine(*engine).walletBalanceE8 == 110 * SpotFixture::q && engine->position.netQty.raw == 0);
    }},
    cxet::testing::Case{"spot_actual_price_overrun_rejects_at_venue_without_future_local_feedback", +[] {
      SpotFixture fixture; auto btc = fixture.engine(0u); auto eth = fixture.engine(1u);
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*btc, SpotFixture::intent(208u, Side::Buy()), 10, 20));
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*eth, SpotFixture::intent(209u, Side::Buy(), SpotFixture::q / 2), 10, 20));
      btc->venueSnapshot.lastBookTicker.ask.px.raw = eth->venueSnapshot.lastBookTicker.ask.px.raw = 80 * SpotFixture::q;
      detail::applyDueActions(*btc, 20); detail::applyDueActions(*eth, 20);
      CXET_CHECK(btc->fills.size() == 1u && eth->fills.empty() && fixture.account.walletBalanceE8 == 20 * SpotFixture::q);
      const auto* record = trading_core::strategy::canonicalOrderEvidence(detail::simulationOrderEvidenceView(*eth), Id{209u}, 1u);
      CXET_CHECK(record && !record->terminal() && !record->venueObserved);
      CXET_CHECK(hft_backtest::accountStateFromEngine(*eth).walletBalanceE8 == 100 * SpotFixture::q);
      SpotFixture::deliverAll(*btc); SpotFixture::deliverAll(*eth);
      CXET_CHECK(record->terminal() && hft_backtest::accountStateFromEngine(*eth).availableBalanceE8 == 20 * SpotFixture::q);
    }},
    cxet::testing::Case{"spot_cash_debits_exact_principal_and_fee_at_private_delivery", +[] {
      SpotFixture fixture; auto engine = fixture.engine(0u); engine->takerFeeBpsE8 = 100'000'000;
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*engine, SpotFixture::intent(210u, Side::Buy()), 10, 20)); detail::applyDueActions(*engine, 20);
      const auto expectedCash = 40 * SpotFixture::q - engine->feesPaidE8;
      CXET_CHECK(engine->feesPaidE8 > 0 && fixture.account.walletBalanceE8 == expectedCash);
      CXET_CHECK(hft_backtest::accountStateFromEngine(*engine).walletBalanceE8 == 100 * SpotFixture::q);
      CXET_CHECK(engine->userEvents.size() == 2u);
      // Admit the ACK as an explicit canonical input so the fill counterexample
      // reaches accounting independently of the simulated producer's markers.
      auto& ack = engine->userEvents[0];
      ack.quantityUnit = cxet::contract::Unit::BaseQuantity;
      ack.quantityDenomination = cxet::contract::QuantityDenomination::Canonical;
      cxet::composite::markUserDataFieldPresent(ack, cxet::composite::UserDataField::QuantityUnit);
      cxet::composite::markUserDataFieldPresent(ack, cxet::composite::UserDataField::QuantityDenomination);
      CXET_CHECK(detail::deliverSimulationOrderEvent(*engine, 0u, engine->userEventDeliveryTsNs[0]));
      auto& fill = engine->userEvents[1];
      const auto producedFill = fill;
      auto canonicalFill = fill;
      canonicalFill.quantityUnit = cxet::contract::Unit::BaseQuantity;
      canonicalFill.quantityDenomination = cxet::contract::QuantityDenomination::Canonical;
      cxet::composite::markUserDataFieldPresent(canonicalFill, cxet::composite::UserDataField::QuantityUnit);
      cxet::composite::markUserDataFieldPresent(canonicalFill, cxet::composite::UserDataField::QuantityDenomination);
      const auto* record = trading_core::strategy::canonicalOrderEvidence(
          detail::simulationOrderEvidenceView(*engine), Id{210u}, 1u);
      CXET_CHECK(record && record->venueObserved && !record->terminal() && record->cumulativeQtyE8 == 0);
      const auto wallet = fixture.account.observedWalletBalanceE8;
      const auto reserved = fixture.account.observedReservedQuoteE8;
      const auto accountRevision = fixture.account.revision;
      const auto contextRevision = engine->contextRevision;
      const auto replayTime = engine->lastReplayMonotonicNs;
      for (unsigned variant = 0u; variant < 8u; ++variant) {
        fill = canonicalFill;
        switch (variant) {
          case 0u: fill.quantityDenomination = cxet::contract::QuantityDenomination::Native; break;
          case 1u: fill.quantityDenomination = cxet::contract::QuantityDenomination::Unknown; break;
          case 2u: fill.quantityUnit = cxet::contract::Unit::ContractQuantity; break;
          case 3u: fill.quantityUnit = cxet::contract::Unit::LotQuantity; break;
          case 4u: fill.fieldPresenceMask &= ~cxet::composite::userDataFieldMaskBit(cxet::composite::UserDataField::QuantityUnit); break;
          case 5u: fill.fieldPresenceMask &= ~cxet::composite::userDataFieldMaskBit(cxet::composite::UserDataField::QuantityDenomination); break;
          case 6u: fill.fieldPresenceMask &= ~cxet::composite::userDataFieldMaskBit(cxet::composite::UserDataField::Qty); break;
          case 7u: fill.quantityUnit = cxet::contract::Unit::None; break;
        }
        CXET_CHECK(!detail::deliverSimulationOrderEvent(*engine, 1u, engine->userEventDeliveryTsNs[1]));
        CXET_CHECK(record->cumulativeQtyE8 == 0 && !record->terminal());
        CXET_CHECK(engine->position.netQty.raw == 0 && engine->deliveredUserEvents == 1u && !engine->userEventConsumed[1]);
        CXET_CHECK(fixture.account.observedWalletBalanceE8 == wallet &&
            fixture.account.observedReservedQuoteE8 == reserved && fixture.account.revision == accountRevision);
        CXET_CHECK(engine->contextRevision == contextRevision && engine->lastReplayMonotonicNs == replayTime);
      }
      fill = producedFill;
      CXET_CHECK(fill.quantityUnit == cxet::contract::Unit::BaseQuantity &&
          fill.quantityDenomination == cxet::contract::QuantityDenomination::Canonical);
      CXET_CHECK(cxet::composite::userDataFieldPresent(fill, cxet::composite::UserDataField::QuantityUnit) &&
          cxet::composite::userDataFieldPresent(fill, cxet::composite::UserDataField::QuantityDenomination));
      SpotFixture::deliverAll(*engine); CXET_CHECK(hft_backtest::accountStateFromEngine(*engine).walletBalanceE8 == expectedCash);
    }},
    cxet::testing::Case{"linear_futures_keep_pnl_wallet_and_allow_short_inventory_model", +[] {
      SpotFixture fixture; auto engine = fixture.engine(0u, canon::kMarketTypeFutures, detail::SimulationAccountingModel::Linear);
      CXET_CHECK(detail::execution_detail::submitOrderIntent(*engine, SpotFixture::intent(211u, Side::Sell()), 10, 20)); detail::applyDueActions(*engine, 20);
      CXET_CHECK(engine->netQtyRaw == -SpotFixture::q && fixture.account.walletBalanceE8 == 100 * SpotFixture::q);
      SpotFixture::deliverAll(*engine); CXET_CHECK(engine->position.netQty.raw == -SpotFixture::q);
    }},
