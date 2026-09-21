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

    void FlagForLogin(Player* player, AtLoginFlags flag)
    {
        player->SetAtLoginFlag(flag);

        // Written now as well as at logout, so a crash in between keeps it.
        CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_ADD_AT_LOGIN_FLAG);
        stmt->setUInt16(0, uint16(flag));
        stmt->setUInt32(1, player->GetGUID().GetCounter());
        CharacterDatabase.Execute(stmt);
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

            if (player->HasAtLoginFlag(AT_LOGIN_RENAME))
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Never mind the new name. I will keep the one I have.", GOSSIP_SENDER_MAIN, ACTION_CANCEL_RENAME);
            else
                AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1, "I would like a new name.", GOSSIP_SENDER_MAIN, ACTION_RENAME,
                    "Your character will be asked for a new name at the character screen. Your family name stays unless you change it there. Continue?", 0, false);

            if (player->HasAtLoginFlag(AT_LOGIN_CHANGE_RACE))
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Never mind the race change. I will stay as I am.", GOSSIP_SENDER_MAIN, ACTION_CANCEL_RACE);
            else
                AddGossipItemFor(player, GOSSIP_ICON_INTERACT_1, "I would like to change my race.", GOSSIP_SENDER_MAIN, ACTION_CHANGE_RACE,
                    "A Race Change button will appear on this character at the character screen. You trade your racial abilities for the new race's. Continue?", 0, false);

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
                    FlagForLogin(player, AT_LOGIN_RENAME);
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
                    FlagForLogin(player, AT_LOGIN_CHANGE_RACE);
                    chat.SendSysMessage("Log out to the character screen and press Race Change on this character.");
                    player->GetSession()->SendNotification("Log out to change your race.");
                    break;
                case ACTION_CANCEL_RENAME:
                    player->RemoveAtLoginFlag(AT_LOGIN_RENAME, true);
                    chat.SendSysMessage("Your name stays as it is.");
                    break;
                case ACTION_CANCEL_RACE:
                    player->RemoveAtLoginFlag(AT_LOGIN_CHANGE_RACE, true);
                    chat.SendSysMessage("Your race stays as it is.");
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
