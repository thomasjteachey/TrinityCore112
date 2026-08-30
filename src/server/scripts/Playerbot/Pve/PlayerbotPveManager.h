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

#ifndef TRINITY_PLAYERBOT_PVE_MANAGER_H
#define TRINITY_PLAYERBOT_PVE_MANAGER_H

#include "Define.h"
#include "ObjectGuid.h"

#include <string>
#include <vector>

class Player;

namespace playerbot
{
struct PveConfig
{
    bool enabled = false;
    float companionFollowDistance = 2.5f;
    float companionAssistRadius = 45.0f;
    uint32 autoReviveSeconds = 30;
    float restHealthPct = 60.0f;
    float restManaPct = 50.0f;
    bool autoLearnSpellsOnLevelUp = true;
    bool grindEnabled = false;
    float grindSearchRadius = 60.0f;
    // Navmesh A* queries the whole bot fleet may issue per second. These run
    // on the map-update thread that also resolves human combat, so this is
    // really a cap on how much player-visible latency the bots may cause.
    uint32 pathBudgetPerSecond = 150;
    float grindWanderRadius = 40.0f;
    uint32 grindMaxLevelAbove = 3;
    uint32 grindMaxLevelBelow = 5;
    bool grindAllowElites = false;
    bool lootEnabled = true;
    bool vendorEnabled = true;
    bool questsEnabled = true;
    bool equipUpgradesEnabled = true;
    bool buffsEnabled = true;
    bool talentsEnabled = true;
    bool relocateEnabled = true;
    uint32 relocateDryWanders = 5;
    std::vector<uint32> relocateMaps = { 0, 1 };
    bool combatDiagnostics = false;
    // Journeys: destinations within this range are WALKED (segmented mmap
    // pathing) instead of teleported; 0 restores teleport-only travel.
    float travelWalkMaxDistance = 900.0f;
    // Real flight-master routes for longer travel (BFS over taxi edges,
    // bot pays the fare); teleport remains the last fallback.
    bool travelUseFlightPaths = true;
    // Bots browse their faction auction house and buy affordable gear
    // upgrades (delivered by mail, which bots now collect).
    bool auctionBuyEnabled = false;
    uint32 auctionBuyBudgetPct = 30;
    // Bots list the gear they cannot use, undercutting the standing price.
    bool auctionSellEnabled = false;
    // Gathering professions: two of herbalism/mining/skinning per bot,
    // auto-learned and ranked, nodes gathered mid-grind, corpses skinned.
    bool professionsEnabled = false;
    // Realm economy switch: off = the free eat/drink spells (L+ style);
    // on = bots buy real food/water/ammo from vendors and consume them (B+).
    bool restUseConsumables = false;
    // Accounts whose bots are PvP-only: no PvE behavior of any kind, they
    // idle at their sanctuary between battleground queues.
    std::vector<uint32> pvpOnlyAccountIds;
    // This percent of the fleet (guid-keyed, deterministic) is reborn at
    // the level cap: full level-1 reset and sent home. 0 disables.
    uint32 rebirthAtMaxLevelPercent = 0;
    // Hardcore realms: bots never join groups - invites are declined and
    // companion summons refused.
    bool declineGroupInvites = false;
    // The hardcore death-chest gameobject: bots loot these like any prize.
    uint32 hardcoreLootChestEntry = 0;
    // How long such a chest stands before despawning - a corpse run is only
    // worth starting while it could still be there.
    uint32 hardcoreChestDespawnSeconds = 600;
    // Zone guardians: this many bots per classic zone are pinned there at
    // the zone's classic level cap with XP gain frozen. 0 disables.
    uint32 zoneGuardiansPerZone = 0;
};

// Open-world PvE behavior for managed random bots, layered on the existing
// PvP decision engine: companion mode (grouped with a human: follow, assist,
// rest, revive) and grind mode (idle bots fight nearby creatures to level).
// Combat itself reuses PvpCore::BuildClassSpellContext through the PvE
// engagement flag; this manager only owns target selection and the
// out-of-combat life around it.
class PveManager
{
public:
    static void LoadConfig();
    static PveConfig const& GetConfig();

    // Called from the world-update hook on the world thread: finalizes pending
    // companion summons/teleports and drains queued group-invite acceptances,
    // so all group mutations happen on the world thread.
    static void OnWorldUpdate(uint32 diffMs);

    // Called from RandomBotParticipationManager::ProcessPlayerLifecycle on the
    // bot's map-update thread (under the per-map decision lock). Internally
    // cadence-gated; inert inside battlegrounds and duels.
    static void OnPlayerLifecycleTick(Player* player);

    static void OnBotLogout(Player const* player);

    // True while a bot is companion-bound to a human (or mid-summon): such a
    // bot must not be queued for battlegrounds or logged out by the
    // population rebalancer.
    static bool IsExemptFromBattlegroundOrchestration(Player const* player);

    // Master -> bot whisper command surface ("follow", "stay", "attack",
    // "passive", "come", "dismiss"). Returns true when the whisper was a
    // recognized PvE command and has been handled.
    static bool HandleWhisperCommand(Player* sender, Player* botReceiver, std::string const& command);

    // .playerbot pve summon <name>: logs the pool character in if needed,
    // teleports it to the summoner and joins the summoner's group.
    static bool RequestCompanionSummon(Player* summoner, std::string const& characterName, std::string& statusMessage);
    static bool RequestCompanionDismiss(Player* requester, Player* bot, std::string& statusMessage);

    // .playerbot pve reset [percent]: strips that share of the online
    // managed bots (companions excluded) back to freshly created level-1
    // characters and ports them to their racial starting spots.
    static uint32 ResetBotsToLevelOne(uint8 percent);

    // True for bots on Playerbot.Pve.PvpOnlyAccountIds: they skip every
    // PvE system and only answer the battleground orchestration.
    static bool IsPvpOnlyBot(Player const* player);

    // Config-gated trainer-spell catch-up whenever a managed bot levels.
    static void OnManagedBotLevelChanged(Player* player, uint8 oldLevel);

    static std::string BuildStatusLine(Player const* bot);
};
}

#endif
