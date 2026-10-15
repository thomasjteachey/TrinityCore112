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

#include "BattlegroundIC.h"
#include "GameObject.h"
#include "Log.h"
#include "Map.h"
#include "Player.h"
#include "ScriptedCreature.h"
#include "Transport.h"
#include "Vehicle.h"
#include "WorldPacket.h"
#include "WorldStatePackets.h"

void BattlegroundICScore::BuildObjectivesBlock(WorldPacket& data)
{
    data << uint32(2); // Objectives Count
    data << uint32(BasesAssaulted);
    data << uint32(BasesDefended);
}

BattlegroundIC::BattlegroundIC()
{
    BgObjects.resize(MAX_NORMAL_GAMEOBJECTS_SPAWNS + MAX_AIRSHIPS_SPAWNS + MAX_HANGAR_TELEPORTERS_SPAWNS + MAX_FORTRESS_TELEPORTERS_SPAWNS + MAX_HANGAR_TELEPORTER_EFFECTS_SPAWNS + MAX_FORTRESS_TELEPORTER_EFFECTS_SPAWNS);
    BgCreatures.resize(MAX_NORMAL_NPCS_SPAWNS + MAX_WORKSHOP_SPAWNS + MAX_DOCKS_SPAWNS + MAX_SPIRIT_GUIDES_SPAWNS + MAX_HANGAR_NPCS_SPAWNS);

    for (uint8 i = 0; i < 2; ++i)
        factionReinforcements[i] = MAX_REINFORCEMENTS;

    for (uint8 i = 0; i < BG_IC_MAXDOOR; ++i)
        GateStatus[i] = BG_IC_GATE_OK;

    closeFortressDoorsTimer = CLOSE_DOORS_TIME; // the doors are closed again... in a special way
    doorsClosed = false;
    docksTimer = DOCKS_UPDATE_TIME;
    resourceTimer = IC_RESOURCE_TIME;

    for (uint8 i = NODE_TYPE_REFINERY; i < MAX_NODE_TYPES; ++i)
        nodePoint[i] = nodePointInitial[i];

    siegeEngineWorkshopTimer = WORKSHOP_UPDATE_TIME;

    gunshipHorde = nullptr;
    gunshipAlliance = nullptr;
}

BattlegroundIC::~BattlegroundIC() { }

void BattlegroundIC::HandlePlayerResurrect(Player* player)
{
    if (nodePoint[NODE_TYPE_QUARRY].nodeState == (player->GetTeamId() == TEAM_ALLIANCE ? NODE_STATE_CONTROLLED_A : NODE_STATE_CONTROLLED_H))
        player->CastSpell(player, SPELL_QUARRY, true);

    if (nodePoint[NODE_TYPE_REFINERY].nodeState == (player->GetTeamId() == TEAM_ALLIANCE ? NODE_STATE_CONTROLLED_A : NODE_STATE_CONTROLLED_H))
        player->CastSpell(player, SPELL_OIL_REFINERY, true);
}

void BattlegroundIC::PostUpdateImpl(uint32 diff)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    if (!doorsClosed)
    {
        if (closeFortressDoorsTimer <= diff)
        {
            RemoveObjectFromWorld(BG_IC_GO_DOODAD_ND_HUMAN_GATE_CLOSEDFX_DOOR01);
            RemoveObjectFromWorld(BG_IC_GO_DOODAD_ND_WINTERORC_WALL_GATEFX_DOOR01);
            RemoveObjectFromWorld(BG_IC_GO_DOODAD_ND_HUMAN_GATE_CLOSEDFX_DOOR02);
            RemoveObjectFromWorld(BG_IC_GO_DOODAD_ND_WINTERORC_WALL_GATEFX_DOOR02);
            RemoveObjectFromWorld(BG_IC_GO_DOODAD_ND_HUMAN_GATE_CLOSEDFX_DOOR03);
            RemoveObjectFromWorld(BG_IC_GO_DOODAD_ND_WINTERORC_WALL_GATEFX_DOOR03);

            GetBGObject(BG_IC_GO_ALLIANCE_GATE_1)->SetDestructibleState(GO_DESTRUCTIBLE_DAMAGED);
            GetBGObject(BG_IC_GO_HORDE_GATE_1)->SetDestructibleState(GO_DESTRUCTIBLE_DAMAGED);
            GetBGObject(BG_IC_GO_ALLIANCE_GATE_2)->SetDestructibleState(GO_DESTRUCTIBLE_DAMAGED);
            GetBGObject(BG_IC_GO_HORDE_GATE_2)->SetDestructibleState(GO_DESTRUCTIBLE_DAMAGED);
            GetBGObject(BG_IC_GO_ALLIANCE_GATE_3)->SetDestructibleState(GO_DESTRUCTIBLE_DAMAGED);
            GetBGObject(BG_IC_GO_HORDE_GATE_3)->SetDestructibleState(GO_DESTRUCTIBLE_DAMAGED);

            doorsClosed = true;
        } else closeFortressDoorsTimer -= diff;
    }

    for (uint8 i = NODE_TYPE_REFINERY; i < MAX_NODE_TYPES; ++i)
    {
        if (nodePoint[i].nodeType == NODE_TYPE_DOCKS)
        {
            if (nodePoint[i].nodeState == NODE_STATE_CONTROLLED_A ||
                nodePoint[i].nodeState == NODE_STATE_CONTROLLED_H)
            {
                if (docksTimer <= diff)
                {
                    // we need to confirm this, i am not sure if this every 3 minutes
                    for (uint8 u = (nodePoint[i].faction == TEAM_ALLIANCE ? BG_IC_NPC_CATAPULT_1_A : BG_IC_NPC_CATAPULT_1_H); u < (nodePoint[i].faction  == TEAM_ALLIANCE ? BG_IC_NPC_CATAPULT_4_A : BG_IC_NPC_CATAPULT_4_H); ++u)
                    {
                        if (Creature* catapult = GetBGCreature(u))
                        {
                            if (!catapult->IsAlive())
                                catapult->Respawn(true);
                        }
                    }

                    // we need to confirm this is blizzlike, not sure if it is every 3 minutes
                    for (uint8 u = (nodePoint[i].faction == TEAM_ALLIANCE ? BG_IC_NPC_GLAIVE_THROWER_1_A : BG_IC_NPC_GLAIVE_THROWER_1_H); u < (nodePoint[i].faction == TEAM_ALLIANCE ? BG_IC_NPC_GLAIVE_THROWER_2_A : BG_IC_NPC_GLAIVE_THROWER_2_H); ++u)
                    {
                        if (Creature* glaiveThrower = GetBGCreature(u))
                        {
                            if (!glaiveThrower->IsAlive())
                                glaiveThrower->Respawn(true);
                        }
                    }

                    docksTimer = DOCKS_UPDATE_TIME;
                } else docksTimer -= diff;
            }
        }

        if (nodePoint[i].nodeType == NODE_TYPE_WORKSHOP)
        {
            if (nodePoint[i].nodeState == NODE_STATE_CONTROLLED_A ||
                nodePoint[i].nodeState == NODE_STATE_CONTROLLED_H)
            {
                if (siegeEngineWorkshopTimer <= diff)
                {
                    uint8 siegeType = (nodePoint[i].faction == TEAM_ALLIANCE ? BG_IC_NPC_SIEGE_ENGINE_A : BG_IC_NPC_SIEGE_ENGINE_H);

                    if (Creature* siege = GetBGCreature(siegeType)) // this always should be true
                    {
                        if (siege->IsAlive())
                        {
                            if (siege->HasUnitFlag(UNIT_FLAG_UNINTERACTIBLE | UNIT_FLAG_CANNOT_SWIM | UNIT_FLAG_IMMUNE_TO_PC))
                                // following sniffs the vehicle always has UNIT_FLAG_CANNOT_SWIM
                                siege->RemoveUnitFlag(UNIT_FLAG_UNINTERACTIBLE | UNIT_FLAG_IMMUNE_TO_PC);
                            else
                                siege->SetHealth(siege->GetMaxHealth());
                        }
                        else
                            siege->Respawn(true);
                    }

                    // we need to confirm if it is every 3 minutes
                    for (uint8 u = (nodePoint[i].faction == TEAM_ALLIANCE ? BG_IC_NPC_DEMOLISHER_1_A : BG_IC_NPC_DEMOLISHER_1_H); u < (nodePoint[i].faction == TEAM_ALLIANCE ? BG_IC_NPC_DEMOLISHER_4_A : BG_IC_NPC_DEMOLISHER_4_H); ++u)
                    {
                        if (Creature* demolisher = GetBGCreature(u))
                        {
                            if (!demolisher->IsAlive())
                                demolisher->Respawn(true);
                        }
                    }
                    siegeEngineWorkshopTimer = WORKSHOP_UPDATE_TIME;
                } else siegeEngineWorkshopTimer -= diff;
            }
        }

        // the point is waiting for a change on its banner
        if (nodePoint[i].needChange)
        {
            if (nodePoint[i].timer <= diff)
            {
                uint32 nextBanner = GetNextBanner(&nodePoint[i], nodePoint[i].faction, true);

                nodePoint[i].last_entry = nodePoint[i].gameobject_entry;
                nodePoint[i].gameobject_entry = nextBanner;
                // nodePoint[i].faction = the faction should be the same one...

                GameObject* banner = GetBGObject(nodePoint[i].gameobject_type);

                if (!banner) // this should never happen
                    return;

                float cords[4] = {banner->GetPositionX(), banner->GetPositionY(), banner->GetPositionZ(), banner->GetOrientation() };

                DelObject(nodePoint[i].gameobject_type);
                AddObject(nodePoint[i].gameobject_type, nodePoint[i].gameobject_entry, cords[0], cords[1], cords[2], cords[3], 0, 0, 0, 0, RESPAWN_IMMEDIATELY);

                GetBGObject(nodePoint[i].gameobject_type)->SetFaction(nodePoint[i].faction == TEAM_ALLIANCE ? BG_IC_Factions[1] : BG_IC_Factions[0]);

                UpdateNodeWorldState(&nodePoint[i]);
                HandleCapturedNodes(&nodePoint[i], false);

                if (nodePoint[i].faction == TEAM_ALLIANCE)
                    SendBroadcastText(ICNodes[i].TextAllianceTaken, CHAT_MSG_BG_SYSTEM_ALLIANCE);
                else
                    SendBroadcastText(ICNodes[i].TextHordeTaken, CHAT_MSG_BG_SYSTEM_HORDE);

                nodePoint[i].needChange = false;
                nodePoint[i].timer = BANNER_STATE_CHANGE_TIME;
            } else nodePoint[i].timer -= diff;
        }
    }

    if (resourceTimer <= diff)
    {
        for (uint8 i = 0; i < NODE_TYPE_DOCKS; ++i)
        {
            if (nodePoint[i].nodeState == NODE_STATE_CONTROLLED_A ||
                nodePoint[i].nodeState == NODE_STATE_CONTROLLED_H)
            {
                factionReinforcements[nodePoint[i].faction] += 1;
                RewardHonorToTeam(RESOURCE_HONOR_AMOUNT, nodePoint[i].faction == TEAM_ALLIANCE ? ALLIANCE : HORDE);
                UpdateWorldState((nodePoint[i].faction == TEAM_ALLIANCE ? BG_IC_ALLIANCE_RENFORT : BG_IC_HORDE_RENFORT), factionReinforcements[nodePoint[i].faction]);
            }
        }
        resourceTimer = IC_RESOURCE_TIME;
    } else resourceTimer -= diff;
}

