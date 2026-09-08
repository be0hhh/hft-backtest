# Statistical Evaluation Framework для HFT Backtest

Документ описывает, как оценивать торговые гипотезы и стратегии на уровне статистики: Monte Carlo, распределения PnL, drawdown, MAE/MFE, SQN, PSR/DSR, PBO и SPA/Reality Check. Цель — не подобрать лучший параметр по одному красивому backtest, а доказать, что стратегия имеет устойчивое математическое ожидание и не является результатом data mining.

Этот файл предназначен для размещения в `doc/backtest/` и должен использоваться как спецификация для дальнейшей разработки исследовательского слоя backtest.

---

## 1. Основная идея

Backtest одной equity curve недостаточен. Один результат вида:

```text
net_pnl = +1234
max_drawdown = -210
sharpe = 4.1
```

не отвечает на главный вопрос:

```text
мог ли этот результат появиться случайно из-за конкретного куска истории, выбранных параметров или перебора гипотез?
```

Поэтому каждая стратегия должна оцениваться не одной цифрой, а распределением возможных исходов:

```text
terminal_pnl_distribution
max_drawdown_distribution
drawdown_duration_distribution
expected_shortfall_distribution
P(PnL < 0)
P(MDD > risk_limit)
P(overfit)
```

Главный объект оптимизации:

```text
нижний хвост распределения после costs, а не максимальный observed PnL
```

Стратегия считается перспективной только если её плохие сценарии остаются приемлемыми.

---

## 2. Единица анализа

Для HFT не следует считать статистику только по отдельным сделкам. Сделки часто зависимы: они идут сериями, зависят от режима волатильности, ликвидности, spread regime и event intensity. Если считать каждую сделку независимой, качество стратегии будет завышено.

Базовая единица анализа для статистики:

```text
time-block PnL
```

Пример:

```text
1s block_pnl
5s block_pnl
10s block_pnl
30s block_pnl
```

Каждый блок содержит суммарный результат стратегии внутри временного интервала:

```text
block_pnl_i = sum(realized_pnl + unrealized_delta - fees - costs внутри блока i)
```

Для alpha research до реального execution можно использовать:

```text
alpha_return_t = signal_t * future_mid_return_t - cost_buffer
```

Затем `alpha_return_t` также агрегируется в time blocks.

---

## 3. Исследовательский pipeline

Единый pipeline оценки стратегии:

```text
strategy candidate
    -> deterministic backtest
    -> block PnL series
    -> stationary block bootstrap / Monte Carlo
    -> distribution report
    -> parameter robustness
    -> PBO
    -> PSR / DSR
    -> SPA or Reality Check vs benchmark
    -> accept / reject / research-only
```

Нельзя принимать стратегию только по `total_pnl`, `win_rate` или `sharpe` из одного прогона.

---

## 4. Monte Carlo через block bootstrap

Monte Carlo в этом контексте не должен симулировать новую цену. Историческая структура рынка уже содержит реальные режимы. Задача Monte Carlo — проверить, насколько результат устойчив, если переупаковать похожие куски истории.

Метод:

```text
stationary block bootstrap по time-block PnL
```

Алгоритм:

```text
input:
    r[1..N] = ряд block_pnl
    B = количество Monte Carlo paths, например 10000
    L = средняя длина bootstrap block

for b in 1..B:
    synthetic_path = []

    while len(synthetic_path) < N:
        start = random integer from [1, N]
        block_len = geometric_random(mean=L)

        append r[start : start + block_len] into synthetic_path
        wrap around if index > N

    synthetic_path = synthetic_path[1..N]
    equity = cumulative_sum(synthetic_path)

    compute metrics:
        terminal_pnl
        max_drawdown
        drawdown_duration
        sharpe
        sortino
        calmar
        skew
        excess_kurtosis
        expected_shortfall
        min_equity

output:
    distributions for all metrics
```

Почему не shuffle по отдельным сделкам:

```text
simple shuffle destroys:
    volatility clustering
    spread regimes
    liquidity regimes
    serial correlation
    losing streaks
    clustered adverse periods
```

Это занижает drawdown и делает стратегию искусственно лучше.

---

## 5. Выбор размера блока

Размер блока должен зависеть от структуры стратегии и рынка.

Правило:

