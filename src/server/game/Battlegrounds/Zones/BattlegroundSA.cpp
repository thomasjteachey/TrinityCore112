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

#include "BattlegroundSA.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "MoveSpline.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptedCreature.h"
#include "UpdateData.h"
#include "Vehicle.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldStatePackets.h"
#include <algorithm>

namespace
{
    // Where a bot comes ashore: put there instead of on a boat it cannot ride
    // by itself, or off the boat it was carried across on.
    Position const SABeachLanding = { 1600.381f, -106.263f, 8.8745f, 3.78f };
}

void BattlegroundSAScore::BuildObjectivesBlock(WorldPacket& data)
{
    data << uint32(2); // Objectives Count
    data << uint32(DemolishersDestroyed);
    data << uint32(GatesDestroyed);
}

BattlegroundSA::BattlegroundSA()
{
    StartMessageIds[BG_STARTING_EVENT_FOURTH] = 0; // handle by Kanrethad

    BgObjects.resize(BG_SA_MAXOBJ);
    BgCreatures.resize(AsUnderlyingType(BG_SA_MAXNPC) + AsUnderlyingType(BG_SA_MAX_GY));
    TimerEnabled = false;
    UpdateWaitTimer = 0;
    SignaledRoundTwo = false;
    SignaledRoundTwoHalfMin = false;
    InitSecondRound = false;
    _gateDestroyed = false;
    Attackers = TEAM_ALLIANCE;
    TotalTime = 0;
    EndRoundTimer = 0;
    ShipsStarted = false;
    Status = BG_SA_NOT_STARTED;
    _boatRiderTimer = 0;

    for (uint8 i = 0; i < MAX_GATES; ++i)
        GateStatus[i] = BG_SA_GATE_OK;

    for (uint8 i = 0; i < 2; i++)
    {
        RoundScores[i].winner = TEAM_ALLIANCE;
        RoundScores[i].time = 0;
        _allVehiclesAlive[i] = true;
    }

    //! This is here to prevent an uninitialised variable warning
    //! The warning only occurs when SetUpBattleGround fails though.
    //! In the future this function should be called BEFORE sending initial worldstates.
    memset(&GraveyardStatus, 0, sizeof(GraveyardStatus));
}

BattlegroundSA::~BattlegroundSA() { }

void BattlegroundSA::Reset()
{
    TotalTime = 0;
    Attackers = ((urand(0, 1)) ? TEAM_ALLIANCE : TEAM_HORDE);
    for (uint8 i = 0; i <= 5; i++)
        GateStatus[i] = BG_SA_GATE_OK;
    ShipsStarted = false;
    _gateDestroyed = false;
    _allVehiclesAlive[TEAM_ALLIANCE] = true;
    _allVehiclesAlive[TEAM_HORDE] = true;
    Status = BG_SA_WARMUP;
}

bool BattlegroundSA::SetupBattleground()
{
    return ResetObjs();
}

bool BattlegroundSA::ResetObjs()
{
    // The boats go: nobody is left standing on one.
    ReleaseBoatRiders(false);

    for (BattlegroundPlayerMap::const_iterator itr = GetPlayers().begin(); itr != GetPlayers().end(); ++itr)
        if (Player* player = ObjectAccessor::FindPlayer(itr->first))
            SendTransportsRemove(player);

    uint32 atF = BG_SA_Factions[Attackers];
    uint32 defF = BG_SA_Factions[Attackers ? TEAM_ALLIANCE : TEAM_HORDE];

    for (uint8 i = 0; i < BG_SA_MAXOBJ; i++)
        DelObject(i);

    for (uint8 i = 0; i < BG_SA_MAXNPC; i++)
        DelCreature(i);

    for (uint8 i = BG_SA_MAXNPC; i < AsUnderlyingType(BG_SA_MAXNPC) + AsUnderlyingType(BG_SA_MAX_GY); i++)
        DelCreature(i);

    for (uint8 i = 0; i < MAX_GATES; ++i)
        GateStatus[i] = BG_SA_GATE_OK;

    if (!AddCreature(BG_SA_NpcEntries[BG_SA_NPC_KANRETHAD], BG_SA_NPC_KANRETHAD, BG_SA_NpcSpawnlocs[BG_SA_NPC_KANRETHAD]))
    {
        TC_LOG_ERROR("bg.battleground", "SOTA: couldn't spawn Kanrethad, aborted. Entry: {}", BG_SA_NpcEntries[BG_SA_NPC_KANRETHAD]);
        return false;
    }

    for (uint8 i = 0; i <= BG_SA_PORTAL_DEFFENDER_RED; i++)
    {
        if (!AddObject(i, BG_SA_ObjEntries[i], BG_SA_ObjSpawnlocs[i], 0, 0, 0, 0, RESPAWN_ONE_DAY))
        {
            TC_LOG_ERROR("bg.battleground", "SOTA: couldn't spawn BG_SA_PORTAL_DEFFENDER_RED, Entry: {}", BG_SA_ObjEntries[i]);
            continue;
        }
    }

    for (uint8 i = BG_SA_BOAT_ONE; i <= BG_SA_BOAT_TWO; i++)
    {
        uint32 boatid = 0;
        switch (i)
        {
            case BG_SA_BOAT_ONE:
                boatid = Attackers ? BG_SA_BOAT_ONE_H : BG_SA_BOAT_ONE_A;
                break;
            case BG_SA_BOAT_TWO:
                boatid = Attackers ? BG_SA_BOAT_TWO_H : BG_SA_BOAT_TWO_A;
                break;
            default:
                break;
        }
        if (!AddObject(i, boatid, BG_SA_ObjSpawnlocs[i].GetPositionX(),
          BG_SA_ObjSpawnlocs[i].GetPositionY(),
          BG_SA_ObjSpawnlocs[i].GetPositionZ() + (Attackers ? -3.750f: 0),
          BG_SA_ObjSpawnlocs[i].GetOrientation(), 0, 0, 0, 0, RESPAWN_ONE_DAY))
        {
            TC_LOG_ERROR("bg.battleground", "SOTA: couldn't spawn one of the BG_SA_BOAT, Entry: {}", boatid);
            continue;
        }
    }

    // The sigils, flagpoles, graveyard flags and bomb piles are added with no
    // respawn time. Battleground::AddObject leaves an object despawned for the
    // respawn time it is given, and unlike the gates and portals above nothing
    // spawns these afterwards, so with RESPAWN_ONE_DAY nobody could see a
    // graveyard flag or take a bomb. Added this way a bomb pile keeps the
    // default respawn delay (5 min) after it hands out its one charge.
    for (uint8 i = BG_SA_SIGIL_1; i <= BG_SA_LEFT_FLAGPOLE; i++)
    {
        if (!AddObject(i, BG_SA_ObjEntries[i], BG_SA_ObjSpawnlocs[i], 0, 0, 0, 0, RESPAWN_IMMEDIATELY))
        {
            TC_LOG_ERROR("bg.battleground", "SOTA: couldn't spawn Sigil, Entry: {}", BG_SA_ObjEntries[i]);
            continue;
        }
    }

    // MAD props for Kiper for discovering those values - 4 hours of his work.
    GetBGObject(BG_SA_BOAT_ONE)->SetParentRotation(QuaternionData(0.f, 0.f, 1.0f, 0.0002f));
    GetBGObject(BG_SA_BOAT_TWO)->SetParentRotation(QuaternionData(0.f, 0.f, 1.0f, 0.00001f));
    SpawnBGObject(BG_SA_BOAT_ONE, RESPAWN_IMMEDIATELY);
    SpawnBGObject(BG_SA_BOAT_TWO, RESPAWN_IMMEDIATELY);

    //Cannons and demolishers - NPCs are spawned
    //By capturing GYs.
    for (uint8 i = 0; i < BG_SA_DEMOLISHER_5; i++)
    {
        if (!AddCreature(BG_SA_NpcEntries[i], i, BG_SA_NpcSpawnlocs[i], Attackers == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE, 600))
        {
            TC_LOG_ERROR("bg.battleground", "SOTA: couldn't spawn Cannon or demolisher, Entry: {}, Attackers: {}", BG_SA_NpcEntries[i], Attackers == TEAM_ALLIANCE ? "Horde(1)" : "Alliance(0)");
            continue;
        }
    }

    OverrideGunFaction();
    DemolisherStartState(true);

    for (uint8 i = 0; i <= BG_SA_PORTAL_DEFFENDER_RED; i++)
    {
        SpawnBGObject(i, RESPAWN_IMMEDIATELY);
        GetBGObject(i)->SetFaction(defF);
    }

    GetBGObject(BG_SA_TITAN_RELIC)->SetFaction(atF);
    GetBGObject(BG_SA_TITAN_RELIC)->Refresh();

    TotalTime = 0;
    ShipsStarted = false;

    //Graveyards
    for (uint8 i = 0; i < BG_SA_MAX_GY; i++)
    {
        WorldSafeLocsEntry const* sg = sWorldSafeLocsStore.LookupEntry(BG_SA_GYEntries[i]);

        if (!sg)
        {
            TC_LOG_ERROR("bg.battleground", "SOTA: Can't find GY entry {}", BG_SA_GYEntries[i]);
            return false;
        }

        if (i == BG_SA_BEACH_GY)
        {
            GraveyardStatus[i] = Attackers;
            AddSpiritGuide(i + BG_SA_MAXNPC, sg->Loc.X, sg->Loc.Y, sg->Loc.Z, BG_SA_GYOrientation[i], Attackers);
        }
        else
        {
            GraveyardStatus[i] = ((Attackers == TEAM_HORDE)? TEAM_ALLIANCE : TEAM_HORDE);
            if (!AddSpiritGuide(i + BG_SA_MAXNPC, sg->Loc.X, sg->Loc.Y, sg->Loc.Z, BG_SA_GYOrientation[i], Attackers == TEAM_HORDE ? TEAM_ALLIANCE : TEAM_HORDE))
                TC_LOG_ERROR("bg.battleground", "SOTA: couldn't spawn GY: {}", i);
        }
    }

    //GY capture points
    for (uint8 i = BG_SA_CENTRAL_FLAG; i <= BG_SA_LEFT_FLAG; i++)
    {
        if (!AddObject(i, (BG_SA_ObjEntries[i] - (Attackers == TEAM_ALLIANCE ? 1 : 0)), BG_SA_ObjSpawnlocs[i], 0, 0, 0, 0, RESPAWN_IMMEDIATELY))
        {
            TC_LOG_ERROR("bg.battleground", "SOTA: couldn't spawn Central Flag Entry: {}", BG_SA_ObjEntries[i] - (Attackers == TEAM_ALLIANCE ? 1 : 0));
            continue;
        }
        GetBGObject(i)->SetFaction(atF);
    }

    UpdateObjectInteractionFlags();

    for (uint8 i = BG_SA_BOMB; i < BG_SA_MAXOBJ; i++)
    {
        if (!AddObject(i, BG_SA_ObjEntries[BG_SA_BOMB], BG_SA_ObjSpawnlocs[i], 0, 0, 0, 0, RESPAWN_IMMEDIATELY))
        {
            TC_LOG_ERROR("bg.battleground", "SOTA: couldn't spawn SA Bomb Entry: {}", BG_SA_ObjEntries[BG_SA_BOMB] + i);
            continue;
        }
        GetBGObject(i)->SetFaction(atF);
    }

    //Player may enter BEFORE we set up BG - lets update his worldstates anyway...
    UpdateWorldState(BG_SA_RIGHT_GY_HORDE, GraveyardStatus[BG_SA_RIGHT_CAPTURABLE_GY] == TEAM_HORDE ? 1 : 0);
    UpdateWorldState(BG_SA_LEFT_GY_HORDE, GraveyardStatus[BG_SA_LEFT_CAPTURABLE_GY] == TEAM_HORDE ? 1 : 0);
    UpdateWorldState(BG_SA_CENTER_GY_HORDE, GraveyardStatus[BG_SA_CENTRAL_CAPTURABLE_GY] == TEAM_HORDE ? 1 : 0);

    UpdateWorldState(BG_SA_RIGHT_GY_ALLIANCE, GraveyardStatus[BG_SA_RIGHT_CAPTURABLE_GY] == TEAM_ALLIANCE ? 1 : 0);
    UpdateWorldState(BG_SA_LEFT_GY_ALLIANCE, GraveyardStatus[BG_SA_LEFT_CAPTURABLE_GY] == TEAM_ALLIANCE ? 1 : 0);
    UpdateWorldState(BG_SA_CENTER_GY_ALLIANCE, GraveyardStatus[BG_SA_CENTRAL_CAPTURABLE_GY] == TEAM_ALLIANCE ? 1 : 0);

    if (Attackers == TEAM_ALLIANCE)
    {
        UpdateWorldState(BG_SA_ALLY_ATTACKS, 1);
        UpdateWorldState(BG_SA_HORDE_ATTACKS, 0);

        UpdateWorldState(BG_SA_RIGHT_ATT_TOKEN_ALL, 1);
        UpdateWorldState(BG_SA_LEFT_ATT_TOKEN_ALL, 1);
        UpdateWorldState(BG_SA_RIGHT_ATT_TOKEN_HRD, 0);
        UpdateWorldState(BG_SA_LEFT_ATT_TOKEN_HRD, 0);

        UpdateWorldState(BG_SA_HORDE_DEFENCE_TOKEN, 1);
        UpdateWorldState(BG_SA_ALLIANCE_DEFENCE_TOKEN, 0);
    }
    else
    {
        UpdateWorldState(BG_SA_HORDE_ATTACKS, 1);
        UpdateWorldState(BG_SA_ALLY_ATTACKS, 0);

        UpdateWorldState(BG_SA_RIGHT_ATT_TOKEN_ALL, 0);
        UpdateWorldState(BG_SA_LEFT_ATT_TOKEN_ALL, 0);
        UpdateWorldState(BG_SA_RIGHT_ATT_TOKEN_HRD, 1);
        UpdateWorldState(BG_SA_LEFT_ATT_TOKEN_HRD, 1);

        UpdateWorldState(BG_SA_HORDE_DEFENCE_TOKEN, 0);
        UpdateWorldState(BG_SA_ALLIANCE_DEFENCE_TOKEN, 1);
    }

    UpdateWorldState(BG_SA_PURPLE_GATEWS, 1);
    UpdateWorldState(BG_SA_RED_GATEWS, 1);
    UpdateWorldState(BG_SA_BLUE_GATEWS, 1);
    UpdateWorldState(BG_SA_GREEN_GATEWS, 1);
    UpdateWorldState(BG_SA_YELLOW_GATEWS, 1);
    UpdateWorldState(BG_SA_ANCIENT_GATEWS, 1);

    for (int i = BG_SA_BOAT_ONE; i <= BG_SA_BOAT_TWO; i++)
        for (BattlegroundPlayerMap::const_iterator itr = GetPlayers().begin(); itr != GetPlayers().end(); ++itr)
            if (Player* player = ObjectAccessor::FindPlayer(itr->first))
                SendTransportInit(player);

    // set status manually so preparation is cast correctly in 2nd round too
    SetStatus(STATUS_WAIT_JOIN);

    TeleportPlayers();
    return true;
}