void BattlegroundIC::StartingEventCloseDoors()
{
}

void BattlegroundIC::StartingEventOpenDoors()
{
    //after 20 seconds they should be despawned
    DoorOpen(BG_IC_GO_DOODAD_ND_HUMAN_GATE_CLOSEDFX_DOOR01);
    DoorOpen(BG_IC_GO_DOODAD_ND_WINTERORC_WALL_GATEFX_DOOR01);
    DoorOpen(BG_IC_GO_DOODAD_ND_HUMAN_GATE_CLOSEDFX_DOOR02);
    DoorOpen(BG_IC_GO_DOODAD_ND_WINTERORC_WALL_GATEFX_DOOR02);
    DoorOpen(BG_IC_GO_DOODAD_ND_HUMAN_GATE_CLOSEDFX_DOOR03);
    DoorOpen(BG_IC_GO_DOODAD_ND_WINTERORC_WALL_GATEFX_DOOR03);

    DoorOpen(BG_IC_GO_DOODAD_HU_PORTCULLIS01_1);
    DoorOpen(BG_IC_GO_DOODAD_HU_PORTCULLIS01_2);
    DoorOpen(BG_IC_GO_DOODAD_VR_PORTCULLIS01_1);
    DoorOpen(BG_IC_GO_DOODAD_VR_PORTCULLIS01_2);

    for (uint8 i = 0; i < MAX_FORTRESS_TELEPORTERS_SPAWNS; ++i)
        GetBGObject(BG_IC_Teleporters[i].type)->RemoveFlag(GO_FLAG_NOT_SELECTABLE);

    for (uint8 i = 0; i < MAX_FORTRESS_TELEPORTER_EFFECTS_SPAWNS; ++i)
        GetBGObject(BG_IC_TeleporterEffects[i].type)->SetGoState(GO_STATE_ACTIVE);
}

void BattlegroundIC::AddPlayer(Player* player)
{
    bool const isInBattleground = IsPlayerInBattleground(player->GetGUID());
    Battleground::AddPlayer(player);
    if (!isInBattleground)
        PlayerScores[player->GetGUID().GetCounter()] = new BattlegroundICScore(player->GetGUID());

    if (nodePoint[NODE_TYPE_QUARRY].nodeState == (player->GetTeamId() == TEAM_ALLIANCE ? NODE_STATE_CONTROLLED_A : NODE_STATE_CONTROLLED_H))
        player->CastSpell(player, SPELL_QUARRY, true);

    if (nodePoint[NODE_TYPE_REFINERY].nodeState == (player->GetTeamId() == TEAM_ALLIANCE ? NODE_STATE_CONTROLLED_A : NODE_STATE_CONTROLLED_H))
        player->CastSpell(player, SPELL_OIL_REFINERY, true);
}

void BattlegroundIC::RemovePlayer(Player* player, ObjectGuid /*guid*/, uint32 /*team*/)
{
    if (player)
    {
        player->RemoveAura(SPELL_QUARRY);
        player->RemoveAura(SPELL_OIL_REFINERY);
    }
}

void BattlegroundIC::HandleAreaTrigger(Player* player, uint32 trigger)
{
    // this is wrong way to implement these things. On official it done by gameobject spell cast.
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    /// @hack: this spell should be cast by npc 22515 (World Trigger) and not by the player
    if (trigger == 5555 && player->GetTeamId() == TEAM_HORDE)
    {
        if (GateStatus[BG_IC_A_FRONT] != BG_IC_GATE_DESTROYED
            && GateStatus[BG_IC_A_WEST] != BG_IC_GATE_DESTROYED
            && GateStatus[BG_IC_A_EAST] != BG_IC_GATE_DESTROYED)
        player->CastSpell(player, SPELL_BACK_DOOR_JOB_ACHIEVEMENT, true);
    }
    else if (trigger == 5535 && player->GetTeamId() == TEAM_ALLIANCE)
    {
        if (GateStatus[BG_IC_H_FRONT] != BG_IC_GATE_DESTROYED
            && GateStatus[BG_IC_H_WEST] != BG_IC_GATE_DESTROYED
            && GateStatus[BG_IC_H_EAST] != BG_IC_GATE_DESTROYED)
        player->CastSpell(player, SPELL_BACK_DOOR_JOB_ACHIEVEMENT, true);
    }
}

void BattlegroundIC::FillInitialWorldStates(WorldPackets::WorldState::InitWorldStates& packet)
{
    packet.Worldstates.emplace_back(BG_IC_ALLIANCE_RENFORT_SET, 1);
    packet.Worldstates.emplace_back(BG_IC_HORDE_RENFORT_SET, 1);
    packet.Worldstates.emplace_back(BG_IC_ALLIANCE_RENFORT, factionReinforcements[TEAM_ALLIANCE]);
    packet.Worldstates.emplace_back(BG_IC_HORDE_RENFORT, factionReinforcements[TEAM_HORDE]);

    for (uint8 itr = 0; itr < MAX_FORTRESS_GATES_SPAWNS; ++itr)
    {
        int32 worldState = GetWorldStateFromGateEntry(BG_IC_ObjSpawnlocs[itr].entry, (GateStatus[GetGateIDFromEntry(BG_IC_ObjSpawnlocs[itr].entry)] == BG_IC_GATE_DESTROYED ? true : false));
        packet.Worldstates.emplace_back(worldState, 1);
    }

    for (uint8 itr = 0; itr < MAX_NODE_TYPES; ++itr)
        packet.Worldstates.emplace_back(nodePoint[itr].worldStates[nodePoint[itr].nodeState], 1);
}

