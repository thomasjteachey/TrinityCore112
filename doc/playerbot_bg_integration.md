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
