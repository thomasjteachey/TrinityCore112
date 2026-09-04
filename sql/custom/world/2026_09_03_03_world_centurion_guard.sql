-- Centurion Guard 900200: what a thirty-stack bounty sends after you.
--
-- Cloned from Orgrimmar Grunt 3296 - level 55 warrior, models 4259/4260,
-- HealthModifier 2, 2000ms swing - so it hits exactly as hard as the city guard
-- it is modelled on, which is what was asked for.
--
-- THREE deliberate departures from the donor:
--
--   faction 35 -> 14
--     35 is the friendly city-guard faction. 14 is MONSTER, hostile to every
--     player of either side, which is the whole point: these are not Orgrimmar's
--     guards, they are the realm's, and they come for whoever has the price on
--     their head regardless of who that is. Faction 14 is already used this way
--     on the realm (the Crystal Cores, 900100+).
--
--   npcflag 1 -> 0
--     The donor is a gossip NPC. A bounty enforcer is not something you talk to.
--
--   flags_extra 98368 -> 64 (CREATURE_FLAG_EXTRA_NO_XP)
--     98368 carries CREATURE_FLAG_EXTRA_GUARD, which wires the donor into the
--     city guard call-for-help behaviour - wrong for something spawned in the
--     open world, and it would drag real city guards into the fight.
--     NO_XP replaces it so a bounty cannot be farmed: without it, standing at
--     thirty stacks and killing the spawns would be a renewable experience
--     fountain that pays BETTER the worse you behave. Say the word if you would
--     rather they were worth experience.
--
-- MovementType 0 and no spawn rows on purpose: these are never placed in the
-- world. They are summoned next to the target by the bounty code and despawn on
-- their own, so nothing here needs a creature table entry.
--
-- Re-runnable.

DELETE FROM `creature_template` WHERE `entry` = 900200;

INSERT INTO `creature_template`
  (`entry`, `name`, `subname`, `minlevel`, `maxlevel`, `exp`, `faction`, `npcflag`,
   `speed_walk`, `speed_run`, `scale`, `rank`, `dmgschool`, `BaseAttackTime`,
   `RangeAttackTime`, `unit_class`, `unit_flags`, `dynamicflags`, `family`, `type`,
   `type_flags`, `lootid`, `mingold`, `maxgold`, `AIName`, `MovementType`,
   `HealthModifier`, `ManaModifier`, `ArmorModifier`, `DamageModifier`,
   `ExperienceModifier`, `RacialLeader`, `RegenHealth`, `flags_extra`,
   `modelid1`, `modelid2`, `VerifiedBuild`)
SELECT
   900200, 'Centurion Guard', '', `minlevel`, `maxlevel`, `exp`, 14, 0,
   `speed_walk`, `speed_run`, `scale`, `rank`, `dmgschool`, `BaseAttackTime`,
   `RangeAttackTime`, `unit_class`, `unit_flags`, `dynamicflags`, `family`, `type`,
   `type_flags`, 0, 0, 0, '', 0,
   `HealthModifier`, `ManaModifier`, `ArmorModifier`, `DamageModifier`,
   `ExperienceModifier`, `RacialLeader`, `RegenHealth`, 64,
   `modelid1`, `modelid2`, 0
  FROM `creature_template` WHERE `entry` = 3296;

-- Verify:
-- SELECT entry, name, minlevel, maxlevel, faction, npcflag, flags_extra,
--        modelid1, HealthModifier, DamageModifier, BaseAttackTime
--   FROM creature_template WHERE entry IN (3296, 900200);