```text
bootstrap_block_size = max(
    5 * average_holding_time,
    autocorrelation_decay_time,
    minimum_regime_fragment
)
```

Где:

```text
average_holding_time
    среднее время удержания сделки или сигнала

autocorrelation_decay_time
    лаг, после которого autocorrelation block_pnl становится близкой к нулю

minimum_regime_fragment
    минимальный кусок рынка, который сохраняет локальный режим ликвидности/волатильности
```

Практический старт для HFT research:

```text
block_pnl_interval = 1s or 5s
bootstrap_block_size = 10s to 60s
```

Но финальное значение должно выбираться по autocorrelation и holding horizon, а не вручную.

---

## 6. Основные распределения

Каждый strategy run должен давать не только observed metrics, но и Monte Carlo distributions.

Обязательные распределения:

```text
terminal_pnl
max_drawdown
drawdown_duration
expected_shortfall
sharpe
sortino
calmar
profit_factor
skew
excess_kurtosis
```

Для каждой метрики нужно сохранять:

```text
observed
mean
std
q01
q05
q10
q25
q50
q75
q90
q95
q99
```

Отдельно считать вероятности:

```text
P(PnL < 0)
P(PnL < required_min_pnl)
P(MDD > risk_limit)
P(drawdown_duration > duration_limit)
P(min_equity < ruin_level)
```

---

## 7. Главные метрики PnL и риска

### 7.1 Terminal PnL

```text
terminal_pnl = final_equity - initial_equity
```

Использование:

```text
q05(terminal_pnl) > 0
```

важнее, чем высокий `observed terminal_pnl`.

---

### 7.2 Expected Shortfall

Expected Shortfall показывает средний результат в плохом хвосте распределения.

```text
ES_5 = mean(PnL paths where PnL <= q05(PnL))
```

Если:

```text
q05(PnL) = -100
ES_5(PnL) = -900
```

то хвост опасный: граница плохого сценария выглядит терпимо, но худшие случаи намного хуже.

---

### 7.3 Maximum Drawdown

```text
MDD = max peak-to-trough loss of equity curve
```

Для каждой Monte Carlo path:

```text
running_peak_t = max(equity_1 ... equity_t)
drawdown_t = equity_t - running_peak_t
MDD = min(drawdown_t)
```

Использование:

```text
q95(abs(MDD)) <= risk_limit
P(abs(MDD) > risk_limit) <= allowed_probability
```

---

### 7.4 Drawdown Duration

```text
drawdown_duration = максимальное время от equity peak до восстановления этого peak
```

Эта метрика важна, потому что стратегия может иметь приемлемый MDD, но очень долго не восстанавливаться.

---

### 7.5 Risk of Ruin

```text
risk_of_ruin = P(min_equity <= ruin_level)
```

Для исследовательской оценки `ruin_level` может быть задан как:

```text
initial_capital - max_allowed_loss
```

или как margin/liquidation threshold для production-сценария.

---

## 8. MAE / MFE диагностика

MAE и MFE нужны для понимания качества входа и выхода.

```text
MAE = Maximum Adverse Excursion
MFE = Maximum Favorable Excursion
```

Для long-сделки:

```text
entry_price = p0
MAE = min(price_t - p0 during trade)
MFE = max(price_t - p0 during trade)
final_pnl = exit_price - p0 - costs
```

Для short-сделки знаки инвертируются так, чтобы положительное движение было favorable.

Интерпретация:

```text
high MAE, low final PnL
    плохой вход или слишком ранний вход

high MFE, low final PnL
    плохой выход, стратегия отдаёт прибыль назад

low MAE, high final PnL
    хороший вход и нормальное удержание

high MAE, high final PnL
    стратегия зарабатывает, но терпит большой intra-trade risk
```

Обязательные MAE/MFE метрики:

```text
mae_mean
mae_q50
mae_q90
mae_q95
mfe_mean
mfe_q50
mfe_q90
mfe_q95
mfe_to_mae_ratio
final_pnl_to_mfe_ratio
```

Полезная метрика качества выхода:

```text
capture_ratio = final_pnl / MFE
```

Если `capture_ratio` низкий, стратегия часто видит прибыль, но не забирает её.

---

## 9. SQN

SQN = System Quality Number.

Классическая формула:

