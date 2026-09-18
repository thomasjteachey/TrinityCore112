/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * Vanilla (40-player) Maexxna. Ported from mod-individual-progression
 * (ZhengPeiRu21, AzerothCore, AGPL-3.0); Naxxramas 40 scripts by Sogladev.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "ScriptMgr.h"
#include "Containers.h"
#include "ObjectAccessor.h"
#include "PassiveAI.h"
#include "Player.h"
#include "ScriptedCreature.h"
#include "SpellAuraEffects.h"
#include "SpellScript.h"
#include "VanillaRaids/VanillaRaids.h"
#include "VanillaRaids/VanillaRaidsAI.h"
#include "naxxramas40.h"
#include <cmath>
#include <list>
#include <vector>

enum MaexxnaSpells40
{
    SPELL_WEB_SPRAY_40                  = 29484,
    SPELL_POISON_SHOCK_40               = 28732,
    SPELL_NECROTIC_POISON_40            = 54121,
    SPELL_FRENZY_40                     = 54123,
    SPELL_WEB_WRAP_STUN_40              = 28622,
    SPELL_WEB_WRAP_SUMMON_WOTLK         = 28627,
    SPELL_WEB_WRAP_KILL_WEBS_40         = 52512,
    SPELL_WEB_WRAP_PACIFY_5_40          = 28618 // 5 second pacify + silence
};

enum MaexxnaEvents40
{
    EVENT_WEB_SPRAY                     = 1,
    EVENT_POISON_SHOCK                  = 2,
    EVENT_NECROTIC_POISON               = 3,
    EVENT_WEB_WRAP                      = 4,
    EVENT_HEALTH_CHECK                  = 5,
    EVENT_SUMMON_SPIDERLINGS            = 6,
    EVENT_WEB_WRAP_APPLY_STUN           = 7
};

enum MaexxnaEmotes40
{
    EMOTE_SPIDERS                       = 0,
    EMOTE_WEB_WRAP                      = 1,
    EMOTE_WEB_SPRAY                     = 2
};

Position const PosWrap40[7] =
{
    { 3496.615f, -3834.182f, 320.7863f },
    { 3509.108f, -3833.922f, 320.4750f },
    { 3523.644f, -3838.309f, 320.5775f },
    { 3538.152f, -3846.353f, 320.5188f },
    { 3546.219f, -3856.167f, 320.9324f },
    { 3555.135f, -3869.507f, 320.8307f },
    { 3560.282f, -3886.143f, 321.2827f }
};

struct WebTargetSelector40
{
    WebTargetSelector40(Unit* maexxna) : _maexxna(maexxna) { }

    bool operator()(Unit const* target) const
    {
        if (!target->IsPlayer())            // never web pets, guardians and the like
            return false;
        if (_maexxna->GetVictim() == target) // never the tank
            return false;
        if (target->HasAura(SPELL_WEB_WRAP_STUN_40)) // never someone already webbed
            return false;
        return true;
    }

private:
    Unit const* _maexxna;
};

struct boss_maexxna_40 : public VanillaRaidsBossAI
{
    boss_maexxna_40(Creature* creature) : VanillaRaidsBossAI(creature, BOSS_MAEXXNA) { }

    bool IsInRoom()
    {
        if (me->GetExactDist(3486.6f, -3890.6f, 291.8f) > 100.0f)
        {
            EnterEvadeMode(EVADE_REASON_BOUNDARY);
            return false;
        }

        return true;
    }

    void Reset() override
    {
        BossAI::Reset();
        events.Reset();
        summons.DespawnAll();
        _wraps.clear();
    }

    void JustEngagedWith(Unit* who) override
    {
        BossAI::JustEngagedWith(who);
        DoZoneInCombat();
        events.ScheduleEvent(EVENT_WEB_WRAP, 20s);
        events.ScheduleEvent(EVENT_WEB_SPRAY, 40s);
        events.ScheduleEvent(EVENT_POISON_SHOCK, 10s);
        events.ScheduleEvent(EVENT_NECROTIC_POISON, 5s);
        events.ScheduleEvent(EVENT_HEALTH_CHECK, 1s);
        events.ScheduleEvent(EVENT_SUMMON_SPIDERLINGS, 30s);
    }

    void JustSummoned(Creature* summon) override
    {
        if (summon->GetEntry() == NPC_MAEXXNA_SPIDERLING_40)
        {
            DoZoneInCombat(summon);
            if (Unit* target = SelectTarget(SelectTargetMethod::Random, 0))
                summon->AI()->AttackStart(target);
        }

        summons.Summon(summon);
    }

    void DoCastWebWrap()
    {
        uint32 const wrapCount = RAID_MODE<uint32>(1, 2, 2, 2);

        std::list<Unit*> candidates;
        SelectTargetList(candidates, wrapCount, SelectTargetMethod::Random, 0, WebTargetSelector40(me));
        if (candidates.empty())
            return;

        std::vector<uint32> positions { 0, 1, 2, 3, 4, 5, 6 };
        Trinity::Containers::RandomShuffle(positions);

        for (uint32 i = 0; i < wrapCount; ++i)
        {
            if (candidates.empty())
                break;

            Position const& randomPos = PosWrap40[positions[i]];

            auto itr = candidates.begin();
            if (candidates.size() > 1)
                std::advance(itr, urand(0, uint32(candidates.size()) - 1));

            Unit* target = *itr;
            candidates.erase(itr);

            float const dx = randomPos.GetPositionX() - target->GetPositionX();
            float const dy = randomPos.GetPositionY() - target->GetPositionY();
            float const distXY = std::sqrt(dx * dx + dy * dy);

            // Smooth knockback arc that clears the webbing without hitting the ceiling.
            float const horizontalSpeed = distXY / 1.5f;
            float verticalSpeed = 28.0f;
            if (distXY <= 10.0f)
                verticalSpeed = 12.0f;
            else if (distXY <= 20.0f)
                verticalSpeed = 16.0f;
            else if (distXY <= 30.0f)
                verticalSpeed = 20.0f;
            else if (distXY <= 40.0f)
                verticalSpeed = 24.0f;

            target->KnockbackFrom(randomPos.GetPositionX(), randomPos.GetPositionY(), -horizontalSpeed, verticalSpeed);
            me->CastSpell(target, SPELL_WEB_WRAP_PACIFY_5_40, CastSpellExtraArgs(true));

            _wraps.push_back(target->GetGUID());
        }

        events.ScheduleEvent(EVENT_WEB_WRAP_APPLY_STUN, 2s);
    }

