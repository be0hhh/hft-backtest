#pragma once

#include "OfflineCase.hpp"
#include "Engine/BacktestEngine.hpp"
#include "Engine/BacktestEngineSetup.hpp"
#include "Execution/Internal/BacktestExecutionHelpers.hpp"
#include "Execution/BacktestOrderEvidence.hpp"
#include "Strategy/BacktestStrategyRuntime.hpp"
#include "Run/BacktestUniverse.hpp"
#include "Accounting/BacktestResultAggregation.hpp"
#include "Accounting/BacktestMetrics.hpp"
#include "Strategy/Inputs/BacktestStrategyWarmup.hpp"
#include "Strategy/Internal/BacktestStrategyRuntimeHelpers.hpp"
#include "trading_core/Strategy/Orders/Evidence/OrderDecisionCursor.hpp"
#include "Input/Binary/BinaryCorpusStream.hpp"
#include "Input/Binary/BinaryCorpusLoader.hpp"
#include "Strategy/Inputs/BacktestStrategyInstrument.hpp"
#include "Input/Binary/Internal/BinaryCorpusHelpers.hpp"
#include "Strategy/Execution/BacktestStrategyIntentLowering.hpp"
#include "hftrec/CorpusContract/BinaryMarketCorpusWriter.hpp"
#include "hftrec/CorpusContract/BinaryMarketCorpusReader.hpp"
#include "cxet/Canon/ExchangeRegistry.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <limits>
#include <string>
#include <unistd.h>
#include <utility>