// Every object here is placed live. Battleground::AddObject now honours its
// respawn time (an object added with one starts despawned and waits it out),
// where it used to ignore it; Isle of Conquest never calls SpawnBGObject, so
// with RESPAWN_ONE_DAY its banners, teleporters and doors never appeared.
bool BattlegroundIC::SetupBattleground()
{
    for (uint8 i = 0; i < MAX_NORMAL_GAMEOBJECTS_SPAWNS; ++i)
    {
        if (!AddObject(BG_IC_ObjSpawnlocs[i].type, BG_IC_ObjSpawnlocs[i].entry, BG_IC_ObjSpawnlocs[i].x, BG_IC_ObjSpawnlocs[i].y, BG_IC_ObjSpawnlocs[i].z, BG_IC_ObjSpawnlocs[i].o, 0, 0, 0, 0, RESPAWN_IMMEDIATELY))
        {
            TC_LOG_ERROR("bg.battleground", "Isle of Conquest: There was an error spawning gameobject {}", BG_IC_ObjSpawnlocs[i].entry);
            return false;
        }
    }

    for (uint8 i = 0; i < MAX_FORTRESS_TELEPORTERS_SPAWNS; ++i)
    {
        if (!AddObject(BG_IC_Teleporters[i].type, BG_IC_Teleporters[i].entry, BG_IC_Teleporters[i].x, BG_IC_Teleporters[i].y, BG_IC_Teleporters[i].z, BG_IC_Teleporters[i].o, 0, 0, 0, 0, RESPAWN_IMMEDIATELY))
        {
            TC_LOG_ERROR("bg.battleground", "Isle of Conquest | Starting Event Open Doors: There was an error spawning gameobject {}", BG_IC_Teleporters[i].entry);
            return false;
        }
    }

    for (uint8 i = 0; i < MAX_FORTRESS_TELEPORTER_EFFECTS_SPAWNS; ++i)
    {
        if (!AddObject(BG_IC_TeleporterEffects[i].type, BG_IC_TeleporterEffects[i].entry, BG_IC_TeleporterEffects[i].x, BG_IC_TeleporterEffects[i].y, BG_IC_TeleporterEffects[i].z, BG_IC_TeleporterEffects[i].o, 0, 0, 0, 0, RESPAWN_IMMEDIATELY))
        {
            TC_LOG_ERROR("bg.battleground", "Isle of Conquest | Starting Event Open Doors: There was an error spawning gameobject {}", BG_IC_Teleporters[i].entry);
            return false;
        }
    }

    for (uint8 i = 2; i < MAX_NORMAL_NPCS_SPAWNS; ++i)
    {
        if (!AddCreature(BG_IC_NpcSpawnlocs[i].entry, BG_IC_NpcSpawnlocs[i].type, BG_IC_NpcSpawnlocs[i].x, BG_IC_NpcSpawnlocs[i].y, BG_IC_NpcSpawnlocs[i].z, BG_IC_NpcSpawnlocs[i].o, BG_IC_NpcSpawnlocs[i].team, RESPAWN_ONE_DAY))
        {
            TC_LOG_ERROR("bg.battleground", "Isle of Conquest: There was an error spawning creature {}", BG_IC_NpcSpawnlocs[i].entry);
            return false;
        }
    }

    if (!AddSpiritGuide(BG_IC_NPC_SPIRIT_GUIDE_1+3, BG_IC_SpiritGuidePos[5], TEAM_ALLIANCE)
        || !AddSpiritGuide(BG_IC_NPC_SPIRIT_GUIDE_1+4, BG_IC_SpiritGuidePos[6], TEAM_HORDE)
        || !AddSpiritGuide(BG_IC_NPC_SPIRIT_GUIDE_1+5, BG_IC_SpiritGuidePos[7], TEAM_ALLIANCE)
        || !AddSpiritGuide(BG_IC_NPC_SPIRIT_GUIDE_1+6, BG_IC_SpiritGuidePos[8], TEAM_HORDE))
    {
        TC_LOG_ERROR("bg.battleground", "Isle of Conquest: Failed to spawn initial spirit guide!");
        return false;
    }

    gunshipHorde = sTransportMgr->CreateTransport(GO_HORDE_GUNSHIP, 0, GetBgMap());
    gunshipAlliance = sTransportMgr->CreateTransport(GO_ALLIANCE_GUNSHIP, 0, GetBgMap());

    if (!gunshipAlliance || !gunshipHorde)
    {
        TC_LOG_ERROR("bg.battleground", "Isle of Conquest: There was an error creating gunships!");
        return false;
    }

    gunshipHorde->EnableMovement(false);
    gunshipAlliance->EnableMovement(false);

    // setting correct factions for Keep Cannons
    for (uint8 i = BG_IC_NPC_KEEP_CANNON_1; i <= BG_IC_NPC_KEEP_CANNON_12; ++i)
        GetBGCreature(i)->SetFaction(BG_IC_Factions[0]);
    for (uint8 i = BG_IC_NPC_KEEP_CANNON_13; i <= BG_IC_NPC_KEEP_CANNON_24; ++i)
        GetBGCreature(i)->SetFaction(BG_IC_Factions[1]);

    // correcting spawn time for keeps bombs
    for (uint8 i = BG_IC_GO_HUGE_SEAFORIUM_BOMBS_A_1; i < BG_IC_GO_HUGE_SEAFORIUM_BOMBS_H_4; ++i)
        GetBGObject(i)->SetRespawnTime(10);

    return true;
}

void BattlegroundIC::HandleKillUnit(Creature* unit, Player* killer)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
       return;

    uint32 entry = unit->GetEntry();
    if (entry == NPC_HIGH_COMMANDER_HALFORD_WYRMBANE)
    {
        RewardHonorToTeam(WINNER_HONOR_AMOUNT, HORDE);
        EndBattleground(HORDE);
    }
    else if (entry == NPC_OVERLORD_AGMAR)
    {
        RewardHonorToTeam(WINNER_HONOR_AMOUNT, ALLIANCE);
        EndBattleground(ALLIANCE);
    }

    //Achievement Mowed Down
    // TO-DO: This should be done on the script of each vehicle of the BG.
    if (unit->IsVehicle())
        killer->CastSpell(killer, SPELL_DESTROYED_VEHICLE_ACHIEVEMENT, true);
}

void BattlegroundIC::HandleKillPlayer(Player* player, Player* killer)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    Battleground::HandleKillPlayer(player, killer);

    factionReinforcements[player->GetTeamId()] -= 1;

    UpdateWorldState((player->GetTeamId() == TEAM_ALLIANCE ? BG_IC_ALLIANCE_RENFORT : BG_IC_HORDE_RENFORT), factionReinforcements[player->GetTeamId()]);

    // we must end the battleground
    if (factionReinforcements[player->GetTeamId()] < 1)
        EndBattleground(GetOtherTeam(player->GetBGTeam()));
}

void BattlegroundIC::EventPlayerClickedOnFlag(Player* player, GameObject* target_obj)
{
    if (GetStatus() != STATUS_IN_PROGRESS)
        return;

    // All the node points are iterated to find the clicked one
    for (uint8 i = 0; i < MAX_NODE_TYPES; ++i)
    {
        if (nodePoint[i].gameobject_entry == target_obj->GetEntry())
        {
            // THIS SHOULD NEEVEER HAPPEN
            if (nodePoint[i].faction == player->GetTeamId())
                return;

            uint32 nextBanner = GetNextBanner(&nodePoint[i], player->GetTeamId(), false);

            // we set the new settings of the nodePoint
            nodePoint[i].faction = player->GetTeamId();
            nodePoint[i].last_entry = nodePoint[i].gameobject_entry;
            nodePoint[i].gameobject_entry = nextBanner;

            // this is just needed if the next banner is grey
            if (nodePoint[i].banners[BANNER_A_CONTESTED] == nextBanner || nodePoint[i].banners[BANNER_H_CONTESTED] == nextBanner)
            {
                nodePoint[i].timer = BANNER_STATE_CHANGE_TIME; // 1 minute for last change (real faction banner)
                nodePoint[i].needChange = true;

                RelocateDeadPlayers(BgCreatures[BG_IC_NPC_SPIRIT_GUIDE_1 + uint32(nodePoint[i].nodeType) - 2]);

                // if we are here means that the point has been lost, or it is the first capture

                if (nodePoint[i].nodeType != NODE_TYPE_REFINERY && nodePoint[i].nodeType != NODE_TYPE_QUARRY)
                    if (!BgCreatures[BG_IC_NPC_SPIRIT_GUIDE_1 + uint32(nodePoint[i].nodeType) - 2].IsEmpty())
                        DelCreature(BG_IC_NPC_SPIRIT_GUIDE_1 + uint32(nodePoint[i].nodeType) - 2);

                UpdatePlayerScore(player, SCORE_BASES_ASSAULTED, 1);

                if (nodePoint[i].faction == TEAM_ALLIANCE)
                    SendBroadcastText(ICNodes[i].TextAssaulted, CHAT_MSG_BG_SYSTEM_ALLIANCE, player);
                else
                    SendBroadcastText(ICNodes[i].TextAssaulted, CHAT_MSG_BG_SYSTEM_HORDE, player);

                HandleContestedNodes(&nodePoint[i]);
            }
            else if (nextBanner == nodePoint[i].banners[BANNER_A_CONTROLLED] || nextBanner == nodePoint[i].banners[BANNER_H_CONTROLLED]) // if we are going to spawn the definitve faction banner, we dont need the timer anymore
            {
                nodePoint[i].timer = BANNER_STATE_CHANGE_TIME;
                nodePoint[i].needChange = false;

                if (nodePoint[i].faction == TEAM_ALLIANCE)
                    SendBroadcastText(ICNodes[i].TextDefended, CHAT_MSG_BG_SYSTEM_ALLIANCE, player);
                else
                    SendBroadcastText(ICNodes[i].TextDefended, CHAT_MSG_BG_SYSTEM_HORDE, player);

                HandleCapturedNodes(&nodePoint[i], true);
                UpdatePlayerScore(player, SCORE_BASES_DEFENDED, 1);
            }

            GameObject* banner = GetBGObject(nodePoint[i].gameobject_type);

            if (!banner) // this should never happen
                return;

            float cords[4] = {banner->GetPositionX(), banner->GetPositionY(), banner->GetPositionZ(), banner->GetOrientation() };

            DelObject(nodePoint[i].gameobject_type);
            if (!AddObject(nodePoint[i].gameobject_type, nodePoint[i].gameobject_entry, cords[0], cords[1], cords[2], cords[3], 0, 0, 0, 0, RESPAWN_IMMEDIATELY))
            {
                TC_LOG_ERROR("bg.battleground", "Isle of Conquest: There was an error spawning a banner (type: {}, entry: {}). Isle of Conquest BG cancelled.", nodePoint[i].gameobject_type, nodePoint[i].gameobject_entry);
                EndBattleground(0);
            }

            GetBGObject(nodePoint[i].gameobject_type)->SetFaction(nodePoint[i].faction == TEAM_ALLIANCE ? BG_IC_Factions[1] : BG_IC_Factions[0]);

            UpdateNodeWorldState(&nodePoint[i]);
            // we dont need iterating if we are here
            // If the needChange bool was set true, we will handle the rest in the Update Map function.
            return;
        }
    }
}

