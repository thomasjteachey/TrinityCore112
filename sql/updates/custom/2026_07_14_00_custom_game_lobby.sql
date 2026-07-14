-- Private custom battleground/arena lobby NPCs.
-- Only the host is a persistent world spawn. The three staging-room NPCs are
-- summoned independently inside each server-only Map 1 subinstance.

DELETE FROM `creature` WHERE `id` = 900001;
DELETE FROM `creature_template` WHERE `entry` IN (900001, 900002, 900003, 900004);

-- Clone full templates through a temporary table so this remains compatible
-- when the active world schema adds creature_template columns.
DROP TEMPORARY TABLE IF EXISTS `custom_game_template`;
CREATE TEMPORARY TABLE `custom_game_template` LIKE `creature_template`;

INSERT INTO `custom_game_template` SELECT * FROM `creature_template` WHERE `entry` = 27856;
UPDATE `custom_game_template` SET
    `entry` = 900001, `name` = 'Custom Gamesmaster', `subname` = 'Private Battlegrounds and Arenas',
    `gossip_menu_id` = 0, `minlevel` = 80, `maxlevel` = 80, `exp` = 0, `faction` = 35, `npcflag` = 1,
    `AIName` = '', `MovementType` = 0, `ScriptName` = 'custom_game_lobby_npc';
INSERT INTO `creature_template` SELECT * FROM `custom_game_template`;

TRUNCATE TABLE `custom_game_template`;
INSERT INTO `custom_game_template` SELECT * FROM `creature_template` WHERE `entry` = 15351;
UPDATE `custom_game_template` SET
    `entry` = 900002, `name` = 'Blue Team Captain', `subname` = 'Custom Game Team Blue',
    `gossip_menu_id` = 0, `minlevel` = 80, `maxlevel` = 80, `exp` = 0, `faction` = 35, `npcflag` = 1,
    `AIName` = '', `MovementType` = 0, `ScriptName` = 'custom_game_lobby_npc';
INSERT INTO `creature_template` SELECT * FROM `custom_game_template`;

TRUNCATE TABLE `custom_game_template`;
INSERT INTO `custom_game_template` SELECT * FROM `creature_template` WHERE `entry` = 32615;
UPDATE `custom_game_template` SET
    `entry` = 900003, `name` = 'Red Team Captain', `subname` = 'Custom Game Team Red',
    `gossip_menu_id` = 0, `minlevel` = 80, `maxlevel` = 80, `exp` = 0, `faction` = 35, `npcflag` = 1,
    `AIName` = '', `MovementType` = 0, `ScriptName` = 'custom_game_lobby_npc';
INSERT INTO `creature_template` SELECT * FROM `custom_game_template`;

TRUNCATE TABLE `custom_game_template`;
INSERT INTO `custom_game_template` SELECT * FROM `creature_template` WHERE `entry` = 27856;
UPDATE `custom_game_template` SET
    `entry` = 900004, `name` = 'Chromie', `subname` = 'Custom Game Controller',
    `gossip_menu_id` = 0, `minlevel` = 80, `maxlevel` = 80, `exp` = 0, `faction` = 35, `npcflag` = 1,
    `AIName` = '', `MovementType` = 0, `ScriptName` = 'custom_game_lobby_npc';
INSERT INTO `creature_template` SELECT * FROM `custom_game_template`;

DROP TEMPORARY TABLE `custom_game_template`;

SET @CUSTOM_GAME_HOST_GUID := (SELECT COALESCE(MAX(`guid`), 0) + 1 FROM `creature`);
INSERT INTO `creature` (`guid`,`id`,`map`,`zoneId`,`areaId`,`spawnMask`,`phaseMask`,`modelid`,`equipment_id`,`position_x`,`position_y`,`position_z`,`orientation`,`spawntimesecs`,`wander_distance`,`currentwaypoint`,`curhealth`,`curmana`,`MovementType`,`npcflag`,`unit_flags`,`dynamicflags`,`ScriptName`,`VerifiedBuild`) VALUES
(@CUSTOM_GAME_HOST_GUID,900001,0,33,0,1,1,0,0,-13235.707031,214.336441,31.276190,1.010225,300,0,0,1,0,0,0,0,0,'',0);
