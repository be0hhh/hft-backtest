#include "backtest_metrics.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

namespace {

using namespace hft_backtest::detail;

bool expect(bool value, std::string_view message) {
  if (value) return true;
  std::cerr << "FAIL: " << message << "\n";
  return false;
}

bool testExcursionMetricsUseRiskSnapshotsWhenAvailable() {
  EngineState engine{};
  engine.equityPoints.push_back(JsonEquityPoint{100, 0, 0, 0, 0, 0, 0, 0, 100000000000ll, 0, 0});
  engine.riskSnapshots.push_back(JsonRiskSnapshot{200, 0, 0, 0, 0, 300000000ll, 0, 100000000000ll, 0, 0, 0, 0, 0, kRiskSnapshotReasonStart});
  engine.riskSnapshots.push_back(JsonRiskSnapshot{300, 0, 0, 0, 0, -200000000ll, 0, 100000000000ll, 0, 0, 0, 0, 0, kRiskSnapshotReasonInterval});
  engine.riskSnapshots.push_back(JsonRiskSnapshot{400, 0, 0, 0, 0, 100000000ll, 0, 100000000000ll, 0, 0, 0, 0, 0, kRiskSnapshotReasonFinal});

  const BacktestExcursionMetrics metrics = calculateExcursionMetrics(engine);

  return expect(metrics.mfeE8 == 300000000ll, "MFE captures best risk snapshot pnl") &&
         expect(metrics.maeE8 == -200000000ll, "MAE captures worst risk snapshot pnl") &&
         expect(metrics.maxDrawdownE8 == 500000000ll, "drawdown captures peak-to-trough loss");
}

bool testExcursionMetricsFallbackToEquityPoints() {
  EngineState engine{};
  engine.equityPoints.push_back(JsonEquityPoint{100, 0, 200000000ll, 0, 0, 200000000ll, 200000000ll, 0, 100000000000ll, 0, 0});
  engine.equityPoints.push_back(JsonEquityPoint{200, 0, -100000000ll, 0, 0, -100000000ll, -100000000ll, 0, 100000000000ll, 0, 0});

  const BacktestExcursionMetrics metrics = calculateExcursionMetrics(engine);

  return expect(metrics.mfeE8 == 200000000ll, "equity fallback captures MFE") &&
         expect(metrics.maeE8 == -100000000ll, "equity fallback captures MAE") &&
         expect(metrics.maxDrawdownE8 == 300000000ll, "equity fallback captures drawdown");
}

}  // namespace

int main() {
  bool ok = true;
  ok = testExcursionMetricsUseRiskSnapshotsWhenAvailable() && ok;
  ok = testExcursionMetricsFallbackToEquityPoints() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
