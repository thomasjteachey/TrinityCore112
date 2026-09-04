-- Give the Centurion Guard an AI that knows who it was sent after.
--
-- The guard carries faction 14 - "Monster", hostile to everything alive - which
-- is what lets it attack a player at all. The side effect is that the stock
-- creature AI aggroes ANYONE who walks into range: a guard summoned onto a
-- bounty in Westfall would break off and chase a level 20 who had never killed
-- anybody. That is precisely what a person turns War Mode off to avoid.
--
-- npc_centurion_guard (src/server/scripts/Custom/custom_bounty.cpp) narrows it
-- to the summoner, whoever is hitting the guard, and anyone who armed War Mode
-- or earned a bounty of their own. It changes nothing about the fight it was
-- summoned for.
--
-- No creature is respawned by this: the guards are TempSummons with a lifetime
-- of a couple of minutes, so the whole population turns over on its own. A
-- worldserver restart is still needed for the script itself.
--
-- Re-runnable.

UPDATE creature_template
   SET ScriptName = 'npc_centurion_guard'
 WHERE entry = 900200;

SELECT CONCAT('Centurion Guard script: ', IFNULL(NULLIF(ScriptName, ''), '(none)')) AS result
  FROM creature_template WHERE entry = 900200;

-- To undo:
--   UPDATE creature_template SET ScriptName = '' WHERE entry = 900200;
