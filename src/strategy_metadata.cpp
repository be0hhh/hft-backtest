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
        .paramCount = 7u,
        .params = {
            StrategyParamMetadata{"distance_bps", "Дистанция котировки, bps", "На сколько базисных пунктов отступать от текущего bid/ask при выставлении лимитных заявок.", "250", StrategyParamKind::Uint, 0u, true, false, true},
            StrategyParamMetadata{"trigger_bps", "Триггер перекотировки, bps", "Минимальное движение базового bid/ask в bps, после которого стратегия пересчитывает и заменяет котировку.", "10", StrategyParamKind::Uint, 0u, true, false, true},
            StrategyParamMetadata{"refresh_ms", "Минимальный интервал обновления, мс", "Защита от слишком частой перекотировки. 0 означает обновлять сразу при выполнении триггера.", "0", StrategyParamKind::Uint, 0u, true, false, true},
            StrategyParamMetadata{"close_delay_us", "Задержка закрытия, мкс", "Пауза перед reduce-only market close после появления позиции.", "2000000", StrategyParamKind::Uint, 0u, false, true, true},
            StrategyParamMetadata{"start_ms", "Стартовая задержка, мс", "Задержка перед первичным seed/order действием стратегии. 0 означает без задержки.", "0", StrategyParamKind::Uint, 0u, false, true, true},
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
