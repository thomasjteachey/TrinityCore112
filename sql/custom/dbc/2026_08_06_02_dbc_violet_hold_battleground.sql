-- Violet Hold survival battleground - DBC mirror rows.
--
-- Map 1608 is a clone of the Violet Hold (map 608) reused as an instanced
-- wave-survival battleground (BATTLEGROUND_VHR = 105).
--
-- Directory stays "DalaranPrison", the stock instance's own folder: the client
-- picks terrain by Directory, not map id, so the same trick map 1615 uses with
-- 615 applies. The client patch overrides the shared WMO group's classification
-- as exterior; all ADT MCNKs continue to resolve through stock area 4415.
-- Server-side
-- maps/vmaps/mmaps are keyed by map id and are byte-copies of 608's
-- (tools/violet_hold/clone_map_data.sh).
--
-- The dbc.*_lplus mirrors are SHARED between the dev and prod realms - there is
-- no *_lplusdev flavour - so these rows become visible to prod as soon as prod
-- restarts. Every id here is new and unused, so nothing existing is disturbed.
--
-- This script only writes the SQL mirrors. The binary .dbc files are a SEPARATE
-- source of truth and drift silently if they are not written too; use
-- tools/violet_hold/vhr_dbc.py for those.
--
-- Replayable: each section deletes its own ids before inserting.

-- Backups, taken once. IF NOT EXISTS keeps a replay from overwriting the
-- original snapshot with already-modified data.
CREATE TABLE IF NOT EXISTS dbc.map_lplus_bak_vhr              AS SELECT * FROM dbc.map_lplus;
CREATE TABLE IF NOT EXISTS dbc.areatable_lplus_bak_vhr        AS SELECT * FROM dbc.areatable_lplus;
CREATE TABLE IF NOT EXISTS dbc.battlemasterlist_lplus_bak_vhr AS SELECT * FROM dbc.battlemasterlist_lplus;
CREATE TABLE IF NOT EXISTS dbc.pvpdifficulty_lplus_bak_vhr    AS SELECT * FROM dbc.pvpdifficulty_lplus;
CREATE TABLE IF NOT EXISTS dbc.worldsafelocs_lplus_bak_vhr    AS SELECT * FROM dbc.worldsafelocs_lplus;
CREATE TABLE IF NOT EXISTS dbc.worldmaparea_lplus_bak_vhr     AS SELECT * FROM dbc.worldmaparea_lplus;
CREATE TABLE IF NOT EXISTS dbc.dungeonmap_lplus_bak_vhr       AS SELECT * FROM dbc.dungeonmap_lplus;
CREATE TABLE IF NOT EXISTS dbc.dungeonmapchunk_lplus_bak_vhr  AS SELECT * FROM dbc.dungeonmapchunk_lplus;

-- ---------------------------------------------------------------- Map.dbc
-- Shape copied from map 608 with the battleground fields changed:
-- InstanceType 3 (battleground), PVP 1, Flags 1 (608's raid-style flags 29 are
-- not wanted on a battleground; 1 matches the other custom BG clones).
-- ExpansionID 0 rather than 608's 2 so account expansion gating can never
-- refuse entry. CorpseMapID -1: corpses stay put - the mode has no graveyards
-- at all, on purpose (see WorldSafeLocs below).
-- MaxPlayers 50: up to 10 humans plus a full 40-clone final wave.
DELETE FROM dbc.map_lplus WHERE ID = 1608;
INSERT INTO dbc.map_lplus
  (ID, Directory, InstanceType, Flags, PVP,
   MapName_Lang_enUS, MapName_Lang_Mask, AreaTableID,
   MapDescription0_Lang_enUS, MapDescription0_Lang_Mask,
   MapDescription1_Lang_enUS, MapDescription1_Lang_Mask,
   LoadingScreenID, MinimapIconScale,
   CorpseMapID, CorpseX, CorpseY,
   TimeOfDayOverride, ExpansionID, RaidOffset, MaxPlayers)
VALUES
  (1608, 'DalaranPrison', 3, 1, 1,
   'The Violet Hold Gauntlet', 16712190, 30608,
   'Hold the line against wave after wave of dark reflections. Every wave adds one more.', 16712188,
   'Hold the line against wave after wave of dark reflections. Every wave adds one more.', 16712188,
   235, 1,
   -1, 0, 0,
   -1, 0, 0, 50);

