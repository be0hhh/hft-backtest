#include "hft_backtest/strategy_metadata.hpp"

namespace hft_backtest {
namespace {

inline constexpr StrategyMetadata kStrategies[] = {
    StrategyMetadata{
        .id = "spread_maker1and2",
        .displayNameRu = "Спред-мейкер 1/2",
        .configTemplateFile = "1and2.ini",
        .minSessionCount = 1u,
        .maxSessionCount = 1u,
        .paramCount = 6u,
        .params = {
            StrategyParamMetadata{"distance_bps", "Дистанция котировки, bps", "На сколько базисных пунктов отступать от текущего bid/ask при выставлении лимитных заявок.", "250", StrategyParamKind::Uint, 0u, true, false, true},
            StrategyParamMetadata{"trigger_bps", "Триггер перекотировки, bps", "Минимальное движение базового bid/ask в bps, после которого стратегия пересчитывает и заменяет котировку.", "10", StrategyParamKind::Uint, 0u, true, false, true},
            StrategyParamMetadata{"refresh_ms", "Минимальный интервал обновления, мс", "Защита от слишком частой перекотировки. 0 означает обновлять сразу при выполнении триггера.", "0", StrategyParamKind::Uint, 0u, true, false, true},
            StrategyParamMetadata{"close_delay_us", "Задержка закрытия, мкс", "Пауза перед reduce-only market close после появления позиции.", "2000000", StrategyParamKind::Uint, 0u, false, true, true},
            StrategyParamMetadata{"amount_qty", "Размер в котируемой валюте", "Размер заявки как сумма в quote currency; стратегия сама переведет его в количество по цене и шагу инструмента.", "8", StrategyParamKind::DecimalQuoteAmount, 1u, true, false, true},
            StrategyParamMetadata{"order_qty", "Размер в базовой валюте", "Фиксированное количество base asset. Используется вместо размера в quote currency.", "0.001", StrategyParamKind::DecimalAmount, 1u, true, false, false},
        },
        .paramGroupCount = 1u,
        .paramGroups = {StrategyParamGroupMetadata{1u, "sizing_mode"}},
    },
    StrategyMetadata{
        .id = "backtest_probe",
        .displayNameRu = "Диагностический backtest probe",
        .configTemplateFile = "probe.ini",
        .minSessionCount = 1u,
        .maxSessionCount = 1u,
        .indicatorCount = 2u,
        .indicators = {
            StrategyIndicatorMetadata{"trade_range", "Trade range"},
            StrategyIndicatorMetadata{"rolling_volume", "Rolling volume"},
        },
    },
    StrategyMetadata{
        .id = "two_leg_probe",
        .displayNameRu = "Диагностический two-leg probe",
        .configTemplateFile = "two_leg_probe.ini",
        .minSessionCount = 2u,
        .maxSessionCount = 2u,
    },
    StrategyMetadata{
        .id = "stat_arb_bestside",
        .displayNameRu = "Стат-арбитраж best-side",
        .configTemplateFile = "stat_arb_bestside.ini",
        .minSessionCount = 2u,
        .maxSessionCount = 2u,
        .paramCount = 7u,
        .params = {
            StrategyParamMetadata{"ema_window_ms", "Окно EMA, мс", "Окно EMA нормального spread отклонения.", "5000", StrategyParamKind::Uint, 0u, true, false, true},
            StrategyParamMetadata{"entry_edge_bps", "Запас входа, bps", "Минимальный запас сверх комиссии и внутренней стоимости закрытия.", "0", StrategyParamKind::DecimalQuoteAmount, 0u, true, false, true},
            StrategyParamMetadata{"exit_edge_bps", "Запас выхода, bps", "Закрывать пару, когда edgeAfterCost возвращается ниже этого значения.", "0", StrategyParamKind::DecimalQuoteAmount, 0u, true, false, true},
            StrategyParamMetadata{"refresh_ms", "Минимальный интервал, мс", "0 означает разрешить новый вход/выход сразу на следующем сигнале.", "0", StrategyParamKind::Uint, 0u, true, false, true},
            StrategyParamMetadata{"max_position_pairs", "Максимум пар", "v1 поддерживает одну открытую пару; параметр оставлен для явного конфига.", "1", StrategyParamKind::Uint, 0u, true, true, true},
            StrategyParamMetadata{"amount_qty", "Размер в котируемой валюте", "Размер каждой ноги как сумма в quote currency.", "10", StrategyParamKind::DecimalQuoteAmount, 1u, true, false, true},
            StrategyParamMetadata{"order_qty", "Размер в базовой валюте", "Фиксированное количество base asset для каждой ноги.", "0.001", StrategyParamKind::DecimalAmount, 1u, true, false, false},
        },
        .paramGroupCount = 1u,
        .paramGroups = {StrategyParamGroupMetadata{1u, "sizing_mode"}},
    },
    StrategyMetadata{
        .id = "range_reversion",
        .displayNameRu = "Ренж-реверсия",
        .configTemplateFile = "range_reversion.ini",
        .minSessionCount = 1u,
        .maxSessionCount = 1u,
        .paramCount = 12u,
        .params = {
            StrategyParamMetadata{"range_window_ms", "Окно ренжа, мс", "Сколько миллисекунд держать в rolling high/low диапазоне.", "30000", StrategyParamKind::Uint, 0u, true, false, true},
            StrategyParamMetadata{"range_bucket_ms", "Bucket ренжа, мс", "Размер временного bucket для сырого high/low перед сглаживанием.", "250", StrategyParamKind::Uint, 0u, true, false, true},
            StrategyParamMetadata{"range_smooth_shift", "Сглаживание ренжа, shift", "Integer smoothing: граница += (raw - граница) >> shift. 0 означает без сглаживания.", "3", StrategyParamKind::Uint, 0u, true, false, true},
            StrategyParamMetadata{"trend_lookback_ms", "Lookback тренда, мс", "Окно для диагностического trend score по наклону mid ренжа.", "5000", StrategyParamKind::Uint, 0u, true, false, true},
            StrategyParamMetadata{"entry_edge_pct", "Вход от края, %", "Покупать у нижнего края и шортить у верхнего края, если позиция цены внутри ренжа ближе этого процента к границе.", "20", StrategyParamKind::Uint, 0u, true, false, true},
            StrategyParamMetadata{"exit_neutral_band_pct", "Выход около mid, %", "Закрывать позицию reduce-only limit, когда цена возвращается в нейтральную зону вокруг середины ренжа.", "5", StrategyParamKind::Uint, 0u, true, false, true},
            StrategyParamMetadata{"min_range_bps", "Минимальный ренж, bps", "Не торговать слишком узкий диапазон.", "3", StrategyParamKind::Uint, 0u, true, false, true},
            StrategyParamMetadata{"max_spread_bps", "Максимальный spread, bps", "0 отключает фильтр; иначе стратегия не выставляется при большем spread.", "8", StrategyParamKind::Uint, 0u, true, false, true},
            StrategyParamMetadata{"post_only_offset_ticks", "Post-only offset, ticks", "На сколько ticks отступать от bid/ask для maker limit.", "0", StrategyParamKind::Uint, 0u, true, false, true},
            StrategyParamMetadata{"refresh_ms", "Минимальный интервал replace, мс", "0 означает обновлять лимитку сразу при новом сигнале.", "0", StrategyParamKind::Uint, 0u, true, false, true},
            StrategyParamMetadata{"amount_qty", "Размер в котируемой валюте", "Размер заявки как сумма в quote currency.", "200", StrategyParamKind::DecimalQuoteAmount, 1u, true, false, true},
            StrategyParamMetadata{"order_qty", "Размер в базовой валюте", "Фиксированное количество base asset. Используется вместо amount_qty.", "0.001", StrategyParamKind::DecimalAmount, 1u, true, false, false},
        },
        .paramGroupCount = 1u,
        .paramGroups = {StrategyParamGroupMetadata{1u, "sizing_mode"}},
        .indicatorCount = 1u,
        .indicators = {
            StrategyIndicatorMetadata{"range_position", "Range position"},
        },
    },
};

}  // namespace

const StrategyMetadata* findStrategyMetadata(std::string_view id) noexcept {
  for (const StrategyMetadata& metadata : kStrategies) {
    if (metadata.id && id == metadata.id) return &metadata;
  }
  return nullptr;
}

const StrategyMetadata* strategyMetadataAt(std::size_t index) noexcept {
  return index < strategyMetadataCount() ? &kStrategies[index] : nullptr;
}

std::size_t strategyMetadataCount() noexcept {
  return sizeof(kStrategies) / sizeof(kStrategies[0]);
}

}  // namespace hft_backtest
