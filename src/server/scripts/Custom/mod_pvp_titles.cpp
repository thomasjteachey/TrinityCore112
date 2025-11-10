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
#include "Configuration/Config.h"
#include "Chat.h"
#include "DBCStores.h"
#include "Player.h"
#include "SharedDefines.h"

namespace
{
struct PvPTitles
{
    uint32 RequiredKills;
    uint32 TitleId;
};

uint32 GetRequiredKills(char const* configName, uint32 defaultValue)
{
    int32 const configValue = sConfigMgr->GetIntDefault(configName, static_cast<int32>(defaultValue));
    return static_cast<uint32>(configValue < 0 ? 0 : configValue);
}

enum Ranks : uint8
{
    RANK_ONE = 0,
    RANK_TWO,
    RANK_THREE,
    RANK_FOUR,
    RANK_FIVE,
    RANK_SIX,
    RANK_SEVEN,
    RANK_EIGHT,
    RANK_NINE,
    RANK_TEN,
    RANK_ELEVEN,
    RANK_TWELVE,
    RANK_THIRTEEN,
    RANK_FOURTEEN,
    MAX_RANK
};

enum RankReqKillDefault : uint32
{
    RANK_ONE_HK_COUNT        = 50,
    RANK_TWO_HK_COUNT        = 100,
    RANK_THREE_HK_COUNT      = 500,
    RANK_FOUR_HK_COUNT       = 1000,
    RANK_FIVE_HK_COUNT       = 2000,
    RANK_SIX_HK_COUNT        = 4000,
    RANK_SEVEN_HK_COUNT      = 5000,
    RANK_EIGHT_HK_COUNT      = 6000,
    RANK_NINE_HK_COUNT       = 8000,
    RANK_TEN_HK_COUNT        = 10000,
    RANK_ELEVEN_HK_COUNT     = 12500,
    RANK_TWELVE_HK_COUNT     = 15000,
    RANK_THIRTEEN_HK_COUNT   = 20000,
    RANK_FOURTEEN_HK_COUNT   = 25000
};

enum Titles : uint32
{
    PRIVATE                  = 1,
    CORPORAL                 = 2,
    SERGEANT                 = 3,
    MASTER_SERGEANT          = 4,
    SERGEANT_MAJOR           = 5,
    KNIGHT                   = 6,
    KNIGHT_LIEUTENANT        = 7,
    KNIGHT_CAPTAIN           = 8,
    KNIGHT_CHAMPION          = 9,
    LIEUTENANT_COMMANDER     = 10,
    COMMANDER                = 11,
    MARSHAL                  = 12,
    FIELD_MARSHAL            = 13,
    GRAND_MARSHAL            = 14,
    SCOUT                    = 15,
    GRUNT                    = 16,
    SERGEANT_H               = 17,
    SENIOR_SERGEANT          = 18,
    FIRST_SERGEANT           = 19,
    STONE_GUARD              = 20,
    BLOOD_GUARD              = 21,
    LEGIONNAIRE              = 22,
    CENTURION                = 23,
    CHAMPION                 = 24,
    LIEUTENANT_GENERAL       = 25,
    GENERAL                  = 26,
    WARLORD                  = 27,
    HIGH_WARLORD             = 28
};

enum CleanUpTitlesModes : int32
{
    CLEAN_UP_NONE           = 0,
    CLEAN_UP_REMOVE_ALL     = 1,
    CLEAN_UP_REMOVE_INVALID = 2
};

uint32 const TitleData[MAX_RANK][2] =
{
    { PRIVATE,              SCOUT              },
    { CORPORAL,             GRUNT              },
    { SERGEANT,             SERGEANT_H         },
    { MASTER_SERGEANT,      SENIOR_SERGEANT    },
    { SERGEANT_MAJOR,       FIRST_SERGEANT     },
    { KNIGHT,               STONE_GUARD        },
    { KNIGHT_LIEUTENANT,    BLOOD_GUARD        },
    { KNIGHT_CAPTAIN,       LEGIONNAIRE        },
    { KNIGHT_CHAMPION,      CENTURION          },
    { LIEUTENANT_COMMANDER, CHAMPION           },
    { COMMANDER,            LIEUTENANT_GENERAL },
    { MARSHAL,              GENERAL            },
    { FIELD_MARSHAL,        WARLORD            },
    { GRAND_MARSHAL,        HIGH_WARLORD       }
};

class PvPTitlesPlayerScript : public PlayerScript
{
public:
    PvPTitlesPlayerScript() : PlayerScript("PvPTitlesPlayerScript") { }

