/*
 * Moves every saved character off the spells a wrapper casts and onto the
 * wrapper (WrappedSpells.h), once per startup, before the first login.
 *
 * The server stopped teaching wrapped spells in the same build - LearnSpell /
 * AddSpell, the trainer list and the Consecration talent all hand out the
 * wrapper now - so this only has the rows saved before that to deal with:
 * spellbooks, action bars, talents (the Holy talent's spell moved from 26573
 * to 90526, see LoadDBCStores) and the bots' talent recipes. It has to run at
 * boot rather than as a hand-applied SQL file: a talent row still saved as
 * 26573 when a binary with the new Talent rank loads it reads as an unspent
 * point, and an over-cap count resets every talent on the character.
 *
 * OnStartup runs after SetInitialWorldSettings and before the world loop, so no
 * player or bot is loaded yet. Every statement is idempotent; on a realm that is
 * already clean the counts come back 0 and nothing is written.
 */

#include "ScriptMgr.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "StringFormat.h"
#include "WrappedSpells.h"

#include <string>

namespace
{
    // "676,81493,8143,..." - every wrapped id.
    std::string WrappedIdList()
    {
        std::string list;
        for (WrappedSpells::Entry const& entry : WrappedSpells::Entries)
        {
            if (!list.empty())
                list += ',';
            list += std::to_string(entry.Wrapped);
        }
        return list;
    }

    // "CASE <column> WHEN 676 THEN 81492 ... END" - the wrapper for each.
    std::string WrapperCase(char const* column)
    {
        std::string sql = Trinity::StringFormat("CASE {}", column);
        for (WrappedSpells::Entry const& entry : WrappedSpells::Entries)
            sql += Trinity::StringFormat(" WHEN {} THEN {}", entry.Wrapped, entry.Wrapper);
        sql += " END";
        return sql;
    }

    uint64 CountRows(std::string const& sql)
    {
        if (QueryResult result = CharacterDatabase.Query(sql.c_str()))
            return (*result)[0].GetUInt64();
        return 0;
    }

    bool TableExists(char const* table)
    {
        return bool(CharacterDatabase.Query(Trinity::StringFormat(
            "SELECT 1 FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = '{}'", table).c_str()));
    }
}

class custom_wrapped_spells_migration : public WorldScript
{
public:
    custom_wrapped_spells_migration() : WorldScript("custom_wrapped_spells_migration") { }

    void OnStartup() override
    {
        std::string const ids = WrappedIdList();

        // Spellbooks: the wrapper is written first, so a failure part-way can
        // only leave a character holding both, never neither.
        uint64 const spells = CountRows(Trinity::StringFormat(
            "SELECT COUNT(*) FROM character_spell WHERE spell IN ({})", ids));
        if (spells)
        {
            CharacterDatabase.DirectExecute(Trinity::StringFormat(
                "INSERT IGNORE INTO character_spell (guid, spell, active, disabled) "
                "SELECT guid, {}, active, disabled FROM character_spell WHERE spell IN ({})",
                WrapperCase("spell"), ids).c_str());
            CharacterDatabase.DirectExecute(Trinity::StringFormat(
                "DELETE FROM character_spell WHERE spell IN ({})", ids).c_str());
        }

        // Action bars. A button on a spell the character no longer holds is
        // dropped at load, so without this every bar would lose the ability.
        uint64 const actions = CountRows(Trinity::StringFormat(
            "SELECT COUNT(*) FROM character_action WHERE `type` = 0 AND action IN ({})", ids));
        if (actions)
            CharacterDatabase.DirectExecute(Trinity::StringFormat(
                "UPDATE character_action SET action = {} WHERE `type` = 0 AND action IN ({})",
                WrapperCase("action"), ids).c_str());

        // Talents, per talent group. IGNORE then DELETE: a row the wrapper
        // already has is simply the old duplicate.
        uint64 const talents = CountRows(Trinity::StringFormat(
            "SELECT COUNT(*) FROM character_talent WHERE spell IN ({})", ids));
        if (talents)
        {
            CharacterDatabase.DirectExecute(Trinity::StringFormat(
                "UPDATE IGNORE character_talent SET spell = {} WHERE spell IN ({})",
                WrapperCase("spell"), ids).c_str());
            CharacterDatabase.DirectExecute(Trinity::StringFormat(
                "DELETE FROM character_talent WHERE spell IN ({})", ids).c_str());
        }

        // The bots' exported talent builds, read by spell id.
        uint64 recipes = 0;
        if (TableExists("playerbot_talent_recipe"))
        {
            recipes = CountRows(Trinity::StringFormat(
                "SELECT COUNT(*) FROM playerbot_talent_recipe WHERE spell IN ({})", ids));
            if (recipes)
            {
                CharacterDatabase.DirectExecute(Trinity::StringFormat(
                    "UPDATE IGNORE playerbot_talent_recipe SET spell = {} WHERE spell IN ({})",
                    WrapperCase("spell"), ids).c_str());
                CharacterDatabase.DirectExecute(Trinity::StringFormat(
                    "DELETE FROM playerbot_talent_recipe WHERE spell IN ({})", ids).c_str());
            }
        }

        if (spells || actions || talents || recipes)
            TC_LOG_INFO("server.loading", ">> Wrapped spells: moved {} spellbook, {} action bar, {} talent and {} bot recipe rows onto their wrappers",
                spells, actions, talents, recipes);
        else
            TC_LOG_INFO("server.loading", ">> Wrapped spells: no character holds a wrapped spell");
    }
};

void AddSC_custom_wrapped_spells()
{
    new custom_wrapped_spells_migration();
}
