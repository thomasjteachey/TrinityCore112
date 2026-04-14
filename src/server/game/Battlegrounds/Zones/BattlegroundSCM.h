/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#ifndef __BATTLEGROUNDSCM_H
#define __BATTLEGROUNDSCM_H

#include "Battleground.h"
#include "BattlegroundScore.h"

struct WorldSafeLocsEntry;

enum BG_SCM_WorldStates
{
    BG_SCM_WORLDSTATE_ALLIANCE_SCORE = 1581,
    BG_SCM_WORLDSTATE_HORDE_SCORE    = 1582,
    BG_SCM_WORLDSTATE_MAX_SCORE      = 1601,
    BG_SCM_WORLDSTATE_TIMER_ACTIVE   = 4247,
    BG_SCM_WORLDSTATE_TIMER_VALUE    = 4248
};

enum BG_SCM_Graveyards
{
    BG_SCM_START_ALLIANCE = 51890,
    BG_SCM_START_HORDE    = 51891,
    BG_SCM_GY_ALLIANCE_A  = 51892,
    BG_SCM_GY_HORDE_A     = 51893,
    BG_SCM_GY_ALLIANCE_B  = 51894,
    BG_SCM_GY_HORDE_B     = 51895
};

enum BG_SCM_CreatureTypes
{
    BG_SCM_SPIRIT_ALLIANCE_A = 0,
    BG_SCM_SPIRIT_ALLIANCE_B = 1,
    BG_SCM_SPIRIT_HORDE_A    = 2,
    BG_SCM_SPIRIT_HORDE_B    = 3,
    BG_SCM_CREATURE_MAX      = 4
};

struct BattlegroundSCMScore final : public BattlegroundScore
{
    explicit BattlegroundSCMScore(ObjectGuid playerGuid) : BattlegroundScore(playerGuid) { }

protected:
    void BuildObjectivesBlock(WorldPacket& data) override
    {
        data << uint32(0);
    }
};

class BattlegroundSCM : public Battleground
{
public:
    BattlegroundSCM();
    ~BattlegroundSCM() override = default;

    void AddPlayer(Player* player) override;
    void Reset() override;
    bool SetupBattleground() override;
    void StartingEventCloseDoors() override;
    void StartingEventOpenDoors() override;
    void HandleKillPlayer(Player* victim, Player* killer) override;
    WorldSafeLocsEntry const* GetClosestGraveyard(Player* player) override;
    void FillInitialWorldStates(WorldPackets::WorldState::InitWorldStates& packet) override;
    bool HandlePlayerUnderMap(Player* player) override;

private:
    static constexpr uint32 KillLimit = 50;

    WorldSafeLocsEntry const* GetTeamStartLoc(TeamId teamId) const;
    WorldSafeLocsEntry const* GetRandomTeamGraveyard(TeamId teamId) const;
};

#endif