```text
SQN = sqrt(N) * mean(R) / std(R)
```

где `R` — нормализованный результат сделки или блока.

Для HFT нельзя считать SQN по каждой микросделке как по независимому наблюдению. Это завысит результат из-за зависимости сделок.

Правильный вариант:

```text
SQN_block = sqrt(N_blocks) * mean(block_R) / std(block_R)
```

Где:

```text
block_R = block_pnl / risk_unit
```

`risk_unit` должен быть стабильным масштабом риска:

```text
notional_risk
average_spread_cost
fixed capital unit
historical block volatility
```

Использование:

```text
SQN_block > 2.0
```

можно считать положительным диагностическим признаком, но не самостоятельным основанием для принятия стратегии.

---

## 10. Skewness и kurtosis

Стратегии HFT и market making часто имеют профиль:

```text
много маленьких выигрышей
редкие большие убытки
```

Поэтому нужно смотреть форму распределения.

```text
skewness < 0
    левый хвост тяжелее, стратегия имеет crash-risk

excess_kurtosis >> 0
    fat tails, обычный Sharpe менее надёжен
```

Обязательные метрики:

```text
skewness
excess_kurtosis
left_tail_mean_1
left_tail_mean_5
right_tail_mean_95
right_tail_mean_99
tail_ratio = abs(q95 positive move / q05 negative move)
```

Если стратегия имеет высокий Sharpe, но сильно отрицательный skew и большой left expected shortfall, её нельзя считать устойчивой без дополнительных ограничений риска.

---

## 11. Sharpe, PSR и DSR

### 11.1 Sharpe Ratio

```text
Sharpe = mean(return) / std(return)
```

Для intraday/HFT annualization часто вводит лишние ошибки. На исследовательском уровне лучше хранить:

```text
raw_sharpe_per_block
mean_block_return
std_block_return
block_interval
```

Annualized Sharpe можно считать только если ясно задана частота блоков и предположение о независимости.

---

### 11.2 PSR — Probabilistic Sharpe Ratio

PSR отвечает на вопрос:

```text
какова вероятность, что настоящий Sharpe выше выбранного benchmark Sharpe?
```

Использование:

```text
PSR(SR > SR_benchmark)
```

Например:

```text
PSR > 0.95
```

означает, что стратегия статистически вероятно лучше заданного Sharpe threshold.

---

### 11.3 DSR — Deflated Sharpe Ratio

DSR нужен, когда было много попыток:

```text
много стратегий
много параметров
много горизонтов
много фильтров
много версий score
```

DSR корректирует Sharpe с учётом:

```text
finite sample
skewness
kurtosis
multiple testing
selection bias
```

Интерпретация:

```text
high Sharpe, low DSR
    вероятный overfit

moderate Sharpe, high DSR
    более чистый статистический edge
```

Для принятия стратегии:

```text
DSR >= 0.95
```

желательный порог.

---

## 12. PBO — Probability of Backtest Overfitting

PBO оценивает вероятность того, что лучший in-sample кандидат окажется плохим out-of-sample.

Используется, когда есть family of candidates:

```text
strategy variants
parameter grid
threshold variants
horizon variants
feature combinations
```

Алгоритм CSCV/PBO:

```text
input:
    M candidates
    S chronological blocks

for each split of S blocks into IS and OOS:
    for each candidate m:
        score_IS[m] = performance on IS
        score_OOS[m] = performance on OOS

    winner = argmax(score_IS)
    rank_OOS = rank of winner by score_OOS among all candidates

    if rank_OOS below median:
        overfit_event += 1

PBO = overfit_event / number_of_splits
```

Интерпретация:

```text
PBO < 0.10
    хороший признак

PBO 0.10..0.25
    осторожно, нужен дополнительный OOS

PBO > 0.25
    высок риск data mining

PBO > 0.50
    selection process скорее всего выбирает случайных победителей
```

PBO должен считаться не по одной стратегии, а по всей группе кандидатов, из которой был выбран победитель.

---

## 13. SPA / Reality Check

SPA и Reality Check нужны, когда было протестировано много моделей и нужно понять:

```text
действительно ли лучший кандидат лучше benchmark,
если учесть data snooping?
```

Benchmark должен быть задан явно.

Для HFT research хорошие benchmark-и:

