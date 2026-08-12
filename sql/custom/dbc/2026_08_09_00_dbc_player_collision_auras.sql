-- Player-collision auras (90210-90213).
--
-- Four buffs that switch on client-side player-vs-player collision. They span
-- two independent axes - WHO is affected, and whether the blocking is one-way or
-- mutual:
--
--   90210 Obstruction   enemies cannot pass through you   (you still pass them)
--   90211 Immovable     nobody can pass through you       (you still pass them)
--   90212 Bodycheck     mutual collision with enemies
--   90213 Solid Form    mutual collision with everyone
--
-- The auras are completely INERT server-side: nothing reads them, no script is
-- bound, and the effect is SPELL_AURA_DUMMY. All of the behaviour lives in the
-- client tweak DLL (tools\animspeedfix\playercollide.cpp), which each frame
-- clips the local player's own movement delta against nearby players. It decides
-- who blocks whom purely by reading these aura ids off the units it can see:
--
--     blocked by X  =  X has Immovable
--                   or X has Obstruction    and we are hostile
--                   or (I or X) has Solid Form
--                   or (I or X) has Bodycheck  and we are hostile
--
-- Hostility uses the client's own reaction test (the same one behind the
-- UnitIsEnemy Lua API), so it matches nameplate colouring exactly.
--
-- Consequences worth knowing:
--   * A player whose client does not have the DLL will walk through everyone.
--     Collision is enforced by each client on ITSELF, so it is only as reliable
--     as client coverage. client-tweaks is a mandatory launcher patch today.
--   * Because each client clips only its own movement, the one-way auras
--     (90210/90211) work without the wearer's client doing anything at all.
--
-- Donor is 23451 Speed, the same clean single-effect APPLY_AURA row used for the
-- Recharge rune: Attributes 0 (nothing hides the buff icon) and one effect.
-- Everything meaningful is overwritten below; see the byte-clone pitfalls note in
-- the spell pipeline docs before swapping donors.
--
-- SpellVisualID is zeroed. The donor carries 6922 - Speed's pickup effect - and a
-- byte-clone inherits it, which would put a speed-boost graphic on every one of
-- these. They are meant to be invisible: the buff icon is the only feedback.
--
-- Two donor traits MUST be cleared, both learned the hard way:
--
--   AuraInterruptFlags 0x20000 = AURA_INTERRUPT_FLAG_MOUNT. The donor is a
--   battleground speed rune, which is *meant* to drop when you mount - so a
--   byte-clone silently loses the aura the moment anyone gets on a mount.
--   Zeroed here: nothing about collision should care what you are riding.
--
--   AttributesExC gains SPELL_ATTR3_DEATH_PERSISTENT (0x100000) so the aura
--   survives dying. Without it every death strips it and it only comes back on
--   the next re-apply sweep. (Column mapping verified empirically: the spells
--   carrying this bit are Ghost, Honor 50%/100% and friends.)
--
--   Attributes gains SPELL_ATTR0_CANT_CANCEL (0x80000000) so a player cannot
--   right-click the buff off to switch collision back off for themselves. Note
--   this stops voluntary cancelling only - it is not dispel protection, and it
--   does not stop someone simply deleting the client DLL, which remains the real
--   hole (see the client-coverage note above).
--
--   AttributesEx gains SPELL_ATTR1_DONT_DISPLAY_IN_AURA_BAR (0x02000000) so the
--   buff draws no icon at all: these are mode switches, not player-facing buffs,
--   and with a server-wide collision aura the icon is permanent clutter.
--
--   Do NOT reach for SPELL_ATTR0_PASSIVE to hide them instead. Passive is not a
--   display flag - Aura::CanBeSentToClient() is `!IsPassive() || area-aura ||
--   ignore-aurastate`, so a passive aura is never given a client slot and never
--   reaches the client at all. The DLL gates collision by reading these ids off
--   the client's own aura table, so a passive version would be invisible to it
--   and collision would silently stop working. DONT_DISPLAY_IN_AURA_BAR is the
--   right flag: the aura is still sent and still in the aura table (which the DLL
--   walks by index), the client just does not draw it in the buff frame.
--
-- DurationIndex is taken from the donor (index 1 = 10s) and overridden to 21,
-- VERIFIED as Duration -1 (infinite) in dbc.spellduration_lplus - these are
-- mode-defining buffs, not timed pickups. Change it if they should expire.
--
-- Icon ids were verified to exist in dbc.spellicon_lplus. An id with no row there
-- renders as a blank/question-mark buff icon.
--
-- After applying: regenerate the binary Spell.dbc from this table with
-- tools\recolor\itemforge\spell_dbc.py (run --verify first, it must report 0
-- mismatches), deploy to ALL FOUR Spell.dbc homes (prod server, dev server,
-- itemforge/dbc, client patch), then repack the client patch. A table-only edit
-- is wiped by the next DB refresh.
--
-- Replayable: deletes its own ids before inserting.

