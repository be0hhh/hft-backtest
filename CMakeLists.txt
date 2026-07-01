cmake_minimum_required(VERSION 3.22)
project(hft_backtest LANGUAGES CXX VERSION 0.1.0)

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "hft-backtest is Clang-only. Configure with -DCMAKE_CXX_COMPILER=clang++.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_SYSTEM_NAME STREQUAL Linux)
  message(FATAL_ERROR hft-backtest_is_Linux-only)
endif()

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING [[]] FORCE)
endif()

option(CXET_PORTABLE_BUILD "If ON: disable -march=native for hft-backtest Release builds." OFF)

set(HFT_BACKTEST_TRADER_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../hft-trader CACHE PATH [[Path to hft-trader app root]])
set(HFT_BACKTEST_TRADER_INCLUDE_DIR ${HFT_BACKTEST_TRADER_ROOT}/include CACHE PATH [[Path to hft-trader public headers]])
set(HFT_BACKTEST_TRADER_GENERATED_INCLUDE_DIR ${HFT_BACKTEST_TRADER_ROOT}/build/generated CACHE PATH [[Path to hft-trader generated headers]])
set(HFT_BACKTEST_TRADER_SHARED_LIB ${HFT_BACKTEST_TRADER_ROOT}/build/libhft_trader_runtime.so CACHE FILEPATH [[Path to prebuilt libhft_trader_runtime.so]])
set(HFT_BACKTEST_CXET_ROOT ${HFT_BACKTEST_TRADER_ROOT}/build/cxetcpp CACHE PATH [[Path to staged CXETCPP root]])

set(HFT_BACKTEST_CXET_INCLUDE_DIR ${HFT_BACKTEST_CXET_ROOT}/include)
set(HFT_BACKTEST_CXET_SHARED_LIB ${HFT_BACKTEST_CXET_ROOT}/lib/libcxet_lib.so)

if(NOT EXISTS ${HFT_BACKTEST_TRADER_SHARED_LIB})
  message(FATAL_ERROR [[Missing hft-trader shared runtime. Build hft-trader first.]])
endif()
if(NOT IS_DIRECTORY ${HFT_BACKTEST_TRADER_INCLUDE_DIR})
  message(FATAL_ERROR [[Missing hft-trader include dir]])
endif()
if(NOT IS_DIRECTORY ${HFT_BACKTEST_TRADER_GENERATED_INCLUDE_DIR})
  message(FATAL_ERROR [[Missing hft-trader generated include dir]])
endif()
if(NOT EXISTS ${HFT_BACKTEST_CXET_SHARED_LIB})
  message(FATAL_ERROR [[Missing staged CXETCPP lib. Build hft-trader first.]])
endif()
if(NOT IS_DIRECTORY ${HFT_BACKTEST_CXET_INCLUDE_DIR})
  message(FATAL_ERROR [[Missing staged CXETCPP include dir. Build hft-trader first.]])
endif()

add_library(cxet_lib SHARED IMPORTED GLOBAL)
set_target_properties(cxet_lib PROPERTIES
  IMPORTED_LOCATION ${HFT_BACKTEST_CXET_SHARED_LIB}
  INTERFACE_LINK_DIRECTORIES ${HFT_BACKTEST_CXET_ROOT}/lib
)
set_property(TARGET cxet_lib PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${HFT_BACKTEST_CXET_INCLUDE_DIR})
set_property(TARGET cxet_lib PROPERTY INTERFACE_LINK_OPTIONS $<$<PLATFORM_ID:Linux>:-Wl,-rpath-link,${HFT_BACKTEST_CXET_ROOT}/lib>)

