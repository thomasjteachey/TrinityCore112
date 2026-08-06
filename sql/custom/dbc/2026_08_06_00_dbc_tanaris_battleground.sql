-- Tanaris deathmatch battleground - DBC mirror rows.
--
-- Map 1620 is a clone of Kalimdor (map 1) reused as an instanced battleground.
--
-- Directory is "TanarisBG", a private copy of the Tanaris tiles shipped in the
-- client patch, NOT "Kalimdor". Pointing it at Kalimdor works and is how this
-- started - the client picks terrain by Directory, not by map id, the same way
-- map 1615 borrows 615's - but it means any terrain edit made for the
-- battleground would also change the live Tanaris zone. The private copy is
-- built by tools/tanaris/build_tanaris_terrain.py.
--
-- Server-side maps/vmaps/mmaps are keyed by map id, not by Directory, so they
-- are unaffected by this and still come from the Kalimdor byte-copies.
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
  (1620, 'TanarisBG', 3, 1, 1,
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
  -- 1-80: 60 is this server's level cap, so the original 60-69 bracket
  -- locked out every leveling character (widened 2026-08-06).
  (91620, 1620, 0, 1, 80, 0);

-- ------------------------------------------------------- WorldSafeLocs.dbc
-- 52500-52503: next free block after Nefarian's Arena (52410/52411).
--
-- Alliance at Southmoon Ruins, Horde at Gadgetzan -- about 2200 yards apart,
-- which is RTS scale rather than arena scale. Each team's spirit healer stands
-- on its own spawn point, so start and graveyard share a position.
--
-- LocZ is the measured ground height plus a yard so nobody spawns inside
-- terrain. Re-measure with .gps if a position moves rather than guessing.
-- Orientations live in battleground_template (AllianceStartO / HordeStartO),
-- not here.
DELETE FROM dbc.worldsafelocs_lplus WHERE ID BETWEEN 52500 AND 52503;
INSERT INTO dbc.worldsafelocs_lplus
  (ID, Continent, LocX, LocY, LocZ, AreaName_Lang_enUS, AreaName_Lang_Mask)
VALUES
  (52500, 1620, -9235.11, -3009.99, 17.19, 'Tanaris - Alliance Start',     16712190),  -- ground 16.19
  (52502, 1620, -9235.11, -3009.99, 17.19, 'Tanaris - Alliance Graveyard', 16712190),
  (52501, 1620, -7166.45, -3760.62,  9.40, 'Tanaris - Horde Start',        16712190),  -- ground 8.40
  (52503, 1620, -7166.45, -3760.62,  9.40, 'Tanaris - Horde Graveyard',    16712190);

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
-- Bounds copied verbatim from row 161 so the arrow lands in the right place.
-- AreaName selects the ART DIRECTORY: Interface\WorldMap\<AreaName>\. It is
-- "TanarisBG", not "Tanaris", because the battleground ships its own pre-baked
-- map image - see tools/tanaris/build_worldmap_art.py. Borrowing Tanaris's art
-- worked but left the map blank inside: the zone detail lives in
-- WorldMapOverlay textures, and the client matches no overlays at all to map
-- 1620 (GetNumMapOverlays() returns 0 there) however correct the rows are.
-- The baked image needs no overlays and no exploration.
--
-- This row and the 12 BLPs must be packed together, and the art goes in a
-- LOCALE patch (patch-enUS-8) because Interface/ lives in the locale MPQs.
-- Note the axes are rotated: LocLeft/LocRight are world Y, LocTop/LocBottom X.
--
-- This one is CLIENT-side. The server never reads it for battleground logic,
-- so it only takes effect once it is packed into the client patch.
DELETE FROM dbc.worldmaparea_lplus WHERE ID = 9532;
INSERT INTO dbc.worldmaparea_lplus
  (ID, MapID, AreaID, AreaName, LocLeft, LocRight, LocTop, LocBottom,
   DisplayMapID, DefaultDungeonFloor, ParentWorldMapID)
VALUES
  (9532, 1620, 440, 'TanarisBG', -218.75, -7118.75, -5875, -10475, -1, 0, 0);

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


-- ----------------------------------------------------- WorldMapOverlay.dbc
-- The explored-area artwork. Without these the world map for 1620 renders as
-- blank parchment: the arrow is in the right place, but none of the zone
-- detail is drawn, however thoroughly the player has explored Tanaris.
--
-- The reason is that WorldMapOverlay.MapAreaID is a WorldMapArea ROW ID, not a
-- map id. Stock Tanaris's 20 overlays all point at row 161, and the client is
-- now displaying row 9532, so it finds none. These are those same 20 rows with
-- MapAreaID repointed at 9532 - identical textures, offsets and hit rects, and
-- identical AreaIDs, so they light up from the player's existing exploration.
--
-- Requires 2026_08_06_01_dbc_worldmapoverlay_mirror.sql to have run first:
-- that is what creates dbc.worldmapoverlay_lplus, which did not exist.
--
-- Client-side, like WorldMapArea and WorldStateUI above.
DELETE FROM dbc.worldmapoverlay_lplus WHERE MapAreaID = 9532;
INSERT INTO dbc.worldmapoverlay_lplus
  (ID, MapAreaID, AreaID_1, AreaID_2, AreaID_3, AreaID_4, MapPointX, MapPointY,
   TextureName, TextureWidth, TextureHeight, OffsetX, OffsetY,
   HitRectTop, HitRectLeft, HitRectBottom, HitRectRight)
VALUES
  (1700, 9532, 980, 0, 0, 0, 0, 0, 'THISTLESHRUBVALLEY', 185, 250, 203, 286, 360, 245, 475, 370),
  (1701, 9532, 990, 0, 0, 0, 0, 0, 'VALLEYOFTHEWATCHERS', 150, 160, 291, 434, 490, 330, 585, 410),
  (1702, 9532, 992, 0, 0, 0, 0, 0, 'SOUTHMOONRUINS', 195, 210, 323, 359, 445, 365, 510, 440),
  (1703, 9532, 987, 0, 0, 0, 0, 0, 'LANDSENDBEACH', 205, 157, 445, 511, 565, 480, 660, 620),
  (1704, 9532, 984, 0, 0, 0, 0, 0, 'EASTMOONRUINS', 160, 150, 395, 346, 410, 435, 460, 500),
  (1705, 9532, 981, 0, 0, 0, 0, 0, 'THEGAPINGCHASM', 220, 210, 449, 372, 415, 490, 530, 600),
  (1706, 9532, 1940, 0, 0, 0, 0, 0, 'SOUTHBREAKSHORE', 215, 175, 499, 293, 365, 570, 430, 680),
  (1707, 9532, 983, 0, 0, 0, 0, 0, 'DUNEMAULCOMPOUND', 205, 145, 325, 289, 335, 380, 415, 460),
  (1708, 9532, 982, 0, 0, 0, 0, 0, 'THENOXIOUSLAIR', 180, 200, 252, 199, 255, 310, 355, 385),
  (1709, 9532, 1938, 0, 0, 0, 0, 0, 'BROKENPILLAR', 110, 180, 473, 234, 275, 500, 335, 565),
  (1710, 9532, 1939, 0, 0, 0, 0, 0, 'ABYSSALSANDS', 215, 180, 363, 194, 240, 410, 330, 505),
  (1711, 9532, 985, 0, 0, 0, 0, 0, 'WATERSPRINGFIELD', 165, 180, 509, 168, 210, 550, 295, 645),
  (1712, 9532, 1336, 0, 0, 0, 0, 0, 'LOSTRIGGERCOVE', 160, 190, 629, 220, 255, 675, 375, 760),
  (1713, 9532, 986, 0, 0, 0, 0, 0, 'ZALASHJISDEN', 110, 140, 611, 147, 190, 640, 255, 700),
  (1714, 9532, 977, 0, 0, 0, 0, 0, 'STEAMWHEEDLEPORT', 155, 150, 592, 75, 110, 630, 175, 710),
  (1715, 9532, 1937, 0, 0, 0, 0, 0, 'NOONSHADERUINS', 120, 135, 533, 104, 140, 570, 190, 630),
  (1716, 9532, 2300, 0, 0, 0, 0, 0, 'CAVERNSOFTIME', 155, 150, 561, 256, 295, 595, 365, 690),
  (1717, 9532, 976, 0, 0, 0, 0, 0, 'GADGETZAN', 175, 165, 421, 91, 140, 460, 225, 565),
  (1718, 9532, 979, 0, 0, 0, 0, 0, 'SANDSORROWWATCH', 195, 175, 299, 100, 155, 365, 225, 435),
  (1719, 9532, 978, 0, 0, 0, 0, 0, 'ZULFARRAK', 210, 175, 254, 0, 25, 300, 145, 450);
