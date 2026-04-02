# Playerbot PvP Port Roadmap (AzerothCore references -> custom TrinityCore)

This roadmap bootstraps the PvP playerbot port in incremental commits.

## Goal

Port PvP-focused playerbot behavior from the provided references into this core, including:

- Battleground + arena participation flow.
- PvP target selection and tactical decision making.
- Class/spec PvP spell and action choices.
- Supporting config + data needed for those systems.

## Phase 0 (this commit): discovery + slice definition

1. Build an inventory of PvP-related code in both references.
2. Identify high-value entry points that can be ported independently.
3. Split work into minimal compilable slices.

Output in this phase:

- `doc/playerbot/pvp-scope-inventory.md`
- `tools/playerbot/extract_pvp_scope.py`

## Phase 1: framework skeleton in this core

- Add `Playerbot` module build skeleton (CMake integration + toggles).
- Add minimal AI context wiring (no behavior enabled yet).
- Add config bootstrap (`playerbots.conf.dist`) and safe defaults (feature flags off).

## Phase 2: PvP core behavior layer

- Port generic PvP triggers/values/actions:
  - `PvpTriggers`
  - `PvpValues`
  - battleground/arena status triggers
- Port battleground tactical scaffold:
  - objective selection
  - movement-to-objective primitives
  - flag carrier attack/protect behavior

## Phase 3: queueing + participation lifecycle

- Battleground join/leave/status packet actions.
- Arena queue + team interaction actions.
- Random bot manager hooks for PvP participation.

## Phase 4: class PvP spell decision trees

- Port class strategy + trigger blocks used in PvP contexts.
- Validate spell availability/rank checks against this core APIs.
- Normalize behavior for interrupts/CC/defensives/offensives per role.

## Phase 5: data + balancing

- SQL payloads needed for PvP playerbot texts and options.
- Config tuning for queue cadence, prohibited zones, reaction speeds.
- Add guardrails to disable any unstable feature independently.

## Phase 6: validation

- Compile-only checks after each slice.
- Battleground smoke test script (queue -> accept -> objective movement).
- Arena smoke tests (2v2/3v3 queue, engage, reset).
- Regression checks for non-PvP behavior when PvP flags are off.

### Lifecycle observability checks

- Validate lifecycle debug logs include dispatcher completion with explicit battleground/arena `didExecute` booleans.
- Validate lifecycle debug logs include deterministic no-op guard output when lifecycle hooks are active but built contexts are both no-op.
- Inspect lifecycle reason counters through the manager snapshot seam to verify gate-disabled, cadence-throttled, invalid-state, no-hook, and executed-path counts are incrementing as expected during runtime checks.

## Porting policy

- Keep each commit focused on one subsystem and compilable.
- Prefer adapter wrappers when AC and Trinity APIs differ.
- Gate incomplete systems behind explicit config flags.
- Preserve references in comments when behavior is intentionally mirrored.