void BattlegroundIC::UpdateNodeWorldState(ICNodePoint* node)
{
    //updating worldstate
    if (node->gameobject_entry == node->banners[BANNER_A_CONTROLLED])
        node->nodeState = NODE_STATE_CONTROLLED_A;
    else if (node->gameobject_entry == node->banners[BANNER_A_CONTESTED])
        node->nodeState = NODE_STATE_CONFLICT_A;
    else if (node->gameobject_entry == node->banners[BANNER_H_CONTROLLED])
        node->nodeState = NODE_STATE_CONTROLLED_H;
    else if (node->gameobject_entry == node->banners[BANNER_H_CONTESTED])
        node->nodeState = NODE_STATE_CONFLICT_H;

    uint32 worldstate = node->worldStates[node->nodeState];

    // with this we are sure we dont bug the client
    for (uint8 i = 0; i < 5; ++i)
    {
        if (node->worldStates[i] == worldstate)
            continue;
        UpdateWorldState(node->worldStates[i], 0);
    }

    UpdateWorldState(worldstate, 1);
}

uint32 BattlegroundIC::GetNextBanner(ICNodePoint* node, uint32 team, bool returnDefinitve)
{
    // this is only used in the update map function
    if (returnDefinitve)
        // here is a special case, here we must return the definitve faction banner after the grey banner was spawned 1 minute
        return node->banners[(team == TEAM_ALLIANCE ? BANNER_A_CONTROLLED : BANNER_H_CONTROLLED)];

    // there were no changes, this point has never been captured by any faction or at least clicked
    if (node->last_entry == 0)
        // 1 returns the CONTESTED ALLIANCE BANNER, 3 returns the HORDE one
        return node->banners[(team == TEAM_ALLIANCE ? BANNER_A_CONTESTED : BANNER_H_CONTESTED)];

    // If the actual banner is the definitive faction banner, we must return the grey banner of the player's faction
    if (node->gameobject_entry == node->banners[BANNER_A_CONTROLLED] || node->gameobject_entry == node->banners[BANNER_H_CONTROLLED])
        return node->banners[(team == TEAM_ALLIANCE ? BANNER_A_CONTESTED : BANNER_H_CONTESTED)];

    // If the actual banner is the grey faction banner, we must return the previous banner
    if (node->gameobject_entry == node->banners[BANNER_A_CONTESTED] || node->banners[BANNER_H_CONTESTED])
        return node->last_entry;

    // we should never be here...
    TC_LOG_ERROR("bg.battleground", "Isle Of Conquest: Unexpected return in GetNextBanner function");
    return 0;
}

void BattlegroundIC::HandleContestedNodes(ICNodePoint* node)
{
    if (node->nodeType == NODE_TYPE_HANGAR)
    {
        if (gunshipAlliance && gunshipHorde)
            (node->faction == TEAM_ALLIANCE ? gunshipHorde : gunshipAlliance)->EnableMovement(false);

        for (uint8 u = BG_IC_GO_HANGAR_TELEPORTER_1; u <= BG_IC_GO_HANGAR_TELEPORTER_3; ++u)
            DelObject(u);

        for (uint8 u = BG_IC_GO_HANGAR_TELEPORTER_EFFECT_1; u <= BG_IC_GO_HANGAR_TELEPORTER_EFFECT_3; ++u)
            DelObject(u);

        DelCreature(BG_IC_NPC_WORLD_TRIGGER_NOT_FLOATING);

        for (uint8 u = 0; u < MAX_CAPTAIN_SPAWNS_PER_FACTION; ++u)
        {
            uint8 type = BG_IC_NPC_GUNSHIP_CAPTAIN_1 + u;
            DelCreature(type);
        }

        std::list<Creature*> cannons;
        if (node->faction == TEAM_HORDE)
            gunshipAlliance->GetCreatureListWithEntryInGrid(cannons, NPC_ALLIANCE_GUNSHIP_CANNON, 150.0f);
        else
            gunshipHorde->GetCreatureListWithEntryInGrid(cannons, NPC_HORDE_GUNSHIP_CANNON, 150.0f);

        for (Creature* cannon : cannons)
        {
            cannon->GetVehicleKit()->RemoveAllPassengers();
            cannon->SetUnitFlag(UNIT_FLAG_UNINTERACTIBLE);
        }
    }
    else if (node->nodeType == NODE_TYPE_WORKSHOP)
    {
        DelObject(BG_IC_GO_SEAFORIUM_BOMBS_1);
        DelObject(BG_IC_GO_SEAFORIUM_BOMBS_2);
    }
}

