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
    capital::finance
    trading_core::algorithm
    trading_core::execution
    trading_core_math
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

add_library(hft_backtest_core STATIC
  src/Runtime/src/Accounting/BacktestAccounting.cpp
  src/Runtime/src/Accounting/BacktestMetrics.cpp
  src/Runtime/src/Accounting/BacktestResultAggregation.cpp
  src/Runtime/src/Config/BacktestRunConfig.cpp
  src/Runtime/src/Config/Strategy/BacktestStrategyConfig.cpp
  src/Runtime/src/Config/Strategy/BacktestStrategyRateLimits.cpp
  src/Runtime/src/Config/Strategy/BacktestStrategyValidation.cpp
  src/Runtime/src/Config/StrategyMetadata.cpp
  src/Runtime/src/Run/BacktestCommon.cpp
  src/Runtime/src/Engine/BacktestEngineSetup.cpp
  src/Runtime/src/Engine/BacktestQuota.cpp
  src/Runtime/src/Preparation/BacktestPrepared.cpp
  src/Runtime/src/Replay/BacktestReplayClock.cpp
  src/Runtime/src/Replay/BacktestTimeline.cpp
  src/Runtime/src/Execution/BacktestExecution.cpp
  src/Runtime/src/Execution/BacktestOrderEvidence.cpp
  src/Runtime/src/Execution/BacktestDerivativeCollateral.cpp
  src/Runtime/src/Execution/Model/BacktestExecutionDepth.cpp
  src/Runtime/src/Execution/Model/BacktestFillModel.cpp
  src/Runtime/src/Execution/Model/BacktestLiquidity.cpp
  src/Runtime/src/Execution/Model/BacktestLatency.cpp
  src/Runtime/src/Input/Binary/BinaryCorpusLoader.cpp
  src/Runtime/src/Input/Binary/BinaryCorpusStream.cpp
  src/Runtime/src/Input/Binary/BinaryCorpusDepth.cpp
  src/Runtime/src/Input/Session/SessionLoader.cpp
  src/Runtime/src/Input/Session/SessionChannels.cpp
  src/Runtime/src/Input/Session/SessionManifestValidation.cpp
  src/Runtime/src/Results/BacktestResultWriter.cpp
  src/Runtime/src/Run/Backtest.cpp
    src/Runtime/src/Strategy/Inputs/BacktestStrategySnapshots.cpp
    src/Runtime/src/Strategy/Inputs/BacktestStrategyInstrument.cpp
    src/Runtime/src/Strategy/Cycle/BacktestStrategySingleCycle.cpp
    src/Runtime/src/Strategy/Cycle/BacktestStrategyPortfolioCycle.cpp
    src/Runtime/src/Strategy/Execution/BacktestStrategySettlement.cpp
    src/Runtime/src/Strategy/BacktestStrategyAccount.cpp
    src/Runtime/src/Strategy/Cycle/BacktestStrategyCycleSupport.cpp
    src/Runtime/src/Execution/Lifecycle/BacktestExecutionDelivery.cpp
    src/Runtime/src/Execution/Lifecycle/BacktestExecutionActivation.cpp
    src/Runtime/src/Execution/Lifecycle/BacktestExecutionPending.cpp
    src/Runtime/src/Execution/Lifecycle/BacktestExecutionFinalization.cpp
    src/Runtime/src/Results/BacktestResultDiagnostic.cpp
    src/Runtime/src/Results/BacktestResultOrderLifetimes.cpp
    src/Runtime/src/Results/BacktestResultStreams.cpp
    src/Runtime/src/Results/BacktestResultManifest.cpp
    src/Runtime/src/Run/BacktestPreparation.cpp
    src/Runtime/src/Run/BacktestReplay.cpp
  src/Runtime/src/Run/BacktestUniverse.cpp
  src/Runtime/src/Run/BacktestUniversePreparation.cpp
  src/Runtime/src/Run/BacktestUniverseMembership.cpp
  src/Runtime/src/Simulation/SimulationSession.cpp
  src/Runtime/src/Simulation/SimulationInput.cpp
  src/Runtime/src/Simulation/SimulationOutput.cpp
  src/Runtime/src/Simulation/SimulationReferences.cpp
  src/Runtime/src/Simulation/SimulationUniverse.cpp
    src/Runtime/src/Run/BacktestFinalization.cpp
    src/Runtime/src/Run/BacktestArtifacts.cpp
    src/Runtime/src/Run/BacktestReplayRisk.cpp
    src/Runtime/src/Replay/BacktestReplaySupport.cpp
  src/Runtime/src/Strategy/Execution/BacktestStrategyIntentLowering.cpp
  src/Runtime/src/Strategy/Execution/BacktestCanonicalPairAdapter.cpp
  src/Runtime/src/Strategy/Execution/BacktestCanonicalPairSelection.cpp
  src/Runtime/src/Strategy/Inputs/BacktestStrategyInputFreshness.cpp
  src/Runtime/src/Strategy/BacktestStrategyRuntime.cpp
  src/Runtime/src/Strategy/Diagnostic/BacktestStrategyTrace.cpp
  src/Runtime/src/Strategy/Inputs/BacktestStrategyWarmup.cpp
  src/Runtime/src/Sweep/BacktestSweep.cpp
  src/Runtime/src/Sweep/BacktestSweepSearch.cpp
  src/Runtime/src/Sweep/BacktestSweepWriter.cpp
)
add_library(hft_backtest::core ALIAS hft_backtest_core)

target_include_directories(hft_backtest_core
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/Runtime/include"
  PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/Runtime/src"
)
target_link_libraries(hft_backtest_core
  PUBLIC
    cxet::cxet_lib
    trading_core_math
    capital::finance
    trading_core::algorithm
    trading_core::execution
    hftrec::reference_contract
  PRIVATE
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
)
target_compile_options(hft_backtest_core PRIVATE
  $<$<CONFIG:Release>:-O3>
  $<$<AND:$<CONFIG:Release>,$<NOT:$<BOOL:${CXET_PORTABLE_BUILD}>>>:-march=native>
)
set_target_properties(hft_backtest_core PROPERTIES
  OUTPUT_NAME hft_backtest_core
  LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

add_subdirectory(src/Research)

add_executable(hft-backtest src/Process/HftBacktest.cpp)
target_link_libraries(hft-backtest PRIVATE hft_backtest_core)
target_compile_features(hft-backtest PRIVATE cxx_std_23)
set_target_properties(hft-backtest PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
)

add_custom_target(hft-backtest-product
  DEPENDS hft-backtest hft-backtestd hft-backtest-tui)

# Aggregate only the concrete production targets.
add_custom_target(hft-backtest-all DEPENDS hft-backtest-product)
