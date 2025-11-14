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
#include "Common.h"
#include "Configuration/Config.h"
#include "Chat.h"
#include "DBCStores.h"
#include "Player.h"

#include <array>
#include <vector>

namespace
{
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

char const* const TitleNameData[MAX_RANK][2] =
{
    { "Private",              "Scout"           },
    { "Corporal",             "Grunt"           },
    { "Sergeant",             "Sergeant"        },
    { "Master Sergeant",      "Senior Sergeant" },
    { "Sergeant Major",       "First Sergeant"  },
    { "Knight",               "Stone Guard"     },
    { "Knight-Lieutenant",    "Blood Guard"     },
    { "Knight-Captain",       "Legionnaire"     },
    { "Knight-Champion",      "Centurion"       },
    { "Lieutenant Commander", "Champion"        },
    { "Commander",            "Lieutenant General" },
    { "Marshal",              "General"         },
    { "Field Marshal",        "Warlord"         },
    { "Grand Marshal",        "High Warlord"    }
};

char const* const RankConfigKeys[MAX_RANK] =
{
    "PvPTitles.Rank_1",
    "PvPTitles.Rank_2",
    "PvPTitles.Rank_3",
    "PvPTitles.Rank_4",
    "PvPTitles.Rank_5",
    "PvPTitles.Rank_6",
    "PvPTitles.Rank_7",
    "PvPTitles.Rank_8",
    "PvPTitles.Rank_9",
    "PvPTitles.Rank_10",
    "PvPTitles.Rank_11",
    "PvPTitles.Rank_12",
    "PvPTitles.Rank_13",
    "PvPTitles.Rank_14"
};

uint32 const RankDefaultKills[MAX_RANK] =
{
    RANK_ONE_HK_COUNT,
    RANK_TWO_HK_COUNT,
    RANK_THREE_HK_COUNT,
    RANK_FOUR_HK_COUNT,
    RANK_FIVE_HK_COUNT,
    RANK_SIX_HK_COUNT,
    RANK_SEVEN_HK_COUNT,
    RANK_EIGHT_HK_COUNT,
    RANK_NINE_HK_COUNT,
    RANK_TEN_HK_COUNT,
    RANK_ELEVEN_HK_COUNT,
    RANK_TWELVE_HK_COUNT,
    RANK_THIRTEEN_HK_COUNT,
    RANK_FOURTEEN_HK_COUNT
};

using RankThresholdArray = std::array<uint32, MAX_RANK>;

RankThresholdArray GetConfiguredKillThresholds()
{
    RankThresholdArray thresholds{};
    for (uint8 rank = RANK_ONE; rank < MAX_RANK; ++rank)
        thresholds[rank] = GetRequiredKills(RankConfigKeys[rank], RankDefaultKills[rank]);

    return thresholds;
}

uint8 GetHighestEligibleRank(uint32 kills, RankThresholdArray const& thresholds)
{
    uint8 highestRank = MAX_RANK;
    for (uint8 rank = RANK_ONE; rank < MAX_RANK; ++rank)
    {
        if (kills >= thresholds[rank])
            highestRank = rank;
    }

    return highestRank;
}

template <typename Func>
void ForEachRankTitle(uint8 rank, Func&& func)
{
    for (size_t teamIndex = 0; teamIndex < 2; ++teamIndex)
    {
        uint32 const titleId = TitleData[rank][teamIndex];
        if (!titleId)
            continue;

        if (CharTitlesEntry const* titleEntry = sCharTitlesStore.LookupEntry(titleId))
            func(teamIndex, titleEntry, TitleNameData[rank][teamIndex]);
    }
}

void EnsureTitleStringsPatched()
{
    static bool initialized = false;
    if (initialized)
        return;

    initialized = true;

    for (uint8 rank = RANK_ONE; rank < MAX_RANK; ++rank)
    {
        for (size_t teamIndex = 0; teamIndex < 2; ++teamIndex)
        {
            uint32 const titleId = TitleData[rank][teamIndex];
            if (!titleId)
                continue;

            if (CharTitlesEntry const* titleEntry = sCharTitlesStore.LookupEntry(titleId))
            {
                if (char const* titleName = TitleNameData[rank][teamIndex])
                {
                    CharTitlesEntry* mutableEntry = const_cast<CharTitlesEntry*>(titleEntry);
                    for (uint8 locale = 0; locale < TOTAL_LOCALES; ++locale)
                    {
                        mutableEntry->Name[locale] = titleName;
                        mutableEntry->Name1[locale] = titleName;
                    }
                }
            }
        }
    }
}

void UpdateKnownTitle(Player* player, CharTitlesEntry const* titleEntry, bool remove)
{
    uint32 const fieldIndexOffset = titleEntry->MaskID / 32;
    uint32 const flag = 1 << (titleEntry->MaskID % 32);

    if (remove)
    {
        if (!player->HasFlag(PLAYER__FIELD_KNOWN_TITLES + fieldIndexOffset, flag))
            return;

        player->RemoveFlag(PLAYER__FIELD_KNOWN_TITLES + fieldIndexOffset, flag);

        if (player->GetUInt32Value(PLAYER_CHOSEN_TITLE) == titleEntry->MaskID)
            player->SetUInt32Value(PLAYER_CHOSEN_TITLE, 0);
    }
    else
    {
        if (player->HasFlag(PLAYER__FIELD_KNOWN_TITLES + fieldIndexOffset, flag))
            return;

        player->SetFlag(PLAYER__FIELD_KNOWN_TITLES + fieldIndexOffset, flag);
    }
}

void RemoveTitlesBelowRank(Player* player, uint8 highestRank)
{
    if (highestRank >= MAX_RANK)
        return;

    for (uint8 rank = RANK_ONE; rank < highestRank; ++rank)
    {
        ForEachRankTitle(rank, [player](size_t /*teamIndex*/, CharTitlesEntry const* titleEntry, char const* /*titleName*/)
        {
            if (!player->HasTitle(titleEntry))
                return;

            UpdateKnownTitle(player, titleEntry, true);
        });
    }
}

void RemoveTitlesBelowHighestEligibleRank(Player* player)
{
    if (!sConfigMgr->GetBoolDefault("PvPTitles.RemoveLowerTitles", false))
        return;

    uint32 const kills = player->GetUInt32Value(PLAYER_FIELD_LIFETIME_HONORABLE_KILLS);
    RankThresholdArray const thresholds = GetConfiguredKillThresholds();
    uint8 const highestEligibleRank = GetHighestEligibleRank(kills, thresholds);

    if (highestEligibleRank < MAX_RANK)
        RemoveTitlesBelowRank(player, highestEligibleRank);
}

class PvPTitlesPlayerScript : public PlayerScript
{
public:
    PvPTitlesPlayerScript() : PlayerScript("PvPTitlesPlayerScript") { }

