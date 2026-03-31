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

#include "Battleground.h"
#include "Log.h"
#include "ScriptMgr.h"

#include <unordered_set>

namespace
{
    class PlayerbotBGStateTracker : public BGScript
    {
        public:
            PlayerbotBGStateTracker() : BGScript("playerbot_bg_state_tracker") { }

            void OnBattlegroundStart(Battleground* bg) override
            {
                if (!bg)
                    return;

                uint32 instanceId = bg->GetInstanceID();
                _activeBattlegrounds.insert(instanceId);

                TC_LOG_INFO("bg.playerbot", "Playerbot BG tracker: started battleground '{}' (type {}, instance {})",
                    bg->GetName(), uint32(bg->GetTypeID()), instanceId);
            }

            void OnBattlegroundEnd(Battleground* bg, TeamId winnerTeam) override
            {
                if (!bg)
                    return;

                uint32 instanceId = bg->GetInstanceID();
                _activeBattlegrounds.erase(instanceId);

                TC_LOG_INFO("bg.playerbot", "Playerbot BG tracker: ended battleground '{}' (type {}, instance {}, winnerTeam {})",
                    bg->GetName(), uint32(bg->GetTypeID()), instanceId, uint32(winnerTeam));
            }

        private:
            std::unordered_set<uint32> _activeBattlegrounds;
    };
}

void AddSC_playerbot_bg_integration()
{
    new PlayerbotBGStateTracker();
}
