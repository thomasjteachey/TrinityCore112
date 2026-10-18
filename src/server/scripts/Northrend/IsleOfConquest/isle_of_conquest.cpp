/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
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
#include "BattlegroundIC.h"
#include "GameObject.h"
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "PassiveAI.h"
#include "Player.h"
#include "ScriptedCreature.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellScript.h"
#include "Vehicle.h"
#include <unordered_map>

// TO-DO: This should be done with SmartAI, but yet it does not correctly support vehicles's AIs.
//        Even adding ReactState Passive we still have issues using SmartAI.

struct npc_four_car_garage : public NullCreatureAI
{
    npc_four_car_garage(Creature* creature) : NullCreatureAI(creature) { }

    void PassengerBoarded(Unit* who, int8 /*seatId*/, bool apply) override
    {
        if (apply)
        {
            uint32 spellId = 0;

            switch (me->GetEntry())
            {
                case NPC_DEMOLISHER:
                    spellId = SPELL_DRIVING_CREDIT_DEMOLISHER;
                    break;
                case NPC_GLAIVE_THROWER_A:
                case NPC_GLAIVE_THROWER_H:
                    spellId = SPELL_DRIVING_CREDIT_GLAIVE;
                    break;
                case NPC_SIEGE_ENGINE_H:
                case NPC_SIEGE_ENGINE_A:
                    spellId = SPELL_DRIVING_CREDIT_SIEGE;
                    break;
                case NPC_CATAPULT:
                    spellId = SPELL_DRIVING_CREDIT_CATAPULT;
                    break;
                default:
                    return;
            }

            me->CastSpell(who, spellId, true);
        }
    }
};

enum Events
{
    EVENT_TALK  = 1,
    EVENT_DESPAWN
};

enum Texts
{
    SAY_ONBOARD = 0
};

struct npc_ioc_gunship_captain : public ScriptedAI
{
    npc_ioc_gunship_captain(Creature* creature) : ScriptedAI(creature) { }

    void DoAction(int32 action) override
    {
        if (action == ACTION_GUNSHIP_READY)
        {
            DoCast(me, SPELL_SIMPLE_TELEPORT);
            _events.ScheduleEvent(EVENT_TALK, 3s);
        }
    }

    void UpdateAI(uint32 diff) override
    {
        _events.Update(diff);
        while (uint32 eventId = _events.ExecuteEvent())
        {
            switch (eventId)
            {
                case EVENT_TALK:
                    _events.ScheduleEvent(EVENT_DESPAWN, 1s);
                    Talk(SAY_ONBOARD);
                    DoCast(me, SPELL_TELEPORT_VISUAL_ONLY);
                    break;
                case EVENT_DESPAWN:
                    if (BattlegroundMap* iocMap = me->GetMap()->ToBattlegroundMap())
                        if (Battleground* bgIoC = iocMap->GetBG())
                            bgIoC->DelCreature(BG_IC_NPC_GUNSHIP_CAPTAIN_1);
                    break;
                default:
                    break;
            }
        }
    }

private:
    EventMap _events;
};

// 66630 - Alliance Gunship Portal
// 66637 - Horde Gunship Portal
class spell_ioc_gunship_portal : public SpellScript
{
    PrepareSpellScript(spell_ioc_gunship_portal);

    bool Load() override
    {
        return GetCaster()->GetTypeId() == TYPEID_PLAYER;
    }

    void HandleScript(SpellEffIndex /*effIndex*/)
    {
        Player* caster = GetCaster()->ToPlayer();
        /*
         * HACK: GetWorldLocation() returns real position and not transportposition.
         * ServertoClient: SMSG_MOVE_TELEPORT (0x0B39)
         * counter: 45
         * Tranpsort Guid: Full: xxxx Type: MOTransport Low: xxx
         * Transport Position X: 0 Y: 0 Z: 0 O: 0
         * Position: X: 7.305609 Y: -0.095246 Z: 34.51022 O: 0
         */
        caster->TeleportTo(GetHitCreature()->GetWorldLocation(), TELE_TO_NOT_LEAVE_TRANSPORT);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_ioc_gunship_portal::HandleScript, EFFECT_0, SPELL_EFFECT_SCRIPT_EFFECT);
    }
};

// 66656 - Parachute
// An area aura worn by everyone within 99 yd of the ship's world trigger (20212 and
// 20213 carry it in creature_template_addon), so one aura object serves every
// wearer and anything kept per player is keyed by guid. The chute used to wait for
// the client to report 2 s of falling AND for the player to be off the transport.
// A client that still counts itself on the ship never passes the second test, and
// 2 s below a moving ship can already be out of the aura's reach, so players fell
// the whole way with the buff on.
class spell_ioc_parachute_ic : public AuraScript
{
    PrepareAuraScript(spell_ioc_parachute_ic);

