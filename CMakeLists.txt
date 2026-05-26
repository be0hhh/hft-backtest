cmake_minimum_required(VERSION 3.22)
project(hft_backtest LANGUAGES CXX VERSION 0.1.0)

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

add_library(hft_backtest_core SHARED
  src/backtest_accounting.cpp
  src/backtest_latency.cpp
  src/backtest_common.cpp
  src/backtest.cpp
  src/backtest_execution.cpp
  src/backtest_metrics.cpp
  src/backtest_result_writer.cpp
  src/backtest_sweep.cpp
  src/backtest_sweep_search.cpp
  src/backtest_sweep_writer.cpp
  src/session_loader.cpp
  src/strategy_metadata.cpp
  src/backtest_timeline.cpp
)
add_library(hft_backtest::core ALIAS hft_backtest_core)

target_include_directories(hft_backtest_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hft_backtest_core PUBLIC hft_trader_runtime cxet_lib Threads::Threads)
target_compile_features(hft_backtest_core PUBLIC cxx_std_23)
target_compile_definitions(hft_backtest_core PRIVATE HFT_BACKTEST_BUILDING_LIBRARY=1)
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

add_executable(hft-backtest-execution-tests tests/backtest_execution_tests.cpp)
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

add_executable(hft-backtest-metrics-tests tests/backtest_metrics_tests.cpp)
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

add_executable(hft-backtest-sweep-tests tests/backtest_sweep_tests.cpp)
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
