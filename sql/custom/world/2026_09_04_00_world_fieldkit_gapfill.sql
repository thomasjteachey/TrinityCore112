-- The rungs the field kit had nothing to stand on.
--
-- 2026_09_02_01 built the kit by DUPLICATING every white and grey item the world
-- can actually produce. That is honest, and it inherits the world's own holes:
-- there is no plain white plate below level 45, no white shoulder piece below
-- 25, and no white helm of any kind below the mid thirties, because classic
-- simply does not make them. The kit dresses ten levels below its wearer, so
-- those holes land squarely on real characters - a level 42 mail wearer came
-- away in a helmet and a chest and nothing else, and a warrior or paladin who
-- had just earned plate at 40 had SIX empty slots until level 55.
--
-- Measured before writing this: 588 (slot x armour x level) combinations had no
-- kit item to offer at all. These 104 pieces close every one of them - one rung
-- per five levels per slot per armour class, wherever the existing kit has
-- nothing inside a rung's own band.
--
-- WHAT THEY ARE
--   Real items, cloned. Each row copies a plain crafted white or grey piece of
--   exactly the right slot and armour class - Light Plate, Laminated Scale,
--   Smooth Leather, Twill - so the model, material and sheath are the world's
--   own art rather than something invented. Only the numbers are ours.
--
--   Armour is read off the real world rather than picked. Within one slot and
--   armour class armour is very nearly proportional to item level, so the
--   least-squares ratio over every real classic piece of that kind gives the
--   value at any rung, including the rungs the world has no item for. That is
--   what makes plate below 45 possible to price at all, and it lands where it
--   should: a level 40 plate chest at 475 against a real 40-49 average of 472.
--
--   NO STATS. Not a simplification - of the 1003 duplicates already in the kit,
--   1001 carry none either. White field gear is armour and nothing else, and a
--   generated piece handing out Strength would be strictly better than the real
--   items standing beside it.
--
-- WHY THERE IS NO zz_fieldkit_map ROW
--   That table exists so BuildWhiteKitCacheOnce can read a duplicate's TIER off
--   the item it was copied from, since every duplicate reads artifact quality
--   and no longer says. These are not copies of some particular item's tier -
--   they are white kit by construction - and KitTierQuality falls back to the
--   item's own quality, which is 6 and therefore not POOR, putting them in the
--   white pool. That is the correct answer, and it is why the fallback exists.
--
--   It also keeps the map's source_entry PRIMARY KEY out of the way: one plain
--   crafted piece is the art donor for several rungs.
--
-- ORDER MATTERS. 2026_09_02_01 deletes the whole 92000-93999 block and rebuilds
-- it, so run this file AFTER it every time that one is run. The block below
-- 93500 is left free so it can grow without colliding.
--
-- The client needs these ids in Item.dbc as well or they render as "?" and
-- cannot be equipped - see sql/custom/dbc/2026_09_04_00_dbc_fieldkit_gapfill_item.sql.
--
-- Re-runnable.

SET SESSION sql_mode = '';

DELETE FROM item_template WHERE entry BETWEEN 93500 AND 93999;

DROP TEMPORARY TABLE IF EXISTS zz_kitfill_spec;
CREATE TEMPORARY TABLE zz_kitfill_spec (
    kit_entry INT NOT NULL PRIMARY KEY,
    donor     INT NOT NULL,
    req       INT NOT NULL,
    ilvl      INT NOT NULL,
    arm       INT NOT NULL,
    nm        VARCHAR(255) NOT NULL
);