    // Longer than any jump on the deck (under a second in the air), short enough
    // that the ship's trigger is still in range.
    static constexpr uint32 OPEN_AFTER_FALL_MS = 1200;

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_PARACHUTE_IC });
    }

    static bool IsFalling(Player const* player)
    {
        return player->HasUnitMovementFlag(MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR);
    }

    // The client's own fall timer, or how long the server has watched the player
    // fall, for a client that sends nothing between the jump and the landing.
    uint32 FallDuration(Player const* player) const
    {
        uint32 watched = 0;
        auto itr = _fallingMs.find(player->GetGUID());
        if (itr != _fallingMs.end())
            watched = itr->second;
        return std::max(player->m_movementInfo.fallTime, watched);
    }

    void HandleTriggerSpell(AuraEffect const* aurEff)
    {
        PreventDefaultAction();
        Player* target = GetTarget()->ToPlayer();
        if (!target)
            return;

        if (!IsFalling(target))
        {
            _fallingMs.erase(target->GetGUID());
            return;
        }

        _fallingMs[target->GetGUID()] += uint32(std::max(aurEff->GetAmplitude(), 0));
        if (FallDuration(target) > OPEN_AFTER_FALL_MS && !target->HasAura(SPELL_PARACHUTE_IC))
        {
            _fallingMs.erase(target->GetGUID());
            target->CastSpell(target, SPELL_PARACHUTE_IC, true);
        }
    }

    // Losing the aura mid-fall means the ship flew on without the player: open now
    // rather than never. Deferred to the player's own update, because this also
    // runs while the trigger itself is leaving the world.
    void HandleRemove(AuraEffect const* /*aurEff*/, AuraEffectHandleModes /*mode*/)
    {
        Player* target = GetTarget()->ToPlayer();
        if (!target)
            return;

        uint32 const fallen = FallDuration(target);
        _fallingMs.erase(target->GetGUID());

        if (GetTargetApplication()->GetRemoveMode() != AURA_REMOVE_BY_DEFAULT)
            return;

        if (!target->IsInWorld() || !target->IsAlive() || !IsFalling(target) || fallen <= OPEN_AFTER_FALL_MS)
            return;

        target->m_Events.AddEventAtOffset([target]()
        {
            if (target->IsInWorld() && target->IsAlive() && IsFalling(target) && !target->HasAura(SPELL_PARACHUTE_IC))
                target->CastSpell(target, SPELL_PARACHUTE_IC, true);
        }, 0ms);
    }

    void Register() override
    {
        OnEffectPeriodic += AuraEffectPeriodicFn(spell_ioc_parachute_ic::HandleTriggerSpell, EFFECT_0, SPELL_AURA_PERIODIC_TRIGGER_SPELL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_ioc_parachute_ic::HandleRemove, EFFECT_0, SPELL_AURA_PERIODIC_TRIGGER_SPELL, AURA_EFFECT_HANDLE_REAL);
    }

    std::unordered_map<ObjectGuid, uint32> _fallingMs;
};

class StartLaunchEvent : public BasicEvent
{
    public:
        StartLaunchEvent(Position const& pos, ObjectGuid::LowType lowGuid) : _pos(pos), _lowGuid(lowGuid)
        {
        }

        bool Execute(uint64 /*time*/, uint32 /*diff*/) override
        {
            Player* player = ObjectAccessor::FindPlayerByLowGUID(_lowGuid);
            if (!player || !player->GetVehicle())
                return true;

            player->AddAura(SPELL_LAUNCH_NO_FALLING_DAMAGE, player); // prevents falling damage
            float speedZ = 10.0f;
            float dist = player->GetExactDist2d(&_pos);

            player->ExitVehicle();
            player->GetMotionMaster()->MoveJump(_pos, dist, speedZ, EVENT_JUMP, true);
            return true;
        }

    private:
        Position _pos;
        ObjectGuid::LowType _lowGuid;
};

// 66218 - Launch
class spell_ioc_launch : public SpellScript
{
    PrepareSpellScript(spell_ioc_launch);

    void Launch()
    {
        if (!GetCaster()->ToCreature() || !GetExplTargetDest())
            return;

        GetCaster()->ToCreature()->m_Events.AddEvent(new StartLaunchEvent(*GetExplTargetDest(), ASSERT_NOTNULL(GetHitPlayer())->GetGUID().GetCounter()), GetCaster()->ToCreature()->m_Events.CalculateTime(2500ms));
    }

    void Register() override
    {
        AfterHit += SpellHitFn(spell_ioc_launch::Launch);
    }
};

enum SeaforiumBombSpells
{
    SPELL_SEAFORIUM_BLAST       = 66676,
    SPELL_HUGE_SEAFORIUM_BLAST  = 66672,
    SPELL_A_BOMB_INABLE_CREDIT  = 68366,
    SPELL_A_BOMB_INATION_CREDIT = 68367
};

// 66672 - Huge Seaforium Blast
// 66676 - Seaforium Blast
class spell_ioc_seaforium_blast_credit : public SpellScript
{
    PrepareSpellScript(spell_ioc_seaforium_blast_credit);

    bool Validate(SpellInfo const* /*spellInfo*/) override
    {
        return ValidateSpellInfo({ SPELL_A_BOMB_INABLE_CREDIT, SPELL_A_BOMB_INATION_CREDIT });
    }

    void HandleAchievementCredit(SpellEffIndex /*effIndex*/)
    {
        uint32 _creditSpell = 0;
        Unit* caster = GetOriginalCaster();
        if (!caster)
            return;

        if (GetSpellInfo()->Id == SPELL_SEAFORIUM_BLAST)
            _creditSpell = SPELL_A_BOMB_INABLE_CREDIT;
        else if (GetSpellInfo()->Id == SPELL_HUGE_SEAFORIUM_BLAST)
            _creditSpell = SPELL_A_BOMB_INATION_CREDIT;

        if (GetHitGObj() && GetHitGObj()->IsDestructibleBuilding())
            caster->CastSpell(caster, _creditSpell, true);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_ioc_seaforium_blast_credit::HandleAchievementCredit, EFFECT_1, SPELL_EFFECT_GAMEOBJECT_DAMAGE);
    }
};

void AddSC_isle_of_conquest()
{
    RegisterCreatureAI(npc_four_car_garage);
    RegisterCreatureAI(npc_ioc_gunship_captain);
    RegisterSpellScript(spell_ioc_gunship_portal);
    RegisterSpellScript(spell_ioc_parachute_ic);
    RegisterSpellScript(spell_ioc_launch);
    RegisterSpellScript(spell_ioc_seaforium_blast_credit);
}
