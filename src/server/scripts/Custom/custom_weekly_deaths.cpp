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

// Counts deaths for the weekly "died the most" board.
//
// Only the counting lives here. Picking the winner and dressing the corpses is
// World::ProcessWeeklyMostDeaths, next to the warchief code it borrows from -
// UpdateHonorNpc is a static in World.cpp's anonymous namespace and cannot be
// reached from a script, and nothing in game/ calls into scripts/Custom, so the
// two halves meet in the database rather than across a link boundary.

#include "custom_barracks_hardcore.h"

#include "Configuration/Config.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "Player.h"
#include "ScriptMgr.h"

namespace
{
    bool WeeklyDeathBoardEnabled()
    {
        return sConfigMgr->GetBoolDefault("Centurion.MostDeaths.Enable", false);
    }
}

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

        CharacterDatabase.Execute(
            "INSERT INTO character_weekly_deaths (guid, deaths) VALUES ({}, 1) "
            "ON DUPLICATE KEY UPDATE deaths = deaths + 1",
            victim->GetGUID().GetCounter());
    }
};

void AddSC_custom_weekly_deaths()
{
    new weekly_deaths_player_script();
}