    void OnLogin(Player* player, bool /*firstLogin*/) override
    {
        EnsureTitleStringsPatched();

        if (!sConfigMgr->GetBoolDefault("PvPTitles.Enable", false))
            return;

        if (sConfigMgr->GetBoolDefault("PvPTitles.Announce", true))
            ChatHandler(player->GetSession()).SendSysMessage("This server is running the |cff4CFF00PvPTitles |rmodule.");

        int32 const cleanUpMode = sConfigMgr->GetIntDefault("PvPTitles.CleanUp", CLEAN_UP_NONE);
        if (cleanUpMode != CLEAN_UP_NONE)
            CleanUpTitles(cleanUpMode, player);

        if (sConfigMgr->GetBoolDefault("PvPTitles.AwardTitlesOnLogin", false))
            AwardEarnedTitles(player);

        RemoveTitlesBelowHighestEligibleRank(player);
    }

    void OnPVPKill(Player* killer, Player* killed) override
    {
        EnsureTitleStringsPatched();

        if (!sConfigMgr->GetBoolDefault("PvPTitles.Enable", false))
            return;

        if (killer == killed)
            return;

        AwardEarnedTitles(killer);
    }

private:
    void AwardEarnedTitles(Player* player)
    {
        uint32 const kills = player->GetUInt32Value(PLAYER_FIELD_LIFETIME_HONORABLE_KILLS);
        RankThresholdArray const thresholds = GetConfiguredKillThresholds();
        uint8 const highestEligibleRank = GetHighestEligibleRank(kills, thresholds);
        bool const removeLower = sConfigMgr->GetBoolDefault("PvPTitles.RemoveLowerTitles", false);

        for (uint8 rank = RANK_ONE; rank < MAX_RANK; ++rank)
        {
            if (kills < thresholds[rank])
                continue;

            std::vector<char const*> newlyAwarded;
            newlyAwarded.reserve(2);

            ForEachRankTitle(rank, [player, &newlyAwarded](size_t /*teamIndex*/, CharTitlesEntry const* titleEntry, char const* titleName)
            {
                if (player->HasTitle(titleEntry))
                    return;

                UpdateKnownTitle(player, titleEntry, false);

                if (titleName)
                    newlyAwarded.push_back(titleName);
            });

            if (!newlyAwarded.empty())
            {
                if (WorldSession* session = player->GetSession())
                {
                    if (newlyAwarded.size() == 1)
                        ChatHandler(session).PSendSysMessage("You have earned the title '%s'.", newlyAwarded[0]);
                    else
                        ChatHandler(session).PSendSysMessage("You have earned the titles '%s' and '%s'.", newlyAwarded[0], newlyAwarded[1]);
                }
            }
        }

        if (removeLower && highestEligibleRank < MAX_RANK)
            RemoveTitlesBelowRank(player, highestEligibleRank);
    }

    void CleanUpTitles(int32 mode, Player* player)
    {
        uint32 const kills = player->GetUInt32Value(PLAYER_FIELD_LIFETIME_HONORABLE_KILLS);

        RankThresholdArray const thresholds = GetConfiguredKillThresholds();
        bool const removeLower = sConfigMgr->GetBoolDefault("PvPTitles.RemoveLowerTitles", false);
        uint8 const highestEligibleRank = GetHighestEligibleRank(kills, thresholds);

        for (uint8 rank = RANK_ONE; rank < MAX_RANK; ++rank)
        {
            bool const meetsRequirement = kills >= thresholds[rank];

            ForEachRankTitle(rank, [player, mode, meetsRequirement](size_t /*teamIndex*/, CharTitlesEntry const* titleEntry, char const* /*titleName*/)
            {
                if (!player->HasTitle(titleEntry))
                    return;

                bool remove = false;
                if (mode == CLEAN_UP_REMOVE_ALL)
                    remove = true;
                else if (mode == CLEAN_UP_REMOVE_INVALID && !meetsRequirement)
                    remove = true;

                if (remove)
                    UpdateKnownTitle(player, titleEntry, true);
            });
        }

        if (removeLower && highestEligibleRank < MAX_RANK)
            RemoveTitlesBelowRank(player, highestEligibleRank);
    }
};
} // namespace

void AddSC_mod_pvp_titles()
{
    EnsureTitleStringsPatched();
    new PvPTitlesPlayerScript();
}

