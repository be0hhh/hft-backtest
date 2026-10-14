#include "BacktestStrategyInstrument.hpp"
#include "trading_core/Algorithm/Math/PriceQuantity/PriceNormalization.hpp"
namespace hft_backtest {
Status prepareBacktestStrategyInstrument(const detail::InstrumentMetadata& metadata,
    trading_core::StrategyInstrumentSpec& instrument, std::string& error) {
  // FillPositionAccounting uses per-base E8 prices. Native orders and feeds
  // cannot be silently repriced into that currency basis by this adapter.
  if (metadata.priceBasisQtyE8 != 0 && metadata.priceBasisQtyE8 != trading_core::kPriceBasisScaleRaw) {
    error = "Backtest accounting does not support a nonunit native price basis";
    return Status::Unsupported;
  }
  if (!metadata.hasExecutionQuantityGrid || metadata.executionQuantityStepE8 <= 0) {
    error = "captured metadata has no genuine canonical execution quantity grid";
    return Status::Unsupported;
  }
  trading_core::StrategyInstrumentSpec candidate{};
  // Older captures without typed currency/FX retain unknown valuation.
  // A symbol spelling or corpus-local asset ID is not conversion authority.
  candidate.quoteCurrency = metadata.quoteCurrency;
  candidate.quoteToPositionLimitE8 = metadata.quoteToPositionLimitE8;
  candidate.tickSizeRaw = metadata.tickSizeE8;
  candidate.stepSizeRaw = metadata.lotSizeE8;
  candidate.executionQuantityStepRaw = metadata.executionQuantityStepE8;
  candidate.contractBaseQtyRaw = metadata.contractBaseQtyE8;
  candidate.priceBasisQtyRaw = metadata.priceBasisQtyE8 > 0 ? metadata.priceBasisQtyE8 : trading_core::kPriceBasisScaleRaw;
  candidate.expiryUtcNs = metadata.expiryUtcNs > 0 ? static_cast<std::uint64_t>(metadata.expiryUtcNs) : 0u;
  if (!trading_core::prepareStrategyInstrumentSpecDerived(candidate)) {
    error = "captured canonical execution quantity grid cannot be prepared exactly";
    return Status::Unsupported;
  }
  instrument = candidate;
  return Status::Ok;
}
}
