/*
 * .spell propagate <haveSpellId> <giveSpellId>
 *
 * Grants giveSpell to every character that already knows haveSpell.
 *
 * It exists because doing this in SQL alone is silently wrong. A logged-in
 * player's `character_spell` rows are rewritten from memory when they save, so
 * an INSERT lands in a table that is about to be overwritten and the spell is
 * gone at logout with nothing to show why. Online characters therefore go
 * through Player::LearnSpell, which updates the live player AND what they will
 * save; only characters nobody is playing get the INSERT.
 *
 * The two halves must not overlap either: the DB half explicitly excludes guids
 * that are online, so a character cannot be handled twice and end up with a row
 * the in-memory copy does not know about.
 */

#include "ScriptMgr.h"
#include "Chat.h"
#include "DatabaseEnv.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "RBAC.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "World.h"
#include "WorldSession.h"

#include <set>

using namespace Trinity::ChatCommands;

class spell_propagate_commandscript : public CommandScript
{
public:
    spell_propagate_commandscript() : CommandScript("spell_propagate_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable spellCommandTable =
        {
            { "propagate", HandlePropagateCommand, rbac::RBAC_PERM_COMMAND_LEARN, Console::Yes },
        };

        static ChatCommandTable commandTable =
        {
            { "spell", spellCommandTable },
        };

        return commandTable;
    }

    static bool HandlePropagateCommand(ChatHandler* handler, uint32 haveSpell, uint32 giveSpell)
    {
        if (!haveSpell || !giveSpell || haveSpell == giveSpell)
        {
            handler->SendSysMessage("Usage: .spell propagate <haveSpellId> <giveSpellId> - "
                                    "the two ids must differ.");
            handler->SetSentErrorMessage(true);
            return false;
        }

        // Both ids are checked before anything is written. Granting a spell the
        // core does not know leaves a character_spell row that logs an error on
        // every one of that character's logins from then on.
        SpellInfo const* haveInfo = sSpellMgr->GetSpellInfo(haveSpell);
        SpellInfo const* giveInfo = sSpellMgr->GetSpellInfo(giveSpell);
        if (!haveInfo || !giveInfo)
        {
            handler->PSendSysMessage("Unknown spell: %u", !haveInfo ? haveSpell : giveSpell);
            handler->SetSentErrorMessage(true);
            return false;
        }

        // ONLINE FIRST, and remember who they were. Doing the DB half first would
        // race: a player logging out in between would save over the fresh row.
        std::set<ObjectGuid::LowType> handledOnline;
        uint32 onlineGranted = 0;
        {
            std::shared_lock<std::shared_mutex> guard(*HashMapHolder<Player>::GetLock());
            HashMapHolder<Player>::MapType const& players = ObjectAccessor::GetPlayers();
            for (auto const& playerEntry : players)
            {
                Player* player = playerEntry.second;
                if (!player)
                    continue;

                handledOnline.insert(player->GetGUID().GetCounter());

                if (!player->HasSpell(haveSpell) || player->HasSpell(giveSpell))
                    continue;

                // dependent = false: this is a grant in its own right, not a rank
                // learned as a side effect of another spell. A dependent spell is
                // removed again when its parent goes.
                player->LearnSpell(giveSpell, false);
                ++onlineGranted;
            }
        }

        // OFFLINE. Excluding the online guids keeps the two halves disjoint - an
        // online player already has the spell in memory and will write the row
        // themselves, and inserting underneath them achieves nothing.
        std::string excluded;
        for (ObjectGuid::LowType guid : handledOnline)
        {
            if (!excluded.empty())
                excluded += ',';
            excluded += std::to_string(guid);
        }

        std::string sql =
            "INSERT INTO character_spell (guid, spell, active, disabled) "
            "SELECT cs.guid, " + std::to_string(giveSpell) + ", 1, 0 "
            "FROM character_spell cs "
            "WHERE cs.spell = " + std::to_string(haveSpell) + " "
            "AND NOT EXISTS (SELECT 1 FROM character_spell x "
            "WHERE x.guid = cs.guid AND x.spell = " + std::to_string(giveSpell) + ")";
        if (!excluded.empty())
            sql += " AND cs.guid NOT IN (" + excluded + ")";

        // Counted before the write so the number reported is the number actually
        // changed, rather than a guess.
        std::string countSql =
            "SELECT COUNT(*) FROM character_spell cs "
            "WHERE cs.spell = " + std::to_string(haveSpell) + " "
            "AND NOT EXISTS (SELECT 1 FROM character_spell x "
            "WHERE x.guid = cs.guid AND x.spell = " + std::to_string(giveSpell) + ")";
        if (!excluded.empty())
            countSql += " AND cs.guid NOT IN (" + excluded + ")";

        uint32 offlineGranted = 0;
        if (QueryResult result = CharacterDatabase.Query(countSql.c_str()))
            offlineGranted = (*result)[0].GetUInt32();

        if (offlineGranted)
            CharacterDatabase.Execute(sql.c_str());

        handler->PSendSysMessage("Propagated spell %u (%s) to holders of %u (%s): "
                                 "%u online, %u offline.",
                                 giveSpell, giveInfo->SpellName[0],
                                 haveSpell, haveInfo->SpellName[0],
                                 onlineGranted, offlineGranted);
        return true;
    }
};

void AddSC_custom_spell_propagate()
{
    new spell_propagate_commandscript();
}
