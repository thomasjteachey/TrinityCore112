/*
 * Violet Hold class boons (artifact tier) - the parts that hang off one class
 * spell. The rules that live in the core are in
 * game/Miscellaneous/VioletHoldClassBoons.{h,cpp}; this file only has the
 * spell scripts. Every script does nothing unless the caster holds the boon.
 *
 * spell_script_names (applied to the world databases, not kept in the repo):
 *   spell_vhr_juggernaut      7922, 20253, 20614, 20615, 25273, 25274   Charge / Intercept stun
 *   spell_vhr_sinister_strike every player rank of Sinister Strike
 *   spell_vhr_shattered_ward  every player rank of Power Word: Shield
 *   spell_vhr_agony           every player rank of Shadow Word: Pain
 *   spell_vhr_frostwalker     1953                                      Blink (Frostwalker + Slipstream)
 *   spell_vhr_second_coming   2006, 2010, 10880, 10881, 20770, 25435, 48171  Resurrection
 *   spell_vhr_serenity        740, 8918, 9862, 9863                     Tranquility
 */

#include "CellImpl.h"
#include "BattlegroundVHR.h"
#include "Corpse.h"
#include "ObjectAccessor.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedCreature.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "TemporarySummon.h"
#include "VioletHoldBoons.h"
#include "VioletHoldClassBoons.h"

#include <algorithm>
#include <limits>
#include <list>
#include <vector>

using namespace VioletHoldClassBoons;

namespace
{
std::vector<Unit*> EnemiesAround(WorldObject* around, Unit* attacker, float radius)
{
    std::vector<Unit*> out;
    if (!around || !attacker || !around->IsInWorld())
        return out;

    std::list<Unit*> found;
    Trinity::AnyUnfriendlyUnitInObjectRangeCheck check(around, attacker, radius);
    Trinity::UnitListSearcher<Trinity::AnyUnfriendlyUnitInObjectRangeCheck> searcher(around, found, check);
    Cell::VisitAllObjects(around, searcher, radius);

    for (Unit* unit : found)
        if (unit != around && unit->IsAlive() && attacker->IsValidAttackTarget(unit))
            out.push_back(unit);
    return out;
}

// Frostwalker: the landing nova waits for the client to finish the Blink
// (a player's near teleport only moves them on its ack).
// Also where Slipstream's passengers are put down: on the spot the mage
// actually landed.
class FrostwalkerLandingEvent : public BasicEvent
{
public:
    FrostwalkerLandingEvent(Player* mage, uint32 novaSpell, std::vector<ObjectGuid> passengers, uint8 tries)
        : _mage(mage), _novaSpell(novaSpell), _passengers(std::move(passengers)), _tries(tries) { }

    bool Execute(uint64 /*time*/, uint32 /*diff*/) override
    {
        if (!_mage->IsInWorld() || !_mage->IsAlive())
            return true;
        if (_mage->IsBeingTeleportedNear() && _tries < 20)
        {
            _mage->m_Events.AddEventAtOffset(new FrostwalkerLandingEvent(_mage, _novaSpell, std::move(_passengers), uint8(_tries + 1)),
                Milliseconds(FROSTWALKER_LANDING_CHECK_MS));
            return true;
        }
        if (_novaSpell)
            _mage->CastSpell(_mage, _novaSpell, TRIGGERED_FULL_MASK);
        if (!_passengers.empty())
            SlipstreamArrive(_mage, _passengers);
        return true;
    }

private:
    Player* _mage;
    uint32 _novaSpell;
    std::vector<ObjectGuid> _passengers;
    uint8 _tries;
};
}

// Boon of the Juggernaut: the stun Charge or Intercept lands also lands on
// every other enemy within JUGGERNAUT_RADIUS of the target.
class spell_vhr_juggernaut : public SpellScript
{
    PrepareSpellScript(spell_vhr_juggernaut);

    void SpreadStun()
    {
        // The copies are this spell too; they must not spread again.
        if (_spreading)
            return;
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        if (!caster || !target || !caster->HasAura(SPELL_JUGGERNAUT) || !IsJuggernautStun(GetSpellInfo()->Id))
            return;

        _spreading = true;
        for (Unit* enemy : EnemiesAround(target, caster, JUGGERNAUT_RADIUS))
            caster->CastSpell(enemy, GetSpellInfo()->Id, TRIGGERED_FULL_MASK);
        _spreading = false;
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_vhr_juggernaut::SpreadStun);
    }

    static thread_local bool _spreading;
};
thread_local bool spell_vhr_juggernaut::_spreading = false;

