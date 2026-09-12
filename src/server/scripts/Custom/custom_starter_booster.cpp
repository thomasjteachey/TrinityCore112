/*
 * Starter booster.
 *
 * One gossip click takes a brand new character to level 10, teaches it everything
 * a trainer of its class would sell at that level, and puts it down at the
 * Crossroads. Placed in every starting zone so it is reachable whatever race
 * somebody rolled.
 *
 * The spell half deliberately reuses the exact walk that ".learn my spells"
 * performs (cs_learn.cpp HandleLearnMySpellsCommand): iterate the class's
 * trainers and take whatever CanTeachSpell approves. That keeps the booster
 * honest - it hands out precisely what the player could have walked to a trainer
 * and bought, including this realm's rebuilt classic trainer lists and its rank
 * gating, rather than a hand-maintained spell table that would drift. Levelling
 * happens FIRST for the same reason: CanTeachSpell filters on the player's
 * current level, so at level 10 the loop yields the level-10 spellbook and
 * nothing beyond it.
 */

#include "Creature.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "ScriptedCreature.h"
#include "ScriptedGossip.h"
#include "Trainer.h"
#include "WorldSession.h"

namespace
{
    constexpr uint8 BOOSTER_TARGET_LEVEL = 10;
    constexpr uint32 BOOSTER_GOSSIP_TEXT = 900300;
    constexpr uint32 BOOSTER_ACTION_BOOST = 1;

    // game_tele 1029 "TheCrossroads". Hardcoded rather than read back from
    // game_tele so the destination cannot silently move or disappear under the
    // booster at runtime.
    constexpr uint32 CROSSROADS_MAP = 1;
    constexpr float CROSSROADS_X = -452.84f;
    constexpr float CROSSROADS_Y = -2650.76f;
    constexpr float CROSSROADS_Z = 95.5209f;
    constexpr float CROSSROADS_O = 0.241081f;

    // Everything that must be true before we start changing the character. The
    // boost is three separate mutations (level, spellbook, position) and there is
    // no way to roll the first two back, so refuse up front rather than half way
    // through.
    char const* BoosterRefusal(Player const* player)
    {
        if (player->GetLevel() >= BOOSTER_TARGET_LEVEL)
            return "You are already past level 10 - there is nothing here for you.";

        if (!player->IsAlive())
            return "Come back once you are on your feet again.";

        if (player->IsInCombat())
            return "Not while you are fighting.";

        if (player->IsInFlight())
            return "Not in mid-flight.";

        if (player->IsBeingTeleported())
            return "Wait until you have arrived somewhere first.";

        // A battleground or arena owns the player's position; dropping them into
        // the Barrens from inside one leaves the match's bookkeeping holding a
        // player who is no longer there.
        if (Map const* map = player->FindMap())
            if (map->IsBattlegroundOrArena())
                return "Not from inside a battleground.";

        return nullptr;
    }

    uint32 LearnClassSpellsForCurrentLevel(Player* player)
    {
        std::vector<Trainer::Trainer const*> const& trainers = sObjectMgr->GetClassTrainers(player->GetClass());

        uint32 learned = 0;
        bool hadNew;

        // Repeat until a pass teaches nothing new: ranks unlock their successors,
        // and a spell that becomes teachable only after its predecessor is known
        // would otherwise be missed by a single pass.
        do
        {
            hadNew = false;
            for (Trainer::Trainer const* trainer : trainers)
            {
                if (!trainer->IsTrainerValidForPlayer(player))
                    continue;

                for (Trainer::Spell const& trainerSpell : trainer->GetSpells())
                {
                    if (!trainer->CanTeachSpell(player, &trainerSpell))
                        continue;

                    if (trainerSpell.IsCastable())
                        player->CastSpell(player, trainerSpell.SpellId, true);
                    else
                        player->LearnSpell(trainerSpell.SpellId, false);

                    ++learned;
                    hadNew = true;
                }
            }
        } while (hadNew);

        return learned;
    }
}

class npc_starter_booster : public CreatureScript
{
public:
    npc_starter_booster() : CreatureScript("npc_starter_booster") { }

    struct npc_starter_boosterAI : public ScriptedAI
    {
        npc_starter_boosterAI(Creature* creature) : ScriptedAI(creature) { }

        bool OnGossipHello(Player* player) override
        {
            if (!player)
                return false;

            if (char const* refusal = BoosterRefusal(player))
            {
                // Still open the window rather than silently swallowing the click,
                // so the reason is visible instead of the NPC looking broken.
                if (WorldSession* session = player->GetSession())
                    session->SendNotification("%s", refusal);
                SendGossipMenuFor(player, BOOSTER_GOSSIP_TEXT, me->GetGUID());
                return true;
            }

            AddGossipItemFor(player, GOSSIP_ICON_CHAT,
                "Send me on my way: level 10, my trainer's spells, and passage to the Crossroads.",
                GOSSIP_SENDER_MAIN, BOOSTER_ACTION_BOOST);
            SendGossipMenuFor(player, BOOSTER_GOSSIP_TEXT, me->GetGUID());
            return true;
        }

        bool OnGossipSelect(Player* player, uint32 /*menuId*/, uint32 gossipListId) override
        {
            if (!player)
                return false;

            uint32 const action = GetGossipActionFor(player, gossipListId);
            ClearGossipMenuFor(player);
            CloseGossipMenuFor(player);

            if (action != BOOSTER_ACTION_BOOST)
                return true;

            // Re-check rather than trusting the menu: the gossip window can sit
            // open across a pull, a death, or another click on the same NPC.
            if (char const* refusal = BoosterRefusal(player))
            {
                if (WorldSession* session = player->GetSession())
                    session->SendNotification("%s", refusal);
                return true;
            }

            player->GiveLevel(BOOSTER_TARGET_LEVEL);
            uint32 const learned = LearnClassSpellsForCurrentLevel(player);

            if (WorldSession* session = player->GetSession())
                session->SendNotification("Level %u, %u spells learned. Good luck out there.",
                    uint32(BOOSTER_TARGET_LEVEL), learned);

            // Last, because it is the only step that can leave the player looking
            // at a loading screen; the level and spellbook are already committed
            // by the time the map change starts.
            player->TeleportTo(CROSSROADS_MAP, CROSSROADS_X, CROSSROADS_Y, CROSSROADS_Z, CROSSROADS_O);
            return true;
        }
    };

    CreatureAI* GetAI(Creature* creature) const override
    {
        return new npc_starter_boosterAI(creature);
    }
};

void AddSC_custom_starter_booster()
{
    new npc_starter_booster();
}
