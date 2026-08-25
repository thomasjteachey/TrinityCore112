-- helper.spMakeLPlusEnchants - corrected 2026-08-21.
--
-- The procedure rebuilds dbc.spellitemenchantment_lplus from scratch:
--     DROP -> CREATE LIKE spellitemenchantment_legionnaire -> INSERT SELECT *
-- so anything hand-edited into the lplus mirror is destroyed on every run. Two
-- separate defects were found and are fixed here.
--
-- DEFECT 1 - THE PROCEDURE WAS ALREADY BROKEN.
-- The five engineer's-belt rows (3884-3888) were plain INSERTs, but those ids ALSO
-- exist in spellitemenchantment_legionnaire with byte-identical values, so the copy
-- step inserts them first and the INSERT then fails with a duplicate-key error.
-- With no handler that aborts the procedure, leaving a 2661-row table. They are now
-- REPLACE INTO, which is idempotent and still lets the procedure re-assert the values
-- if the base table ever loses them.
--
-- DEFECT 2 - LPLUS-ONLY CONTENT AND THE CLASSIC PASS WERE NOT REPRODUCED.
-- The legionnaire base is stock-flavoured, so a rebuild silently reverted 29 rows and
-- dropped 2 more entirely. That is how the 2026-07-05 classic poison pass was lost
-- (its values survive only in SpellItemEnchantment.dbc.bak-t2rework). Everything the
-- lplus mirror is supposed to carry is now re-asserted below, so the procedure is
-- idempotent against the live table: running it reproduces the current 2663 rows
-- exactly.
--
-- Values are the classic 1.12 spec - see
-- sql/custom/dbc/2026_08_21_03_dbc_classic_poison_proc_chances.sql and
-- sql/custom/dbc/2026_08_21_04_dbc_classic_rockbiter.sql for the derivations.
--
-- APPLY WITH `mysql --comments`, or the comments in this body are stripped on the way
-- in and the next person loses every explanation above.

DROP PROCEDURE IF EXISTS `helper`.`spMakeLPlusEnchants`;

DELIMITER $$

CREATE DEFINER=`brokilodeluxe`@`%` PROCEDURE `helper`.`spMakeLPlusEnchants`()
BEGIN

-- ============================================================ base rebuild
drop table if exists dbc.spellitemenchantment_lplus;
create table dbc.spellitemenchantment_lplus like dbc.spellitemenchantment_legionnaire;
insert into dbc.spellitemenchantment_lplus select * from dbc.spellitemenchantment_legionnaire;

