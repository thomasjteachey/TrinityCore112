/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * Vanilla (40-player) Heigan the Unclean. Ported from mod-individual-progression
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
#include "GameObject.h"
#include "GameObjectAI.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "ScriptedCreature.h"
#include "VanillaRaids/VanillaRaidsAI.h"
#include "naxxramas40.h"
#include <algorithm>
#include <iterator>
#include <list>
#include <unordered_map>
#include <vector>

enum HeiganSays40
{
    SAY_AGGRO                       = 0,
    SAY_SLAY                        = 1,
    SAY_TAUNT                       = 2,
    EMOTE_DEATH                     = 3,
    EMOTE_DANCE                     = 4,
    EMOTE_DANCE_END                 = 5,
    SAY_DANCE                       = 6
};

enum HeiganSpells40
{
    SPELL_DECREPIT_FEVER            = 29998,
    SPELL_PLAGUE_CLOUD              = 29350,
    SPELL_TELEPORT_SELF             = 30211,
    SPELL_TELEPORT_PLAYERS          = 29273, // target updated in the database
    SPELL_SUMMON_PLAYER             = 25104
};

enum HeiganPhases40
{
    PHASE_SLOW_DANCE                = 0,
    PHASE_FAST_DANCE                = 1
};

// The eruption tiles are ordinary map-533 gameobjects shared by all three
// difficulties, so the vanilla wing drives them exactly the way TrinityCore's
// level 80 Heigan does - by spawn id, in four sections running away from the
// entrance.
static constexpr uint32 firstEruptionDBGUID40 = 84980;
static constexpr uint8 numSections40 = 4;
static constexpr uint8 numEruptions40[numSections40] = { 15, 25, 23, 13 };

struct boss_heigan_40 : public VanillaRaidsBossAI
{
    boss_heigan_40(Creature* creature) : VanillaRaidsBossAI(creature, BOSS_HEIGAN) { }

    void Reset() override
    {
        BossAI::Reset();
        _currentPhase = PHASE_SLOW_DANCE;
        _safeSection = 3;
        _moveRight = true;
        _eruptionScheduler.CancelAll();
        _portedPlayersThisPhase.clear();
        KillPlayersInTheTunnel();
    }

    void KilledUnit(Unit* who) override
    {
        if (!who->IsPlayer())
            return;

        Talk(SAY_SLAY);
    }

    void JustDied(Unit* killer) override
    {
        _eruptionScheduler.CancelAll();
        BossAI::JustDied(killer);
        Talk(EMOTE_DEATH);
    }

    void JustEngagedWith(Unit* who) override
    {
        BossAI::JustEngagedWith(who);
        DoZoneInCombat();
        Talk(SAY_AGGRO);
        CacheEruptionTiles();
        StartFightPhase(PHASE_SLOW_DANCE);
    }

    // Resolves the eruption tile gameobjects for each section once per pull.
    void CacheEruptionTiles()
    {
        std::unordered_multimap<uint32, GameObject*> const& mapGOs = me->GetMap()->GetGameObjectBySpawnIdStore();
        uint32 spawnId = firstEruptionDBGUID40;
        for (uint8 section = 0; section < numSections40; ++section)
        {
            _eruptTiles[section].clear();
            for (uint8 i = 0; i < numEruptions40[section]; ++i)
            {
                auto tileIt = mapGOs.equal_range(spawnId++);
                for (auto it = tileIt.first; it != tileIt.second; ++it)
                    _eruptTiles[section].push_back(it->second->GetGUID());
            }
        }
    }

    void EruptSection()
    {
        TeleportCheaters();

        for (uint8 section = 0; section < numSections40; ++section)
        {
            if (section == _safeSection)
                continue;

            for (ObjectGuid const& tileGUID : _eruptTiles[section])
            {
                if (GameObject* tile = ObjectAccessor::GetGameObject(*me, tileGUID))
                {
                    tile->SendCustomAnim(0);
                    CastSpellExtraArgs args;
                    args.OriginalCaster = me->GetGUID();
                    tile->CastSpell(tile, tile->GetGOInfo()->trap.spellId, args);
                }
            }
        }

        if (_safeSection == numSections40 - 1)
            _moveRight = false;
        else if (_safeSection == 0)
            _moveRight = true;

        _moveRight ? ++_safeSection : --_safeSection;
    }

