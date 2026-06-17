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
            StrategyParamMetadata{"entry_edge_bps", "Запас входа, bps", "Signed edge от cost boundary: плюс = дальше снаружи границы, минус = ближе внутри границы.", "0", StrategyParamKind::DecimalQuoteAmount, 0u, true, false, true},
            StrategyParamMetadata{"exit_edge_bps", "Запас выхода, bps", "Signed edge от close boundary: плюс = дальше снаружи границы, минус = ближе внутри границы.", "0", StrategyParamKind::DecimalQuoteAmount, 0u, true, false, true},
            StrategyParamMetadata{"refresh_ms", "Минимальный интервал, мс", "0 означает разрешить новый вход/выход сразу на следующем сигнале.", "0", StrategyParamKind::Uint, 0u, true, false, true},
            StrategyParamMetadata{"max_position_pairs", "Максимум пар", "v1 поддерживает одну открытую пару; параметр оставлен для явного конфига.", "1", StrategyParamKind::Uint, 0u, true, true, true},
            StrategyParamMetadata{"amount_qty", "Размер в котируемой валюте", "USD/quote notional для расчета общего base qty по самой дорогой ноге с округлением до общего лота обеих ног.", "10", StrategyParamKind::DecimalQuoteAmount, 1u, true, false, true},
            StrategyParamMetadata{"order_qty", "Размер в базовой валюте", "Фиксированное количество base asset для каждой ноги.", "0.001", StrategyParamKind::DecimalAmount, 1u, true, false, false},
        },
        .paramGroupCount = 1u,
        .paramGroups = {StrategyParamGroupMetadata{1u, "sizing_mode"}},
    },
    StrategyMetadata{
        .id = "stat_arb_band",
        .displayNameRu = "Стат-арбитраж band",
        .configTemplateFile = "stat_arb_band.ini",
        .minSessionCount = 2u,
        .maxSessionCount = 2u,
        .paramCount = 10u,
        .params = {
            StrategyParamMetadata{"ema_window_ms", "Окно EMA, мс", "Окно EMA центральной линии spread band.", "5000", StrategyParamKind::Uint, 0u, true, false, true},
            StrategyParamMetadata{"entry_edge_bps", "Запас входа, bps", "Signed edge от band boundary: плюс = дальше снаружи серой границы, минус = ближе внутри серой границы.", "0", StrategyParamKind::DecimalQuoteAmount, 0u, true, false, true},
            StrategyParamMetadata{"exit_edge_bps", "Запас выхода, bps", "Signed edge от противоположной band boundary: плюс = дальше снаружи серой границы, минус = ближе внутри серой границы.", "0", StrategyParamKind::DecimalQuoteAmount, 0u, true, false, true},
            StrategyParamMetadata{"refresh_ms", "Минимальный интервал входа, мс", "0 означает разрешить новый вход сразу на следующем сигнале; выход не блокируется refresh guard.", "0", StrategyParamKind::Uint, 0u, true, false, true},
            StrategyParamMetadata{"pending_timeout_ms", "Timeout pending, мс", "0 отключает timeout; небезопасно для live, если возможны partial fills. При timeout стратегия различает flat, still hedged и unhedged.", "0", StrategyParamKind::Uint, 0u, true, false, true},
            StrategyParamMetadata{"max_quote_age_ms", "Макс. возраст котировки, мс", "0 отключает guard. Если включено, стратегия пропускает сигнал при слишком старой котировке одной из ног.", "0", StrategyParamKind::Uint, 0u, true, false, true},
            StrategyParamMetadata{"max_leg_skew_ms", "Макс. skew ног, мс", "0 отключает guard. Если включено, стратегия пропускает сигнал при слишком большой разнице timestamp между ногами.", "0", StrategyParamKind::Uint, 0u, true, false, true},
            StrategyParamMetadata{"max_position_pairs", "Максимум пар", "v1 поддерживает одну открытую пару; параметр оставлен для явного конфига.", "1", StrategyParamKind::Uint, 0u, true, true, true},
            StrategyParamMetadata{"amount_qty", "Размер в котируемой валюте", "USD/quote notional для расчета общего base qty по самой дорогой ноге с округлением до общего лота обеих ног.", "10", StrategyParamKind::DecimalQuoteAmount, 1u, true, false, true},
            StrategyParamMetadata{"order_qty", "Размер в базовой валюте", "Фиксированное количество base asset для каждой ноги.", "0.001", StrategyParamKind::DecimalAmount, 1u, true, false, false},
        },
        .paramGroupCount = 1u,
        .paramGroups = {StrategyParamGroupMetadata{1u, "sizing_mode"}},
    },
    StrategyMetadata{
        .id = "stat_arb_boundary_maker",
        .displayNameRu = "Стат-арбитраж boundary maker",
        .configTemplateFile = "stat_arb_boundary_maker.ini",
        .minSessionCount = 2u,
        .maxSessionCount = 2u,
        .paramCount = 14u,
        .params = {
            StrategyParamMetadata{"ema_window_ms", "Окно EMA, мс", "Окно EMA центральной линии spread boundary.", "5000", StrategyParamKind::Uint, 0u, true, false, true},
            StrategyParamMetadata{"maker_entry_edge_bps", "Maker edge входа, bps", "Минимальный запас для пассивной boundary-лимитки до учета slippage buffer.", "3", StrategyParamKind::DecimalQuoteAmount, 0u, true, false, true},
            StrategyParamMetadata{"maker_slippage_buffer_bps", "Slippage buffer, bps", "Дополнительный запас к maker boundary под taker hedge и прострел.", "2", StrategyParamKind::DecimalQuoteAmount, 0u, true, false, true},
            StrategyParamMetadata{"maker_offset_ticks", "Maker offset, ticks", "На сколько ticks отодвигать boundary-лимитку от расчетной цены.", "0", StrategyParamKind::Uint, 0u, true, false, true},
            StrategyParamMetadata{"maker_replace_threshold_ticks", "Порог replace, ticks", "Минимальный сдвиг цены в ticks для cancel+new replacement лимитки. 0 заменяет при любом изменении.", "1", StrategyParamKind::Uint, 0u, true, false, true},
            StrategyParamMetadata{"maker_ttl_ms", "TTL лимитки, мс", "Принудительный cancel+new replacement после этого возраста. 0 отключает TTL.", "500", StrategyParamKind::Uint, 0u, true, false, true},
            StrategyParamMetadata{"exit_edge_bps", "Запас выхода, bps", "Signed edge от противоположной spread boundary для taker-taker закрытия.", "-5", StrategyParamKind::DecimalQuoteAmount, 0u, true, false, true},
            StrategyParamMetadata{"refresh_ms", "Минимальный интервал входа, мс", "0 означает разрешить перекотировку сразу на следующем сигнале.", "0", StrategyParamKind::Uint, 0u, true, false, true},
            StrategyParamMetadata{"pending_timeout_ms", "Timeout pending, мс", "0 отключает timeout; после maker fill hedge идет собственной taker-веткой стратегии.", "0", StrategyParamKind::Uint, 0u, true, false, true},
            StrategyParamMetadata{"max_quote_age_ms", "Макс. возраст котировки, мс", "0 отключает guard. Если включено, стратегия не котирует по старой котировке одной из ног.", "0", StrategyParamKind::Uint, 0u, true, false, true},
            StrategyParamMetadata{"max_leg_skew_ms", "Макс. skew ног, мс", "0 отключает guard. Если включено, стратегия не котирует при большом timestamp skew между ногами.", "0", StrategyParamKind::Uint, 0u, true, false, true},
            StrategyParamMetadata{"max_position_pairs", "Максимум пар", "v1 поддерживает одну открытую пару; параметр оставлен для явного конфига.", "1", StrategyParamKind::Uint, 0u, true, true, true},
            StrategyParamMetadata{"amount_qty", "Размер в котируемой валюте", "USD/quote notional для расчета общего base qty по самой дорогой ноге с округлением до общего лота обеих ног.", "10", StrategyParamKind::DecimalQuoteAmount, 1u, true, false, true},
            StrategyParamMetadata{"order_qty", "Размер в базовой валюте", "Фиксированное количество base asset для каждой ноги.", "0.001", StrategyParamKind::DecimalAmount, 1u, true, false, false},
        },
        .paramGroupCount = 1u,
        .paramGroups = {StrategyParamGroupMetadata{1u, "sizing_mode"}},
    },
    StrategyMetadata{
        .id = "stat_arb_band_ladder",
        .displayNameRu = "Стат-арбитраж band ladder",
        .configTemplateFile = "stat_arb_band_ladder.ini",
        .minSessionCount = 2u,
        .maxSessionCount = 2u,
        .paramCount = 12u,
        .params = {
            StrategyParamMetadata{"ema_window_ms", "Окно EMA, мс", "Окно EMA центральной линии spread band.", "5000", StrategyParamKind::Uint, 0u, true, false, true},
            StrategyParamMetadata{"entry_edge_bps", "Запас входа, bps", "Первый threshold по edge_after_cost от band boundary.", "0", StrategyParamKind::DecimalQuoteAmount, 0u, true, false, true},
            StrategyParamMetadata{"exit_edge_bps", "Запас выхода, bps", "Signed edge от противоположной band boundary для полного reduce-only выхода.", "0", StrategyParamKind::DecimalQuoteAmount, 0u, true, false, true},
            StrategyParamMetadata{"refresh_ms", "Минимальный интервал входа, мс", "0 означает разрешить новый вход или ladder add сразу на следующем сигнале; выход не блокируется refresh guard.", "0", StrategyParamKind::Uint, 0u, true, false, true},
            StrategyParamMetadata{"pending_timeout_ms", "Timeout pending, мс", "0 отключает timeout; небезопасно для live, если возможны partial fills. При timeout стратегия различает flat, still hedged и unhedged.", "0", StrategyParamKind::Uint, 0u, true, false, true},
            StrategyParamMetadata{"max_quote_age_ms", "Макс. возраст котировки, мс", "0 отключает guard. Если включено, стратегия пропускает вход/add при слишком старой котировке одной из ног.", "0", StrategyParamKind::Uint, 0u, true, false, true},
            StrategyParamMetadata{"max_leg_skew_ms", "Макс. skew ног, мс", "0 отключает guard. Если включено, стратегия пропускает вход/add при слишком большой разнице timestamp между ногами.", "0", StrategyParamKind::Uint, 0u, true, false, true},
            StrategyParamMetadata{"max_position_pairs", "Максимум пар", "Оставлен совместимым с stat_arb_band; ladder-size задается через max_ladder_levels.", "1", StrategyParamKind::Uint, 0u, true, true, true},
            StrategyParamMetadata{"ladder_step_bps", "Шаг лесенки, bps", "Дистанция между дополнительными threshold по edge_after_cost после первого входа.", "0", StrategyParamKind::DecimalQuoteAmount, 0u, true, false, true},
            StrategyParamMetadata{"max_ladder_levels", "Максимум уровней лесенки", "Максимальное число entry-пар, включая первую. Значение 1 сохраняет single-entry поведение.", "1", StrategyParamKind::Uint, 0u, true, false, true},
            StrategyParamMetadata{"amount_qty", "Размер в котируемой валюте", "USD/quote notional для расчета per-level base qty по самой дорогой ноге с округлением до общего лота обеих ног.", "10", StrategyParamKind::DecimalQuoteAmount, 1u, true, false, true},
            StrategyParamMetadata{"order_qty", "Размер в базовой валюте", "Фиксированное per-level количество base asset для каждой ноги.", "0.001", StrategyParamKind::DecimalAmount, 1u, true, false, false},
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