// Boon of the Assassin: Sinister Strike also hits the nearest other enemy in
// melee reach, for what it did to the first.
class spell_vhr_sinister_strike : public SpellScript
{
    PrepareSpellScript(spell_vhr_sinister_strike);

    void Cleave()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        int32 const damage = GetHitDamage();
        if (!caster || !target || damage <= 0 || !caster->HasAura(SPELL_ASSASSIN) || !IsSinisterStrike(GetSpellInfo()))
            return;

        Unit* second = nullptr;
        float best = std::numeric_limits<float>::max();
        for (Unit* enemy : EnemiesAround(caster, caster, SINISTER_CLEAVE_REACH + caster->GetCombatReach() + 5.0f))
        {
            if (enemy == target || !caster->IsWithinMeleeRange(enemy))
                continue;
            float const dist = caster->GetExactDist(enemy);
            if (dist < best)
            {
                best = dist;
                second = enemy;
            }
        }
        if (!second)
            return;

        CastSpellExtraArgs args(TRIGGERED_FULL_MASK);
        args.AddSpellBP0(damage);
        caster->CastSpell(second, SPELL_SINISTER_CLEAVE, args);
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_vhr_sinister_strike::Cleave);
    }
};

// Boon of the Shattered Ward: when the holder's Power Word: Shield breaks or
// runs out, it explodes for everything it absorbed onto enemies within
// SHATTERED_WARD_RADIUS of whoever wore it.
class spell_vhr_shattered_ward : public AuraScript
{
    PrepareAuraScript(spell_vhr_shattered_ward);

    void CountAbsorb(AuraEffect* /*aurEff*/, DamageInfo& /*dmgInfo*/, uint32& absorbAmount)
    {
        _absorbed += absorbAmount;
    }

    void Explode(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        AuraRemoveMode const how = GetTargetApplication()->GetRemoveMode();
        // Broken (absorbed to nothing) or expired; not dispelled, cancelled or recast.
        if (how != AURA_REMOVE_BY_ENEMY_SPELL && how != AURA_REMOVE_BY_EXPIRE)
            return;
        Unit* caster = GetCaster();
        Unit* wearer = GetTarget();
        if (!_absorbed || !caster || !wearer || !caster->HasAura(SPELL_SHATTERED_WARD) || !caster->IsInMap(wearer))
            return;

        CastSpellExtraArgs args(TRIGGERED_FULL_MASK);
        args.AddSpellBP0(int32(std::min<uint32>(_absorbed, uint32(std::numeric_limits<int32>::max()))));
        for (Unit* enemy : EnemiesAround(wearer, caster, SHATTERED_WARD_RADIUS))
            caster->CastSpell(enemy, SPELL_SHATTERED_SHIELD, args);
    }

    void Register() override
    {
        AfterEffectAbsorb += AuraEffectAbsorbFn(spell_vhr_shattered_ward::CountAbsorb, EFFECT_0);
        AfterEffectRemove += AuraEffectRemoveFn(spell_vhr_shattered_ward::Explode, EFFECT_0, SPELL_AURA_SCHOOL_ABSORB, AURA_EFFECT_HANDLE_REAL);
    }

    uint32 _absorbed = 0;
};

// Boon of Agony: the holder's Shadow Word: Pain deals every tick at once (the
// aura's per-tick amount already carries the caster's bonuses) and goes.
class spell_vhr_agony : public SpellScript
{
    PrepareSpellScript(spell_vhr_agony);

    void AllAtOnce()
    {
        Unit* caster = GetCaster();
        Unit* target = GetHitUnit();
        Aura* aura = GetHitAura();
        if (!caster || !target || !aura || !caster->HasAura(SPELL_AGONY))
            return;

        int64 total = 0;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (AuraEffect const* eff = aura->GetEffect(i))
                if (eff->GetAuraType() == SPELL_AURA_PERIODIC_DAMAGE)
                    total += int64(std::max(eff->GetAmount(), 0)) * eff->GetTotalTicks();
        if (total <= 0)
            return;

        aura->Remove();

        CastSpellExtraArgs args(TRIGGERED_FULL_MASK);
        args.AddSpellBP0(int32(std::min<int64>(total, std::numeric_limits<int32>::max())));
        caster->CastSpell(target, SPELL_AGONY_DAMAGE, args);
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_vhr_agony::AllAtOnce);
    }
};

