/*
 * Violet Hold class boons - the ARTIFACT tier (2026-11-13).
 *
 * Three per class, offered only to that class, rolled a little rarer than a
 * legendary weapon (VioletHoldBoons::ARTIFACT_ROLL_QUARTERS) and shown on the
 * top card frame. The boon itself is a unique record aura (Boon::Juggernaut ..
 * Boon::Ancestors, spells 94340-94366); everything it changes is here, called
 * from the core at the one place each rule lives, plus the spell scripts in
 * scripts/Custom/violet_hold_class_boons.cpp and two edits in spell_paladin.cpp.
 *
 * Every hook answers "no" / 0 for anyone who does not hold the boon, so the
 * callers stay cheap: one HasAura on the unit asking.
 *
 * What the client has to agree with, and how it is told:
 *  - Relentless: the record aura carries ABILITY_IGNORE_AURASTATE (262) on
 *    Overpower/Revenge. The client honours 262 itself (Taste for Blood,
 *    Warbringer). Overpower also needs a combo point on the target client
 *    side, so the holder keeps one on whatever they have selected
 *    (RefreshReactives).
 *  - Transmutation: the record aura is a -100% cast-time spellmod on Polymorph.
 *  - Sealkeeper (2026-11-14): the record aura IS three seal spellmods
 *    (duration +100%, cost -100%, global cooldown -500 ms, paladin mask
 *    0x0A000200 = every seal), so the client shows and times them itself.
 *  - Barrage: the record aura is a +97 JUMP_TARGETS spellmod on Multi-Shot.
 *  - Serenity's ready charge (94382) is a -100% cost spellmod on Tranquility.
 *  - Warlord: the cast's shared category cooldown is re-sent as a per-spell
 *    cooldown after SMSG_SPELL_GO (SpellHistory::SyncUnboundCategoryToClient,
 *    the T2 unbound-shock recipe).
 *  - Spellbreaker: every cleared lockout is an SMSG_CLEAR_COOLDOWN.
 *  - NOT reachable without the client DLL (see the report): Point Blank's
 *    minimum range, the Untamed shapeshift while stunned/feared/polymorphed,
 *    and the totem bar for more than one totem per element.
 */

#ifndef TRINITYCORE_VIOLET_HOLD_CLASS_BOONS_H
#define TRINITYCORE_VIOLET_HOLD_CLASS_BOONS_H

#include "Define.h"
#include <vector>

class Aura;
class Creature;
class GameObject;
class Player;
class SpellInfo;
class Totem;
class Unit;
class WorldObject;

namespace VioletHoldClassBoons
{
    enum Spells : uint32
    {
        // The record auras, in Boon order (VioletHoldBoons::SPELL_BOON_CLASS_FIRST).
        SPELL_JUGGERNAUT        = 94340,
        SPELL_RELENTLESS        = 94341,
        SPELL_WARLORD           = 94342,
        SPELL_SEALKEEPER        = 94343,   // was Unbound Light (no Forbearance) until 2026-11-14
        SPELL_RADIANT_HOST      = 94344,
        SPELL_JUDGEMENT_DAY     = 94345,
        SPELL_POINT_BLANK       = 94346,
        SPELL_TRAPPER           = 94347,
        SPELL_BARRAGE           = 94348,   // was Skirmisher until 2026-11-14 (folded into Point Blank)
        SPELL_ASSASSIN          = 94349,
        SPELL_UNSEEN            = 94350,
        SPELL_CRIPPLER          = 94351,
        SPELL_DREAD             = 94352,
        SPELL_SHATTERED_WARD    = 94353,
        SPELL_AGONY             = 94354,
        SPELL_FROSTWALKER       = 94355,
        SPELL_SPELLBREAKER      = 94356,
        SPELL_TRANSMUTATION     = 94357,
        SPELL_CONTAGION         = 94358,
        SPELL_INDELIBLE         = 94359,
        SPELL_ETERNAL_PACT      = 94360,
        SPELL_UNTAMED           = 94361,
        SPELL_WELLSPRING        = 94362,
        SPELL_SERENITY          = 94363,
        SPELL_STORMCALLER       = 94364,
        SPELL_TOTEM_LORD        = 94365,
        SPELL_ANCESTORS         = 94366,

        // What the hooks cast. The three damage spells are the Ricochet
        // helper's shape (no crit, no done bonus - the amount already has it -,
        // ignores hit, procs nothing); the heal is the Vampire heal's, aimed at
        // an ally; Wellspring is Innervate's periodic mana gain, 5 sec.
        SPELL_SINISTER_CLEAVE   = 94367,   // Sinister Strike's second target
        SPELL_SHATTERED_SHIELD  = 94368,   // Power Word: Shield's explosion (holy)
        SPELL_AGONY_DAMAGE      = 94369,   // Shadow Word: Pain, all at once
        SPELL_SERENITY_HEAL     = 94370,   // Tranquility, all at once
        SPELL_WELLSPRING_MANA   = 94371,   // the 5 sec Innervate

