cxet_add_offline_suite(hft_backtest_offline_quota OWNER hft-backtest EXPECTED 3
  SOURCES QuotaConsumerCases.cpp
  LIBRARIES hft_backtest::core
  INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/../../src/Runtime/src"
  LABELS quota-consumer)
