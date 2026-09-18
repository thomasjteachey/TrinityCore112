/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * Omarion the Frostforged: the Argent Dawn craftsman outside the vanilla
 * Naxxramas who teaches the Glacial, Icebane, Polar and Icy Scale patterns and
 * hands out Omarion's Handbook. Reputation- and skill-gated, and rude to anyone
 * who does not qualify.
 *
 * Ported from mod-individual-progression (ZhengPeiRu21, AzerothCore, AGPL-3.0);
 * Naxxramas 40 scripts by Sogladev.
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
#include "GossipDef.h"
#include "Player.h"
#include "ScriptedCreature.h"
#include "ScriptedGossip.h"

enum OmarionCraftSpells : uint32
{
    // Tailoring
    LEARN_GLACIAL_GLOVES        = 28261,
    HAS_GLACIAL_GLOVES          = 28205,
    LEARN_GLACIAL_WRISTS        = 28262,
    HAS_GLACIAL_WRISTS          = 28209,
    LEARN_GLACIAL_VEST          = 28260,
    HAS_GLACIAL_VEST            = 28207,
    LEARN_GLACIAL_CLOAK         = 28263,
    HAS_GLACIAL_CLOAK           = 28208,

    // Leatherworking
    LEARN_POLAR_GLOVES          = 28255,
    HAS_POLAR_GLOVES            = 28220,
    LEARN_POLAR_BRACERS         = 28256,
    HAS_POLAR_BRACERS           = 28221,
    LEARN_POLAR_TUNIC           = 28254,
    HAS_POLAR_TUNIC             = 28219,
    LEARN_ICY_SCALE_GAUNTLETS   = 28258,
    HAS_ICY_SCALE_GAUNTLETS     = 28223,
    LEARN_ICY_SCALE_BRACERS     = 28259,
    HAS_ICY_SCALE_BRACERS       = 28224,
    LEARN_ICY_SCALE_BREASTPLATE = 28257,
    HAS_ICY_SCALE_BREASTPLATE   = 28222,

    // Blacksmithing
    LEARN_ICEBANE_GAUNTLETS     = 28253,
    HAS_ICEBANE_GAUNTLETS       = 28243,
    LEARN_ICEBANE_BRACERS       = 28252,
    HAS_ICEBANE_BRACERS         = 28244,
    LEARN_ICEBANE_BREASTPLATE   = 28251,
    HAS_ICEBANE_BREASTPLATE     = 28242
};

enum OmarionSay
{
    BROADCAST_TEXT_SPIT_TARGET = 31673
};

enum OmarionRequirements
{
    FACTION_ARGENT_DAWN = 529,
    BOOK_REQ_RANK       = REP_HONORED,
    CRAFT1_REQ_RANK     = REP_REVERED,
    CRAFT2_REQ_RANK     = REP_EXALTED,
    MASTER_REQ_SKILL    = 225,
    LEARN_REQ_SKILL     = 300
};

enum OmarionQuests
{
    QUEST_OMARIONS_HANDBOOK = 9233,
    ITEM_OMARIONS_HANDBOOK  = 22719
};

enum OmarionGossip
{
    // npc_text ids
    NPC_TEXT_INTRO                 = 8507,
    NPC_TEXT_NO_CRAFTER            = 8516,
    NPC_TEXT_NEW_ENTRY             = 24400,
    NPC_TEXT_TAILORING             = NPC_TEXT_NEW_ENTRY + 1,
    NPC_TEXT_BLACKSMITHING         = NPC_TEXT_NEW_ENTRY + 2,
    NPC_TEXT_LEATHERWORKING        = NPC_TEXT_NEW_ENTRY + 3,

    // gossip_menu ids
    MENU_ID_ENTRY                  = 24400,
    MENU_ID_NO_CRAFTER             = MENU_ID_ENTRY,
    MENU_ID_TAILORING              = MENU_ID_ENTRY + 1,
    MENU_ID_BLACKSMITHING          = MENU_ID_ENTRY + 2,
    MENU_ID_LEATHERWORKING         = MENU_ID_ENTRY + 3,
    MENU_ID_INTRO                  = MENU_ID_ENTRY + 4,

    // Intro options
    ITEM_ID_LEATHERWORKING         = 1,
    ITEM_ID_BLACKSMITHING          = 2,
    ITEM_ID_TAILORING              = 3,
    ITEM_ID_NO_CRAFTER             = 4,

    // Tailoring options
    ITEM_ID_GLACIAL_CLOAK          = 1,
    ITEM_ID_GLACIAL_GLOVES         = 2,
    ITEM_ID_GLACIAL_WRISTS         = 3,
    ITEM_ID_GLACIAL_VEST           = 4,
    ITEM_ID_GOODBYE_TAILORING      = 5,

