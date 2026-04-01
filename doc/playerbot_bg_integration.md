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
6. ✅ **Validation pass (code-path scaffolding)**
   - Added server-side auto-queue mechanism gated by config:
     - `Playerbot.BG.AutoQueueBots`
     - `Playerbot.BG.AutoQueueBgType`
     - `Playerbot.BG.BotNamePrefix`
     - `Playerbot.BG.PreferRealPlayers`
     - `Playerbot.BG.AutoGenerateBots`
     - `Playerbot.BG.AutoGenerateTargetAlliance`
     - `Playerbot.BG.AutoGenerateTargetHorde`
     - `Playerbot.BG.AutoGenerateMaxCreatePerTick`
     - `Playerbot.BG.AutoGenerateNamePrefix`
   - Auto-queue tick now consumes both:
     - active-BG backfill demand (`BuildSymmetricBackfillRequest` aggregate),
     - queue-start demand for configured BG queue (underfilled queue toward min-per-team start threshold).
   - Important current limitation:
     - This mechanism does **not** fabricate new character records or sessions.
     - It queues **online candidate characters** that match `BotNamePrefix` and pass queue eligibility checks.
     - If `PreferRealPlayers = 1`, queued bot candidates are automatically removed from queue when demand drops (for example as real players join).
     - Auto-generation scaffolding now computes missing pool capacity and emits in-memory seed placeholders, but still needs project-specific hooks for real account/character creation + session bootstrap.

## "How do I solo queue Warsong and fight bots?" (current tree reality)

You need two parts:

1. **Queue coordinator** (already in this tree).
2. **A real bot runtime that can provide online bot sessions** (not in this tree yet).

### What to set for Warsong testing

- `Playerbot.BG.Enable = 1`
- `Playerbot.BG.AutoQueueBots = 1`
- `Playerbot.BG.AutoQueueBgType = 2` (`BATTLEGROUND_WS`, Warsong Gulch)
- `Playerbot.BG.BotNamePrefix = "bot"` (or your chosen pool prefix)
- `Playerbot.BG.PreferRealPlayers = 1` (recommended)
- `Playerbot.BG.RuntimeBootstrapProvider = "none"` unless you have registered a runtime provider

### What this gives you today

- If you already have online `bot*` characters, the system can auto-queue them to satisfy queue-start and active-BG backfill demand.
- If demand exists but there are no matching online candidates, the system logs a clear diagnostic message.

### What this does **not** give you yet

- No automatic account creation.
- No automatic character creation.
- No automatic login/session bootstrap.

`Playerbot.BG.AutoGenerateBots` is scaffold-only at the moment; it creates placeholder seed records in memory and logs where real creation/bootstrap hooks should be added.

### To get true "solo queue WSG vs bots"

Implement or integrate a provider that can do:

1. Persist/select bot account + character pool.
2. Materialize missing bots (if pool insufficient).
3. Log those bots in (headless or managed sessions).
4. Hand them to this queue coordinator as online candidates.
5. Cleanly park/logout bots when not needed.

## New runtime bootstrap hook (implemented)

This tree now includes a pluggable runtime bootstrap registry so another module can supply the missing "bring bots online" step.

- Register a provider in code:
  - `RegisterPlayerbotBGRuntimeBootstrapProvider("your-provider-name", callback)`
- Callback signature:
  - `(TeamId team, uint32 requestedCount, std::string const& botNamePrefix, BattlegroundTypeId bgTypeId) -> uint32`
- Set config:
  - `Playerbot.BG.RuntimeBootstrapProvider = "your-provider-name"`

The BG queue updater calls the provider each tick when online candidates are below queue demand. The provider is expected to materialize/log in bot sessions and return how many it brought online.

### Built-in provider: `sql_queue`

This tree now registers a built-in provider named `sql_queue`. It does not log bots in directly; it writes bootstrap jobs into a Character DB table for an external bot runtime worker to consume.

Set:

- `Playerbot.BG.RuntimeBootstrapProvider = "sql_queue"`

Required table:

```sql
CREATE TABLE IF NOT EXISTS playerbot_bg_bootstrap_queue (
  id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  requested_at DATETIME NOT NULL,
  team_id TINYINT UNSIGNED NOT NULL,
  battleground_type_id SMALLINT UNSIGNED NOT NULL,
  bot_name_prefix VARCHAR(32) NOT NULL,
  state VARCHAR(16) NOT NULL DEFAULT 'queued',
  PRIMARY KEY (id),
  KEY idx_state_requested (state, requested_at)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
```

External runtime contract:

1. Poll rows with `state='queued'`.
2. Bring bots online that satisfy team/prefix/bg constraints.
3. Update row state (for example `processing`, `done`, or `failed`).
