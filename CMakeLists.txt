cmake_minimum_required(VERSION 3.22)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  message(FATAL_ERROR
    "hft-backtest standalone configure is unavailable: CXET root graph required")
endif()

if(NOT DEFINED CXET_ROOT_SOURCE_GRAPH OR NOT CXET_ROOT_SOURCE_GRAPH)
  message(FATAL_ERROR
    "hft-backtest integration is invalid: CXET root graph required (CXET_ROOT_SOURCE_GRAPH is not enabled)")
endif()

foreach(required_target IN ITEMS
    cxet::cxet_lib
    trading_core::algorithm
    trading_core::execution
    trading_core::strategy
    hftrec::corpus_contract
    absl::flat_hash_map
    absl::inlined_vector)
  if(NOT TARGET ${required_target})
    message(FATAL_ERROR
      "hft-backtest integration is invalid: missing root target ${required_target}")
  endif()
endforeach()

if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR
    "hft-backtest is Clang-only. Configure the CXET root with clang++.")
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL Linux)
  message(FATAL_ERROR "hft-backtest is Linux-only")
endif()

option(CXET_PORTABLE_BUILD
  "If ON: disable -march=native for hft-backtest Release builds." OFF)

if(CXET_SANITIZER_PROFILE STREQUAL "address-undefined")
  add_compile_options(
    -O2 -g3 -fsanitize=address,undefined
    -fsanitize-address-use-after-scope -fno-omit-frame-pointer
    -fno-optimize-sibling-calls -fno-sanitize-recover=all)
  add_link_options(
    -fsanitize=address,undefined -fno-omit-frame-pointer
    -fno-sanitize-recover=all)
elseif(CXET_SANITIZER_PROFILE STREQUAL "thread")
  add_compile_options(
    -O2 -g3 -fsanitize=thread -fno-omit-frame-pointer
    -fno-optimize-sibling-calls)
  add_link_options(-fsanitize=thread -fno-omit-frame-pointer)
endif()

find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