DELETE FROM `spell_lplus` WHERE `ID` BETWEEN 90210 AND 90213;

DROP TEMPORARY TABLE IF EXISTS `tmp_collide`;
CREATE TEMPORARY TABLE `tmp_collide` AS
SELECT * FROM `spell_lplus` WHERE `ID` = 23451;

-- 90210 Obstruction - one-way, enemies only
UPDATE `tmp_collide` SET
  `ID` = 90210,
  `Name_Lang_enUS` = 'Obstruction',
  `Description_Lang_enUS` = '',
  `AuraDescription_Lang_enUS` = 'Enemies cannot pass through you. You may still pass through them.',
  `EffectAura_1` = 4,
  `EffectBasePoints_1` = 0,
  `DurationIndex` = 21,
  `SpellVisualID_1` = 0,   -- no visual: the donor's 6922 is Speed's pickup effect
  `SpellVisualID_2` = 0,
  `AuraInterruptFlags` = 0,      -- donor carried 0x20000 AURA_INTERRUPT_FLAG_MOUNT
  `AttributesExC` = 1048576,     -- SPELL_ATTR3_DEATH_PERSISTENT
  `Attributes` = 2147483648,     -- SPELL_ATTR0_CANT_CANCEL: player cannot right-click it off
  `AttributesEx` = 33554432,     -- SPELL_ATTR1_DONT_DISPLAY_IN_AURA_BAR: no buff icon
  `SpellIconID` = 28;      -- Ability_Defend
INSERT INTO `spell_lplus` SELECT * FROM `tmp_collide`;

-- 90211 Immovable - one-way, everyone
UPDATE `tmp_collide` SET
  `ID` = 90211,
  `Name_Lang_enUS` = 'Immovable',
  `AuraDescription_Lang_enUS` = 'No one can pass through you. You may still pass through them.',
  `SpellIconID` = 281;     -- Ability_Warrior_ShieldWall
INSERT INTO `spell_lplus` SELECT * FROM `tmp_collide`;

-- 90212 Bodycheck - mutual, enemies only
UPDATE `tmp_collide` SET
  `ID` = 90212,
  `Name_Lang_enUS` = 'Bodycheck',
  `AuraDescription_Lang_enUS` = 'You and your enemies cannot pass through one another.',
  `SpellIconID` = 280;     -- Ability_Warrior_ShieldBash
INSERT INTO `spell_lplus` SELECT * FROM `tmp_collide`;

-- 90213 Solid Form - mutual, everyone
UPDATE `tmp_collide` SET
  `ID` = 90213,
  `Name_Lang_enUS` = 'Solid Form',
  `AuraDescription_Lang_enUS` = 'You and all other players cannot pass through one another.',
  `SpellIconID` = 2007;    -- Ability_Warrior_ShieldMastery
INSERT INTO `spell_lplus` SELECT * FROM `tmp_collide`;

DROP TEMPORARY TABLE `tmp_collide`;
