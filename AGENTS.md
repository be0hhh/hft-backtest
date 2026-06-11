# Agent rules - hft-backtest

`hft-backtest` is an app-layer replay/backtest module above CXETCPP, hft-trader, and hft-recorder session corpora.

- Do not change strategy code to make backtests work. Link the prebuilt hft-trader runtime shared library and adapt at the app boundary.
- Keep recorder as the owner of captured corpus files and UI launch/display. Backtest owns replay execution, fills, accounting, and result JSON.
- Build from WSL/Linux only. The module links Linux `.so` artifacts from hft-trader and CXETCPP.
- Do not run builds/tests unless the current user message explicitly asks for the exact verification command.
- Keep hot replay state integer-scaled and deterministic. No floating point in execution/accounting code.

## Subagents and planning

- Root `AGENTS.md` permits moderate automatic Codex subagent use for independent work. In `hft-backtest`, use subagents only for clearly separated replay, accounting, result JSON, documentation, or app-boundary tasks.
- Subagents must not run builds/tests or compile linked dependencies unless the current user message explicitly asks for that exact verification.
- Before non-trivial backtest changes, state the goal, assumptions, affected replay/accounting/result areas, and the manual verification command or checklist.
