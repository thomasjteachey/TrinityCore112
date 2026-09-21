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

#ifndef CUSTOM_BARRACKS_HARDCORE_H
#define CUSTOM_BARRACKS_HARDCORE_H

#include "Define.h"

#include <string>

class Player;

namespace BarracksHardcore
{
    // Copper as a player reads it: "12g 40s", with zero parts left out.
    // Exported so a price quoted on a gossip row and a loss reported in chat
    // are written the same way.
    std::string FormatMoney(uint32 copper);

    // Whether this character is one of the fleet, by account id.
    //
    // Exported because the bot account set is configured in exactly one
    // place and a second copy of the answer would drift from it. The bounty
    // ruleset needs it to decide how much of a corpse's gold burns.
    bool IsPlayerbot(Player const* player);

    // Inside a dungeon or a raid - instance types 1 and 2, so raids count and a
    // battleground or arena does not.
    //
    // Exported because "a death in here takes nothing from you" is one rule with
    // two halves in two files: the gear the hardcore cache would have staked,
    // and the coin the bounty ruleset would have taxed and left on the floor. A
    // second copy of the map test is one edit away from the halves disagreeing
    // about where a dungeon begins.
    bool IsInstancedContent(Player const* player);

    // A death that must not leave a death chest. Chromie's executions in the
    // Gurubashi Arena (custom_gurubashi_arena.cpp) set it around their
    // Unit::Kill and clear it right after, so the mark never outlives the kill
    // it was set for.
    void SetDeathChestSuppressed(Player const* player, bool suppressed);

    // The Gurubashi Arena as a whole: the grounds (1741), the catacombs under
    // them (2177) and the sand itself (30232, a WMOAreaTable id).
    //
    // Exported because the arena is one place that happens to be spelled with
    // three area ids, and a second copy of that list would be one edit away
    // from disagreeing about where the arena ends. The PvP consumable top-up
    // in custom_gurubashi_arena.cpp asks the same question this file's War
    // Mode and death-chest rules do.
    bool IsInGurubashiArena(Player const* player);

    // Whether this PERSON has armed War Mode.
    //
    // False for every playerbot by construction - a bot has no setting to read -
    // so this answers "is this someone who came here looking for a fight", which
    // is the question the playerbot manager asks before it sends anyone after
    // them. Exported rather than copied because the opt-in set is loaded and
    // maintained in exactly one place.
    bool IsWarModeOptedIn(Player const* player);

    // Whether War Mode is PAUSED for this person: opted in, standing in an open
    // world zone whose level band they are above.
    //
    // War Mode does not travel downhill, but it is suspended rather than
    // refused. The earlier rule turned people out of the zone altogether and it
    // was a wall in front of the 99% of journeys that were innocent - flying
    // over Mulgore, riding to a capital, helping a friend through Westfall. So
    // nobody is moved any more: they simply stop being a combatant while they
    // are down there, in both directions.
    //
    // "Above the band's top", not "above its bottom": a level 35 in a 30-40
    // zone is doing that zone's content. Strictly above, so a max-level player
    // is never paused in a zone that tops out at the cap - which is every zone
    // they have left to play in.
    //
    // The Gurubashi Arena is the one level-blind case: paused everywhere in it
    // except on the Battle Ring floor, where everyone fights.
    //
    // Exported because three separate places need the same answer and none of
    // them may keep its own copy: the FFA ruleset (which disarms), the gate
    // script (which wears the aura and speaks the lines) and, through
    // Player::IsWarModePaused, the core's attack and assist checks.
    bool IsWarModePaused(Player const* player);

    // True when a fight between people can actually happen in this zone.
    //
    // Exported so the playerbot manager asks the same question the FFA ruleset
    // answers, rather than keeping a second copy of the zone table that would
    // drift out of step with this one. A bot that travels to pick a fight in a
    // zone where no fight is possible is not aggressive, it is lost.
    bool IsOpenWorldPvpZone(uint32 zoneId);

    // What level this zone tops out at.
    //
    // The realm's own table, written because AreaTableEntry::ExplorationLevel is
    // ZERO for every zone in this rebuilt DBC - reading that armed nothing,
    // anywhere. Exported so the bounty guards are sized by the same answer the
    // FFA rules use, rather than a second copy that could drift from it.
    // Unknown zones (Outland, Northrend, dungeons) return 60.
    uint8 ZoneTopLevel(uint32 zoneId);

    // Whether the world can produce this item at all: sold by a vendor,
    // dropped by something, or handed to a new character. Fails open when the
    // set has not been built, so nothing is destroyed on a cold cache.
    bool IsObtainableInWorld(uint32 itemId);

    // An item no death ever takes: never staked into the full-loot cache, never
    // burned by the deflation roll, and skipped by the Semi-Hardcore penalty.
    //
    // Written for the class Insignias, which every innkeeper on the realm hands
    // out free. There is nothing to win by taking one - the loser walks to the
    // nearest inn and asks for another - and a PvP trinket that disappears on
    // death is missing exactly when the next fight starts.
    //
    // Configured as Centurion.Hardcore.DeathProofItems. Exported because the
    // Semi-Hardcore penalty lives in ChallengeModes.cpp and has to answer this
    // the same way the cache does; two lists would drift.
    bool IsDeathProofItem(uint32 itemId);

    // Fill every empty equipment slot with the white field kit, and replace worn
    // kit that has fallen too far behind the wearer. Gear the character actually
    // earned is never touched.
    //
    // Exported for the zone-band rebirth. That reset strips the bot to re-level
    // it, and the loose-kit sweep then destroys the pieces it just unequipped -
    // correctly, since a kit piece in a bag is normally debris - so a bot that
    // died and was reborn in the same breath came back with whatever the starter
    // outfit gave it and nothing else. Issuing the kit at the END of the reset,
    // at the bot's NEW level, is the order that leaves it dressed.
    //
    // Transient copies are left alone here; see IssueWhiteFieldKitToCopy.
    void IssueWhiteFieldKit(Player* player);

    // The same kit for a transient copy - a battleground fill clone, an Obsidian
    // Colosseum mirror, a custom-game, Violet Hold or bounty copy - judged by its
    // SOURCE: a copy of a playerbot gets the playerbot level offset, and a copy
    // of a tournament character gets no kit at all. The copy's own session has
    // no account, so judged by itself it would be kitted as a person.
    //
    // Call it while the copy is being built, before it is seated anywhere: in
    // a battleground only weapons and trinkets may change, so a copy that is
    // not dressed on the way in stays short of armour for the whole match.
    void IssueWhiteFieldKitToCopy(Player* copy, Player const* source);
}

#endif