void BattlegroundSA::StartShips()
{
    if (ShipsStarted)
        return;

    DoorOpen(BG_SA_BOAT_ONE);
    DoorOpen(BG_SA_BOAT_TWO);

    for (int i = BG_SA_BOAT_ONE; i <= BG_SA_BOAT_TWO; i++)
    {
        for (BattlegroundPlayerMap::const_iterator itr = GetPlayers().begin(); itr != GetPlayers().end(); ++itr)
        {
            if (Player* p = ObjectAccessor::FindPlayer(itr->first))
            {
                UpdateData data;
                WorldPacket pkt;
                GetBGObject(i)->BuildValuesUpdateBlockForPlayer(&data, p);
                data.BuildPacket(&pkt);
                p->SendDirectMessage(&pkt);
            }
        }
    }
    ShipsStarted = true;
}

void BattlegroundSA::PostUpdateImpl(uint32 diff)
{
    UpdateBoatRiders(diff);

    if (InitSecondRound)
    {
        if (UpdateWaitTimer < diff)
        {
            if (!SignaledRoundTwo)
            {
                SignaledRoundTwo = true;
                InitSecondRound = false;
                SendBroadcastText(BG_SA_TEXT_ROUND_TWO_START_ONE_MINUTE, CHAT_MSG_BG_SYSTEM_NEUTRAL);
            }
        }
        else
        {
            UpdateWaitTimer -= diff;
            return;
        }
    }
    TotalTime += diff;

    if (Status == BG_SA_WARMUP)
    {
        EndRoundTimer = BG_SA_ROUNDLENGTH;
        if (TotalTime >= BG_SA_WARMUPLENGTH)
        {
            if (Creature* c = GetBGCreature(BG_SA_NPC_KANRETHAD))
                SendChatMessage(c, TEXT_ROUND_STARTED);

            TotalTime = 0;
            ToggleTimer();
            DemolisherStartState(false);
            Status = BG_SA_ROUND_ONE;
            StartTimedAchievement(ACHIEVEMENT_TIMED_TYPE_EVENT, (Attackers == TEAM_ALLIANCE) ? 23748 : 21702);
        }
        if (TotalTime >= BG_SA_BOAT_START)
            StartShips();
        return;
    }
    else if (Status == BG_SA_SECOND_WARMUP)
    {
        if (RoundScores[0].time<BG_SA_ROUNDLENGTH)
            EndRoundTimer = RoundScores[0].time;
        else
            EndRoundTimer = BG_SA_ROUNDLENGTH;

        if (TotalTime >= 60000)
        {
            if (Creature* c = GetBGCreature(BG_SA_NPC_KANRETHAD))
                SendChatMessage(c, TEXT_ROUND_STARTED);

            TotalTime = 0;
            ToggleTimer();
            DemolisherStartState(false);
            Status = BG_SA_ROUND_TWO;
            StartTimedAchievement(ACHIEVEMENT_TIMED_TYPE_EVENT, (Attackers == TEAM_ALLIANCE) ? 23748 : 21702);
            // status was set to STATUS_WAIT_JOIN manually for Preparation, set it back now
            SetStatus(STATUS_IN_PROGRESS);
            for (BattlegroundPlayerMap::const_iterator itr = GetPlayers().begin(); itr != GetPlayers().end(); ++itr)
                if (Player* p = ObjectAccessor::FindPlayer(itr->first))
                    p->RemoveAurasDueToSpell(SPELL_PREPARATION);
        }
        if (TotalTime >= 30000)
        {
            if (!SignaledRoundTwoHalfMin)
            {
                SignaledRoundTwoHalfMin = true;
                SendBroadcastText(BG_SA_TEXT_ROUND_TWO_START_HALF_MINUTE, CHAT_MSG_BG_SYSTEM_NEUTRAL);
            }
        }
        StartShips();
        return;
    }
    else if (GetStatus() == STATUS_IN_PROGRESS)
    {
        if (Status == BG_SA_ROUND_ONE)
        {
            if (TotalTime >= BG_SA_ROUNDLENGTH)
            {
                CastSpellOnTeam(SPELL_END_OF_ROUND, ALLIANCE);
                CastSpellOnTeam(SPELL_END_OF_ROUND, HORDE);
                RoundScores[0].winner = Attackers;
                RoundScores[0].time = BG_SA_ROUNDLENGTH;
                TotalTime = 0;
                Status = BG_SA_SECOND_WARMUP;
                Attackers = (Attackers == TEAM_ALLIANCE) ? TEAM_HORDE : TEAM_ALLIANCE;
                UpdateWaitTimer = 5000;
                SignaledRoundTwo = false;
                SignaledRoundTwoHalfMin = false;
                InitSecondRound = true;
                ToggleTimer();
                ResetObjs();
                GetBgMap()->UpdateAreaDependentAuras();
                return;
            }
        }
        else if (Status == BG_SA_ROUND_TWO)
        {
            if (TotalTime >= EndRoundTimer)
            {
                CastSpellOnTeam(SPELL_END_OF_ROUND, ALLIANCE);
                CastSpellOnTeam(SPELL_END_OF_ROUND, HORDE);
                RoundScores[1].time = BG_SA_ROUNDLENGTH;
                RoundScores[1].winner = (Attackers == TEAM_ALLIANCE) ? TEAM_HORDE : TEAM_ALLIANCE;
                if (RoundScores[0].time == RoundScores[1].time)
                    EndBattleground(0);
                else if (RoundScores[0].time < RoundScores[1].time)
                    EndBattleground(RoundScores[0].winner == TEAM_ALLIANCE ? ALLIANCE : HORDE);
                else
                    EndBattleground(RoundScores[1].winner == TEAM_ALLIANCE ? ALLIANCE : HORDE);
                return;
            }
        }
        if (Status == BG_SA_ROUND_ONE || Status == BG_SA_ROUND_TWO)
        {
            SendTime();
            UpdateDemolisherSpawns();
        }
    }
}

