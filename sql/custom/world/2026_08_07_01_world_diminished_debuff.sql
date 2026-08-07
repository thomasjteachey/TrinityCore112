-- Diminished: bind the scripts that make the debuff track its stack count.
--
-- spell_gen_diminished recalculates all three of 90201's effect amounts to
-- -stackAmount, and applies / stack-syncs / removes the hidden healing helper
-- 90202. spell_gen_diminished_healing does the same recalculation for 90202's
-- single effect.
--
-- Without these bindings the stored per-stack value of -1 applies flat, so the
-- debuff is a 1% nuisance at any stack count rather than a handicap.
--
-- An earlier draft linked the helper through spell_linked_spell; that is gone,
-- because the link applies and removes an aura but has no way to keep its stack
-- count in step with the parent's. The script does both.

DELETE FROM `spell_linked_spell` WHERE `spell_trigger` IN (90201, 90203, 90205);

DELETE FROM `spell_script_names` WHERE `spell_id` IN (90201, 90202);
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
  (90201, 'spell_gen_diminished'),
  (90202, 'spell_gen_diminished_healing');
