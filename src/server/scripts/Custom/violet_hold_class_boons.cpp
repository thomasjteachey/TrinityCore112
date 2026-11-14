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
 *   spell_vhr_frostwalker     1953                                      Blink
 *   spell_vhr_serenity        740, 8918, 9862, 9863                     Tranquility
 */

#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
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
class FrostwalkerLandingEvent : public BasicEvent
{
public:
    FrostwalkerLandingEvent(Player* mage, uint32 novaSpell, uint8 tries) : _mage(mage), _novaSpell(novaSpell), _tries(tries) { }

    bool Execute(uint64 /*time*/, uint32 /*diff*/) override
    {
        if (!_mage->IsInWorld() || !_mage->IsAlive())
            return true;
        if (_mage->IsBeingTeleportedNear() && _tries < 20)
        {
            _mage->m_Events.AddEventAtOffset(new FrostwalkerLandingEvent(_mage, _novaSpell, uint8(_tries + 1)),
                Milliseconds(FROSTWALKER_LANDING_CHECK_MS));
            return true;
        }
        _mage->CastSpell(_mage, _novaSpell, TRIGGERED_FULL_MASK);
        return true;
    }

private:
    Player* _mage;
    uint32 _novaSpell;
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
// and again where they land.
class spell_vhr_frostwalker : public SpellScript
{
    PrepareSpellScript(spell_vhr_frostwalker);

    void Departure()
    {
        Player* mage = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!mage || !mage->HasAura(SPELL_FROSTWALKER))
            return;
        _nova = GetFrostNovaRank(mage);
        if (_nova)
            mage->CastSpell(mage, _nova, TRIGGERED_FULL_MASK);
    }

    void Landing()
    {
        Player* mage = GetCaster() ? GetCaster()->ToPlayer() : nullptr;
        if (!mage || !_nova)
            return;
        mage->m_Events.AddEventAtOffset(new FrostwalkerLandingEvent(mage, _nova, 0), Milliseconds(FROSTWALKER_LANDING_CHECK_MS));
    }

    void Register() override
    {
        BeforeCast += SpellCastFn(spell_vhr_frostwalker::Departure);
        AfterCast += SpellCastFn(spell_vhr_frostwalker::Landing);
    }

    uint32 _nova = 0;
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

void AddSC_violet_hold_class_boons()
{
    RegisterSpellScript(spell_vhr_juggernaut);
    RegisterSpellScript(spell_vhr_sinister_strike);
    RegisterSpellScript(spell_vhr_shattered_ward);
    RegisterSpellScript(spell_vhr_agony);
    RegisterSpellScript(spell_vhr_frostwalker);
    RegisterSpellScript(spell_vhr_serenity);
}