        // Serenity's once-per-wave charge: while it is up Tranquility costs
        // nothing (its own -100% cost spellmod, so the client agrees) and lands
        // all at once; the cast spends it. Given on taking the boon and as
        // every wave falls (OnWaveRestored).
        SPELL_SERENITY_READY    = 94382,

        // Stock "Resurrection Impact Visual", the Ancestors' rising.
        SPELL_ANCESTORS_VISUAL  = 24171,
    };

    constexpr float JUGGERNAUT_RADIUS          = 8.0f;
    constexpr float JUDGEMENT_DAY_RADIUS       = 20.0f;   // around the judged target
    constexpr float SHATTERED_WARD_RADIUS      = 10.0f;
    constexpr float CONTAGION_RADIUS           = 20.0f;
    constexpr float SINISTER_CLEAVE_REACH      = 5.0f;    // beyond both combat reaches
    constexpr int32 CRIPPLER_MULTIPLIER        = 3;
    constexpr uint32 ETERNAL_PACT_COOLDOWN_MS  = 20000;
    constexpr uint32 ETERNAL_PACT_DELAY_MS     = 500;     // after the corpse is cleared away
    constexpr uint32 WELLSPRING_THRESHOLD_PCT  = 50;    // once per wave (BattlegroundVHR::ClaimOncePerWave)
    // Half a real Innervate (225% of base mana over its 10 sec): 112.5% over
    // 5 sec, so 22.5% of base mana every second.
    constexpr uint32 WELLSPRING_PERMILLE_PER_TICK = 225;
    constexpr uint32 TOTEM_LORD_DURATION_MS    = 24 * 60 * 60 * 1000;
    constexpr float TOTEM_LORD_FOLLOW_DISTANCE = 6.0f;
    constexpr float TOTEM_LORD_SPEED_RATE      = 0.5f;    // half a normal run, "slowly"
    constexpr uint32 FROSTWALKER_LANDING_CHECK_MS = 100;
    // Reincarnation (21169) is what the Ancestors restore with: its own 20%
    // health and mana, plus whatever Improved Reincarnation adds to it.
    constexpr uint32 SPELL_REINCARNATION_RESURRECT = 21169;

    // ---- boon bookkeeping (VioletHoldBoons::Take / StripAll) ----------------
    TC_GAME_API void OnTaken(Player* player);
    TC_GAME_API void OnStripped(Player* player);

    // ---- warrior -----------------------------------------------------------
    // Juggernaut: the stun spells Charge / Intercept trigger (script).
    TC_GAME_API bool IsJuggernautStun(uint32 spellId);
    // Relentless: Unit::UpdateReactives must not let the Revenge state lapse.
    TC_GAME_API bool KeepsReactivesLit(Unit const* unit);
    // Relentless: Revenge's aura state on, and one combo point on the selected
    // enemy for Overpower. Player::SetSelection, after a finisher, on pick.
    TC_GAME_API void RefreshReactives(Unit* unit);
    // Relentless: Overpower needs no combo point on the server (Spell::CheckCast).
    TC_GAME_API bool IgnoresComboRequirement(Unit const* caster, SpellInfo const* spellInfo);
    // Warlord: this spell keeps its category cooldown to itself (every rank of
    // the same spell shares it) - SpellHistory.
    TC_GAME_API bool IsCategoryUnbound(Unit const* owner, SpellInfo const* spellInfo);

    // ---- paladin -----------------------------------------------------------
    // Radiant Host: the caster's Auras do not push each other off. Sealkeeper:
    // nor do their seals, nor the judgements those seals leave on a target
    // (Aura::CanStackWith).
    TC_GAME_API bool AurasStackFreely(Unit const* caster, SpellInfo const* a, SpellInfo const* b);
    // Sealkeeper: Judgement judges every seal the paladin has up, not only the
    // freshest (spell_paladin.cpp, Zealot's Persistence's path). Judgement Day
    // then carries every one of them to each enemy it splashes.
    TC_GAME_API bool JudgesEverySeal(Unit const* caster);
    // Judgement Day: every other enemy within JUDGEMENT_DAY_RADIUS of the judged
    // target, who is judged too (spell_paladin.cpp).
    TC_GAME_API void CollectJudgementSplash(Unit* caster, Unit* mainTarget, std::vector<Unit*>& out);

    // ---- hunter ------------------------------------------------------------
    // Point Blank: no minimum range for the hunter's shots (Spell::GetMinMaxRange),
    // and Auto Shot keeps firing on the move (Unit::_UpdateAutoRepeatSpell,
    // Spell::CheckCast) - Skirmisher was folded into it 2026-11-14.
    TC_GAME_API bool IgnoresMinRange(Unit const* caster, SpellInfo const* spellInfo);
    TC_GAME_API bool ShootsWhileMoving(Unit const* caster, SpellInfo const* spellInfo);
    // Trapper: a holder's Freezing Trap does not break on damage
    // (Unit::RemoveAurasWithInterruptFlags, AURA_INTERRUPT_FLAG_TAKE_DAMAGE).
    TC_GAME_API bool HoldsThroughDamage(Aura const* aura);

