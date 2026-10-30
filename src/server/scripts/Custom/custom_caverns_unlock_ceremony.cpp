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

/*
 * "HAHAHAHA" - the last rung of the Warchief's Socks ladder, and the last
 * thing there is to do on the realm. Arthas <Bad Boy> takes the socks and
 * opens the Caverns Waste of Time (world quest 60006 at 921168, tournament
 * quest 26040 at 920168).
 *
 * Handing it in used to be silent: the window closed and a teleport spell
 * appeared. This script turns the hand-in into a short scene:
 *
 *   - Arthas turns to the player, laughs (the Lich King's loud laugh) and
 *     yells their name and the number of socks they gave him.
 *   - Fireworks go off over the player for the length of the scene.
 *   - The whole realm hears the laugh and gets a center-screen announcement,
 *     plus a chat line saying whether this was the realm first or the Nth
 *     character ever to finish it.
 *
 * Every quest Arthas ENDS triggers it, so the binding is the ScriptName on
 * the creature and no quest id is written here. The sock count is the one
 * the player actually paid (quest RequiredItemCount after the
 * Centurion.WorldPrices.QuestSocks override), not the quest_template value.
 */

#include "Chat.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "ItemTemplate.h"
#include "MiscPackets.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QueryCallback.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "ScriptedCreature.h"
#include "SharedDefines.h"
#include "TaskScheduler.h"
#include "World.h"
#include "WorldSession.h"

#include <atomic>
#include <memory>
#include <string>

namespace
{
    enum CavernsCeremony : uint32
    {
        SOUND_LICH_KING_LAUGH_LOUD = 14973,  // "Lich King Laughing (LOUD)"
        SOUND_ACHIEVEMENT          = 12891,  // the achievement-earned chime

        SPELL_FIREWORK_RED_WHITE_BLUE = 11543,
        SPELL_FIREWORK_BLUE           = 11540,
        SPELL_FIREWORK_GREEN          = 11541,
        SPELL_FIREWORK_RED_STREAKS    = 11542,
        SPELL_FIREWORK_YELLOW_ROSE    = 11544,
        SPELL_FIREWORK_LARGE          = 62077,  // Test of Strength Firework, large
    };

    // 1950000 -> "1,950,000"
    std::string WithThousands(uint32 value)
    {
        std::string digits = std::to_string(value);
        for (int pos = int(digits.size()) - 3; pos > 0; pos -= 3)
            digits.insert(std::size_t(pos), 1, ',');
        return digits;
    }

    // 1 -> "1st", 2 -> "2nd", 11 -> "11th", 23 -> "23rd"
    std::string Ordinal(uint32 n)
    {
        char const* suffix = "th";
        if (n % 100 < 11 || n % 100 > 13)
        {
            switch (n % 10)
            {
                case 1: suffix = "st"; break;
                case 2: suffix = "nd"; break;
                case 3: suffix = "rd"; break;
                default: break;
            }
        }
        return std::to_string(n) + suffix;
    }

    void SendToRealm(WorldPacket const* packet)
    {
        sWorld->SendGlobalMessage(packet);
    }
}

class npc_caverns_unlock_arthas : public CreatureScript
{
public:
    npc_caverns_unlock_arthas() : CreatureScript("npc_caverns_unlock_arthas") { }

    struct npc_caverns_unlock_arthasAI : public ScriptedAI
    {
        npc_caverns_unlock_arthasAI(Creature* creature) : ScriptedAI(creature) { }

        void UpdateAI(uint32 diff) override
        {
            // A quest NPC on a friendly faction: no combat to run, only the
            // scene's timers.
            _scheduler.Update(diff);
        }

