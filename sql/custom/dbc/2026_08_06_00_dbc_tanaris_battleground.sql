-- Tanaris deathmatch battleground - DBC mirror rows.
--
-- Map 1620 is a clone of Kalimdor (map 1) reused as an instanced battleground.
-- Directory stays "Kalimdor" on purpose: the client picks terrain by Directory,
-- not by map id, so the cloned continent needs no new client terrain files.
-- Map 1615 (Obsidian Colosseum) does the same thing with 615.
--
-- The dbc.*_lplus mirrors are SHARED between the dev and prod realms - there is
-- no *_lplusdev flavour - so these rows become visible to prod as soon as prod
-- restarts. Every id here is new and unused, so nothing existing is disturbed.
--
-- This script only writes the SQL mirrors. The binary .dbc files are a SEPARATE
-- source of truth and drift silently if they are not written too; use
-- tools/tanaris/tanaris_dbc.py for those (see the header comment there).
--
-- Replayable: each section deletes its own ids before inserting.

-- Backups, taken once. IF NOT EXISTS keeps a replay from overwriting the
-- original snapshot with already-modified data.
CREATE TABLE IF NOT EXISTS dbc.map_lplus_bak_tanaris              AS SELECT * FROM dbc.map_lplus;
CREATE TABLE IF NOT EXISTS dbc.areatable_lplus_bak_tanaris        AS SELECT * FROM dbc.areatable_lplus;
CREATE TABLE IF NOT EXISTS dbc.battlemasterlist_lplus_bak_tanaris AS SELECT * FROM dbc.battlemasterlist_lplus;
CREATE TABLE IF NOT EXISTS dbc.pvpdifficulty_lplus_bak_tanaris    AS SELECT * FROM dbc.pvpdifficulty_lplus;
CREATE TABLE IF NOT EXISTS dbc.worldsafelocs_lplus_bak_tanaris    AS SELECT * FROM dbc.worldsafelocs_lplus;
CREATE TABLE IF NOT EXISTS dbc.worldmaparea_lplus_bak_tanaris     AS SELECT * FROM dbc.worldmaparea_lplus;

-- ---------------------------------------------------------------- Map.dbc
DELETE FROM dbc.map_lplus WHERE ID = 1620;
INSERT INTO dbc.map_lplus
  (ID, Directory, InstanceType, Flags, PVP,
   MapName_Lang_enUS, MapName_Lang_Mask, AreaTableID,
   MapDescription0_Lang_enUS, MapDescription0_Lang_Mask,
   MapDescription1_Lang_enUS, MapDescription1_Lang_Mask,
   LoadingScreenID, MinimapIconScale,
   CorpseMapID, CorpseX, CorpseY,
   TimeOfDayOverride, ExpansionID, RaidOffset, MaxPlayers)
VALUES
  (1620, 'Kalimdor', 3, 1, 1,
   'Tanaris Deathmatch', 16712190, 30234,
   'A deathmatch in the deep desert of Tanaris. First team to the kill limit wins.', 16712188,
   'A deathmatch in the deep desert of Tanaris. First team to the kill limit wins.', 16712188,
   3, 1,
   1, -7177, -3785,
   -1, 0, 0, 20);

-- ----------------------------------------------------------- AreaTable.dbc
-- Only referenced as Map.dbc.AreaTableID. The zone names players actually see
-- come from the area ids baked into the Kalimdor ADTs (440 Tanaris and its
-- children), which already exist and are untouched.
-- AreaBit 3717: highest in use was 3716, and the client's explored-zones
-- bitfield holds 4096 bits, so this is in range.
-- The sound fields are copied from real Tanaris (area 440) so it sounds right.
DELETE FROM dbc.areatable_lplus WHERE ID = 30234;
INSERT INTO dbc.areatable_lplus
  (ID, ContinentID, ParentAreaID, AreaBit, Flags,
   SoundProviderPref, SoundProviderPrefUnderwater, AmbienceID, ZoneMusic, IntroSound,
   ExplorationLevel, AreaName_Lang_enUS, AreaName_Lang_Mask, FactionGroupMask,
   LiquidTypeID_1, LiquidTypeID_2, LiquidTypeID_3, LiquidTypeID_4,
   MinElevation, Ambient_Multiplier, Lightid)
VALUES
  (30234, 1620, 0, 3717, 0,
   0, 11, 39, 176, 0,
   0, 'Tanaris Deathmatch', 16712190, 0,
   0, 0, 0, 0,
   -500, 0.5, 0);

-- ---------------------------------------------------- BattlemasterList.dbc
-- ID must equal the BattlegroundTypeId (BATTLEGROUND_TRT = 104).
DELETE FROM dbc.battlemasterlist_lplus WHERE ID = 104;
INSERT INTO dbc.battlemasterlist_lplus
  (ID, MapID_1, MapID_2, MapID_3, MapID_4, MapID_5, MapID_6, MapID_7, MapID_8,
   InstanceType, GroupsAllowed, Name_Lang_enUS, Name_Lang_Mask,
   MaxGroupSize, HolidayWorldState, Minlevel, Maxlevel)
VALUES
  (104, 1620, -1, -1, -1, -1, -1, -1, -1,
   3, 1, 'Tanaris', 16712190,
   10, 1942, 10, 80);

