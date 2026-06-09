#!/usr/bin/env bash
set -euo pipefail

APP_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$APP_ROOT/build"

if [[ $(uname -s) != Linux ]]; then
  echo hft-backtest must be built from WSL/Linux. >&2
  exit 1
fi

clear_stale_cmake_cache() {
  local cache_file="$BUILD_DIR/CMakeCache.txt"
  if [[ ! -f "$cache_file" ]]; then
    return 0
  fi
  local cached_source cached_build
  cached_source="$(grep -E '^CMAKE_HOME_DIRECTORY:INTERNAL=' "$cache_file" 2>/dev/null | cut -d= -f2- || true)"
  cached_build="$(grep -E '^CMAKE_CACHEFILE_DIR:INTERNAL=' "$cache_file" 2>/dev/null | cut -d= -f2- || true)"
  if [[ "$cached_source" == "$APP_ROOT" && "$cached_build" == "$BUILD_DIR" ]]; then
    return 0
  fi
  if [[ "$BUILD_DIR" != "$APP_ROOT/build" ]]; then
    echo "Refusing to clean unexpected hft-backtest build dir: $BUILD_DIR" >&2
    exit 1
  fi
  echo "CMake cache was created for a different hft-backtest path; regenerating build cache." >&2
  rm -rf "$BUILD_DIR/CMakeCache.txt" \
         "$BUILD_DIR/CMakeFiles" \
         "$BUILD_DIR/compile_commands.json" \
         "$BUILD_DIR/cmake_install.cmake" \
         "$BUILD_DIR/Makefile" \
         "$BUILD_DIR/build.ninja" \
         "$BUILD_DIR/.ninja_deps" \
         "$BUILD_DIR/.ninja_log"
}

if [[ ! -f "$APP_ROOT/../hft-trader/build/libhft_trader_runtime.so" ]]; then
  echo Missing trader shared runtime: ../hft-trader/build/libhft_trader_runtime.so >&2
  echo Build apps/hft-trader first. >&2
  exit 1
fi

clear_stale_cmake_cache

cmake -S "$APP_ROOT" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release
cmake --build "$BUILD_DIR" --target hft_backtest_core hft-backtest --parallel "$(nproc)"

echo hft-backtest build OK. CLI: ./build/hft-backtest