        void OnQuestReward(Player* player, Quest const* quest, uint32 /*opt*/) override
        {
            if (!player || !quest)
                return;

            ObjectGuid const playerGuid = player->GetGUID();
            std::string const name = player->GetName();
            uint32 const questId = quest->GetQuestId();

            std::string socks;
            if (quest->RequiredItemId[0] && quest->RequiredItemCount[0])
            {
                std::string itemName = "socks";
                if (ItemTemplate const* item = sObjectMgr->GetItemTemplate(quest->RequiredItemId[0]))
                    itemName = item->Name1;
                socks = WithThousands(quest->RequiredItemCount[0]) + " " + itemName;
            }

            // How many characters finished this before. The hand-in's own row
            // is not saved yet, so this character is excluded explicitly and
            // the ordinal is the count plus one. Asked asynchronously so the
            // map never waits on the table scan; the scene's realm-wide line
            // comes 9 seconds later, and without an answer by then it simply
            // leaves the ordinal out. 0 = not known.
            auto ordinal = std::make_shared<std::atomic<uint32>>(0);
            std::string const sql = "SELECT COUNT(*) FROM character_queststatus_rewarded WHERE quest = "
                + std::to_string(questId) + " AND guid <> " + std::to_string(playerGuid.GetCounter());
            player->GetSession()->GetQueryProcessor().AddCallback(CharacterDatabase.AsyncQuery(sql.c_str())
                .WithCallback([ordinal](QueryResult result)
                {
                    ordinal->store(result ? uint32((*result)[0].GetUInt64()) + 1 : 0);
                }));

            // --- 0 s: the laugh -------------------------------------------
            me->SetFacingToObject(player);
            me->HandleEmoteCommand(EMOTE_ONESHOT_LAUGH);
            me->PlayDistanceSound(SOUND_LICH_KING_LAUGH_LOUD);
            me->Yell("HA. HA HA. HAHAHAHAHAHAHAHAHAHAHAHAHAHAHAHA!", LANG_UNIVERSAL, player);
            player->PlayDirectSound(SOUND_ACHIEVEMENT, player);
            player->CastSpell(player, SPELL_FIREWORK_RED_WHITE_BLUE, true);

            // --- 3 s: he says their name ------------------------------------
            _scheduler.Schedule(3s, [this, playerGuid, name](TaskContext)
            {
                Player* p = ObjectAccessor::GetPlayer(*me, playerGuid);
                if (p)
                    me->SetFacingToObject(p);
                me->HandleEmoteCommand(EMOTE_ONESHOT_POINT);
                me->Yell(name + ". You actually did it.", LANG_UNIVERSAL, p);
                if (p)
                {
                    p->CastSpell(p, SPELL_FIREWORK_BLUE, true);
                    p->CastSpell(p, SPELL_FIREWORK_GREEN, true);
                }
            });

            // --- 6 s: the price -------------------------------------------
            _scheduler.Schedule(6s, [this, playerGuid, socks](TaskContext)
            {
                Player* p = ObjectAccessor::GetPlayer(*me, playerGuid);
                me->HandleEmoteCommand(EMOTE_ONESHOT_LAUGH);
                me->Yell(socks.empty()
                    ? std::string("Every last sock. For a cave. HAHAHAHAHA!")
                    : socks + ". Every last one of them. For a CAVE. HAHAHAHAHA!", LANG_UNIVERSAL, p);
                if (p)
                    p->CastSpell(p, SPELL_FIREWORK_RED_STREAKS, true);
            });

            // --- 9 s: the realm hears about it ------------------------------
            _scheduler.Schedule(9s, [this, playerGuid, name, socks, ordinal](TaskContext)
            {
                std::string announce = "%s roars with laughter as " + name;
                announce += socks.empty() ? std::string(" pays the toll") : " hands over " + socks;
                announce += " and opens the Caverns Waste of Time!";

                WorldPacket emote;
                ChatHandler::BuildChatPacket(emote, CHAT_MSG_RAID_BOSS_EMOTE, LANG_UNIVERSAL, me, nullptr, announce);
                SendToRealm(&emote);
                SendToRealm(WorldPackets::Misc::PlaySound(SOUND_LICH_KING_LAUGH_LOUD).Write());

                uint32 const n = ordinal->load();
                std::string line;
                if (n == 1)
                    line = "|cffff8000Realm First!|r " + name
                        + " is the first to finish HAHAHAHA and open the Caverns Waste of Time. There is nothing left to do.";
                else if (n > 1)
                    line = "|cffff8000" + name + "|r is the " + Ordinal(n)
                        + " ever to finish HAHAHAHA and open the Caverns Waste of Time.";
                else
                    line = "|cffff8000" + name + "|r has finished HAHAHAHA and opened the Caverns Waste of Time.";

                WorldPacket system;
                ChatHandler::BuildChatPacket(system, CHAT_MSG_SYSTEM, LANG_UNIVERSAL, nullptr, nullptr, line);
                SendToRealm(&system);

                me->HandleEmoteCommand(EMOTE_ONESHOT_LAUGH);
                if (Player* p = ObjectAccessor::GetPlayer(*me, playerGuid))
                    p->CastSpell(p, SPELL_FIREWORK_LARGE, true);
            });

            // --- 12 s: the punchline ----------------------------------------
            _scheduler.Schedule(12s, [this, playerGuid](TaskContext)
            {
                Player* p = ObjectAccessor::GetPlayer(*me, playerGuid);
                me->HandleEmoteCommand(EMOTE_ONESHOT_LAUGH);
                me->PlayDistanceSound(SOUND_LICH_KING_LAUGH_LOUD);
                me->Yell("The Caverns are yours. There is nothing left to do. You have finished the game. HAHAHAHAHAHAHAHAHAHA!",
                    LANG_UNIVERSAL, p);
                if (p)
                {
                    p->CastSpell(p, SPELL_FIREWORK_YELLOW_ROSE, true);
                    p->CastSpell(p, SPELL_FIREWORK_RED_WHITE_BLUE, true);
                }
            });
        }

    private:
        TaskScheduler _scheduler;
    };

    CreatureAI* GetAI(Creature* creature) const override
    {
        return new npc_caverns_unlock_arthasAI(creature);
    }
};

void AddSC_custom_caverns_unlock_ceremony()
{
    new npc_caverns_unlock_arthas();
}
