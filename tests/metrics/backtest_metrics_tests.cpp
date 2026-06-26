#include "accounting/backtest_metrics.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>
#include <vector>

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

bool testPortfolioExcursionMetricsMergeLegRiskSnapshots() {
  EngineState first{};
  first.riskSnapshots.push_back(JsonRiskSnapshot{100, 0, 0, 0, 0, 100000000ll, 0, 100000000000ll, 0, 0, 0, 0, 0, kRiskSnapshotReasonStart});
  first.riskSnapshots.push_back(JsonRiskSnapshot{300, 0, 0, 0, 0, -200000000ll, 0, 100000000000ll, 0, 0, 0, 0, 0, kRiskSnapshotReasonInterval});

  EngineState second{};
  second.riskSnapshots.push_back(JsonRiskSnapshot{200, 0, 0, 0, 0, 250000000ll, 0, 100000000000ll, 0, 0, 0, 0, 0, kRiskSnapshotReasonStart});
  second.riskSnapshots.push_back(JsonRiskSnapshot{400, 0, 0, 0, 0, -50000000ll, 0, 100000000000ll, 0, 0, 0, 0, 0, kRiskSnapshotReasonFinal});

  const std::vector<EngineState> engines{first, second};
  const BacktestExcursionMetrics metrics = calculatePortfolioExcursionMetrics(engines);

  return expect(metrics.mfeE8 == 350000000ll, "portfolio MFE sums latest leg pnl at merged timestamps") &&
         expect(metrics.maeE8 == -250000000ll, "portfolio MAE captures aggregate adverse excursion") &&
         expect(metrics.maxDrawdownE8 == 600000000ll, "portfolio drawdown captures aggregate peak-to-trough loss");
}

bool testPortfolioPnlCurveMergesLegEquityPoints() {
  EngineState first{};
  first.equityPoints.push_back(JsonEquityPoint{100, 0, 100000000ll, 0, 0, 100000000ll, 100000000ll, 0, 100000000000ll, 0, 0});
  first.equityPoints.push_back(JsonEquityPoint{300, 0, -200000000ll, 0, 0, -200000000ll, -200000000ll, 0, 100000000000ll, 0, 0});

  EngineState second{};
  second.equityPoints.push_back(JsonEquityPoint{200, 0, 250000000ll, 0, 0, 250000000ll, 250000000ll, 0, 100000000000ll, 0, 0});
  second.equityPoints.push_back(JsonEquityPoint{400, 0, -50000000ll, 0, 0, -50000000ll, -50000000ll, 0, 100000000000ll, 0, 0});

  const std::vector<EngineState> engines{first, second};
  const std::vector<std::int64_t> curve = buildPortfolioTotalPnlCurveE8(engines);

  return expect(curve.size() == 4u, "portfolio curve has one point per merged leg timestamp") &&
         expect(curve[0] == 100000000ll, "first aggregate curve point uses first leg pnl") &&
         expect(curve[1] == 350000000ll, "second aggregate curve point sums latest leg values") &&
         expect(curve[2] == 50000000ll, "third aggregate curve point carries second leg forward") &&
         expect(curve[3] == -250000000ll, "final aggregate curve point sums both latest leg values");
}

bool testPortfolioExcursionMetricsFallbackToEquityWhenRiskSnapshotsAreMixed() {
  EngineState first{};
  first.riskSnapshots.push_back(JsonRiskSnapshot{100, 0, 0, 0, 0, 100000000ll, 0, 100000000000ll, 0, 0, 0, 0, 0, kRiskSnapshotReasonStart});
  first.equityPoints.push_back(JsonEquityPoint{100, 0, 100000000ll, 0, 0, 100000000ll, 100000000ll, 0, 100000000000ll, 0, 0});
  first.equityPoints.push_back(JsonEquityPoint{300, 0, -200000000ll, 0, 0, -200000000ll, -200000000ll, 0, 100000000000ll, 0, 0});

  EngineState second{};
  second.equityPoints.push_back(JsonEquityPoint{200, 0, 250000000ll, 0, 0, 250000000ll, 250000000ll, 0, 100000000000ll, 0, 0});
  second.equityPoints.push_back(JsonEquityPoint{400, 0, -50000000ll, 0, 0, -50000000ll, -50000000ll, 0, 100000000000ll, 0, 0});

  const std::vector<EngineState> engines{first, second};
  const BacktestExcursionMetrics metrics = calculatePortfolioExcursionMetrics(engines);

  return expect(metrics.mfeE8 == 350000000ll, "mixed risk snapshots fallback keeps both legs in MFE") &&
         expect(metrics.maeE8 == -250000000ll, "mixed risk snapshots fallback keeps both legs in MAE") &&
         expect(metrics.maxDrawdownE8 == 600000000ll, "mixed risk snapshots fallback keeps both legs in drawdown");
}

}  // namespace

int main() {
  bool ok = true;
  ok = testExcursionMetricsUseRiskSnapshotsWhenAvailable() && ok;
  ok = testExcursionMetricsFallbackToEquityPoints() && ok;
  ok = testPortfolioExcursionMetricsMergeLegRiskSnapshots() && ok;
  ok = testPortfolioPnlCurveMergesLegEquityPoints() && ok;
  ok = testPortfolioExcursionMetricsFallbackToEquityWhenRiskSnapshotsAreMixed() && ok;
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
