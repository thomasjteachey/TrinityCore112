-- Allow depleted marks of honor to stack up to 200, matching restored marks
UPDATE `item_template` SET `stackable` = 200 WHERE `entry` BETWEEN 20559 AND 20575;

-- Retire the voucher-based exchange quest and its supporting data
DELETE FROM `creature_queststarter` WHERE `quest` = 90050;
DELETE FROM `creature_questender` WHERE `quest` = 90050;
DELETE FROM `creature` WHERE `id` = 91000 OR `guid` = 910000;
DELETE FROM `creature_template` WHERE `entry` = 91000;
DELETE FROM `quest_offer_reward` WHERE `ID` = 90050;
DELETE FROM `quest_request_items` WHERE `ID` = 90050;
DELETE FROM `quest_details` WHERE `ID` = 90050;
DELETE FROM `quest_template_addon` WHERE `ID` = 90050;
DELETE FROM `quest_template` WHERE `ID` = 90050;
DELETE FROM `item_template` WHERE `entry` = 91001;

-- Teach depleted marks to cast a spell that mirrors the enchanting reagents
SET @DEPLETED_MARK_SPELL := 91050;
-- The matching Spell.dbc row should be cloned from spell 13361 with the new tooltip text.

UPDATE `item_template`
SET `ScriptName` = '',
    `spellid_1` = @DEPLETED_MARK_SPELL,
    `spelltrigger_1` = 0,
    `spellcharges_1` = 0,
    `spellcooldown_1` = 0,
    `spellcategorycooldown_1` = 0,
    `description` = 'A depleted medal that hums with unstable energy.\n\nUse: Turn three ineligible depleted marks into one suited to your class.'
WHERE `entry` BETWEEN 20559 AND 20575;

DELETE FROM `spell_script_names` WHERE `spell_id` = @DEPLETED_MARK_SPELL;
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
(@DEPLETED_MARK_SPELL, 'spell_depleted_mark_converter');
