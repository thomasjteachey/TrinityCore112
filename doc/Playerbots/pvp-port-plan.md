# Playerbot PvP port plan for this custom TrinityCore

This repository now includes a **starter PvP role-selection scaffold** under `src/server/game/AI/Playerbots/Pvp`.

## Reference inputs used

- `playerbot reference/mod-playerbots-master/src/Ai/Base/Actions/BattleGroundTactics.cpp`
- `playerbot reference/mod-playerbots-master/src/Ai/Base/Actions/BattleGroundTactics.h`
- `playerbot reference/mod-playerbots-master/src/Ai/Base/Actions/BattleGroundJoinAction.*`
- `playerbot reference/mod-playerbots-master/src/Ai/Base/Values/BGStatusValue.*`

## What was ported in this commit

- Added `playerbot::BattlegroundPvpRole` and `playerbot::BattlegroundPvpStateSnapshot`.
- Added `playerbot::PlayerbotPvpRoleSelector` with deterministic role selection based on:
  - flag-carrier pressure,
  - objective proximity,
  - low-health fallback,
  - team-size disadvantage.

This is intentionally small and compile-safe so you can iterate in your custom core without importing the full AzerothCore playerbot stack in one step.

## Next porting steps (recommended order)

1. Add a `PlayerbotPvpContext` adapter that reads real battleground state from TrinityCore APIs.
2. Add battleground objective providers per map (WSG/AB/EotS/AV/IoC).
3. Add movement driver actions (follow objective, defend objective, escort carrier).
4. Add queue/join/leave battleground state machine.
5. Add SQL/config knobs for bot PvP participation rates and role weights.
6. Add debug commands (`.playerbot pvp role`, `.playerbot pvp state`) for live tuning.

## Why this approach

Directly copying `mod-playerbots` PvP code is high-risk because the original module depends on:

- AzerothCore-specific hooks,
- module lifecycle wiring,
- broader playerbot AI contexts and value stores.

By introducing a local scaffold first, you can port behavior incrementally while keeping your core build stable.
