/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * The area trigger at the mouth of Onyxia's Lair. Unlike Naxxramas, the lair has
 * no teleporter to route people through - players walk in through the front
 * door - so this is what decides which Onyxia they meet, and it does it for
 * them rather than asking them to set a raid difficulty by hand.
 *
 * At or below VanillaRaids.MaxEntryLevel it switches the player and their group
 * to the 40-player difficulty; above it, it leaves the difficulty alone and they
 * get the level 80 lair as before.
 *
 * Ported from mod-individual-progression (ZhengPeiRu21, AzerothCore, AGPL-3.0).
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

#include "ScriptMgr.h"
#include "Chat.h"
#include "Group.h"
#include "Player.h"
#include "VanillaRaids/VanillaRaids.h"
#include <string>

enum OnyxiaEntrance40
{
    ITEM_DRAKEFIRE_AMULET = 18814,
    MIN_ENTRY_LEVEL       = 50
};

namespace
{
    // Whether this character may be switched to the 40-player lair.
    // reason is filled in with something the raid leader can act on.
    bool CanEnterOnyxia40(Player* player, std::string& reason)
    {
        if (!player)
            return false;

        if (player->IsGameMaster() || VanillaRaids::IsBotAccount(player))
            return true;

        if (player->GetLevel() < MIN_ENTRY_LEVEL)
        {
            reason = "needs to be at least level 50";
            return false;
        }

        // Off by default on this realm - the Drakefire Amulet chain is the
        // vanilla attunement, and VanillaRaids.Attunement.Enable governs it.
        if (VanillaRaids::AttunementRequired() && !player->HasItemCount(ITEM_DRAKEFIRE_AMULET))
        {
            reason = "does not have the Drakefire Amulet";
            return false;
        }

        return true;
    }
}

class onyxia_entrance_trigger : public AreaTriggerScript
{
public:
    onyxia_entrance_trigger() : AreaTriggerScript("onyxia_entrance_trigger") { }

    bool OnTrigger(Player* player, AreaTriggerEntry const* /*trigger*/) override
    {
        if (!player || !player->IsInWorld())
            return false;

        if (!VanillaRaids::Onyxia40Enabled())
            return false;

        // Above the cap this trigger does nothing at all, so a level 80 raid
        // walks into the level 80 lair exactly as it did before.
        if (player->GetLevel() > VanillaRaids::MaxEntryLevel())
            return false;

        ChatHandler handler(player->GetSession());

        std::string reason;
        if (!CanEnterOnyxia40(player, reason))
        {
            handler.PSendSysMessage("You %s.", reason.c_str());
            return false;
        }

        player->SetRaidDifficulty(VanillaRaids::VanillaRaidDifficulty);

        if (Group* group = player->GetGroup())
        {
            group->SetRaidDifficulty(VanillaRaids::VanillaRaidDifficulty);

            for (GroupReference* itr = group->GetFirstMember(); itr; itr = itr->next())
            {
                Player* member = itr->GetSource();
                if (!member || member->GetGUID() == player->GetGUID())
                    continue;

                std::string memberReason;
                if (!CanEnterOnyxia40(member, memberReason))
                {
                    handler.PSendSysMessage("|cff00ffff%s|r %s.", member->GetName().c_str(), memberReason.c_str());
                    continue;
                }

                member->SetRaidDifficulty(VanillaRaids::VanillaRaidDifficulty);
            }
        }

        // False: the trigger only decides the difficulty, it does not move anyone.
        return false;
    }
};

void AddSC_onyxia_entrance_40()
{
    new onyxia_entrance_trigger();
}
