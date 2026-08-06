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

#include "PlayerbotVhrWaveDriver.h"

#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "BattlegroundVHR.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotObcClone.h"
#include "SharedDefines.h"

#include <unordered_set>

namespace
{
// Instances whose current run has already had its clones torn down. An
// instance id can be recycled by a later battleground, so entries are dropped
// as soon as the id stops referring to a live match.
std::unordered_set<uint32> g_TornDownInstances;

void FulfilWaveRequest(BattlegroundVHR* bg)
{
    VhrWaveSpawnRequest const* request = bg->GetPendingWaveSpawnRequest();
    if (!request)
        return;

    // A pending request means the previous wave is fully dead (that is the
    // only way a new wave begins), so every clone this instance still holds is
    // a corpse. Take them down now rather than letting forty waves of bodies
    // and their transient sessions pile up in the player roster.
    playerbot::PlayerbotObcCloneManager::DestroyCustomGameClones(bg->GetInstanceID());

    // Copy what we need before spawning: the first successful clone mutates
    // the battleground's player roster, and AddPlayer paths may touch the
    // request's storage via world-state updates.
    uint32 const waveNumber = request->waveNumber;
    uint32 const enemyTeam = request->enemyTeam;
    std::vector<ObjectGuid> const sources = request->sourceGuids;
    std::vector<Position> const positions = request->spawnPositions;

    uint32 spawned = 0;
    for (size_t i = 0; i < sources.size() && i < positions.size(); ++i)
    {
        Player* source = ObjectAccessor::FindPlayer(sources[i]);
        if (!source)
            continue;

        Player* clone = playerbot::PlayerbotObcCloneManager::CreateCustomGameClone(source, bg, enemyTeam, "Dark ");
        if (!clone)
        {
            TC_LOG_WARN("playerbot", "PlayerbotVhrWaveDriver: failed to clone {} for wave {} of instance {}.",
                source->GetName(), waveNumber, bg->GetInstanceID());
            continue;
        }

        // CreateCustomGameClone seats the clone at the team start position;
        // its cell slot is decided by the battleground per wave.
        clone->NearTeleportTo(positions[i]);
        ++spawned;
    }

    TC_LOG_DEBUG("playerbot", "PlayerbotVhrWaveDriver: wave {} of instance {} fielded {}/{} clones.",
        waveNumber, bg->GetInstanceID(), spawned, uint32(sources.size()));

    // Report back even at zero: the battleground's wipe check will close the
    // run out, rather than this driver retrying a wave that cannot spawn.
    bg->NotifyWaveSpawnFulfilled(waveNumber, spawned);
}
}

namespace playerbot
{
void PlayerbotVhrWaveDriver::OnWorldUpdate(uint32 /*diffMs*/)
{
    BattlegroundContainer const* instances = sBattlegroundMgr->GetBattlegroundsByType(BATTLEGROUND_VHR);
    if (!instances)
    {
        g_TornDownInstances.clear();
        return;
    }

    std::unordered_set<uint32> liveIds;
    for (auto const& [instanceId, bg] : *instances)
    {
        // Entry 0 is the template, never a live match.
        if (!instanceId || !bg)
            continue;

        liveIds.insert(instanceId);

        BattlegroundVHR* vhr = static_cast<BattlegroundVHR*>(bg);
        switch (vhr->GetStatus())
        {
            case STATUS_IN_PROGRESS:
                FulfilWaveRequest(vhr);
                break;
            case STATUS_WAIT_LEAVE:
                // The run is over; take the surviving clones down with it.
                // Guarded so the teardown runs once, not every tick of the
                // leave window.
                if (g_TornDownInstances.insert(instanceId).second)
                    PlayerbotObcCloneManager::DestroyCustomGameClones(instanceId);
                break;
            default:
                break;
        }
    }

    // Forget ids that no longer name a live instance so their eventual reuse
    // is not mistaken for an already-handled run.
    for (auto itr = g_TornDownInstances.begin(); itr != g_TornDownInstances.end();)
    {
        if (liveIds.find(*itr) == liveIds.end())
            itr = g_TornDownInstances.erase(itr);
        else
            ++itr;
    }
}
}
