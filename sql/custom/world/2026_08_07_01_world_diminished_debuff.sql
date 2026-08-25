-- Diminished: bind the scripts that make the debuff track its stack count.
--
-- spell_gen_diminished applies, stack-syncs and removes the hidden model-scale
-- companion 90202, holding it at HALF the parent's stack count. That is its
-- only job - it no longer computes any effect amounts, because the engine
-- already multiplies base points by the stack count and doing it in script too
-- squared the values.
--
-- 90202 needs no script of its own: its per-stack -1 times its own stack count
-- is the whole behaviour. The old spell_gen_diminished_healing is gone.
--
-- Without this binding the companion is never applied, so the debuff still
-- works but nothing shrinks.
--
-- An earlier draft linked the companion through spell_linked_spell; that is
-- gone, because the link applies and removes an aura but has no way to keep its
-- stack count in step with the parent's. The script does both.

DELETE FROM `spell_linked_spell` WHERE `spell_trigger` IN (90201, 90203, 90205);

DELETE FROM `spell_script_names` WHERE `spell_id` IN (90201, 90202);
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
  (90201, 'spell_gen_diminished');
