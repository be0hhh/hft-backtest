#include "BacktestStrategyIntentLowering.hpp"

#include "trading_core/Strategy/Context/StrategyContextView.hpp"
#include "trading_core/Strategy/StrategyTransactionValidation.hpp"

#include <array>

namespace hft_backtest::strategy_intent_lowering {
namespace {

[[nodiscard]] bool orderIntentTradable(
    const BacktestRunRequest& request,
    const trading_core::StrategyContext& context,
    const trading_core::OrderIntent& intent,
    std::uint64_t tradableLegMask) noexcept {
  const std::size_t leg = routeLegIndex(context, intent.routeHandle);
  return routeTradable(request, context.marketData.count, leg) && leg < 64u &&
      (tradableLegMask & (std::uint64_t{1u} << leg)) != 0u;
}

[[nodiscard]] bool cancelIntentRouteExists(
    const trading_core::StrategyContext& context,
    const trading_core::CancelIntent& intent) noexcept {
  const std::size_t leg = routeLegIndex(context, intent.routeHandle);
  return leg < context.marketData.count;
}

[[nodiscard]] bool commandIntentTradable(
    const BacktestRunRequest& request,
    const trading_core::StrategyContext& context,
    const trading_core::StrategyCommandIntent& command,
    std::uint64_t tradableLegMask) noexcept {
  if (command.kind == trading_core::StrategyCommandIntentKind::Cancel) {
    return cancelIntentRouteExists(context, command.cancel);
  }
  if (command.kind == trading_core::StrategyCommandIntentKind::Order) {
    return orderIntentTradable(
        request, context, command.order, tradableLegMask);
  }
  if (command.kind ==
      trading_core::StrategyCommandIntentKind::WarmupOrder) {
    return orderIntentTradable(
        request, context, command.warmup.order, tradableLegMask);
  }
  return true;
}

[[nodiscard]] bool strategyIoCountsValid(
    const trading_core::StrategyIo& io) noexcept {
  return !io.capacityExceeded && io.orderCount <= io.orderOut.size() &&
      io.cancelCount <= io.cancelOut.size() &&
      io.commandCount <= io.commandOut.size() &&
      io.transactionCount <= io.transactionOut.size();
}

struct TransactionOrderSimulation final {
  bool attempted{false};
  bool accepted{false};
};

[[nodiscard]] TransactionOrderSimulation transactionOrderSimulation(
    Span<const detail::EngineState> engines,
    const std::array<std::size_t,
                     trading_core::kStrategyRuntimeMaxLegs>& firstRecords,
    std::size_t leg,
    Id clientIntentId) noexcept {
  if (leg >= engines.size() || clientIntentId.raw == 0u) return {};
  const std::vector<detail::JsonOrder>& orders = engines[leg].orders;
  const std::size_t first = firstRecords[leg] < orders.size()
      ? firstRecords[leg]
      : orders.size();
  for (std::size_t index = first; index < orders.size(); ++index) {
    const detail::JsonOrder& order = orders[index];
    if (order.id != clientIntentId.raw ||
        order.action != detail::kRecordActionOrder) {
      continue;
    }
    return TransactionOrderSimulation{
        true,
        order.status != detail::kRecordStatusRejected &&
            order.status != detail::kRecordStatusMissing};
  }
  return {};
}

}  // namespace

std::uint32_t configuredPrimaryLegIndex(
    const BacktestRunRequest& request,
    std::size_t legCount) noexcept {
  if (legCount == 0u || !request.hasPrimaryLegIndex) return 0u;
  return request.primaryLegIndex < legCount ? request.primaryLegIndex : 0u;
}

trading_core::ExecutionRouteHandle configuredDefaultExecutionRoute(
    const BacktestRunRequest& request,
    std::size_t legCount) noexcept {
  if (legCount == 0u) return trading_core::kDefaultExecutionRouteHandle;
  const std::uint32_t defaultLeg =
      request.tradeMode == BacktestTradeMode::PrimaryOnly
          ? configuredPrimaryLegIndex(request, legCount)
          : 0u;
  return trading_core::strategy::route(defaultLeg);
}

std::size_t routeLegIndex(
    const trading_core::StrategyContext& context,
    trading_core::ExecutionRouteHandle route) noexcept {
  return trading_core::strategy::executionRouteLegIndex(context, route);
}

bool routeTradable(const BacktestRunRequest& request,
                   std::size_t legCount,
                   std::size_t leg) noexcept {
  if (leg >= legCount) return false;
  if (request.tradeMode == BacktestTradeMode::PrimaryOnly) {
    return leg == static_cast<std::size_t>(
                      configuredPrimaryLegIndex(request, legCount));
  }
  return true;
}

bool footprintSupported(
    const trading_core::StrategyDescriptor& descriptor) noexcept {
  const trading_core::StrategyRuntimeFootprint& footprint =
      descriptor.footprint;
  return trading_core::strategyDescriptorContractValid(descriptor) &&
      footprint.maxOrders <= detail::kIntentCapacity &&
      footprint.maxCancels <= detail::kIntentCapacity &&
      footprint.maxDesiredOrders == 0u &&
      trading_core::strategyRuntimeCommandCapacity(footprint) <=
          trading_core::kStrategyRuntimeMaxCommands +
              2u * trading_core::kStrategyRuntimeMaxDesiredOrders &&
      footprint.maxTransactions <=
          trading_core::kStrategyRuntimeMaxTransactions;
}

bool validateOutput(trading_core::StrategyContext& context) noexcept {
  if (!strategyIoCountsValid(context.io) ||
      !trading_core::strategyIoCommandSequencesValid(context.io)) return false;
  // This simulator has no paired physical-send model. Reject the entire
  // cycle before applying any leg rather than silently dropping Replace.
  for (std::size_t index = 0u; index < context.io.commandCount; ++index)
    if (context.io.commandOut[index].kind == trading_core::StrategyCommandIntentKind::Replace)
      return false;
  return true;
}

bool bindTransactionLegs(trading_core::StrategyContext& context) noexcept {
  trading_core::StrategyIo& io = context.io;
  if (!strategyIoCountsValid(io) ||
      !trading_core::validStrategyTransactionalIoShape(io) ||
      !trading_core::validStrategyTransactionBindings(context)) {
    return false;
  }

  const auto bindOrder = [&context, &io](
                             const trading_core::OrderIntent& order) noexcept {
    if (order.transactionId.raw == 0u) return;
    const std::size_t orderLeg = routeLegIndex(context, order.routeHandle);
    if (orderLeg == trading_core::strategy::kInvalidExecutionRouteLegIndex) {
      return;
    }
    for (std::size_t transactionIndex = 0u;
         transactionIndex < io.transactionCount;
         ++transactionIndex) {
      trading_core::StrategyTransactionIntent& transaction =
          io.transactionOut[transactionIndex];
      if (transaction.transactionId.raw != order.transactionId.raw) continue;
      const std::size_t legCount =
          static_cast<std::size_t>(transaction.legCount) <
                  trading_core::kStrategyTransactionMaxLegs
              ? static_cast<std::size_t>(transaction.legCount)
              : trading_core::kStrategyTransactionMaxLegs;
      for (std::size_t legIndex = 0u; legIndex < legCount; ++legIndex) {
        trading_core::StrategyTransactionLegDelta& leg =
            transaction.legs[legIndex];
        if (routeLegIndex(context, leg.routeHandle) != orderLeg) continue;
        leg.clientIntentId = order.clientIntentId;
        leg.expectedFillQty = order.qty;
        return;
      }
    }
  };

  for (std::size_t index = 0u; index < io.orderCount; ++index) {
    bindOrder(io.orderOut[index]);
  }
  for (std::size_t index = 0u; index < io.commandCount; ++index) {
    const trading_core::StrategyCommandIntent& command = io.commandOut[index];
    if (command.kind == trading_core::StrategyCommandIntentKind::Order) {
      bindOrder(command.order);
    } else if (command.kind ==
               trading_core::StrategyCommandIntentKind::WarmupOrder) {
      bindOrder(command.warmup.order);
    }
  }
  return trading_core::validStrategyTransactionBindings(context);
}

StrategyTransactionAdmission transactionAdmission(
    const trading_core::StrategyContext& context,
    std::size_t legCount,
    const BacktestRunRequest& request,
    bool newOrderEmissionBlocked) noexcept {
  const trading_core::StrategyIo& io = context.io;
  StrategyTransactionAdmission admission{};
  if (io.transactionCount > io.transactionOut.size() ||
      io.transactionCount > trading_core::kStrategyRuntimeMaxTransactions ||
      io.orderCount > io.orderOut.size()) {
    for (std::size_t transactionIndex = 0u;
         transactionIndex < io.transactionCount && transactionIndex < 64u;
         ++transactionIndex) {
      admission.localFaultMask |=
          std::uint64_t{1u} << transactionIndex;
    }
    return admission;
  }
  for (std::size_t orderIndex = 0u;
       orderIndex < io.orderCount; ++orderIndex) {
    const trading_core::OrderIntent& order = io.orderOut[orderIndex];
    std::size_t transactionIndex = io.transactionCount;
    for (std::size_t candidate = 0u;
         candidate < io.transactionCount; ++candidate) {
      if (io.transactionOut[candidate].transactionId.raw ==
          order.transactionId.raw) {
        transactionIndex = candidate;
        break;
      }
    }
    if (transactionIndex == io.transactionCount) {
      for (std::size_t candidate = 0u;
           candidate < io.transactionCount; ++candidate) {
        admission.localFaultMask |= std::uint64_t{1u} << candidate;
      }
      return admission;
    }
    const std::size_t leg =
        routeLegIndex(context, order.routeHandle);
    // A transaction cannot partially submit its supported legs while another
    // leg depends on a retired mark/index feed. Existing local-fault admission
    // rejects the complete transaction before any simulated order operation.
    const bool unavailableTrigger =
        order.orderType == canon::OrderType::Stop &&
        order.triggerSource != canon::OrderTriggerSource::Last;
    if (newOrderEmissionBlocked || unavailableTrigger ||
        !routeTradable(request, legCount, leg) || leg >= 64u ||
        (context.tradableLegMask & (std::uint64_t{1u} << leg)) == 0u) {
      admission.localFaultMask |=
          std::uint64_t{1u} << transactionIndex;
    }
  }

  std::uint64_t failedTouchedLegMask = 0u;
  for (std::size_t transactionIndex = 0u;
       transactionIndex < io.transactionCount; ++transactionIndex) {
    if ((admission.localFaultMask &
         (std::uint64_t{1u} << transactionIndex)) != 0u) {
      failedTouchedLegMask |=
          io.transactionOut[transactionIndex].touchedLegMask;
    }
  }
  for (std::size_t transactionIndex = 0u;
       transactionIndex < io.transactionCount; ++transactionIndex) {
    const std::uint64_t transactionBit =
        std::uint64_t{1u} << transactionIndex;
    if ((admission.localFaultMask & transactionBit) == 0u &&
        (io.transactionOut[transactionIndex].touchedLegMask &
         failedTouchedLegMask) != 0u) {
      admission.deferredMask |= transactionBit;
    }
  }
  return admission;
}

void applyToSingleLeg(detail::EngineState& engine,
                      const trading_core::StrategyContext& context,
                      std::int64_t tsNs,
                      detail::BacktestLatencyRuntime& latency,
                      const BacktestRunRequest& request) {
  const trading_core::StrategyIo& io = context.io;
  std::array<trading_core::OrderIntent, detail::kIntentCapacity> orders{};
  std::array<trading_core::CancelIntent, detail::kIntentCapacity> cancels{};
  std::array<trading_core::StrategyCommandIntent,
             trading_core::kStrategyRuntimeMaxCommands +
                 2u * trading_core::kStrategyRuntimeMaxDesiredOrders>
      commands{};
  trading_core::StrategyIo filtered{};
  filtered.orderOut =
      Span<trading_core::OrderIntent>(orders.data(), orders.size());
  filtered.cancelOut =
      Span<trading_core::CancelIntent>(cancels.data(), cancels.size());
  filtered.commandOut = Span<trading_core::StrategyCommandIntent>(
      commands.data(), commands.size());
  filtered.requestRateLimitSnapshot = io.requestRateLimitSnapshot;

  for (std::size_t index = 0u;
       index < io.commandCount &&
       filtered.commandCount < filtered.commandOut.size();
       ++index) {
    if (!commandIntentTradable(
            request, context, io.commandOut[index],
            context.tradableLegMask)) {
      continue;
    }
    filtered.commandOut[filtered.commandCount++] = io.commandOut[index];
  }
  for (std::size_t index = 0u;
       index < io.cancelCount &&
       filtered.cancelCount < filtered.cancelOut.size();
       ++index) {
    if (!cancelIntentRouteExists(context, io.cancelOut[index])) continue;
    filtered.cancelOut[filtered.cancelCount++] = io.cancelOut[index];
  }
  for (std::size_t index = 0u;
       index < io.orderCount &&
       filtered.orderCount < filtered.orderOut.size();
       ++index) {
    if (!orderIntentTradable(
            request, context, io.orderOut[index],
            context.tradableLegMask)) {
      continue;
    }
    filtered.orderOut[filtered.orderCount++] = io.orderOut[index];
  }
  detail::applyIntents(engine, filtered, tsNs, latency);
}

void applyToLegs(Span<detail::EngineState> engines,
                 const trading_core::StrategyContext& context,
                 std::int64_t tsNs,
                 const BacktestRunRequest& request) {
  const trading_core::StrategyIo& io = context.io;
  for (std::size_t index = 0u; index < io.commandCount; ++index) {
    const trading_core::StrategyCommandIntent& command = io.commandOut[index];
    if (command.kind == trading_core::StrategyCommandIntentKind::Cancel) {
      const std::size_t leg =
          routeLegIndex(context, command.cancel.routeHandle);
      if (leg >= engines.size()) continue;
      std::array<trading_core::CancelIntent, 1u> cancel{command.cancel};
      trading_core::StrategyIo one{};
      one.cancelOut =
          Span<trading_core::CancelIntent>(cancel.data(), cancel.size());
      one.cancelCount = 1u;
      detail::applyIntents(engines[leg], one, tsNs, engines[leg].latency);
    } else if (command.kind ==
               trading_core::StrategyCommandIntentKind::Order) {
      const std::size_t leg =
          routeLegIndex(context, command.order.routeHandle);
      if (!routeTradable(request, engines.size(), leg) || leg >= 64u ||
          (context.tradableLegMask & (std::uint64_t{1u} << leg)) == 0u) {
        continue;
      }
      std::array<trading_core::OrderIntent, 1u> order{command.order};
      trading_core::StrategyIo one{};
      one.orderOut =
          Span<trading_core::OrderIntent>(order.data(), order.size());
      one.orderCount = 1u;
      detail::applyIntents(engines[leg], one, tsNs, engines[leg].latency);
    } else if (command.kind ==
               trading_core::StrategyCommandIntentKind::WarmupOrder) {
      const std::size_t leg =
          routeLegIndex(context, command.warmup.order.routeHandle);
      if (!routeTradable(request, engines.size(), leg) || leg >= 64u ||
          (context.tradableLegMask & (std::uint64_t{1u} << leg)) == 0u) {
        continue;
      }
      std::array<trading_core::StrategyCommandIntent, 1u> warmup{command};
      trading_core::StrategyIo one{};
      one.commandOut = Span<trading_core::StrategyCommandIntent>(
          warmup.data(), warmup.size());
      one.commandCount = 1u;
      detail::applyIntents(engines[leg], one, tsNs, engines[leg].latency);
    }
  }
  for (std::size_t index = 0u; index < io.cancelCount; ++index) {
    const std::size_t leg =
        routeLegIndex(context, io.cancelOut[index].routeHandle);
    if (leg >= engines.size()) continue;
    trading_core::StrategyIo one{};
    one.cancelOut = Span<trading_core::CancelIntent>(
        const_cast<trading_core::CancelIntent*>(&io.cancelOut[index]), 1u);
    one.cancelCount = 1u;
    detail::applyIntents(engines[leg], one, tsNs, engines[leg].latency);
  }
  for (std::size_t index = 0u; index < io.orderCount; ++index) {
    const std::size_t leg =
        routeLegIndex(context, io.orderOut[index].routeHandle);
    if (!routeTradable(request, engines.size(), leg) || leg >= 64u ||
        (context.tradableLegMask & (std::uint64_t{1u} << leg)) == 0u) {
      continue;
    }
    trading_core::StrategyIo one{};
    one.orderOut = Span<trading_core::OrderIntent>(
        const_cast<trading_core::OrderIntent*>(&io.orderOut[index]), 1u);
    one.orderCount = 1u;
    detail::applyIntents(engines[leg], one, tsNs, engines[leg].latency);
  }
}

bool applyTransactionCycle(
    const trading_core::StrategyDescriptor& descriptor,
    void* state,
    const trading_core::StrategyContext& context,
    Span<detail::EngineState> engines,
    std::int64_t tsNs,
    StrategyTransactionAdmission admission) {
  const trading_core::StrategyIo& io = context.io;
  if (io.transactionCount == 0u) return true;
  if (!descriptor.applyEmitReport || !descriptor.replayTransaction ||
      !descriptor.allocateClientId) {
    return false;
  }
  if (io.transactionCount > io.transactionOut.size() ||
      io.transactionCount > trading_core::kStrategyRuntimeMaxTransactions ||
      io.orderCount > io.orderOut.size() ||
      io.orderCount > trading_core::kStrategyRuntimeMaxOrders ||
      !trading_core::validStrategyTransactionalIoShape(io) ||
      !trading_core::validStrategyTransactionBindings(context)) {
    return false;
  }
  std::array<trading_core::StrategyEmitReport,
             trading_core::kStrategyRuntimeMaxTransactions> reports{};
  std::array<bool,
             trading_core::kStrategyRuntimeMaxTransactions> reportReady{};
  std::uint64_t acceptedOrderMask = 0u;
  bool transactionEmitFailed = false;
  bool recoveryCompletionRequiresFailStop = false;
  std::uint64_t failedTouchedLegMask = 0u;
  for (std::size_t transactionIndex = 0u;
       transactionIndex < io.transactionCount; ++transactionIndex) {
    if ((admission.localFaultMask &
         (std::uint64_t{1u} << transactionIndex)) != 0u) {
      failedTouchedLegMask |=
          io.transactionOut[transactionIndex].touchedLegMask;
    }
  }
  for (std::size_t transactionIndex = 0u;
       transactionIndex < io.transactionCount;
       ++transactionIndex) {
    const trading_core::StrategyTransactionIntent& transaction =
        io.transactionOut[transactionIndex];
    if (transaction.recordKind !=
        trading_core::StrategyTransactionRecordKind::Proposed) {
      // Backtest has no authoritative recovery owner. A strategy-produced
      // Confirmed record is a clean completion, but Rejected/Uncertain means
      // the strategy has entered recovery quarantine after asynchronous
      // simulator evidence. Consume the whole output cycle, then fail-stop so
      // replay cannot continue under hidden permanent quarantine.
      recoveryCompletionRequiresFailStop =
          recoveryCompletionRequiresFailStop ||
          transaction.recordKind ==
              trading_core::StrategyTransactionRecordKind::Rejected ||
          transaction.recordKind ==
              trading_core::StrategyTransactionRecordKind::Uncertain;
      continue;
    }
    if (transaction.transactionId.raw == 0u ||
        transaction.legCount > trading_core::kStrategyTransactionMaxLegs) {
      return false;
    }

    trading_core::StrategyEmitReport& report =
        reports[transactionIndex];
    reportReady[transactionIndex] = true;
    report.transactionId = transaction.transactionId;
    report.semantic = transaction.semantic;
    report.allAccepted = true;
    const std::uint64_t transactionBit =
        std::uint64_t{1u} << transactionIndex;
    const bool localFaultBlocked =
        (admission.localFaultMask & transactionBit) != 0u;
    const bool deferredBlocked = !localFaultBlocked &&
        (((admission.deferredMask & transactionBit) != 0u) ||
         (transaction.touchedLegMask & failedTouchedLegMask) != 0u);
    report.deferred = deferredBlocked;
    if (localFaultBlocked || deferredBlocked) {
      transactionEmitFailed = true;
      report.allAccepted = false;
      report.localFault = localFaultBlocked;
      for (std::uint8_t legIndex = 0u;
           legIndex < transaction.legCount; ++legIndex) {
        const trading_core::StrategyTransactionLegDelta& transactionLeg =
            transaction.legs[legIndex];
        if (transactionLeg.clientIntentId.raw == 0u) continue;
        trading_core::StrategyEmitLegReport& leg =
            report.legs[report.legCount++];
        leg.clientIntentId = transactionLeg.clientIntentId;
        leg.routeHandle = transactionLeg.routeHandle;
        leg.requestedQty = transactionLeg.expectedFillQty;
        leg.generation = transactionLeg.generation;
      }
    } else {
      std::size_t transactionOrderCount = 0u;
      std::size_t attemptedOrderCount = 0u;
      std::size_t acceptedOrderCount = 0u;
      for (std::size_t orderIndex = 0u;
           orderIndex < io.orderCount; ++orderIndex) {
        const trading_core::OrderIntent& order = io.orderOut[orderIndex];
        if (order.transactionId.raw != transaction.transactionId.raw) {
          continue;
        }
        ++transactionOrderCount;
        if (report.localFault) continue;
        const std::size_t routeLeg =
            routeLegIndex(context, order.routeHandle);
        const trading_core::StrategyTransactionLegDelta* transactionLeg =
            nullptr;
        for (std::uint8_t legIndex = 0u;
             legIndex < transaction.legCount; ++legIndex) {
          if (routeLegIndex(context,
                            transaction.legs[legIndex].routeHandle) ==
              routeLeg) {
            transactionLeg = &transaction.legs[legIndex];
            break;
          }
        }
        if (!transactionLeg || routeLeg >= engines.size()) {
          report.localFault = true;
          continue;
        }
        std::array<std::size_t,
                   trading_core::kStrategyRuntimeMaxLegs> firstRecords{};
        firstRecords[routeLeg] = engines[routeLeg].orders.size();
        trading_core::StrategyIo one{};
        one.orderOut = Span<trading_core::OrderIntent>(
            const_cast<trading_core::OrderIntent*>(&order), 1u);
        one.orderCount = 1u;
        detail::applyIntents(
            engines[routeLeg], one, tsNs, engines[routeLeg].latency);
        const TransactionOrderSimulation simulation =
            transactionOrderSimulation(
                Span<const detail::EngineState>(
                    engines.data(), engines.size()),
                firstRecords,
                routeLeg,
                order.clientIntentId);
        trading_core::StrategyEmitLegReport& leg =
            report.legs[report.legCount++];
        leg.clientIntentId = order.clientIntentId;
        leg.routeHandle = transactionLeg->routeHandle;
        leg.requestedQty = order.qty;
        leg.generation = transactionLeg->generation;
        leg.attempted = simulation.attempted;
        leg.accepted = simulation.accepted;
        ++attemptedOrderCount;
        report.anyAccepted = report.anyAccepted || leg.accepted;
        acceptedOrderCount += leg.accepted ? 1u : 0u;
        if (leg.accepted) {
          if (orderIndex >= 64u) return false;
          acceptedOrderMask |= std::uint64_t{1u} << orderIndex;
        } else {
          report.localFault = true;
        }
      }
      if (attemptedOrderCount != transactionOrderCount) {
        report.localFault = true;
      }
      report.allAccepted = !report.localFault &&
          acceptedOrderCount == transactionOrderCount;
      if (report.localFault) {
        transactionEmitFailed = true;
        failedTouchedLegMask |= transaction.touchedLegMask;
      }
    }
  }

  bool reportsValid = true;
  for (std::size_t transactionIndex = 0u;
       transactionIndex < io.transactionCount; ++transactionIndex) {
    if (!reportReady[transactionIndex]) continue;
    if (!trading_core::validStrategyEmitReportForTransaction(
            reports[transactionIndex],
            io.transactionOut[transactionIndex])) {
      reportsValid = false;
      transactionEmitFailed = true;
      failedTouchedLegMask |=
          io.transactionOut[transactionIndex].touchedLegMask;
    }
  }

  // Cleanup is scoped to the failed/overlapping lineage. A disjoint proposal
  // is independently admissible and must not lose its accepted resting order
  // because another transaction failed. Market orders cannot be canceled
  // after acceptance, so their later private evidence remains authoritative.
  if (transactionEmitFailed) {
    for (std::size_t orderIndex = 0u;
         orderIndex < io.orderCount; ++orderIndex) {
      if ((acceptedOrderMask &
           (std::uint64_t{1u} << orderIndex)) == 0u) {
        continue;
      }
      const trading_core::OrderIntent& order = io.orderOut[orderIndex];
      if (order.orderType == canon::OrderType::Market) continue;
      const trading_core::StrategyTransactionIntent* owner = nullptr;
      for (std::size_t transactionIndex = 0u;
           transactionIndex < io.transactionCount; ++transactionIndex) {
        if (io.transactionOut[transactionIndex].transactionId.raw ==
            order.transactionId.raw) {
          owner = &io.transactionOut[transactionIndex];
          break;
        }
      }
      if (!owner ||
          (owner->touchedLegMask & failedTouchedLegMask) == 0u) {
        continue;
      }
      const std::size_t leg =
          routeLegIndex(context, order.routeHandle);
      if (leg >= engines.size()) return false;
      trading_core::CancelIntent cleanup{};
      cleanup.clientIntentId = descriptor.allocateClientId(state);
      if (cleanup.clientIntentId.raw == 0u) return false;
      cleanup.targetClientIntentId = order.clientIntentId;
      cleanup.targetOrderType = order.orderType;
      cleanup.routeHandle = order.routeHandle;
      trading_core::StrategyIo one{};
      one.cancelOut = Span<trading_core::CancelIntent>(&cleanup, 1u);
      one.cancelCount = 1u;
      detail::applyIntents(
          engines[leg], one, tsNs, engines[leg].latency);
    }
  }
  if (!reportsValid) return false;

  for (std::size_t transactionIndex = 0u;
       transactionIndex < io.transactionCount; ++transactionIndex) {
    if (!reportReady[transactionIndex]) continue;
    if (!descriptor.applyEmitReport(
            state, reports[transactionIndex])) {
      return false;
    }
  }
  // Backtest owns no authoritative recovery/reconciliation policy. Reports
  // are delivered first so strategy state observes the actual emit outcome;
  // then the replay fails closed instead of continuing under permanent hidden
  // quarantine.
  return !transactionEmitFailed && !recoveryCompletionRequiresFailStop;
}

}  // namespace hft_backtest::strategy_intent_lowering
