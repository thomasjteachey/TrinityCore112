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
struct PvpValues;
struct RandomBotParticipationHooks;

class RandomBotParticipationLifecycle
{
public:
    // Manager-facing seam for per-bot update cadence/eligibility ownership.
    static void ProcessManagerLifecycleEntryPoint(Player* player);

    // Lifecycle seam: dispatcher only, executes decisions from context.
    static void ProcessLifecycleEntryPoint(Player* player);

    // Seam entry points for future random-bot manager wiring (Phase 4+).
    static void ProcessBattlegroundLifecycleEntryPoint(Player* player, PvpValues const& values, RandomBotParticipationHooks const& hooks);
    static void ProcessArenaLifecycleEntryPoint(Player* player, PvpValues const& values, RandomBotParticipationHooks const& hooks);
};
}

#endif