    // Blacksmithing options
    ITEM_ID_ICEBANE_BRACERS        = 1,
    ITEM_ID_ICEBANE_GAUNTLETS      = 2,
    ITEM_ID_ICEBANE_BREASTPLATE    = 3,
    ITEM_ID_GOODBYE_BLACKSMITHING  = 4,

    // Leatherworking options
    ITEM_ID_POLAR_BRACERS          = 1,
    ITEM_ID_POLAR_GLOVES           = 2,
    ITEM_ID_POLAR_TUNIC            = 3,
    ITEM_ID_ICY_SCALE_BRACERS      = 4,
    ITEM_ID_ICY_SCALE_GAUNTLETS    = 5,
    ITEM_ID_ICY_SCALE_BREASTPLATE  = 6,
    ITEM_ID_GOODBYE_LEATHERWORKING = 7,

    // Book option
    ITEM_ID_GOODBYE_NO_CRAFTER     = 1,

    GOSSIP_CLOSE                   = 100
};

struct npc_omarion_gossip : public ScriptedAI
{
    npc_omarion_gossip(Creature* creature) : ScriptedAI(creature) { }

    bool OnGossipHello(Player* player) override
    {
        ClearGossipMenuFor(player);

        if (player->GetSkillValue(SKILL_TAILORING) >= MASTER_REQ_SKILL)
            AddGossipItemFor(player, MENU_ID_INTRO, ITEM_ID_TAILORING, GOSSIP_SENDER_MAIN, MENU_ID_TAILORING);

        if (player->GetSkillValue(SKILL_BLACKSMITHING) >= MASTER_REQ_SKILL)
            AddGossipItemFor(player, MENU_ID_INTRO, ITEM_ID_BLACKSMITHING, GOSSIP_SENDER_MAIN, MENU_ID_BLACKSMITHING);

        if (player->GetSkillValue(SKILL_LEATHERWORKING) >= MASTER_REQ_SKILL)
            AddGossipItemFor(player, MENU_ID_INTRO, ITEM_ID_LEATHERWORKING, GOSSIP_SENDER_MAIN, MENU_ID_LEATHERWORKING);

        AddGossipItemFor(player, MENU_ID_INTRO, ITEM_ID_NO_CRAFTER, GOSSIP_SENDER_MAIN, MENU_ID_NO_CRAFTER);
        SendGossipMenuFor(player, NPC_TEXT_INTRO, me->GetGUID());
        return true;
    }

    void LearnCraftIfNotAlreadyKnown(uint32 learnId, uint32 knowId, Player* player)
    {
        if (!player->HasSpell(knowId))
            player->CastSpell(player, learnId, CastSpellExtraArgs(TRIGGERED_NONE));
    }

    void CloseGossipEmoteAndSpitOnPlayer(Player* player)
    {
        CloseGossipMenuFor(player);
        me->TextEmote(BROADCAST_TEXT_SPIT_TARGET, player);
        me->HandleEmoteCommand(EMOTE_ONESHOT_NONE); // skip whatever was playing
        me->HandleEmoteCommand(EMOTE_ONESHOT_RUDE);
    }

