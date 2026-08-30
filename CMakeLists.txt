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
  "${CMAKE_CURRENT_LIST_DIR}/../trading-core/strategy_profiles/*.ini")
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
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_strategy_metadata.py"
    --profile-root "${CMAKE_CURRENT_LIST_DIR}/../trading-core/strategy_profiles"
    --output "${HFT_BACKTEST_GENERATED_STRATEGY_METADATA}"
    ${HFT_BACKTEST_REQUIRED_STRATEGY_ARGS}
    ${HFT_BACKTEST_STRATEGY_PROFILES}
  DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_strategy_metadata.py"
    ${HFT_BACKTEST_STRATEGY_PROFILES}
  COMMENT "Generating bounded hft-backtest strategy metadata"
  VERBATIM
)
add_custom_target(hft_backtest_generate_strategy_metadata
  DEPENDS "${HFT_BACKTEST_GENERATED_STRATEGY_METADATA}")

add_library(hft_backtest_core SHARED
  src/accounting/backtest_accounting.cpp
  src/accounting/backtest_metrics.cpp
  src/accounting/backtest_result_aggregation.cpp
  src/config/backtest_run_config.cpp
  src/config/backtest_strategy_config.cpp
  src/config/strategy_metadata.cpp
  "${HFT_BACKTEST_GENERATED_STRATEGY_METADATA}"
  src/engine/backtest_common.cpp
  src/engine/backtest_engine_setup.cpp
  src/engine/backtest_prepared.cpp
  src/engine/backtest_replay_clock.cpp
  src/engine/backtest_timeline.cpp
  src/execution/backtest_execution.cpp
  src/execution/backtest_execution_depth.cpp
  src/execution/backtest_fill_model.cpp
  src/execution/backtest_latency.cpp
  src/input/binary_corpus_loader.cpp
  src/input/session_loader.cpp
  src/results/backtest_result_writer.cpp
  src/runner/backtest.cpp
  src/strategy/backtest_strategy_intent_lowering.cpp
  src/strategy/backtest_strategy_input_freshness.cpp
  src/strategy/backtest_strategy_runtime.cpp
  src/strategy/backtest_strategy_trace.cpp
  src/strategy/backtest_strategy_warmup.cpp
  src/sweep/backtest_sweep.cpp
  src/sweep/backtest_sweep_search.cpp
  src/sweep/backtest_sweep_writer.cpp
)
add_library(hft_backtest::core ALIAS hft_backtest_core)

target_include_directories(hft_backtest_core
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
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

add_executable(hft-backtest tools/hft_backtest.cpp)
target_link_libraries(hft-backtest PRIVATE hft_backtest_core)
target_compile_features(hft-backtest PRIVATE cxx_std_23)
set_target_properties(hft-backtest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

function(add_hft_backtest_test target source)
  add_executable(${target} ${source})
  target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
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
  tests/execution/backtest_execution_tests.cpp)
add_hft_backtest_test(hft-backtest-client-order-id-tests
  tests/execution/backtest_client_order_id_tests.cpp)
add_hft_backtest_test(hft-backtest-replace-lifecycle-tests
  tests/execution/backtest_replace_lifecycle_tests.cpp)
add_hft_backtest_test(hft-backtest-stat-arb-late-fill-lifecycle-tests
  tests/execution/backtest_stat_arb_late_fill_lifecycle_tests.cpp)
add_hft_backtest_test(hft-backtest-strategy-config-tests
  tests/config/backtest_strategy_config_tests.cpp)
add_hft_backtest_test(hft-backtest-passive-bbo-touch-tests
  tests/execution/backtest_passive_bbo_touch_tests.cpp)
add_hft_backtest_test(hft-backtest-stop-limit-tests
  tests/execution/backtest_stop_limit_tests.cpp)
add_hft_backtest_test(hft-backtest-rate-limit-tests
  tests/execution/backtest_rate_limit_tests.cpp)
add_hft_backtest_test(hft-backtest-multileg-context-tests
  tests/execution/backtest_multileg_context_tests.cpp)
add_hft_backtest_test(hft-backtest-transaction-admission-tests
  tests/execution/backtest_transaction_admission_tests.cpp)
add_hft_backtest_test(hft-backtest-stat-arb-replay-tests
  tests/replay/stat_arb_edge_maker_replay_tests.cpp)
add_hft_backtest_test(hft-backtest-uptick-replay-tests
  tests/replay/uptick_replay_tests.cpp)
add_hft_backtest_test(hft-backtest-strategy-indicator-tests
  tests/strategy/backtest_strategy_indicator_tests.cpp)
add_hft_backtest_test(hft-backtest-strategy-profile-tests
  tests/strategy/backtest_strategy_profile_tests.cpp)
add_hft_backtest_test(hft-backtest-metrics-tests
  tests/metrics/backtest_metrics_tests.cpp)
add_hft_backtest_test(hft-backtest-accounting-tests
  tests/accounting/backtest_accounting_tests.cpp)
add_hft_backtest_test(hft-backtest-session-loader-tests
  tests/input/session_loader_tests.cpp)
add_hft_backtest_test(hft-backtest-sweep-tests
  tests/sweep/backtest_sweep_tests.cpp)

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
