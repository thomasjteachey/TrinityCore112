/*
 * Violet Hold class boons - the ARTIFACT tier (2026-11-13).
 *
 * Offered only to their class, rolled a little rarer than a legendary weapon
 * (VioletHoldBoons::ARTIFACT_ROLL_QUARTERS) and shown on the top card frame.
 * Each boon is a unique record aura; everything it changes is here, called
 * from the core at the one place each rule lives, plus the spell scripts in
 * scripts/Custom/violet_hold_class_boons.cpp and two edits in spell_paladin.cpp.
 *
 * Every hook answers "no" / 0 for anyone who does not hold the boon, so the
 * callers stay cheap: one HasAura on the unit asking.
 *
 * What the client has to agree with, and how it is told:
 *  - Relentless, Trapper: the record aura carries ABILITY_IGNORE_AURASTATE
 *    (262) on Overpower/Revenge and (misc 1, combat) on the trap spells. The
 *    client honours 262 itself (Taste for Blood, Warbringer). Overpower also
 *    needs a combo point on the target client side, so the holder keeps one
 *    on whatever they have selected (RefreshReactives).
 *  - Transmutation: the record aura is a -100% cast-time spellmod on Polymorph.
 *  - Sealkeeper (2026-11-14): the record aura IS three seal spellmods
 *    (duration +100%, cost -100%, global cooldown -500 ms, paladin mask
 *    0x0A000200 = every seal), so the client shows and times them itself.
 *  - Barrage: the record aura is a +97 JUMP_TARGETS spellmod on Multi-Shot so
 *    the client advertises an uncapped shot. Server selection is caster-centered
 *    and unlimited; see IsBarrage/BarrageAvoidsTarget below.
 *  - Serenity's ready charge (94382) is a -100% cost spellmod on Tranquility.
 *  - Warlord, Trapper: the cast's shared category cooldown is re-sent as a
 *    per-spell cooldown after SMSG_SPELL_GO (SpellHistory::SyncUnboundCategoryToClient,
 *    the T2 unbound-shock recipe).
 *  - Spellbreaker: every cleared lockout is an SMSG_CLEAR_COOLDOWN; the enemy
 *    side receives the interrupted school's normal Counterspell lockout.
 *  - NOT reachable without the client DLL (see the report): Point Blank's
 *    minimum range and starting Aimed/Multi-Shot while moving, the Untamed
 *    shapeshift while stunned/feared/polymorphed, and the totem bar for more
 *    than one totem per element.
 */

#ifndef TRINITYCORE_VIOLET_HOLD_CLASS_BOONS_H
#define TRINITYCORE_VIOLET_HOLD_CLASS_BOONS_H

#include "Define.h"
#include "ObjectGuid.h"
#include "SharedDefines.h"
#include <vector>

