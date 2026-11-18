-- World templates used by the 2026-11-18 Violet Hold class artifacts.
-- This migration is replayable in each realm world schema.

DELETE FROM `creature_template` WHERE `entry` = 900120;
DROP TEMPORARY TABLE IF EXISTS `tmp_vhr_mirror_caster`;
CREATE TEMPORARY TABLE `tmp_vhr_mirror_caster` AS
SELECT * FROM `creature_template` WHERE `entry` = 31216;
UPDATE `tmp_vhr_mirror_caster` SET
  `entry`=900120,
  `name`='Violet Hold Mirror Image',
  `subname`='',
  `npcflag`=0,
  `unit_flags`=33554434,
  `dynamicflags`=0,
  `AIName`='',
  `ScriptName`='';
INSERT INTO `creature_template` SELECT * FROM `tmp_vhr_mirror_caster`;
DROP TEMPORARY TABLE `tmp_vhr_mirror_caster`;

DELETE FROM `gameobject_template` WHERE `entry` IN (900010, 900011);
DROP TEMPORARY TABLE IF EXISTS `tmp_vhr_war_banner`;
CREATE TEMPORARY TABLE `tmp_vhr_war_banner` AS
SELECT * FROM `gameobject_template` WHERE `entry` = 152079;
UPDATE `tmp_vhr_war_banner` SET
  `entry`=900010, `type`=5, `displayId`=5771,
  `name`='Violet Hold War Banner (Alliance)', `ScriptName`='';
INSERT INTO `gameobject_template` SELECT * FROM `tmp_vhr_war_banner`;
UPDATE `tmp_vhr_war_banner` SET
  `entry`=900011, `type`=5, `displayId`=5773,
  `name`='Violet Hold War Banner (Horde)', `ScriptName`='';
INSERT INTO `gameobject_template` SELECT * FROM `tmp_vhr_war_banner`;
DROP TEMPORARY TABLE `tmp_vhr_war_banner`;
