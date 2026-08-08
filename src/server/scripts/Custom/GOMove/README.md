# GOMove

Gameobject editor by Rochet2 (original idea by Mordred), integrated from
https://github.com/Rochet2/TrinityCore/tree/gomove_3.3.5

Select, spawn, move, rotate and delete gameobjects from an in-game addon UI:
compass-direction nudging, ground/floor snapping, favourites list, multi-select
by radius, respawn, go-to, and phase editing.

## Server side

- `src/server/game/Entities/GameObject/GOMove.{h,cpp}` — spawn/move/delete helpers + addon messaging (game lib).
- `src/server/scripts/Custom/GOMove/GOMoveScripts.cpp` — the `.gomove` command, spell-place spellscript, logout cleanup.
- Registered via `AddSC_GOMove_commandscript()` in `custom_script_loader.cpp`.
- SQL: `sql/custom/world/2026_08_07_02_world_gomove.sql` (command help + optional `GOMove_spell_place` binding to spell 27651).
- Uses existing RBAC permission 390 (`RBAC_PERM_COMMAND_GOBJECT_ADD_TEMP`) — no new permissions needed.

## Client side

Copy the addon folder `GOMove/` (the one inside this directory containing the
.toc/.lua/.xml) to `WowInstallFolder\Interface\AddOns\GOMove`.

In game: `/gomove` toggles the UI (minimap "G" button too), `/gomove help`
lists addon commands, `/gomove reset` resets frame positions.

Note: only objects showing a real numeric guid in the selection list are saved
to DB; hex guids are temporary objects.
