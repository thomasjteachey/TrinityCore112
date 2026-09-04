-- Flametongue Weapon does no fire damage on Barracks Plus.
--
-- The imbue itself is fine and byte-identical to Legionnaire Plus. Enchants
-- 3/4/5/523/1665/1666/2634 are type 3 (EQUIP_SPELL) pointing at the seven
-- "Flametongue Weapon (Passive)" spells 10400/15567/15568/15569/16311/16312/
-- 16313, each of which is Effect 6 / aura 4 (SPELL_AURA_DUMMY) with ProcTypeMask
-- 20 and ProcChance 100. All fourteen rows match L+ exactly.
--
-- What is missing is the thing that ANSWERS the proc. A dummy aura does nothing
-- by itself: spell_sha_flametongue_weapon (src/server/scripts/Spells/spell_shaman.cpp
-- ~638) is an AuraScript whose OnEffectProc(EFFECT_0, SPELL_AURA_DUMMY) is what
-- works out the weapon speed, scales the damage and casts Flametongue Attack.
-- With no spell_script_names row the script is never attached, so the aura procs
-- on every swing and lands on nobody.
--
-- This is the ONLY row that differs between the two realms' script tables -
-- 2855 rows on B+ against 2856 on L+, and this is the one. It was lost rather
-- than removed for a reason.
--
-- The negative id is TrinityCore's "whole rank chain" form, so one row covers
-- all seven ranks, exactly as on L+.
--
-- Needs a worldserver restart: script names are bound when scripts load and
-- there is no reload command for this table.
--
-- Re-runnable.

DELETE FROM spell_script_names
 WHERE spell_id = -10400 AND ScriptName = 'spell_sha_flametongue_weapon';

INSERT INTO spell_script_names (spell_id, ScriptName)
VALUES (-10400, 'spell_sha_flametongue_weapon');

SELECT CONCAT('flametongue script rows: ', COUNT(*)) AS result
  FROM spell_script_names WHERE ScriptName = 'spell_sha_flametongue_weapon';

-- To undo:
--   DELETE FROM spell_script_names WHERE spell_id = -10400
--     AND ScriptName = 'spell_sha_flametongue_weapon';
