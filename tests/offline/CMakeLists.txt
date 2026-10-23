cxet_add_offline_suite(hft_backtest_offline_quota OWNER hft-backtest EXPECTED 8
  SOURCES Quota/QuotaConsumerCases.cpp
  LIBRARIES hft_backtest::core hftrec::corpus_contract trading_core::strategy
  INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/../../src/Runtime/src"
  LABELS quota-consumer)
cxet_add_offline_suite(hft_backtest_offline_trade OWNER hft-backtest EXPECTED 29
  SOURCES Replay/TradeReplayCases.cpp
  LIBRARIES hft_backtest::core hftrec::corpus_contract trading_core::strategy
  INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/../../src/Runtime/src"
  LABELS trade replay)

cxet_add_offline_suite(hft_backtest_offline_quant OWNER hft-backtest EXPECTED 44
  SOURCES Replay/QuantReplayCases.cpp
  LIBRARIES hft_backtest::core hftrec::corpus_contract trading_core::strategy
  INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/../../src/Runtime/src"
  LABELS quant replay)

cxet_add_offline_suite(hft_backtest_offline_execution OWNER hft-backtest EXPECTED 39
  SOURCES Execution/ExecutionSimulationCases.cpp
  LIBRARIES hft_backtest::core hftrec::corpus_contract trading_core::strategy
  INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/../../src/Runtime/src"
  LABELS trade execution simulation)
cxet_add_offline_suite(hft_backtest_offline_canonical_pair OWNER hft-backtest EXPECTED 11
  SOURCES Execution/CanonicalPairAdapterCases.cpp
  LIBRARIES hft_backtest::core hftrec::corpus_contract trading_core::strategy
  INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/../../src/Runtime/src"
  LABELS trade execution retirement)
cxet_add_offline_suite(hft_backtest_offline_research_tui OWNER hft-backtest EXPECTED 12
  SOURCES Research/ResearchTuiCases.cpp
    ../../src/Process/Tui/ResearchTuiModel.cpp
    ../../src/Process/Tui/ResearchTuiRendering.cpp
  LIBRARIES hft_backtest::research_client hftrec::service_client
  INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/../../src/Process/Tui"
  LABELS research tui)
cxet_add_offline_suite(hft_backtest_offline_research_service OWNER hft-backtest EXPECTED 15
  SOURCES Research/ResearchCases.cpp
  LIBRARIES hft_backtest::research_service Threads::Threads
  INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/../../src/Research/src"
  LABELS research persistence)
cxet_add_offline_suite(hft_backtest_offline_simulation_session OWNER hft-backtest EXPECTED 9
  SOURCES Simulation/SimulationSessionCases.cpp
  LIBRARIES hft_backtest::research_service hft_backtest::core hftrec::corpus_contract trading_core::strategy
  INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/../../src/Runtime/src"
  LABELS replay simulation session)
