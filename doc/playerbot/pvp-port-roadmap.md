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

### Phase 6 manual validation procedure (pre-Phase-4, validation-only)

- Config prerequisites:
  - `Playerbot.Enable = 1`
  - `Playerbot.PvpCore.Enable = 1`
  - `Playerbot.PvpLifecycle.Enable = 1`
  - Optional for branch visibility tests: enable `playerbots.pvp.lifecycle` debug log filtering in your logger config.
- In-game command hook:
  - Run `.playerbot pvp lifecycle snapshot` from a GM account to print `RandomBotParticipationManager::GetLifecycleObservationSnapshot()` counters.
- Expected log lines:
  - Branch marker line: `Playerbot PvP lifecycle branch: guid=<...> branch=<...>.`
  - Observation reason line: `Playerbot PvP lifecycle observation: reason=<...> guid=<...> count=<...>.`
  - Dispatcher summary line: `Playerbot PvP lifecycle dispatcher complete: guid=<...>, didExecuteBattleground=<0|1>, didExecuteArena=<0|1>.`
- Expected counter movement patterns:
  - `gateDisabled` increases only when any lifecycle gate in `Playerbot.Enable && Playerbot.PvpCore.Enable && Playerbot.PvpLifecycle.Enable` is false at runtime.
  - `cadenceThrottled` increases during repeated `OnUpdate` calls inside the manager-owned cadence interval.
  - `invalidPlayerState` increases for null/out-of-world/teleporting players.
  - `noLifecycleHooksActive` increases when lifecycle gate is on but both participation hooks are false.
  - `battlegroundLifecycleExecuted`/`arenaLifecycleExecuted` increase only when their corresponding lifecycle action path actually executes.

## Porting policy

- Keep each commit focused on one subsystem and compilable.
- Prefer adapter wrappers when AC and Trinity APIs differ.
- Gate incomplete systems behind explicit config flags.
- Preserve references in comments when behavior is intentionally mirrored.

### Phase-4 Slice 1 parity mapping (updated)

- Reference files mapped:
  - `playerbot reference/mod-playerbots-master/src/Ai/Base/StrategyContext.h` (strategy naming context for PvP strategy enable surface)
  - `playerbot reference/mod-playerbots-master/src/Ai/Base/TriggerContext.h` (trigger naming semantics used by PvP/class strategies)
  - `playerbot reference/mod-playerbots-master/src/Ai/Base/Strategy/BattlegroundStrategy.cpp` (battleground-active trigger context)
  - `playerbot reference/mod-playerbots-master/src/Ai/Class/Warrior/Strategy/ArmsWarriorStrategy.{h,cpp}` (Arms priority and trigger/action intent)
  - `playerbot reference/mod-playerbots-master/src/Ai/Class/Warrior/Trigger/WarriorTriggers.{h,cpp}` (trigger activation semantics for sudden death, taste for blood, high rage, battle stance/shout, and mobility)
  - `playerbot reference/mod-playerbots-master/src/Ai/Class/Warrior/Action/WarriorActions.{h,cpp}` (useful/possible checks and spell action targeting semantics)
  - `playerbot reference/mod-playerbots-master/src/Bot/RandomPlayerbotMgr.cpp` (manager-owned cadence and lifecycle topology expectations)
  - `playerbot reference/mod-playerbots-master/src/Ai/Class/*/Strategy/*Strategy.cpp` (class default-action ordering used to extend parity selection across all playable classes)
- Port files touched:
  - `src/server/scripts/Playerbot/Pvp/PlayerbotPvpCore.{h,cpp}`
  - `src/server/scripts/Playerbot/Pvp/PlayerbotPvpClassActions.cpp`
  - `doc/playerbot/pvp-port-roadmap.md`
- Intentional divergences:
  - Spell resolution uses known-rank lookup by spell ID instead of action-node factories, because the Trinity slice does not yet port the full `NamedObjectFactory<ActionNode>` class AI stack.
  - Trigger checks are condensed into a single context builder/executor path to keep the current lifecycle topology unchanged while still mirroring Arms trigger priority order.
  - `piercing howl`/`mocking blow` fallback is represented by direct `hamstring` execution because this Trinity slice does not yet expose those fallback actions in the PvP class action enum.
  - `slam`/`victory rush`/`retaliation`/`shattering throw` priorities are intentionally deferred to later slices because this Phase-4 Slice 1 scope is constrained to the direct Arms baseline decision chain used for primary PvP pressure setup.
  - All classes currently mirror reference default-action priority spell choices (known-rank/cooldown/range checks) without full per-class trigger graph port because TriggerContext/ActionNode class stacks are still outside this slice’s topology constraints.

Loader path and lifecycle gates remain preserved as-is:
- Loader update path remains exactly `RandomBotParticipationManager::ProcessPlayerLifecycle(Player*)`.
- Lifecycle gate chain remains exactly `Playerbot.Enable && Playerbot.PvpCore.Enable && Playerbot.PvpLifecycle.Enable`.
- Manager cadence ownership and interval remain unchanged.
