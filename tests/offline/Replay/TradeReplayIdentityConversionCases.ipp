    cxet::testing::Case{"trade_replay_captured_zero_tail_stays_known_and_malformed_key_refuses", +[] {
      namespace m = cxet::api::market;
      // The actual reader admits shape2 structurally; replay admission must
      // reject its malformed first limb before any TradeRow is installed.
      for (const auto shape : {std::uint8_t{2u}, std::uint8_t{4u}}) {
        const auto second = shape == 2u ? 7u : 0u;
        BinaryTradeFixture fixture(shape, 0u, second);
        hftrec::corpus::BinaryMarketSelectionRequest selected{};
        selected.root = fixture.root; selected.exchange = "fixture";
        selected.market = "futures"; selected.canonicalSymbol = "BTCUSDT";
        selected.beginReceiveNs = 100; selected.endReceiveNs = 10'000;
        selected.channelMask = selected.requiredChannelMask =
            hftrec::corpus::binaryMarketChannelBit(hftrec::corpus::BinaryMarketChannel::Trade);
        hftrec::corpus::BinaryMarketSelection records;
        std::string error;
        CXET_CHECK(hftrec::corpus::BinaryMarketCorpusReader{}.select(selected, records, error) == hftrec::Status::Ok);
        CXET_CHECK(records.records.size() == 1u && records.records[0].header.nativeIdentityFirst == 0u);
        detail::SessionData loaded;
        const auto status = detail::loadBinaryMarketCorpus(fixture.root, fixture.request(), fixture.plan(), loaded, error);
        if (shape == 2u) {
          CXET_CHECK(status == hft_backtest::Status::CorruptData && loaded.trades.empty());
          CXET_CHECK(error == "invalid binary corpus native trade identity");
        } else {
          CXET_CHECK(status == hft_backtest::Status::Ok && loaded.trades.size() == 1u);
          cxet::composite::Trade converted{};
          CXET_CHECK(detail::toRuntime(loaded.trades[0], converted) && converted.eventId.raw == 0u);
        }
      }
      {
        // Hyperliquid's exact pair projects the second limb (tid).
        BinaryTradeFixture fixture(2u, 199u, 7u);
        detail::SessionData loaded; std::string error;
        CXET_CHECK(detail::loadBinaryMarketCorpus(fixture.root, fixture.request(), fixture.plan(), loaded, error) == hft_backtest::Status::Ok);
        CXET_CHECK(loaded.trades.size() == 1u);
        cxet::composite::Trade converted{};
        CXET_CHECK(detail::toRuntime(loaded.trades[0], converted) && converted.eventId.raw == 7u);
      }
      detail::TradeRow row{}; row.priceE8 = 1; row.qtyE8 = 2; row.tsNs = 100;
      row.nativeIdentity = {.origin=m::PublicMarketIdentityOrigin::NativeTail64,
          .nativeShape=m::PublicMarketNativeIdentityShape::UuidTail64};
      cxet::composite::Trade trade{};
      CXET_CHECK(detail::toRuntime(row, trade));
      trading_core::StrategyMarketSnapshot snapshot{};
      CXET_CHECK(trading_core::copyTradeSnapshot(snapshot, trade, row.nativeIdentity));
      CXET_CHECK(snapshot.lastTradeIdentityShape == m::PublicMarketNativeIdentityShape::UuidTail64 &&
          trade.eventId.raw == 0);
      row.nativeIdentity.nativeSecond = 1; row.priceE8 = 9;
      CXET_CHECK(!detail::toRuntime(row, trade) && trade.price().raw == 1);
    }},
    cxet::testing::Case{"trade_replay_negative_json_refused_without_row_mutation", +[] {
      detail::TradeRow row{};
      row.priceE8 = 11; row.qtyE8 = 12; row.tsNs = 13;
      for (const auto input : {negativePrice, negativeQuantity}) {
        hftrec::replay::TradeRow corpus{};
        CXET_CHECK(hftrec::isOk(hftrec::replay::parseTradeLine(input, corpus)));
        CXET_CHECK(!detail::session_loader::parseContractTradeLine(input, row));
        CXET_CHECK(row.priceE8 == 11 && row.qtyE8 == 12 && row.tsNs == 13);
      }
    }},
    cxet::testing::Case{"trade_replay_zero_json_preserves_existing_admission", +[] {
      detail::TradeRow row{};
      CXET_CHECK(detail::session_loader::parseContractTradeLine(zeroMagnitudes, row));
      cxet::composite::Trade trade{};
      CXET_CHECK(detail::toRuntime(row, trade));
      CXET_CHECK(trade.price().raw == 0 && trade.qty().raw == 0 &&
          trade.initiatorSide() == Initiator::Seller && trade.ts.raw == 100);
    }},
    cxet::testing::Case{"trade_replay_conversion_range_side_and_atomic_refusal", +[] {
      detail::TradeRow row{};
      row.priceE8 = std::numeric_limits<std::int64_t>::max();
      row.qtyE8 = row.priceE8; row.tsNs = 100;
      cxet::composite::Trade trade{};
      for (const auto side : {-1, 0, 1}) {
        row.side = side;
        CXET_CHECK(detail::toRuntime(row, trade));
        CXET_CHECK(trade.price().raw == row.priceE8 && trade.qty().raw == row.qtyE8);
        CXET_CHECK(trade.initiatorSide() == (side == -1 ? Initiator::Unknown :
            (side == 0 ? Initiator::Seller : Initiator::Buyer)));
      }
      row.qtyE8 = -1;
      CXET_CHECK(!detail::toRuntime(row, trade));
      CXET_CHECK(trade.qty().raw == std::numeric_limits<std::int64_t>::max() &&
          trade.initiatorSide() == Initiator::Buyer && trade.ts.raw == 100);
      row.qtyE8 = 1; row.side = 2; row.tsNs = 200;
      CXET_CHECK(!detail::toRuntime(row, trade));
      CXET_CHECK(trade.qty().raw == std::numeric_limits<std::int64_t>::max() &&
          trade.initiatorSide() == Initiator::Buyer && trade.ts.raw == 100);
    }},