void BattlegroundIC::HandleCapturedNodes(ICNodePoint* node, bool recapture)
{
    if (node->nodeType != NODE_TYPE_REFINERY && node->nodeType != NODE_TYPE_QUARRY)
    {
        if (!AddSpiritGuide(BG_IC_NPC_SPIRIT_GUIDE_1 + uint32(node->nodeType) - 2, BG_IC_SpiritGuidePos[node->nodeType], node->faction))
            TC_LOG_ERROR("bg.battleground", "Isle of Conquest: Failed to spawn spirit guide! point: {}, team: {}, ", node->nodeType, node->faction);
    }

    switch (node->gameobject_type)
    {
        case BG_IC_GO_HANGAR_BANNER:
            {
                if (!gunshipAlliance || !gunshipHorde)
                    break;

                std::list<Creature*> cannons;
                if (node->faction == TEAM_ALLIANCE)
                    gunshipAlliance->GetCreatureListWithEntryInGrid(cannons, NPC_ALLIANCE_GUNSHIP_CANNON, 150.0f);
                else
                    gunshipHorde->GetCreatureListWithEntryInGrid(cannons, NPC_HORDE_GUNSHIP_CANNON, 150.0f);

                for (Creature* cannon : cannons)
                    cannon->RemoveUnitFlag(UNIT_FLAG_UNINTERACTIBLE);

                for (uint8 u = 0; u < MAX_HANGAR_TELEPORTERS_SPAWNS; ++u)
                {
                    uint8 type = BG_IC_GO_HANGAR_TELEPORTER_1 + u;
                    if (!AddObject(type, (node->faction == TEAM_ALLIANCE ? GO_ALLIANCE_GUNSHIP_PORTAL : GO_HORDE_GUNSHIP_PORTAL), BG_IC_HangarTeleporters[u], 0, 0, 0, 0, RESPAWN_IMMEDIATELY))
                        TC_LOG_ERROR("bg.battleground", "Isle of Conquest: There was an error spawning a gunship portal. Type: {}", BG_IC_GO_HANGAR_TELEPORTER_1 + u);
                }

                for (uint8 u = 0; u < MAX_HANGAR_TELEPORTER_EFFECTS_SPAWNS; ++u)
                {
                    uint8 type = BG_IC_GO_HANGAR_TELEPORTER_EFFECT_1 + u;
                    if (!AddObject(type, (node->faction == TEAM_ALLIANCE ? GO_ALLIANCE_GUNSHIP_PORTAL_EFFECTS : GO_HORDE_GUNSHIP_PORTAL_EFFECTS), BG_IC_HangarTeleporterEffects[u], 0, 0, 0, 0, RESPAWN_IMMEDIATELY, GO_STATE_ACTIVE))
                        TC_LOG_ERROR("bg.battleground", "Isle of Conquest: There was an error spawning a gunship portal effects. Type: {}", BG_IC_GO_HANGAR_TELEPORTER_1 + u);
                }

                for (uint8 u = 0; u < MAX_TRIGGER_SPAWNS_PER_FACTION; ++u)
                {
                    if (!AddCreature(NPC_WORLD_TRIGGER_NOT_FLOATING, BG_IC_NPC_WORLD_TRIGGER_NOT_FLOATING, BG_IC_HangarTrigger[node->faction], node->faction, RESPAWN_ONE_DAY, node->faction == TEAM_ALLIANCE ? gunshipAlliance : gunshipHorde))
                        TC_LOG_ERROR("bg.battleground", "Isle of Conquest: There was an error spawning a world trigger. Type: {}", BG_IC_NPC_WORLD_TRIGGER_NOT_FLOATING);
                }

                for (uint8 u = 0; u < MAX_CAPTAIN_SPAWNS_PER_FACTION; ++u)
                {
                    uint8 type = BG_IC_NPC_GUNSHIP_CAPTAIN_1 + u;

                    if (type == BG_IC_NPC_GUNSHIP_CAPTAIN_1)
                        if (AddCreature(node->faction == TEAM_ALLIANCE ? NPC_ALLIANCE_GUNSHIP_CAPTAIN : NPC_HORDE_GUNSHIP_CAPTAIN, type, BG_IC_HangarCaptains[node->faction == TEAM_ALLIANCE ? 2 : 0], node->faction, RESPAWN_ONE_DAY))
                            GetBGCreature(BG_IC_NPC_GUNSHIP_CAPTAIN_1)->GetAI()->DoAction(ACTION_GUNSHIP_READY);

                    if (type == BG_IC_NPC_GUNSHIP_CAPTAIN_2)
                        if (!AddCreature(node->faction == TEAM_ALLIANCE ? NPC_ALLIANCE_GUNSHIP_CAPTAIN : NPC_HORDE_GUNSHIP_CAPTAIN, type, BG_IC_HangarCaptains[node->faction == TEAM_ALLIANCE ? 3 : 1], node->faction, RESPAWN_ONE_DAY, node->faction == TEAM_ALLIANCE ? gunshipAlliance : gunshipHorde))
                            TC_LOG_ERROR("bg.battleground", "Isle of Conquest: There was an error spawning a world trigger. Type: {}", BG_IC_NPC_GUNSHIP_CAPTAIN_2);
                }

                (node->faction == TEAM_ALLIANCE ? gunshipAlliance : gunshipHorde)->EnableMovement(true);
                break;
            }
        case BG_IC_GO_QUARRY_BANNER:
            RemoveAuraOnTeam(SPELL_QUARRY, (node->faction == TEAM_ALLIANCE ? HORDE : ALLIANCE));
            CastSpellOnTeam(SPELL_QUARRY, (node->faction == TEAM_ALLIANCE ? ALLIANCE : HORDE));
            break;
        case BG_IC_GO_REFINERY_BANNER:
            RemoveAuraOnTeam(SPELL_OIL_REFINERY, (node->faction == TEAM_ALLIANCE ? HORDE : ALLIANCE));
            CastSpellOnTeam(SPELL_OIL_REFINERY, (node->faction == TEAM_ALLIANCE ? ALLIANCE : HORDE));
            break;
        case BG_IC_GO_DOCKS_BANNER:
            if (recapture)
                break;

            if (docksTimer < DOCKS_UPDATE_TIME)
                docksTimer = DOCKS_UPDATE_TIME;

            // we must del opposing faction vehicles when the node is captured (unused ones)
            for (uint8 i = (node->faction == TEAM_ALLIANCE ? BG_IC_NPC_GLAIVE_THROWER_1_H : BG_IC_NPC_GLAIVE_THROWER_1_A); i < (node->faction == TEAM_ALLIANCE ? BG_IC_NPC_GLAIVE_THROWER_2_H : BG_IC_NPC_GLAIVE_THROWER_2_A); ++i)
            {
                if (Creature* glaiveThrower = GetBGCreature(i, false))
                {
                    if (Vehicle* vehicleGlaive = glaiveThrower->GetVehicleKit())
                    {
                        if (!vehicleGlaive->GetPassenger(0))
                            DelCreature(i);
                    }
                }
            }

            for (uint8 i = (node->faction == TEAM_ALLIANCE ? BG_IC_NPC_CATAPULT_1_H : BG_IC_NPC_CATAPULT_1_A); i < (node->faction == TEAM_ALLIANCE ? BG_IC_NPC_CATAPULT_4_H  : BG_IC_NPC_CATAPULT_4_A); ++i)
            {
                if (Creature* catapult = GetBGCreature(i, false))
                {
                    if (Vehicle* vehicleGlaive = catapult->GetVehicleKit())
                    {
                        if (!vehicleGlaive->GetPassenger(0))
                            DelCreature(i);
                    }
                }
            }

            // spawning glaive throwers
            for (uint8 i = 0; i < MAX_GLAIVE_THROWERS_SPAWNS_PER_FACTION; ++i)
            {
                uint8 type = (node->faction == TEAM_ALLIANCE ? BG_IC_NPC_GLAIVE_THROWER_1_A : BG_IC_NPC_GLAIVE_THROWER_1_H)+i;

                if (GetBGCreature(type, false) && GetBGCreature(type)->IsAlive())
                    continue;

                if (AddCreature(node->faction == TEAM_ALLIANCE ? NPC_GLAIVE_THROWER_A : NPC_GLAIVE_THROWER_H, type, BG_IC_DocksVehiclesGlaives[i], node->faction, RESPAWN_ONE_DAY))
                    GetBGCreature(type)->SetFaction(BG_IC_Factions[(node->faction == TEAM_ALLIANCE ? 0 : 1)]);
            }

            // spawning catapults
            for (uint8 i = 0; i < MAX_CATAPULTS_SPAWNS_PER_FACTION; ++i)
            {
                uint8 type = (node->faction == TEAM_ALLIANCE ? BG_IC_NPC_CATAPULT_1_A : BG_IC_NPC_CATAPULT_1_H)+i;

                if (GetBGCreature(type, false) && GetBGCreature(type)->IsAlive())
                    continue;

                if (AddCreature(NPC_CATAPULT, type, BG_IC_DocksVehiclesCatapults[i], node->faction, RESPAWN_ONE_DAY))
                    GetBGCreature(type)->SetFaction(BG_IC_Factions[(node->faction == TEAM_ALLIANCE ? 0 : 1)]);
            }
            break;
        case BG_IC_GO_WORKSHOP_BANNER:
            {
                if (siegeEngineWorkshopTimer < WORKSHOP_UPDATE_TIME)
                    siegeEngineWorkshopTimer = WORKSHOP_UPDATE_TIME;

                if (!recapture)
                {
                    // we must del opposing faction vehicles when the node is captured (unused ones)
                    for (uint8 i = (node->faction == TEAM_ALLIANCE ? BG_IC_NPC_DEMOLISHER_1_H : BG_IC_NPC_DEMOLISHER_1_A); i < (node->faction == TEAM_ALLIANCE ? BG_IC_NPC_DEMOLISHER_4_H : BG_IC_NPC_DEMOLISHER_4_A); ++i)
                    {
                        if (Creature* demolisher = GetBGCreature(i, false))
                        {
                            if (Vehicle* vehicleDemolisher = demolisher->GetVehicleKit())
                            {
                                // is IsVehicleInUse working as expected?
                                if (!vehicleDemolisher->IsVehicleInUse())
                                    DelCreature(i);
                            }
                        }
                    }

                    for (uint8 i = 0; i < MAX_DEMOLISHERS_SPAWNS_PER_FACTION; ++i)
                    {
                        uint8 type = (node->faction == TEAM_ALLIANCE ? BG_IC_NPC_DEMOLISHER_1_A : BG_IC_NPC_DEMOLISHER_1_H)+i;

                        if (GetBGCreature(type, false) && GetBGCreature(type)->IsAlive())
                            continue;

                        if (AddCreature(NPC_DEMOLISHER, type, BG_IC_WorkshopVehicles[i], node->faction, RESPAWN_ONE_DAY))
                            GetBGCreature(type)->SetFaction(BG_IC_Factions[(node->faction == TEAM_ALLIANCE ? 0 : 1)]);
                    }

                    // we check if the opossing siege engine is in use
                    int8 enemySiege = (node->faction == TEAM_ALLIANCE ? BG_IC_NPC_SIEGE_ENGINE_H : BG_IC_NPC_SIEGE_ENGINE_A);

                    if (Creature* siegeEngine = GetBGCreature(enemySiege, false))
                    {
                        if (Vehicle* vehicleSiege = siegeEngine->GetVehicleKit())
                        {
                            // is VehicleInUse working as expected ?
                            if (!vehicleSiege->IsVehicleInUse())
                                DelCreature(enemySiege);
                        }
                    }

                    uint8 siegeType = (node->faction == TEAM_ALLIANCE ? BG_IC_NPC_SIEGE_ENGINE_A : BG_IC_NPC_SIEGE_ENGINE_H);
                    if (!GetBGCreature(siegeType, false) || !GetBGCreature(siegeType)->IsAlive())
                    {
                        AddCreature((node->faction == TEAM_ALLIANCE ? NPC_SIEGE_ENGINE_A : NPC_SIEGE_ENGINE_H), siegeType,
                            BG_IC_WorkshopVehicles[4], node->faction, RESPAWN_ONE_DAY);

                        if (Creature* siegeEngine = GetBGCreature(siegeType))
                        {
                            siegeEngine->SetUnitFlag(UNIT_FLAG_UNINTERACTIBLE | UNIT_FLAG_CANNOT_SWIM);
                            siegeEngine->SetImmuneToPC(true);
                            siegeEngine->SetFaction(BG_IC_Factions[(node->faction == TEAM_ALLIANCE ? 0 : 1)]);
                        }
                    }
                }

                for (uint8 i = 0; i < MAX_WORKSHOP_BOMBS_SPAWNS_PER_FACTION; ++i)
                {
                    AddObject(BG_IC_GO_SEAFORIUM_BOMBS_1+i, GO_SEAFORIUM_BOMBS,
                    workshopBombs[i].GetPositionX(), workshopBombs[i].GetPositionY(),
                    workshopBombs[i].GetPositionZ(), workshopBombs[i].GetOrientation(),
                    0, 0, 0, 0, 10);

                    if (GameObject* seaforiumBombs = GetBGObject(BG_IC_GO_SEAFORIUM_BOMBS_1+i))
                    {
                        seaforiumBombs->SetRespawnTime(10);
                        seaforiumBombs->SetFaction(BG_IC_Factions[(node->faction == TEAM_ALLIANCE ? 0 : 1)]);
                    }
                }
                break;
            }
        default:
            break;
    }
}