    void StartFightPhase(uint8 phase)
    {
        _safeSection = 3;
        _currentPhase = phase;
        scheduler.CancelAll();
        _eruptionScheduler.CancelAll();

        if (phase == PHASE_SLOW_DANCE)
        {
            me->CastStop();
            me->SetReactState(REACT_AGGRESSIVE);
            DoZoneInCombat();

            ScheduleTimedEvent(12s, 15s, [this]
            {
                // 25 yard radius - the vanilla Mana Burn stand-in for Spell Disruption.
                me->CastSpell(me, SPELL_DISRUPTION_40,
                    CastSpellExtraArgs(TRIGGERED_NONE).AddSpellMod(SPELLVALUE_RADIUS_MOD, 2500));
            }, 10s);

            ScheduleTimedEvent(17s, [this]
            {
                me->CastSpell(me, SPELL_DECREPIT_FEVER,
                    CastSpellExtraArgs(TRIGGERED_NONE).AddSpellBP1(499));
            }, 22s, 25s);

            // The teleport kills whoever it moves, so upstream fires it once and
            // parks the repeat far out of reach rather than cancelling it.
            ScheduleTimedEvent(40s, [this] { DoEventTeleportPlayer(); }, 600s);

            _eruptionScheduler.Schedule(15s, [this](TaskContext context)
            {
                EruptSection();
                Talk(SAY_TAUNT);
                context.Repeat(10s);
            }).Schedule(90s, [this](TaskContext /*context*/)
            {
                StartFightPhase(PHASE_FAST_DANCE);
            });

            _portedPlayersThisPhase.clear();
        }
        else // PHASE_FAST_DANCE
        {
            Talk(EMOTE_DANCE);
            Talk(SAY_DANCE);
            me->AttackStop();
            me->StopMoving();
            me->SetReactState(REACT_PASSIVE);
            DoCastSelf(SPELL_TELEPORT_SELF);
            me->SetFacingTo(2.40f);

            scheduler.Schedule(1s, [this](TaskContext /*context*/)
            {
                DoCastSelf(SPELL_PLAGUE_CLOUD);
            });

            _eruptionScheduler.Schedule(7s, [this](TaskContext context)
            {
                EruptSection();
                context.Repeat(4s);
            }).Schedule(45s, [this](TaskContext /*context*/)
            {
                StartFightPhase(PHASE_SLOW_DANCE);
                Talk(EMOTE_DANCE_END); // not on the pull, only when a dance ends
            });
        }
    }

    bool IsInRoom(Unit* who)
    {
        if (who->GetPositionX() > 2826.0f || who->GetPositionX() < 2723.0f
            || who->GetPositionY() > -3641.0f || who->GetPositionY() < -3736.0f)
        {
            if (who->GetGUID() == me->GetGUID())
                EnterEvadeMode(EVADE_REASON_BOUNDARY);

            return false;
        }

        return true;
    }

    void KillPlayersInTheTunnel()
    {
        Map::PlayerList const& players = me->GetMap()->GetPlayers();
        for (auto const& itr : players)
            if (Player* player = itr.GetSource())
                if (player->IsAlive() && !player->IsGameMaster())
                    if (player->GetPositionY() <= -3735.0f)
                        player->KillSelf();
    }

    void DoEventTeleportPlayer()
    {
        std::list<Unit*> candidates;
        SelectTargetList(candidates, 3, SelectTargetMethod::Random, 0, [this](Unit* target)
        {
            if (!target->IsPlayer())            // never pets, guardians and the like
                return false;
            if (!target->IsAlive())
                return false;
            if (me->GetVictim() == target)      // never the tank
                return false;
            // skip anyone already moved this phase
            return std::find(_portedPlayersThisPhase.begin(), _portedPlayersThisPhase.end(),
                target->GetGUID()) == _portedPlayersThisPhase.end();
        });

        if (candidates.empty())
            return;

        for (uint8 i = 0; i < 3; ++i)
        {
            if (candidates.empty())
                break;

            auto itr = candidates.begin();
            if (candidates.size() > 1)
                std::advance(itr, urand(0, uint32(candidates.size()) - 1));

            Unit* target = *itr;
            candidates.erase(itr);

            _portedPlayersThisPhase.push_back(target->GetGUID());
            ModifyThreatByPercent(target, -99); // so he does not chase and reset
            target->CastSpell(target, SPELL_TELEPORT_PLAYERS, CastSpellExtraArgs(true));
        }
    }

    void UpdateAI(uint32 diff) override
    {
        if (!IsInRoom(me))
            return;

        if (!UpdateVictim())
            return;

        if (Unit* victim = me->GetVictim())
            if (!me->IsWithinDistInMap(victim, VISIBILITY_DISTANCE_NORMAL))
                me->CastSpell(victim, SPELL_SUMMON_PLAYER, CastSpellExtraArgs(true));

        _eruptionScheduler.Update(diff);
        scheduler.Update(diff);

        DoMeleeAttackIfReady();
    }

private:
    uint8 _currentPhase = PHASE_SLOW_DANCE;
    uint8 _safeSection = 3;
    bool _moveRight = true;
    TaskScheduler _eruptionScheduler;
    GuidVector _portedPlayersThisPhase;
    std::vector<ObjectGuid> _eruptTiles[numSections40];
};

void AddSC_boss_heigan_40()
{
    RegisterNaxxramasCreatureAI(boss_heigan_40);
}