void BattlegroundSA::StartingEventCloseDoors() { }

void BattlegroundSA::StartingEventOpenDoors()
{
    // The boats leave when the battle starts, however it was started, and the
    // round begins as they come in (BG_SA_WARMUPLENGTH - BG_SA_BOAT_START
    // later). A start brought forward (.gm bgstart) otherwise left the match
    // running for most of the warmup with the boats still tied up.
    if (Status == BG_SA_WARMUP && TotalTime < BG_SA_BOAT_START)
        TotalTime = BG_SA_BOAT_START;
}

void BattlegroundSA::FillInitialWorldStates(WorldPackets::WorldState::InitWorldStates& packet)
{
    bool const allyAttacks = Attackers == TEAM_ALLIANCE;
    bool const hordeAttacks = Attackers == TEAM_HORDE;

    packet.Worldstates.emplace_back(BG_SA_ANCIENT_GATEWS, GateStatus[BG_SA_ANCIENT_GATE]);
    packet.Worldstates.emplace_back(BG_SA_YELLOW_GATEWS, GateStatus[BG_SA_YELLOW_GATE]);
    packet.Worldstates.emplace_back(BG_SA_GREEN_GATEWS, GateStatus[BG_SA_GREEN_GATE]);
    packet.Worldstates.emplace_back(BG_SA_BLUE_GATEWS, GateStatus[BG_SA_BLUE_GATE]);
    packet.Worldstates.emplace_back(BG_SA_RED_GATEWS, GateStatus[BG_SA_RED_GATE]);
    packet.Worldstates.emplace_back(BG_SA_PURPLE_GATEWS, GateStatus[BG_SA_PURPLE_GATE]);

    packet.Worldstates.emplace_back(BG_SA_BONUS_TIMER, 0);
    packet.Worldstates.emplace_back(BG_SA_HORDE_ATTACKS, hordeAttacks);
    packet.Worldstates.emplace_back(BG_SA_ALLY_ATTACKS, allyAttacks);

    // Time will be sent on first update...
    packet.Worldstates.emplace_back(BG_SA_ENABLE_TIMER, TimerEnabled);
    packet.Worldstates.emplace_back(BG_SA_TIMER_MINS, 0);
    packet.Worldstates.emplace_back(BG_SA_TIMER_SEC_TENS, 0);
    packet.Worldstates.emplace_back(BG_SA_TIMER_SEC_DECS, 0);

    packet.Worldstates.emplace_back(BG_SA_RIGHT_GY_HORDE, (GraveyardStatus[BG_SA_RIGHT_CAPTURABLE_GY] == TEAM_HORDE) ? 1 : 0);
    packet.Worldstates.emplace_back(BG_SA_LEFT_GY_HORDE, (GraveyardStatus[BG_SA_LEFT_CAPTURABLE_GY] == TEAM_HORDE) ? 1 : 0);
    packet.Worldstates.emplace_back(BG_SA_CENTER_GY_HORDE, (GraveyardStatus[BG_SA_CENTRAL_CAPTURABLE_GY] == TEAM_HORDE) ? 1 : 0);
    packet.Worldstates.emplace_back(BG_SA_RIGHT_GY_ALLIANCE, (GraveyardStatus[BG_SA_RIGHT_CAPTURABLE_GY] == TEAM_ALLIANCE) ? 1 : 0);
    packet.Worldstates.emplace_back(BG_SA_LEFT_GY_ALLIANCE, (GraveyardStatus[BG_SA_LEFT_CAPTURABLE_GY] == TEAM_ALLIANCE) ? 1 : 0);
    packet.Worldstates.emplace_back(BG_SA_CENTER_GY_ALLIANCE, (GraveyardStatus[BG_SA_CENTRAL_CAPTURABLE_GY] == TEAM_ALLIANCE) ? 1 : 0);

    packet.Worldstates.emplace_back(BG_SA_HORDE_DEFENCE_TOKEN, allyAttacks);
    packet.Worldstates.emplace_back(BG_SA_ALLIANCE_DEFENCE_TOKEN, hordeAttacks);
    packet.Worldstates.emplace_back(BG_SA_LEFT_ATT_TOKEN_HRD, hordeAttacks);
    packet.Worldstates.emplace_back(BG_SA_RIGHT_ATT_TOKEN_HRD, hordeAttacks);
    packet.Worldstates.emplace_back(BG_SA_RIGHT_ATT_TOKEN_ALL, allyAttacks);
    packet.Worldstates.emplace_back(BG_SA_LEFT_ATT_TOKEN_ALL, allyAttacks);
}

void BattlegroundSA::AddPlayer(Player* player)
{
    bool const isInBattleground = IsPlayerInBattleground(player->GetGUID());
    Battleground::AddPlayer(player);
    if (!isInBattleground)
        PlayerScores[player->GetGUID().GetCounter()] = new BattlegroundSAScore(player->GetGUID());

    SendTransportInit(player);

    if (!isInBattleground)
        TeleportToEntrancePosition(player);
}

void BattlegroundSA::RemovePlayer(Player* player, ObjectGuid guid, uint32 /*team*/)
{
    auto const itr = _boatRiders.find(guid);
    if (itr == _boatRiders.end())
        return;

    if (player && player->IsInWorld())
        ReleaseBoatRider(player, itr->second, false);
    _boatRiders.erase(itr);
}

void BattlegroundSA::HandleAreaTrigger(Player* /*Source*/, uint32 /*Trigger*/)
{
    // this is wrong way to implement these things. On official it done by gameobject spell cast.
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;
}

void BattlegroundSA::TeleportPlayers()
{
    for (BattlegroundPlayerMap::const_iterator itr = GetPlayers().begin(); itr != GetPlayers().end(); ++itr)
    {
        if (Player* player = ObjectAccessor::FindPlayer(itr->first))
        {
            // should remove spirit of redemption
            if (player->HasAuraType(SPELL_AURA_SPIRIT_OF_REDEMPTION))
                player->RemoveAurasByType(SPELL_AURA_MOD_SHAPESHIFT);

            if (!player->IsAlive())
            {
                player->ResurrectPlayer(1.0f);
                player->SpawnCorpseBones();
            }

            player->ResetAllPowers();
            player->CombatStopWithPets(true);

            player->CastSpell(player, SPELL_PREPARATION, true);

            TeleportToEntrancePosition(player);
        }
    }
}

