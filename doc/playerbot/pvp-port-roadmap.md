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

### Phase-4 parity mapping (updated)

- Reference files mapped:
  - `playerbot reference/mod-playerbots-master/src/Ai/Base/StrategyContext.h` (strategy naming context for PvP strategy enable surface)
  - `playerbot reference/mod-playerbots-master/src/Ai/Base/TriggerContext.h` (trigger naming semantics used by PvP/class strategies)
  - `playerbot reference/mod-playerbots-master/src/Ai/Base/Strategy/BattlegroundStrategy.cpp` (battleground-active trigger context)
  - `playerbot reference/mod-playerbots-master/src/Ai/Class/Warrior/Strategy/ArmsWarriorStrategy.{h,cpp}` (Arms priority ordering and ActionNode-style fallback intent)
  - `playerbot reference/mod-playerbots-master/src/Ai/Class/Warrior/Trigger/WarriorTriggers.{h,cpp}` (trigger activation semantics for sudden death, taste for blood, high rage, battle stance/shout, and mobility)
  - `playerbot reference/mod-playerbots-master/src/Ai/Class/Warrior/Action/WarriorActions.{h,cpp}` (useful/possible checks and spell action targeting semantics)
  - `playerbot reference/mod-playerbots-master/src/Ai/Class/*/Strategy/*Strategy*.{h,cpp}` (default PvP action ordering across class/spec strategies used as class parity baseline)
  - `playerbot reference/mod-playerbots-master/src/Ai/Class/*/Trigger/*Triggers*.{h,cpp}` (trigger vocabulary and useful gating used to keep action naming and ordering aligned)
  - `playerbot reference/mod-playerbots-master/src/Ai/Class/*/Action/*Actions*.{h,cpp}` (spell useful/possible checks mirrored where Trinity APIs allow)
  - `playerbot reference/mod-playerbots-master/src/Bot/RandomPlayerbotMgr.cpp` (manager-owned cadence and lifecycle topology expectations)
- Port files touched:
  - `src/server/scripts/Playerbot/Pvp/PlayerbotPvpCore.{h,cpp}`
  - `src/server/scripts/Playerbot/Pvp/PlayerbotPvpLifecycleActions.{h,cpp}`
  - `src/server/scripts/Playerbot/Pvp/PlayerbotRandomBotParticipation.cpp`
  - `src/server/scripts/Playerbot/Pvp/PlayerbotPvpClassActions.cpp`
  - `src/server/worldserver/playerbots.conf.dist`
  - `doc/playerbot/pvp-port-roadmap.md`
- Intentional divergences:
  - Spell resolution uses known-rank lookup by spell ID instead of action-node factories, because the Trinity slice does not yet port the full `NamedObjectFactory<ActionNode>` class AI stack.
  - Trigger/action selection is executed in consolidated table-driven context builders plus primitive tactical execution stubs instead of individual `TriggerNode`/`ActionNode` factories, to preserve existing Trinity lifecycle topology.
  - (Historical in Phase-4) Flag-carrier ownership/proximity triggers were hard-disabled before objective-state seams were added in the later Phase-5/Phase-6 completion pass.
  - Spec strategy selection is inferred from active-spec talent ownership and rank-known spell availability, because full strategy graph context objects are not yet instantiated in this module.

Loader path and lifecycle gates remain preserved as-is:
- Loader update path remains exactly `RandomBotParticipationManager::ProcessPlayerLifecycle(Player*)`.
- Lifecycle gate chain remains exactly `Playerbot.Enable && Playerbot.PvpCore.Enable && Playerbot.PvpLifecycle.Enable`.
- Manager cadence ownership and interval remain unchanged.

### Phase-4 review follow-up (class behavior parity corrections)

- Exact reference files consulted in this pass:
  - `playerbot reference/mod-playerbots-master/src/Ai/Base/StrategyContext.h`
  - `playerbot reference/mod-playerbots-master/src/Ai/Base/TriggerContext.h`
  - `playerbot reference/mod-playerbots-master/src/Ai/Base/Strategy/BattlegroundStrategy.cpp`
  - `playerbot reference/mod-playerbots-master/src/Ai/Class/Warrior/Strategy/ArmsWarriorStrategy.cpp`
  - `playerbot reference/mod-playerbots-master/src/Ai/Class/Hunter/Strategy/MarksmanshipHunterStrategy.cpp`
  - `playerbot reference/mod-playerbots-master/src/Ai/Class/Rogue/Strategy/DpsRogueStrategy.cpp`
  - `playerbot reference/mod-playerbots-master/src/Ai/Class/Mage/Strategy/FrostMageStrategy.cpp`
  - `playerbot reference/mod-playerbots-master/src/Ai/Class/Dk/Strategy/FrostDKStrategy.cpp`
  - `playerbot reference/mod-playerbots-master/src/Ai/Class/Shaman/Strategy/EnhancementShamanStrategy.cpp`
  - `playerbot reference/mod-playerbots-master/src/Ai/Class/Priest/Strategy/ShadowPriestStrategy.cpp`
  - `playerbot reference/mod-playerbots-master/src/Ai/Class/Paladin/Strategy/DpsPaladinStrategy.cpp`
  - `playerbot reference/mod-playerbots-master/src/Ai/Class/Druid/Strategy/FeralDruidStrategy.cpp`
  - `playerbot reference/mod-playerbots-master/src/Ai/Class/Warlock/Strategy/DestructionWarlockStrategy.cpp`
  - `playerbot reference/mod-playerbots-master/src/Bot/RandomPlayerbotMgr.cpp`
