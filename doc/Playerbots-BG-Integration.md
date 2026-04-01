# Playerbot Battleground integration (AzerothCore module -> TrinityCore)

This repository now includes a BG queue integration hook that lets an external playerbot module inject bots before a battleground match is assembled.

## What is implemented in TrinityCore

1. `WITH_PLAYERBOTS_BG_BRIDGE` CMake option.
2. `BattlegroundBotIntegration` hook object in the game layer.
3. Hook invocation inside `BattlegroundQueue::Update` right before premade/normal match checks.

That means your playerbot adapter can register a callback and add bots whenever queue population is below the required team sizes.

## Build flag

Configure with:

```bash
cmake -S . -B build -DWITH_PLAYERBOTS_BG_BRIDGE=ON
```

The build defines `TRINITY_WITH_PLAYERBOTS_BG_BRIDGE` for game interface consumers.

## Adapter expected from your playerbot port

Create your adapter (recommended under custom scripts/module code) that calls:

- `BattlegroundBotIntegration::Instance().RegisterEnsureQueueHook(...)`

Callback responsibilities for BG-only behavior:

1. Skip arenas/rated queues.
2. Determine missing slots per team using `minPlayersPerTeam` and current queue state.
3. Add/requeue playerbots into the target battleground queue bracket.
4. Keep faction balance (same missing count for alliance/horde unless you intentionally allow asymmetry).

## Suggested porting scope from AzerothCore module

For BG-only MVP, focus on porting these behaviors first (not the entire module stack):

1. Queue fill policy (when to inject bots).
2. Bot participation lifecycle (accept invite, enter BG, leave/despawn after match).
3. Combat AI profile set tuned for battleground objectives.

Delay these until phase 2:

- Economy/profession/travel systems.
- Non-BG random world behaviors.
- Full playerbot database feature set.

## Practical migration plan

1. Extract only battleground-related playerbot classes from your AzerothCore source.
2. Build a Trinity adapter layer around queue/session/player APIs that differ between AC and TC.
3. Register the queue hook during worldserver startup from your custom script/module initializer.
4. Add SQL/config for BG bot caps and enable per battleground type.
5. Validate with one BG (WSG) first, then expand to AB/AV/IoC.

## Notes

- The bridge is intentionally no-op by default when no hook is registered.
- No gameplay behavior changes occur unless your adapter registers the callback.
- A copied AzerothCore reference snapshot is available under `contrib/playerbots-ac-reference/` to support direct porting without moving the original root folder.
- `src/server/scripts/Custom/custom_playerbots_bg_bridge.cpp` now consumes the world hook `OnBattlegroundQueueNeedBots(...)` for WSG and can auto-queue configured online filler characters (`Playerbots.BG.WSG.AutoQueue.*`) when slots are missing.
- The bridge now also loads configured filler metadata from the `characters` table at startup (guid/race/team), which is the first step toward true offline-session orchestration on Trinity.
- A new `OfflineBotSessionManager` skeleton is now wired from the WSG bridge for unresolved deficits, providing the core insertion point for upcoming headless offline session boot logic.
- `OfflineBotSessionManager` now also tracks pending offline fill requests and processes them via an update loop placeholder, ready to swap in real headless-session boot internals.
- `Playerbots.BG.Offline.MaxAttemptsPerTick` controls placeholder offline start-attempt throughput while phase-2 headless session internals are being implemented.
- Offline manager now performs a DB-backed precheck (`characters.online`) with retry cooldown before headless-start placeholder attempts.
- Offline manager now exposes `RegisterHeadlessStartCallback(...)` so real headless session boot can be plugged in without changing the queue-processing pipeline.
- Progress tracker: see `doc/Playerbots-Port-Status.md` for ongoing phase/checklist state.