-- ------------------------------------------------------- PvpDifficulty.dbc
-- Miss this and the queue silently refuses everyone:
-- GetBattlegroundBracketByLevel finds no bracket and fails without logging.
-- Id follows the existing 9<mapid> convention (91615 is the Obsidian Colosseum).
DELETE FROM dbc.pvpdifficulty_lplus WHERE MapID = 1620;
INSERT INTO dbc.pvpdifficulty_lplus
  (ID, MapID, RangeIndex, MinLevel, MaxLevel, Difficulty)
VALUES
  (91620, 1620, 0, 60, 69, 0);

-- ------------------------------------------------------- WorldSafeLocs.dbc
-- 52500-52503: next free block after Nefarian's Arena (52410/52411).
--
-- Laid out symmetrically about the arena centre (-8365, -3010), 180 yards
-- apart, on the level desert shelf in north-west Tanaris.
--
-- Each LocZ is the real terrain height sampled from the server's own
-- 16204737.map tile at that point, plus one yard so nobody spawns inside the
-- ground. They differ because the shelf is level but not perfectly flat. If a
-- position moves, re-measure rather than copying a neighbour's value.
DELETE FROM dbc.worldsafelocs_lplus WHERE ID BETWEEN 52500 AND 52503;
INSERT INTO dbc.worldsafelocs_lplus
  (ID, Continent, LocX, LocY, LocZ, AreaName_Lang_enUS, AreaName_Lang_Mask)
VALUES
  (52500, 1620, -8455, -3010,  9.6, 'Tanaris - Alliance Start',      16712190),  -- ground 8.63
  (52501, 1620, -8275, -3010, 11.0, 'Tanaris - Horde Start',         16712190),  -- ground 10.01
  (52502, 1620, -8470, -3010, 10.2, 'Tanaris - Alliance Graveyard',  16712190),  -- ground 9.23
  (52503, 1620, -8260, -3010,  9.7, 'Tanaris - Horde Graveyard',     16712190);  -- ground 8.69

-- -------------------------------------------------------- WorldMapArea.dbc
-- Without this the world map draws the Tanaris artwork correctly but never
-- draws the player arrow. The client only positions the blob when the
-- displayed area's MapID equals the map the player is standing on, and stock
-- Tanaris (row 161) says MapID 1.
--
-- AreaID stays 440 rather than the custom area 30234: the client works out the
-- current area from the Kalimdor ADTs it loaded, and those say 440. That leaves
-- two rows sharing AreaID 440, which is exactly what the Obsidian Colosseum
-- already does with 4493 (row 531 on map 615, row 9531 on map 1615) - the
-- client disambiguates on MapID, so the real Tanaris map is unaffected.
--
-- Bounds copied verbatim from row 161 so the arrow lands in the right place on
-- the same artwork; AreaName is what selects that artwork. Note the axes are
-- rotated: LocLeft/LocRight are world Y, LocTop/LocBottom are world X.
--
-- This one is CLIENT-side. The server never reads it for battleground logic,
-- so it only takes effect once it is packed into the client patch.
DELETE FROM dbc.worldmaparea_lplus WHERE ID = 9532;
INSERT INTO dbc.worldmaparea_lplus
  (ID, MapID, AreaID, AreaName, LocLeft, LocRight, LocTop, LocBottom,
   DisplayMapID, DefaultDungeonFloor, ParentWorldMapID)
VALUES
  (9532, 1620, 440, 'Tanaris', -218.75, -7118.75, -5875, -10475, -1, 0, 0);

-- -------------------------------------------------------- WorldStateUI.dbc
-- The top-frame score readout. Without these two rows the server still sends
-- the world states and the client still stores them, but nothing is declared
-- to display them, so the battleground shows no score at all.
--
-- Shape copied from Scarlet Chapel's pair (90002/90003). "%9300w" substitutes
-- the live value of that world state into the string, so these read
-- "<team kills>/<kill limit>" - see BG_TRT_WorldStates in BattlegroundTRT.h.
--
-- StateVariable 0 means "always visible", which is what a plain deathmatch
-- wants. The Obsidian Colosseum sets it instead, because its rows are the
-- mutually exclusive flag-carrier indicators rather than a permanent score.
--
-- AreaID 0 means "anywhere on this map", which matters here because the arena
-- reports stock Tanaris (440) rather than a custom area id.
--
-- Client-side, like WorldMapArea above: no effect until it is packed.
DELETE FROM dbc.worldstateui_lplus WHERE ID IN (90023, 90024);
INSERT INTO dbc.worldstateui_lplus
  (ID, MapID, AreaID, PhaseShift, Icon,
   String_Lang_enUS, String_Lang_Mask, Tooltip_Lang_Mask,
   StateVariable, Type, DynamicIcon, DynamicTooltip_Lang_Mask,
   ExtendedUI, ExtendedUIStateVariable_1, ExtendedUIStateVariable_2, ExtendedUIStateVariable_3)
VALUES
  -- NOTE the doubled backslashes: MySQL treats a single backslash as an escape
  -- introducer, so 'Interface\T...' silently becomes 'InterfaceT...' and the
  -- icon just fails to load. They must survive into the column as single ones.
  (90023, 1620, 0, 0, 'Interface\\TargetingFrame\\UI-PVP-Alliance',
   '%9300w/%9306w', 16712190, 16712190,
   0, 0, '', 16712188,
   '', 0, 0, 0),
  (90024, 1620, 0, 0, 'Interface\\TargetingFrame\\UI-PVP-Horde',
   '%9301w/%9306w', 16712190, 16712190,
   0, 0, '', 16712188,
   '', 0, 0, 0);