void BattlegroundIC::DestroyGate(Player* player, GameObject* go)
{
    GateStatus[GetGateIDFromEntry(go->GetEntry())] = BG_IC_GATE_DESTROYED;
    uint32 uws_open = GetWorldStateFromGateEntry(go->GetEntry(), true);
    uint32 uws_close = GetWorldStateFromGateEntry(go->GetEntry(), false);
    if (uws_open)
    {
        UpdateWorldState(uws_close, 0);
        UpdateWorldState(uws_open, 1);
    }
    if (player->GetTeamId() == TEAM_ALLIANCE)
    {
        DoorOpen(BG_IC_GO_HORDE_KEEP_PORTCULLIS);
        GetBGObject(BG_IC_GO_HORDE_BANNER)->RemoveFlag(GO_FLAG_NOT_SELECTABLE);
    }
    else
    {
        DoorOpen(BG_IC_GO_DOODAD_PORTCULLISACTIVE02);
        GetBGObject(BG_IC_GO_ALLIANCE_BANNER)->RemoveFlag(GO_FLAG_NOT_SELECTABLE);
    }

    uint32 textId;
    ChatMsg msgType;
    switch (go->GetEntry())
    {
        case GO_HORDE_GATE_1:
            textId = BG_IC_TEXT_FRONT_GATE_HORDE_DESTROYED;
            msgType = CHAT_MSG_BG_SYSTEM_ALLIANCE;
            break;
        case GO_HORDE_GATE_2:
            textId = BG_IC_TEXT_WEST_GATE_HORDE_DESTROYED;
            msgType = CHAT_MSG_BG_SYSTEM_ALLIANCE;
            break;
        case GO_HORDE_GATE_3:
            textId = BG_IC_TEXT_EAST_GATE_HORDE_DESTROYED;
            msgType = CHAT_MSG_BG_SYSTEM_ALLIANCE;
            break;
        case GO_ALLIANCE_GATE_1:
            textId = BG_IC_TEXT_WEST_GATE_ALLIANCE_DESTROYED;
            msgType = CHAT_MSG_BG_SYSTEM_HORDE;
            break;
        case GO_ALLIANCE_GATE_2:
            textId = BG_IC_TEXT_EAST_GATE_ALLIANCE_DESTROYED;
            msgType = CHAT_MSG_BG_SYSTEM_HORDE;
            break;
        case GO_ALLIANCE_GATE_3:
            textId = BG_IC_TEXT_FRONT_GATE_ALLIANCE_DESTROYED;
            msgType = CHAT_MSG_BG_SYSTEM_HORDE;
            break;
        default:
            return;
    }

    if (go->GetEntry() == GO_HORDE_GATE_1 || go->GetEntry() == GO_HORDE_GATE_2 || go->GetEntry() == GO_HORDE_GATE_3)
    {
        if (!GetBgMap()->GetCreature(BgCreatures[BG_IC_NpcSpawnlocs[BG_IC_NPC_OVERLORD_AGMAR].type]) && !AddCreature(BG_IC_NpcSpawnlocs[BG_IC_NPC_OVERLORD_AGMAR].entry, BG_IC_NpcSpawnlocs[BG_IC_NPC_OVERLORD_AGMAR].type, BG_IC_NpcSpawnlocs[BG_IC_NPC_OVERLORD_AGMAR].x, BG_IC_NpcSpawnlocs[BG_IC_NPC_OVERLORD_AGMAR].y, BG_IC_NpcSpawnlocs[BG_IC_NPC_OVERLORD_AGMAR].z, BG_IC_NpcSpawnlocs[BG_IC_NPC_OVERLORD_AGMAR].o, BG_IC_NpcSpawnlocs[BG_IC_NPC_OVERLORD_AGMAR].team, RESPAWN_ONE_DAY))
            TC_LOG_ERROR("bg.battleground", "Isle of Conquest: There was an error spawning creature {}", BG_IC_NpcSpawnlocs[BG_IC_NPC_OVERLORD_AGMAR].entry);
    }
    else if (go->GetEntry() == GO_ALLIANCE_GATE_1 || go->GetEntry() == GO_ALLIANCE_GATE_2 || go->GetEntry() == GO_ALLIANCE_GATE_3)
    {
        if (!GetBgMap()->GetCreature(BgCreatures[BG_IC_NpcSpawnlocs[BG_IC_NPC_HIGH_COMMANDER_HALFORD_WYRMBANE].type]) && !AddCreature(BG_IC_NpcSpawnlocs[BG_IC_NPC_HIGH_COMMANDER_HALFORD_WYRMBANE].entry, BG_IC_NpcSpawnlocs[BG_IC_NPC_HIGH_COMMANDER_HALFORD_WYRMBANE].type, BG_IC_NpcSpawnlocs[BG_IC_NPC_HIGH_COMMANDER_HALFORD_WYRMBANE].x, BG_IC_NpcSpawnlocs[BG_IC_NPC_HIGH_COMMANDER_HALFORD_WYRMBANE].y, BG_IC_NpcSpawnlocs[BG_IC_NPC_HIGH_COMMANDER_HALFORD_WYRMBANE].z, BG_IC_NpcSpawnlocs[BG_IC_NPC_HIGH_COMMANDER_HALFORD_WYRMBANE].o, BG_IC_NpcSpawnlocs[BG_IC_NPC_HIGH_COMMANDER_HALFORD_WYRMBANE].team, RESPAWN_ONE_DAY))
            TC_LOG_ERROR("bg.battleground", "Isle of Conquest: There was an error spawning creature {}", BG_IC_NpcSpawnlocs[BG_IC_NPC_HIGH_COMMANDER_HALFORD_WYRMBANE].entry);
    }

    SendBroadcastText(textId, msgType);
}

WorldSafeLocsEntry const* BattlegroundIC::GetClosestGraveyard(Player* player)
{
    TeamId teamIndex = GetTeamIndexByTeamId(player->GetTeam());

    // Is there any occupied node for this team?
    std::vector<uint8> nodes;
    for (uint8 i = 0; i < MAX_NODE_TYPES; ++i)
        if (nodePoint[i].faction == player->GetTeamId())
            nodes.push_back(i);

    WorldSafeLocsEntry const* good_entry = nullptr;
    // If so, select the closest node to place ghost on
    if (!nodes.empty())
    {
        float player_x = player->GetPositionX();
        float player_y = player->GetPositionY();

        float mindist = 999999.0f;
        for (uint8 i = 0; i < nodes.size(); ++i)
        {
            WorldSafeLocsEntry const*entry = sWorldSafeLocsStore.LookupEntry(BG_IC_GraveyardIds[nodes[i]]);
            if (!entry)
                continue;
            float dist = (entry->Loc.X - player_x)*(entry->Loc.X - player_x)+(entry->Loc.Y - player_y)*(entry->Loc.Y - player_y);
            if (mindist > dist)
            {
                mindist = dist;
                good_entry = entry;
            }
        }
        nodes.clear();
    }
    // If not, place ghost on starting location
    if (!good_entry)
        good_entry = sWorldSafeLocsStore.LookupEntry(BG_IC_GraveyardIds[uint32(teamIndex) + MAX_NODE_TYPES]);

    return good_entry;
}

bool BattlegroundIC::IsAllNodesControlledByTeam(uint32 team) const
{
    uint32 count = 0;
    ICNodeState controlledState = team == ALLIANCE ? NODE_STATE_CONTROLLED_A : NODE_STATE_CONTROLLED_H;
    for (int i = 0; i < NODE_TYPE_WORKSHOP; ++i)
    {
        if (nodePoint[i].nodeState == controlledState)
            ++count;
    }

    return count == NODE_TYPE_WORKSHOP;
}

bool BattlegroundIC::IsSpellAllowed(uint32 spellId, Player const* player) const
{
    switch (spellId)
    {
        case SPELL_OIL_REFINERY:
        case SPELL_QUARRY:
        {
            uint32 team = player->GetTeamId();
            uint8 nodeType = spellId == SPELL_OIL_REFINERY ? NODE_TYPE_REFINERY : NODE_TYPE_QUARRY;
            uint8 nodeState = team == TEAM_ALLIANCE ? NODE_STATE_CONTROLLED_A : NODE_STATE_CONTROLLED_H;
            return GetNodeState(nodeType) == nodeState;
        }
        default:
           break;
    }

    return true;
}