void BattlegroundSA::TeleportToEntrancePosition(Player* player)
{
    if (player->GetTeamId() == Attackers)
    {
        // Only a client rides the boat by itself: the client animates it and
        // carries its passengers along, so a bot put on the deck would stay
        // where the boat set out and be left in the sea. A bot starts on the
        // beach, where the boats put everyone else ashore, and is carried
        // across on a boat somebody is riding (UpdateBoatRiders). No head start:
        // the bot module holds it in place until the round begins, through
        // STATUS_WAIT_JOIN and OwnsPreparationState.
        if (!ShipsStarted && !IsBotParticipantSession(player->GetSession()))
        {
            player->AddUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT);

            if (urand(0, 1))
                player->TeleportTo(607, 2682.936f, -830.368f, 15.0f, 2.895f, 0);
            else
                player->TeleportTo(607, 2577.003f, 980.261f, 15.0f, 0.807f, 0);
        }
        else
            player->TeleportTo(WorldLocation(607, SABeachLanding));
    }
    else
        player->TeleportTo(607, 1209.7f, -65.16f, 70.1f, 0.0f, 0);
}

void BattlegroundSA::ProcessEvent(WorldObject* obj, uint32 eventId, WorldObject* invoker /*= nullptr*/)
{
    if (GameObject* go = obj->ToGameObject())
    {
        switch (go->GetGoType())
        {
            case GAMEOBJECT_TYPE_GOOBER:
                if (invoker)
                    if (eventId == BG_SA_EVENT_TITAN_RELIC_ACTIVATED)
                        TitanRelicActivated(invoker->ToPlayer());
                break;
            case GAMEOBJECT_TYPE_DESTRUCTIBLE_BUILDING:
            {
                if (GateInfo const* gate = GetGate(obj->GetEntry()))
                {
                    uint8 gateId = gate->GateId;

                    // damaged
                    if (eventId == go->GetGOInfo()->building.damagedEvent)
                    {
                        GateStatus[gateId] = BG_SA_GATE_DAMAGED;

                        if (Creature* c = obj->FindNearestCreature(NPC_WORLD_TRIGGER, 500.0f))
                            SendChatMessage(c, gate->DamagedText, invoker);

                        PlaySoundToAll(Attackers == TEAM_ALLIANCE ? SOUND_WALL_ATTACKED_ALLIANCE : SOUND_WALL_ATTACKED_HORDE);
                    }
                    // destroyed
                    else if (eventId == go->GetGOInfo()->building.destroyedEvent)
                    {
                        GateStatus[gate->GateId] = BG_SA_GATE_DESTROYED;
                        _gateDestroyed = true;

                        if (gateId < 5)
                            DelObject(gateId + 14);

                        if (Creature* c = obj->FindNearestCreature(NPC_WORLD_TRIGGER, 500.0f))
                            SendChatMessage(c, gate->DestroyedText, invoker);

                        PlaySoundToAll(Attackers == TEAM_ALLIANCE ? SOUND_WALL_DESTROYED_ALLIANCE : SOUND_WALL_DESTROYED_HORDE);

                        bool rewardHonor = true;
                        switch (gateId)
                        {
                            case BG_SA_GREEN_GATE:
                                if (GateStatus[BG_SA_BLUE_GATE] == BG_SA_GATE_DESTROYED)
                                    rewardHonor = false;
                                break;
                            case BG_SA_BLUE_GATE:
                                if (GateStatus[BG_SA_GREEN_GATE] == BG_SA_GATE_DESTROYED)
                                    rewardHonor = false;
                                break;
                            case BG_SA_RED_GATE:
                                if (GateStatus[BG_SA_PURPLE_GATE] == BG_SA_GATE_DESTROYED)
                                    rewardHonor = false;
                                break;
                            case BG_SA_PURPLE_GATE:
                                if (GateStatus[BG_SA_RED_GATE] == BG_SA_GATE_DESTROYED)
                                    rewardHonor = false;
                                break;
                            default:
                                break;
                        }

                        if (invoker)
                        {
                            if (Unit* unit = invoker->ToUnit())
                            {
                                if (Player* player = unit->GetCharmerOrOwnerPlayerOrPlayerItself())
                                {
                                    UpdatePlayerScore(player, SCORE_DESTROYED_WALL, 1);
                                    if (rewardHonor)
                                        UpdatePlayerScore(player, SCORE_BONUS_HONOR, sWorld->getIntConfig(CONFIG_CENTURION_BG_REWARD_HONOR_FLAG_CAP));
                                }
                            }
                        }

                        UpdateObjectInteractionFlags();
                    }
                    else
                        break;

                    UpdateWorldState(gate->WorldState, GateStatus[gateId]);
                }
                break;
            }
            default:
                break;
        }
    }
}

void BattlegroundSA::HandleKillUnit(Creature* creature, Player* killer)
{
    if (creature->GetEntry() == NPC_DEMOLISHER_SA)
    {
        UpdatePlayerScore(killer, SCORE_DESTROYED_DEMOLISHER, 1);
        _allVehiclesAlive[Attackers] = false;
    }
}

/*
  You may ask what the fuck does it do?
  Prevents owner overwriting guns faction with own.
 */
void BattlegroundSA::OverrideGunFaction()
{
    if (!BgCreatures[0])
        return;

    for (uint8 i = BG_SA_GUN_1; i <= BG_SA_GUN_10; i++)
    {
        if (Creature* gun = GetBGCreature(i))
            gun->SetFaction(BG_SA_Factions[Attackers ? TEAM_ALLIANCE : TEAM_HORDE]);
    }

    for (uint8 i = BG_SA_DEMOLISHER_1; i <= BG_SA_DEMOLISHER_4; i++)
    {
        if (Creature* dem = GetBGCreature(i))
            dem->SetFaction(BG_SA_Factions[Attackers]);
    }
}

void BattlegroundSA::DemolisherStartState(bool start)
{
    if (!BgCreatures[0])
        return;

    // set flags only for the demolishers on the beach, factory ones dont need it
    for (uint8 i = BG_SA_DEMOLISHER_1; i <= BG_SA_DEMOLISHER_4; i++)
    {
        if (Creature* dem = GetBGCreature(i))
        {
            if (start)
                dem->SetUnitFlag(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_UNINTERACTIBLE);
            else
                dem->RemoveUnitFlag(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_UNINTERACTIBLE);
        }
    }
}

void BattlegroundSA::DestroyGate(Player* /*player*/, GameObject* /*go*/)
{
}

WorldSafeLocsEntry const* BattlegroundSA::GetClosestGraveyard(Player* player)
{
    uint32 safeloc = 0;
    WorldSafeLocsEntry const* ret;
    WorldSafeLocsEntry const* closest;
    float dist, nearest;
    float x, y, z;

    player->GetPosition(x, y, z);

    if (player->GetTeamId() == Attackers)
        safeloc = BG_SA_GYEntries[BG_SA_BEACH_GY];
    else
        safeloc = BG_SA_GYEntries[BG_SA_DEFENDER_LAST_GY];

    closest = sWorldSafeLocsStore.LookupEntry(safeloc);
    nearest = player->GetExactDistSq(closest->Loc.X, closest->Loc.Y, closest->Loc.Z);

    for (uint8 i = BG_SA_RIGHT_CAPTURABLE_GY; i < BG_SA_MAX_GY; i++)
    {
        if (GraveyardStatus[i] != player->GetTeamId())
            continue;

        ret = sWorldSafeLocsStore.LookupEntry(BG_SA_GYEntries[i]);
        dist = player->GetExactDistSq(ret->Loc.X, ret->Loc.Y, ret->Loc.Z);
        if (dist < nearest)
        {
            closest = ret;
            nearest = dist;
        }
    }

    return closest;
}

void BattlegroundSA::SendTime()
{
    uint32 end_of_round = (EndRoundTimer - TotalTime);
    UpdateWorldState(BG_SA_TIMER_MINS, end_of_round/60000);
    UpdateWorldState(BG_SA_TIMER_SEC_TENS, (end_of_round%60000)/10000);
    UpdateWorldState(BG_SA_TIMER_SEC_DECS, ((end_of_round%60000)%10000)/1000);
}

bool BattlegroundSA::CanInteractWithObject(uint32 objectId) const
{
    switch (objectId)
    {
        case BG_SA_TITAN_RELIC:
            if (GateStatus[BG_SA_ANCIENT_GATE] != BG_SA_GATE_DESTROYED || GateStatus[BG_SA_YELLOW_GATE] != BG_SA_GATE_DESTROYED)
                return false;
            [[fallthrough]];
        case BG_SA_CENTRAL_FLAG:
            if (GateStatus[BG_SA_RED_GATE] != BG_SA_GATE_DESTROYED && GateStatus[BG_SA_PURPLE_GATE] != BG_SA_GATE_DESTROYED)
                return false;
            [[fallthrough]];
        case BG_SA_LEFT_FLAG:
        case BG_SA_RIGHT_FLAG:
            if (GateStatus[BG_SA_GREEN_GATE] != BG_SA_GATE_DESTROYED && GateStatus[BG_SA_BLUE_GATE] != BG_SA_GATE_DESTROYED)
                return false;
            break;
        default:
            ABORT();
            break;
    }

    return true;
}

