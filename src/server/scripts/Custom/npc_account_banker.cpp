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

#include "AccountBankMgr.h"
#include "Creature.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedCreature.h"
#include "WorldSession.h"

class npc_account_banker : public CreatureScript
{
public:
    npc_account_banker() : CreatureScript("npc_account_banker") { }

    struct npc_account_bankerAI : public ScriptedAI
    {
        npc_account_bankerAI(Creature* creature) : ScriptedAI(creature) { }

        bool OnGossipHello(Player* player) override
        {
            if (!player)
                return false;

            if (!AccountBank::OpenAccountBank(player, me->GetGUID()))
            {
                if (WorldSession* session = player->GetSession())
                    session->SendNotification("Unable to open the shared account bank.");
                return false;
            }

            if (WorldSession* session = player->GetSession())
                session->SendShowBank(me->GetGUID());

            return true;
        }
    };

    CreatureAI* GetAI(Creature* creature) const override
    {
        return new npc_account_bankerAI(creature);
    }
};

class account_bank_player_script : public PlayerScript
{
public:
    account_bank_player_script() : PlayerScript("account_bank_player_script") { }

    void OnLogin(Player* player, bool /*firstLogin*/) override
    {
        AccountBank::HandleLogin(player);
    }

    void OnLogout(Player* player) override
    {
        AccountBank::CloseAccountBank(player);
    }

    void OnMapChanged(Player* player) override
    {
        AccountBank::CloseAccountBank(player);
    }
};

class account_bank_world_script : public WorldScript
{
public:
    account_bank_world_script() : WorldScript("account_bank_world_script") { }

    void OnUpdate(uint32 /*diff*/) override
    {
        AccountBank::UpdateAccountBankSessions();
    }
};

void AddSC_npc_account_banker()
{
    new npc_account_banker();
    new account_bank_player_script();
    new account_bank_world_script();
}
