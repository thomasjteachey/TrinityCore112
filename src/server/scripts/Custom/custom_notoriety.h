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

#ifndef CUSTOM_NOTORIETY_H
#define CUSTOM_NOTORIETY_H

#include "Define.h"
#include "ObjectGuid.h"
#include "Position.h"

class Player;
class Quest;

// ---------------------------------------------------------------------------
// Notoriety: selling the price on your own head.
//
// NAMING. "Bounty" is the internal codename for the whole system and stays that
// way on purpose - it is 320 source occurrences and 29 config keys, several of
// which are set in the live configs, where a rename silently reverts to the
// compiled default on the next .reload config with no error line. NOTORIETY is
// what a player reads: the aura's name, and the contract below. Do not "fix"
// the inconsistency.
//
// THE FEATURE. Past ContractStacks marks, Grix Warbrand - the registrar who
// writes bounties down but does not pay them - will give you a name and a
// bearing. A fence stands somewhere out in the world, several hundred yards
// off, and will buy the page off you for coin and experience.
//
// The walk is the whole feature. Carrying a contract HOLDS the notoriety aura
// up rather than letting it decay, which means you stay lit and the fleet stays
// on you for the entire journey. In exchange the contract banks the HIGHEST
// stack count you reach while carrying it - so every hunter you put down on the
// road raises the payout and none of them can lower it. The walk is not a thing
// to survive, it is a thing to fight through.
//
// And if you die, the contract dies at the corpse. Nobody buys a dead man's
// name.
// ---------------------------------------------------------------------------
namespace Notoriety
{
    bool Enabled();

    // The quest that carries the contract, so callers do not hardcode it.
    uint32 QuestId();

    // Marks required before the registrar will write a contract at all.
    uint32 ContractStacks();

    // Eligible, carrying no live contract, and off the cooldown from the last
    // one. This is what puts the ! over Grix's head and the quest in his menu.
    bool ShouldOffer(Player const* player);

    bool HasLiveContract(ObjectGuid guid);

    // Where this player's fence is standing. False when there is no contract.
    bool GetRendezvous(ObjectGuid guid, uint32& mapId, uint32& zoneId, Position& out);

    // Called from the bounty engine on the exact instruction that raises a
    // stack, with the player in hand on the map thread. Banks the peak.
    void OnStacksChanged(Player* player, uint32 newStacks);

    // Pick a rendezvous, persist it, tell the player where to go. False if no
    // ground could be found, in which case the caller must not leave the quest
    // in the log.
    bool IssueContract(Player* player);

    // Re-roll an existing contract to a different fence. Free to the player and
    // deliberately so: a rendezvous the map stranded them behind is a support
    // ticket otherwise.
    bool RerollContract(Player* player);

    // Void it, loudly, wherever the player is standing. Called at the corpse.
    void VoidContract(Player* player, char const* reason);

    // Re-send the map marker and the bearing.
    void SendRendezvousPoi(Player* player);

    // Everything the payout is computed from, in one place, so the goodie bag
    // does not have to re-derive any of it.
    struct Payout
    {
        Player* Who = nullptr;
        uint32 Stacks = 0;      // the banked peak, not the live count
        uint32 Tier = 0;        // 0 at ContractStacks, +1 per StacksPerTier over
        uint32 ZoneId = 0;
        uint32 MoneyPaid = 0;
        uint32 XpPaid = 0;
        uint8 Level = 0;
    };

    // THE SEAM. Money and experience are paid before this is called; this is
    // where the bag of level-appropriate loot goes when it exists. Deliberately
    // a no-op today so the shape is already right when it lands.
    void GrantGoodieBag(Payout const& payout);
}

#endif // CUSTOM_NOTORIETY_H