INSERT INTO zz_kitfill_spec (kit_entry, donor, req, ilvl, arm, nm) VALUES
(93500, 8746, 1, 6, 8, 'Conscript''s Cloth Cowl'),  -- Cloth Head, art from 8746 Interlaced Cowl
(93501, 8746, 5, 10, 13, 'Recruit''s Cloth Cowl'),  -- Cloth Head, art from 8746 Interlaced Cowl
(93502, 8746, 10, 15, 19, 'Trainee''s Cloth Cowl'),  -- Cloth Head, art from 8746 Interlaced Cowl
(93503, 8746, 15, 20, 25, 'Footman''s Cloth Cowl'),  -- Cloth Head, art from 8746 Interlaced Cowl
(93504, 8746, 20, 25, 31, 'Guardsman''s Cloth Cowl'),  -- Cloth Head, art from 8746 Interlaced Cowl
(93505, 8746, 25, 30, 38, 'Sentry''s Cloth Cowl'),  -- Cloth Head, art from 8746 Interlaced Cowl
(93506, 8754, 50, 55, 69, 'Champion''s Cloth Cowl'),  -- Cloth Head, art from 8754 Twill Cover
(93507, 8747, 1, 6, 14, 'Conscript''s Leather Cap'),  -- Leather Head, art from 8747 Hardened Leather Helm
(93508, 8747, 5, 10, 24, 'Recruit''s Leather Cap'),  -- Leather Head, art from 8747 Hardened Leather Helm
(93509, 8747, 10, 15, 35, 'Trainee''s Leather Cap'),  -- Leather Head, art from 8747 Hardened Leather Helm
(93510, 8747, 15, 20, 47, 'Footman''s Leather Cap'),  -- Leather Head, art from 8747 Hardened Leather Helm
(93511, 8747, 20, 25, 59, 'Guardsman''s Leather Cap'),  -- Leather Head, art from 8747 Hardened Leather Helm
(93512, 8747, 25, 30, 71, 'Sentry''s Leather Cap'),  -- Leather Head, art from 8747 Hardened Leather Helm
(93513, 8753, 50, 55, 130, 'Champion''s Leather Cap'),  -- Leather Head, art from 8753 Smooth Leather Helmet
(93514, 8748, 1, 6, 29, 'Conscript''s Chain Coif'),  -- Mail Head, art from 8748 Double Mail Coif
(93515, 8748, 5, 10, 48, 'Recruit''s Chain Coif'),  -- Mail Head, art from 8748 Double Mail Coif
(93516, 8748, 10, 15, 72, 'Trainee''s Chain Coif'),  -- Mail Head, art from 8748 Double Mail Coif
(93517, 8748, 15, 20, 96, 'Footman''s Chain Coif'),  -- Mail Head, art from 8748 Double Mail Coif
(93518, 8748, 20, 25, 120, 'Guardsman''s Chain Coif'),  -- Mail Head, art from 8748 Double Mail Coif
(93519, 8748, 30, 35, 168, 'Trooper''s Chain Coif'),  -- Mail Head, art from 8748 Double Mail Coif
(93520, 8752, 55, 60, 288, 'Praetorian''s Chain Coif'),  -- Mail Head, art from 8752 Laminated Scale Circlet
(93521, 8092, 30, 35, 300, 'Trooper''s Plate Helm'),  -- Plate Head, art from 8092 Platemail Helm
(93522, 8092, 35, 40, 342, 'Sergeant''s Plate Helm'),  -- Plate Head, art from 8092 Platemail Helm
(93523, 8092, 40, 45, 385, 'Vanguard''s Plate Helm'),  -- Plate Head, art from 8092 Platemail Helm
(93524, 8092, 55, 60, 513, 'Praetorian''s Plate Helm'),  -- Plate Head, art from 8092 Platemail Helm
(93525, 1769, 1, 6, 7, 'Conscript''s Cloth Mantle'),  -- Cloth Shoulder, art from 1769 Canvas Shoulderpads
(93526, 1769, 5, 10, 12, 'Recruit''s Cloth Mantle'),  -- Cloth Shoulder, art from 1769 Canvas Shoulderpads
(93527, 1769, 10, 15, 18, 'Trainee''s Cloth Mantle'),  -- Cloth Shoulder, art from 1769 Canvas Shoulderpads
(93528, 3798, 35, 40, 47, 'Sergeant''s Cloth Mantle'),  -- Cloth Shoulder, art from 3798 Interlaced Shoulderpads
(93529, 3942, 40, 45, 53, 'Vanguard''s Cloth Mantle'),  -- Cloth Shoulder, art from 3942 Crochet Shoulderpads
(93530, 3950, 50, 55, 65, 'Champion''s Cloth Mantle'),  -- Cloth Shoulder, art from 3950 Twill Shoulderpads
(93531, 1793, 1, 6, 14, 'Conscript''s Leather Shoulderpads'),  -- Leather Shoulder, art from 1793 Patched Leather Shoulderpads
(93532, 1793, 5, 10, 23, 'Recruit''s Leather Shoulderpads'),  -- Leather Shoulder, art from 1793 Patched Leather Shoulderpads
(93533, 1793, 10, 15, 34, 'Trainee''s Leather Shoulderpads'),  -- Leather Shoulder, art from 1793 Patched Leather Shoulderpads
(93534, 3806, 30, 35, 80, 'Trooper''s Leather Shoulderpads'),  -- Leather Shoulder, art from 3806 Hardened Leather Shoulderpads
(93535, 3967, 40, 45, 102, 'Vanguard''s Leather Shoulderpads'),  -- Leather Shoulder, art from 3967 Thick Leather Shoulderpads
(93536, 3975, 55, 60, 136, 'Praetorian''s Leather Shoulderpads'),  -- Leather Shoulder, art from 3975 Smooth Leather Shoulderpads
(93537, 1744, 1, 6, 27, 'Conscript''s Chain Spaulders'),  -- Mail Shoulder, art from 1744 Laced Mail Shoulderpads
(93538, 1744, 5, 10, 45, 'Recruit''s Chain Spaulders'),  -- Mail Shoulder, art from 1744 Laced Mail Shoulderpads
(93539, 1744, 10, 15, 68, 'Trainee''s Chain Spaulders'),  -- Mail Shoulder, art from 1744 Laced Mail Shoulderpads
(93540, 3814, 30, 35, 158, 'Trooper''s Chain Spaulders'),  -- Mail Shoulder, art from 3814 Double Mail Shoulderpads
(93541, 3998, 45, 50, 226, 'Veteran''s Chain Spaulders'),  -- Mail Shoulder, art from 3998 Laminated Scale Shoulderpads
(93542, 3998, 55, 60, 271, 'Praetorian''s Chain Spaulders'),  -- Mail Shoulder, art from 3998 Laminated Scale Shoulderpads
(93543, 8086, 30, 35, 277, 'Trooper''s Plate Pauldrons'),  -- Plate Shoulder, art from 8086 Light Plate Shoulderpads
(93544, 8086, 35, 40, 317, 'Sergeant''s Plate Pauldrons'),  -- Plate Shoulder, art from 8086 Light Plate Shoulderpads
(93545, 8086, 40, 45, 356, 'Vanguard''s Plate Pauldrons'),  -- Plate Shoulder, art from 8086 Light Plate Shoulderpads
(93546, 8086, 45, 50, 396, 'Veteran''s Plate Pauldrons'),  -- Plate Shoulder, art from 8086 Light Plate Shoulderpads
(93547, 8086, 55, 60, 475, 'Praetorian''s Plate Pauldrons'),  -- Plate Shoulder, art from 8086 Light Plate Shoulderpads
(93548, 3799, 30, 35, 47, 'Trooper''s Cloth Tunic'),  -- Cloth Chest, art from 3799 Interlaced Vest
(93549, 3951, 50, 55, 74, 'Champion''s Cloth Tunic'),  -- Cloth Chest, art from 3951 Twill Vest
(93550, 3976, 50, 55, 165, 'Champion''s Leather Vest'),  -- Leather Chest, art from 3976 Smooth Leather Armor
(93551, 3999, 55, 60, 367, 'Praetorian''s Chain Hauberk'),  -- Mail Chest, art from 3999 Laminated Scale Armor
(93552, 8080, 30, 35, 369, 'Trooper''s Plate Breastplate'),  -- Plate Chest, art from 8080 Light Plate Chestpiece
(93553, 8080, 35, 40, 422, 'Sergeant''s Plate Breastplate'),  -- Plate Chest, art from 8080 Light Plate Chestpiece
(93554, 8080, 40, 45, 475, 'Vanguard''s Plate Breastplate'),  -- Plate Chest, art from 8080 Light Plate Chestpiece
(93555, 8080, 50, 55, 580, 'Champion''s Plate Breastplate'),  -- Plate Chest, art from 8080 Light Plate Chestpiece
(93556, 3944, 55, 60, 53, 'Praetorian''s Cloth Sash'),  -- Cloth Waist, art from 3944 Twill Belt
(93557, 3800, 30, 35, 61, 'Trooper''s Leather Belt'),  -- Leather Waist, art from 3800 Hardened Leather Belt
(93558, 3969, 55, 60, 105, 'Praetorian''s Leather Belt'),  -- Leather Waist, art from 3969 Smooth Leather Belt
(93559, 3992, 55, 60, 202, 'Praetorian''s Chain Girdle'),  -- Mail Waist, art from 3992 Laminated Scale Belt
(93560, 8081, 30, 35, 205, 'Trooper''s Plate Waistguard'),  -- Plate Waist, art from 8081 Light Plate Belt
(93561, 8081, 35, 40, 235, 'Sergeant''s Plate Waistguard'),  -- Plate Waist, art from 8081 Light Plate Belt
(93562, 8081, 40, 45, 264, 'Vanguard''s Plate Waistguard'),  -- Plate Waist, art from 8081 Light Plate Belt
(93563, 8081, 50, 55, 323, 'Champion''s Plate Waistguard'),  -- Plate Waist, art from 8081 Light Plate Belt
(93564, 3797, 30, 35, 48, 'Trooper''s Cloth Leggings'),  -- Cloth Legs, art from 3797 Interlaced Pants
(93565, 3949, 50, 55, 75, 'Champion''s Cloth Leggings'),  -- Cloth Legs, art from 3949 Twill Pants
(93566, 3974, 55, 60, 158, 'Praetorian''s Leather Pants'),  -- Leather Legs, art from 3974 Smooth Leather Pants
(93567, 3997, 50, 55, 292, 'Champion''s Chain Legguards'),  -- Mail Legs, art from 3997 Laminated Scale Pants
(93568, 8085, 30, 35, 325, 'Trooper''s Plate Greaves'),  -- Plate Legs, art from 8085 Light Plate Pants
(93569, 8085, 35, 40, 371, 'Sergeant''s Plate Greaves'),  -- Plate Legs, art from 8085 Light Plate Pants
(93570, 8085, 40, 45, 418, 'Vanguard''s Plate Greaves'),  -- Plate Legs, art from 8085 Light Plate Pants
(93571, 8085, 55, 60, 557, 'Praetorian''s Plate Greaves'),  -- Plate Legs, art from 8085 Light Plate Pants
(93572, 3937, 40, 45, 50, 'Vanguard''s Cloth Slippers'),  -- Cloth Feet, art from 3937 Crochet Boots
(93573, 3945, 55, 60, 66, 'Praetorian''s Cloth Slippers'),  -- Cloth Feet, art from 3945 Twill Boots
(93574, 3962, 40, 45, 95, 'Vanguard''s Leather Boots'),  -- Leather Feet, art from 3962 Thick Leather Boots
(93575, 3970, 55, 60, 127, 'Praetorian''s Leather Boots'),  -- Leather Feet, art from 3970 Smooth Leather Boots
(93576, 4001, 40, 45, 187, 'Vanguard''s Chain Warboots'),  -- Mail Feet, art from 4001 Overlinked Chain Boots
(93577, 3993, 55, 60, 249, 'Praetorian''s Chain Warboots'),  -- Mail Feet, art from 3993 Laminated Scale Boots
(93578, 8082, 30, 35, 252, 'Trooper''s Plate Sabatons'),  -- Plate Feet, art from 8082 Light Plate Boots
(93579, 8082, 35, 40, 288, 'Sergeant''s Plate Sabatons'),  -- Plate Feet, art from 8082 Light Plate Boots
(93580, 8082, 40, 45, 324, 'Vanguard''s Plate Sabatons'),  -- Plate Feet, art from 8082 Light Plate Boots
(93581, 8082, 55, 60, 432, 'Praetorian''s Plate Sabatons'),  -- Plate Feet, art from 8082 Light Plate Boots
(93582, 3794, 30, 35, 22, 'Trooper''s Cloth Cuffs'),  -- Cloth Wrist, art from 3794 Interlaced Bracers
(93583, 3946, 55, 60, 38, 'Praetorian''s Cloth Cuffs'),  -- Cloth Wrist, art from 3946 Twill Bracers
(93584, 3971, 50, 55, 70, 'Champion''s Leather Bracers'),  -- Leather Wrist, art from 3971 Smooth Leather Bracers
(93585, 3810, 30, 35, 93, 'Trooper''s Chain Wristguards'),  -- Mail Wrist, art from 3810 Double Mail Bracers
(93586, 4002, 40, 45, 119, 'Vanguard''s Chain Wristguards'),  -- Mail Wrist, art from 4002 Overlinked Chain Bracers
(93587, 3994, 50, 55, 146, 'Champion''s Chain Wristguards'),  -- Mail Wrist, art from 3994 Laminated Scale Bracers
(93588, 8083, 30, 35, 160, 'Trooper''s Plate Vambraces'),  -- Plate Wrist, art from 8083 Light Plate Bracers
(93589, 8083, 35, 40, 183, 'Sergeant''s Plate Vambraces'),  -- Plate Wrist, art from 8083 Light Plate Bracers
(93590, 8083, 40, 45, 206, 'Vanguard''s Plate Vambraces'),  -- Plate Wrist, art from 8083 Light Plate Bracers
(93591, 8083, 55, 60, 275, 'Praetorian''s Plate Vambraces'),  -- Plate Wrist, art from 8083 Light Plate Bracers
(93592, 3940, 40, 45, 43, 'Vanguard''s Cloth Gloves'),  -- Cloth Hands, art from 3940 Crochet Gloves
(93593, 3948, 50, 55, 53, 'Champion''s Cloth Gloves'),  -- Cloth Hands, art from 3948 Twill Gloves
(93594, 3804, 30, 35, 66, 'Trooper''s Leather Handwraps'),  -- Leather Hands, art from 3804 Hardened Leather Gloves
(93595, 3965, 40, 45, 85, 'Vanguard''s Leather Handwraps'),  -- Leather Hands, art from 3965 Thick Leather Gloves
(93596, 3973, 50, 55, 104, 'Champion''s Leather Handwraps'),  -- Leather Hands, art from 3973 Smooth Leather Gloves
(93597, 3812, 30, 35, 133, 'Trooper''s Chain Gauntlets'),  -- Mail Hands, art from 3812 Double Mail Gloves
(93598, 4004, 40, 45, 171, 'Vanguard''s Chain Gauntlets'),  -- Mail Hands, art from 4004 Overlinked Chain Gloves
(93599, 3996, 50, 55, 209, 'Champion''s Chain Gauntlets'),  -- Mail Hands, art from 3996 Laminated Scale Gloves
(93600, 8084, 30, 35, 229, 'Trooper''s Plate Handguards'),  -- Plate Hands, art from 8084 Light Plate Gloves
(93601, 8084, 35, 40, 262, 'Sergeant''s Plate Handguards'),  -- Plate Hands, art from 8084 Light Plate Gloves
(93602, 8084, 40, 45, 295, 'Vanguard''s Plate Handguards'),  -- Plate Hands, art from 8084 Light Plate Gloves
(93603, 8084, 50, 55, 360, 'Champion''s Plate Handguards');  -- Plate Hands, art from 8084 Light Plate Gloves

