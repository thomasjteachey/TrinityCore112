-- Bounty 90701 belongs in the DEBUFF frame.
--
-- A bare DUMMY aura with no negative effect values is judged POSITIVE by
-- SpellMgr's own heuristics, which puts a price on your head in the buff bar
-- next to your food buff. 4096 is SPELL_ATTR0_CU_NEGATIVE_EFF0 and settles it.
--
-- It also matters mechanically, not only cosmetically: Unit::RemoveArenaAuras
-- keeps an aura that is negative AND death-persistent, and the client only
-- offers to cancel a POSITIVE aura - so the same flag that puts it in the right
-- frame is part of what stops a bounty being clicked away or queued away.
--
-- spell_custom_attr is read once at startup. This needs a worldserver RESTART,
-- not a .reload.
--
-- Re-runnable. Run against BOTH bplusworld and lplusworld.

DELETE FROM `spell_custom_attr` WHERE `entry` = 90701;
INSERT INTO `spell_custom_attr` (`entry`, `attributes`) VALUES
(90701, 4096);