    void OnLogin(Player* player, bool /*firstLogin*/) override
    {
        if (!sConfigMgr->GetBoolDefault("PvPTitles.Enable", false))
            return;

        if (sConfigMgr->GetBoolDefault("PvPTitles.Announce", true))
            ChatHandler(player->GetSession()).SendSysMessage("This server is running the |cff4CFF00PvPTitles |rmodule.");

        if (sConfigMgr->GetBoolDefault("PvPTitles.AwardTitlesOnLogin", false))
            AwardEarnedTitles(player);

        int32 const cleanUpMode = sConfigMgr->GetIntDefault("PvPTitles.CleanUp", CLEAN_UP_NONE);
        if (cleanUpMode != CLEAN_UP_NONE)
            CleanUpTitles(cleanUpMode, player);
    }

    void OnPVPKill(Player* killer, Player* killed) override
    {
        if (!sConfigMgr->GetBoolDefault("PvPTitles.Enable", false))
            return;

        if (killer == killed)
            return;

        AwardEarnedTitles(killer);
    }

private:
    void AwardEarnedTitles(Player* player)
    {
        TeamId const teamId = player->GetTeamId();
        if (teamId != TEAM_ALLIANCE && teamId != TEAM_HORDE)
            return;

        size_t const teamIndex = static_cast<size_t>(teamId);
        uint32 const kills = player->GetUInt32Value(PLAYER_FIELD_LIFETIME_HONORABLE_KILLS);

        PvPTitles const pvpTitlesList[MAX_RANK] =
        {
            { GetRequiredKills("PvPTitles.Rank_1", RANK_ONE_HK_COUNT),       TitleData[RANK_ONE][teamIndex] },
            { GetRequiredKills("PvPTitles.Rank_2", RANK_TWO_HK_COUNT),       TitleData[RANK_TWO][teamIndex] },
            { GetRequiredKills("PvPTitles.Rank_3", RANK_THREE_HK_COUNT),     TitleData[RANK_THREE][teamIndex] },
            { GetRequiredKills("PvPTitles.Rank_4", RANK_FOUR_HK_COUNT),      TitleData[RANK_FOUR][teamIndex] },
            { GetRequiredKills("PvPTitles.Rank_5", RANK_FIVE_HK_COUNT),      TitleData[RANK_FIVE][teamIndex] },
            { GetRequiredKills("PvPTitles.Rank_6", RANK_SIX_HK_COUNT),       TitleData[RANK_SIX][teamIndex] },
            { GetRequiredKills("PvPTitles.Rank_7", RANK_SEVEN_HK_COUNT),     TitleData[RANK_SEVEN][teamIndex] },
            { GetRequiredKills("PvPTitles.Rank_8", RANK_EIGHT_HK_COUNT),     TitleData[RANK_EIGHT][teamIndex] },
            { GetRequiredKills("PvPTitles.Rank_9", RANK_NINE_HK_COUNT),      TitleData[RANK_NINE][teamIndex] },
            { GetRequiredKills("PvPTitles.Rank_10", RANK_TEN_HK_COUNT),      TitleData[RANK_TEN][teamIndex] },
            { GetRequiredKills("PvPTitles.Rank_11", RANK_ELEVEN_HK_COUNT),   TitleData[RANK_ELEVEN][teamIndex] },
            { GetRequiredKills("PvPTitles.Rank_12", RANK_TWELVE_HK_COUNT),   TitleData[RANK_TWELVE][teamIndex] },
            { GetRequiredKills("PvPTitles.Rank_13", RANK_THIRTEEN_HK_COUNT), TitleData[RANK_THIRTEEN][teamIndex] },
            { GetRequiredKills("PvPTitles.Rank_14", RANK_FOURTEEN_HK_COUNT), TitleData[RANK_FOURTEEN][teamIndex] }
        };

        for (PvPTitles const& title : pvpTitlesList)
        {
            if (kills < title.RequiredKills || player->HasTitle(title.TitleId))
                continue;

            if (CharTitlesEntry const* titleEntry = sCharTitlesStore.LookupEntry(title.TitleId))
                player->SetTitle(titleEntry);
        }
    }

