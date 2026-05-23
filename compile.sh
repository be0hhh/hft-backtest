#!/usr/bin/env bash
set -euo pipefail

if [[ $(uname -s) != Linux ]]; then
  echo hft-backtest must be built from WSL/Linux. >&2
  exit 1
fi

if [[ ! -f ../hft-trader/build/libhft_trader_runtime.so ]]; then
  echo Missing trader shared runtime: ../hft-trader/build/libhft_trader_runtime.so >&2
  echo Build apps/hft-trader first. >&2
  exit 1
fi

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target hft_backtest_core hft-backtest --parallel $(nproc)

echo hft-backtest build OK. CLI: ./build/hft-backtest
