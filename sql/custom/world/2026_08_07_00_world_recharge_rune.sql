-- Recharge rune: the pickup gameobject, and the script binding for its buff.
--
-- Cloned field-for-field from 179871 Speed Buff, which is the shape every BG
-- powerup uses: GAMEOBJECT_TYPE_TRAP (6), so the client fires it at whoever
-- walks into it without any interaction. For that type the data columns are
--   Data0 lockId, Data1 level, Data2 radius, Data3 spellId,
--   Data4 charges, Data5 cooldown (seconds), Data6 autoClose
-- Radius 0 means "use the trap default", matching the other three runes, and
-- the 3s cooldown is theirs as well - it stops one player triggering it twice
-- while standing on top of it.
--
-- displayId 5932 is World\Generic\PVP\Runes\PVP_Rune_Invis.mdx: the fourth PVP
-- rune Blizzard shipped and never used. It already has a GameObjectDisplayInfo
-- row in the live patch-enUS-8, so no DBC work is needed for the art. Note it
-- is the only one of the four NOT replaced by an HD model in patch-U, so it
-- looks plainer beside Speed/Restoration/Berserking until it gets the same
-- treatment.
--
-- Entry 300500 is above the current MAX(entry) of 300401.
--
-- Spawning is handled separately - this only defines the object.

DELETE FROM `gameobject_template` WHERE `entry` = 300500;
INSERT INTO `gameobject_template`
  (`entry`, `type`, `displayId`, `name`, `IconName`, `castBarCaption`, `unk1`, `size`,
   `Data0`, `Data1`, `Data2`, `Data3`, `Data4`, `Data5`, `Data6`, `Data7`, `Data8`, `Data9`,
   `Data10`, `Data11`, `Data12`, `Data13`, `Data14`, `Data15`, `Data16`, `Data17`, `Data18`,
   `Data19`, `Data20`, `Data21`, `Data22`, `Data23`, `AIName`, `ScriptName`, `VerifiedBuild`)
VALUES
  (300500, 6, 5932, 'Recharge Buff', '', '', '', 1,
   0, 0, 0, 90200, 0, 3, 0, 0, 0, 0,
   0, 0, 0, 0, 0, 0, 0, 0, 0,
   0, 0, 0, 0, 0, '', '', 0);

-- Binds the aura script that does the actual cooldown wipe. Without this row
-- the rune applies a 10s aura that does nothing at all.
DELETE FROM `spell_script_names` WHERE `spell_id` = 90200;
INSERT INTO `spell_script_names` (`spell_id`, `ScriptName`) VALUES
  (90200, 'spell_gen_recharge');