void BattlegroundSA::UpdateObjectInteractionFlags(uint32 objectId)
{
    if (GameObject* go = GetBGObject(objectId))
    {
        if (CanInteractWithObject(objectId))
            go->RemoveFlag(GO_FLAG_NOT_SELECTABLE);
        else
            go->SetFlag(GO_FLAG_NOT_SELECTABLE);
    }
}

void BattlegroundSA::UpdateObjectInteractionFlags()
{
    for (uint8 i = BG_SA_CENTRAL_FLAG; i <= BG_SA_LEFT_FLAG; ++i)
        UpdateObjectInteractionFlags(i);
    UpdateObjectInteractionFlags(BG_SA_TITAN_RELIC);
}

void BattlegroundSA::EventPlayerClickedOnFlag(Player* source, GameObject* go)
{
    switch (go->GetEntry())
    {
        case 191307:
        case 191308:
            if (CanInteractWithObject(BG_SA_LEFT_FLAG))
                CaptureGraveyard(BG_SA_LEFT_CAPTURABLE_GY, source);
            break;
        case 191305:
        case 191306:
            if (CanInteractWithObject(BG_SA_RIGHT_FLAG))
                CaptureGraveyard(BG_SA_RIGHT_CAPTURABLE_GY, source);
            break;
        case 191310:
        case 191309:
            if (CanInteractWithObject(BG_SA_CENTRAL_FLAG))
                CaptureGraveyard(BG_SA_CENTRAL_CAPTURABLE_GY, source);
            break;
        default:
            return;
    };
}

void BattlegroundSA::CaptureGraveyard(BG_SA_Graveyards i, Player* Source)
{
    if (GraveyardStatus[i] == Attackers)
        return;

    DelCreature(AsUnderlyingType(BG_SA_MAXNPC) + i);
    GraveyardStatus[i] = Source->GetTeamId();
    WorldSafeLocsEntry const* sg = sWorldSafeLocsStore.LookupEntry(BG_SA_GYEntries[i]);
    if (!sg)
    {
        TC_LOG_ERROR("bg.battleground", "BattlegroundSA::CaptureGraveyard: non-existant GY entry: {}", BG_SA_GYEntries[i]);
        return;
    }

    AddSpiritGuide(i + AsUnderlyingType(BG_SA_MAXNPC), sg->Loc.X, sg->Loc.Y, sg->Loc.Z, BG_SA_GYOrientation[i], GraveyardStatus[i]);
    uint32 npc = 0;
    uint32 flag = 0;

    // The new flag is added spawned, like the first one (see ResetObjs).
    switch (i)
    {
        case BG_SA_LEFT_CAPTURABLE_GY:
            flag = BG_SA_LEFT_FLAG;
            DelObject(flag);
            AddObject(flag, (BG_SA_ObjEntries[flag] - (Source->GetTeamId() == TEAM_ALLIANCE ? 0 : 1)),
                BG_SA_ObjSpawnlocs[flag], 0, 0, 0, 0, RESPAWN_IMMEDIATELY);

            npc = BG_SA_NPC_RIGSPARK;
            if (Creature* rigspark = AddCreature(BG_SA_NpcEntries[npc], npc, BG_SA_NpcSpawnlocs[npc], Attackers))
                rigspark->AI()->Talk(TEXT_SPARKLIGHT_RIGSPARK_SPAWN);

            for (uint8 j = BG_SA_DEMOLISHER_7; j <= BG_SA_DEMOLISHER_8; j++)
            {
                AddCreature(BG_SA_NpcEntries[j], j, BG_SA_NpcSpawnlocs[j], (Attackers == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE), 600);

                if (Creature* dem = GetBGCreature(j))
                    dem->SetFaction(BG_SA_Factions[Attackers]);
            }

            UpdateWorldState(BG_SA_LEFT_GY_ALLIANCE, GraveyardStatus[i] == TEAM_ALLIANCE);
            UpdateWorldState(BG_SA_LEFT_GY_HORDE, GraveyardStatus[i] == TEAM_HORDE);

            if (Creature* c = Source->FindNearestCreature(NPC_WORLD_TRIGGER, 500.0f))
                SendChatMessage(c, Source->GetTeamId() == TEAM_ALLIANCE ? TEXT_WEST_GRAVEYARD_CAPTURED_A : TEXT_WEST_GRAVEYARD_CAPTURED_H, Source);

            break;
        case BG_SA_RIGHT_CAPTURABLE_GY:
            flag = BG_SA_RIGHT_FLAG;
            DelObject(flag);
            AddObject(flag, (BG_SA_ObjEntries[flag] - (Source->GetTeamId() == TEAM_ALLIANCE ? 0 : 1)),
                BG_SA_ObjSpawnlocs[flag], 0, 0, 0, 0, RESPAWN_IMMEDIATELY);

            npc = BG_SA_NPC_SPARKLIGHT;
            if (Creature* sparklight = AddCreature(BG_SA_NpcEntries[npc], npc, BG_SA_NpcSpawnlocs[npc], Attackers))
                sparklight->AI()->Talk(TEXT_SPARKLIGHT_RIGSPARK_SPAWN);

            for (uint8 j = BG_SA_DEMOLISHER_5; j <= BG_SA_DEMOLISHER_6; j++)
            {
                AddCreature(BG_SA_NpcEntries[j], j, BG_SA_NpcSpawnlocs[j], Attackers == TEAM_ALLIANCE ? TEAM_HORDE : TEAM_ALLIANCE, 600);

                if (Creature* dem = GetBGCreature(j))
                    dem->SetFaction(BG_SA_Factions[Attackers]);
            }

            UpdateWorldState(BG_SA_RIGHT_GY_ALLIANCE, GraveyardStatus[i] == TEAM_ALLIANCE);
            UpdateWorldState(BG_SA_RIGHT_GY_HORDE, GraveyardStatus[i] == TEAM_HORDE);

            if (Creature* c = Source->FindNearestCreature(NPC_WORLD_TRIGGER, 500.0f))
                SendChatMessage(c, Source->GetTeamId() == TEAM_ALLIANCE ? TEXT_EAST_GRAVEYARD_CAPTURED_A : TEXT_EAST_GRAVEYARD_CAPTURED_H, Source);

            break;
        case BG_SA_CENTRAL_CAPTURABLE_GY:
            flag = BG_SA_CENTRAL_FLAG;
            DelObject(flag);
            AddObject(flag, (BG_SA_ObjEntries[flag] - (Source->GetTeamId() == TEAM_ALLIANCE ? 0 : 1)),
              BG_SA_ObjSpawnlocs[flag], 0, 0, 0, 0, RESPAWN_IMMEDIATELY);

            UpdateWorldState(BG_SA_CENTER_GY_ALLIANCE, GraveyardStatus[i] == TEAM_ALLIANCE);
            UpdateWorldState(BG_SA_CENTER_GY_HORDE, GraveyardStatus[i] == TEAM_HORDE);

            if (Creature* c = Source->FindNearestCreature(NPC_WORLD_TRIGGER, 500.0f))
                SendChatMessage(c, Source->GetTeamId() == TEAM_ALLIANCE ? TEXT_SOUTH_GRAVEYARD_CAPTURED_A : TEXT_SOUTH_GRAVEYARD_CAPTURED_H, Source);

            break;
        default:
            ABORT();
            break;
    };
}