namespace
{
    // "Seaforium Bombs": the workshop's bomb piles (GO_SEAFORIUM_BOMBS) hand
    // it out through spell 66265, and using it beside a gate plants the
    // charge (GO 195235) that brings the gate down.
    uint32 const ITEM_IC_SEAFORIUM_BOMBS = 46847;

    // How far out in front of its gate a siege objective stands.
    float const IC_GATE_FRONT_DISTANCE = 10.0f;

    struct ICSiegeGate
    {
        uint32 ObjectType; // BG_IC_GOs
        uint32 DoorIndex;  // ICDoorList, the GateStatus[] slot
        TeamId Team;
    };

    // Siege objective order: each keep's front, west and east gate, the
    // Alliance keep first. The slots follow GetGateIDFromEntry.
    ICSiegeGate const ICSiegeGates[MAX_FORTRESS_GATES_SPAWNS] =
    {
        { BG_IC_GO_ALLIANCE_GATE_3, BG_IC_A_FRONT, TEAM_ALLIANCE },
        { BG_IC_GO_ALLIANCE_GATE_1, BG_IC_A_WEST,  TEAM_ALLIANCE },
        { BG_IC_GO_ALLIANCE_GATE_2, BG_IC_A_EAST,  TEAM_ALLIANCE },
        { BG_IC_GO_HORDE_GATE_1,    BG_IC_H_FRONT, TEAM_HORDE    },
        { BG_IC_GO_HORDE_GATE_2,    BG_IC_H_WEST,  TEAM_HORDE    },
        { BG_IC_GO_HORDE_GATE_3,    BG_IC_H_EAST,  TEAM_HORDE    }
    };

    // Plain doors, which block while closed: the closed-gate effects that
    // fill the gateways until the match starts, the starting portcullises,
    // and the inner keep portcullises DestroyGate raises.
    // Doodad_PortcullisActive01 is left out - its spawn row lost its X
    // coordinate, so it stands nowhere near the keeps.
    uint32 const ICDoors[] =
    {
        BG_IC_GO_DOODAD_ND_HUMAN_GATE_CLOSEDFX_DOOR01,
        BG_IC_GO_DOODAD_ND_HUMAN_GATE_CLOSEDFX_DOOR02,
        BG_IC_GO_DOODAD_ND_HUMAN_GATE_CLOSEDFX_DOOR03,
        BG_IC_GO_DOODAD_ND_WINTERORC_WALL_GATEFX_DOOR01,
        BG_IC_GO_DOODAD_ND_WINTERORC_WALL_GATEFX_DOOR02,
        BG_IC_GO_DOODAD_ND_WINTERORC_WALL_GATEFX_DOOR03,
        BG_IC_GO_DOODAD_HU_PORTCULLIS01_1,
        BG_IC_GO_DOODAD_HU_PORTCULLIS01_2,
        BG_IC_GO_DOODAD_VR_PORTCULLIS01_1,
        BG_IC_GO_DOODAD_VR_PORTCULLIS01_2,
        BG_IC_GO_DOODAD_PORTCULLISACTIVE02,
        BG_IC_GO_HORDE_KEEP_PORTCULLIS
    };

    // The World Trigger (22515) spawns of map 628. They live in the world
    // database's `creature` table, not in this script, and are where the
    // keep teleporters land: 66548/66549 trigger 66550/66551, which
    // `conditions` aim at the nearest trigger MORE than 10 yards away - the
    // one on the other side of the wall from the teleporter clicked.
    Position const ICTeleportTriggers[] =
    {
        // Alliance keep
        { 326.214f, -744.024f, 49.4358f },
        { 325.917f, -781.899f, 49.0052f },
        { 430.601f, -857.005f, 48.3118f },
        { 392.496f, -859.458f, 48.9987f },
        { 323.496f, -883.802f, 48.9996f },
        { 313.234f, -918.045f, 48.856f },
        { 279.87f, -832.807f, 51.5509f },
        // Horde keep
        { 1296.53f, -766.182f, 50.7029f },
        { 1232.39f, -861.024f, 48.9996f },
        { 1233.11f, -838.316f, 48.9996f },
        { 1232.52f, -666.325f, 48.134f },
        { 1234.3f, -688.299f, 49.223f },
        { 1162.91f, -745.908f, 48.7151f },
        { 1139.5f, -779.474f, 48.735f }
    };

    float const IC_TELEPORT_MIN_DISTANCE = 10.0f;

    Position GetICObjectSpawnPosition(uint32 type)
    {
        for (ICGo const& spawn : BG_IC_ObjSpawnlocs)
            if (spawn.type == type)
                return Position(spawn.x, spawn.y, spawn.z, spawn.o);

        return Position();
    }

    // Open ground IC_GATE_FRONT_DISTANCE outside the gate, facing it.
    Position GetICGateFront(uint32 siegeIndex)
    {
        ICSiegeGate const& gate = ICSiegeGates[siegeIndex];
        Position const gatePos = GetICObjectSpawnPosition(gate.ObjectType);

        // The keep's middle, taken as the mean of its gates, only says which
        // side of the wall is outside.
        float centreX = 0.0f;
        float centreY = 0.0f;
        uint32 keepGates = 0;
        for (ICSiegeGate const& other : ICSiegeGates)
        {
            if (other.Team != gate.Team)
                continue;

            Position const otherPos = GetICObjectSpawnPosition(other.ObjectType);
            centreX += otherPos.GetPositionX();
            centreY += otherPos.GetPositionY();
            ++keepGates;
        }

        centreX /= float(keepGates);
        centreY /= float(keepGates);

        // The way out is along the gate's own facing: every gate model stands
        // square across its wall. Pushing along (gate - centre) itself would
        // slide the side gates' spot some three yards along the wall, as they
        // sit well off the keep's centre line.
        float dirX = std::cos(gatePos.GetOrientation());
        float dirY = std::sin(gatePos.GetOrientation());
        if (dirX * (gatePos.GetPositionX() - centreX) + dirY * (gatePos.GetPositionY() - centreY) < 0.0f)
        {
            dirX = -dirX;
            dirY = -dirY;
        }

        return Position(gatePos.GetPositionX() + dirX * IC_GATE_FRONT_DISTANCE,
            gatePos.GetPositionY() + dirY * IC_GATE_FRONT_DISTANCE,
            gatePos.GetPositionZ(), std::atan2(-dirY, -dirX));
    }
}

bool BattlegroundIC::GetObjectiveTeam(ObjectGuid playerGuid, TeamId& teamId) const
{
    if (GetStatus() != STATUS_IN_PROGRESS || !FindBgMap())
        return false;

    uint32 const playerTeam = GetPlayerTeam(playerGuid);
    if (playerTeam != ALLIANCE && playerTeam != HORDE)
        return false;

    teamId = GetTeamIndexByTeamId(playerTeam);
    return true;
}

int32 BattlegroundIC::GetFirstDestroyedGate(TeamId keepTeam) const
{
    for (uint32 i = 0; i < MAX_FORTRESS_GATES_SPAWNS; ++i)
        if (ICSiegeGates[i].Team == keepTeam && GateStatus[ICSiegeGates[i].DoorIndex] == BG_IC_GATE_DESTROYED)
            return int32(i);

    return -1;
}

bool BattlegroundIC::GetDynamicNodeInfo(ObjectGuid playerGuid, uint32 nodeId, BattlegroundNodeObjective& node) const
{
    TeamId teamId = TEAM_NEUTRAL;
    if (nodeId >= MAX_NODE_TYPES || !GetObjectiveTeam(playerGuid, teamId))
        return false;

    ICNodePoint const& point = nodePoint[nodeId];
    GameObject* banner = FindBgMap()->GetGameObject(BgObjects[point.gameobject_type]);

    // A keep's graveyard banner cannot be clicked until one of that keep's
    // gates is down (DestroyGate lifts its GO_FLAG_NOT_SELECTABLE), and the
    // keep's own banner brings the flag back from its template whenever it
    // is put up again.
    if (point.nodeType == NODE_TYPE_GRAVEYARD_A || point.nodeType == NODE_TYPE_GRAVEYARD_H)
    {
        TeamId const keepTeam = point.nodeType == NODE_TYPE_GRAVEYARD_A ? TEAM_ALLIANCE : TEAM_HORDE;
        if (GetFirstDestroyedGate(keepTeam) < 0 || !banner || banner->HasFlag(GO_FLAG_NOT_SELECTABLE))
            return false;
    }

    node.NodeId = nodeId;
    node.BannerGuid = banner ? banner->GetGUID() : ObjectGuid::Empty;
    node.Location = banner ? banner->GetPosition() : GetICObjectSpawnPosition(point.gameobject_type);
    node.CaptureRadius = 0.0f;

    // nodeState follows the banner on show. While a capture timer runs,
    // `faction` is the side the node falls to when it expires - which is not
    // always the side nodeState names, since GetNextBanner can hand an
    // assaulted grey banner back to the neutral one - and last_entry is the
    // banner that stood before the last click.
    if (point.nodeState == NODE_STATE_CONTROLLED_A || point.nodeState == NODE_STATE_CONTROLLED_H)
    {
        TeamId const owner = point.nodeState == NODE_STATE_CONTROLLED_A ? TEAM_ALLIANCE : TEAM_HORDE;
        node.Status = owner == teamId ? BattlegroundNodeStatus::FriendlyControlled : BattlegroundNodeStatus::EnemyControlled;
    }
    else if (point.faction != TEAM_ALLIANCE && point.faction != TEAM_HORDE)
        node.Status = BattlegroundNodeStatus::Neutral;
    else if (point.faction == teamId)
        node.Status = BattlegroundNodeStatus::FriendlyContested;
    else if (point.last_entry == point.banners[teamId == TEAM_ALLIANCE ? BANNER_A_CONTROLLED : BANNER_H_CONTROLLED])
        node.Status = BattlegroundNodeStatus::FriendlyUnderAttack;
    else
        node.Status = BattlegroundNodeStatus::EnemyContested;

    return true;
}

