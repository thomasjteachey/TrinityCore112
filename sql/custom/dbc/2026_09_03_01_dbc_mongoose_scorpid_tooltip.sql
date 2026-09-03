-- Mongoose Bite's Scorpid line describes the new heal.
--
-- The tooltip said the heal was "$81291s1" - the flat base point of Scorpid
-- Healing, which the client renders as 262. The heal is now five times the
-- Strength the consumed sting is draining, and no $-token can express that, so
-- the token is replaced with words.
--
-- Only the token changes. The weapon damage, the Serpent and Viper lines and
-- the both-hands requirement are left exactly as they were.
--
-- The first statement of each pair repairs a botched earlier run: applying this
-- through a double-quoted shell argument let the shell expand "$8" as an empty
-- positional parameter, so the pattern arrived as "1291s1" and left a stray
-- "$8" behind. Run this file with `mysql < file`, never inline in a shell
-- string, and the problem cannot recur.
--
-- The binary Spell.dbc must be patched to match on each realm AND in each
-- client patch - the client draws the tooltip from its own copy. The mirrors
-- alone change nothing.
--
-- Re-runnable.

UPDATE dbc.spell_bplus
   SET Description_Lang_enUS = REPLACE(Description_Lang_enUS,
       '$85 times the Strength drained', '5 times the Strength drained')
 WHERE ID IN (81285, 81286);

UPDATE dbc.spell_bplus
   SET Description_Lang_enUS = REPLACE(Description_Lang_enUS,
       '$81291s1', '5 times the Strength drained')
 WHERE ID IN (81285, 81286);

UPDATE dbc.spell_lplus
   SET Description_Lang_enUS = REPLACE(Description_Lang_enUS,
       '$85 times the Strength drained', '5 times the Strength drained')
 WHERE ID IN (81285, 81286);

UPDATE dbc.spell_lplus
   SET Description_Lang_enUS = REPLACE(Description_Lang_enUS,
       '$81291s1', '5 times the Strength drained')
 WHERE ID IN (81285, 81286);
