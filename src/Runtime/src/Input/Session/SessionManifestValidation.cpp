#include "Internal/SessionLoaderHelpers.hpp"

namespace hft_backtest::detail::session_loader {

bool cleanRequiredRuntimeChannel(const hftrec::capture::ChannelRuntimeHealth& health) noexcept {
  return health.state == "live" && health.reconnectCount == 0u &&
      health.droppedEventCount == 0u && health.unroutableEventCount == 0u &&
      health.lastError.empty();
}

bool addArrivalRowCount(std::uint64_t value, std::uint64_t& total) noexcept {
  if (value > std::numeric_limits<std::uint64_t>::max() - total) return false;
  total += value;
  return true;
}

bool arrivalSummaryMatchesDeclaredRows(
    const hftrec::capture::SessionManifest& manifest) noexcept {
  // Framing evidence covers the complete immutable recording, including
  // historical channels which this product intentionally no longer replays.
  std::uint64_t declaredRows = 0u;
  if (!addArrivalRowCount(manifest.tradesCount, declaredRows) ||
      !addArrivalRowCount(manifest.liquidationsCount, declaredRows) ||
      !addArrivalRowCount(manifest.bookTickerCount, declaredRows) ||
      !addArrivalRowCount(manifest.markPriceCount, declaredRows) ||
      !addArrivalRowCount(manifest.indexPriceCount, declaredRows) ||
      !addArrivalRowCount(manifest.fundingCount, declaredRows) ||
      !addArrivalRowCount(manifest.priceLimitCount, declaredRows) ||
      !addArrivalRowCount(manifest.depthCount, declaredRows) ||
      !addArrivalRowCount(manifest.candlesCount, declaredRows) ||
      !addArrivalRowCount(manifest.candles2Count, declaredRows)) {
    return false;
  }

  std::uint64_t accountedRows = 0u;
  return addArrivalRowCount(manifest.arrivalClock.capturedRows, accountedRows) &&
      addArrivalRowCount(manifest.arrivalClock.historicalRows, accountedRows) &&
      addArrivalRowCount(manifest.arrivalClock.unavailableRows, accountedRows) &&
      accountedRows == declaredRows;
}

bool validateManifestChannel(std::string_view name,
                             bool load,
                             bool required,
                             bool enabled,
                             std::uint64_t count,
                             std::string_view path,
                             const hftrec::capture::ChannelRuntimeHealth* runtime,
                             std::vector<std::string>& warnings,
                             std::string& error) {
  if (!load) return true;
  if (!enabled) {
    if (!required) return true;
    error = "manifest does not enable required ";
    error += name;
    error += " channel";
    return false;
  }
  if (path.empty()) {
    error = "unsupported captured-arrival ";
    error += name;
    error += " channel contract";
    return false;
  }
  if (required && count == 0u) {
    error = "manifest declares empty required ";
    error += name;
    error += " channel";
    return false;
  }
  if (runtime && !cleanRequiredRuntimeChannel(*runtime)) {
    if (required) {
      error = "required ";
      error += name;
      error += " channel is not clean";
      return false;
    }
    appendSessionWarning(warnings, "optional " + std::string{name} + " channel is degraded");
  }
  return true;
}

bool validateCurrentRecorderManifest(const hftrec::capture::SessionManifest& manifest,
                                     const SessionLoadPlan& plan,
                                     std::vector<std::string>& warnings,
                                     std::string& error) {
  if (manifest.sessionStatus != "complete") {
    error = "recorder session is not finalized: status=" + manifest.sessionStatus;
    return false;
  }
  if (!manifest.structurallyLoadable ||
      manifest.sessionHealth != hftrec::SessionHealth::Clean ||
      !manifest.exactReplayEligible) {
    error = "recorder session is structurally incompatible or corrupt";
    return false;
  }
  if (manifest.arrivalClock.boundary !=
          "hft-parser.application-frame-ready" ||
      manifest.arrivalClock.realtimeClock != "CLOCK_REALTIME" ||
      manifest.arrivalClock.monotonicClock != "CLOCK_MONOTONIC") {
    error = "recorder session uses an unsupported arrival-clock boundary";
    return false;
  }
  if (manifest.arrivalClock.capturedRows == 0u ||
      manifest.arrivalClock.unavailableRows != 0u) {
    error = "recorder session is not captured-arrival replay eligible";
    return false;
  }
  if (manifest.arrivalClock.firstReceiveRealtimeNs <= 0 ||
      manifest.arrivalClock.lastReceiveRealtimeNs <= 0 ||
      manifest.arrivalClock.firstReceiveMonotonicNs == 0u ||
      manifest.arrivalClock.lastReceiveMonotonicNs == 0u ||
      !arrivalSummaryMatchesDeclaredRows(manifest)) {
    error = "recorder arrival-clock summary does not match declared corpus rows";
    return false;
  }
  if (manifest.arrivalClock.realtimeRegressions != 0u)
    appendSessionWarning(warnings, "CLOCK_REALTIME regressions preserved: " +
        std::to_string(manifest.arrivalClock.realtimeRegressions));
  if (manifest.arrivalClock.monotonicNonIncreasing != 0u)
    appendSessionWarning(warnings, "non-increasing CLOCK_MONOTONIC samples preserved: " +
        std::to_string(manifest.arrivalClock.monotonicNonIncreasing));
  if (manifest.arrivalClock.exchangeAheadOfReceive != 0u)
    appendSessionWarning(warnings, "exchange timestamps ahead of local receive preserved: " +
        std::to_string(manifest.arrivalClock.exchangeAheadOfReceive));
  if (manifest.arrivalClock.exchangeTimestampMissing != 0u)
    appendSessionWarning(warnings, "missing exchange timestamps preserved: " +
        std::to_string(manifest.arrivalClock.exchangeTimestampMissing));
  if (!manifest.warningSummary.empty()) appendSessionWarning(warnings, "capture warning: " + manifest.warningSummary);

  if (!validateManifestChannel("trades", plan.loadTrades, plan.requireTrades,
          manifest.tradesEnabled, manifest.tradesCount, manifest.tradesPath,
          &manifest.tradesRuntime,
          warnings, error)) return false;
  if (!validateManifestChannel("bookticker", plan.loadBookTickers, plan.requireBookTickers,
          manifest.bookTickerEnabled, manifest.bookTickerCount, manifest.bookTickerPath,
          &manifest.bookTickerRuntime,
          warnings, error)) return false;
  if (!validateManifestChannel("depth", plan.loadDepths, plan.requireDepths,
          manifest.orderbookEnabled, manifest.depthCount, manifest.depthPath,
          &manifest.depthRuntime,
          warnings, error)) return false;
  if (plan.loadDepths && manifest.orderbookEnabled && manifest.depthSidecarPath.empty()) {
    error = "captured-arrival depth sidecar path is missing";
    return false;
  }
  if (plan.loadCandles && manifest.exchange == "finam") {
    const bool useDetailed = manifest.candles2Enabled;
    const bool enabled = useDetailed || manifest.candlesEnabled;
    const std::uint64_t count = useDetailed ? manifest.candles2Count : manifest.candlesCount;
    const std::string& path = useDetailed ? manifest.candles2Path : manifest.candlesPath;
    if (!validateManifestChannel("candles", true, plan.requireCandles, enabled, count, path,
                                 nullptr, warnings, error)) return false;
  }
  return true;
}

bool validateLoadedRowCount(std::string_view channel,
                            std::size_t actual,
                            std::uint64_t declared,
                            std::string& error) {
  if (static_cast<std::uint64_t>(actual) == declared) return true;
  error = "captured-arrival ";
  error += channel;
  error += " row count does not match finalized manifest";
  return false;
}

bool validateHistoricalSeeds(const SessionData& out, std::string& error) {
  for (std::size_t i = 0u; i < out.tradeWarmup.size(); ++i) {
    if (hftrec::replay::isHistoricalBackfill(out.tradeWarmup[i].arrival)) continue;
    error = "trade warmup row is not marked historical_backfill at row " +
        std::to_string(i + 1u);
    return false;
  }
  for (std::size_t i = 0u; i < out.candles.size(); ++i) {
    if (hftrec::replay::isHistoricalBackfill(out.candles[i].arrival)) continue;
    error = "candle seed row is not marked historical_backfill at row " +
        std::to_string(i + 1u);
    return false;
  }
  return true;
}

bool validateCapturedReplayRows(const SessionData& out, std::string& error) {
  if (out.firstReceiveRealtimeNs <= 0 || out.firstReceiveMonotonicNs == 0u) {
    error = "captured-arrival replay clock anchor is missing";
    return false;
  }
  const __int128 replayOffset = static_cast<__int128>(out.firstReceiveRealtimeNs) -
      static_cast<__int128>(out.firstReceiveMonotonicNs);
  if (replayOffset < std::numeric_limits<std::int64_t>::min() ||
      replayOffset > std::numeric_limits<std::int64_t>::max()) {
    error = "captured-arrival replay clock anchor is out of range";
    return false;
  }
  return validateCapturedLiveRows("trades", out.trades, error) &&
      validateCapturedLiveRows("bookticker", out.bookTickers, error) &&
      validateCapturedLiveRows("depth", out.depths, error) &&
      validateReplayClockRows("trades", out.trades, replayOffset, error) &&
      validateReplayClockRows("bookticker", out.bookTickers, replayOffset, error) &&
      validateReplayClockRows("depth", out.depths, replayOffset, error) &&
      validateHistoricalSeeds(out, error);
}

}  // namespace hft_backtest::detail::session_loader