class Aura;
class BattlegroundVHR;
class Creature;
class GameObject;
class Player;
class Spell;
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

        // The second artifact block (the first party-oriented additions).
        SPELL_FIELD_MEDIC       = 94383,   // hunter
        SPELL_SLIPSTREAM        = 94384,   // mage
        SPELL_SECOND_COMING     = 94385,   // priest record aura
        SPELL_SECOND_COMING_READY = 94386, // its once-per-wave charge (OnWaveRestored)
        SPELL_AEGIS             = 94387,   // paladin record
        SPELL_AEGIS_SHIELD      = 94388,   // the allies' 4 sec bubble (Undying's immunities)
        SPELL_VENGEANCE         = 94389,   // warrior record
        SPELL_VENGEANCE_FURY    = 94390,   // 12 sec: +50% damage, crowd-control immunity
        SPELL_BODY_DOUBLE       = 94391,   // rogue record; also the 30 sec cooldown's id
        SPELL_BODY_DOUBLE_VEIL  = 94392,   // the ally's 6 sec: stealth + total immunity
        SPELL_PLAGUEBEARER      = 94393,   // warlock record
        SPELL_PLAGUE_BURDEN     = 94394,   // +10% damage per stack = per harmful effect carried
        SPELL_WILDCALLER        = 94395,   // druid record
        SPELL_WILDCALLER_CAT    = 94396,   // +15% damage done, 8 sec
        SPELL_WILDCALLER_BEAR   = 94397,   // 15% less damage taken, 8 sec
        SPELL_WILDCALLER_CLARITY = 94398,  // 25% cheaper mana costs, 8 sec (ModManaCost)
        SPELL_OWL_CHOIR         = 94399,   // moonkin look + 50% Echoes, 8 sec (spell_vhr_boon_echoes)
        SPELL_SPIRIT_LINK       = 94400,   // priest record (was the shaman's until 2026-11-15)
        SPELL_WINDLORD          = 94401,   // shaman record
        SPELL_PRIMAL_ECHO       = 94405,   // druid record

        // The 2026-11-18 approved class-artifact block.
        SPELL_UNENDING_VOLLEY   = 94406,   // hunter
        SPELL_WAR_BANNER        = 94407,   // warrior
        SPELL_ASPECT_SAVANT     = 94408,   // hunter
        SPELL_MIRROR_IMAGE      = 94409,   // mage; also its 5 sec cooldown key
        SPELL_MANA_CONDUIT      = 94410,   // mage, party
        SPELL_CURSEWEAVER       = 94411,   // warlock
        SPELL_PATIENT_GROWTH    = 94412,   // druid
        SPELL_CLEANSE           = 94413,   // paladin, party
        SPELL_LIGHTNING_ROD     = 94414,   // shaman

        SPELL_WINDFURY_WEAPON_PASSIVE = 33757,
        SPELL_ROGUE_EVASION     = 5277,

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
    // Warlord / Trapper: this spell keeps its category cooldown to itself
    // (every rank of the same spell shares it) - SpellHistory.
    TC_GAME_API bool IsCategoryUnbound(Unit const* owner, SpellInfo const* spellInfo);
    // Trapper: one of each trap down. Every trap-laying spell shares a summon
    // slot, so a new trap would otherwise take the last one up whatever its
    // kind; with the boon it replaces only the caster's earlier trap of the
    // same kind (any rank) - Spell::EffectSummonObject.
    TC_GAME_API bool KeepsOneOfEachTrap(Unit const* caster, SpellInfo const* spellInfo);
    TC_GAME_API std::vector<GameObject*> SameKindTraps(Unit const* caster, SpellInfo const* spellInfo);

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
    // Auto Shot keeps firing on the move, and Aimed Shot / Multi-Shot cast on
    // the move (Spell::prepare/update, Unit::_UpdateAutoRepeatSpell) -
    // Skirmisher was folded into it 2026-11-14.
    TC_GAME_API bool IgnoresMinRange(Unit const* caster, SpellInfo const* spellInfo);
    TC_GAME_API bool ShootsWhileMoving(Unit const* caster, SpellInfo const* spellInfo);
    // Barrage: Multi-Shot selects every valid unit in its full range around
    // the hunter, regardless of facing or the explicitly aimed target. Hostile
    // units under break-on-damage crowd control are excluded, including the
    // explicit target. Field Medic allies remain eligible because the shot
    // heals rather than damages them.
    TC_GAME_API bool IsBarrage(Unit const* caster, SpellInfo const* spellInfo);
    TC_GAME_API bool BarrageAvoidsTarget(Unit const* caster, SpellInfo const* spellInfo, Unit const* target);
    // Unending Volley: the channel becomes a placed permanent dynamic aura.
    // A new Volley removes every older rank first, so recasting moves it.
    TC_GAME_API bool IsUnendingVolley(Unit const* caster, SpellInfo const* spellInfo);
    TC_GAME_API void BeforeUnendingVolleyPlaced(Unit* caster);
    TC_GAME_API void MakeUnendingVolleyPermanent(Unit const* caster, SpellInfo const* spellInfo, Aura* aura);
    // Trapper (as well as the trap rules above): a holder's Freezing Trap does not break on damage
    // (Unit::RemoveAurasWithInterruptFlags, AURA_INTERRUPT_FLAG_TAKE_DAMAGE).
    TC_GAME_API bool HoldsThroughDamage(Aura const* aura);
    // Field Medic: the holder's damaging shots (hunter family, ranged damage
    // class, nothing but damage effects - Auto Shot, Arcane Shot, Multi-Shot,
    // Aimed Shot...) may be aimed at an ally and heal what they would have hit
    // for. ShootsAllies: this cast is such a shot from a holder.
    // IsFieldMedicHeal: and `target` is an ally it heals (not attackable,
    // assistable) - asked by SpellInfo::CheckExplicitTarget (target allowed),
    // Spell::SelectExplicitTargets (no redirect), Spell::AddUnitTarget (no
    // miss roll) and Spell::TargetInfo::DoDamageAndTriggers (damage -> heal,
    // no procs). Multi-Shot's jumps go to the nearest units whatever side
    // they are on (Spell::SelectImplicitChainTargets). The client half is the
    // client-tweaks DLL rule `fieldmedic`.
    TC_GAME_API bool ShootsAllies(WorldObject const* caster, SpellInfo const* spellInfo);
    TC_GAME_API bool IsFieldMedicHeal(WorldObject const* caster, SpellInfo const* spellInfo, Unit const* target);

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
    // Slipstream: the allies (players) within SLIPSTREAM_RADIUS of a holder
    // about to Blink (spell_vhr_frostwalker's BeforeCast, which carries Blink
    // for both boons) ...
    constexpr float SLIPSTREAM_RADIUS = 15.0f;
    TC_GAME_API std::vector<ObjectGuid> SlipstreamPassengers(Unit* mage);
    // ... are put where the mage landed and freed of every stun and root
    // (once the mage's own near teleport has gone through).
    TC_GAME_API void SlipstreamArrive(Unit* mage, std::vector<ObjectGuid> const& passengers);
    // Stun and root auras off `unit` (hostile ones), Slipstream's freeing.
    TC_GAME_API void FreeFromStunAndRoot(Unit* unit);
    // Frostwalker: the Frost Nova rank this mage knows (0 if none).
    TC_GAME_API uint32 GetFrostNovaRank(Unit const* caster);
    // Spellbreaker: Spell::EffectInterruptCast, after something was interrupted.
    // Clears allied lockouts and spreads the interrupted school's lockout to
    // every living enemy (and their controlled units) in the run.
    TC_GAME_API void OnSuccessfulInterrupt(Unit* caster, SpellInfo const* spellInfo, Unit* interruptedTarget,
        SpellSchoolMask schoolMask, uint32 lockoutTime);
    // Mirror Image: Spell::cancel, before the original cast is torn down.
    TC_GAME_API void OnSpellCanceled(Spell const* spell);
    // Mana Conduit: the 2-second periodic effect of Evocation.
    TC_GAME_API void OnEvocationTick(Unit* caster);

    // ---- priest (Second Coming) ---------------------------------------------
    // While the charge is up, priest Resurrection (family 6, RESURRECT_NEW) is
    // instant (WorldObject::ModSpellCastTime), castable in combat (Spell::
    // CheckCast) and full health (spell_vhr_second_coming, which also spends
    // the charge). The client half is the client-tweaks rule `secondcoming`.
    TC_GAME_API bool IsSecondComingResurrection(Unit const* caster, SpellInfo const* spellInfo);

    // ---- the rest of the second block (2026-11-14) -------------------------
    constexpr float AEGIS_RADIUS            = 20.0f;
    constexpr int32 VENGEANCE_KILL_HEAL_PCT = 10;
    constexpr float BODY_DOUBLE_RANGE       = 30.0f;
    constexpr uint32 BODY_DOUBLE_HEALTH_PCT = 25;
    constexpr uint32 BODY_DOUBLE_COOLDOWN_MS = 30000;
    constexpr float PLAGUEBEARER_RADIUS     = 20.0f;
    constexpr uint8 PLAGUEBEARER_MAX_STACKS = 50;
    constexpr float WILDCALLER_RANGE        = 40.0f;
    constexpr uint32 WILDCALLER_MANA_CUT_PCT = 25;
    constexpr float SPIRIT_LINK_RADIUS      = 20.0f;
    constexpr float WAR_BANNER_RADIUS       = 30.0f;
    constexpr float MANA_CONDUIT_RADIUS     = 40.0f;
    constexpr float LIGHTNING_ROD_WIDTH     = 2.0f;
    constexpr uint32 MIRROR_IMAGE_COOLDOWN_MS = 5000;
    constexpr uint32 LIGHTNING_ROD_PERIOD_MS  = 10000;
    constexpr uint32 GO_WAR_BANNER_ALLIANCE   = 900010;
    constexpr uint32 GO_WAR_BANNER_HORDE      = 900011;
    constexpr uint32 NPC_MIRROR_IMAGE          = 900120;
    constexpr uint32 NPC_PRIMAL_ECHO         = 900119;
    constexpr uint32 PRIMAL_ECHO_DURATION_MS = 12000;
    constexpr uint32 DATA_PRIMAL_ECHO_FORM   = 1;

    // Spell::_cast, a cast the caster chose (not triggered): Aegis spreads
    // Divine Shield's bubble.
    TC_GAME_API void OnSpellCast(Unit* caster, SpellInfo const* spellInfo, Unit* explicitTarget);
    // The banner's deliberately hostile zone: nearby enemies deal 110% and
    // take 90%. Called from the same UnitScript hooks as party damage boons.
    TC_GAME_API uint32 ModifyWarBannerDamage(Unit* attacker, Unit* victim, uint32 damage);
    // BattlegroundVHR::HandleKillPlayer, a member of the party fell:
    // every Vengeance holder in the run goes berserk, wherever they stand.
    TC_GAME_API void OnAllyDied(Player* dead);
    // The UnitScript damage hook, before `damage` lands on `victim`: a hit
    // that takes a party member below BODY_DOUBLE_HEALTH_PCT (not a lethal
    // one) calls a Body Double holder in.
    TC_GAME_API void OnAllyDamaged(Unit* victim, uint32 damage);
    // Once a second from BattlegroundVHR::PostUpdateImpl (`tick` counts up):
    // Plaguebearer's burden every tick and its pull every 3rd, Spirit Link
    // every 2nd.
    TC_GAME_API void UpdateRun(BattlegroundVHR* run, uint32 tick);
    // Wildcaller: the druid just came back to caster form
    // (AuraEffect::HandleAuraModShapeshift, the last form gone).
    TC_GAME_API void OnCasterForm(Unit* target, uint8 previousForm);
    // SpellInfo::CalcPowerCost, mana spells: Wildcaller's caster-form rally.
    TC_GAME_API int32 ModManaCost(Unit const* caster, int32 cost);
    // Windlord: Windfury Weapon's proc (Aura::CalcProcChance; its 3 sec
    // internal cooldown still applies) uses the boon's fixed 50% chance ...
    TC_GAME_API float ModProcChance(Aura const* aura, float chance);
    // ... and Windfury Totem's weapon enchant on anyone in a holder's party
    // (Player::CastItemCombatSpell) uses that same chance.
    TC_GAME_API float ModWindfuryTotemProcChance(Player const* wielder, uint32 enchantId, float chance);

    // Inside the Violet Hold, seals and Windfury Totem coexist as in the base
    // game: the realm's "a seal strips the totem's weapon buff" rule (Unit::
    // _ApplyAura) and "the totem skips seal carriers" (Windfury Totem
    // Effect's ExcludeTargetAuraState, SpellInfo::CheckTarget) are off for a
    // unit in a Violet Hold run. `spellInfo` null = the seal-strips-buff rule.
    TC_GAME_API bool KeepsWindfuryWithSeals(Unit const* unit, SpellInfo const* spellInfo);
    // Aspect Savant: suppress Cheetah/Pack's triggered daze proc.
    TC_GAME_API bool PreventsAspectDaze(Aura const* aura);

    // ---- warlock -----------------------------------------------------------
    // Indelible: Aura::CalcDispelChance -> 0.
    TC_GAME_API bool IsUndispellable(Aura const* aura, Unit const* auraTarget);
    // Curseweaver is dispatched from OnSpellCast; this declaration documents
    // that its other half is AurasStackFreely above.
    // Contagion and Eternal Pact: Unit::Kill, while the victim still has its
    // auras (any unit, not only in the Violet Hold - the holders only exist there).
    // Vengeance: a berserk killer (or its pet's owner) heals for the kill.
    TC_GAME_API void OnUnitDying(Unit* victim, Unit* attacker);

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
    // Patient Growth: Aura::UpdateOwner leaves both duration and HoT periodic
    // clocks untouched while this says true.
    TC_GAME_API bool PausesHealingOverTime(Aura const* aura, Unit const* target);

    // ---- shaman ------------------------------------------------------------
    // Stormcaller: Purge's dispel count becomes "everything", and every
    // successful removal is transferred to the shaman (Spell::EffectDispel).
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
