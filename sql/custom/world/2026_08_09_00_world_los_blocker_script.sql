-- Bind the LoS-blocker aura (90214) to its script.
--
-- Without this row the spell is an inert dummy aura: the script is what
-- registers the wearer with LosBlocker, which the spell LoS checks consult.
--
-- Replayable.

DELETE FROM `spell_script_names` WHERE `spell_id` = 90214;
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
  (90214, 'spell_los_blocker');