    void CleanUpTitles(int32 mode, Player* player)
    {
        TeamId const teamId = player->GetTeamId();
        if (teamId != TEAM_ALLIANCE && teamId != TEAM_HORDE)
            return;

        size_t const teamIndex = static_cast<size_t>(teamId);

        PvPTitles const pvpTitlesList[MAX_RANK] =
        {
            { GetRequiredKills("PvPTitles.Rank_1", RANK_ONE_HK_COUNT),       TitleData[RANK_ONE][teamIndex] },
            { GetRequiredKills("PvPTitles.Rank_2", RANK_TWO_HK_COUNT),       TitleData[RANK_TWO][teamIndex] },
            { GetRequiredKills("PvPTitles.Rank_3", RANK_THREE_HK_COUNT),     TitleData[RANK_THREE][teamIndex] },
            { GetRequiredKills("PvPTitles.Rank_4", RANK_FOUR_HK_COUNT),      TitleData[RANK_FOUR][teamIndex] },
            { GetRequiredKills("PvPTitles.Rank_5", RANK_FIVE_HK_COUNT),      TitleData[RANK_FIVE][teamIndex] },
            { GetRequiredKills("PvPTitles.Rank_6", RANK_SIX_HK_COUNT),       TitleData[RANK_SIX][teamIndex] },
            { GetRequiredKills("PvPTitles.Rank_7", RANK_SEVEN_HK_COUNT),     TitleData[RANK_SEVEN][teamIndex] },
            { GetRequiredKills("PvPTitles.Rank_8", RANK_EIGHT_HK_COUNT),     TitleData[RANK_EIGHT][teamIndex] },
            { GetRequiredKills("PvPTitles.Rank_9", RANK_NINE_HK_COUNT),      TitleData[RANK_NINE][teamIndex] },
            { GetRequiredKills("PvPTitles.Rank_10", RANK_TEN_HK_COUNT),      TitleData[RANK_TEN][teamIndex] },
            { GetRequiredKills("PvPTitles.Rank_11", RANK_ELEVEN_HK_COUNT),   TitleData[RANK_ELEVEN][teamIndex] },
            { GetRequiredKills("PvPTitles.Rank_12", RANK_TWELVE_HK_COUNT),   TitleData[RANK_TWELVE][teamIndex] },
            { GetRequiredKills("PvPTitles.Rank_13", RANK_THIRTEEN_HK_COUNT), TitleData[RANK_THIRTEEN][teamIndex] },
            { GetRequiredKills("PvPTitles.Rank_14", RANK_FOURTEEN_HK_COUNT), TitleData[RANK_FOURTEEN][teamIndex] }
        };

        uint32 const kills = player->GetUInt32Value(PLAYER_FIELD_LIFETIME_HONORABLE_KILLS);

        for (PvPTitles const& title : pvpTitlesList)
        {
            if (!player->HasTitle(title.TitleId))
                continue;

            if (CharTitlesEntry const* titleEntry = sCharTitlesStore.LookupEntry(title.TitleId))
            {
                if (mode == CLEAN_UP_REMOVE_ALL)
                    player->SetTitle(titleEntry, true);
                else if (mode == CLEAN_UP_REMOVE_INVALID && kills < title.RequiredKills)
                    player->SetTitle(titleEntry, true);
            }
        }
    }
};
} // namespace

void AddSC_mod_pvp_titles()
{
    new PvPTitlesPlayerScript();
}