-- Copy the donors through a scratch table so all hundred-odd columns come along
-- without naming them. The primary key goes first because one donor supplies the
-- art for several rungs, and zz_kit carries the new id until the row is
-- renumbered onto it.
DROP TEMPORARY TABLE IF EXISTS zz_kitfill;
CREATE TEMPORARY TABLE zz_kitfill LIKE item_template;
ALTER TABLE zz_kitfill DROP PRIMARY KEY, ADD COLUMN zz_kit INT NOT NULL;

INSERT INTO zz_kitfill
    SELECT t.*, s.kit_entry
      FROM item_template t
      JOIN zz_kitfill_spec s ON s.donor = t.entry;

UPDATE zz_kitfill f
  JOIN zz_kitfill_spec s ON s.kit_entry = f.zz_kit
   SET f.entry = f.zz_kit,
       f.name = s.nm,
       f.RequiredLevel = s.req,
       f.ItemLevel = s.ilvl,
       f.armor = s.arm,
       -- Artifact quality and the loaner text, matching 2026_09_02_02.
       f.Quality = 6,
       f.description = 'Temporal issue. Awarded automatically, and destroyed when you replace it.',
       -- Free gear must never be a gold faucet.
       f.BuyPrice = 0, f.SellPrice = 0,
       -- Armour only. Whatever the donor happened to carry is cleared, so a rung
       -- can never out-stat the earned gear it is standing in for.
       f.StatsCount = 0,
       f.stat_type1 = 0, f.stat_value1 = 0, f.stat_type2 = 0, f.stat_value2 = 0,
       f.stat_type3 = 0, f.stat_value3 = 0, f.stat_type4 = 0, f.stat_value4 = 0,
       f.stat_type5 = 0, f.stat_value5 = 0, f.stat_type6 = 0, f.stat_value6 = 0,
       f.stat_type7 = 0, f.stat_value7 = 0, f.stat_type8 = 0, f.stat_value8 = 0,
       f.stat_type9 = 0, f.stat_value9 = 0, f.stat_type10 = 0, f.stat_value10 = 0,
       f.holy_res = 0, f.fire_res = 0, f.nature_res = 0, f.frost_res = 0,
       f.shadow_res = 0, f.arcane_res = 0,
       f.spellid_1 = 0, f.spelltrigger_1 = 0, f.spellcharges_1 = 0,
       f.spellppmRate_1 = 0, f.spellcooldown_1 = -1, f.spellcategory_1 = 0,
       f.spellcategorycooldown_1 = -1,
       f.spellid_2 = 0, f.spelltrigger_2 = 0, f.spellcharges_2 = 0,
       f.spellppmRate_2 = 0, f.spellcooldown_2 = -1, f.spellcategory_2 = 0,
       f.spellcategorycooldown_2 = -1,
       f.spellid_3 = 0, f.spelltrigger_3 = 0, f.spellcharges_3 = 0,
       f.spellppmRate_3 = 0, f.spellcooldown_3 = -1, f.spellcategory_3 = 0,
       f.spellcategorycooldown_3 = -1,
       f.spellid_4 = 0, f.spelltrigger_4 = 0, f.spellcharges_4 = 0,
       f.spellppmRate_4 = 0, f.spellcooldown_4 = -1, f.spellcategory_4 = 0,
       f.spellcategorycooldown_4 = -1,
       f.spellid_5 = 0, f.spelltrigger_5 = 0, f.spellcharges_5 = 0,
       f.spellppmRate_5 = 0, f.spellcooldown_5 = -1, f.spellcategory_5 = 0,
       f.spellcategorycooldown_5 = -1,
       f.ScalingStatDistribution = 0, f.ScalingStatValue = 0,
       f.ArmorDamageModifier = 0,
       f.RandomProperty = 0, f.RandomSuffix = 0, f.itemset = 0,
       f.socketColor_1 = 0, f.socketContent_1 = 0,
       f.socketColor_2 = 0, f.socketContent_2 = 0,
       f.socketColor_3 = 0, f.socketContent_3 = 0,
       f.socketBonus = 0, f.GemProperties = 0,
       -- Everything the kit's own filter refuses to hand out.
       f.RequiredSkill = 0, f.RequiredSkillRank = 0, f.requiredspell = 0,
       f.requiredhonorrank = 0, f.RequiredCityRank = 0,
       f.RequiredReputationFaction = 0, f.RequiredReputationRank = 0,
       f.area = 0, f.Map = 0, f.duration = 0, f.startquest = 0, f.lockid = 0,
       f.PageText = 0, f.BagFamily = 0, f.TotemCategory = 0,
       f.ItemLimitCategory = 0, f.HolidayId = 0,
       f.AllowableClass = -1, f.AllowableRace = -1,
       f.Flags = 0, f.FlagsExtra = 0, f.flagsCustom = 0,
       f.maxcount = 0, f.stackable = 1, f.ContainerSlots = 0,
       -- Bound on pickup: a loaner is not something to hand around.
       f.bonding = 1,
       f.DisenchantID = 0, f.RequiredDisenchantSkill = -1,
       f.scriptName = '', f.VerifiedBuild = 0;