add_library(hft_trader_runtime SHARED IMPORTED GLOBAL)
set(HFT_BACKTEST_TRADER_INTERFACE_INCLUDE_DIRS
  ${HFT_BACKTEST_TRADER_INCLUDE_DIR}
  ${HFT_BACKTEST_TRADER_ROOT}
  ${HFT_BACKTEST_TRADER_GENERATED_INCLUDE_DIR}
  ${HFT_BACKTEST_CXET_INCLUDE_DIR}
)
set_target_properties(hft_trader_runtime PROPERTIES
  IMPORTED_LOCATION ${HFT_BACKTEST_TRADER_SHARED_LIB}
  INTERFACE_LINK_DIRECTORIES ${HFT_BACKTEST_TRADER_ROOT}/build
)
set_property(TARGET hft_trader_runtime PROPERTY INTERFACE_INCLUDE_DIRECTORIES ${HFT_BACKTEST_TRADER_INTERFACE_INCLUDE_DIRS})
set_property(TARGET hft_trader_runtime PROPERTY INTERFACE_LINK_OPTIONS
  $<$<PLATFORM_ID:Linux>:-Wl,-rpath-link,${HFT_BACKTEST_TRADER_ROOT}/build>
  $<$<PLATFORM_ID:Linux>:-Wl,-rpath-link,${HFT_BACKTEST_CXET_ROOT}/lib>
)

find_package(Threads REQUIRED)
find_package(absl CONFIG REQUIRED)

add_library(hft_backtest_core SHARED
  src/accounting/backtest_accounting.cpp
  src/accounting/backtest_metrics.cpp
  src/accounting/backtest_result_aggregation.cpp
  src/config/backtest_run_config.cpp
  src/config/strategy_metadata.cpp
  src/engine/backtest_common.cpp
  src/engine/backtest_engine_setup.cpp
  src/engine/backtest_prepared.cpp
  src/engine/backtest_timeline.cpp
  src/execution/backtest_execution.cpp
  src/execution/backtest_execution_depth.cpp
  src/execution/backtest_execution_reprice.cpp
  src/execution/backtest_fill_model.cpp
  src/execution/backtest_latency.cpp
  src/input/session_loader.cpp
  src/results/backtest_result_writer.cpp
  src/runner/backtest.cpp
  src/strategy/backtest_strategy_runtime.cpp
  src/strategy/backtest_strategy_trace.cpp
  src/strategy/backtest_strategy_warmup.cpp
  src/sweep/backtest_sweep.cpp
  src/sweep/backtest_sweep_search.cpp
  src/sweep/backtest_sweep_writer.cpp
)
add_library(hft_backtest::core ALIAS hft_backtest_core)

target_include_directories(hft_backtest_core
  PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)