void BattlegroundSA::TitanRelicActivated(Player* clicker)
{
    if (!clicker)
        return;

    if (CanInteractWithObject(BG_SA_TITAN_RELIC))
    {
        if (clicker->GetTeamId() == Attackers)
        {
            if (clicker->GetTeamId() == TEAM_ALLIANCE)
                SendBroadcastText(BG_SA_TEXT_ALLIANCE_CAPTURED_TITAN_PORTAL, CHAT_MSG_BG_SYSTEM_ALLIANCE);
            else
                SendBroadcastText(BG_SA_TEXT_HORDE_CAPTURED_TITAN_PORTAL, CHAT_MSG_BG_SYSTEM_HORDE);

            if (Status == BG_SA_ROUND_ONE)
            {
                RoundScores[0].winner = Attackers;
                RoundScores[0].time = TotalTime;
                // Achievement Storm the Beach (1310)
                for (BattlegroundPlayerMap::const_iterator itr = GetPlayers().begin(); itr != GetPlayers().end(); ++itr)
                {
                    if (Player* player = ObjectAccessor::FindPlayer(itr->first))
                        if (player->GetTeamId() == Attackers)
                            player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_BE_SPELL_TARGET, 65246);
                }

                Attackers = (Attackers == TEAM_ALLIANCE) ? TEAM_HORDE : TEAM_ALLIANCE;
                Status = BG_SA_SECOND_WARMUP;
                TotalTime = 0;
                ToggleTimer();

                if (Creature* c = GetBGCreature(BG_SA_NPC_KANRETHAD))
                    SendChatMessage(c, TEXT_ROUND_1_FINISHED);

                UpdateWaitTimer = 5000;
                SignaledRoundTwo = false;
                SignaledRoundTwoHalfMin = false;
                InitSecondRound = true;
                ResetObjs();
                GetBgMap()->UpdateAreaDependentAuras();
                CastSpellOnTeam(SPELL_END_OF_ROUND, ALLIANCE);
                CastSpellOnTeam(SPELL_END_OF_ROUND, HORDE);
            }
            else if (Status == BG_SA_ROUND_TWO)
            {
                RoundScores[1].winner = Attackers;
                RoundScores[1].time = TotalTime;
                ToggleTimer();
                // Achievement Storm the Beach (1310)
                for (BattlegroundPlayerMap::const_iterator itr = GetPlayers().begin(); itr != GetPlayers().end(); ++itr)
                {
                    if (Player* player = ObjectAccessor::FindPlayer(itr->first))
                        if (player->GetTeamId() == Attackers && RoundScores[1].winner == Attackers)
                            player->UpdateAchievementCriteria(ACHIEVEMENT_CRITERIA_TYPE_BE_SPELL_TARGET, 65246);
                }

                if (RoundScores[0].time == RoundScores[1].time)
                    EndBattleground(0);
                else if (RoundScores[0].time < RoundScores[1].time)
                    EndBattleground(RoundScores[0].winner == TEAM_ALLIANCE ? ALLIANCE : HORDE);
                else
                    EndBattleground(RoundScores[1].winner == TEAM_ALLIANCE ? ALLIANCE : HORDE);
            }
        }
    }
}

void BattlegroundSA::ToggleTimer()
{
    TimerEnabled = !TimerEnabled;
    UpdateWorldState(BG_SA_ENABLE_TIMER, TimerEnabled);
}

void BattlegroundSA::EndBattleground(uint32 winner)
{
    // End-of-match honor is the flat Centurion.Battleground.RewardHonorWinner /
    // _LOSER that Battleground::EndBattleground pays every participant, doubled
    // there on a Call to Arms. The stock per-kill awards that used to sit here
    // paid a second, unconfigurable amount on top of it.
    Battleground::EndBattleground(winner);
}

void BattlegroundSA::UpdateDemolisherSpawns()
{
    for (uint8 i = BG_SA_DEMOLISHER_1; i <= BG_SA_DEMOLISHER_8; i++)
    {
        if (BgCreatures[i])
        {
            if (Creature* Demolisher = GetBGCreature(i))
            {
                if (Demolisher->isDead())
                {
                    // Demolisher is not in list
                    if (DemoliserRespawnList.find(i) == DemoliserRespawnList.end())
                    {
                        DemoliserRespawnList[i] = GameTime::GetGameTimeMS()+30000;
                    }
                    else
                    {
                        if (DemoliserRespawnList[i] < GameTime::GetGameTimeMS())
                        {
                            Demolisher->Relocate(BG_SA_NpcSpawnlocs[i]);
                            Demolisher->Respawn();
                            DemoliserRespawnList.erase(i);
                        }
                    }
                }
            }
        }
    }
}

void BattlegroundSA::SendTransportInit(Player* player)
{
    if (BgObjects[BG_SA_BOAT_ONE] ||  BgObjects[BG_SA_BOAT_TWO])
    {
        UpdateData transData;
        if (BgObjects[BG_SA_BOAT_ONE])
            GetBGObject(BG_SA_BOAT_ONE)->BuildCreateUpdateBlockForPlayer(&transData, player);
        if (BgObjects[BG_SA_BOAT_TWO])
            GetBGObject(BG_SA_BOAT_TWO)->BuildCreateUpdateBlockForPlayer(&transData, player);
        WorldPacket packet;
        transData.BuildPacket(&packet);
        player->SendDirectMessage(&packet);
    }
}

void BattlegroundSA::SendTransportsRemove(Player* player)
{
    if (BgObjects[BG_SA_BOAT_ONE] ||  BgObjects[BG_SA_BOAT_TWO])
    {
        UpdateData transData;
        if (BgObjects[BG_SA_BOAT_ONE])
            GetBGObject(BG_SA_BOAT_ONE)->BuildOutOfRangeUpdateBlock(&transData);
        if (BgObjects[BG_SA_BOAT_TWO])
            GetBGObject(BG_SA_BOAT_TWO)->BuildOutOfRangeUpdateBlock(&transData);
        WorldPacket packet;
        transData.BuildPacket(&packet);
        player->SendDirectMessage(&packet);
    }
}

bool BattlegroundSA::CheckAchievementCriteriaMeet(uint32 criteriaId, Player const* source, Unit const* target, uint32 miscValue)
{
    switch (criteriaId)
    {
        case BG_CRITERIA_CHECK_NOT_EVEN_A_SCRATCH:
            return _allVehiclesAlive[GetTeamIndexByTeamId(source->GetTeam())];
        case BG_CRITERIA_CHECK_DEFENSE_OF_THE_ANCIENTS:
            return source->GetTeamId() != Attackers && !_gateDestroyed;
    }

    return Battleground::CheckAchievementCriteriaMeet(criteriaId, source, target, miscValue);
}

bool BattlegroundSA::IsSpellAllowed(uint32 spellId, Player const* /*player*/) const
{
    switch (spellId)
    {
        case SPELL_ALLIANCE_CONTROL_PHASE_SHIFT:
            return Attackers == TEAM_HORDE;
        case SPELL_HORDE_CONTROL_PHASE_SHIFT:
            return Attackers == TEAM_ALLIANCE;
        case SPELL_PREPARATION:
            return Status == BG_SA_WARMUP || Status == BG_SA_SECOND_WARMUP;
        default:
           break;
    }

    return true;
}

// The match described to players without a client (the playerbot module, which
// never names a battleground) through the objective calls of Battleground.h.

namespace
{
    // The graveyard nodes of GetDynamicNodeInfo, by the flag that takes each.
    struct SAGraveyardNode
    {
        uint32 Flag;
        BG_SA_Graveyards Graveyard;
    };

    SAGraveyardNode const SAGraveyardNodes[BG_SA_NODE_TITAN_RELIC] =
    {
        { BG_SA_RIGHT_FLAG,   BG_SA_RIGHT_CAPTURABLE_GY   },
        { BG_SA_LEFT_FLAG,    BG_SA_LEFT_CAPTURABLE_GY    },
        { BG_SA_CENTRAL_FLAG, BG_SA_CENTRAL_CAPTURABLE_GY }
    };

    // How far in front of a gate's origin its siege objective stands. Every
    // gate faces the side its attackers come from (the outer pair look at the
    // beach, the yellow gate and the chamber straight down the road away from
    // the relic), and the gate model (WG_Gate01) reaches 5.9 yards out along
    // that facing, so this is open ground a few yards clear of its face.
    float const SAGateFrontDistance = 10.0f;

    // The ground under each gate's front spot, in gate order, from the live
    // navmesh (2026-10-15). A gate's origin is not at its foot - the chamber's
    // stands 8.3 yards above the road - and the server's path search only
    // reaches about five yards up or down, so a spot at gate height left
    // nobody able to walk to it.
    float const SAGateFrontGroundZ[MAX_GATES] = { 26.91f, 83.46f, 28.11f, 53.82f, 50.45f, 109.59f };

    // The relic floats 7.7 yards over the chamber floor, which is level all
    // around it; players click it from there.
    float const SATitanRelicFloorZ = 112.5f;

    // The spawns of World Trigger (Large AOI, Not Immune PC/NPC), NPC 23472, on
    // map 607 in the world database. Using a Defender's Portal has the user cast
    // 54640 on themselves, whose spell_scripts row has them cast 54643, and
    // 54643 sends its caster to the 23472 nearest to it
    // (TARGET_DEST_NEARBY_ENTRY): a portal leads to the trigger nearest to it.
    // Each portal stands outside its gate, and that trigger is behind the gate.
    Position const SADefenderPortalExits[] =
    {
        { 1043.52f,  -87.8507f, 87.197f  },
        { 1193.76f,   69.9405f, 58.1295f },
        { 1227.89f, -235.458f,  60.1112f },
        { 1389.06f,  102.941f,  34.5742f },
        { 1441.08f, -240.594f,  35.3471f }
    };

    Position const& GetDefenderPortalExit(Position const& portal)
    {
        Position const* exit = &SADefenderPortalExits[0];
        for (Position const& trigger : SADefenderPortalExits)
            if (portal.GetExactDistSq(trigger) < portal.GetExactDistSq(*exit))
                exit = &trigger;

        return *exit;
    }
}

bool BattlegroundSA::IsRoundInProgress() const
{
    // ResetObjs puts the match back in STATUS_WAIT_JOIN between the rounds,
    // and nothing can be taken during either warmup.
    return GetStatus() == STATUS_IN_PROGRESS && (Status == BG_SA_ROUND_ONE || Status == BG_SA_ROUND_TWO);
}