-- ====================================================== engineer's belts
-- REPLACE, not INSERT: these ids already exist in the base table (see DEFECT 1).
REPLACE INTO `dbc`.`spellitemenchantment_lplus` (`ID`, `Charges`, `Effect_1`, `Effect_2`, `Effect_3`, `EffectPointsMin_1`, `EffectPointsMin_2`, `EffectPointsMin_3`, `EffectPointsMax_1`, `EffectPointsMax_2`, `EffectPointsMax_3`, `EffectArg_1`, `EffectArg_2`, `EffectArg_3`, `Name_Lang_enUS`, `Name_Lang_enGB`, `Name_Lang_koKR`, `Name_Lang_frFR`, `Name_Lang_deDE`, `Name_Lang_enCN`, `Name_Lang_zhCN`, `Name_Lang_enTW`, `Name_Lang_zhTW`, `Name_Lang_esES`, `Name_Lang_esMX`, `Name_Lang_ruRU`, `Name_Lang_ptPT`, `Name_Lang_ptBR`, `Name_Lang_itIT`, `Name_Lang_Unk`, `Name_Lang_Mask`, `ItemVisual`, `Flags`, `Src_ItemID`, `Condition_Id`, `RequiredSkillID`, `RequiredSkillRank`, `MinLevel`) VALUES ('3884', '5', '7', '0', '0', '0', '0', '0', '0', '0', '0', '4042', '0', '0', 'Healing Potion Belt', '', '', '', '', '', '', '', '', '', '', '', '', '', '', '', '16712190', '0', '1', '0', '0', '0', '0', '0');
REPLACE INTO `dbc`.`spellitemenchantment_lplus` (`ID`, `Charges`, `Effect_1`, `Effect_2`, `Effect_3`, `EffectPointsMin_1`, `EffectPointsMin_2`, `EffectPointsMin_3`, `EffectPointsMax_1`, `EffectPointsMax_2`, `EffectPointsMax_3`, `EffectArg_1`, `EffectArg_2`, `EffectArg_3`, `Name_Lang_enUS`, `Name_Lang_enGB`, `Name_Lang_koKR`, `Name_Lang_frFR`, `Name_Lang_deDE`, `Name_Lang_enCN`, `Name_Lang_zhCN`, `Name_Lang_enTW`, `Name_Lang_zhTW`, `Name_Lang_esES`, `Name_Lang_esMX`, `Name_Lang_ruRU`, `Name_Lang_ptPT`, `Name_Lang_ptBR`, `Name_Lang_itIT`, `Name_Lang_Unk`, `Name_Lang_Mask`, `ItemVisual`, `Flags`, `Src_ItemID`, `Condition_Id`, `RequiredSkillID`, `RequiredSkillRank`, `MinLevel`) VALUES ('3885', '5', '7', '0', '0', '0', '0', '0', '0', '0', '0', '17530', '0', '0', 'Mana Potion Belt', '', '', '', '', '', '', '', '', '', '', '', '', '', '', '', '16712190', '0', '1', '0', '0', '0', '0', '0');
REPLACE INTO `dbc`.`spellitemenchantment_lplus` (`ID`, `Charges`, `Effect_1`, `Effect_2`, `Effect_3`, `EffectPointsMin_1`, `EffectPointsMin_2`, `EffectPointsMin_3`, `EffectPointsMax_1`, `EffectPointsMax_2`, `EffectPointsMax_3`, `EffectArg_1`, `EffectArg_2`, `EffectArg_3`, `Name_Lang_enUS`, `Name_Lang_enGB`, `Name_Lang_koKR`, `Name_Lang_frFR`, `Name_Lang_deDE`, `Name_Lang_enCN`, `Name_Lang_zhCN`, `Name_Lang_enTW`, `Name_Lang_zhTW`, `Name_Lang_esES`, `Name_Lang_esMX`, `Name_Lang_ruRU`, `Name_Lang_ptPT`, `Name_Lang_ptBR`, `Name_Lang_itIT`, `Name_Lang_Unk`, `Name_Lang_Mask`, `ItemVisual`, `Flags`, `Src_ItemID`, `Condition_Id`, `RequiredSkillID`, `RequiredSkillRank`, `MinLevel`) VALUES ('3886', '8', '7', '0', '0', '0', '0', '0', '0', '0', '0', '4068', '0', '0', 'Frag Belt', '', '', '', '', '', '', '', '', '', '', '', '', '', '', '', '16712190', '0', '1', '0', '0', '0', '0', '0');
REPLACE INTO `dbc`.`spellitemenchantment_lplus` (`ID`, `Charges`, `Effect_1`, `Effect_2`, `Effect_3`, `EffectPointsMin_1`, `EffectPointsMin_2`, `EffectPointsMin_3`, `EffectPointsMax_1`, `EffectPointsMax_2`, `EffectPointsMax_3`, `EffectArg_1`, `EffectArg_2`, `EffectArg_3`, `Name_Lang_enUS`, `Name_Lang_enGB`, `Name_Lang_koKR`, `Name_Lang_frFR`, `Name_Lang_deDE`, `Name_Lang_enCN`, `Name_Lang_zhCN`, `Name_Lang_enTW`, `Name_Lang_zhTW`, `Name_Lang_esES`, `Name_Lang_esMX`, `Name_Lang_ruRU`, `Name_Lang_ptPT`, `Name_Lang_ptBR`, `Name_Lang_itIT`, `Name_Lang_Unk`, `Name_Lang_Mask`, `ItemVisual`, `Flags`, `Src_ItemID`, `Condition_Id`, `RequiredSkillID`, `RequiredSkillRank`, `MinLevel`) VALUES ('3887', '1', '7', '0', '0', '0', '0', '0', '0', '0', '0', '6615', '0', '0', 'Free Action Belt', '', '', '', '', '', '', '', '', '', '', '', '', '', '', '', '16712190', '0', '1', '0', '0', '0', '0', '0');
REPLACE INTO `dbc`.`spellitemenchantment_lplus` (`ID`, `Charges`, `Effect_1`, `Effect_2`, `Effect_3`, `EffectPointsMin_1`, `EffectPointsMin_2`, `EffectPointsMin_3`, `EffectPointsMax_1`, `EffectPointsMax_2`, `EffectPointsMax_3`, `EffectArg_1`, `EffectArg_2`, `EffectArg_3`, `Name_Lang_enUS`, `Name_Lang_enGB`, `Name_Lang_koKR`, `Name_Lang_frFR`, `Name_Lang_deDE`, `Name_Lang_enCN`, `Name_Lang_zhCN`, `Name_Lang_enTW`, `Name_Lang_zhTW`, `Name_Lang_esES`, `Name_Lang_esMX`, `Name_Lang_ruRU`, `Name_Lang_ptPT`, `Name_Lang_ptBR`, `Name_Lang_itIT`, `Name_Lang_Unk`, `Name_Lang_Mask`, `ItemVisual`, `Flags`, `Src_ItemID`, `Condition_Id`, `RequiredSkillID`, `RequiredSkillRank`, `MinLevel`) VALUES ('3888', '1', '7', '0', '0', '0', '0', '0', '0', '0', '0', '3169', '0', '0', 'Limited Invulnerability Belt', '', '', '', '', '', '', '', '', '', '', '', '', '', '', '', '16712190', '0', '1', '0', '0', '0', '0', '0');

