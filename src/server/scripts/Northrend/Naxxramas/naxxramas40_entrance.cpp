/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * Getting into (and out of) the vanilla 40-player Naxxramas: the teleporter at
 * the Plaguewood gate outside Stratholme, the area triggers that keep the
 * Northrend portals on the level 80 versions, and the trash that only exists in
 * the 40-player wing.
 *
 * Ported from mod-individual-progression (ZhengPeiRu21, AzerothCore, AGPL-3.0);
 * Naxxramas 40 scripts by Sogladev.
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
#include "Chat.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GameObjectAI.h"
#include "Group.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptedCreature.h"
#include "ThreatManager.h"
#include "VanillaRaids/VanillaRaids.h"
#include "naxxramas40.h"
#include <algorithm>
#include <limits>

namespace
{
    // Just inside the door, on the 40-player side.
    Position const Naxx40EntrancePos = { 3005.51f, -3434.64f, 304.195f, 6.2831f };

    // Whether this character may be taken into the 40-player wing at all.
    // reason is filled in with something the raid leader can act on.
    bool CanEnterNaxx40(Player* player, std::string& reason)
    {
        if (!player)
            return false;

        if (player->IsGameMaster() || VanillaRaids::IsBotAccount(player))
            return true;

        if (player->GetLevel() > VanillaRaids::MaxEntryLevel())
        {
            reason = "is above the level cap for this version of Naxxramas";
            return false;
        }

        if (VanillaRaids::StratholmeEntranceRequired()
            && player->GetQuestStatus(VanillaRaids::QUEST_NAXX40_ENTRANCE_FLAG) != QUEST_STATUS_COMPLETE
            && player->GetQuestStatus(VanillaRaids::QUEST_NAXX40_ENTRANCE_FLAG) != QUEST_STATUS_REWARDED)
        {
            reason = "needs to walk in through Stratholme first";
            return false;
        }

        if (VanillaRaids::AttunementRequired() && !VanillaRaids::IsAttuned(player))
        {
            reason = "is not attuned to Naxxramas";
            return false;
        }

        return true;
    }

    void SendIntoNaxx40(Player* player)
    {
        player->SetRaidDifficulty(VanillaRaids::VanillaRaidDifficulty);
        player->TeleportTo(VanillaRaids::MAP_NAXXRAMAS, Naxx40EntrancePos.GetPositionX(), Naxx40EntrancePos.GetPositionY(),
            Naxx40EntrancePos.GetPositionZ(), Naxx40EntrancePos.GetOrientation());
    }
}

// The teleporter object at the Plaguewood gate. It takes the whole raid, so the
// group's difficulty and everyone's is set before anyone zones.
struct go_naxx40_tele : public GameObjectAI
{
    go_naxx40_tele(GameObject* go) : GameObjectAI(go) { }

    bool OnGossipHello(Player* player) override
    {
        if (!player || !player->IsInWorld())
            return false;

        if (!VanillaRaids::Naxx40Enabled())
            return false;

        ChatHandler handler(player->GetSession());

        std::string reason;
        if (!CanEnterNaxx40(player, reason))
        {
            handler.PSendSysMessage("You %s.", reason.c_str());
            return false;
        }

        if (Group* group = player->GetGroup())
        {
            group->SetRaidDifficulty(VanillaRaids::VanillaRaidDifficulty);

            for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
            {
                Player* member = itr->GetSource();
                if (!member || member->GetGUID() == player->GetGUID())
                    continue;

                std::string memberReason;
                if (!CanEnterNaxx40(member, memberReason))
                {
                    handler.PSendSysMessage("|cff00ffff%s|r %s.", member->GetName().c_str(), memberReason.c_str());
                    continue;
                }

                member->SetRaidDifficulty(VanillaRaids::VanillaRaidDifficulty);

                // Only pull in people who actually came to the gate.
                if (member->GetMapId() != VanillaRaids::MAP_NAXXRAMAS && player->GetDistance(member) <= 30.0f)
                    SendIntoNaxx40(member);
            }
        }

        SendIntoNaxx40(player);
        return true;
    }
};

// The four Northrend portals. They serve the level 80 wings only: anyone who
// arrives with raid difficulty 2 set is put back on 10-player normal first.
class at_naxx40_northrend_entrance : public AreaTriggerScript
{
public:
    at_naxx40_northrend_entrance() : AreaTriggerScript("at_naxx40_northrend_entrance") { }

    bool OnTrigger(Player* player, AreaTriggerEntry const* trigger) override
    {
        Difficulty const difficulty = player->GetGroup() ? player->GetGroup()->GetDifficulty(true) : player->GetDifficulty(true);
        if (difficulty == VanillaRaids::VanillaRaidDifficulty)
            player->SetRaidDifficulty(RAID_DIFFICULTY_10MAN_NORMAL);

        switch (trigger->ID)
        {
            case 5191: player->TeleportTo(533, 3005.68f, -3447.77f, 293.93f, 4.65f); break;
            case 5192: player->TeleportTo(533, 3019.34f, -3434.36f, 293.99f, 6.27f); break;
            case 5193: player->TeleportTo(533, 3005.90f, -3420.58f, 294.11f, 1.58f); break;
            case 5194: player->TeleportTo(533, 2992.50f, -3434.42f, 293.94f, 3.13f); break;
            default: break;
        }

        return true;
    }
};

// The exit portals inside. As in Classic, the 40-player wing has none.
class at_naxx40_exit : public AreaTriggerScript
{
public:
    at_naxx40_exit() : AreaTriggerScript("at_naxx40_exit") { }

