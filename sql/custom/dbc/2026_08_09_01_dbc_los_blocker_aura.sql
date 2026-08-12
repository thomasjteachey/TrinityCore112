-- Obstructing Presence (90214): the wearer eclipses hostile line of sight.
--
-- While it is up, enemies cannot cast THROUGH the wearer at something behind
-- them. Spells aimed at the wearer itself still land, and allies are unaffected.
--
-- This is the server-side sibling of the collision auras (90210-90213). Those
-- can live entirely in the client because movement is client-authoritative;
-- line of sight is decided by the SERVER, so this one is enforced in the core
-- (src/server/game/Entities/Object/LosBlocker.{h,cpp}, consulted from the LoS
-- checks in Spell::CheckCast) and needs a worldserver build to take effect.
--
-- Unlike 90210-90213 this aura is NOT inert: it is bound to the script
-- spell_los_blocker via spell_script_names, which registers and unregisters the
-- wearer. Without that binding the spell does nothing at all.
--
-- Same donor and same caveats as the collision auras: 23451 Speed, Attributes 0,
-- SpellVisualID zeroed (the donor's 6922 is Speed's pickup effect and a
-- byte-clone inherits it), DurationIndex 21 = infinite.
--
-- Replayable: deletes its own id before inserting.

DELETE FROM `spell_lplus` WHERE `ID` = 90214;

DROP TEMPORARY TABLE IF EXISTS `tmp_losblock`;
CREATE TEMPORARY TABLE `tmp_losblock` AS
SELECT * FROM `spell_lplus` WHERE `ID` = 23451;

UPDATE `tmp_losblock` SET
  `ID` = 90214,
  `Name_Lang_enUS` = 'Obstructing Presence',
  `Description_Lang_enUS` = '',
  `AuraDescription_Lang_enUS` = 'Enemies cannot cast through you. Spells aimed at you are unaffected.',
  `EffectAura_1` = 4,          -- SPELL_AURA_DUMMY; the script does the work
  `EffectBasePoints_1` = 0,
  `DurationIndex` = 21,        -- infinite
  `SpellVisualID_1` = 0,
  `SpellVisualID_2` = 0,
  `AuraInterruptFlags` = 0,      -- donor's 0x20000 would drop it on mounting
  `AttributesExC` = 1048576,     -- SPELL_ATTR3_DEATH_PERSISTENT
  `Attributes` = 2147483648,     -- SPELL_ATTR0_CANT_CANCEL: player cannot right-click it off
  `AttributesEx` = 33554432,     -- SPELL_ATTR1_DONT_DISPLAY_IN_AURA_BAR: no buff icon.
                                 -- Never use SPELL_ATTR0_PASSIVE for this: passive
                                 -- auras fail Aura::CanBeSentToClient() and are not
                                 -- sent to the client at all.
  `SpellIconID` = 2135;        -- Spell_Holy_ArdentDefender

INSERT INTO `spell_lplus` SELECT * FROM `tmp_losblock`;
DROP TEMPORARY TABLE `tmp_losblock`;