namespace {
namespace detail = hft_backtest::detail;
namespace corpus = hftrec::corpus;

struct QuantityFixtureConfig {
  corpus::BinaryMarketQuantityConversion conversion{};
  std::int64_t qtyRaw{100'000'000};
  std::uint8_t scale{8u};
  std::uint8_t marketKind{1u};
  std::uint8_t marketRaw{};
  std::string_view venue{"binance"};
  bool allChannels{};
  bool changeConversion{};
  bool wholeUniverseSources{};
  bool recordedOnlyRequiredSource{};
  bool noncompatibleRequiredRow{};
  QuantityFixtureConfig() {
    conversion.kinds.fill(corpus::BinaryMarketQuantityKind::CanonicalBase);
    conversion.canonicalBaseMultiplier = conversion.nativeBaseMultiplier = 1u;
  }
};

struct SpotFixture {
  static constexpr std::int64_t q = 100'000'000;
  detail::SharedAccountState account{};
  SpotFixture() {
    account.accountingModel = detail::SimulationAccountingModel::Cash;
    account.quoteAssetId = 1u;
    account.initialBalanceE8 = account.walletBalanceE8 = 100 * q;
  }
  std::unique_ptr<detail::EngineState> engine(std::uint32_t source, canon::MarketType market = canon::kMarketTypeSpot,
      detail::SimulationAccountingModel model = detail::SimulationAccountingModel::Cash) {
    auto out = std::make_unique<detail::EngineState>(); out->legIndex = source; out->market = market;
    out->accountingModel = model;
    if (account.observedInitialized) CXET_CHECK(account.accountingModel == model);
    account.accountingModel = out->accountingModel;
    out->activeContextIndex = 1u; out->sharedAccount = &account;
    hft_backtest::BacktestRateLimitSchedule schedule; schedule.enabled = false;
    std::string quotaError;
    CXET_CHECK(hft_backtest::applyRateLimitSchedule(*out, schedule, quotaError));
    out->exchange = detail::exchangeFromText("binance"); out->symbol.copyFrom(source ? "ETH_USDT" : "BTC_USDT");
    hft_backtest::BacktestRunRequest request{}; request.initialBalanceE8 = source ? 0 : account.initialBalanceE8;
    request.hasMakerFeeBps = request.hasTakerFeeBps = true;
    detail::initializeAccounting(*out, request);
    if (model == detail::SimulationAccountingModel::Linear)
      CXET_CHECK(detail::configureSimulationDerivativeCollateral(*out, 10 * q, q, account.quoteAssetId));
    CXET_CHECK(out->market.raw == market.raw);
    top(*out, 60 * q, 60 * q);
    detail::BacktestLatencyConfig config{}; config.userData = {1u, 0u}; detail::initLatencyRuntime(out->latency, config);
    return out;
  }
  static void top(detail::EngineState& engine, std::int64_t bid, std::int64_t ask) {
    engine.venueSnapshot.hasBookTicker = engine.snapshot.hasBookTicker = trading_core::kAlgoFlagTrue;
    engine.venueSnapshot.lastBookTicker.bid.px.raw = bid; engine.venueSnapshot.lastBookTicker.ask.px.raw = ask;
    engine.venueSnapshot.lastBookTicker.bid.qty.raw = engine.venueSnapshot.lastBookTicker.ask.qty.raw = 10 * q;
    engine.snapshot.lastBookTicker = engine.venueSnapshot.lastBookTicker;
    detail::recordSpotValuationMark(engine, false);
    detail::recordSpotValuationMark(engine, true);
    detail::refreshSimulationDerivativeCollateral(engine, false);
    detail::refreshSimulationDerivativeCollateral(engine, true);
  }
  static trading_core::OrderIntent intent(std::uint64_t id, Side side, std::int64_t qty = q,
      canon::OrderType type = canon::OrderType::Market, std::int64_t price = 0) {
    trading_core::OrderIntent out{}; out.clientIntentId.raw = id; out.side = side; out.qty.raw = qty;
    out.orderType = type; out.price.raw = price; return out;
  }
  static void deliverAll(detail::EngineState& engine) {
    while (engine.deliveredUserEvents < engine.userEvents.size()) {
      const auto index = engine.deliveredUserEvents; engine.activeContextIndex = engine.userEventContextIndices[index];
      CXET_CHECK(detail::deliverSimulationOrderEvent(engine, index, engine.userEventDeliveryTsNs[index]));
    }
  }
};

void prepareCashReplayEngine(detail::EngineState& engine, std::int64_t cash = 100'000'000) {
  const auto nativeProduct = engine.market.raw;
  engine.accountingModel = detail::SimulationAccountingModel::Cash;
  hft_backtest::BacktestRunRequest request{}; request.initialBalanceE8 = cash;
  request.hasMakerFeeBps = request.hasTakerFeeBps = true;
  detail::initializeAccounting(engine, request);
  CXET_CHECK(engine.market.raw == nativeProduct);
}

struct QuantFixture {
  std::filesystem::path root = std::filesystem::current_path() /
      ("backtest-quant-fixture-" + std::to_string(::getpid()) + "-" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::string venueName, marketName;
  explicit QuantFixture(std::int64_t priceBasis = 0, bool executionAuthority = false,
                        std::uint8_t assetDomain = 1u, QuantityFixtureConfig quantity = {}) {
    std::vector<corpus::BinaryMarketSource> sources(quantity.wholeUniverseSources ? 3u : 1u);
    auto& source = sources[0];
    source.sourceId = 1u; source.canonicalSymbolId = 7u; source.initialSourceGeneration = 11u;
    source.venueId = detail::exchangeFromText(std::string(quantity.venue)).raw;
    source.marketRaw = quantity.marketRaw ? quantity.marketRaw : quantity.marketKind == 1u ? 2u : 1u;
    source.marketKindRaw = quantity.marketKind; source.assetDomainRaw = assetDomain;
    source.quantityConversion = quantity.conversion;
    source.economicBaseAssetId = 1u; source.quoteAssetId = 2u;
    source.priceScale = 8u; source.quantityScale = quantity.scale;
    source.tickSizeRaw = 1; source.stepSizeRaw = 6;
    source.priceBasisQtyRaw = priceBasis;
    source.directoryFlags = corpus::BinaryMarketDirectoryBookTicker | corpus::BinaryMarketDirectoryTickSize;
    source.instrumentMetadataFlags = corpus::BinaryMarketInstrumentStepSize;
    if (executionAuthority) {
      source.canonicalStepSizeRaw = 2;
      source.executionQuantityStepRaw = 4;
      source.instrumentMetadataFlags |= corpus::BinaryMarketInstrumentCanonicalStepSize |
          corpus::BinaryMarketInstrumentExecutionQuantityStep;
    }
    source.configuredChannelMask = source.availableChannelMask = source.traderReplayChannelMask =
        corpus::binaryMarketChannelBit(corpus::BinaryMarketChannel::BookTicker);
    source.compatibility[corpus::binaryMarketChannelIndex(corpus::BinaryMarketChannel::BookTicker)] =
        corpus::BinaryMarketCompatibility::ExactTraderReplay;
    if (quantity.allChannels) {
      source.configuredChannelMask |= corpus::binaryMarketChannelBit(corpus::BinaryMarketChannel::Trade) |
          corpus::binaryMarketChannelBit(corpus::BinaryMarketChannel::Depth);
      source.availableChannelMask = source.traderReplayChannelMask = source.configuredChannelMask;
      source.directoryFlags |= corpus::BinaryMarketDirectoryTrade | corpus::BinaryMarketDirectoryDepth;
      source.compatibility[1] = source.compatibility[2] = corpus::BinaryMarketCompatibility::ExactTraderReplay;
    } else {
      source.quantityConversion.kinds[1] = source.quantityConversion.kinds[2] = corpus::BinaryMarketQuantityKind::Unknown;
    }
    const auto text = [](auto& destination, std::string_view value) {
      for (std::size_t i = 0u; i < value.size(); ++i) destination[i] = value[i];
    };
    text(source.venue, quantity.venue); source.venueBytes = quantity.venue.size();
    const auto marketText = source.marketRaw == 2u ? std::string_view{"spot"} :
        source.marketRaw == 6u ? std::string_view{"swap"} : source.marketRaw == 5u ? std::string_view{"inverse"} : std::string_view{"futures"};
    text(source.market, marketText); source.marketBytes = marketText.size();
    venueName = quantity.venue; marketName = marketText;
    text(source.canonicalSymbol, "BTC_USDT"); source.canonicalSymbolBytes = 8u;
    text(source.nativeSymbol, "BTCUSDT"); source.nativeSymbolBytes = 7u;
    if (quantity.wholeUniverseSources) {
      for (std::size_t i = 1u; i < sources.size(); ++i) {
        sources[i] = source; sources[i].sourceId = i + 1u; sources[i].canonicalSymbolId = 7u + i;
        sources[i].canonicalSymbol = {}; sources[i].nativeSymbol = {};
        text(sources[i].canonicalSymbol, i == 1u ? "ETH_USDT" : "LTC_USDT");
        text(sources[i].nativeSymbol, i == 1u ? "ETHUSDT" : "LTCUSDT");
      }
      if (quantity.recordedOnlyRequiredSource) {
        sources[2].traderReplayChannelMask = 0u;
        sources[2].compatibility[0] = corpus::BinaryMarketCompatibility::RecordedOnly;
      }
    }
    corpus::BinaryMarketWriterConfig config{};
    config.root = root; config.sources = sources; config.producerEpoch = 13u;
    config.maximumBytes = 128u * 1024u; config.segmentTargetBytes = 4096u;
    config.targetDurationNs = 10'000u; config.startedReceiveNs = 100;
    config.startedMonotonicNs = 100u; config.ringCapacity = 2u; config.shardCount = 1u;
    corpus::BinaryMarketCorpusWriter writer;
    CXET_CHECK(writer.start(config) == hftrec::Status::Ok);
    corpus::BinaryMarketRecord record{};
    auto& header = record.header;
    header.sourceId = 1u; header.sourceGeneration = 11u; header.sessionEpoch = 17u;
    header.eventSequence = header.frameSequence = header.shardSequence = 1u;
    header.receiveRealtimeNs = 200; header.receiveMonotonicNs = 200u; header.exchangeTimestampNs = 199;
    header.channel = corpus::BinaryMarketChannel::BookTicker;
    header.flags = corpus::BinaryMarketRecordTraderReplayCompatible;
    header.sourceFlags = corpus::BinaryMarketSourceBookTickerBidPresent | corpus::BinaryMarketSourceBookTickerAskPresent;
    header.payloadBytes = sizeof(corpus::BinaryMarketBookTickerPayload);
    *corpus::binaryMarketPayload<corpus::BinaryMarketBookTickerPayload>(&record) =
        {10'000'000'000, quantity.qtyRaw, 10'010'000'000, quantity.qtyRaw};
    CXET_CHECK(writer.append(record) == hftrec::Status::Ok);
    if (quantity.wholeUniverseSources) {
      auto addedSymbol = record; addedSymbol.header.sourceId = 2u; addedSymbol.header.shardSequence = 2u;
      addedSymbol.header.receiveMonotonicNs = addedSymbol.header.receiveRealtimeNs = 350u;
      CXET_CHECK(writer.append(addedSymbol) == hftrec::Status::Ok);
      if (quantity.recordedOnlyRequiredSource || quantity.noncompatibleRequiredRow) {
        auto unsupported = addedSymbol; unsupported.header.sourceId = 3u; unsupported.header.shardSequence = 3u;
        unsupported.header.receiveMonotonicNs = unsupported.header.receiveRealtimeNs = 400u;
        unsupported.header.flags = 0u;
        CXET_CHECK(writer.append(unsupported) == hftrec::Status::Ok);
      }
    }
    if (quantity.allChannels) {
      header.eventSequence = header.frameSequence = header.shardSequence = 2u;
      header.receiveMonotonicNs = header.receiveRealtimeNs = 250u;
      header.channel = corpus::BinaryMarketChannel::Trade; header.sourceFlags = 0u;
      header.payloadBytes = sizeof(corpus::BinaryMarketTradePayload);
      record.payload = {};
      *corpus::binaryMarketPayload<corpus::BinaryMarketTradePayload>(&record) = {10'000'000'000, quantity.qtyRaw, 1u, {}};
      CXET_CHECK(writer.append(record) == hftrec::Status::Ok);
      record = {}; auto& depthHeader = record.header;
      depthHeader.sourceId = 1u; depthHeader.sourceGeneration = 11u; depthHeader.sessionEpoch = 17u;
      depthHeader.eventSequence = depthHeader.frameSequence = depthHeader.shardSequence = 3u;
      depthHeader.receiveMonotonicNs = depthHeader.receiveRealtimeNs = 300u; depthHeader.exchangeTimestampNs = 299;
      depthHeader.channel = corpus::BinaryMarketChannel::Depth;
      depthHeader.flags = corpus::BinaryMarketRecordTraderReplayCompatible | corpus::BinaryMarketRecordDepthSnapshot | corpus::BinaryMarketRecordDepthRebase;
      depthHeader.payloadBytes = sizeof(corpus::BinaryMarketDepthChunkPayload);
      auto& depth = *corpus::binaryMarketPayload<corpus::BinaryMarketDepthChunkPayload>(&record); depth = {};
      depth.totalLevelCount = depth.levelCount = 2u; depth.partCount = depth.transactionPartCount = 1u;
      depth.frameKind = corpus::BinaryMarketDepthFrameKind::Snapshot; depth.state = corpus::BinaryMarketDepthState::RebaseInFlight;
      depth.sequence.firstUpdateId = depth.sequence.lastUpdateId = 1u;
      depth.sourceReceiveRealtimeNs = depth.sourceFreshnessMonotonicNs = 300u;
      depth.levels[0] = {10'000'000'000u, static_cast<std::uint64_t>(quantity.qtyRaw), 1u, 1u, {}};
      depth.levels[1] = {10'010'000'000u, static_cast<std::uint64_t>(quantity.qtyRaw), 2u, 1u, {}};
      CXET_CHECK(writer.append(record) == hftrec::Status::Ok);
    }
    if (quantity.changeConversion) {
      record = {}; auto& lifeHeader = record.header;
      lifeHeader.sourceId = 1u; lifeHeader.sourceGeneration = 12u; lifeHeader.sessionEpoch = 17u;
      lifeHeader.eventSequence = lifeHeader.shardSequence = quantity.allChannels ? 4u : 2u;
      lifeHeader.receiveRealtimeNs = lifeHeader.receiveMonotonicNs = 400u;
      lifeHeader.channel = corpus::BinaryMarketChannel::SourceLifecycle;
      lifeHeader.flags = corpus::BinaryMarketRecordSourceLifecycle | corpus::BinaryMarketRecordTraderReplayCompatible | corpus::BinaryMarketRecordExchangeTimestampMissing;
      lifeHeader.payloadBytes = sizeof(corpus::BinaryMarketSourceLifecyclePayload);
      auto& life = *corpus::binaryMarketPayload<corpus::BinaryMarketSourceLifecyclePayload>(&record); life = {};
      life.directoryRevision = life.requestSequence = 1u; life.previousSourceGeneration = 11u;
      life.channelMaskBefore = life.channelMaskAfter = source.availableChannelMask;
      life.kind = corpus::BinaryMarketSourceLifecycleKind::DirectoryChanged;
      life.affectedChannel = static_cast<std::uint8_t>(corpus::BinaryMarketChannel::BookTicker);
      life.source = source; life.source.initialSourceGeneration = 12u;
      ++life.source.quantityConversion.canonicalBaseMultiplier;
      CXET_CHECK(writer.append(record) == hftrec::Status::Ok);
    }
    CXET_CHECK(writer.finalize(corpus::BinaryMarketStopReason::Requested, 10'000) == hftrec::Status::Ok);
  }
  ~QuantFixture() { std::error_code error; std::filesystem::remove_all(root, error); }
  struct Loaded {
    hft_backtest::Status status{};
    std::string error;
    detail::SessionData session;
  };
  Loaded load(std::string symbol = "BTC_USDT") const {
    Loaded result;
    detail::SessionLoadPlan plan; plan.loadTrades = false; plan.requireBookTickers = true;
    result.status = detail::loadBinaryMarketCorpus(root,
        {venueName, marketName, std::move(symbol), 100, 10'000}, plan, result.session, result.error);
    return result;
  }
};

}
