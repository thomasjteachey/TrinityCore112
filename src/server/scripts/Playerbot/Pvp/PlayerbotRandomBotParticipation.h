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

#ifndef TRINITY_PLAYERBOT_RANDOM_BOT_PARTICIPATION_H
#define TRINITY_PLAYERBOT_RANDOM_BOT_PARTICIPATION_H

class Player;

namespace playerbot
{
class RandomBotParticipationLifecycle
{
public:
    // Neutral registration seam for manager-facing integration wiring.
    static void RegisterManagerHooks();

    // Manager-facing seam: neutral lifecycle dispatcher for a random bot player.
    static void ProcessLifecycleEntryPoint(Player* player);

    // Seam entry points for future random-bot manager wiring (Phase 4+).
    static void ProcessBattlegroundLifecycleEntryPoint(Player* player);
    static void ProcessArenaLifecycleEntryPoint(Player* player);
};
}

#endif
