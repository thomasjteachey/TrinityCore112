-- Prowl ranks 1-2 break exactly like rogue Stealth.
--
-- There are two different mechanisms for dropping a stealth aura on damage and
-- the Prowl ranks were split across both.
--
-- Rogue Stealth (1784) and Prowl rank 3 (9913) use the PROC system:
-- AuraInterruptFlags 0x00023C04 carries neither TAKE_DAMAGE nor HITBYSPELL, and
-- the break comes from ProcTypeMask 0x000A22A8 - every TAKEN hit class,
-- including TAKEN_PERIODIC - plus a single charge, which
-- SpellMgr::LoadSpellProcs turns into a generated proc entry that the charge
-- drop then consumes. This is stock Blizzard data.
--
-- Prowl ranks 1-2 (5215, 6783) had been converted to the other mechanism: proc
-- flags zeroed, AuraInterruptFlags 0x00003C07 (HITBYSPELL | TAKE_DAMAGE), so
-- they dropped through Unit::RemoveAurasWithInterruptFlags instead. A druid's
-- low ranks therefore broke on things the max rank and rogue Stealth ignore.
-- HITBYSPELL is the flag that makes ANY hostile spell hit break the aura even
-- when it deals no damage, which is why Spell::PreprocessSpellHit carries
-- hardcoded exemptions for Earthbind Totem (6474/3600) and the Violet Hold
-- Recharge rune (90200). Those exemptions stay - Vanish (1856/11327) still
-- holds HITBYSPELL - but they no longer have to cover Prowl.
--
-- After this, damage breaks all three Prowl ranks through the proc path exactly
-- as it breaks Stealth, including fully absorbed damage (Power Word: Shield,
-- Sacrificial Aura redirects) now that MOD_STEALTH proc entries accept
-- PROC_HIT_ABSORB in SpellMgr::LoadSpellProcs. Two behaviours change to match
-- Stealth rather than to keep the old Prowl handling: zero-damage hostile
-- effects no longer break Prowl, and neither does environmental damage
-- (falling, lava), which fires no procs and only ever reached Prowl through
-- TAKE_DAMAGE. The added 0x20000 MOUNT bit is inert for a form-locked aura.
--
-- Values are Stealth's, written literally rather than copied from 1784 so the
-- statement cannot be broken by MySQL's rules about reading the table it
-- updates. Verify with the SELECT at the bottom: all four rows must agree.
--
-- The binary C:\Projects\Gamedev\wow\data\dbc\lplus\Spell.dbc has been patched
-- with the same four values in place (backup: Spell.dbc.bak-prowl-20260810).
-- Deploy that file to the server data dirs. Nothing user-visible changes - no
-- name, icon or tooltip text is touched and the server is what removes the
-- aura - so the client copy can ride along with the next repack.
--
-- Replayable.

UPDATE `spell_lplus`
SET `AuraInterruptFlags` = 146436,   -- 0x00023C04  CAST|TALK|LOOTING|MELEE_ATTACK|SPELL_ATTACK|MOUNT
    `ProcTypeMask`       = 664232,   -- 0x000A22A8  all TAKEN hit classes incl. TAKEN_PERIODIC
    `ProcChance`         = 100,
    `ProcCharges`        = 1
WHERE `ID` IN (5215, 6783);

-- Verification: 1784 Stealth, 5215/6783/9913 Prowl must all report the same
-- four values.
SELECT `ID`, `Name_Lang_enUS`, `AuraInterruptFlags`, `ProcTypeMask`, `ProcChance`, `ProcCharges`
FROM `spell_lplus`
WHERE `ID` IN (1784, 5215, 6783, 9913)
ORDER BY `ID`;