// Boon of the Frostwalker: Blink casts the mage's Frost Nova where they leave
// and again where they land. Boon of the Slipstream: every ally within 10 yd
// when the mage Blinks is put down where the mage lands, and every stun and
// root on them (and the mage) is broken.
class spell_vhr_frostwalker : public SpellScript
{
    PrepareSpellScript(spell_vhr_frostwalker);

    void Departure()
    {
        Player* mage = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!mage)
            return;
        // Taken before the Nova below, which roots nobody on the mage's side.
        _passengers = SlipstreamPassengers(mage);
        if (!mage->HasAura(SPELL_FROSTWALKER))
            return;
        _nova = GetFrostNovaRank(mage);
        if (_nova)
            mage->CastSpell(mage, _nova, TRIGGERED_FULL_MASK);
    }

    void Landing()
    {
        Player* mage = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!mage || (!_nova && _passengers.empty()))
            return;
        mage->m_Events.AddEventAtOffset(new FrostwalkerLandingEvent(mage, _nova, std::move(_passengers), 0),
            Milliseconds(FROSTWALKER_LANDING_CHECK_MS));
    }

    void Register() override
    {
        BeforeCast += SpellCastFn(spell_vhr_frostwalker::Departure);
        AfterCast += SpellCastFn(spell_vhr_frostwalker::Landing);
    }

    uint32 _nova = 0;
    std::vector<ObjectGuid> _passengers;
};

// Boon of the Second Coming: with the once-per-wave charge up, Resurrection
// (made instant and allowed in combat in the core) brings the ally back at
// full health, and the cast spends the charge.
class spell_vhr_second_coming : public SpellScript
{
    PrepareSpellScript(spell_vhr_second_coming);

    void FullHealth(SpellEffIndex /*effIndex*/)
    {
        if (!IsSecondComingResurrection(GetCaster(), GetSpellInfo()))
            return;
        Player* raised = GetHitPlayer();
        if (!raised)
            if (Corpse* corpse = GetHitCorpse())
                raised = ObjectAccessor::FindPlayer(corpse->GetOwnerGUID());
        if (raised)
            SetEffectValue(int32(raised->GetMaxHealth()));
    }

    void SpendCharge()
    {
        if (Unit* caster = GetCaster())
            if (IsSecondComingResurrection(caster, GetSpellInfo()))
                caster->RemoveAurasDueToSpell(SPELL_SECOND_COMING_READY);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_vhr_second_coming::FullHealth, EFFECT_0, SPELL_EFFECT_RESURRECT_NEW);
        AfterCast += SpellCastFn(spell_vhr_second_coming::SpendCharge);
    }
};

// Boon of Serenity: once per wave, Tranquility heals everything its whole
// channel would have, at once, to every party member in its radius - then the
// channel ends. The charge for it is SPELL_SERENITY_READY, which also made the
// cast free (its -100% cost spellmod, read when the cast began); this spends
// it. Without the charge Tranquility is the ordinary channel.
class spell_vhr_serenity : public SpellScript
{
    PrepareSpellScript(spell_vhr_serenity);

    void AllAtOnce()
    {
        Unit* caster = GetCaster();
        Aura* aura = GetHitAura();
        // The area aura's owner is the druid; party members get theirs from it.
        if (!caster || GetHitUnit() != caster || !aura || !caster->HasAura(SPELL_SERENITY)
            || !caster->HasAura(SPELL_SERENITY_READY))
            return;
        caster->RemoveAurasDueToSpell(SPELL_SERENITY_READY);

        AuraEffect const* heal = nullptr;
        for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
            if (AuraEffect const* eff = aura->GetEffect(i))
                if (eff->GetAuraType() == SPELL_AURA_PERIODIC_HEAL)
                    heal = eff;
        if (!heal)
            return;

        int64 const total = int64(std::max(heal->GetAmount(), 0)) * heal->GetTotalTicks();
        float const radius = heal->GetSpellEffectInfo().CalcRadius(caster);
        if (total <= 0)
            return;

        std::vector<Unit*> healed;
        healed.push_back(caster);
        if (Player* druid = caster->ToPlayer())
            if (Group* group = druid->GetGroup())
                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                    if (Player* member = ref->GetSource())
                        if (member != druid && member->IsInMap(druid) && member->IsAlive() && member->IsWithinDistInMap(druid, radius))
                            healed.push_back(member);

        CastSpellExtraArgs args(TRIGGERED_FULL_MASK);
        args.AddSpellBP0(int32(std::min<int64>(total, std::numeric_limits<int32>::max())));
        for (Unit* unit : healed)
            caster->CastSpell(unit, SPELL_SERENITY_HEAL, args);

        // No aura on the druid = no channel target left; the channel finishes
        // on its next update instead of ticking.
        aura->Remove();
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_vhr_serenity::AllAtOnce);
    }
};

