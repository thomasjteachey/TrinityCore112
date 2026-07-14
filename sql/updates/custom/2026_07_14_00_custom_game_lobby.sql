-- Private custom battleground/arena lobby NPCs.
-- Only the host is a persistent world spawn. The three staging-room NPCs are
-- summoned independently inside each server-only Map 1 subinstance.

DELETE FROM `creature` WHERE `id` = 900001;
DELETE FROM `creature_template` WHERE `entry` IN (900001, 900002, 900003, 900004);

INSERT INTO `creature_template` (`entry`, `modelid1`, `name`, `subname`, `gossip_menu_id`, `minlevel`, `maxlevel`, `exp`, `faction`, `npcflag`, `speed_walk`, `speed_run`, `scale`, `rank`, `dmgschool`, `BaseAttackTime`, `RangeAttackTime`, `BaseVariance`, `RangeVariance`, `unit_class`, `unit_flags`, `unit_flags2`, `dynamicflags`, `family`, `type`, `type_flags`, `AIName`, `MovementType`, `HoverHeight`, `HealthModifier`, `ManaModifier`, `ArmorModifier`, `DamageModifier`, `ExperienceModifier`, `RacialLeader`, `movementId`, `RegenHealth`, `flags_extra`, `ScriptName`)
SELECT 900001, `modelid1`, 'Custom Gamesmaster', 'Private Battlegrounds and Arenas', 0, 80, 80, 0, 35, 1, 1, 1.14286, 1, 0, 0, 2000, 2000, 1, 1, 1, 0, 0, 0, 0, 7, 0, '', 0, 1, 1, 1, 1, 1, 0, 0, 1, 0, 'custom_game_lobby_npc'
FROM `creature_template` WHERE `entry` = 27856;

INSERT INTO `creature_template` (`entry`, `modelid1`, `name`, `subname`, `gossip_menu_id`, `minlevel`, `maxlevel`, `exp`, `faction`, `npcflag`, `speed_walk`, `speed_run`, `scale`, `rank`, `dmgschool`, `BaseAttackTime`, `RangeAttackTime`, `BaseVariance`, `RangeVariance`, `unit_class`, `unit_flags`, `unit_flags2`, `dynamicflags`, `family`, `type`, `type_flags`, `AIName`, `MovementType`, `HoverHeight`, `HealthModifier`, `ManaModifier`, `ArmorModifier`, `DamageModifier`, `ExperienceModifier`, `RacialLeader`, `movementId`, `RegenHealth`, `flags_extra`, `ScriptName`)
SELECT 900002, `modelid1`, 'Blue Team Captain', 'Custom Game Team Blue', 0, 80, 80, 0, 35, 1, 1, 1.14286, 1, 0, 0, 2000, 2000, 1, 1, 1, 0, 0, 0, 0, 7, 0, '', 0, 1, 1, 1, 1, 1, 0, 0, 1, 0, 'custom_game_lobby_npc'
FROM `creature_template` WHERE `entry` = 15351;

INSERT INTO `creature_template` (`entry`, `modelid1`, `name`, `subname`, `gossip_menu_id`, `minlevel`, `maxlevel`, `exp`, `faction`, `npcflag`, `speed_walk`, `speed_run`, `scale`, `rank`, `dmgschool`, `BaseAttackTime`, `RangeAttackTime`, `BaseVariance`, `RangeVariance`, `unit_class`, `unit_flags`, `unit_flags2`, `dynamicflags`, `family`, `type`, `type_flags`, `AIName`, `MovementType`, `HoverHeight`, `HealthModifier`, `ManaModifier`, `ArmorModifier`, `DamageModifier`, `ExperienceModifier`, `RacialLeader`, `movementId`, `RegenHealth`, `flags_extra`, `ScriptName`)
SELECT 900003, `modelid1`, 'Red Team Captain', 'Custom Game Team Red', 0, 80, 80, 0, 35, 1, 1, 1.14286, 1, 0, 0, 2000, 2000, 1, 1, 1, 0, 0, 0, 0, 7, 0, '', 0, 1, 1, 1, 1, 1, 0, 0, 1, 0, 'custom_game_lobby_npc'
FROM `creature_template` WHERE `entry` = 32615;

INSERT INTO `creature_template` (`entry`, `modelid1`, `name`, `subname`, `gossip_menu_id`, `minlevel`, `maxlevel`, `exp`, `faction`, `npcflag`, `speed_walk`, `speed_run`, `scale`, `rank`, `dmgschool`, `BaseAttackTime`, `RangeAttackTime`, `BaseVariance`, `RangeVariance`, `unit_class`, `unit_flags`, `unit_flags2`, `dynamicflags`, `family`, `type`, `type_flags`, `AIName`, `MovementType`, `HoverHeight`, `HealthModifier`, `ManaModifier`, `ArmorModifier`, `DamageModifier`, `ExperienceModifier`, `RacialLeader`, `movementId`, `RegenHealth`, `flags_extra`, `ScriptName`)
SELECT 900004, `modelid1`, 'Chromie', 'Custom Game Controller', 0, 80, 80, 0, 35, 1, 1, 1.14286, 1, 0, 0, 2000, 2000, 1, 1, 1, 0, 0, 0, 0, 7, 0, '', 0, 1, 1, 1, 1, 1, 0, 0, 1, 0, 'custom_game_lobby_npc'
FROM `creature_template` WHERE `entry` = 27856;

SET @CUSTOM_GAME_HOST_GUID := (SELECT COALESCE(MAX(`guid`), 0) + 1 FROM `creature`);
INSERT INTO `creature` (`guid`,`id`,`map`,`zoneId`,`areaId`,`spawnMask`,`phaseMask`,`modelid`,`equipment_id`,`position_x`,`position_y`,`position_z`,`orientation`,`spawntimesecs`,`wander_distance`,`currentwaypoint`,`curhealth`,`curmana`,`MovementType`,`npcflag`,`unit_flags`,`dynamicflags`,`ScriptName`,`VerifiedBuild`) VALUES
(@CUSTOM_GAME_HOST_GUID,900001,0,33,0,1,1,0,0,-13235.707031,214.336441,31.276190,1.010225,300,0,0,1,0,0,0,0,0,'',0);