-- ----------------------------------------------------------- AreaTable.dbc
-- Only referenced as Map.dbc.AreaTableID. The zone id players actually report
-- comes from the WMO area data in the byte-copied map files, which still says
-- 4415 (stock Violet Hold) - the same way map 1620 reports plain Tanaris. The
-- server code accounts for this (Player::SendInitWorldStates case 4415).
-- Both 4415 and this custom fallback are normalized to AREA_FLAG_OUTSIDE by
-- the dedicated outdoor migration. Shape copied from area 4415; AreaBit 3718 is the next free bit after
-- Tanaris's 3717 (the client's explored-zones bitfield holds 4096).
DELETE FROM dbc.areatable_lplus WHERE ID = 30608;
INSERT INTO dbc.areatable_lplus
  (ID, ContinentID, ParentAreaID, AreaBit, Flags,
   SoundProviderPref, SoundProviderPrefUnderwater, AmbienceID, ZoneMusic, IntroSound,
   ExplorationLevel, AreaName_Lang_enUS, AreaName_Lang_Mask, FactionGroupMask,
   LiquidTypeID_1, LiquidTypeID_2, LiquidTypeID_3, LiquidTypeID_4,
   MinElevation, Ambient_Multiplier, Lightid)
VALUES
  (30608, 1608, 0, 3718, 67108864,
   0, 0, 0, 0, 0,
   0, 'The Violet Hold Gauntlet', 16712190, 0,
   0, 0, 0, 0,
   -500, 0, 0);

-- ---------------------------------------------------- BattlemasterList.dbc
-- ID must equal the BattlegroundTypeId (BATTLEGROUND_VHR = 105).
-- MaxGroupSize 10 is the "party/raid of up to 10" cap: the group-queue check
-- (Group::CanJoinBattlegroundQueue) reads it directly.
DELETE FROM dbc.battlemasterlist_lplus WHERE ID = 105;
INSERT INTO dbc.battlemasterlist_lplus
  (ID, MapID_1, MapID_2, MapID_3, MapID_4, MapID_5, MapID_6, MapID_7, MapID_8,
   InstanceType, GroupsAllowed, Name_Lang_enUS, Name_Lang_Mask,
   MaxGroupSize, HolidayWorldState, Minlevel, Maxlevel)
VALUES
  (105, 1608, -1, -1, -1, -1, -1, -1, -1,
   3, 1, 'The Violet Hold', 16712190,
   10, 0, 10, 80);

-- ------------------------------------------------------- PvpDifficulty.dbc
-- Miss this and the queue silently refuses everyone:
-- GetBattlegroundBracketByLevel finds no bracket and fails without logging.
-- Id follows the existing 9<mapid> convention. One 1-80 bracket: 60 is this
-- server's level cap, so the older custom BGs' 60-69 shape would lock out
-- every leveling character.
DELETE FROM dbc.pvpdifficulty_lplus WHERE MapID = 1608;
INSERT INTO dbc.pvpdifficulty_lplus
  (ID, MapID, RangeIndex, MinLevel, MaxLevel, Difficulty)
VALUES
  (91608, 1608, 0, 1, 80, 0);

-- ------------------------------------------------------- WorldSafeLocs.dbc
-- 52520/52521: next free block after Tanaris (52500-52503).
--
-- 52520 is the party's start: the entrance landing just inside the prison
-- seal, where Sinclari's intro stands in the stock instance. 52521 is the
-- enemy team's nominal start in the middle of the chamber floor - clones are
-- placed there for one frame before the wave driver moves each one into its
-- assigned cell, so it only needs to be somewhere valid.
--
-- Deliberately NO graveyard entries and no graveyard_zone rows: death in the
-- gauntlet is final for the run. BattlegroundVHR::GetClosestGraveyard returns
-- nothing, so a released ghost simply stays in the chamber.
DELETE FROM dbc.worldsafelocs_lplus WHERE ID IN (52520, 52521);
INSERT INTO dbc.worldsafelocs_lplus
  (ID, Continent, LocX, LocY, LocZ, AreaName_Lang_enUS, AreaName_Lang_Mask)