    bool OnTrigger(Player* player, AreaTriggerEntry const* trigger) override
    {
        if (VanillaRaids::IsNaxx40Map(player->GetMap()))
            return false;

        switch (trigger->ID)
        {
            case 5196: player->TeleportTo(571, 3679.25f, -1278.58f, 243.55f, 2.39f); break;
            case 5197: player->TeleportTo(571, 3679.03f, -1259.68f, 243.55f, 3.98f); break;
            case 5198: player->TeleportTo(571, 3661.14f, -1279.55f, 243.55f, 0.82f); break;
            case 5199: player->TeleportTo(571, 3660.01f, -1260.99f, 243.55f, 5.51f); break;
            default: break;
        }

        return true;
    }
};

// Marks a character as having come in through Stratholme, which is what the
// optional RequireStratholmeEntrance rule reads.
class naxxramas40_playerscript : public PlayerScript
{
public:
    naxxramas40_playerscript() : PlayerScript("naxxramas40_playerscript") { }

    void OnMapChanged(Player* player) override
    {
        if (!player || player->IsGameMaster())
            return;

        if (!VanillaRaids::IsNaxx40Map(player->GetMap()))
            return;

        if (player->GetQuestStatus(VanillaRaids::QUEST_NAXX40_ENTRANCE_FLAG) == QUEST_STATUS_REWARDED)
            return;

        Quest const* quest = sObjectMgr->GetQuestTemplate(VanillaRaids::QUEST_NAXX40_ENTRANCE_FLAG);
        if (!quest)
            return;

        player->AddQuest(quest, nullptr);
        player->CompleteQuest(VanillaRaids::QUEST_NAXX40_ENTRANCE_FLAG);
        player->RewardQuest(quest, 0, player, false);
    }
};

// An invisible marker at the Plaguewood gate: it opens the door for anyone
// allowed in and sends them through when they walk into it.
struct npc_naxx40_area_trigger : public ScriptedAI
{
    npc_naxx40_area_trigger(Creature* creature) : ScriptedAI(creature)
    {
        me->SetDisplayId(11686); // invisible
    }

    void MoveInLineOfSight(Unit* who) override
    {
        if (!VanillaRaids::Naxx40Enabled())
            return;

        Player* player = who ? who->ToPlayer() : nullptr;
        if (!player)
            return;

        std::string reason;
        if (!CanEnterNaxx40(player, reason))
            return;

        float const distance = me->GetDistance2d(player);
        if (distance < 5.0f)
            SendIntoNaxx40(player);
        else if (distance < 20.0f)
        {
            if (GameObject* door = me->FindNearestGameObject(GO_STRATH_GATE_40, 100.0f))
                door->SetGoState(GO_STATE_ACTIVE);
        }
    }
};

// Heigan's eye stalks, which only the vanilla wing spawns.
struct npc_heigan_eye_stalk_40 : public ScriptedAI
{
    npc_heigan_eye_stalk_40(Creature* creature) : ScriptedAI(creature) { }

    static constexpr uint32 SPELL_MIND_FLAY = 29407;
    static constexpr uint32 SPELL_SUBMERGE = 26234;

    void Reset() override
    {
        me->SetNoCallAssistance(true);
        // Same bit AzerothCore calls UNIT_FLAG_DISABLE_MOVE: they are rooted in place.
        me->SetUnitFlag(UNIT_FLAG_REMOVE_CLIENT_CONTROL);
    }

    void MoveInLineOfSight(Unit* who) override
    {
        if (_timeSinceSpawn < 3000)
            return;

        if (!who || who->GetDistance2d(me) > 19.0f)
            return;

        if (!me->HasReactState(REACT_AGGRESSIVE) || !me->CanStartAttack(who, false))
            return;

        if (!me->IsWithinLOSInMap(who))
            return;

        me->SetNoCallAssistance(true);

        if (!me->GetVictim())
            AttackStart(who);
        else if (me->GetMap()->IsDungeon())
        {
            who->SetInCombatWith(me);
            me->GetThreatManager().AddThreat(who, 0.0f);
        }
    }

    void UpdateAI(uint32 diff) override
    {
        me->SetNoCallAssistance(true);
        _timeSinceSpawn += std::min(diff, std::numeric_limits<uint32>::max() - _timeSinceSpawn);

        if (_haveSubmerged)
        {
            if (!_haveCastSubmerge)
            {
                _haveCastSubmerge = true;
                DoCastSelf(SPELL_SUBMERGE);
            }

            return;
        }

        if (!UpdateVictim())
            return;

        if (!me->IsNonMeleeSpellCast(false))
        {
            if (me->GetDistance(me->GetVictim()) < 35.0f)
                me->CastSpell(me->GetVictim(), SPELL_MIND_FLAY, CastSpellExtraArgs(TRIGGERED_NONE)
                    .AddSpellBP0(750)   // damage
                    .AddSpellBP1(-20)); // movement speed
            else
                DoStopAttack();
        }

        DoMeleeAttackIfReady();
    }

private:
    uint32 _timeSinceSpawn = 0;
    bool _haveSubmerged = false;
    bool _haveCastSubmerge = false;
};

void AddSC_naxxramas40_entrance()
{
    RegisterGameObjectAI(go_naxx40_tele);
    RegisterCreatureAI(npc_naxx40_area_trigger);
    RegisterNaxxramasCreatureAI(npc_heigan_eye_stalk_40);
    new at_naxx40_northrend_entrance();
    new at_naxx40_exit();
    new naxxramas40_playerscript();
}
