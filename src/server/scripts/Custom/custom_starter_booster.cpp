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

#include "ChallengeModes.h"
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

        // Anyone who has taken a challenge has opted INTO the first ten levels.
        // Iron Man, hardcore, self-crafted, the slowed experience modes - every
        // one of them is a promise about how the climb is made, and handing over
        // ten free levels and a spellbook is the one thing that cannot be
        // reconciled with any of them. HARDCORE_DEAD is deliberately not in this
        // range: it is a headstone rather than a mode, and a character wearing it
        // is already refused by the modes it sits beside.
        for (uint8 setting = SETTING_HARDCORE; setting <= SETTING_IRON_MAN; ++setting)
            if (sChallengeModes->IsEnabledForPlayer(ChallengeModeSettings(setting), player))
                return "You are under a challenge. Those ten levels are yours to earn.";

        return nullptr;
    }

    // Weapon skills the character ALREADY has, plus defence, taken to the cap for
    // its level - which in 3.3.5 is simply level times five.
    //
    // Already has, on purpose. The boost is meant to hand somebody a level 10
    // that is ready to be played, not to teach proficiencies their class was
    // never going to own: a mage does not walk out of this holding axes. Run
    // after the trainer catch-up rather than before it, so a proficiency the
    // boost itself just granted is capped too instead of starting at one.
    //
    // Defence is in the same list because it is the same kind of skill and the
    // same cap, and a level 10 whose defence is still 5 is hit by everything.
    uint32 MaxOutCombatSkillsForLevel(Player* player)
    {
        static constexpr uint32 kCappedSkills[] = {
            SKILL_SWORDS, SKILL_AXES, SKILL_BOWS, SKILL_GUNS, SKILL_MACES,
            SKILL_2H_SWORDS, SKILL_STAVES, SKILL_2H_MACES, SKILL_UNARMED,
            SKILL_2H_AXES, SKILL_DAGGERS, SKILL_THROWN, SKILL_CROSSBOWS,
            SKILL_WANDS, SKILL_POLEARMS, SKILL_FIST_WEAPONS, SKILL_DEFENSE
        };

        uint16 const cap = uint16(player->GetLevel()) * 5;
        if (!cap)
            return 0;

        uint32 raised = 0;
        for (uint32 skillId : kCappedSkills)
        {
            if (!player->HasSkill(skillId))
                continue;

            // Both halves, or a skill whose VALUE is already at the cap but whose
            // ceiling is not keeps being capped one point later by the next hit.
            if (player->GetSkillValue(skillId) >= cap && player->GetMaxSkillValue(skillId) >= cap)
                continue;

            player->SetSkill(skillId, player->GetSkillStep(skillId), cap, cap);
            ++raised;
        }

        return raised;
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
            uint32 const capped = MaxOutCombatSkillsForLevel(player);

            if (WorldSession* session = player->GetSession())
                session->SendNotification("Level %u, %u spells learned, %u skills capped. Good luck out there.",
                    uint32(BOOSTER_TARGET_LEVEL), learned, capped);

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