// Boon of the Primal Echo: the core summons this creature with the display and
// form being left. It is a creature rather than a Player, so it cannot run the
// Playerbot brain directly; give each copied form the small druid rotation that
// brain would have selected instead of reducing every physical form to an auto
// attack and caster form to an invisible synthetic heal. A newer shift dismisses
// the previous echo in LeavePrimalEcho.
class npc_violet_hold_primal_echo : public CreatureScript
{
public:
    npc_violet_hold_primal_echo() : CreatureScript("npc_violet_hold_primal_echo") { }

    struct npc_violet_hold_primal_echoAI : public ScriptedAI
    {
        npc_violet_hold_primal_echoAI(Creature* creature) : ScriptedAI(creature) { }

        void Reset() override
        {
            _form = FORM_NONE;
            _ready = false;
            _pulseMs = 250;
            _sequence = 0;
            me->SetReactState(REACT_AGGRESSIVE);
        }

        void SetData(uint32 type, uint32 value) override
        {
            if (type == DATA_PRIMAL_ECHO_FORM)
            {
                _form = uint8(value);
                _ready = true;
                _pulseMs = 0;
                Act();
            }
        }

        Player* Owner() const
        {
            if (!me->IsSummon())
                return nullptr;
            Unit* summoner = me->ToTempSummon()->GetSummonerUnit();
            return summoner ? summoner->ToPlayer() : nullptr;
        }

        Unit* Enemy(Player* owner)
        {
            if (Unit* victim = owner->GetVictim())
                if (victim->IsAlive() && me->IsValidAttackTarget(victim))
                    return victim;
            return me->SelectNearestTarget(40.0f);
        }

        Player* LowestInjuredAlly(Player* owner, uint32 healthPctCeiling)
        {
            Battleground* bg = owner->GetBattleground();
            if (!bg || bg->GetTypeID() != BATTLEGROUND_VHR)
                return owner->GetHealthPct() < healthPctCeiling ? owner : nullptr;

            Player* lowest = nullptr;
            auto consider = [this, &lowest, healthPctCeiling](Player* candidate)
            {
                if (!candidate || !candidate->IsAlive() || !candidate->IsInWorld() || candidate->GetMap() != me->GetMap()
                    || !candidate->IsWithinDistInMap(me, 40.0f) || candidate->GetHealthPct() >= healthPctCeiling)
                    return;
                if (!lowest || uint64(candidate->GetHealth()) * lowest->GetMaxHealth()
                    < uint64(lowest->GetHealth()) * candidate->GetMaxHealth())
                    lowest = candidate;
            };

            // Echoes fight beside the whole team, including Fellowship and
            // playerbot companions. Always consider the owner explicitly: a
            // just-seated player can briefly precede their BG roster entry.
            consider(owner);
            for (auto const& [guid, bgPlayer] : bg->GetPlayers())
            {
                if (bgPlayer.Team != owner->GetBGTeam())
                    continue;
                consider(ObjectAccessor::GetPlayer(*owner, guid));
            }
            return lowest;
        }

        uint32 Rank(uint32 firstRank, Player const* owner) const
        {
            return VioletHoldBoons::ResolveRank(firstRank, owner->GetLevel());
        }

        bool Mend(Player* owner, bool tree)
        {
            // Caster form triages meaningful damage and otherwise helps kill;
            // Tree form is a dedicated healer and tops off any injured ally.
            Player* target = LowestInjuredAlly(owner, tree ? 100u : 80u);
            if (!target)
                return false;

            uint32 spell = 0;
            if (tree && !target->GetAura(Rank(774, owner), me->GetGUID())) // Rejuvenation
                spell = Rank(774, owner);
            else if ((_sequence++ & 1u) == 0)
                spell = Rank(8936, owner); // Regrowth
            else
                spell = Rank(5185, owner); // Healing Touch

            me->GetMotionMaster()->MoveIdle();
            me->SetFacingToObject(target);
            me->CastSpell(target, spell, false);
            _pulseMs = 500;
            return true;
        }

