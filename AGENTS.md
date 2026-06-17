# Agent rules - hft-backtest

`hft-backtest` is an app-layer replay/backtest module above CXETCPP, hft-trader, and hft-recorder session corpora.

## Boundaries

- Do not change strategy code to make backtests work. Link the prebuilt hft-trader runtime shared library and adapt at the app boundary.
- Keep recorder as the owner of captured corpus files and UI launch/display.
- Backtest owns replay execution, fills, accounting, latency simulation, sweeps, metrics, and result JSON.
- Build from WSL/Linux only. The module links Linux `.so` artifacts from hft-trader and CXETCPP.
- Keep hot replay state integer-scaled and deterministic. No floating point in execution/accounting code.

## Build/test restraint

- Do not run builds/tests unless the current user message explicitly asks for the exact verification command.
- If verification is needed, state the exact command for the user to run manually.

## Subagents and planning

Use subagents only for clearly separated replay, accounting, result JSON, documentation, or app-boundary tasks.

Before non-trivial backtest changes, state:

- selected roles;
- affected replay/accounting/result areas;
- determinism/accounting risks;
- manual verification checklist.
