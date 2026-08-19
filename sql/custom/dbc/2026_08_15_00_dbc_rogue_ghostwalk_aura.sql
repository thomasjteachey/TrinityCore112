-- Ghostwalk (90218): rogue collision-bypass talent aura.
--
-- A rogue carrying this aura passes through ALL unit collision, in both
-- directions, and it takes precedence over every collision rule. All of the
-- behaviour lives in the client tweak DLL (clientedits\src\playercollide.cpp),
-- which lists 90218 in its ExcludeAuras set: a unit with an excluded aura is
-- vetoed in BlocksMe before any positive collision rule runs. So this aura is
-- completely INERT server-side (SPELL_AURA_DUMMY, no script) - exactly like the
-- collision auras 90210-90213.
--
-- INVISIBLE BUT CLIENT-VISIBLE. The DLL gates collision by reading this id off
-- the client's own aura table, so the aura MUST reach the client. That rules out
-- SPELL_ATTR0_PASSIVE (passive auras fail Aura::CanBeSentToClient() and are never
-- sent - the DLL would go blind). Instead it is hidden with BOTH DONT_DISPLAY
-- bits (AttributesEx = 0x12000000 = 301989888): still sent, still in the aura
-- table the DLL walks, just no buff icon. This matches 90210-90214 and the
-- confirmed-hidden Boon Broker markers (90248/90257). "Passive and invisible" is
-- achieved this way, NOT with SPELL_ATTR0_PASSIVE.
--
-- HOW A ROGUE RECEIVES IT is deliberately out of scope of this file - wire it to
-- your talent/perk system (learn -> apply 90218), or a login PlayerScript that
-- gives it to rogues who have the talent. The DLL only cares that the rogue has
-- the aura; anything that applies it works.
--
-- Same donor and byte-clone caveats as 90210-90213: 23451 Speed, Attributes 0,
-- SpellVisualID zeroed (donor's 6922 is Speed's pickup effect), AuraInterruptFlags
-- cleared (donor's 0x20000 drops the aura on mounting), DurationIndex 21 =
-- infinite. Icon is irrelevant (the aura never renders) but must exist in
-- dbc.spellicon_lplus; reuses a verified id.
--
-- After applying: regenerate the binary Spell.dbc (tools\recolor\itemforge\
-- spell_dbc.py --verify then --out), deploy to ALL FOUR Spell.dbc homes, repack
-- the client patch. Replayable: deletes its own id before inserting.

DELETE FROM `spell_lplus` WHERE `ID` = 90218;

DROP TEMPORARY TABLE IF EXISTS `tmp_ghostwalk`;
CREATE TEMPORARY TABLE `tmp_ghostwalk` AS
SELECT * FROM `spell_lplus` WHERE `ID` = 23451;

UPDATE `tmp_ghostwalk` SET
  `ID` = 90218,
  `Name_Lang_enUS` = 'Ghostwalk',
  `Description_Lang_enUS` = '',
  `AuraDescription_Lang_enUS` = 'You slip through other units, unhindered by collision.',
  `EffectAura_1` = 4,            -- SPELL_AURA_DUMMY; the client DLL does the work
  `EffectBasePoints_1` = 0,
  `DurationIndex` = 21,          -- infinite
  `SpellVisualID_1` = 0,
  `SpellVisualID_2` = 0,
  `AuraInterruptFlags` = 0,      -- donor carried 0x20000 AURA_INTERRUPT_FLAG_MOUNT
  `AttributesExC` = 1048576,     -- SPELL_ATTR3_DEATH_PERSISTENT
  `Attributes` = 2147483648,     -- SPELL_ATTR0_CANT_CANCEL
  `AttributesEx` = 301989888,    -- 0x12000000: BOTH DONT_DISPLAY bits, no buff icon
  `SpellIconID` = 2135;          -- irrelevant (never rendered); verified to exist

INSERT INTO `spell_lplus` SELECT * FROM `tmp_ghostwalk`;
DROP TEMPORARY TABLE `tmp_ghostwalk`;