set(HFT_BACKTEST_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(HFT_BACKTEST_GENERATED_STRATEGY_METADATA
  "${HFT_BACKTEST_GENERATED_DIR}/hft_backtest/generated/StrategyMetadata.hpp")
file(GLOB_RECURSE HFT_BACKTEST_STRATEGY_PROFILES CONFIGURE_DEPENDS
  LIST_DIRECTORIES false
  "${CMAKE_CURRENT_LIST_DIR}/../trading-core/StrategyProfiles/*.ini")
list(SORT HFT_BACKTEST_STRATEGY_PROFILES)
if(NOT HFT_BACKTEST_STRATEGY_PROFILES)
  message(FATAL_ERROR
    "hft-backtest integration is invalid: trading-core has no canonical strategy profiles")
endif()
get_target_property(HFT_BACKTEST_REQUIRED_STRATEGY_NAMES
  trading_core_strategy TRADING_CORE_REGISTERED_STRATEGY_NAMES)
if(NOT HFT_BACKTEST_REQUIRED_STRATEGY_NAMES OR
   HFT_BACKTEST_REQUIRED_STRATEGY_NAMES MATCHES "-NOTFOUND$")
  message(FATAL_ERROR
    "hft-backtest integration is invalid: trading-core registered no canonical strategies")
endif()
set(HFT_BACKTEST_REQUIRED_STRATEGY_ARGS)
foreach(required_strategy IN LISTS HFT_BACKTEST_REQUIRED_STRATEGY_NAMES)
  list(APPEND HFT_BACKTEST_REQUIRED_STRATEGY_ARGS
    --required-strategy "${required_strategy}")
endforeach()
add_custom_command(
  OUTPUT "${HFT_BACKTEST_GENERATED_STRATEGY_METADATA}"
  COMMAND "${Python3_EXECUTABLE}"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/GenerateStrategyMetadata.py"
    --profile-root "${CMAKE_CURRENT_LIST_DIR}/../trading-core/StrategyProfiles"
    --output "${HFT_BACKTEST_GENERATED_STRATEGY_METADATA}"
    ${HFT_BACKTEST_REQUIRED_STRATEGY_ARGS}
    ${HFT_BACKTEST_STRATEGY_PROFILES}
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/GenerateStrategyMetadata.py"
    ${HFT_BACKTEST_STRATEGY_PROFILES}
  COMMENT "Generating bounded hft-backtest strategy metadata"
  VERBATIM
)
add_custom_target(hft_backtest_generate_strategy_metadata
  DEPENDS "${HFT_BACKTEST_GENERATED_STRATEGY_METADATA}")

add_library(hft_backtest_core SHARED
  src/Runtime/src/Accounting/BacktestAccounting.cpp
  src/Runtime/src/Accounting/BacktestMetrics.cpp
  src/Runtime/src/Accounting/BacktestResultAggregation.cpp
  src/Runtime/src/Config/BacktestRunConfig.cpp
  src/Runtime/src/Config/BacktestStrategyConfig.cpp
  src/Runtime/src/Config/StrategyMetadata.cpp
  "${HFT_BACKTEST_GENERATED_STRATEGY_METADATA}"
  src/Runtime/src/BacktestCommon.cpp
  src/Runtime/src/BacktestEngineSetup.cpp
  src/Runtime/src/BacktestPrepared.cpp
  src/Runtime/src/BacktestReplayClock.cpp
  src/Runtime/src/BacktestTimeline.cpp
  src/Runtime/src/Execution/BacktestExecution.cpp
  src/Runtime/src/Execution/BacktestExecutionDepth.cpp
  src/Runtime/src/Execution/BacktestFillModel.cpp
  src/Runtime/src/Execution/BacktestLatency.cpp
  src/Runtime/src/Input/BinaryCorpusLoader.cpp
  src/Runtime/src/Input/SessionLoader.cpp
  src/Runtime/src/Results/BacktestResultWriter.cpp
  src/Runtime/src/Backtest.cpp
  src/Runtime/src/Strategy/BacktestStrategyIntentLowering.cpp
  src/Runtime/src/Strategy/BacktestStrategyInputFreshness.cpp
  src/Runtime/src/Strategy/BacktestStrategyRuntime.cpp
  src/Runtime/src/Strategy/BacktestStrategyTrace.cpp
  src/Runtime/src/Strategy/BacktestStrategyWarmup.cpp
  src/Runtime/src/Sweep/BacktestSweep.cpp
  src/Runtime/src/Sweep/BacktestSweepSearch.cpp
  src/Runtime/src/Sweep/BacktestSweepWriter.cpp
)
add_library(hft_backtest::core ALIAS hft_backtest_core)

target_include_directories(hft_backtest_core
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/Runtime/include"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Runtime/src"
    "${HFT_BACKTEST_GENERATED_DIR}"
)
add_dependencies(hft_backtest_core hft_backtest_generate_strategy_metadata)
target_link_libraries(hft_backtest_core
  PUBLIC
    cxet::cxet_lib
    trading_core::execution
  PRIVATE
    trading_core::algorithm
    trading_core::strategy
    hftrec::corpus_contract
    Threads::Threads
    absl::flat_hash_map
    absl::inlined_vector
)
target_compile_features(hft_backtest_core PUBLIC cxx_std_23)
target_compile_definitions(hft_backtest_core
  PRIVATE
    HFT_BACKTEST_BUILDING_LIBRARY=1
  PUBLIC HFT_BACKTEST_SHARED=1
)
target_compile_options(hft_backtest_core PRIVATE
  $<$<CONFIG:Release>:-O3>
  $<$<AND:$<CONFIG:Release>,$<NOT:$<BOOL:${CXET_PORTABLE_BUILD}>>>:-march=native>
)
set_target_properties(hft_backtest_core PROPERTIES
  OUTPUT_NAME hft_backtest_core
  VERSION 0.2.0
  SOVERSION 3
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

add_executable(hft-backtest src/Process/HftBacktest.cpp)
target_link_libraries(hft-backtest PRIVATE hft_backtest_core)
target_compile_features(hft-backtest PRIVATE cxx_std_23)
set_target_properties(hft-backtest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

function(add_hft_backtest_test target source)
  add_executable(${target} ${source})
  target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/Runtime/src")
  target_link_libraries(${target}
    PRIVATE hft_backtest_core trading_core::strategy
            hftrec::corpus_contract ${ARGN})
  target_compile_features(${target} PRIVATE cxx_std_23)
  set_target_properties(${target} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
  )
  add_test(NAME ${target} COMMAND "$<TARGET_FILE:${target}>")
  set_tests_properties(${target} PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    LABELS "cxet-family;hft-backtest;contract")
endfunction()

add_hft_backtest_test(hft-backtest-execution-tests
  tests/Execution/BacktestExecutionTests.cpp)
add_hft_backtest_test(hft-backtest-client-order-id-tests
  tests/Execution/BacktestClientOrderIdTests.cpp)
add_hft_backtest_test(hft-backtest-replace-lifecycle-tests
  tests/Execution/BacktestReplaceLifecycleTests.cpp)
add_hft_backtest_test(hft-backtest-stat-arb-late-fill-lifecycle-tests
  tests/Execution/BacktestStatArbLateFillLifecycleTests.cpp)
add_hft_backtest_test(hft-backtest-strategy-config-tests
  tests/config/BacktestStrategyConfigTests.cpp)
add_hft_backtest_test(hft-backtest-passive-bbo-touch-tests
  tests/Execution/BacktestPassiveBboTouchTests.cpp)
add_hft_backtest_test(hft-backtest-stop-limit-tests
  tests/Execution/BacktestStopLimitTests.cpp)
add_hft_backtest_test(hft-backtest-rate-limit-tests
  tests/Execution/BacktestRateLimitTests.cpp)
add_hft_backtest_test(hft-backtest-multileg-context-tests
  tests/Execution/BacktestMultilegContextTests.cpp)
add_hft_backtest_test(hft-backtest-transaction-admission-tests
  tests/Execution/BacktestTransactionAdmissionTests.cpp)
add_hft_backtest_test(hft-backtest-stat-arb-replay-tests
  tests/Replay/StatArbEdgeMakerReplayTests.cpp)
add_hft_backtest_test(hft-backtest-uptick-replay-tests
  tests/Replay/UptickReplayTests.cpp)
add_hft_backtest_test(hft-backtest-strategy-indicator-tests
  tests/Strategy/BacktestStrategyIndicatorTests.cpp)
add_hft_backtest_test(hft-backtest-strategy-profile-tests
  tests/Strategy/BacktestStrategyProfileTests.cpp)
add_hft_backtest_test(hft-backtest-metrics-tests
  tests/Metrics/BacktestMetricsTests.cpp)
add_hft_backtest_test(hft-backtest-accounting-tests
  tests/Accounting/BacktestAccountingTests.cpp)
add_hft_backtest_test(hft-backtest-session-loader-tests
  tests/Input/SessionLoaderTests.cpp)
add_hft_backtest_test(hft-backtest-sweep-tests
  tests/Sweep/BacktestSweepTests.cpp)

set_property(TEST
  hft-backtest-execution-tests
  hft-backtest-client-order-id-tests
  hft-backtest-replace-lifecycle-tests
  hft-backtest-stat-arb-late-fill-lifecycle-tests
  APPEND PROPERTY LABELS "order-race;lifecycle")
set_property(TEST hft-backtest-session-loader-tests APPEND PROPERTY
  LABELS "corpus-contract")

add_custom_target(hft-backtest-product
  DEPENDS hft-backtest)

# Keep the product-wide build entrypoint closed over every test target added
# above, including future targets registered in this directory.
get_property(_hft_backtest_all_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
add_custom_target(hft-backtest-all DEPENDS ${_hft_backtest_all_targets})
unset(_hft_backtest_all_targets)