    void UpdateAI(uint32 diff) override
    {
        if (!IsInRoom())
            return;

        if (!UpdateVictim())
            return;

        events.Update(diff);
        if (me->HasUnitState(UNIT_STATE_CASTING))
            return;

        switch (events.ExecuteEvent())
        {
            case EVENT_WEB_SPRAY:
                Talk(EMOTE_WEB_SPRAY);
                DoCastSelf(SPELL_WEB_SPRAY_40, CastSpellExtraArgs(true));
                events.Repeat(40s);
                break;
            case EVENT_POISON_SHOCK:
                DoCastVictim(SPELL_POISON_SHOCK_40);
                events.Repeat(10s);
                break;
            case EVENT_NECROTIC_POISON:
                DoCastVictim(SPELL_NECROTIC_POISON_40);
                events.Repeat(30s);
                break;
            case EVENT_SUMMON_SPIDERLINGS:
                Talk(EMOTE_SPIDERS);
                for (uint8 i = 0; i < 8; ++i)
                    me->SummonCreature(NPC_MAEXXNA_SPIDERLING_40, me->GetPositionX(), me->GetPositionY(),
                        me->GetPositionZ(), me->GetOrientation());
                events.Repeat(40s);
                break;
            case EVENT_HEALTH_CHECK:
                if (me->GetHealthPct() < 30)
                {
                    DoCastSelf(SPELL_FRENZY_40, CastSpellExtraArgs(true));
                    break;
                }
                events.Repeat(1s);
                break;
            case EVENT_WEB_WRAP:
                Talk(EMOTE_WEB_WRAP);
                DoCastWebWrap();
                events.Repeat(40s);
                break;
            case EVENT_WEB_WRAP_APPLY_STUN:
                for (ObjectGuid const& guid : _wraps)
                    if (Player* player = ObjectAccessor::GetPlayer(*me, guid))
                        player->CastSpell(player, SPELL_WEB_WRAP_STUN_40, CastSpellExtraArgs(true));
                _wraps.clear();
                break;
            default:
                break;
        }

        DoMeleeAttackIfReady();
    }

private:
    GuidVector _wraps;
};

struct boss_maexxna_webwrap_40 : public NullCreatureAI
{
    boss_maexxna_webwrap_40(Creature* creature) : NullCreatureAI(creature) { }

    void IsSummonedBy(WorldObject* summoner) override
    {
        if (!summoner)
            return;

        _victimGUID = summoner->GetGUID();
    }

    void JustDied(Unit* /*killer*/) override
    {
        if (!_victimGUID)
            return;

        if (Unit* victim = ObjectAccessor::GetUnit(*me, _victimGUID))
        {
            if (victim->IsAlive())
            {
                victim->RemoveAurasDueToSpell(SPELL_WEB_WRAP_STUN_40);
                victim->RemoveAurasDueToSpell(SPELL_WEB_WRAP_SUMMON_40);
            }
        }
    }

    void UpdateAI(uint32 /*diff*/) override
    {
        if (!_victimGUID)
            return;

        if (Unit* victim = ObjectAccessor::GetUnit(*me, _victimGUID))
            if (!victim->IsAlive())
                DoCastSelf(SPELL_WEB_WRAP_KILL_WEBS_40, CastSpellExtraArgs(true));
    }

private:
    ObjectGuid _victimGUID;
};

// 28622 - Web Wrap.
//
// TrinityCore's 10- and 25-player Maexxna drives the wrap through npc_webwrap
// and needs nothing from this script, so it does its work only on the
// 40-player difficulty and returns untouched otherwise.
class spell_web_wrap_damage_40 : public AuraScript
{
    PrepareAuraScript(spell_web_wrap_damage_40);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_WEB_WRAP_SUMMON_40 });
    }

    void OnPeriodic(AuraEffect const* aurEff)
    {
        Unit* caster = GetCaster();
        if (!caster || !VanillaRaids::IsNaxx40(caster))
            return;

        AuraEffect* effect = const_cast<AuraEffect*>(aurEff);
        effect->SetAmount(int32(urand(657, 843)));

        if (aurEff->GetTickNumber() == 2)
            GetTarget()->CastSpell(GetTarget(), SPELL_WEB_WRAP_SUMMON_40, CastSpellExtraArgs(true));
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_web_wrap_damage_40::OnPeriodic, EFFECT_1, SPELL_AURA_PERIODIC_DAMAGE);
    }
};

void AddSC_boss_maexxna_40()
{
    RegisterNaxxramasCreatureAI(boss_maexxna_40);
    RegisterNaxxramasCreatureAI(boss_maexxna_webwrap_40);
    RegisterSpellScript(spell_web_wrap_damage_40);
}