VALUES
  (52520, 1608, 1848.03, 804.62, 44.07, 'Violet Hold - Defenders', 16712190),
  (52521, 1608, 1886.25, 803.07, 38.42, 'Violet Hold - Assault',   16712190);

-- -------------------------------------------------------- WorldMapArea.dbc
-- The in-instance map. Row 536 declares the stock Violet Hold map for MapID
-- 608; without a matching row for 1608 the client falls back to the Northrend
-- continent map and never draws the player arrow. Same bounds, same AreaID
-- (the client disambiguates on MapID, as OBC proved with its duplicated 4493),
-- same "VioletHold" art directory - the stock art is reused untouched.
DELETE FROM dbc.worldmaparea_lplus WHERE ID = 9533;
INSERT INTO dbc.worldmaparea_lplus
  (ID, MapID, AreaID, AreaName, LocLeft, LocRight, LocTop, LocBottom,
   DisplayMapID, DefaultDungeonFloor, ParentWorldMapID)
VALUES
  (9533, 1608, 4415, 'VioletHold', 983.333, 600, 2006.25, 1750, -1, 0, 0);

-- ---------------------------------------------------------- DungeonMap.dbc
-- The dungeon-floor image the client draws inside an instance. Stock row 52
-- covers map 608 floor 1; map 1608 needs its own or the floor view is blank.
-- 1101 is the next free id after the stock table's 1100.
DELETE FROM dbc.dungeonmap_lplus WHERE MapID = 1608;
INSERT INTO dbc.dungeonmap_lplus
  (ID, MapID, FloorIndex, MinX, MaxX, MinY, MaxY, ParentWorldMapID)
VALUES
  (1101, 1608, 1, 665.347, 921.576, 1813.35, 1984.17, 504);

-- ----------------------------------------------------- DungeonMapChunk.dbc
-- Binds the hold's WMO group (25154) to the floor above so the client knows
-- which floor a player inside the building is on. One stock row exists for
-- map 608; this is its 1608 twin. 51002 is the next free id after 51001.
DELETE FROM dbc.dungeonmapchunk_lplus WHERE MapID = 1608;
INSERT INTO dbc.dungeonmapchunk_lplus
  (ID, MapID, WmoGroupID, DungeonMapID, MinZ)
VALUES
  (51002, 1608, 25154, 1101, -10000);

-- -------------------------------------------------------- WorldStateUI.dbc
-- The top-frame readout: party members left, enemies left, and current wave.
-- Without these rows the server still sends the world states and the client
-- still stores them, but nothing is declared to display them.
--
-- "%9401w" substitutes the live value of that world state into the string -
-- see BG_VHR_WorldStates in BattlegroundVHR.h. The format copies the custom
-- arenas' rows ("Green Team: %3600w Players Remaining"): plain labelled text,
-- no icon. StateVariable 9400 is this battleground's own show flag, sent as 1
-- for the whole match. AreaID 0 = anywhere on the map, which matters because
-- the hold reports stock area 4415 rather than the custom area id.
--
-- Client-side: no effect until packed into the client patch.
DELETE FROM dbc.worldstateui_lplus WHERE ID IN (90025, 90026, 90027);
INSERT INTO dbc.worldstateui_lplus
  (ID, MapID, AreaID, PhaseShift, Icon,
   String_Lang_enUS, String_Lang_Mask, Tooltip_Lang_enUS, Tooltip_Lang_Mask,
   StateVariable, Type, DynamicIcon, DynamicTooltip_Lang_Mask,
   ExtendedUI, ExtendedUIStateVariable_1, ExtendedUIStateVariable_2, ExtendedUIStateVariable_3)
VALUES
  (90025, 1608, 0, 0, '',
   '%9401w Players Remaining', 16712190, '', 16712190,
   9400, 0, '', 16712188,
   '', 0, 0, 0),
  (90026, 1608, 0, 0, '',
   '%9402w Memories Remaining', 16712190, '', 16712190,
   9400, 0, '', 16712188,
   '', 0, 0, 0),
  (90027, 1608, 0, 0, '',
   'Wave: %9403w', 16712190, '', 16712190,
   9400, 0, '', 16712188,
   '', 0, 0, 0);