target_link_libraries(hft_backtest_core
  PUBLIC hft_trader_runtime cxet_lib Threads::Threads
  PRIVATE absl::flat_hash_map absl::inlined_vector
)
target_compile_features(hft_backtest_core PUBLIC cxx_std_23)
target_compile_definitions(hft_backtest_core PRIVATE
  HFT_BACKTEST_BUILDING_LIBRARY=1
  HFT_BACKTEST_TRADER_SOURCE_DIR="${HFT_BACKTEST_TRADER_ROOT}"
)
target_compile_options(hft_backtest_core PRIVATE
  $<$<CONFIG:Release>:-O3>
  $<$<AND:$<CONFIG:Release>,$<NOT:$<BOOL:${CXET_PORTABLE_BUILD}>>>:-march=native>
)
set_target_properties(hft_backtest_core PROPERTIES
  OUTPUT_NAME hft_backtest_core
  LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
set_property(TARGET hft_backtest_core PROPERTY BUILD_RPATH
  $ORIGIN
  ${HFT_BACKTEST_TRADER_ROOT}/build
  ${HFT_BACKTEST_CXET_ROOT}/lib
)

add_executable(hft-backtest tools/hft_backtest.cpp)
target_link_libraries(hft-backtest PRIVATE hft_backtest_core)
target_compile_features(hft-backtest PRIVATE cxx_std_23)
set_target_properties(hft-backtest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
set_property(TARGET hft-backtest PROPERTY BUILD_RPATH
  $ORIGIN
  ${HFT_BACKTEST_TRADER_ROOT}/build
  ${HFT_BACKTEST_CXET_ROOT}/lib
)

add_executable(hft-backtest-execution-tests tests/execution/backtest_execution_tests.cpp)
target_include_directories(hft-backtest-execution-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hft-backtest-execution-tests PRIVATE hft_backtest_core)
target_compile_features(hft-backtest-execution-tests PRIVATE cxx_std_23)
set_target_properties(hft-backtest-execution-tests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
set_property(TARGET hft-backtest-execution-tests PROPERTY BUILD_RPATH
  $ORIGIN
  ${HFT_BACKTEST_TRADER_ROOT}/build
  ${HFT_BACKTEST_CXET_ROOT}/lib
)

add_executable(hft-backtest-phase2-maker-queue-tests tests/execution/backtest_phase2_maker_queue_tests.cpp)
target_include_directories(hft-backtest-phase2-maker-queue-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hft-backtest-phase2-maker-queue-tests PRIVATE hft_backtest_core)
target_compile_features(hft-backtest-phase2-maker-queue-tests PRIVATE cxx_std_23)
set_target_properties(hft-backtest-phase2-maker-queue-tests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
set_property(TARGET hft-backtest-phase2-maker-queue-tests PROPERTY BUILD_RPATH
  $ORIGIN
  ${HFT_BACKTEST_TRADER_ROOT}/build
  ${HFT_BACKTEST_CXET_ROOT}/lib
)

add_executable(hft-backtest-passive-bbo-touch-tests tests/execution/backtest_passive_bbo_touch_tests.cpp)
target_include_directories(hft-backtest-passive-bbo-touch-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hft-backtest-passive-bbo-touch-tests PRIVATE hft_backtest_core)
target_compile_features(hft-backtest-passive-bbo-touch-tests PRIVATE cxx_std_23)
set_target_properties(hft-backtest-passive-bbo-touch-tests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
set_property(TARGET hft-backtest-passive-bbo-touch-tests PROPERTY BUILD_RPATH
  $ORIGIN
  ${HFT_BACKTEST_TRADER_ROOT}/build
  ${HFT_BACKTEST_CXET_ROOT}/lib
)

add_executable(hft-backtest-stop-limit-tests tests/execution/backtest_stop_limit_tests.cpp)
target_include_directories(hft-backtest-stop-limit-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hft-backtest-stop-limit-tests PRIVATE hft_backtest_core)
target_compile_features(hft-backtest-stop-limit-tests PRIVATE cxx_std_23)
set_target_properties(hft-backtest-stop-limit-tests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
set_property(TARGET hft-backtest-stop-limit-tests PROPERTY BUILD_RPATH
  $ORIGIN
  ${HFT_BACKTEST_TRADER_ROOT}/build
  ${HFT_BACKTEST_CXET_ROOT}/lib
)

add_executable(hft-backtest-rate-limit-tests tests/execution/backtest_rate_limit_tests.cpp)
target_include_directories(hft-backtest-rate-limit-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hft-backtest-rate-limit-tests PRIVATE hft_backtest_core)
target_compile_features(hft-backtest-rate-limit-tests PRIVATE cxx_std_23)
set_target_properties(hft-backtest-rate-limit-tests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
set_property(TARGET hft-backtest-rate-limit-tests PROPERTY BUILD_RPATH
  $ORIGIN
  ${HFT_BACKTEST_TRADER_ROOT}/build
  ${HFT_BACKTEST_CXET_ROOT}/lib
)

add_executable(hft-backtest-multileg-context-tests tests/execution/backtest_multileg_context_tests.cpp)
target_include_directories(hft-backtest-multileg-context-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hft-backtest-multileg-context-tests PRIVATE hft_backtest_core)
target_compile_features(hft-backtest-multileg-context-tests PRIVATE cxx_std_23)
set_target_properties(hft-backtest-multileg-context-tests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
set_property(TARGET hft-backtest-multileg-context-tests PROPERTY BUILD_RPATH
  $ORIGIN
  ${HFT_BACKTEST_TRADER_ROOT}/build
  ${HFT_BACKTEST_CXET_ROOT}/lib
)

add_executable(hft-backtest-stat-arb-replay-tests tests/replay/stat_arb_edge_maker_replay_tests.cpp)
target_link_libraries(hft-backtest-stat-arb-replay-tests PRIVATE hft_backtest_core)
target_compile_features(hft-backtest-stat-arb-replay-tests PRIVATE cxx_std_23)
set_target_properties(hft-backtest-stat-arb-replay-tests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
set_property(TARGET hft-backtest-stat-arb-replay-tests PROPERTY BUILD_RPATH
  $ORIGIN
  ${HFT_BACKTEST_TRADER_ROOT}/build
  ${HFT_BACKTEST_CXET_ROOT}/lib
)

add_executable(hft-backtest-uptick-replay-tests tests/replay/uptick_replay_tests.cpp)
target_link_libraries(hft-backtest-uptick-replay-tests PRIVATE hft_backtest_core)
target_compile_features(hft-backtest-uptick-replay-tests PRIVATE cxx_std_23)
set_target_properties(hft-backtest-uptick-replay-tests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
set_property(TARGET hft-backtest-uptick-replay-tests PROPERTY BUILD_RPATH
  $ORIGIN
  ${HFT_BACKTEST_TRADER_ROOT}/build
  ${HFT_BACKTEST_CXET_ROOT}/lib
)

add_executable(hft-backtest-strategy-indicator-tests tests/strategy/backtest_strategy_indicator_tests.cpp)
target_link_libraries(hft-backtest-strategy-indicator-tests PRIVATE hft_backtest_core)
target_compile_features(hft-backtest-strategy-indicator-tests PRIVATE cxx_std_23)
set_target_properties(hft-backtest-strategy-indicator-tests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
set_property(TARGET hft-backtest-strategy-indicator-tests PROPERTY BUILD_RPATH
  $ORIGIN
  ${HFT_BACKTEST_TRADER_ROOT}/build
  ${HFT_BACKTEST_CXET_ROOT}/lib
)

add_executable(hft-backtest-metrics-tests tests/metrics/backtest_metrics_tests.cpp)
target_include_directories(hft-backtest-metrics-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hft-backtest-metrics-tests PRIVATE hft_backtest_core)
target_compile_features(hft-backtest-metrics-tests PRIVATE cxx_std_23)
set_target_properties(hft-backtest-metrics-tests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
set_property(TARGET hft-backtest-metrics-tests PROPERTY BUILD_RPATH
  $ORIGIN
  ${HFT_BACKTEST_TRADER_ROOT}/build
  ${HFT_BACKTEST_CXET_ROOT}/lib
)

add_executable(hft-backtest-accounting-tests tests/accounting/backtest_accounting_tests.cpp)
target_include_directories(hft-backtest-accounting-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hft-backtest-accounting-tests PRIVATE hft_backtest_core)
target_compile_features(hft-backtest-accounting-tests PRIVATE cxx_std_23)
set_target_properties(hft-backtest-accounting-tests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
set_property(TARGET hft-backtest-accounting-tests PROPERTY BUILD_RPATH
  $ORIGIN
  ${HFT_BACKTEST_TRADER_ROOT}/build
  ${HFT_BACKTEST_CXET_ROOT}/lib
)

add_executable(hft-backtest-session-loader-tests tests/input/session_loader_tests.cpp)
target_include_directories(hft-backtest-session-loader-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hft-backtest-session-loader-tests PRIVATE hft_backtest_core)
target_compile_features(hft-backtest-session-loader-tests PRIVATE cxx_std_23)
set_target_properties(hft-backtest-session-loader-tests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
set_property(TARGET hft-backtest-session-loader-tests PROPERTY BUILD_RPATH
  $ORIGIN
  ${HFT_BACKTEST_TRADER_ROOT}/build
  ${HFT_BACKTEST_CXET_ROOT}/lib
)

add_executable(hft-backtest-sweep-tests tests/sweep/backtest_sweep_tests.cpp)
target_include_directories(hft-backtest-sweep-tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hft-backtest-sweep-tests PRIVATE hft_backtest_core)
target_compile_features(hft-backtest-sweep-tests PRIVATE cxx_std_23)
set_target_properties(hft-backtest-sweep-tests PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
set_property(TARGET hft-backtest-sweep-tests PROPERTY BUILD_RPATH
  $ORIGIN
  ${HFT_BACKTEST_TRADER_ROOT}/build
  ${HFT_BACKTEST_CXET_ROOT}/lib
)
