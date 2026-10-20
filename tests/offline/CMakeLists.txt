cxet_add_offline_suite(hft_backtest_offline_quota OWNER hft-backtest EXPECTED 8
  SOURCES QuotaConsumerCases.cpp
  LIBRARIES hft_backtest::core hftrec::corpus_contract trading_core::strategy
  INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/../../src/Runtime/src"
  LABELS quota-consumer)
cxet_add_offline_suite(hft_backtest_offline_trade OWNER hft-backtest EXPECTED 29
  SOURCES TradeReplayCases.cpp
  LIBRARIES hft_backtest::core hftrec::corpus_contract trading_core::strategy
  INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/../../src/Runtime/src"
  LABELS trade replay)

cxet_add_offline_suite(hft_backtest_offline_quant OWNER hft-backtest EXPECTED 44
  SOURCES QuantReplayCases.cpp
  LIBRARIES hft_backtest::core hftrec::corpus_contract trading_core::strategy
  INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/../../src/Runtime/src"
  LABELS quant replay)

cxet_add_offline_suite(hft_backtest_offline_execution OWNER hft-backtest EXPECTED 20
  SOURCES ExecutionSimulationCases.cpp
  LIBRARIES hft_backtest::core hftrec::corpus_contract trading_core::strategy
  INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/../../src/Runtime/src"
  LABELS trade execution simulation)