        void CastAtEnemy(Player* owner, Unit* target, bool moonkin)
        {
            AttackStartCaster(target, 30.0f);
            uint32 const moonfire = Rank(8921, owner);
            uint32 spell = 0;
            if (!target->GetAura(moonfire, me->GetGUID()))
                spell = moonfire;
            else if (moonkin && (_sequence++ & 1u))
                spell = Rank(2912, owner); // Starfire
            else
                spell = Rank(5176, owner); // Wrath
            me->CastSpell(target, spell, false);
            _pulseMs = 500;
        }

        void FightInCat(Player* owner, Unit* target)
        {
            AttackStart(target);
            if (!me->IsWithinMeleeRange(target))
            {
                _pulseMs = 250;
                return;
            }
            uint32 const rake = Rank(1822, owner);
            uint32 const spell = !target->GetAura(rake, me->GetGUID()) ? rake : Rank(1082, owner); // Claw
            me->CastSpell(target, spell, TRIGGERED_FULL_MASK);
            DoMeleeAttackIfReady();
            _pulseMs = 1500;
        }

        void FightInBear(Player* owner, Unit* target)
        {
            AttackStart(target);
            if (!me->IsWithinMeleeRange(target))
            {
                _pulseMs = 250;
                return;
            }
            uint32 spell = 0;
            switch (_sequence++ % 3)
            {
                case 0: spell = Rank(6795, owner); break; // Growl: be the tank
                case 1: spell = Rank(779, owner);  break; // Swipe
                default: spell = Rank(6807, owner); break; // Maul
            }
            me->CastSpell(target, spell, TRIGGERED_FULL_MASK);
            DoMeleeAttackIfReady();
            _pulseMs = 1500;
        }

        void Act()
        {
            Player* owner = Owner();
            if (!_ready || !owner || !owner->IsAlive() || !owner->IsInWorld())
                return;

            if (_form == FORM_TREE)
            {
                if (!Mend(owner, true))
                    _pulseMs = 500;
                return;
            }

            if (_form == FORM_NONE && Mend(owner, false))
                return;

            Unit* target = Enemy(owner);
            if (!target)
            {
                _pulseMs = 250;
                return;
            }

            // Acquire the druid's target as part of summon setup instead of
            // waiting for the creature's next aggro scan/fade-in cycle.
            me->EngageWithTarget(target);
            switch (_form)
            {
                case FORM_NONE:     CastAtEnemy(owner, target, false); return;
                case FORM_MOONKIN:  CastAtEnemy(owner, target, true);  return;
                case FORM_CAT:      FightInCat(owner, target);         return;
                case FORM_BEAR:
                case FORM_DIREBEAR: FightInBear(owner, target);        return;
                default: break;
            }

            AttackStart(target);
            DoMeleeAttackIfReady();
            _pulseMs = 250;
        }

        void UpdateAI(uint32 diff) override
        {
            Player* owner = Owner();
            if (!owner || !owner->IsAlive() || !owner->IsInWorld())
            {
                me->DespawnOrUnsummon();
                return;
            }

            if (!_ready || me->HasUnitState(UNIT_STATE_CASTING))
                return;

            if (_pulseMs > diff)
            {
                _pulseMs -= diff;
                if (_form == FORM_CAT || _form == FORM_BEAR || _form == FORM_DIREBEAR)
                    DoMeleeAttackIfReady();
                return;
            }

            Act();
        }

    private:
        uint8 _form = FORM_NONE;
        bool _ready = false;
        uint32 _pulseMs = 500;
        uint32 _sequence = 0;
    };

    CreatureAI* GetAI(Creature* creature) const override
    {
        return new npc_violet_hold_primal_echoAI(creature);
    }
};

void AddSC_violet_hold_class_boons()
{
    RegisterSpellScript(spell_vhr_juggernaut);
    RegisterSpellScript(spell_vhr_sinister_strike);
    RegisterSpellScript(spell_vhr_shattered_ward);
    RegisterSpellScript(spell_vhr_agony);
    RegisterSpellScript(spell_vhr_frostwalker);
    RegisterSpellScript(spell_vhr_second_coming);
    RegisterSpellScript(spell_vhr_serenity);
    new npc_violet_hold_primal_echo();
}
