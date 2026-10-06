#include "BacktestUniverse.hpp"
#include "Internal/BacktestRunContext.hpp"
#include "Internal/BacktestRunHelpers.hpp"
#include "../Input/Binary/BinaryCorpusStream.hpp"
#include "../Input/Binary/Internal/BinaryCorpusHelpers.hpp"
#include "../Accounting/BacktestResultAggregation.hpp"
#include "../Engine/BacktestEngineSetup.hpp"
#include "../Config/BacktestRunConfig.hpp"
#include "../Strategy/Diagnostics/BacktestStrategyTrace.hpp"
#include "../Strategy/Inputs/BacktestStrategyWarmup.hpp"
#include "../Strategy/Inputs/BacktestStrategyInstrument.hpp"
#include "../Strategy/Internal/BacktestStrategyRuntimeHelpers.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <set>

namespace hft_backtest::run_detail {
namespace {
struct UniverseState {
  alignas(trading_core::kStrategyRuntimeStateAlign)
      std::array<unsigned char, trading_core::kStrategyRuntimeStateBytes> state{};
  std::array<detail::EngineState*, trading_core::kStrategyRuntimeMaxLegs> routes{};
  std::vector<StrategyInstrumentSpec> instruments;
  std::unique_ptr<MultiLegCycleScratch> scratch;
  trading_core::SymbolShardConfig shard;
  trading_core::ShardControlState control;
  BacktestStrategyInputPolicy inputPolicy;
  BacktestRunRequest request;
  std::size_t count{};
  std::uint16_t requiredCaptureMask{};
};
const BacktestStrategyVenueConfig* configuredVenue(const BacktestStrategyConfig& config,
    const detail::SessionData& source) {
  for (const auto& venue : config.venues) {
    if (venue.exchange.raw != detail::exchangeFromText(source.exchange).raw ||
        venue.market.raw != detail::marketFromText(source.market).raw) continue;
    for (const auto& symbol : venue.symbols)
      if (source.symbol == symbol.data) return &venue;
  }
  return nullptr;
}
}
void runBinaryUniverse(BacktestRunResult& result, const BacktestRunRequest& request,
    const BacktestStrategyConfig& inputConfig, const trading_core::StrategyDescriptor& descriptor,
    BacktestProgressCallback progress, void* userData) noexcept {
  try {
    std::string error;
    const auto reject = [&](Status status, std::string message) {
      result.status = status; result.error = std::move(message);
    };
    detail::SessionLoadPlan plan;
    if (!buildSessionLoadPlan(descriptor, plan, error)) {
      reject(Status::Unsupported, error); return;
    }
    hftrec::corpus::BinaryMarketManifest manifest;
    std::vector<hftrec::corpus::BinaryMarketSource> catalog;
    auto catalogStatus = hftrec::corpus::BinaryMarketCorpusReader{}.catalog(
        request.corpusPath, manifest, catalog, error);
    if (!hftrec::isOk(catalogStatus)) {
      reject(detail::binary_corpus::convertStatus(catalogStatus), error); return;
    }
    auto config=inputConfig;
    if (request.corpusSources.empty()) {
      if (!materializeUniverseSymbolsFromCorpus(config,catalog,error)) {
        reject(Status::ConfigMismatch,error);return;
      }
      result.warnings.push_back("Whole recorded market: configured product symbols are expanded from the captured catalog in one proven quote asset.");
    }
    std::vector<BacktestCorpusSourceRequest> selections = request.corpusSources;
    if (selections.empty()) {
      const auto begin = request.corpusBeginReceiveNs > 0 ? request.corpusBeginReceiveNs : manifest.firstReceiveNs;
      const auto end = request.corpusEndReceiveNs > 0 ? request.corpusEndReceiveNs :
          (manifest.lastReceiveNs == std::numeric_limits<std::int64_t>::max() ? 0 : manifest.lastReceiveNs + 1);
      for (const auto& source : catalog) {
        detail::SessionData metadata;
        metadata.exchange = detail::binary_corpus::fixedText(source.venue.data(), source.venue.size(), source.venueBytes);
        metadata.market = detail::binary_corpus::fixedText(source.market.data(), source.market.size(), source.marketBytes);
        metadata.symbol = detail::binary_corpus::fixedText(source.canonicalSymbol.data(), source.canonicalSymbol.size(), source.canonicalSymbolBytes);
        if (!configuredVenue(config, metadata)) continue;
        selections.push_back({metadata.exchange, metadata.market, metadata.symbol, begin, end});
      }
    }
    if (selections.empty()) { reject(Status::ConfigMismatch, "folder has no config-eligible market sources"); return; }
    std::vector<detail::SessionData> catalogSources;
    for (const auto& selected : selections) {
      const auto match = std::find_if(catalog.begin(), catalog.end(), [&](const auto& source) {
        return detail::binary_corpus::fixedText(source.venue.data(), source.venue.size(), source.venueBytes) == selected.exchange &&
            detail::binary_corpus::fixedText(source.market.data(), source.market.size(), source.marketBytes) == selected.market &&
            detail::binary_corpus::fixedText(source.canonicalSymbol.data(), source.canonicalSymbol.size(), source.canonicalSymbolBytes) == selected.canonicalSymbol;
      });
      if (match == catalog.end()) { reject(Status::InvalidArgument, "selected source is outside corpus catalog"); return; }
      detail::SessionData source;
      source.exchange = selected.exchange; source.market = selected.market; source.symbol = selected.canonicalSymbol;
      source.capturedSourceId = match->sourceId;
      source.canonicalInstrumentId = match->canonicalSymbolId;
      source.assetDomainRaw = match->assetDomainRaw;
      source.quoteAssetId = match->quoteAssetId;
      catalogSources.push_back(std::move(source));
    }
    std::vector<UniverseContextSelection> selectedContexts;
    if (!discoverUniverseContexts(descriptor, config, catalogSources, selectedContexts, error)) {
      reject(Status::ConfigMismatch, error); return;
    }
    // Only complete context members become execution/account owners.
    std::vector<unsigned char> participating(selections.size());
    for (const auto& context : selectedContexts)
      for (auto source : context.sourceIndices) participating[source] = 1u;
    std::vector<std::size_t> remap(selections.size());
    std::vector<BacktestCorpusSourceRequest> participatingSelections;
    for (std::size_t i = 0; i < selections.size(); ++i) if (participating[i]) {
      remap[i] = participatingSelections.size();
      participatingSelections.push_back(std::move(selections[i]));
    }
    selections = std::move(participatingSelections);
    for (auto& context : selectedContexts)
      for (auto& source : context.sourceIndices) source = remap[source];
    catalogSources.clear();
    // Cursor owns one decoded/encoded block per shard. Refuse an oversized
    // cold universe before opening those owners or allocating replay state.
    const std::uint64_t cursorBytes =
        manifest.shardCount * hftrec::corpus::kBinaryMarketBlockRecords *
            sizeof(hftrec::corpus::BinaryMarketRecord) * 3u +
        catalog.size() * sizeof(hftrec::corpus::BinaryMarketSource) * 6u +
        manifest.indexCount * sizeof(hftrec::corpus::BinaryMarketIndexEntry) * 3u +
        cxet::market::kMaximumDepthTransactionMutations *
            (sizeof(detail::PriceLevelRow) + sizeof(hftrec::corpus::BinaryMarketRecord)) * 2u;
    std::uint64_t sourceBytes = sizeof(detail::EngineState) + (plan.loadDepths ? sizeof(detail::AggressiveFillPlan) : 0u) +
        sizeof(detail::PendingAction) * detail::kMaxOrderStates +
        sizeof(detail::SimulationOrderEvidenceOwner) +
        sizeof(detail::SharedAccountState) + sizeof(detail::SessionData) +
        sizeof(StrategyInstrumentSpec) + sizeof(detail::binary_corpus::BinaryDecodeState) +
        sizeof(hftrec::corpus::BinaryMarketSelection) + 1024u;
    {
      auto prototype = std::make_unique<detail::EngineState>();
      if (!prepareEngineOrderBookStorage(*prototype, &descriptor, plan)) {
        reject(Status::IoError, "failed to plan universe source storage"); return;
      }
      sourceBytes += prototype->orderBook.storageBytes() +
          prototype->venueOrderBook.storageBytes() + prototype->topExecutionOverlayScratch.storageBytes();
      if (plan.loadDepths) {
        const std::array<std::uint32_t, 1> capacities{cxet::market::kMaximumDepthLevelsPerSide};
        cxet::runtime::market::LocalOrderBookMemoryPlan coreBookPlan;
        if (!cxet::runtime::market::planLocalOrderBookMemory(capacities,
              std::numeric_limits<std::size_t>::max(), &coreBookPlan)) {
          reject(Status::Unsupported, "canonical depth memory plan rejected"); return;
        }
        sourceBytes += 2u * (coreBookPlan.requiredBytes +
            cxet::market::kMaximumDepthSnapshotMutations * sizeof(detail::ReplayDepthBook::IndexedMutation));
      }
    }
    constexpr std::uint64_t envelope = 3ull * 1024u * 1024u * 1024u;
    if (cursorBytes >= envelope || selections.size() > (envelope - cursorBytes) / sourceBytes) {
      reject(Status::Unsupported, "universe source owners exceed 3 GiB replay preparation envelope"); return;
    }
    detail::BinaryCorpusStream stream;
    const auto opened = stream.open(request.corpusPath, selections, plan, error, request.corpusSources.empty());
    if (!isOk(opened)) { reject(opened, error); return; }
    const auto& sources = stream.sessions();
    // Catalog metadata is an initial directory snapshot. Hydration can change
    // the quote asset before begin; validate the actual replay owners before
    // constructing account state or aggregating their integer balances.
    std::vector<UniverseContextSelection> hydratedContexts;
    if(!discoverUniverseContexts(descriptor,config,sources,hydratedContexts,error)) {
      reject(Status::ConfigMismatch,error);return;
    }
    selectedContexts=std::move(hydratedContexts);
    appendSessionWarnings(result, sources);
    const auto contextBytes = sizeof(UniverseState) + sizeof(MultiLegCycleScratch) +
        trading_core::kStrategyRuntimeMaxLegs * (sizeof(StrategyInstrumentSpec) + 2u * sizeof(std::size_t));
    if (sourceBytes * sources.size() + cursorBytes > envelope ||
        selectedContexts.size() > (envelope - cursorBytes - sourceBytes * sources.size()) / contextBytes) {
      reject(Status::Unsupported, "universe contexts exceed 3 GiB replay preparation envelope"); return;
    }
    std::vector<detail::EngineState> engines(sources.size());
    std::vector<StrategyInstrumentSpec> instruments(sources.size());
    std::vector<std::size_t> accountIndices(sources.size());
    std::vector<std::unique_ptr<detail::SharedAccountState>> accounts;
    std::vector<std::size_t> accountSources;
    std::vector<std::unique_ptr<UniverseState>> contexts;
    std::vector<std::vector<std::size_t>> subscribers(sources.size());
    std::int64_t initialBalance = 0;
    StrategyIndicatorProfileInfo indicator;
    if (!resolveStrategyIndicatorProfile(request.indicatorProfile, indicator)) {
      reject(Status::InvalidArgument, "unknown indicator profile"); return;
    }
    for (std::size_t i = 0; i < sources.size(); ++i) {
      const auto& source = sources[i];
      const auto* venue = configuredVenue(config, source);
      if (!venue) { reject(Status::ConfigMismatch, "selected source is outside configured strategy universe"); return; }
      auto& engine = engines[i];
      auto& instrument = instruments[i];
      if (source.assetDomainRaw != 1u ||
          !((source.capturedMarketKindRaw == 1u && source.capturedMarketRaw == canon::kMarketTypeSpot.raw) ||
            (source.capturedMarketKindRaw == 2u && (source.capturedMarketRaw == canon::kMarketTypeFutures.raw ||
             source.capturedMarketRaw == canon::kMarketTypeSwap.raw)))) {
        reject(Status::Unsupported, "market quantity replay supports Crypto Spot and proven linear Futures/Swaps only"); return;
      }
      const auto preparedInstrument = prepareBacktestStrategyInstrument(source.instrument, instrument, error);
      if (!isOk(preparedInstrument)) { reject(preparedInstrument, error); return; }
      engine.capturedSourceManaged = true;
      engine.capturedSourceMember = source.capturedInitiallyPresent;
      engine.capturedSourceGeneration = source.capturedSourceGeneration;
      engine.capturedDirectoryRevision = source.capturedDirectoryRevision;
      engine.capturedUnhealthyChannels = source.capturedUnhealthyChannels;
      engine.capturedActiveChannels = source.capturedInitiallyPresent ? source.capturedAvailableChannels : 0u;
      engine.exchange = detail::exchangeFromText(source.exchange);
      engine.market = detail::marketFromText(source.market);
      engine.symbol.copyFrom(source.symbol.c_str());
      engine.legIndex = static_cast<std::uint32_t>(i);
      engine.tickSizeE8 = instrument.tickSizeRaw;
      engine.makerFillMinCrossTicks = request.makerFillMinCrossTicks;
      if (!prepareEngineOrderBookStorage(engine, &descriptor, plan)) {
        reject(Status::IoError, "failed to prepare source depth owner"); return;
      }
      engine.venueOrderBookExecutionEnabled = plan.loadDepths;
      if (plan.loadDepths) engine.depthExecutionMode = BacktestDepthExecutionMode::Depth;
      (void)trading_core::configureBboReconstruction(engine.bboReconstruction, Price{instrument.tickSizeRaw});
      (void)trading_core::configureBboReconstruction(engine.venueBboReconstruction, Price{instrument.tickSizeRaw});
      configureStrategyIndicator(engine, indicator);
      detail::initLatencyRuntime(engine.latency, resolveLatencyConfig(request, source.exchange, source.market));
      auto zeroBalance = request;
      zeroBalance.initialBalanceE8 = 0;
      detail::initializeAccounting(engine, zeroBalance);
      const auto fee = resolveFeeSchedule(request, source.exchange, source.market);
      engine.makerFeeBpsE8 = fee.makerFeeBpsE8;
      engine.takerFeeBpsE8 = fee.takerFeeBpsE8;
      std::size_t account = accounts.size();
      for (std::size_t a = 0; a < accountSources.size(); ++a) {
        const auto owner = accountSources[a];
        if (engines[owner].exchange.raw == engine.exchange.raw &&
            engines[owner].market.raw == engine.market.raw) { account = a; break; }
      }
      const auto balance = virtualAccountInitialBalance(request, *venue, i);
      if (account == accounts.size()) {
        accounts.push_back(std::make_unique<detail::SharedAccountState>());
        accountSources.push_back(i);
        accounts.back()->quoteAssetId = source.quoteAssetId;
        accounts.back()->initialBalanceE8 = accounts.back()->walletBalanceE8 = balance;
        accounts.back()->observedWalletBalanceE8 = balance;
        accounts.back()->observedInitialized = true;
        engine.initialBalanceE8 = engine.walletBalanceE8 = balance;
        engine.observedWalletBalanceE8 = balance;
        initialBalance = trading_core::saturatingAddI64(initialBalance, balance);
        const auto policy = resolveRateLimitSchedule(request, source.exchange, source.market);
        if (!accounts.back()->quotas.configure(policy, error)) { reject(Status::InvalidArgument, error); return; }
        result.rateLimitSchedules.push_back(policy);
        result.rateLimitsEnabled=result.rateLimitsEnabled || policy.enabled;
        result.strictRateLimitRejects=request.strictRateLimitRejects && result.rateLimitsEnabled;

      } else if (!request.legInitialBalancesE8.empty() && accounts[account]->initialBalanceE8 != balance) {
        reject(Status::InvalidArgument, "sources on one virtual account disagree on initial balance"); return;
      }
      if (source.quoteAssetId == 0u || accounts[account]->quoteAssetId != source.quoteAssetId) {
        reject(Status::Unsupported, "virtual account requires one exact captured quote asset"); return;
      }
      accountIndices[i] = account;
      engine.sharedAccount = accounts[account].get();
    }
    if (accounts.size() > trading_core::kStrategyRuntimeMaxLegs) {
      reject(Status::Unsupported, "virtual account risk domain exceeds canonical account bound"); return;
    }
    for (std::size_t i = 0u; i < selectedContexts.size(); ++i) {
      const auto& selected = selectedContexts[i];
      auto context = std::make_unique<UniverseState>();
      context->count = selected.sourceIndices.size();
      context->scratch = std::make_unique<MultiLegCycleScratch>();
      context->scratch->reset(context->count);
      // Only cycle policy is copied; folder selections and schedules stay
      // with their single cold owner rather than growing once per context.
      context->request.marketOrderLatency = request.marketOrderLatency;
      context->request.limitOrderLatency = request.limitOrderLatency;
      context->request.cancelOrderLatency = request.cancelOrderLatency;
      context->request.userDataLatency = request.userDataLatency;
      context->request.orderLatencyUs = request.orderLatencyUs;
      context->request.cancelLatencyUs = request.cancelLatencyUs;
      context->request.rateLimitsEnabled = request.rateLimitsEnabled;
      context->request.strictRateLimitRejects = request.strictRateLimitRejects;
      context->request.captureStrategySpread = request.captureStrategySpread;
      context->request.tradeMode = selected.tradeMode;
      context->request.primaryLegIndex = request.hasPrimaryLegIndex ?
          request.primaryLegIndex : selected.primaryLegIndex;
      if (request.hasPrimaryLegIndex) context->request.tradeMode = request.tradeMode;
      if (context->request.primaryLegIndex >= context->count) {
        reject(Status::InvalidArgument, "primary leg index is outside one strategy context"); return;
      }
      context->request.hasPrimaryLegIndex = true;
      context->shard.shardId.raw = static_cast<std::uint32_t>(i + 1);
      context->shard.strategyId.raw = 1;
      context->shard.symbolId.raw = static_cast<std::uint32_t>(selected.sourceIndices[0] + 1);
      context->shard.symbol = engines[selected.sourceIndices[0]].symbol;
      context->control.shardId = context->shard.shardId;
      for (auto source : selected.sourceIndices) {
        context->routes[context->instruments.size()] = &engines[source];
        context->instruments.push_back(instruments[source]);
        subscribers[source].push_back(i);
      }
      trading_core::StrategyParamStorage params;
      const auto* venue = configuredVenue(config, sources[selected.sourceIndices[0]]);
      if (!descriptor.resolveParams(makeBacktestStrategyParamContext(config, *venue), params, error)) {
        reject(Status::InvalidArgument, error); return;
      }
      context->inputPolicy = resolveBacktestStrategyInputPolicy(descriptor, params);
      if (plan.requireTrades) context->requiredCaptureMask |= hftrec::corpus::binaryMarketChannelBit(hftrec::corpus::BinaryMarketChannel::Trade);
      if (plan.requireBookTickers) context->requiredCaptureMask |= hftrec::corpus::binaryMarketChannelBit(hftrec::corpus::BinaryMarketChannel::BookTicker);
      if (plan.requireDepths) context->requiredCaptureMask |= hftrec::corpus::binaryMarketChannelBit(hftrec::corpus::BinaryMarketChannel::Depth);
      for (auto source : selected.sourceIndices)
        engines[source].capturedExecutionRequiredChannels |= context->requiredCaptureMask;
      if (!descriptor.initState(context->state.data(), params, context->instruments[0]) ||
          !seedTradeWarmupFromSession(descriptor, context->state.data(), params, sources[selected.sourceIndices[0]], error)) {
        reject(Status::Unsupported, error.empty() ? "strategy has unsupported binary warmup" : error); return;
      }
      contexts.push_back(std::move(context));
    }
    std::array<trading_core::StrategyAccountState, trading_core::kStrategyRuntimeMaxLegs> riskAccounts;
    std::array<trading_core::PortfolioRiskLegConfig, trading_core::kStrategyRuntimeMaxLegs> riskConfigs;
    const auto refreshAccounts = [&] {
      for (auto& engine : engines) {
        detail::refreshAccountPositionValue(engine, false);
        detail::refreshAccountPositionValue(engine, true);
      }
      for (std::size_t a = 0; a < accounts.size(); ++a)
        riskAccounts[a] = accountStateFromEngine(engines[accountSources[a]]);
    };
    refreshAccounts();
    for (std::size_t a = 0; a < accounts.size(); ++a)
      riskConfigs[a] = makeBacktestPortfolioRiskLegConfig(configuredVenue(config, sources[accountSources[a]]));
    auto risk = trading_core::makePortfolioRiskState(makeBacktestPortfolioRiskConfig(config.risk),
        riskAccounts.data(), accounts.size(), riskConfigs.data());
    const auto selectContext = [&](std::size_t i) {
      auto& context = *contexts[i];
      for (std::size_t leg = 0; leg < context.count; ++leg)
        context.routes[leg]->activeContextIndex = static_cast<std::uint32_t>(i + 1);
      return detail::EngineView(context.routes.data(), context.count);
    };
    const auto contextActive = [&](std::size_t i) {
      const auto& context = *contexts[i];
      for (std::size_t leg = 0u; leg < context.count; ++leg) {
        const auto& source = *context.routes[leg];
        if (!source.capturedSourceMember ||
            (context.requiredCaptureMask & ~source.capturedActiveChannels) != 0u ||
            (context.requiredCaptureMask & source.capturedUnhealthyChannels) != 0u ||
            !backtestStrategySnapshotReady(context.inputPolicy, source.snapshot)) return false;
      }
      return true;
    };
    auto schedulerFloor = std::int64_t{0};
    const auto until = [&](std::int64_t ts, RunUntilBoundary boundary) {
      for (const auto& engine : engines) if (engine.simulationEvidenceFailed) {
        reject(Status::Unsupported, "canonical simulated order evidence capacity or identity refused"); return false;
      }
      for (std::uint32_t guard = 0u; guard < 1'000'000u; ++guard) {
        std::size_t selected = contexts.size();
        auto due = std::numeric_limits<std::int64_t>::max();
        std::size_t privateSource = engines.size();
        auto privateDue = std::numeric_limits<std::int64_t>::max();
        for (std::size_t source = 0u; source < engines.size(); ++source) {
          const auto& engine = engines[source];
          const auto head = engine.deliveredUserEvents;
          if (head >= engine.userEvents.size()) continue;
          if (head >= engine.userEventDeliveryTsNs.size() || head >= engine.userEventContextIndices.size()) {
            reject(Status::CorruptData, "simulated private source prefix metadata missing"); return false;
          }
          const auto eligible = std::max({engine.userEventDeliveryTsNs[head], engine.replayNotBeforeNs, schedulerFloor});
          if (eligible < privateDue || (eligible == privateDue &&
              (privateSource == engines.size() || engine.userEventDeliveryTsNs[head] <
               engines[privateSource].userEventDeliveryTsNs[engines[privateSource].deliveredUserEvents]))) {
            privateDue = eligible; privateSource = source;
          }
        }
        for (std::size_t i = 0; i < contexts.size(); ++i) {
          if (!contextActive(i)) continue;
          const auto engines = selectContext(i);
          const auto& context = *contexts[i];
          const auto next = std::min({
              strategy_runtime_detail::nextWakeTs(descriptor, context.state.data()),
              strategy_runtime_detail::nextPendingTs(engines),
              strategy_runtime_detail::nextUserEventDeliveryTs(engines),
              strategy_runtime_detail::nextRateLimitCooldownTs(engines)});
          auto floor = schedulerFloor;
          for (const auto& source : engines) floor = std::max(floor, source.replayNotBeforeNs);
          const auto eligible = std::max(next, floor);
          if (eligible < due) { due = eligible; selected = i; }
        }
        if (privateSource < engines.size() && privateDue <= due &&
            privateDue <= ts && !(boundary == RunUntilBoundary::Exclusive && privateDue == ts)) {
          auto& engine = engines[privateSource];
          const auto head = engine.deliveredUserEvents;
          const auto ownerIndex = engine.userEventContextIndices[head];
          if (!ownerIndex || ownerIndex > contexts.size()) {
            reject(Status::CorruptData, "simulated private event has no admitted context owner"); return false;
          }
          const auto event = engine.userEvents[head];
          const auto contextIndex = static_cast<std::size_t>(ownerIndex - 1u);
          const auto& indices = selectedContexts[contextIndex].sourceIndices;
          const auto found = std::find(indices.begin(), indices.end(), privateSource);
          if (found == indices.end()) { reject(Status::CorruptData, "private event source is foreign to context"); return false; }
          schedulerFloor = privateDue;
          engine.replayNotBeforeNs = std::max(engine.replayNotBeforeNs, privateDue);
          engine.activeContextIndex = ownerIndex;
          if (!detail::deliverSimulationOrderEvent(engine, head, privateDue)) {
            reject(Status::CorruptData, "simulated private event correlation or accounting refused"); return false;
          }
          if (!engine.userEventServiceFlags[head] && contextActive(contextIndex)) {
            auto& context = *contexts[contextIndex];
            refreshAccounts();
            if (!runCycleMulti(descriptor, context.inputPolicy, context.state.data(),
                  selectContext(contextIndex), context.instruments, *context.scratch, context.shard,
                  context.control, static_cast<std::uint32_t>(found - indices.begin()), privateDue,
                  context.request, &event)) {
              const auto evidenceFailed = std::any_of(engines.begin(), engines.end(),
                  [](const auto& source) { return source.simulationEvidenceFailed; });
              reject(evidenceFailed ? Status::Unsupported : Status::InvalidArgument,
                  evidenceFailed ? "canonical simulated order evidence capacity or identity refused" :
                      "canonical strategy rejected private delivery context"); return false;
            }
          }
          continue;
        }
        if (selected == contexts.size() || due > ts ||
            (boundary == RunUntilBoundary::Exclusive && due == ts)) return true;
        auto& context = *contexts[selected];
        schedulerFloor = due;
        for (auto* source : context.routes) if (source)
          source->replayNotBeforeNs = std::max(source->replayNotBeforeNs, due);
        refreshAccounts();
        if (!runUntilMulti(descriptor, context.inputPolicy, context.state.data(),
              selectContext(selected), context.instruments, *context.scratch, context.shard,
              context.control, due, context.request, RunUntilBoundary::Inclusive)) {
          for (const auto& engine : engines) if (engine.simulationEvidenceFailed) {
            reject(Status::Unsupported, "canonical simulated order evidence capacity or identity refused"); break;
          }
          return false;
        }
      }
      return false;
    };
    result.status = Status::Ok;
    std::vector<detail::TimelineEvent> transaction;
    bool available = false;
    std::int64_t now = 0;
    result.universeSources = sources.size();
    result.universeContexts = contexts.size();
    result.virtualAccounts = accounts.size();
    result.replayClock.venueSchedule = "captured_arrival_no_future_lookahead";
    result.initialBalanceE8 = result.walletBalanceE8 = initialBalance;
    result.events = result.trades = result.bookTickers = result.depths = 0;
    result.firstTsNs = result.lastTsNs = 0;
    while (true) {
      const auto read = stream.next(transaction, available, error);
      if (!isOk(read)) { reject(read, error); break; }
      if (!available) break;
      const auto first = transaction.front();
      if (first.deliveryTsNs < now || !until(first.deliveryTsNs, RunUntilBoundary::Exclusive)) {
        if (isOk(result.status)) reject(Status::CorruptData, "universe delivery or strategy timer order rejected");
        break;
      }
      now = first.deliveryTsNs;
      schedulerFloor = now;
      for (auto& engine : engines) applyCapturedDeliveryClock(engine, first);
      for (const auto& event : transaction) {
        auto& engine = engines[event.legIndex];
        if (event.kind == detail::kTimelineKindHealth) {
          if (!applyCapturedSourceHealth(engine, sources[event.legIndex].sourceHealth[event.index], now, error)) {
            reject(Status::CorruptData, error); break;
          }
        } else if (event.kind == detail::kTimelineKindLifecycle) {
          const auto& row = sources[event.legIndex].sourceLifecycle[event.index];
          if (sources[event.legIndex].assetDomainRaw != 1u ||
              !((sources[event.legIndex].capturedMarketKindRaw == 1u && sources[event.legIndex].capturedMarketRaw == canon::kMarketTypeSpot.raw) ||
                (sources[event.legIndex].capturedMarketKindRaw == 2u && (sources[event.legIndex].capturedMarketRaw == canon::kMarketTypeFutures.raw ||
                 sources[event.legIndex].capturedMarketRaw == canon::kMarketTypeSwap.raw)))) {
            reject(Status::Unsupported, "market quantity replay supports Crypto Spot and proven linear Futures/Swaps only"); break;
          }
          auto& instrument = instruments[event.legIndex];
          trading_core::StrategyInstrumentSpec candidate;
          const auto preparedInstrument = prepareBacktestStrategyInstrument(
              sources[event.legIndex].instrument, candidate, error);
          if (!isOk(preparedInstrument)) { reject(preparedInstrument, error); break; }
          if (candidate.tickSizeRaw != instrument.tickSizeRaw ||
              candidate.stepSizeRaw != instrument.stepSizeRaw ||
              candidate.executionQuantityStepRaw != instrument.executionQuantityStepRaw ||
              candidate.contractBaseQtyRaw != instrument.contractBaseQtyRaw ||
              candidate.priceBasisQtyRaw != instrument.priceBasisQtyRaw ||
              candidate.expiryUtcNs != instrument.expiryUtcNs) {
            reject(Status::Unsupported, "strategy has no canonical instrument rebind for changed captured metadata"); break;
          }
          if (!applyCapturedSourceLifecycle(engine, row, now, error)) {
            reject(Status::CorruptData, error); break;
          }
          instrument = candidate;
          engine.tickSizeE8 = instrument.tickSizeRaw;
          engine.bboReconstruction.tickSize = Price{instrument.tickSizeRaw};
          engine.venueBboReconstruction.tickSize = Price{instrument.tickSizeRaw};
          for (auto contextIndex : subscribers[event.legIndex]) {
            auto& context = *contexts[contextIndex];
            const auto& indices = selectedContexts[contextIndex].sourceIndices;
            const auto local = std::find(indices.begin(), indices.end(), event.legIndex) - indices.begin();
            context.instruments[local] = instrument;
            context.scratch->revisions[local] = 0u;
          }
        } else {
        if (!event.applicationOnly)
          applyMarketEventToSnapshots(engine, sources[event.legIndex], event,
              engine.symbol, instruments[event.legIndex], false);
        applyMarketEventToSnapshots(engine, sources[event.legIndex], event,
            engine.symbol, instruments[event.legIndex], true);
        }
        includeTimelineClock(result.replayClock, event);
        includeTimelineClock(engine, event);
        if (event.venueScheduleAdjusted && !event.applicationOnly) {
          ++result.replayClock.venueScheduleAdjustedEvents;
          ++engine.venueScheduleAdjustedEvents;
        }
        ++result.events;
        if (event.kind == detail::kTimelineKindTrade) ++result.trades;
        else if (event.kind == detail::kTimelineKindBookTicker) ++result.bookTickers;
        else if (event.kind == detail::kTimelineKindDepth) ++result.depths;
        if (event.exchangeTsNs > 0 && (result.firstTsNs == 0 || event.exchangeTsNs < result.firstTsNs))
          result.firstTsNs = event.exchangeTsNs;
        result.lastTsNs = std::max(result.lastTsNs, event.exchangeTsNs);
        if (engine.depthReplayFailed) { reject(Status::CorruptData, "captured depth transaction rejected"); break; }
        if (engine.simulationEvidenceFailed) {
          reject(Status::Unsupported, "canonical simulated order evidence capacity or identity refused"); break;
        }
      }
      if (!isOk(result.status)) break;
      if (!transaction.back().applicationOnly) {
        for (auto i : subscribers[first.legIndex]) {
          if (!contextActive(i)) continue;
          auto& context = *contexts[i];
          const auto& indices = selectedContexts[i].sourceIndices;
          const auto leg = std::find(indices.begin(), indices.end(), first.legIndex) - indices.begin();
          refreshAccounts();
          if (!runCycleMulti(descriptor, context.inputPolicy, context.state.data(),
                selectContext(i), context.instruments, *context.scratch, context.shard,
                context.control, static_cast<std::uint32_t>(leg), now, context.request, nullptr)) {
            const auto evidenceFailed = std::any_of(engines.begin(), engines.end(),
                [](const auto& engine) { return engine.simulationEvidenceFailed; });
            reject(evidenceFailed ? Status::Unsupported : Status::InvalidArgument,
                evidenceFailed ? "canonical simulated order evidence capacity or identity refused" :
                    "canonical strategy rejected universe context"); break;
          }
        }
      }
      if (!isOk(result.status) || !until(now, RunUntilBoundary::Inclusive)) {
        if (isOk(result.status)) reject(Status::InvalidArgument, "universe user event loop did not settle");
        break;
      }
      refreshAccounts();
      const auto evaluation = trading_core::evaluatePortfolioRisk(risk,
          riskAccounts.data(), accounts.size(), timeNsFromI64(now));
      if (evaluation.breached) {
        for (auto& engine : engines) {
          engine.riskStopped = true;
          engine.riskStopTsNs = now;
          engine.riskStopEquityE8 = evaluation.equityE8;
          engine.riskStopThresholdEquityE8 = evaluation.thresholdEquityE8;
        }
        result.riskStopped = true;
        result.riskStopTsNs = now;
        result.riskStopEquityE8 = evaluation.equityE8;
        result.riskStopThresholdEquityE8 = evaluation.thresholdEquityE8;
        break;
      }
      if (request.strictRateLimitRejects &&
          rateLimitRejectedCommands(engines[0], &engines) != 0u) {
        reject(Status::InvalidArgument, "strict shared-account quota rejection"); break;
      }
      std::uint64_t outputBytes = 0u;
      for (auto& engine : engines) {
        (void)detail::appendRiskSnapshotIfDue(engine, now);
        outputBytes += engine.orders.capacity() * sizeof(detail::JsonOrder) +
            engine.fills.capacity() * sizeof(detail::JsonFill) +
            engine.equityPoints.capacity() * sizeof(detail::JsonEquityPoint) +
            engine.riskSnapshots.capacity() * sizeof(detail::JsonRiskSnapshot) +
            engine.userEvents.capacity() * sizeof(cxet::composite::UserDataEvent) +
            engine.userEventDeliveryTsNs.capacity() * sizeof(std::int64_t) +
            engine.userEventClientIntentIds.capacity() * sizeof(Id) +
            engine.userEventCommandIds.capacity() * sizeof(std::uint64_t) +
            engine.userEventGenerations.capacity() * sizeof(std::uint64_t) +
            engine.userEventContextIndices.capacity() * sizeof(std::uint32_t) +
            engine.userEventConsumed.capacity() +
            engine.userEventServiceFlags.capacity() +
            engine.strategyIndicator.capacity() * sizeof(detail::JsonStrategyIndicatorPoint) +
            engine.strategyRange.capacity() * sizeof(detail::JsonStrategyRangePoint) +
            engine.strategySpread.capacity() * sizeof(detail::JsonStrategySpreadPoint) +
            engine.executionQuality.capacity() * sizeof(detail::JsonExecutionQualityAction) +
            engine.rateLimitUsage.capacity() * sizeof(detail::JsonRateLimitUsagePoint);
      }
      const auto reserved = cursorBytes + sourceBytes * sources.size() +
          contextBytes * contexts.size();
      // Aggregation and result projection duplicate diagnostic rows at finalization.
      if (outputBytes > (envelope - reserved) / 4u) {
        reject(Status::Unsupported, "bounded universe diagnostic output capacity exhausted"); break;
      }
      if (result.events % 1024u == 0 && !reportProgress(result.events, 0, "running", progress, userData)) {
        reject(Status::Cancelled, "cancelled"); break;
      }
    }
    // Capture exhaustion and risk fencing never manufacture cancellation/fills.
    for (const auto& source : sources) mergeReplayClockEvidence(result.replayClock, source);
    for (auto& engine : engines) {
      finalizeExecutionQuality(engine);
      detail::appendEquityPoint(engine, now);
      detail::appendRiskSnapshot(engine, now, detail::kRiskSnapshotReasonFinal);
    }
    populatePortfolioResultPnl(result, sources, engines, initialBalance);
    auto aggregate = std::make_unique<detail::EngineState>();
    populateAggregateArtifactEngine(engines, initialBalance, now, *aggregate);
    appendRateLimitWarnings(result, *aggregate, &engines);
    appendDepthExecutionWarnings(result, *aggregate, &engines);
    if (request.writeArtifacts) {
      const auto written = detail::writeResult(result, *aggregate, &engines);
      if (!isOk(written)) result.status = written;
    }
    (void)reportProgress(result.events, result.events, "complete", progress, userData);
  } catch (...) {
    result.status = Status::IoError;
    result.error = "universe replay owner allocation failed";
  }
}
}
