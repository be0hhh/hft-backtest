# Producer-owned research clients carry neither simulation nor session authority.
add_library(hft_backtest_research_client STATIC
  src/ResearchCodec.cpp
  src/ResearchStorage.cpp
  src/ResearchSpec.cpp
  src/ResearchClient.cpp)
add_library(hft_backtest::research_client ALIAS hft_backtest_research_client)
target_include_directories(hft_backtest_research_client PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_features(hft_backtest_research_client PUBLIC cxx_std_23)
target_link_libraries(hft_backtest_research_client
  PUBLIC cxet::read_only_contracts hftrec::corpus_contract PRIVATE Threads::Threads)

add_library(hft_backtest_research_service STATIC
  src/ResearchRegistry.cpp
  src/ResearchServer.cpp
  src/ResearchService.cpp
  src/ResearchStudy.cpp
  src/ResearchMidpriceStudy.cpp
  src/ResearchDaemon.cpp)
add_library(hft_backtest::research_service ALIAS hft_backtest_research_service)
target_compile_features(hft_backtest_research_service PUBLIC cxx_std_23)
target_link_libraries(hft_backtest_research_service
  PUBLIC hft_backtest::research_client
  PRIVATE hft_backtest::core hftrec::service_client hftrec::reference_contract Threads::Threads)

add_executable(hft-backtestd ../Process/Research/ResearchMain.cpp)
target_link_libraries(hft-backtestd PRIVATE hft_backtest::research_service)
target_compile_features(hft-backtestd PRIVATE cxx_std_23)
set_target_properties(hft-backtestd PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

add_executable(hft-backtest-tui
  ../Process/Tui/Main.cpp
  ../Process/Tui/ResearchTuiApp.cpp
  ../Process/Tui/ResearchTuiModel.cpp
  ../Process/Tui/ResearchTuiRendering.cpp
  ../Process/Tui/Terminal.cpp)
target_compile_features(hft-backtest-tui PRIVATE cxx_std_23)
target_link_libraries(hft-backtest-tui PRIVATE
  hft_backtest::research_client hftrec::service_client Threads::Threads)
set_target_properties(hft-backtest-tui PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