-- ====================================================== lplus-only rows
-- Absent from the legionnaire base; a plain rebuild used to drop them outright.
REPLACE INTO `dbc`.`spellitemenchantment_lplus` VALUES (3889,0,1,0,0,30,0,0,30,0,0,90513,0,0,'Chilling Poison','','','','','','','','','','','','','','','',16712190,26,9,0,0,0,0,0);
REPLACE INTO `dbc`.`spellitemenchantment_lplus` VALUES (3890,0,3,0,0,0,0,0,0,0,0,89164,0,0,'+100% Honor Gain and no Honor Cap','','','','','','','','','','','','','','','',16712190,0,0,0,0,0,0,0);

-- ================================================ classic 1.12: Rockbiter
-- Enchant type 3 EQUIP_SPELL -> the lplus Rockbiter passives 90600-90606
-- (SPELL_AURA_MOD_ATTACK_POWER). The base table carries the stock 3.3.5 type 6 TOTEM
-- form, which is flat weapon damage and is NOT classic.
UPDATE `dbc`.`spellitemenchantment_lplus` SET `Effect_1`=3, `EffectPointsMin_1`=0, `EffectPointsMax_1`=0, `EffectArg_1`=90600, `Flags`=1 WHERE `ID`=29;
UPDATE `dbc`.`spellitemenchantment_lplus` SET `Effect_1`=3, `EffectPointsMin_1`=0, `EffectPointsMax_1`=0, `EffectArg_1`=90601, `Flags`=1 WHERE `ID`=6;
UPDATE `dbc`.`spellitemenchantment_lplus` SET `Effect_1`=3, `EffectPointsMin_1`=0, `EffectPointsMax_1`=0, `EffectArg_1`=90602, `Flags`=1 WHERE `ID`=1;
UPDATE `dbc`.`spellitemenchantment_lplus` SET `Effect_1`=3, `EffectPointsMin_1`=0, `EffectPointsMax_1`=0, `EffectArg_1`=90603, `Flags`=1 WHERE `ID`=503;
UPDATE `dbc`.`spellitemenchantment_lplus` SET `Effect_1`=3, `EffectPointsMin_1`=0, `EffectPointsMax_1`=0, `EffectArg_1`=90604, `Flags`=1 WHERE `ID`=1663;
UPDATE `dbc`.`spellitemenchantment_lplus` SET `Effect_1`=3, `EffectPointsMin_1`=0, `EffectPointsMax_1`=0, `EffectArg_1`=90605, `Flags`=1 WHERE `ID`=683;
UPDATE `dbc`.`spellitemenchantment_lplus` SET `Effect_1`=3, `EffectPointsMin_1`=0, `EffectPointsMax_1`=0, `EffectArg_1`=90606, `Flags`=1 WHERE `ID`=1664;
UPDATE `dbc`.`spellitemenchantment_lplus` SET `Name_Lang_enUS`='Rockbiter +80', `Flags`=1 WHERE `ID`=504;

-- ================================================== classic 1.12: poisons
-- Flat percentages in EffectPointsMin, weapon-speed independent. 20% for
-- Instant/Mind-numbing/Venomhide, 30% for Deadly/Crippling/Wound. The base table
-- carries the WotLK 50/100 values, which are meaningless without the ProcsPerMinute
-- rows that lplusworld.spell_enchant_proc_data no longer has.
UPDATE `dbc`.`spellitemenchantment_lplus` SET `EffectPointsMin_1`=20, `EffectPointsMax_1`=20
 WHERE `ID` IN (323, 324, 325, 623, 624, 625, 2641, 3768, 3769, 35);
UPDATE `dbc`.`spellitemenchantment_lplus` SET `EffectPointsMin_1`=30, `EffectPointsMax_1`=30
 WHERE `ID` IN (703, 704, 705, 706, 2644, 3772, 3773, 22, 3889);

-- ============================================ Steel Weapon Chain (enchant 37)
-- 43588 "Disarm Duration Reduction" (-50%), not 7219 "Immune to Disarm". The item's
-- own tooltip cites $43588s1, and the Adamantite/Titanium chains both use 43588.
UPDATE `dbc`.`spellitemenchantment_lplus` SET `EffectArg_1`=43588, `Name_Lang_enUS`='Steel Weapon Chain' WHERE `ID`=37;

-- ===================================================== corrected display names
-- Payloads are right in the base; only the green line rendered on the item was wrong.
-- 24154 Falcon's Call grants aura 85 MOD_POWER_REGEN +4 (not hit rating);
-- 24156 Presence of Sight grants aura 123 MOD_TARGET_RESISTANCE -11 = spell penetration.
UPDATE `dbc`.`spellitemenchantment_lplus` SET `Name_Lang_enUS`='Ranged Attack Power +24/Stamina +10/Mana Regen +4' WHERE `ID`=2586;
UPDATE `dbc`.`spellitemenchantment_lplus` SET `Name_Lang_enUS`='Healing and Spell Damage +18/Spell Penetration +11' WHERE `ID`=2588;

END$$

DELIMITER ;