ALTER TABLE zz_kitfill DROP COLUMN zz_kit;
INSERT INTO item_template SELECT * FROM zz_kitfill;

DROP TEMPORARY TABLE zz_kitfill;
DROP TEMPORARY TABLE zz_kitfill_spec;

-- Sanity.
SELECT CONCAT('gap-fill pieces created: ', COUNT(*)) AS result
  FROM item_template WHERE entry BETWEEN 93500 AND 93999;

SELECT CONCAT('any carrying stats (must be 0): ', COUNT(*)) AS result
  FROM item_template WHERE entry BETWEEN 93500 AND 93999 AND StatsCount <> 0;

SELECT CONCAT('any that would sell (must be 0): ', COUNT(*)) AS result
  FROM item_template WHERE entry BETWEEN 93500 AND 93999 AND SellPrice <> 0;

SELECT CONCAT('any with no model (must be 0): ', COUNT(*)) AS result
  FROM item_template WHERE entry BETWEEN 93500 AND 93999 AND displayid = 0;

-- The thing that was actually broken: a fresh level 40 warrior in plate.
SELECT CONCAT('plate slots dressable at kit level 30 (was 0, want 8): ',
              COUNT(DISTINCT InventoryType)) AS result
  FROM item_template
 WHERE entry BETWEEN 92000 AND 93999 AND class = 4 AND subclass = 4
   AND RequiredLevel <= 30;

-- To undo:
--   DELETE FROM item_template WHERE entry BETWEEN 93500 AND 93999;
-- (and the matching rows from dbc.item_lplus, then rebuild Item.dbc)
