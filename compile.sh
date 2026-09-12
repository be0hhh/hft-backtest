#!/usr/bin/env bash
set -euo pipefail

APP_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
CXET_ROOT="$(cd "$APP_ROOT/../.." && pwd)"
ROOT_WRAPPER="$CXET_ROOT/compile.sh"

if [[ $(uname -s) != Linux ]]; then
  echo "hft-backtest must be built from WSL/Linux through the CXET root graph." >&2
  exit 1
fi
if [[ ! -f "$ROOT_WRAPPER" || ! -r "$ROOT_WRAPPER" ]]; then
  echo "Missing readable CXET root build wrapper: $ROOT_WRAPPER" >&2
  exit 1
fi

exec bash "$ROOT_WRAPPER" hft-backtest "$@"