bool BattlegroundIC::GetSiegeObjectiveInfo(ObjectGuid playerGuid, uint32 index, BattlegroundSiegeObjective& objective) const
{
    TeamId teamId = TEAM_NEUTRAL;
    if (index >= MAX_FORTRESS_GATES_SPAWNS || !GetObjectiveTeam(playerGuid, teamId))
        return false;

    ICSiegeGate const& gate = ICSiegeGates[index];
    objective.Id = index;
    objective.GateGuid = BgObjects[gate.ObjectType];
    objective.Location = GetICGateFront(index);
    objective.Friendly = gate.Team == teamId;
    // The gates stand open for the first CLOSE_DOORS_TIME of the match. Once
    // one gate of a keep is down the keep is open - DestroyGate raises its
    // inner portcullis - and its other two gates guard nothing.
    objective.Breachable = doorsClosed && GetFirstDestroyedGate(gate.Team) < 0;
    return true;
}

bool BattlegroundIC::GetSiegeChargeInfo(ObjectGuid playerGuid, BattlegroundSiegeChargeInfo& charge) const
{
    TeamId teamId = TEAM_NEUTRAL;
    if (!GetObjectiveTeam(playerGuid, teamId))
        return false;

    // HandleCapturedNodes puts the bomb piles up, in the holder's faction,
    // whenever the workshop is captured or defended, and HandleContestedNodes
    // takes them away as soon as it is assaulted.
    if (nodePoint[NODE_TYPE_WORKSHOP].nodeState != (teamId == TEAM_ALLIANCE ? NODE_STATE_CONTROLLED_A : NODE_STATE_CONTROLLED_H))
        return false;

    GuidVector sources;
    for (uint32 type = BG_IC_GO_SEAFORIUM_BOMBS_1; type <= BG_IC_GO_SEAFORIUM_BOMBS_2; ++type)
    {
        GameObject* bombs = FindBgMap()->GetGameObject(BgObjects[type]);
        if (!bombs || !bombs->IsInWorld() || !bombs->isSpawned() || bombs->HasFlag(GO_FLAG_NOT_SELECTABLE) ||
            bombs->GetFaction() != BG_IC_Factions[teamId])
            continue;

        sources.push_back(bombs->GetGUID());
    }

    if (sources.empty())
        return false;

    charge.ItemId = ITEM_IC_SEAFORIUM_BOMBS;
    charge.Sources = sources;
    return true;
}

void BattlegroundIC::GetSiegeVehicles(ObjectGuid playerGuid, GuidVector& vehicles) const
{
    TeamId teamId = TEAM_NEUTRAL;
    if (!GetObjectiveTeam(playerGuid, teamId))
        return;

    // The workshop's machines, the ones built to batter gates; the docks'
    // catapults and glaive throwers are not sent at them.
    uint32 const firstDemolisher = teamId == TEAM_ALLIANCE ? BG_IC_NPC_DEMOLISHER_1_A : BG_IC_NPC_DEMOLISHER_1_H;
    uint32 const types[] =
    {
        uint32(teamId == TEAM_ALLIANCE ? BG_IC_NPC_SIEGE_ENGINE_A : BG_IC_NPC_SIEGE_ENGINE_H),
        firstDemolisher,
        firstDemolisher + 1,
        firstDemolisher + 2,
        firstDemolisher + 3
    };

    for (uint32 type : types)
    {
        // A new siege engine is UNINTERACTIBLE and immune to players until
        // the workshop's next three-minute tick in PostUpdateImpl. A driven
        // vehicle takes on its driver's faction, so the faction test also
        // drops the ones somebody already sits in.
        Creature* vehicle = FindBgMap()->GetCreature(BgCreatures[type]);
        if (!vehicle || !vehicle->IsInWorld() || !vehicle->IsAlive() ||
            vehicle->HasUnitFlag(UNIT_FLAG_UNINTERACTIBLE) || vehicle->IsImmuneToPC() ||
            vehicle->GetFaction() != BG_IC_Factions[teamId])
            continue;

        vehicles.push_back(vehicle->GetGUID());
    }
}

void BattlegroundIC::GetStandingGates(GuidVector& gates) const
{
    BattlegroundMap* map = FindBgMap();
    if (!map)
        return;

    // The siege gates stand in their intact model - an open gateway - until
    // PostUpdateImpl swings them shut CLOSE_DOORS_TIME into the match.
    if (doorsClosed)
    {
        for (ICSiegeGate const& gate : ICSiegeGates)
        {
            if (GateStatus[gate.DoorIndex] == BG_IC_GATE_DESTROYED)
                continue;

            if (GameObject* go = map->GetGameObject(BgObjects[gate.ObjectType]))
                if (go->GetDestructibleState() != GO_DESTRUCTIBLE_DESTROYED)
                    gates.push_back(go->GetGUID());
        }
    }

    // A despawned door is neither shown to clients nor collided with, so
    // only a spawned one still closed (DoorOpen sets GO_STATE_ACTIVE) blocks.
    for (uint32 type : ICDoors)
        if (GameObject* door = map->GetGameObject(BgObjects[type]))
            if (door->IsInWorld() && door->isSpawned() && door->GetGoState() == GO_STATE_READY)
                gates.push_back(door->GetGUID());
}

bool BattlegroundIC::GetBossObjectiveInfo(ObjectGuid playerGuid, uint32 index, BattlegroundBossObjective& boss) const
{
    TeamId teamId = TEAM_NEUTRAL;
    if (index >= PVP_TEAMS_COUNT || !GetObjectiveTeam(playerGuid, teamId))
        return false;

    // Index 0 is the Alliance commander, 1 the Horde's. Neither exists until
    // DestroyGate spawns them as the first gate of their keep falls.
    TeamId const bossTeam = TeamId(index);
    uint32 const bossType = bossTeam == TEAM_ALLIANCE ? BG_IC_NPC_HIGH_COMMANDER_HALFORD_WYRMBANE : BG_IC_NPC_OVERLORD_AGMAR;
    Creature* commander = FindBgMap()->GetCreature(BgCreatures[bossType]);
    bool const alive = commander && commander->IsInWorld() && commander->IsAlive();
    int32 const breach = GetFirstDestroyedGate(bossTeam);

    boss.Id = index;
    boss.BossGuid = alive ? commander->GetGUID() : ObjectGuid::Empty;
    boss.Friendly = bossTeam == teamId;
    boss.Engageable = !boss.Friendly && alive && breach >= 0;
    // In through the broken gate: the straight line to the commander runs
    // into the gates still standing.
    boss.HasApproach = breach >= 0;
    if (boss.HasApproach)
        boss.Approach = GetICGateFront(uint32(breach));

    return true;
}

void BattlegroundIC::GetTeleporters(ObjectGuid playerGuid, std::vector<BattlegroundTeleporter>& teleporters) const
{
    TeamId teamId = TEAM_NEUTRAL;
    if (!GetObjectiveTeam(playerGuid, teamId))
        return;

    for (ICGo const& spawn : BG_IC_Teleporters)
    {
        // Each keep has its own pair of teleporter entries, in its faction.
        bool const allianceKeep = spawn.entry == GO_TELEPORTER_3 || spawn.entry == GO_TELEPORTER_4;
        if (allianceKeep != (teamId == TEAM_ALLIANCE))
            continue;

        // StartingEventOpenDoors makes them selectable.
        GameObject* teleporter = FindBgMap()->GetGameObject(BgObjects[spawn.type]);
        if (!teleporter || !teleporter->IsInWorld() || !teleporter->isSpawned() || teleporter->HasFlag(GO_FLAG_NOT_SELECTABLE))
            continue;

        Position const entrance(spawn.x, spawn.y, spawn.z, spawn.o);
        Position const* landing = nullptr;
        float landingDistSq = 0.0f;
        for (Position const& trigger : ICTeleportTriggers)
        {
            float const distSq = entrance.GetExactDistSq(trigger);
            if (distSq <= IC_TELEPORT_MIN_DISTANCE * IC_TELEPORT_MIN_DISTANCE)
                continue;

            if (!landing || distSq < landingDistSq)
            {
                landing = &trigger;
                landingDistSq = distSq;
            }
        }

        if (!landing)
            continue;

        BattlegroundTeleporter shortcut;
        shortcut.GoGuid = teleporter->GetGUID();
        shortcut.Entrance = teleporter->GetPosition();
        shortcut.Exit = *landing;
        teleporters.push_back(shortcut);
    }
}