bool BattlegroundSA::GetParticipantTeamId(ObjectGuid playerGuid, TeamId& teamId) const
{
    uint32 const team = GetPlayerTeam(playerGuid);
    if (team != ALLIANCE && team != HORDE)
        return false;

    teamId = GetTeamIndexByTeamId(team);
    return true;
}

bool BattlegroundSA::IsGateBreachable(uint32 gateId) const
{
    if (gateId >= MAX_GATES || GateStatus[gateId] == BG_SA_GATE_DESTROYED)
        return false;

    // The chain CanInteractWithObject walks: either gate of a wall opens the
    // ground behind that whole wall.
    bool const outerWallOpen = GateStatus[BG_SA_GREEN_GATE] == BG_SA_GATE_DESTROYED || GateStatus[BG_SA_BLUE_GATE] == BG_SA_GATE_DESTROYED;
    bool const middleWallOpen = GateStatus[BG_SA_RED_GATE] == BG_SA_GATE_DESTROYED || GateStatus[BG_SA_PURPLE_GATE] == BG_SA_GATE_DESTROYED;

    switch (gateId)
    {
        case BG_SA_GREEN_GATE:
        case BG_SA_BLUE_GATE:
            return true;
        case BG_SA_RED_GATE:
        case BG_SA_PURPLE_GATE:
            return outerWallOpen;
        case BG_SA_YELLOW_GATE:
            return outerWallOpen && middleWallOpen;
        case BG_SA_ANCIENT_GATE:
            return outerWallOpen && middleWallOpen && GateStatus[BG_SA_YELLOW_GATE] == BG_SA_GATE_DESTROYED;
        default:
            return false;
    }
}

GameObject* BattlegroundSA::FindMatchObject(uint32 type) const
{
    BattlegroundMap* map = FindBgMap();
    if (!map || type >= BgObjects.size() || !BgObjects[type])
        return nullptr;

    return map->GetGameObject(BgObjects[type]);
}

Creature* BattlegroundSA::FindMatchCreature(uint32 type) const
{
    BattlegroundMap* map = FindBgMap();
    if (!map || type >= BgCreatures.size() || !BgCreatures[type])
        return nullptr;

    return map->GetCreature(BgCreatures[type]);
}

bool BattlegroundSA::GetDynamicNodeInfo(ObjectGuid playerGuid, uint32 nodeId, BattlegroundNodeObjective& node) const
{
    if (nodeId >= BG_SA_DYNAMIC_NODE_COUNT || !IsRoundInProgress())
        return false;

    // Only the attackers take anything here: a graveyard is never taken back
    // and the relic answers only them. The defenders get no nodes and play
    // the gates (GetSiegeObjectiveInfo) instead.
    TeamId teamId = TEAM_NEUTRAL;
    if (!GetParticipantTeamId(playerGuid, teamId) || teamId != Attackers)
        return false;

    uint32 objectId = BG_SA_TITAN_RELIC;
    if (nodeId != BG_SA_NODE_TITAN_RELIC)
    {
        // Nobody has to stand on a graveyard the attackers already hold, and
        // once the relic can be clicked it ends the round: nobody is sent to
        // a graveyard instead.
        if (GraveyardStatus[SAGraveyardNodes[nodeId].Graveyard] == Attackers ||
            CanInteractWithObject(BG_SA_TITAN_RELIC))
            return false;

        objectId = SAGraveyardNodes[nodeId].Flag;
    }

    // A flag, and the relic, open once the gates in front of it are down.
    if (!CanInteractWithObject(objectId))
        return false;

    GameObject* objective = FindMatchObject(objectId);
    if (!objective || !objective->isSpawned())
        return false;

    node.NodeId = nodeId;
    node.Status = BattlegroundNodeStatus::EnemyControlled;
    node.Location = objective->GetPosition();
    if (nodeId == BG_SA_NODE_TITAN_RELIC)
        node.Location.m_positionZ = SATitanRelicFloorZ;
    node.BannerGuid = objective->GetGUID();
    return true;
}

bool BattlegroundSA::GetSiegeObjectiveInfo(ObjectGuid playerGuid, uint32 index, BattlegroundSiegeObjective& objective) const
{
    if (index >= MAX_GATES || GetStatus() != STATUS_IN_PROGRESS)
        return false;

    TeamId teamId = TEAM_NEUTRAL;
    if (!GetParticipantTeamId(playerGuid, teamId) || !BgObjects[index])
        return false;

    // Gate objects and GateStatus share the BG_SA_*_GATE numbering.
    Position const& gate = BG_SA_ObjSpawnlocs[index];
    float const facing = gate.GetOrientation();

    objective.Id = index;
    objective.GateGuid = BgObjects[index];
    // Facing the gate, as a charge is set down in front of whoever plants it
    // (52410, TARGET_DEST_CASTER_FRONT).
    objective.Location.Relocate(gate.GetPositionX() + SAGateFrontDistance * std::cos(facing),
        gate.GetPositionY() + SAGateFrontDistance * std::sin(facing), SAGateFrontGroundZ[index], facing + float(M_PI));
    objective.Friendly = teamId != Attackers;
    // A gate can be broken while a round is fought. The defenders man the ones
    // the first round opens on while its attackers are still at sea, as people
    // do; the second round's crossing is spent in STATUS_WAIT_JOIN.
    bool const siegeUnderWay = IsRoundInProgress() || (objective.Friendly && Status == BG_SA_WARMUP);
    objective.Breachable = siegeUnderWay && IsGateBreachable(index);
    return true;
}

bool BattlegroundSA::GetSiegeChargeInfo(ObjectGuid playerGuid, BattlegroundSiegeChargeInfo& charge) const
{
    if (!IsRoundInProgress())
        return false;

    // The bomb piles carry the attackers' faction (ResetObjs).
    TeamId teamId = TEAM_NEUTRAL;
    if (!GetParticipantTeamId(playerGuid, teamId) || teamId != Attackers)
        return false;

    charge.Sources.clear();
    for (uint32 i = BG_SA_BOMB; i < BG_SA_MAXOBJ; ++i)
    {
        GameObject* bomb = FindMatchObject(i);
        if (!bomb || !bomb->isSpawned() || bomb->HasFlag(GO_FLAG_NOT_SELECTABLE))
            continue;

        // A pile hands out a single charge and only goes on its respawn timer
        // at its next update, so one emptied this tick is already gone.
        uint32 const charges = bomb->GetGOInfo()->GetCharges();
        if (charges && bomb->GetUseCount() >= charges)
            continue;

        charge.Sources.push_back(bomb->GetGUID());
    }

    if (charge.Sources.empty())
        return false;

    charge.ItemId = ITEM_MASSIVE_SEAFORIUM_CHARGE;
    return true;
}

void BattlegroundSA::GetSiegeVehicles(ObjectGuid playerGuid, GuidVector& vehicles) const
{
    if (!IsRoundInProgress())
        return;

    // Demolishers carry the attackers' faction (OverrideGunFaction,
    // CaptureGraveyard), and their spellclick seats only a friendly clicker.
    TeamId teamId = TEAM_NEUTRAL;
    if (!GetParticipantTeamId(playerGuid, teamId) || teamId != Attackers)
        return;

    for (uint32 i = BG_SA_DEMOLISHER_1; i <= BG_SA_DEMOLISHER_8; ++i)
    {
        // The workshop four exist once their graveyard is taken, a dead one
        // waits on UpdateDemolisherSpawns, and DemolisherStartState keeps the
        // beach four uninteractible until the round starts. A vehicle with no
        // seat left to click loses its spellclick flag.
        Creature* demolisher = FindMatchCreature(i);
        if (!demolisher || !demolisher->IsAlive() || demolisher->HasUnitFlag(UNIT_FLAG_UNINTERACTIBLE) ||
            !demolisher->HasNpcFlag(UNIT_NPC_FLAG_SPELLCLICK))
            continue;

        Vehicle const* vehicle = demolisher->GetVehicleKit();
        if (!vehicle)
            continue;

        // Free to drive while its driver's seat is: the other two seats of
        // vehicle 158 only carry passengers.
        bool driverSeatFree = false;
        for (SeatMap::value_type const& seat : vehicle->Seats)
        {
            if (seat.second.SeatInfo && seat.second.SeatInfo->HasFlag(VEHICLE_SEAT_FLAG_CAN_CONTROL))
            {
                driverSeatFree = seat.second.IsEmpty();
                break;
            }
        }

        if (driverSeatFree)
            vehicles.push_back(demolisher->GetGUID());
    }
}

void BattlegroundSA::GetStandingGates(GuidVector& gates) const
{
    // A damaged gate still blocks the way; only a destroyed one is open.
    for (uint8 i = 0; i < MAX_GATES; ++i)
        if (GateStatus[i] != BG_SA_GATE_DESTROYED && !BgObjects[i].IsEmpty())
            gates.push_back(BgObjects[i]);
}

