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

// Counts deaths per zone for the weekly "died the most" board.
//
// Only the counting lives here. Picking each zone's leader and dressing that
// zone's inn corpse is World::ProcessWeeklyMostDeaths, next to the warchief code
// it borrows from - UpdateHonorNpc is a static in World.cpp's anonymous namespace
// and cannot be reached from a script, and nothing in game/ calls into
// scripts/Custom, so the two halves meet in the database rather than across a
// link boundary.

#include "custom_barracks_hardcore.h"

#include "Configuration/Config.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Player.h"
#include "ScriptMgr.h"

#include <atomic>

namespace
{
    // Probed once at startup: the first write to a missing table would abort the
    // worldserver, and a realm on this branch may not have it yet.
    std::atomic<bool> ZoneDeathsTableExists{ false };

    bool WeeklyDeathBoardEnabled()
    {
        return ZoneDeathsTableExists && sConfigMgr->GetBoolDefault("Centurion.MostDeaths.Enable", false);
    }
}

class weekly_deaths_world_script : public WorldScript
{
public:
    weekly_deaths_world_script() : WorldScript("weekly_deaths_world_script") { }

    void OnStartup() override
    {
        ZoneDeathsTableExists = bool(CharacterDatabase.Query(
            "SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'character_weekly_zone_deaths'"));

        if (!ZoneDeathsTableExists && sConfigMgr->GetBoolDefault("Centurion.MostDeaths.Enable", false))
            TC_LOG_ERROR("server.loading", "Centurion.MostDeaths.Enable is on but table character_weekly_zone_deaths is missing: deaths are not counted.");
    }
};

class weekly_deaths_player_script : public PlayerScript
{
public:
    weekly_deaths_player_script() : PlayerScript("weekly_deaths_player_script") { }

    void OnPlayerJustDied(Player* victim, Unit* /*killer*/) override
    {
        if (!victim || !WeeklyDeathBoardEnabled())
            return;

        // Playerbots are excluded, and this is the whole reason the board needs a
        // filter at all: the fleet dies hundreds of times a day while grinding, so a
        // bot would win every single week and the board would never show a person.
        if (BarracksHardcore::IsPlayerbot(victim))
            return;

        // Counted against the zone the victim died in: each inn's corpse shows its
        // own zone's leader.
        CharacterDatabase.Execute(
            "INSERT INTO character_weekly_zone_deaths (guid, zone, deaths) VALUES ({}, {}, 1) "
            "ON DUPLICATE KEY UPDATE deaths = deaths + 1",
            victim->GetGUID().GetCounter(), victim->GetZoneId());
    }
};

void AddSC_custom_weekly_deaths()
{
    new weekly_deaths_world_script();
    new weekly_deaths_player_script();
}