- Exact local files touched in this pass:
  - `src/server/scripts/Playerbot/Pvp/PlayerbotPvpCore.cpp`
  - `src/server/scripts/Playerbot/Pvp/PlayerbotPvpClassActions.cpp`
  - `src/server/worldserver/playerbots.conf.dist`
  - `doc/playerbot/pvp-port-roadmap.md`
  - `compile_commands.json`
- One-line intentional divergences still true after this pass:
  - Flag-carrier trigger queries are still hard-disabled (`false`) because BG objective ownership/proximity seams are not yet ported in this Trinity slice.
  - Class behavior remains table-driven in a consolidated selector rather than full upstream `TriggerNode`/`ActionNode` graph factories.
- Preservation statement:
  - Loader path remains exactly `RandomBotParticipationManager::ProcessPlayerLifecycle(Player*)`; lifecycle gate chain remains exactly `Playerbot.Enable && Playerbot.PvpCore.Enable && Playerbot.PvpLifecycle.Enable`; cadence ownership and interval remain manager-owned and unchanged.

### Phase-5 / Phase-6 completion pass (validation-ready behavior)

- Exact reference files consulted in this pass:
  - `playerbot reference/mod-playerbots-master/src/Ai/Base/StrategyContext.h`
  - `playerbot reference/mod-playerbots-master/src/Ai/Base/TriggerContext.h`
  - `playerbot reference/mod-playerbots-master/src/Ai/Base/Strategy/BattlegroundStrategy.cpp`
  - `playerbot reference/mod-playerbots-master/src/Ai/Base/Trigger/PvpTriggers.{h,cpp}`
  - `playerbot reference/mod-playerbots-master/src/Ai/Class/*/Strategy/*Strategy*.{h,cpp}`
  - `playerbot reference/mod-playerbots-master/src/Ai/Class/*/Trigger/*Triggers*.{h,cpp}`
  - `playerbot reference/mod-playerbots-master/src/Ai/Class/*/Action/*Actions*.{h,cpp}`
  - `playerbot reference/mod-playerbots-master/src/Bot/RandomPlayerbotMgr.cpp`
- Exact local files touched in this pass:
  - `src/server/scripts/Playerbot/Pvp/PlayerbotPvpCore.h`
  - `src/server/scripts/Playerbot/Pvp/PlayerbotPvpCore.cpp`
  - `src/server/scripts/Playerbot/Pvp/PlayerbotPvpLifecycleActions.cpp`
  - `src/server/worldserver/playerbots.conf.dist`
  - `doc/playerbot/pvp-port-roadmap.md`
- One-line intentional divergences still true after this pass:
  - Class behavior remains table-driven in a consolidated selector rather than full upstream `TriggerNode`/`ActionNode` graph factories.
  - Objective navigation still uses safe local movement primitives (follow/move-point) rather than full upstream tactical pathing graph and role-coordination state.
- Preservation statement:
  - Loader update path remains exactly `RandomBotParticipationManager::ProcessPlayerLifecycle(Player*)`.
  - Lifecycle gate chain remains exactly `Playerbot.Enable && Playerbot.PvpCore.Enable && Playerbot.PvpLifecycle.Enable`.
  - Cadence ownership/interval remain manager-owned and unchanged.
  - Queue policy in lifecycle actions remains unchanged.
  - Class-slice default remains OFF unless explicitly enabled.

### Phase-6 validation upgrade checklist (compile + smoke)

- Compile validation:
  - Confirm `compile_commands.json` includes all touched Playerbot PvP `.cpp` paths.
  - Run per-file syntax checks:
    - `clang++ -std=c++20 -fsyntax-only <flags-from-compile_commands> src/server/scripts/Playerbot/Pvp/PlayerbotPvpCore.cpp`
    - `clang++ -std=c++20 -fsyntax-only <flags-from-compile_commands> src/server/scripts/Playerbot/Pvp/PlayerbotPvpLifecycleActions.cpp`
  - Run narrow object-target builds for touched objects from existing build tree.
- Lifecycle observability expectations (runtime):
  - `.playerbot pvp lifecycle snapshot` must still expose `gateDisabled`, `cadenceThrottled`, `invalidPlayerState`, `noLifecycleHooksActive`, `battlegroundLifecycleExecuted`, and `arenaLifecycleExecuted`.
  - `playerbots.pvp.lifecycle` debug logs must still include:
    - branch marker lines
    - reason observation lines
    - dispatcher completion lines including battleground/arena/tactical/class booleans.
- Explicit manual verification checklist:
  - Battleground queueing:
    - Bot can join BG queue with lifecycle gates enabled.
    - BG invites are accepted/declined according to existing lifecycle context policy.
  - Battleground objective behavior:
    - In WSG/EotS when bot carries objective flag, `player has flag` trigger can activate and objective movement executes.
    - In WSG/EotS when enemy carrier is near, `enemy flagcarrier near` trigger can activate and `attack enemy flag carrier` movement executes.
    - In WSG/EotS when friendly carrier is near (and not in both-flags-out suppression window for WSG), `team flagcarrier near` trigger can activate and `bg protect fc` movement executes.
    - Trigger ordering semantics still prefer emergency/objective handling before sustain actions.
  - Arena behavior:
    - Arena queue join/leave and team invite handling continue to execute with unchanged policy.
  - Regression safety:
    - With `Playerbot.PvpCore.Enable = 0` or `Playerbot.PvpLifecycle.Enable = 0`, lifecycle remains gated and no queue/tactical/class PvP behavior executes.