```text
zero-trade benchmark
random sign with same timestamps
same signal shifted in time
simple microprice baseline
simple spread-maker baseline
```

Базовый benchmark для alpha research:

```text
same timestamps, randomized direction
```

Это проверяет, действительно ли направление сигнала несёт информацию, а не только выбор моментов входа.

SPA/Reality Check оценивают distribution of performance differences:

```text
d_t_m = return_t_strategy_m - return_t_benchmark
```

Если SPA p-value низкий:

```text
SPA p-value < 0.05
```

то лучший кандидат статистически превосходит benchmark после поправки на множественный перебор.

---

## 14. Проверка alpha до execution backtest

Перед полноценной торговой симуляцией нужно проверять саму predictive связь.

Для каждого market state в момент `t`:

```text
mid_t = (best_bid_t + best_ask_t) / 2
future_return_h = mid_{t+h} - mid_t
```

Горизонты:

```text
50ms
100ms
250ms
500ms
1s
2s
5s
```

Для сигнала:

```text
signal_t in [-1, +1]
alpha_return_t_h = signal_t * future_return_t_h - cost_buffer
```

Проверки:

```text
conditional expected return by signal quantile
monotonicity by decile
horizon decay curve
regime stability
daily stability
bootstrap confidence interval for mean alpha_return
```

Сигнал перспективен, если:

```text
higher signal quantile -> higher future return
lower signal quantile -> lower future return
neutral signal -> near zero future return
bootstrap CI for mean alpha_return is above zero
```

Плохой признак:

```text
top 1% signal works, but top 5% and top 10% do not
```

Это часто означает noise или overfit.

---

## 15. Parameter robustness

Нельзя выбирать параметр по максимальному observed PnL.

Плохой selection rule:

```text
best = argmax(observed_pnl)
```

Правильный selection rule:

```text
best = argmax(robust_score)
```

Где:

```text
robust_score =
    q10(PnL)
    - 0.5 * q95(abs(MDD))
    - abs(ExpectedShortfall_5)
    - instability_penalty
```

`instability_penalty` должен штрафовать параметры, которые работают только в одной точке grid.

Признак хорошего параметра:

```text
сам параметр положительный
соседние параметры тоже положительные
q05(PnL) > 0
P(PnL < 0) низкая
PBO низкий
```

Признак плохого параметра:

```text
одна точка grid даёт огромный PnL,
а соседние точки резко хуже
```

Это почти всегда overfit.

---

## 16. Regime split

Каждая стратегия должна проверяться отдельно по режимам.

Минимальные режимы:

```text
calm
normal
volatile
high_spread
low_liquidity
high_event_rate
trend
mean_reversion
news-like spike
```

Для каждого режима сохранять:

```text
sample_count
trade_count
net_pnl
pnl_per_block
q05_monte_carlo_pnl
MDD
adverse_tail
sharpe_per_block
```

Стратегия не обязана работать во всех режимах, но она должна явно знать, где ей нельзя торговать.

Хороший результат:

```text
strategy earns in normal/calm,
does not trade or loses little in volatile/toxic regimes
```

Плохой результат:

```text
strategy earns slowly in calm regimes,
then loses most PnL in rare volatile periods
```

---

## 17. Acceptance criteria

Стратегия может перейти из research в execution-development только если проходит единый фильтр.

Рекомендуемый фильтр:

```text
q05(PnL) > 0
P(PnL < 0) < 5%
P(abs(MDD) > risk_limit) < 1%
ExpectedShortfall_5 is acceptable
DSR >= 0.95
PBO < 0.10
SPA p-value < 0.05
MAE/MFE profile does not show broken exits
parameter neighborhood is stable
regime split has no hidden catastrophic regime
```

Если стратегия не проходит фильтр, она остаётся research candidate.

---

## 18. Output schema для backtest report

Каждый запуск должен писать machine-readable report.

Пример структуры:

