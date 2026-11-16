-- Violet Hold boon expansion support.
--
-- 900119 is the short-lived creature used by Boon of the Primal Echo. Its
-- display, level, faction, health and weapon damage are replaced from the
-- summoning druid at runtime. 900118 is already the South Park scorpion and
-- is deliberately not reused. Replayable.

DELETE FROM `creature_template` WHERE `entry` = 900119;
INSERT INTO `creature_template`
  (`entry`, `modelid1`, `name`, `subname`, `minlevel`, `maxlevel`, `faction`, `npcflag`,
   `speed_walk`, `speed_run`, `scale`, `unit_class`, `unit_flags`, `unit_flags2`,
   `type`, `flags_extra`, `RegenHealth`, `ScriptName`)
VALUES
  (900119, 27486, 'Primal Echo', '', 1, 1, 35, 0,
   1, 1.14286, 1, 1, 0, 2048,
   7, 0, 1, 'npc_violet_hold_primal_echo');
