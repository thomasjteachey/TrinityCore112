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

// The Gurubashi Arena's name-and-race NPC. It only sets the character's
// at_login flag; the character screen does the rest, through the stock rename
// and race change dialogs. What a race change does to the spellbook is
// Miscellaneous/RaceChange.h.

#include "ArenaTeamMgr.h"
#include "Chat.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "Player.h"
#include "ScriptedCreature.h"
#include "ScriptedGossip.h"
#include "ScriptMgr.h"
#include "WorldSession.h"

namespace
{
    // npc_text row with the greeting; the stock one shows if it is missing.
    constexpr uint32 NPC_TEXT_CHARACTER_SERVICES = 921217;

    enum CharacterServiceActions : uint32
    {
        ACTION_RENAME = GOSSIP_ACTION_INFO_DEF + 1,
        ACTION_CHANGE_RACE,
        ACTION_CANCEL_RENAME,
        ACTION_CANCEL_RACE,
    };

    // Each service, paid when it is flagged and handed back if it is called off
    // before the character screen has used it.
    constexpr uint32 SERVICE_PRICE = 100 * GOLD;

    // The flag and the gold are written together and at once, rather than at
    // the next save, so a crash in between can neither keep the gold and lose
    // the flag nor the other way round.
    void WriteNow(Player* player, AtLoginFlags flag, bool add)
    {
        CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();

        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(add ? CHAR_UPD_ADD_AT_LOGIN_FLAG : CHAR_UPD_REM_AT_LOGIN_FLAG);
        stmt->setUInt16(0, uint16(flag));
        stmt->setUInt32(1, player->GetGUID().GetCounter());
        trans->Append(stmt);

        player->SaveGoldToDB(trans);
        CharacterDatabase.CommitTransaction(trans);
    }

    bool Purchase(Player* player, AtLoginFlags flag)
    {
        if (!player->HasEnoughMoney(SERVICE_PRICE))
        {
            player->SendBuyError(BUY_ERR_NOT_ENOUGHT_MONEY, nullptr, 0, 0);
            return false;
        }

        player->ModifyMoney(-int32(SERVICE_PRICE));
        player->SetAtLoginFlag(flag);
        WriteNow(player, flag, true);
        return true;
    }

    void Refund(Player* player, AtLoginFlags flag)
    {
        player->RemoveAtLoginFlag(flag);
        player->ModifyMoney(int32(SERVICE_PRICE));
        WriteNow(player, flag, false);
    }
}

class npc_character_services : public CreatureScript
{
public:
    npc_character_services() : CreatureScript("npc_character_services") { }

    struct npc_character_servicesAI : public ScriptedAI
    {
        npc_character_servicesAI(Creature* creature) : ScriptedAI(creature) { }

        bool OnGossipHello(Player* player) override
        {
            ClearGossipMenuFor(player);

            // The popup's money line shows the price and greys the button out
            // for anyone who cannot pay it.
            if (player->HasAtLoginFlag(AT_LOGIN_RENAME))
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Never mind the new name. I will keep the one I have. (refund)", GOSSIP_SENDER_MAIN, ACTION_CANCEL_RENAME);
            else
                AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, "I would like a new name.", GOSSIP_SENDER_MAIN, ACTION_RENAME,
                    "Your character will be asked for a new name at the character screen, first and last. Continue?", SERVICE_PRICE, false);

            if (player->HasAtLoginFlag(AT_LOGIN_CHANGE_RACE))
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Never mind the race change. I will stay as I am. (refund)", GOSSIP_SENDER_MAIN, ACTION_CANCEL_RACE);
            else
                AddGossipItemFor(player, GOSSIP_ICON_MONEY_BAG, "I would like to change my race.", GOSSIP_SENDER_MAIN, ACTION_CHANGE_RACE,
                    "A Race Change button will appear on this character at the character screen. You trade your racial abilities for the new race's. Continue?", SERVICE_PRICE, false);

            SendGossipMenuFor(player, NPC_TEXT_CHARACTER_SERVICES, me);
            return true;
        }

        bool OnGossipSelect(Player* player, uint32 /*menuId*/, uint32 gossipListId) override
        {
            uint32 const action = player->PlayerTalkClass->GetGossipOptionAction(gossipListId);
            CloseGossipMenuFor(player);

            ChatHandler chat(player->GetSession());
            switch (action)
            {
                case ACTION_RENAME:
                    if (player->HasAtLoginFlag(AT_LOGIN_RENAME) || !Purchase(player, AT_LOGIN_RENAME))
                        break;
                    chat.SendSysMessage("Log out to the character screen to choose your new name.");
                    player->GetSession()->SendNotification("Log out to choose your new name.");
                    break;
                case ACTION_CHANGE_RACE:
                    // The character screen refuses an arena team captain with a
                    // bare error code; say so here, where it can be explained.
                    if (sArenaTeamMgr->GetArenaTeamByCaptain(player->GetGUID()))
                    {
                        chat.SendSysMessage("An arena team captain cannot change race. Hand the captaincy to a teammate first.");
                        break;
                    }
                    if (player->HasAtLoginFlag(AT_LOGIN_CHANGE_RACE) || !Purchase(player, AT_LOGIN_CHANGE_RACE))
                        break;
                    chat.SendSysMessage("Log out to the character screen and press Race Change on this character.");
                    player->GetSession()->SendNotification("Log out to change your race.");
                    break;
                // Checked again: the menu may be older than the flag.
                case ACTION_CANCEL_RENAME:
                    if (!player->HasAtLoginFlag(AT_LOGIN_RENAME))
                        break;
                    Refund(player, AT_LOGIN_RENAME);
                    chat.SendSysMessage("Your name stays as it is, and your gold is returned.");
                    break;
                case ACTION_CANCEL_RACE:
                    if (!player->HasAtLoginFlag(AT_LOGIN_CHANGE_RACE))
                        break;
                    Refund(player, AT_LOGIN_CHANGE_RACE);
                    chat.SendSysMessage("Your race stays as it is, and your gold is returned.");
                    break;
                default:
                    break;
            }

            return true;
        }
    };

    CreatureAI* GetAI(Creature* creature) const override
    {
        return new npc_character_servicesAI(creature);
    }
};

void AddSC_npc_character_services()
{
    new npc_character_services();
}