    bool OnGossipSelect(Player* player, uint32 /*menuId*/, uint32 gossipListId) override
    {
        uint32 const action = player->PlayerTalkClass->GetGossipOptionAction(gossipListId);

        ReputationRank const argentDawnRep = player->GetReputationRank(FACTION_ARGENT_DAWN);
        uint32 const tailorSkill = player->GetSkillValue(SKILL_TAILORING);
        uint32 const blacksmithSkill = player->GetSkillValue(SKILL_BLACKSMITHING);
        uint32 const leatherworkSkill = player->GetSkillValue(SKILL_LEATHERWORKING);

        switch (action)
        {
            case GOSSIP_CLOSE:
                CloseGossipMenuFor(player);
                break;
            case MENU_ID_TAILORING:
                ClearGossipMenuFor(player);
                if (argentDawnRep < CRAFT1_REQ_RANK || tailorSkill < LEARN_REQ_SKILL)
                {
                    CloseGossipEmoteAndSpitOnPlayer(player);
                    break;
                }

                AddGossipItemFor(player, MENU_ID_TAILORING, ITEM_ID_GLACIAL_WRISTS, GOSSIP_SENDER_MAIN, LEARN_GLACIAL_WRISTS);
                AddGossipItemFor(player, MENU_ID_TAILORING, ITEM_ID_GLACIAL_GLOVES, GOSSIP_SENDER_MAIN, LEARN_GLACIAL_GLOVES);
                if (argentDawnRep >= CRAFT2_REQ_RANK)
                {
                    AddGossipItemFor(player, MENU_ID_TAILORING, ITEM_ID_GLACIAL_VEST, GOSSIP_SENDER_MAIN, LEARN_GLACIAL_VEST);
                    AddGossipItemFor(player, MENU_ID_TAILORING, ITEM_ID_GLACIAL_CLOAK, GOSSIP_SENDER_MAIN, LEARN_GLACIAL_CLOAK);
                }
                AddGossipItemFor(player, MENU_ID_TAILORING, ITEM_ID_GOODBYE_TAILORING, GOSSIP_SENDER_MAIN, GOSSIP_CLOSE);
                SendGossipMenuFor(player, NPC_TEXT_TAILORING, me->GetGUID());
                break;
            case MENU_ID_BLACKSMITHING:
                ClearGossipMenuFor(player);
                if (argentDawnRep < CRAFT1_REQ_RANK || blacksmithSkill < LEARN_REQ_SKILL)
                {
                    CloseGossipEmoteAndSpitOnPlayer(player);
                    break;
                }

                AddGossipItemFor(player, MENU_ID_BLACKSMITHING, ITEM_ID_ICEBANE_BRACERS, GOSSIP_SENDER_MAIN, LEARN_ICEBANE_BRACERS);
                AddGossipItemFor(player, MENU_ID_BLACKSMITHING, ITEM_ID_ICEBANE_GAUNTLETS, GOSSIP_SENDER_MAIN, LEARN_ICEBANE_GAUNTLETS);
                if (argentDawnRep >= CRAFT2_REQ_RANK)
                    AddGossipItemFor(player, MENU_ID_BLACKSMITHING, ITEM_ID_ICEBANE_BREASTPLATE, GOSSIP_SENDER_MAIN, LEARN_ICEBANE_BREASTPLATE);
                AddGossipItemFor(player, MENU_ID_BLACKSMITHING, ITEM_ID_GOODBYE_BLACKSMITHING, GOSSIP_SENDER_MAIN, GOSSIP_CLOSE);
                SendGossipMenuFor(player, NPC_TEXT_BLACKSMITHING, me->GetGUID());
                break;
            case MENU_ID_LEATHERWORKING:
                ClearGossipMenuFor(player);
                if (argentDawnRep < CRAFT1_REQ_RANK || leatherworkSkill < LEARN_REQ_SKILL)
                {
                    CloseGossipEmoteAndSpitOnPlayer(player);
                    break;
                }

                AddGossipItemFor(player, MENU_ID_LEATHERWORKING, ITEM_ID_POLAR_BRACERS, GOSSIP_SENDER_MAIN, LEARN_POLAR_BRACERS);
                AddGossipItemFor(player, MENU_ID_LEATHERWORKING, ITEM_ID_POLAR_GLOVES, GOSSIP_SENDER_MAIN, LEARN_POLAR_GLOVES);
                if (argentDawnRep >= CRAFT2_REQ_RANK)
                    AddGossipItemFor(player, MENU_ID_LEATHERWORKING, ITEM_ID_POLAR_TUNIC, GOSSIP_SENDER_MAIN, LEARN_POLAR_TUNIC);

                AddGossipItemFor(player, MENU_ID_LEATHERWORKING, ITEM_ID_ICY_SCALE_BRACERS, GOSSIP_SENDER_MAIN, LEARN_ICY_SCALE_BRACERS);
                AddGossipItemFor(player, MENU_ID_LEATHERWORKING, ITEM_ID_ICY_SCALE_GAUNTLETS, GOSSIP_SENDER_MAIN, LEARN_ICY_SCALE_GAUNTLETS);
                if (argentDawnRep >= CRAFT2_REQ_RANK)
                    AddGossipItemFor(player, MENU_ID_LEATHERWORKING, ITEM_ID_ICY_SCALE_BREASTPLATE, GOSSIP_SENDER_MAIN, LEARN_ICY_SCALE_BREASTPLATE);
                AddGossipItemFor(player, MENU_ID_LEATHERWORKING, ITEM_ID_GOODBYE_LEATHERWORKING, GOSSIP_SENDER_MAIN, GOSSIP_CLOSE);
                SendGossipMenuFor(player, NPC_TEXT_LEATHERWORKING, me->GetGUID());
                break;
            case MENU_ID_NO_CRAFTER:
                ClearGossipMenuFor(player);
                if (argentDawnRep < BOOK_REQ_RANK)
                {
                    CloseGossipEmoteAndSpitOnPlayer(player);
                    break;
                }

                if (player->GetQuestStatus(QUEST_OMARIONS_HANDBOOK) == QUEST_STATUS_NONE
                    && !player->HasItemCount(ITEM_OMARIONS_HANDBOOK, 1, true))
                    player->AddItem(ITEM_OMARIONS_HANDBOOK, 1);

                AddGossipItemFor(player, MENU_ID_NO_CRAFTER, ITEM_ID_GOODBYE_NO_CRAFTER, GOSSIP_SENDER_MAIN, GOSSIP_CLOSE);
                SendGossipMenuFor(player, NPC_TEXT_NO_CRAFTER, me->GetGUID());
                break;
            case LEARN_GLACIAL_CLOAK:
                LearnCraftIfNotAlreadyKnown(LEARN_GLACIAL_CLOAK, HAS_GLACIAL_CLOAK, player);
                SendGossipMenuFor(player, NPC_TEXT_TAILORING, me->GetGUID());
                break;
            case LEARN_GLACIAL_GLOVES:
                LearnCraftIfNotAlreadyKnown(LEARN_GLACIAL_GLOVES, HAS_GLACIAL_GLOVES, player);
                SendGossipMenuFor(player, NPC_TEXT_TAILORING, me->GetGUID());
                break;
            case LEARN_GLACIAL_WRISTS:
                LearnCraftIfNotAlreadyKnown(LEARN_GLACIAL_WRISTS, HAS_GLACIAL_WRISTS, player);
                SendGossipMenuFor(player, NPC_TEXT_TAILORING, me->GetGUID());
                break;
            case LEARN_GLACIAL_VEST:
                LearnCraftIfNotAlreadyKnown(LEARN_GLACIAL_VEST, HAS_GLACIAL_VEST, player);
                SendGossipMenuFor(player, NPC_TEXT_TAILORING, me->GetGUID());
                break;
            case LEARN_ICEBANE_BRACERS:
                LearnCraftIfNotAlreadyKnown(LEARN_ICEBANE_BRACERS, HAS_ICEBANE_BRACERS, player);
                SendGossipMenuFor(player, NPC_TEXT_BLACKSMITHING, me->GetGUID());
                break;
            case LEARN_ICEBANE_GAUNTLETS:
                LearnCraftIfNotAlreadyKnown(LEARN_ICEBANE_GAUNTLETS, HAS_ICEBANE_GAUNTLETS, player);
                SendGossipMenuFor(player, NPC_TEXT_BLACKSMITHING, me->GetGUID());
                break;
            case LEARN_ICEBANE_BREASTPLATE:
                LearnCraftIfNotAlreadyKnown(LEARN_ICEBANE_BREASTPLATE, HAS_ICEBANE_BREASTPLATE, player);
                SendGossipMenuFor(player, NPC_TEXT_BLACKSMITHING, me->GetGUID());
                break;
            case LEARN_POLAR_BRACERS:
                LearnCraftIfNotAlreadyKnown(LEARN_POLAR_BRACERS, HAS_POLAR_BRACERS, player);
                SendGossipMenuFor(player, NPC_TEXT_LEATHERWORKING, me->GetGUID());
                break;
            case LEARN_POLAR_GLOVES:
                LearnCraftIfNotAlreadyKnown(LEARN_POLAR_GLOVES, HAS_POLAR_GLOVES, player);
                SendGossipMenuFor(player, NPC_TEXT_LEATHERWORKING, me->GetGUID());
                break;
            case LEARN_POLAR_TUNIC:
                LearnCraftIfNotAlreadyKnown(LEARN_POLAR_TUNIC, HAS_POLAR_TUNIC, player);
                SendGossipMenuFor(player, NPC_TEXT_LEATHERWORKING, me->GetGUID());
                break;
            case LEARN_ICY_SCALE_BRACERS:
                LearnCraftIfNotAlreadyKnown(LEARN_ICY_SCALE_BRACERS, HAS_ICY_SCALE_BRACERS, player);
                SendGossipMenuFor(player, NPC_TEXT_LEATHERWORKING, me->GetGUID());
                break;
            case LEARN_ICY_SCALE_GAUNTLETS:
                LearnCraftIfNotAlreadyKnown(LEARN_ICY_SCALE_GAUNTLETS, HAS_ICY_SCALE_GAUNTLETS, player);
                SendGossipMenuFor(player, NPC_TEXT_LEATHERWORKING, me->GetGUID());
                break;
            case LEARN_ICY_SCALE_BREASTPLATE:
                LearnCraftIfNotAlreadyKnown(LEARN_ICY_SCALE_BREASTPLATE, HAS_ICY_SCALE_BREASTPLATE, player);
                SendGossipMenuFor(player, NPC_TEXT_LEATHERWORKING, me->GetGUID());
                break;
            default:
                break;
        }

        return true;
    }
};

void AddSC_npc_omarion_40()
{
    RegisterCreatureAI(npc_omarion_gossip);
}
