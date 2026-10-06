cxet_add_offline_suite(hft_backtest_offline_quota OWNER hft-backtest EXPECTED 5
  SOURCES QuotaConsumerCases.cpp
  LIBRARIES hft_backtest::core
  INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/../../src/Runtime/src"
  LABELS quota-consumer)
cxet_add_offline_suite(hft_backtest_offline_trade OWNER hft-backtest EXPECTED 28
  SOURCES TradeReplayCases.cpp
  LIBRARIES hft_backtest::core
  INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/../../src/Runtime/src"
  LABELS trade replay)

cxet_add_offline_suite(hft_backtest_offline_quant OWNER hft-backtest EXPECTED 41
  SOURCES QuantReplayCases.cpp
  LIBRARIES hft_backtest::core
  INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/../../src/Runtime/src"
  LABELS quant replay)
