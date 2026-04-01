# Integrating `azerothcore-wotlk-Playerbot` (Battleground-focused) into this TrinityCore tree

This repository is TrinityCore-based, while `mod-playerbots` targets an AzerothCore fork that already carries Playerbot core patches.

Because of that, full drop-in integration is **not** possible without a compatibility layer. This commit adds the first required compatibility hook: a new script type named `BGScript` with battleground start/end callbacks.

## What was added in core

- `BGScript` script type in `ScriptMgr`.
- New callbacks:
  - `ScriptMgr::OnBattlegroundStart(Battleground* bg)`
  - `ScriptMgr::OnBattlegroundEnd(Battleground* bg, TeamId winnerTeam)`
- Battleground lifecycle bridge calls from Trinity core:
  - `Battleground::StartBattleground()` now emits `OnBattlegroundStart`.
  - `Battleground::EndBattleground()` now emits `OnBattlegroundEnd`.

This mirrors the hook pattern used by AzerothCore modules that register a `BGScript` for battleground bot tactics.

## Recommended module layout in this repo

Place your module in:

- `src/server/scripts/Custom/Playerbots/` for gradual Trinity-ported files, or
- keep your imported source in `azerothcore-wotlk-Playerbot/` and copy/port only BG-relevant code into Trinity script files.

## BG-only porting order (minimal path)

1. Port `PlayerBotsBGScript` logic first (BG start/end strategy management).
2. Port queue participation logic from `RandomPlayerbotMgr` (auto-join/fill behavior).
3. Port only BG strategy/action classes actually referenced by step 1 and 2.
4. Add config keys in `worldserver.conf.dist` under a dedicated section (e.g. `Playerbot.BG.*`).
5. Add SQL tables required by bot queue state, if your selected logic depends on persistent state.

## Practical caveats

- AzerothCore Playerbot uses APIs and helper managers that do not exist in TrinityCore as-is.
- Expect symbol mismatches around:
  - bot account/session managers,
  - queue/invite helper wrappers,
  - script registration macros and naming.
- Keep the scope BG-only until queue filling and match behavior are stable.

## Quick validation checklist after porting module files

1. Build succeeds.
2. Bots can queue into at least one BG bracket.
3. BG start/end callbacks are hit (add temporary logs).
4. Teams receive bot backfill symmetrically.
5. No crash on BG shutdown / teleport out.


## Current status (started in this tree)

- ✅ Core hook bridge is in place (`BGScript`, `OnBattlegroundStart`, `OnBattlegroundEnd`).
- ✅ BG-only module bootstrapping exists under `src/server/scripts/Custom/Playerbots/`.
- ✅ Step 1 complete (strategy management baseline): Trinity-side `PlayerbotBGScript` now builds per-BG map policies (CTF/resource/lane/skirmish), squad plans, and role composition targets on BG start, then retires per-instance plans on BG end.
- ✅ Step 2 complete (core-side queue participation/fill behavior): BG queue-fill state is now tracked per instance, recalculated on a world update cadence, and kept symmetric using live+invited populations, min-team requirements, and free-slot caps.

## Remaining work (in order)

1. ✅ **Port `PlayerBotsBGScript` decision logic**
   - Completed baseline per-BG strategy management in Trinity-side script scaffolding (composition targets, role policies, map-specific squad plans).
2. ✅ **Port queue participation / fill behavior**
   - Bring over the subset of `RandomPlayerbotMgr` logic needed for auto-join and symmetric team backfill.
   - Implemented Trinity-side queue-fill coordinator with per-instance tracking and periodic recalculation.
   - Implemented symmetric fill policy using:
     - current + invited population,
     - minimum players per team,
     - one-player max team imbalance window,
     - available free slots per team.
   - Added transition-only logging for queue-fill request snapshots (`initialized`, `updated`, `final snapshot`).
3. ✅ **Port required BG strategy/action classes**
   - Added Trinity-side minimal directive/action classes for BG squads:
     - `PlayerbotBGAction`
     - `PlayerbotBGSquadDirective`
   - Added map-profile-to-action translation (`BuildSquadDirectives`) and directive logging on BG start.
4. ✅ **Add config keys (`Playerbot.BG.*`)**
   - Added settings to `worldserver.conf.dist`:
     - `Playerbot.BG.Enable`
     - `Playerbot.BG.QueueUpdateMs`
     - `Playerbot.BG.MaxTeamImbalance`
     - `Playerbot.BG.MaxBackfillPerUpdate`
   - Wired settings into runtime behavior (enable gate, queue cadence, imbalance limit, per-tick cap).
5. ✅ **Add SQL persistence (if needed by selected queue logic)**
   - Not required for current implementation: queue/backfill state is computed from live battleground populations + invite counts and kept in-memory per active instance.
6. **Validation pass**
   - Verify queueing, callback hit rates, symmetric backfill, and clean BG teardown with no crash/teleport regressions.
