# Agent rules - hft-backtest

`hft-backtest` is the deterministic replay/simulation consumer of CXET,
trading-core and hft-recorder session corpora.

## Project-owned versioning

- Keep one canonical current backtest contract. Do not add parallel
  `V1`/`V2`/`V3` types, compatibility aliases or forwarding facades; update all
  in-repo producers and consumers atomically.
- Numeric result/corpus/ABI guards remain fail-closed, but current app-owned
  names stay free of `V<number>`. Only isolated legacy readers, externally
  dictated protocol tokens and immutable evidence may retain versioned names.

## Boundaries

- Do not change strategy semantics to make backtests pass. Compile and link the
  canonical `trading_core::*` targets directly and keep simulation adapters at
  the Backtest boundary.
- Keep recorder as the owner of captured corpus files and UI launch/display.
- Backtest owns replay execution, fills, accounting, latency simulation, sweeps, metrics, and result JSON.
- Build from WSL/Linux only through the CXET root source graph. Link public CXET,
  trading-core and `hftrec::corpus_contract` targets; never import an HFT Trader
  runtime `.so`, copy HFT Trader runtime sources, or stage a sibling SDK.
- Backtest must remain independent of live HFT Trader and Trader session,
  readiness, reconnect and terminal-order owners.
- Keep hot replay state integer-scaled and deterministic. No floating point in execution/accounting code.

## Build/test restraint

- Do not run builds/tests unless explicit active-task authorization covers the exact verification command.
- If verification is needed, state the exact command for the user to run manually.

## Subagents and planning

Use subagents only for clearly separated replay, accounting, result JSON, documentation, or app-boundary tasks.

Before non-trivial backtest changes, state:

- selected roles;
- affected replay/accounting/result areas;
- determinism/accounting risks;
- manual verification checklist.

## Development workflow

Follow `../../docs/Agents/DevelopmentWorkflow.md` for behavioral TDD, refactor coverage and exact authorized
offline checks. Implementation alone permits edits only; local RED/GREEN and
CI, runtime and live evidence remain separate.