```json
{
  "strategy": "adaptive_order_flow_v1",
  "strategy_hash": "...",
  "dataset_id": "...",
  "session_id": "...",
  "symbol": "BTCUSDT",
  "time_range": {
    "start": "...",
    "end": "..."
  },
  "params": {
    "...": "..."
  },
  "observed": {
    "net_pnl": 0.0,
    "gross_pnl": 0.0,
    "fees": 0.0,
    "max_drawdown": 0.0,
    "drawdown_duration_ns": 0,
    "sharpe_per_block": 0.0,
    "profit_factor": 0.0,
    "trade_count": 0,
    "block_count": 0
  },
  "monte_carlo": {
    "method": "stationary_block_bootstrap",
    "paths": 10000,
    "block_pnl_interval_ns": 1000000000,
    "mean_bootstrap_block_size": 30,
    "terminal_pnl": {
      "mean": 0.0,
      "std": 0.0,
      "q01": 0.0,
      "q05": 0.0,
      "q10": 0.0,
      "q50": 0.0,
      "q90": 0.0,
      "q95": 0.0,
      "q99": 0.0
    },
    "max_drawdown": {
      "q50": 0.0,
      "q90": 0.0,
      "q95": 0.0,
      "q99": 0.0
    },
    "probabilities": {
      "p_pnl_lt_0": 0.0,
      "p_mdd_gt_limit": 0.0,
      "p_ruin": 0.0
    },
    "expected_shortfall": {
      "es_1": 0.0,
      "es_5": 0.0,
      "es_10": 0.0
    }
  },
  "distribution": {
    "skewness": 0.0,
    "excess_kurtosis": 0.0,
    "tail_ratio": 0.0
  },
  "mae_mfe": {
    "mae_q50": 0.0,
    "mae_q90": 0.0,
    "mae_q95": 0.0,
    "mfe_q50": 0.0,
    "mfe_q90": 0.0,
    "mfe_q95": 0.0,
    "capture_ratio_mean": 0.0
  },
  "quality": {
    "sqn_block": 0.0,
    "psr": 0.0,
    "dsr": 0.0,
    "pbo": 0.0,
    "spa_p_value": 0.0
  },
  "decision": {
    "status": "research_only",
    "reason": "q05 pnl below zero"
  }
}
```

---

## 19. Development checklist

Реализация в backtest должна идти в таком порядке:

```text
1. Export block PnL series from every run.
2. Add equity path metrics: MDD, drawdown duration, ES.
3. Add MAE/MFE per trade or per signal.
4. Implement stationary block bootstrap.
5. Generate Monte Carlo distribution report.
6. Add robust_score for parameter selection.
7. Add parameter neighborhood stability check.
8. Add PBO for strategy/parameter families.
9. Add PSR/DSR.
10. Add SPA/Reality Check against benchmark.
11. Add regime split report.
12. Add final accept/reject decision block.
```

---

## 20. Prompt для дальнейшей разработки

Использовать этот блок как контекст при разработке новых backtest-модулей:

```text
We are building an HFT backtesting research framework. The goal is not to maximize a single observed backtest PnL, but to evaluate whether a strategy has robust statistical edge after costs and after accounting for overfitting.

Implement features according to doc/backtest/statistical_evaluation.md.

Core principles:
- Use time-block PnL as the main statistical unit, not individual trades.
- Use stationary block bootstrap for Monte Carlo paths.
- Report distributions for terminal PnL, max drawdown, drawdown duration, expected shortfall, Sharpe, Sortino, skew, kurtosis.
- Select parameters by lower-tail robustness, not max observed PnL.
- Implement PBO for parameter/strategy families.
- Implement PSR/DSR to adjust Sharpe for finite samples, skew, kurtosis, and multiple testing.
- Implement SPA or Reality Check against explicit benchmarks.
- Add MAE/MFE diagnostics to understand entry/exit quality.
- Add regime split to detect hidden catastrophic regimes.

A strategy can only be promoted from research candidate to execution-development candidate if:
q05(PnL) > 0,
P(PnL < 0) < 5%,
P(abs(MDD) > risk_limit) < 1%,
DSR >= 0.95,
PBO < 0.10,
SPA p-value < 0.05,
parameter neighborhood is stable,
and MAE/MFE does not show broken exits.

When implementing, prefer deterministic, reproducible outputs. Every stochastic procedure must store seed, number of paths, block size, input series hash, and dataset/session identifiers.
```

---

## 21. Final rule

The backtest framework should answer one question:

```text
Does this strategy still look profitable and controlled in the bad part of the distribution,
after costs, regimes, parameter search, and multiple testing?
```

If the answer is not clearly yes, the strategy is not accepted.
