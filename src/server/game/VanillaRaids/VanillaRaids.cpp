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

#include "VanillaRaids.h"
#include "Config.h"
#include "Log.h"
#include "Map.h"
#include "Object.h"
#include "Player.h"
#include "QuestDef.h"
#include "WorldSession.h"
#include <algorithm>
#include <sstream>
#include <vector>

namespace
{
    struct Settings
    {
        bool Enabled              = false;
        bool Naxx40               = true;
        bool Onyxia40             = true;

        bool AttunementRequired   = false;
        bool StratholmeRequired   = false;
        uint8 MaxEntryLevel       = 70;

        bool NerfFourHorsemen     = false;
        bool NerfPatchwerk        = false;
        bool NerfRazuvious        = false;
        bool NerfGluth            = false;

        std::vector<uint32> BotAccountIds;
    };

    Settings s_settings;

    std::vector<uint32> ParseAccountIds(std::string const& raw)
    {
        std::vector<uint32> ids;
        std::stringstream stream(raw);
        std::string token;
        while (std::getline(stream, token, ','))
            if (!token.empty())
                ids.push_back(uint32(std::strtoul(token.c_str(), nullptr, 10)));
        return ids;
    }
}

namespace VanillaRaids
{
    void LoadConfig()
    {
        Settings loaded;

        loaded.Enabled            = sConfigMgr->GetBoolDefault("VanillaRaids.Enable", false);
        loaded.Naxx40             = sConfigMgr->GetBoolDefault("VanillaRaids.Naxx40.Enable", true);
        loaded.Onyxia40           = sConfigMgr->GetBoolDefault("VanillaRaids.Onyxia40.Enable", true);

        loaded.AttunementRequired = sConfigMgr->GetBoolDefault("VanillaRaids.Attunement.Enable", false);
        loaded.StratholmeRequired = sConfigMgr->GetBoolDefault("VanillaRaids.RequireStratholmeEntrance", false);

        int32 const maxLevel      = sConfigMgr->GetIntDefault("VanillaRaids.MaxEntryLevel", 70);
        loaded.MaxEntryLevel      = uint8(std::clamp<int32>(maxLevel, 1, 255));

        loaded.NerfFourHorsemen   = sConfigMgr->GetBoolDefault("VanillaRaids.Naxx40.Nerf.FourHorsemen", false);
        loaded.NerfPatchwerk      = sConfigMgr->GetBoolDefault("VanillaRaids.Naxx40.Nerf.Patchwerk", false);
        loaded.NerfRazuvious      = sConfigMgr->GetBoolDefault("VanillaRaids.Naxx40.Nerf.Razuvious", false);
        loaded.NerfGluth          = sConfigMgr->GetBoolDefault("VanillaRaids.Naxx40.Nerf.Gluth", false);

        // Same list the rest of the playerbot systems read.
        loaded.BotAccountIds      = ParseAccountIds(sConfigMgr->GetStringDefault("Playerbot.RandomPopulation.BotAccountIds", ""));

        s_settings = std::move(loaded);

        if (s_settings.Enabled)
            TC_LOG_INFO(LogFilter, "Vanilla raids enabled: Naxxramas 40 {}, Onyxia 40 {} (attunement {}, Stratholme entrance {})",
                s_settings.Naxx40 ? "on" : "off",
                s_settings.Onyxia40 ? "on" : "off",
                s_settings.AttunementRequired ? "required" : "not required",
                s_settings.StratholmeRequired ? "required" : "not required");
        else
            TC_LOG_INFO(LogFilter, "Vanilla raids disabled - Naxxramas and Onyxia's Lair serve only their level 80 versions.");
    }

    bool Enabled()                     { return s_settings.Enabled; }
    bool Naxx40Enabled()               { return s_settings.Enabled && s_settings.Naxx40; }
    bool Onyxia40Enabled()             { return s_settings.Enabled && s_settings.Onyxia40; }

    bool AttunementRequired()          { return s_settings.Enabled && s_settings.AttunementRequired; }
    bool StratholmeEntranceRequired()  { return s_settings.Enabled && s_settings.StratholmeRequired; }
    uint8 MaxEntryLevel()              { return s_settings.MaxEntryLevel; }

    bool NerfFourHorsemen()            { return s_settings.NerfFourHorsemen; }
    bool NerfPatchwerk()               { return s_settings.NerfPatchwerk; }
    bool NerfRazuvious()               { return s_settings.NerfRazuvious; }
    bool NerfGluth()                   { return s_settings.NerfGluth; }

    bool IsNaxx40Map(Map const* map)
    {
        return map && Naxx40Enabled()
            && map->GetId() == MAP_NAXXRAMAS
            && map->GetSpawnMode() == VanillaRaidDifficulty;
    }

    bool IsOnyxia40Map(Map const* map)
    {
        return map && Onyxia40Enabled()
            && map->GetId() == MAP_ONYXIAS_LAIR
            && map->GetSpawnMode() == VanillaRaidDifficulty;
    }

    bool IsVanillaRaidMap(Map const* map)
    {
        return IsNaxx40Map(map) || IsOnyxia40Map(map);
    }

    bool IsNaxx40(WorldObject const* object)
    {
        return object && IsNaxx40Map(object->GetMap());
    }

    bool IsOnyxia40(WorldObject const* object)
    {
        return object && IsOnyxia40Map(object->GetMap());
    }

    bool IsAttuned(Player const* player)
    {
        if (!player || !player->IsInWorld())
            return false;

        return player->GetQuestStatus(QUEST_NAXX40_ATTUNEMENT_1) == QUEST_STATUS_REWARDED
            || player->GetQuestStatus(QUEST_NAXX40_ATTUNEMENT_2) == QUEST_STATUS_REWARDED
            || player->GetQuestStatus(QUEST_NAXX40_ATTUNEMENT_3) == QUEST_STATUS_REWARDED;
    }

    bool IsBotAccount(Player const* player)
    {
        if (!player)
            return false;

        WorldSession const* session = player->GetSession();
        if (!session)
            return false;

        if (session->IsVirtualSession() || session->IsTransientPlayerSession())
            return true;

        return std::find(s_settings.BotAccountIds.begin(), s_settings.BotAccountIds.end(),
            session->GetAccountId()) != s_settings.BotAccountIds.end();
    }
}