    // ---- rogue -------------------------------------------------------------
    // Assassin: Backstab from the front (Spell::CheckCast) and unavoidable
    // (Unit::MeleeSpellHitResult); Hemorrhage +1 combo point (Spell finish).
    TC_GAME_API bool WaivesBehindRequirement(Unit const* caster, SpellInfo const* spellInfo);
    TC_GAME_API bool IgnoresAvoidance(Unit const* caster, SpellInfo const* spellInfo);
    TC_GAME_API int8 GetExtraComboPoints(Unit const* caster, SpellInfo const* spellInfo);
    TC_GAME_API bool IsSinisterStrike(SpellInfo const* spellInfo);
    // Unseen: Stealth's take-damage proc never fires (Aura::GetProcEffectMask).
    TC_GAME_API bool StealthHoldsThroughHits(Unit const* owner, SpellInfo const* stealth);
    // Crippler: x3 on Kidney Shot (WorldObject::ModSpellDuration). 1 otherwise.
    TC_GAME_API int32 GetDurationMultiplier(WorldObject const* caster, SpellInfo const* spellInfo);

    // ---- priest ------------------------------------------------------------
    // Dread: the scream's fear ignores damage (AuraEffect::HandleBreakableCCAuraProc).
    TC_GAME_API bool FearHoldsThroughDamage(Unit const* caster, SpellInfo const* fear);

    // ---- mage --------------------------------------------------------------
    // Transmutation: how many targets the caster may keep this single-target
    // aura on at once (Unit::_AddAura); 0 = no say, the caller's own limit.
    constexpr uint32 TRANSMUTATION_POLYMORPH_TARGETS = 3;
    TC_GAME_API uint32 GetSingleTargetLimit(Unit const* caster, SpellInfo const* spellInfo);
    // Frostwalker: the Frost Nova rank this mage knows (0 if none).
    TC_GAME_API uint32 GetFrostNovaRank(Unit const* caster);
    // Spellbreaker: Spell::EffectInterruptCast, after something was interrupted.
    TC_GAME_API void OnSuccessfulInterrupt(Unit* caster, SpellInfo const* spellInfo);

    // ---- warlock -----------------------------------------------------------
    // Indelible: Aura::CalcDispelChance -> 0.
    TC_GAME_API bool IsUndispellable(Aura const* aura, Unit const* auraTarget);
    // Contagion and Eternal Pact: Unit::Kill, while the victim still has its
    // auras (any unit, not only in the Violet Hold - the holders only exist there).
    TC_GAME_API void OnUnitDying(Unit* victim);

    // ---- druid -------------------------------------------------------------
    // Untamed: a shapeshift ignores stun/fear/confuse on the server
    // (Spell::CheckCasterAuras) and breaks them (HandleAuraModShapeshift).
    TC_GAME_API bool ShiftsThroughControl(Unit const* caster, SpellInfo const* spellInfo);
    TC_GAME_API void OnShapeshift(Unit* target, SpellInfo const* form);
    // Wellspring: Unit::ModifyPower, mana going from `before` to `after`.
    TC_GAME_API void OnManaChange(Unit* unit, int32 before, int32 after);
    // Serenity: a wave just fell and `player` was put back on their feet
    // (BattlegroundVHR::RestorePartyAfterWave) - the ready charge comes back.
    TC_GAME_API void OnWaveRestored(Player* player);

    // ---- shaman ------------------------------------------------------------
    // Stormcaller: Purge's dispel count becomes "everything" (Spell::EffectDispel).
    TC_GAME_API bool DispelsEverything(WorldObject const* caster, SpellInfo const* spellInfo);
    // Totem Lord: Spell::EffectSummonType, before the totem is made: the
    // holder's earlier copy of this same totem goes, and the duration becomes
    // TOTEM_LORD_DURATION_MS.
    TC_GAME_API void BeforeTotemSummon(Unit* owner, SpellInfo const* totemSpell, int32& duration);
    // TempSummon::InitStats: a totem taking a holder's slot leaves the one it
    // displaces standing (the client bar shows the newest).
    TC_GAME_API bool KeepsDisplacedTotem(Unit const* owner, Creature const* displaced);
    // Totem::UnSummon, the slot just freed: an older totem still standing
    // for that element takes the bar back.
    TC_GAME_API void OnTotemSlotFreed(Unit* owner, uint8 slot, Totem const* leaving);
    // Totem::Update: walk after the holder.
    TC_GAME_API void UpdateTotemFollow(Totem* totem);
    // Unit::UnsummonAllTotems: every totem still standing, the ones no slot
    // points at included.
    TC_GAME_API void UnsummonLooseTotems(Unit* owner);
    // Ancestors, Phoenix's shape (Unit::DealDamage, after Undying, before
    // Phoenix): once per wave a blow that would kill a holder of the party is
    // clamped to leave one health and spends that wave's Reincarnation ...
    TC_GAME_API bool TryAncestors(Unit* victim);
    // ... which then lands, once the damage has: Reincarnation's health and
    // mana (20%, raised by Improved Reincarnation's ALL_EFFECTS modifier).
    TC_GAME_API void CompleteAncestors(Unit* victim);
}

#endif // TRINITYCORE_VIOLET_HOLD_CLASS_BOONS_H