void BattlegroundSA::GetTeleporters(ObjectGuid playerGuid, std::vector<BattlegroundTeleporter>& teleporters) const
{
    if (!IsRoundInProgress())
        return;

    // The portals carry the defenders' faction (ResetObjs), which is all that
    // keeps an attacker off them: 54640's script checks teams only in
    // Wintergrasp.
    TeamId teamId = TEAM_NEUTRAL;
    if (!GetParticipantTeamId(playerGuid, teamId) || teamId == Attackers)
        return;

    for (uint32 i = BG_SA_PORTAL_DEFFENDER_BLUE; i <= BG_SA_PORTAL_DEFFENDER_RED; ++i)
    {
        GameObject* portal = FindMatchObject(i);
        if (!portal || !portal->isSpawned())
            continue;

        BattlegroundTeleporter teleporter;
        teleporter.GoGuid = portal->GetGUID();
        teleporter.Entrance = portal->GetPosition();
        teleporter.Exit = GetDefenderPortalExit(*portal);
        teleporters.push_back(teleporter);
    }
}

bool BattlegroundSA::OwnsPreparationState(Player const* player) const
{
    // Round one's attackers spend the minute after the start at sea: the boats
    // leave as the battle starts and the round begins as they come in, so the
    // match reads in progress while nobody can land yet. Their bots wait it
    // out - on a boat, or on the beach - instead of running at gates nobody may
    // touch yet. The second round's crossing is spent in STATUS_WAIT_JOIN,
    // which holds everybody already.
    if (!player || Status != BG_SA_WARMUP || GetStatus() != STATUS_IN_PROGRESS)
        return false;

    TeamId teamId = TEAM_NEUTRAL;
    return GetParticipantTeamId(player->GetGUID(), teamId) && teamId == Attackers;
}

// Bots on the boats. A boat (GAMEOBJECT_TYPE_TRANSPORT) only moves on the
// clients, and a client riding one names the boat and its place on the deck in
// every movement packet it sends (MovementHandler keeps both). A bot given that
// boat and a place beside such a passenger is drawn on the deck, and carried
// along, by every client that sees it. Server-side it stays next to the
// passenger, so the two stay in sight of each other, and its pets within their
// leash, all the way across.
namespace
{
    uint32 const SABoatRiderUpdateMs = 1000;
    // Riders stand in rows of four beside the passenger whose place on the deck
    // they share, this far apart: tight enough to stay on the deck whichever
    // way its length runs, which nothing here knows.
    float const SABoatRiderSpacing = 1.5f;
    float const SABoatRiderFollowRange = 5.0f;

    Position GetBoatRiderOffset(Position const& passenger, uint32 slot)
    {
        // Never on the passenger: the first row a pace to one side, the second
        // a pace to the other, then two paces out, and so on.
        float const along = (float(slot % 4) - 1.5f) * SABoatRiderSpacing;
        uint32 const row = slot / 4;
        float const across = (row % 2 ? -1.0f : 1.0f) * float(row / 2 + 1) * SABoatRiderSpacing;
        return Position(passenger.GetPositionX() + along, passenger.GetPositionY() + across, passenger.GetPositionZ(),
            passenger.GetOrientation());
    }

    // What follows a bot about: its pet and guardians, not its totems.
    void CollectFollowers(Player* bot, std::vector<Unit*>& followers)
    {
        for (Unit* unit : bot->m_Controlled)
            if (unit && unit->GetTypeId() != TYPEID_PLAYER && !unit->IsTotem() && unit->IsInWorld() &&
                unit->GetMapId() == bot->GetMapId())
                followers.push_back(unit);
    }
}

void BattlegroundSA::UpdateBoatRiders(uint32 diff)
{
    // Everybody comes off when the round starts.
    bool const crossing = (Status == BG_SA_WARMUP || Status == BG_SA_SECOND_WARMUP) &&
        (GetStatus() == STATUS_WAIT_JOIN || GetStatus() == STATUS_IN_PROGRESS);
    if (!crossing)
    {
        if (!_boatRiders.empty())
            ReleaseBoatRiders(Status == BG_SA_ROUND_ONE || Status == BG_SA_ROUND_TWO);
        return;
    }

    if (_boatRiderTimer > diff)
    {
        _boatRiderTimer -= diff;
        return;
    }
    _boatRiderTimer = SABoatRiderUpdateMs;

    BattlegroundMap* map = FindBgMap();
    if (!map)
        return;

    // A passenger with a client on each boat, and the attackers' bots.
    Player* passengers[2] = { nullptr, nullptr };
    std::vector<Player*> bots;
    for (auto const& entry : GetPlayers())
    {
        TeamId teamId = TEAM_NEUTRAL;
        Player* player = ObjectAccessor::GetPlayer(map, entry.first);
        if (!player || !player->IsInWorld() || !GetParticipantTeamId(entry.first, teamId) || teamId != Attackers)
            continue;

        if (IsBotParticipantSession(player->GetSession()))
        {
            bots.push_back(player);
            continue;
        }

        if (!player->HasUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT))
            continue;

        for (uint8 boat = 0; boat < 2; ++boat)
            if (!passengers[boat] && !BgObjects[BG_SA_BOAT_ONE + boat].IsEmpty() &&
                player->m_movementInfo.transport.guid == BgObjects[BG_SA_BOAT_ONE + boat])
                passengers[boat] = player;
    }

    uint32 riders[2] = { 0, 0 };
    for (auto const& entry : _boatRiders)
        ++riders[entry.second.Boat];

    for (Player* bot : bots)
    {
        auto itr = _boatRiders.find(bot->GetGUID());
        if (itr == _boatRiders.end())
        {
            // A bot still on its way to the beach would be put back there.
            if (!bot->IsAlive() || bot->IsBeingTeleported() || bot->GetVehicle())
                continue;

            // The boat with a passenger standing on its deck and the fewest
            // bots aboard.
            int8 boat = -1;
            for (uint8 i = 0; i < 2; ++i)
                if (passengers[i] && !passengers[i]->m_movementInfo.HasMovementFlag(MOVEMENTFLAG_FALLING | MOVEMENTFLAG_FALLING_FAR) &&
                    (boat < 0 || riders[i] < riders[boat]))
                    boat = int8(i);
            if (boat < 0)
                continue;

            BoatRider rider;
            rider.Boat = uint8(boat);
            rider.Offset = GetBoatRiderOffset(passengers[boat]->m_movementInfo.transport.pos, riders[boat]++);
            itr = _boatRiders.emplace(bot->GetGUID(), rider).first;
        }

        PlaceBoatRider(bot, itr->second, passengers[itr->second.Boat]);
    }
}

void BattlegroundSA::PlaceBoatRider(Player* bot, BoatRider& rider, Player const* passenger)
{
    MovementInfo::TransportInfo& transport = bot->m_movementInfo.transport;
    transport.guid = BgObjects[BG_SA_BOAT_ONE + rider.Boat];
    transport.pos.Relocate(rider.Offset);
    transport.seat = -1;
    if (passenger)
        transport.time = passenger->m_movementInfo.transport.time;
    bot->AddUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT);

    if (passenger && bot->movespline->Finalized() && bot->GetExactDist(passenger) > SABoatRiderFollowRange)
        bot->UpdatePosition(passenger->GetPositionX(), passenger->GetPositionY(), passenger->GetPositionZ(),
            passenger->GetOrientation(), true);

    // Its pets ride hidden, kept with it: nothing draws a creature on the deck,
    // and one left on the beach would be dismissed for straying from its master.
    std::vector<Unit*> followers;
    CollectFollowers(bot, followers);
    for (Unit* follower : followers)
    {
        if (std::find(rider.Hidden.begin(), rider.Hidden.end(), follower->GetGUID()) == rider.Hidden.end())
        {
            rider.Hidden.push_back(follower->GetGUID());
            follower->SetVisible(false);
        }

        if (follower->GetExactDist(bot) > SABoatRiderFollowRange)
            follower->NearTeleportTo(bot->GetPosition());
    }

    // Anything the server says about the bot in world coordinates (a facing
    // spline) takes it off the boat on the clients; this puts it back.
    bot->BroadcastServerPlacedMovement();
}

void BattlegroundSA::ReleaseBoatRider(Player* bot, BoatRider const& rider, bool land)
{
    bot->m_movementInfo.transport.Reset();
    bot->RemoveUnitMovementFlag(MOVEMENTFLAG_ONTRANSPORT);

    // Ashore where a bot comes ashore, and off the boat on every client.
    if (land && bot->IsAlive())
        bot->UpdatePosition(SABeachLanding, true);
    bot->BroadcastServerPlacedMovement();

    for (ObjectGuid const& guid : rider.Hidden)
    {
        Unit* follower = ObjectAccessor::GetUnit(*bot, guid);
        if (!follower || !follower->IsInWorld())
            continue;

        if (land)
            follower->NearTeleportTo(bot->GetPosition());
        follower->SetVisible(true);
    }
}

void BattlegroundSA::ReleaseBoatRiders(bool land)
{
    BattlegroundMap* map = FindBgMap();
    for (auto const& entry : _boatRiders)
        if (Player* bot = map ? ObjectAccessor::GetPlayer(map, entry.first) : nullptr)
            if (bot->IsInWorld())
                ReleaseBoatRider(bot, entry.second, land);

    _boatRiders.clear();
}
