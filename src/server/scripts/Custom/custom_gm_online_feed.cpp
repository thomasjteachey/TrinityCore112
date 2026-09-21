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

// Who is online, on EVERY realm, for a GM (the CENTURION_GMOnline addon).
//
// GM ONLY: character names, levels, zones and ACCOUNT names across realms is a
// staff view. The gate is the RBAC GM command permission - the account, not the
// .gm toggle - exactly as the bot stats feed does it.
//
// The realm the GM is standing on answers from memory: the live session list,
// so zones are where people are now. Every other realm answers from its own
// characters database - online = 1 - which this realm can read because all the
// realms share one MySQL server and one login database. That is as fresh as
// the other realm's last character save (PlayerSaveInterval), which for a zone
// is a minute or two, and it needs nothing built on the other realm at all.
//
// Inbound is the GM command. The addon sends it over the core's own addon
// command channel ("TrinityCore" prefix, AddonChannelCommandHandler) rather
// than as a dot-prefixed SAY: the channel runs the same RBAC-gated command,
// answers a non-GM with a hidden "failed", and never broadcasts anything -
// where a SAY from a player account is simply said out loud in this fork.
//
//   .gmonline            a plain chat listing of the people (bots counted)
//   .gmonline bots       ...and the bots too
//   .gmonline addon      the addon's feed, nothing printed
//   .gmonline addon bots
//
// And the other half of the staff question - "who else is this?" - every
// character one account owns, on every realm, offline ones included:
//
//   .gmonline alts <who>        <who> is an account name OR a character name
//   .gmonline addon alts <who>
//
// That one is all database: an offline character is in no session list, so
// even this realm's own rows come from `characters`. The realm the GM is
// standing on then has its live sessions laid over the top, because a saved
// zone is only as fresh as the last PlayerSaveInterval.
//
// Replies ride the CCGAME addon whisper everything else uses. One reply is:
//
//   GMOB:<seq>|<withBots>                           begin; the addon starts a fresh list
//   GMOR:<realmId>|<name>|<people>|<bots>|<live>     one per realm; live = from memory
//   GMOZ:<zoneId>,<name>;...                        names of the zones the rows use
//   GMOP:<realmId>|<name>,<lvl>,<class>,<race>,<zone>,<flags>,<account>;...
//   GMOE:<seq>                                      end; the addon swaps the list in
//
// and an account listing is the same shape with its own tags:
//
//   GMAB:<seq>|<asked>|<found>                      begin; <asked> is what was typed
//   GMAI:<id>|<account>|<security>|<online>|<banned>|<muted>|<lastLoginAgo>|<joinedAgo>|<lastIp>
//   GMAR:<realmId>|<name>|<characters>|<live>       one per realm
//   GMAZ:<zoneId>,<name>;...
//   GMAC:<realmId>|<name>,<lvl>,<class>,<race>,<zone>,<flags>,<idleSecs>,<gold>,<hours>;...
//   GMAE:<seq>
//
// flags: 1 bot, 2 GM account, 4 in a battleground or arena, 8 dead,
//        16 online, 32 a deleted character (the last two, an alt listing only).
//
// Config:
//   Centurion.GMOnline.Enable            1
//   Centurion.GMOnline.Realms            "5:bpluscharacters,4:lpluscharacters"
//                                        realm id : characters schema, every realm
//                                        to show; this realm's own entry is served
//                                        from memory and its schema is not used
//   Centurion.GMOnline.BotAccountPrefix  "PLAYERBOT"

#include "CharacterCache.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "Configuration/Config.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "Log.h"
#include "Miscellaneous/Surnames.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "QueryCallback.h"
#include "RBAC.h"
#include "Realm.h"
#include "ScriptMgr.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Util.h"
#include "World.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

using namespace Trinity::ChatCommands;

namespace
{
    struct OnlineRealm
    {
        uint32 id = 0;
        std::string schema;
    };

    struct OnlineRow
    {
        uint32 realmId = 0;
        std::string name;
        std::string account;
        uint32 level = 0;
        uint32 playerClass = 0;
        uint32 race = 0;
        uint32 zoneId = 0;
        uint32 flags = 0;
    };

    enum OnlineRowFlags : uint32
    {
        ROW_BOT     = 1,
        ROW_GM      = 2,
        ROW_BG      = 4,
        ROW_DEAD    = 8,
        // An online listing is online by definition; these two say something
        // only in an account listing, which carries offline rows as well.
        ROW_ONLINE  = 16,
        ROW_DELETED = 32,
    };

    // One account's whole roster: what a GM wants alongside the names.
    struct AltAccount
    {
        uint32 id = 0;
        std::string username;
        std::string lastIp;
        uint32 lastLoginAgo = 0;    // seconds; 0 = never logged in
        uint32 joinedAgo = 0;       // seconds; 0 = unknown
        uint32 security = 0;
        bool online = false;
        bool banned = false;
        bool muted = false;
    };

    struct AltRow
    {
        uint32 realmId = 0;
        std::string name;
        uint32 level = 0;
        uint32 playerClass = 0;
        uint32 race = 0;
        uint32 zoneId = 0;
        uint32 flags = 0;
        uint32 idleSeconds = 0;     // since logout; 0 while online
        uint32 gold = 0;
        uint32 hoursPlayed = 0;
    };

    bool s_enabled = true;
    std::vector<OnlineRealm> s_realms;
    std::string s_botPrefix = "PLAYERBOT";
    std::string s_authSchema;
    uint32 s_sequence = 0;

    // Which characters tables carry a `surname` column (the Centurion realms
    // do; the older ones do not), so names go out as "First Last" where there
    // is a last name and a query never names a column that is not there.
    // Probed once, lazily, from a command's world-thread turn - never from the
    // config hook, which can run before the databases are up - and again after
    // a config reload.
    bool s_surnameProbed = false;
    bool s_thisRealmHasSurname = false;
    std::set<std::string> s_surnameSchemas;

    // Realm names from the login database's own realmlist, read once and again
    // after a config reload. Guarded: the command runs on the world thread, but
    // a config reload is its own caller.
    std::mutex s_realmNameLock;
    std::map<uint32, std::string> s_realmNames;
    bool s_realmNamesLoaded = false;

    // A schema name goes into SQL text, so it is held to what a schema name can
    // be. The config is trusted, but not that far.
    bool IsSafeIdentifier(std::string const& text)
    {
        return !text.empty() && std::all_of(text.begin(), text.end(), [](char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
        });
    }

    // Commas, semicolons and pipes separate the records, so they are scrubbed
    // rather than escaped; no name the feed carries legitimately contains one.
    std::string Scrub(std::string text)
    {
        for (char& c : text)
            if (c == ',' || c == ';' || c == '|')
                c = ' ';
        return text;
    }

    void LoadGmOnlineConfig()
    {
        s_enabled = sConfigMgr->GetBoolDefault("Centurion.GMOnline.Enable", true);
        s_botPrefix = sConfigMgr->GetStringDefault("Centurion.GMOnline.BotAccountPrefix", "PLAYERBOT");
        if (!IsSafeIdentifier(s_botPrefix))
            s_botPrefix = "PLAYERBOT";

        // The login schema is the FIFTH field of LoginDatabaseInfo
        // (host;port;user;password;database[;ssl]) - not the last, which is the
        // optional ssl flag when present. Only that field is read; nothing else
        // in the string is touched or kept.
        s_authSchema.clear();
        std::string const loginInfo = sConfigMgr->GetStringDefault("LoginDatabaseInfo", "");
        std::vector<std::string_view> const loginFields = Trinity::Tokenize(loginInfo, ';', true);
        if (loginFields.size() >= 5 && IsSafeIdentifier(std::string(loginFields[4])))
            s_authSchema = std::string(loginFields[4]);

        s_realms.clear();
        std::string const realms = sConfigMgr->GetStringDefault("Centurion.GMOnline.Realms", "");
        for (std::string_view rawEntry : Trinity::Tokenize(realms, ',', false))
        {
            std::string entry(rawEntry);
            entry.erase(std::remove(entry.begin(), entry.end(), ' '), entry.end());
            std::string::size_type const colon = entry.find(':');
            if (colon == std::string::npos)
                continue;

            Optional<uint32> id = Trinity::StringTo<uint32>(std::string_view(entry).substr(0, colon));
            std::string const schema = entry.substr(colon + 1);
            if (!id || !*id || !IsSafeIdentifier(schema))
            {
                TC_LOG_ERROR("server.loading", "Centurion.GMOnline.Realms: ignoring entry '{}'.", entry);
                continue;
            }

            s_realms.push_back({ *id, schema });
        }

        s_surnameProbed = false;

        std::lock_guard<std::mutex> guard(s_realmNameLock);
        s_realmNamesLoaded = false;
    }

    void EnsureSurnameProbe()
    {
        if (s_surnameProbed)
            return;
        s_surnameProbed = true;
        s_thisRealmHasSurname = false;
        s_surnameSchemas.clear();

        if (QueryResult result = CharacterDatabase.Query("SELECT TABLE_SCHEMA, CAST(TABLE_SCHEMA = DATABASE() AS UNSIGNED) "
            "FROM information_schema.COLUMNS WHERE TABLE_NAME = 'characters' AND COLUMN_NAME = 'surname'"))
        {
            do
            {
                Field* fields = result->Fetch();
                s_surnameSchemas.insert(fields[0].GetString());
                if (fields[1].GetUInt32())
                    s_thisRealmHasSurname = true;
            } while (result->NextRow());
        }
    }

    bool RealmHasSurname(uint32 realmId)
    {
        if (realmId == realm.Id.Realm)
            return s_thisRealmHasSurname;
        for (OnlineRealm const& entry : s_realms)
            if (entry.id == realmId)
                return s_surnameSchemas.count(entry.schema) != 0;
        return false;
    }

    // SQL for a character's shown name: "First Last" where the realm has last
    // names, the bare column where it does not.
    std::string NameSql(uint32 realmId, char const* alias, char const* column)
    {
        std::string const col = std::string(alias) + "." + column;
        if (!RealmHasSurname(realmId))
            return col;
        return "CONCAT_WS(' ', " + col + ", NULLIF(" + alias + ".surname, ''))";
    }

    std::string RealmName(uint32 realmId)
    {
        std::lock_guard<std::mutex> guard(s_realmNameLock);
        if (!s_realmNamesLoaded)
        {
            s_realmNamesLoaded = true;
            s_realmNames.clear();
            // Once per uptime (and per config reload), on the command's own
            // thread: a handful of rows from the login database.
            if (QueryResult result = LoginDatabase.Query("SELECT id, name FROM realmlist"))
            {
                do
                {
                    Field* fields = result->Fetch();
                    s_realmNames[fields[0].GetUInt32()] = Scrub(fields[1].GetString());
                } while (result->NextRow());
            }
        }

        auto const itr = s_realmNames.find(realmId);
        return itr != s_realmNames.end() ? itr->second : ("Realm " + std::to_string(realmId));
    }

    bool IsBotAccountName(std::string const& account)
    {
        return !s_botPrefix.empty() && account.compare(0, s_botPrefix.size(), s_botPrefix) == 0;
    }

    std::string ZoneName(uint32 zoneId)
    {
        AreaTableEntry const* area = sAreaTableStore.LookupEntry(zoneId);
        char const* name = area ? area->AreaName[LOCALE_enUS] : nullptr;
        if (!name || !*name)
            return "Zone " + std::to_string(zoneId);
        return Scrub(name);
    }

    bool IsBattlegroundMap(uint32 mapId)
    {
        MapEntry const* entry = sMapStore.LookupEntry(mapId);
        return entry && entry->IsBattlegroundOrArena();
    }

    // This realm, from the live sessions. World thread only: the command handler
    // is PROCESS_THREADUNSAFE, so no map is updating while this reads players.
    std::vector<OnlineRow> SnapshotThisRealm()
    {
        std::vector<OnlineRow> rows;
        uint32 const thisRealm = realm.Id.Realm;

        for (auto const& [accountId, session] : sWorld->GetAllSessions())
        {
            // NOT IsInWorld: a player on a loading screen (a portal, a boat, a
            // battleground) is out of the world for the whole load and would
            // flicker out of the list. Such a player is on no map and only the
            // world thread touches it, so reading it here is safe; its zone is
            // the one it is leaving until the load completes.
            Player* player = session ? session->GetPlayer() : nullptr;
            if (!player || session->PlayerLogout())
                continue;

            OnlineRow row;
            row.realmId = thisRealm;
            // The name the client knows: the cache name (a battleground clone's
            // GetName() is its internal Obcm name), with the family name.
            std::string shown;
            if (!sCharacterCache->GetCharacterNameByGuid(player->GetGUID(), shown))
                shown = player->GetName();
            row.name = Scrub(Surnames::Decorated(player->GetGUID(), shown));
            row.account = Scrub(session->GetAccountName());
            row.level = player->GetLevel();
            row.playerClass = player->GetClass();
            row.race = player->GetRace();
            row.zoneId = player->GetZoneId();

            // Virtual sessions are the bot fleet; transient ones are the
            // battleground clones of it. Either way not a person.
            if (session->IsVirtualSession() || session->IsTransientPlayerSession() || IsBotAccountName(session->GetAccountName()))
                row.flags |= ROW_BOT;
            // Bot accounts can carry GM security (L+'s PLAYERBOTONE is level 3);
            // that is plumbing, not staff, so a bot is never tagged GM.
            else if (session->GetSecurity() > SEC_PLAYER)
                row.flags |= ROW_GM;
            if (player->InBattleground() || player->InArena())
                row.flags |= ROW_BG;
            if (!player->IsAlive())
                row.flags |= ROW_DEAD;

            rows.push_back(std::move(row));
        }

        return rows;
    }

    // Every other configured realm, in one statement.
    std::string BuildOtherRealmsQuery()
    {
        if (s_authSchema.empty())
            return std::string();

        uint32 const thisRealm = realm.Id.Realm;
        std::ostringstream sql;
        bool first = true;
        for (OnlineRealm const& other : s_realms)
        {
            if (other.id == thisRealm)
                continue;

            if (!first)
                sql << " UNION ALL ";
            first = false;

            // Every computed column is CAST to an integer: an aggregate can come
            // back typed as DECIMAL, which the Field converters read as a string
            // and answer 0 for, silently.
            sql << "SELECT CAST(" << other.id << " AS UNSIGNED), " << NameSql(other.id, "c", "name") << ", a.username, c.level, c.class, c.race, c.zone, c.map, "
                << "CAST(COALESCE((SELECT MAX(aa.SecurityLevel) FROM `" << s_authSchema << "`.account_access aa "
                << "WHERE aa.AccountID = a.id AND (aa.RealmID = -1 OR aa.RealmID = " << other.id << ")), 0) AS UNSIGNED), "
                // Dead: no health, or a released ghost (PLAYER_FLAGS_GHOST) - a
                // ghost is saved with 1 health, so both are needed.
                << "CAST((c.health = 0 OR (c.playerFlags & 16) <> 0) AS UNSIGNED)"
                << " FROM `" << other.schema << "`.characters c"
                << " JOIN `" << s_authSchema << "`.account a ON a.id = c.account"
                << " WHERE c.online = 1";
        }

        return sql.str();
    }

    void AppendOtherRealmRows(QueryResult result, std::vector<OnlineRow>& rows)
    {
        if (!result)
            return;

        do
        {
            Field* fields = result->Fetch();
            OnlineRow row;
            row.realmId = fields[0].GetUInt32();
            row.name = Scrub(fields[1].GetString());
            row.account = Scrub(fields[2].GetString());
            row.level = fields[3].GetUInt8();
            row.playerClass = fields[4].GetUInt8();
            row.race = fields[5].GetUInt8();
            row.zoneId = fields[6].GetUInt16();
            if (IsBotAccountName(fields[2].GetString()))
                row.flags |= ROW_BOT;
            else if (fields[8].GetUInt32() > SEC_PLAYER)
                row.flags |= ROW_GM;
            if (IsBattlegroundMap(fields[7].GetUInt16()))
                row.flags |= ROW_BG;
            if (fields[9].GetUInt32())
                row.flags |= ROW_DEAD;
            rows.push_back(std::move(row));
        } while (result->NextRow());
    }

    // From the viewer to the viewer. The addon accepts the feed only as a whisper
    // from its own character: nobody else can send one of those, so a player
    // cannot plant rows in a GM's list.
    //
    // Built with the GUID overload on purpose. The WorldObject overload decides
    // the language for its caller, and this fork once had it force everything to
    // Universal - LANG_ADDON included - which put this feed in the chat frame as
    // plain whispers. The GUID overload writes the language it is given.
    void SendTagged(Player* viewer, std::string const& tag, std::string const& payload)
    {
        std::string const message = "CCGAME\t" + tag + ":" + payload;
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, viewer->GetGUID(), viewer->GetGUID(),
            message, 0, viewer->GetName(), viewer->GetName());
        viewer->SendDirectMessage(&data);
    }

    // The realms to report, in the configured order, with this realm first if
    // it is not listed at all - a GM always sees the realm they are on.
    std::vector<uint32> RealmsToReport()
    {
        std::vector<uint32> ids;
        uint32 const thisRealm = realm.Id.Realm;
        bool listedThis = false;
        for (OnlineRealm const& entry : s_realms)
        {
            ids.push_back(entry.id);
            listedThis = listedThis || entry.id == thisRealm;
        }
        if (!listedThis)
            ids.insert(ids.begin(), thisRealm);
        return ids;
    }

    // The same realms, each with the table its characters live in. This realm's
    // schema need not be configured at all: the character database this
    // worldserver is connected to IS that schema, so it is named plainly. The
    // order matches RealmsToReport so both feeds list the realms alike.
    std::vector<std::pair<uint32, std::string>> RealmTables()
    {
        std::vector<std::pair<uint32, std::string>> tables;
        uint32 const thisRealm = realm.Id.Realm;
        bool listedThis = false;
        for (OnlineRealm const& entry : s_realms)
        {
            if (entry.id == thisRealm)
            {
                listedThis = true;
                tables.emplace_back(entry.id, "characters");
            }
            else
                tables.emplace_back(entry.id, "`" + entry.schema + "`.characters");
        }
        if (!listedThis)
            tables.insert(tables.begin(), { thisRealm, "characters" });
        return tables;
    }

    // One whisper stays well inside what the client takes in an addon message.
    constexpr std::size_t kMaxPayload = 200;

    void SendFeed(Player* viewer, std::vector<OnlineRow> const& rows, bool withBots, uint32 sequence)
    {
        SendTagged(viewer, "GMOB", std::to_string(sequence) + "|" + (withBots ? "1" : "0"));

        uint32 const thisRealm = realm.Id.Realm;
        for (uint32 realmId : RealmsToReport())
        {
            uint32 people = 0;
            uint32 bots = 0;
            std::set<uint32> zones;
            for (OnlineRow const& row : rows)
            {
                if (row.realmId != realmId)
                    continue;
                if (row.flags & ROW_BOT)
                    ++bots;
                else
                    ++people;
                if (withBots || !(row.flags & ROW_BOT))
                    zones.insert(row.zoneId);
            }

            SendTagged(viewer, "GMOR", std::to_string(realmId) + "|" + RealmName(realmId) + "|" +
                std::to_string(people) + "|" + std::to_string(bots) + "|" + (realmId == thisRealm ? "1" : "0"));

            std::string zonePayload;
            for (uint32 zoneId : zones)
            {
                std::string const entry = std::to_string(zoneId) + "," + ZoneName(zoneId) + ";";
                if (!zonePayload.empty() && zonePayload.size() + entry.size() > kMaxPayload)
                {
                    SendTagged(viewer, "GMOZ", zonePayload);
                    zonePayload.clear();
                }
                zonePayload += entry;
            }
            if (!zonePayload.empty())
                SendTagged(viewer, "GMOZ", zonePayload);

            std::string const head = std::to_string(realmId) + "|";
            std::string rowPayload;
            for (OnlineRow const& row : rows)
            {
                if (row.realmId != realmId || (!withBots && (row.flags & ROW_BOT)))
                    continue;

                std::string const entry = row.name + "," + std::to_string(row.level) + "," +
                    std::to_string(row.playerClass) + "," + std::to_string(row.race) + "," +
                    std::to_string(row.zoneId) + "," + std::to_string(row.flags) + "," + row.account + ";";
                if (!rowPayload.empty() && head.size() + rowPayload.size() + entry.size() > kMaxPayload)
                {
                    SendTagged(viewer, "GMOP", head + rowPayload);
                    rowPayload.clear();
                }
                rowPayload += entry;
            }
            if (!rowPayload.empty())
                SendTagged(viewer, "GMOP", head + rowPayload);
        }

        SendTagged(viewer, "GMOE", std::to_string(sequence));
    }

    char const* ClassName(uint32 playerClass)
    {
        switch (playerClass)
        {
            case CLASS_WARRIOR:      return "Warrior";
            case CLASS_PALADIN:      return "Paladin";
            case CLASS_HUNTER:       return "Hunter";
            case CLASS_ROGUE:        return "Rogue";
            case CLASS_PRIEST:       return "Priest";
            case CLASS_DEATH_KNIGHT: return "Death Knight";
            case CLASS_SHAMAN:       return "Shaman";
            case CLASS_MAGE:         return "Mage";
            case CLASS_WARLOCK:      return "Warlock";
            case CLASS_DRUID:        return "Druid";
            default:                 return "?";
        }
    }

    // The same reply as chat lines, for a GM without the addon.
    void PrintFeed(Player* viewer, std::vector<OnlineRow> const& rows, bool withBots)
    {
        ChatHandler chat(viewer->GetSession());
        uint32 const thisRealm = realm.Id.Realm;
        for (uint32 realmId : RealmsToReport())
        {
            uint32 people = 0;
            uint32 bots = 0;
            for (OnlineRow const& row : rows)
                if (row.realmId == realmId)
                    ++((row.flags & ROW_BOT) ? bots : people);

            chat.PSendSysMessage("%s%s: %u %s, %u bot%s.", RealmName(realmId).c_str(),
                realmId == thisRealm ? "" : " (last save)", people, people == 1 ? "person" : "people",
                bots, bots == 1 ? "" : "s");

            for (OnlineRow const& row : rows)
            {
                if (row.realmId != realmId || (!withBots && (row.flags & ROW_BOT)))
                    continue;

                chat.PSendSysMessage("  %s - %u %s - %s - %s%s%s%s", row.name.c_str(), row.level,
                    ClassName(row.playerClass), ZoneName(row.zoneId).c_str(), row.account.c_str(),
                    (row.flags & ROW_GM) ? " [GM]" : "", (row.flags & ROW_BG) ? " [BG]" : "",
                    (row.flags & ROW_DEAD) ? " [dead]" : "");
            }
        }
    }

    // ------------------------------------------------------------------ alts
    //
    // Two statements, chained on one callback. The first turns whatever was
    // typed into an account; the second lists that account's characters on
    // every realm. They are separate because the answer to the first is what
    // the second asks about - and because a name that matches nothing has to
    // come back as "no such account" rather than as an empty roster.

    // What was typed can be an account name or a character name, on any realm,
    // and a character that has since been deleted still answers - TrinityCore
    // blanks a deleted character's name and account and keeps both in the
    // deleteInfos_ columns. Account names win over character names, and the
    // realms are tried in their configured order.
    //
    // "First Last" names one character by the pair, on the realms that have last
    // names; an account name never has a space, so the account clause simply
    // finds nothing for it. A single word is a first name, as it always was.
    std::string BuildAccountLookupQuery(std::string const& target)
    {
        if (s_authSchema.empty())
            return std::string();

        std::string escaped = target;
        CharacterDatabase.EscapeString(escaped);

        std::string first = target, last;
        std::string::size_type const space = target.find(' ');
        if (space != std::string::npos)
        {
            first = target.substr(0, space);
            last = target.substr(space + 1);
        }
        CharacterDatabase.EscapeString(first);
        CharacterDatabase.EscapeString(last);

        std::ostringstream match;
        match << "SELECT a2.id AS id FROM `" << s_authSchema << "`.account a2 WHERE a2.username = '" << escaped << "'";
        for (auto const& entry : RealmTables())
        {
            if (last.empty())
            {
                match << " UNION ALL SELECT c2.account FROM " << entry.second << " c2"
                      << " WHERE c2.name = '" << first << "' AND c2.account <> 0"
                      << " UNION ALL SELECT c2.deleteInfos_Account FROM " << entry.second << " c2"
                      << " WHERE c2.deleteInfos_Name = '" << first << "' AND c2.deleteInfos_Account <> 0";
            }
            else if (RealmHasSurname(entry.first))
            {
                match << " UNION ALL SELECT c2.account FROM " << entry.second << " c2"
                      << " WHERE c2.name = '" << first << "' AND c2.surname = '" << last << "' AND c2.account <> 0"
                      << " UNION ALL SELECT c2.deleteInfos_Account FROM " << entry.second << " c2"
                      << " WHERE c2.deleteInfos_Name = '" << first << "' AND c2.surname = '" << last << "'"
                      << " AND c2.deleteInfos_Account <> 0";
            }
        }

        // Same rule as the online feed: every computed column is CAST to an
        // integer, or an aggregate comes back as DECIMAL and reads as 0.
        std::ostringstream sql;
        sql << "SELECT CAST(a.id AS UNSIGNED), a.username, a.last_ip,"
            << " CAST(COALESCE(UNIX_TIMESTAMP(a.last_login), 0) AS UNSIGNED),"
            << " CAST(COALESCE(UNIX_TIMESTAMP(a.joindate), 0) AS UNSIGNED),"
            << " CAST(a.online AS UNSIGNED), CAST(COALESCE(a.mutetime, 0) AS SIGNED),"
            << " CAST(COALESCE((SELECT MAX(aa.SecurityLevel) FROM `" << s_authSchema << "`.account_access aa"
            << " WHERE aa.AccountID = a.id), 0) AS UNSIGNED),"
            << " CAST(COALESCE((SELECT MAX(ab.active) FROM `" << s_authSchema << "`.account_banned ab"
            << " WHERE ab.id = a.id AND ab.active = 1), 0) AS UNSIGNED)"
            << " FROM `" << s_authSchema << "`.account a"
            << " JOIN (" << match.str() << " LIMIT 1) m ON m.id = a.id LIMIT 1";
        return sql.str();
    }

    bool ReadAltAccount(QueryResult result, AltAccount& account)
    {
        if (!result)
            return false;

        time_t const now = GameTime::GetGameTime();
        Field* fields = result->Fetch();
        account.id = fields[0].GetUInt32();
        account.username = Scrub(fields[1].GetString());
        account.lastIp = Scrub(fields[2].GetString());
        uint32 const lastLogin = fields[3].GetUInt32();
        uint32 const joined = fields[4].GetUInt32();
        account.online = fields[5].GetUInt32() != 0;
        // mutetime is when the mute ENDS, so a stale one is not a mute.
        account.muted = fields[6].GetInt64() > int64(now);
        account.security = fields[7].GetUInt32();
        account.banned = fields[8].GetUInt32() != 0;
        if (lastLogin && uint32(now) > lastLogin)
            account.lastLoginAgo = uint32(now) - lastLogin;
        if (joined && uint32(now) > joined)
            account.joinedAgo = uint32(now) - joined;
        return true;
    }

    std::string BuildAltsQuery(uint32 accountId)
    {
        std::ostringstream sql;
        bool first = true;
        for (auto const& entry : RealmTables())
        {
            if (!first)
                sql << " UNION ALL ";
            first = false;

            sql << "SELECT CAST(" << entry.first << " AS UNSIGNED),"
                << " CASE WHEN c.deleteDate IS NULL THEN " << NameSql(entry.first, "c", "name")
                << " ELSE " << NameSql(entry.first, "c", "deleteInfos_Name") << " END,"
                << " CAST(c.level AS UNSIGNED), CAST(c.class AS UNSIGNED), CAST(c.race AS UNSIGNED),"
                << " CAST(c.zone AS UNSIGNED), CAST(c.map AS UNSIGNED), CAST(c.online AS UNSIGNED),"
                << " CAST(c.logout_time AS UNSIGNED), CAST(c.money AS UNSIGNED), CAST(c.totaltime AS UNSIGNED),"
                // Dead: no health, or a released ghost (PLAYER_FLAGS_GHOST) - a
                // ghost is saved with 1 health, so both are needed.
                << " CAST((c.health = 0 OR (c.playerFlags & 16) <> 0) AS UNSIGNED),"
                << " CAST((c.deleteDate IS NOT NULL) AS UNSIGNED)"
                << " FROM " << entry.second << " c"
                << " WHERE c.account = " << accountId
                << " OR (c.deleteDate IS NOT NULL AND c.deleteInfos_Account = " << accountId << ")";
        }

        return sql.str();
    }

    void AppendAltRows(QueryResult result, std::map<std::string, OnlineRow> const& live, std::vector<AltRow>& alts)
    {
        if (!result)
            return;

        uint32 const thisRealm = realm.Id.Realm;
        uint32 const now = uint32(GameTime::GetGameTime());
        do
        {
            Field* fields = result->Fetch();
            AltRow row;
            row.realmId = fields[0].GetUInt32();
            row.name = Scrub(fields[1].GetString());
            row.level = fields[2].GetUInt32();
            row.playerClass = fields[3].GetUInt32();
            row.race = fields[4].GetUInt32();
            row.zoneId = fields[5].GetUInt32();
            uint32 const mapId = fields[6].GetUInt32();
            bool const online = fields[7].GetUInt32() != 0;
            uint32 const logoutTime = fields[8].GetUInt32();
            row.gold = fields[9].GetUInt32() / 10000;
            row.hoursPlayed = fields[10].GetUInt32() / 3600;
            if (fields[11].GetUInt32())
                row.flags |= ROW_DEAD;
            if (fields[12].GetUInt32())
                row.flags |= ROW_DELETED;
            if (online)
                row.flags |= ROW_ONLINE;
            if (IsBattlegroundMap(mapId))
                row.flags |= ROW_BG;
            if (!online && logoutTime && now > logoutTime)
                row.idleSeconds = now - logoutTime;

            // On this realm the session beats the save: a character logged in
            // right now is where its session says it is, not where the last
            // PlayerSaveInterval left it. Bot and GM tags only exist there too.
            if (row.realmId == thisRealm)
            {
                auto const itr = live.find(row.name);
                if (itr != live.end())
                {
                    row.level = itr->second.level;
                    row.zoneId = itr->second.zoneId;
                    row.idleSeconds = 0;
                    row.flags &= ~(ROW_BG | ROW_DEAD);
                    row.flags |= ROW_ONLINE | (itr->second.flags & (ROW_BOT | ROW_GM | ROW_BG | ROW_DEAD));
                }
            }

            alts.push_back(std::move(row));
        } while (result->NextRow());

        // Highest level first, then by name. Realms are grouped only so the
        // comparison stays a real ordering - which realm comes first in the
        // feed is RealmTables', and that is what both feeds walk.
        std::sort(alts.begin(), alts.end(), [](AltRow const& a, AltRow const& b)
        {
            if (a.realmId != b.realmId)
                return a.realmId < b.realmId;
            if (a.level != b.level)
                return a.level > b.level;
            return a.name < b.name;
        });
    }

    void SendAltsFeed(Player* viewer, std::string const& asked, AltAccount const& account,
        std::vector<AltRow> const& alts, uint32 sequence, bool found)
    {
        SendTagged(viewer, "GMAB", std::to_string(sequence) + "|" + asked + "|" + (found ? "1" : "0"));

        if (!found)
        {
            SendTagged(viewer, "GMAE", std::to_string(sequence));
            return;
        }

        SendTagged(viewer, "GMAI", std::to_string(account.id) + "|" + account.username + "|" +
            std::to_string(account.security) + "|" + (account.online ? "1" : "0") + "|" +
            (account.banned ? "1" : "0") + "|" + (account.muted ? "1" : "0") + "|" +
            std::to_string(account.lastLoginAgo) + "|" + std::to_string(account.joinedAgo) + "|" +
            account.lastIp);

        uint32 const thisRealm = realm.Id.Realm;
        for (auto const& entry : RealmTables())
        {
            uint32 const realmId = entry.first;
            uint32 count = 0;
            std::set<uint32> zones;
            for (AltRow const& row : alts)
            {
                if (row.realmId != realmId)
                    continue;
                ++count;
                zones.insert(row.zoneId);
            }

            SendTagged(viewer, "GMAR", std::to_string(realmId) + "|" + RealmName(realmId) + "|" +
                std::to_string(count) + "|" + (realmId == thisRealm ? "1" : "0"));

            std::string zonePayload;
            for (uint32 zoneId : zones)
            {
                std::string const zoneEntry = std::to_string(zoneId) + "," + ZoneName(zoneId) + ";";
                if (!zonePayload.empty() && zonePayload.size() + zoneEntry.size() > kMaxPayload)
                {
                    SendTagged(viewer, "GMAZ", zonePayload);
                    zonePayload.clear();
                }
                zonePayload += zoneEntry;
            }
            if (!zonePayload.empty())
                SendTagged(viewer, "GMAZ", zonePayload);

            std::string const head = std::to_string(realmId) + "|";
            std::string rowPayload;
            for (AltRow const& row : alts)
            {
                if (row.realmId != realmId)
                    continue;

                std::string const rowEntry = row.name + "," + std::to_string(row.level) + "," +
                    std::to_string(row.playerClass) + "," + std::to_string(row.race) + "," +
                    std::to_string(row.zoneId) + "," + std::to_string(row.flags) + "," +
                    std::to_string(row.idleSeconds) + "," + std::to_string(row.gold) + "," +
                    std::to_string(row.hoursPlayed) + ";";
                if (!rowPayload.empty() && head.size() + rowPayload.size() + rowEntry.size() > kMaxPayload)
                {
                    SendTagged(viewer, "GMAC", head + rowPayload);
                    rowPayload.clear();
                }
                rowPayload += rowEntry;
            }
            if (!rowPayload.empty())
                SendTagged(viewer, "GMAC", head + rowPayload);
        }

        SendTagged(viewer, "GMAE", std::to_string(sequence));
    }

    // The same account listing as chat lines, for a GM without the addon.
    void PrintAlts(Player* viewer, std::string const& asked, AltAccount const& account,
        std::vector<AltRow> const& alts, bool found)
    {
        ChatHandler chat(viewer->GetSession());
        if (!found)
        {
            chat.PSendSysMessage("No account or character named '%s'.", asked.c_str());
            return;
        }

        std::string const gm = account.security ? Trinity::StringFormat(" - GM level {}", account.security) : "";
        std::string const seen = account.lastLoginAgo
            ? secsToTimeString(account.lastLoginAgo, TimeFormat::ShortText) + " ago" : "never";

        chat.PSendSysMessage("Account %s (id %u) - %u character%s%s%s%s.", account.username.c_str(), account.id,
            uint32(alts.size()), alts.size() == 1 ? "" : "s", gm.c_str(),
            account.banned ? " - BANNED" : "", account.muted ? " - muted" : "");
        chat.PSendSysMessage("  last login %s%s%s.", seen.c_str(),
            account.lastIp.empty() ? "" : " from ", account.lastIp.c_str());

        uint32 const thisRealm = realm.Id.Realm;
        for (auto const& entry : RealmTables())
        {
            uint32 const realmId = entry.first;
            bool any = false;
            for (AltRow const& row : alts)
            {
                if (row.realmId != realmId)
                    continue;

                if (!any)
                {
                    any = true;
                    chat.PSendSysMessage("%s%s:", RealmName(realmId).c_str(), realmId == thisRealm ? "" : " (last save)");
                }

                std::string const when = (row.flags & ROW_ONLINE) ? "online"
                    : row.idleSeconds ? secsToTimeString(row.idleSeconds, TimeFormat::ShortText) + " ago"
                    : "never played";

                chat.PSendSysMessage("  %s - %u %s - %s - %s%s%s%s%s", row.name.c_str(), row.level,
                    ClassName(row.playerClass), ZoneName(row.zoneId).c_str(), when.c_str(),
                    (row.flags & ROW_DELETED) ? " [deleted]" : "", (row.flags & ROW_GM) ? " [GM]" : "",
                    (row.flags & ROW_BG) ? " [BG]" : "", (row.flags & ROW_DEAD) ? " [dead]" : "");
            }
        }
    }

    void DeliverAlts(ObjectGuid viewerGuid, std::string const& asked, AltAccount const& account,
        std::vector<AltRow> const& alts, bool addon, uint32 sequence, bool found)
    {
        Player* viewer = ObjectAccessor::FindConnectedPlayer(viewerGuid);
        if (!viewer || !viewer->GetSession())
            return;

        if (addon)
            SendAltsFeed(viewer, asked, account, alts, sequence, found);
        else
            PrintAlts(viewer, asked, account, alts, found);
    }

    void Deliver(ObjectGuid viewerGuid, std::vector<OnlineRow> const& rows, bool addon, bool withBots, uint32 sequence)
    {
        Player* viewer = ObjectAccessor::FindConnectedPlayer(viewerGuid);
        if (!viewer || !viewer->GetSession())
            return;

        if (addon)
            SendFeed(viewer, rows, withBots, sequence);
        else
            PrintFeed(viewer, rows, withBots);
    }

    class centurion_gm_online_config : public WorldScript
    {
    public:
        centurion_gm_online_config() : WorldScript("centurion_gm_online_config") { }

        void OnConfigLoad(bool /*reload*/) override
        {
            LoadGmOnlineConfig();
        }
    };

    class centurion_gm_online_commands : public CommandScript
    {
    public:
        centurion_gm_online_commands() : CommandScript("centurion_gm_online_commands") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable commandTable =
            {
                { "gmonline", HandleGmOnline, rbac::RBAC_PERM_COMMAND_GM, Console::No },
            };
            return commandTable;
        }

        static bool HandleGmOnline(ChatHandler* handler, Tail args)
        {
            WorldSession* session = handler->GetSession();
            Player* viewer = session ? session->GetPlayer() : nullptr;
            if (!viewer)
                return false;

            if (!s_enabled)
            {
                handler->SendSysMessage("gmonline is switched off (Centurion.GMOnline.Enable).");
                return true;
            }

            bool addon = false;
            bool withBots = false;
            bool wantAlts = false;
            std::string target;
            for (std::string_view word : Trinity::Tokenize(args, ' ', false))
            {
                if (word == "addon")
                    addon = true;
                else if (word == "bots")
                    withBots = true;
                else if (word == "alts" || word == "account")
                    wantAlts = true;    // a keyword; the name follows
                else if (target.size() < 64)
                {
                    // Every other word is the name, so "Elgrom Fernbloom" stays
                    // whole rather than turning into "Elgrom".
                    if (!target.empty())
                        target += ' ';
                    target += std::string(word);
                }
            }

            uint32 const sequence = ++s_sequence;
            EnsureSurnameProbe();

            // A name turns the command into the account listing, with or
            // without the "alts" keyword: ".gmonline Koda" is what a GM types.
            if (wantAlts && target.empty())
            {
                handler->SendSysMessage("Usage: .gmonline alts <account or character name>.");
                return true;
            }
            if (!target.empty())
                return HandleGmAlts(handler, viewer, std::move(target), addon, sequence);

            // Realm names come from a blocking login-database read the first
            // time they are asked for. Asked here, on the command's world-thread
            // turn, so the reply - which may be built on a map thread - never
            // waits on one.
            RealmName(realm.Id.Realm);

            std::vector<OnlineRow> rows = SnapshotThisRealm();
            std::string const sql = BuildOtherRealmsQuery();
            ObjectGuid const viewerGuid = viewer->GetGUID();

            if (sql.empty())
            {
                Deliver(viewerGuid, rows, addon, withBots, sequence);
                return true;
            }

            // The other realms come back asynchronously, into THIS session's own
            // query processor, so the callback lives exactly as long as the GM's
            // session does. It may run on a map thread, which is why this realm
            // was snapshotted above and the callback only appends and sends.
            session->GetQueryProcessor().AddCallback(CharacterDatabase.AsyncQuery(sql.c_str())
                .WithCallback([viewerGuid, rows = std::move(rows), addon, withBots, sequence](QueryResult result) mutable
                {
                    AppendOtherRealmRows(result, rows);
                    Deliver(viewerGuid, rows, addon, withBots, sequence);
                }));
            return true;
        }

        // Every character on one account. Two chained queries: resolve the name
        // to an account, then list that account's characters everywhere. Both
        // run on this session's query processor, so they cannot outlive the GM,
        // and neither callback touches anything but the rows it was handed -
        // they may run on a map thread, which is why this realm's live sessions
        // are snapshotted HERE, on the command's own world-thread turn.
        static bool HandleGmAlts(ChatHandler* handler, Player* viewer, std::string target, bool addon, uint32 sequence)
        {
            if (s_authSchema.empty())
            {
                handler->SendSysMessage("gmonline cannot read the login database schema out of LoginDatabaseInfo.");
                return true;
            }

            // Both an account name and a character name fit in 32 bytes; a
            // longer one matches nothing, so it is cut rather than refused.
            if (target.size() > 32)
                target.resize(32);

            std::string const asked = Scrub(target);

            RealmName(realm.Id.Realm);

            std::map<std::string, OnlineRow> live;
            for (OnlineRow& row : SnapshotThisRealm())
            {
                std::string name = row.name;
                live.emplace(std::move(name), std::move(row));
            }

            ObjectGuid const viewerGuid = viewer->GetGUID();
            auto account = std::make_shared<AltAccount>();

            handler->GetSession()->GetQueryProcessor().AddCallback(
                CharacterDatabase.AsyncQuery(BuildAccountLookupQuery(target).c_str())
                .WithChainingCallback([viewerGuid, asked, addon, sequence, account](QueryCallback& chain, QueryResult result)
                {
                    // Nothing matched: say so and let the chain end here. A
                    // callback that sets no next query simply stops.
                    if (!ReadAltAccount(std::move(result), *account))
                    {
                        DeliverAlts(viewerGuid, asked, *account, {}, addon, sequence, false);
                        return;
                    }

                    chain.SetNextQuery(CharacterDatabase.AsyncQuery(BuildAltsQuery(account->id).c_str()));
                })
                .WithCallback([viewerGuid, asked, addon, sequence, account, live = std::move(live)](QueryResult result)
                {
                    std::vector<AltRow> alts;
                    AppendAltRows(std::move(result), live, alts);
                    DeliverAlts(viewerGuid, asked, *account, alts, addon, sequence, true);
                }));
            return true;
        }
    };
}

void AddSC_centurion_gm_online_feed()
{
    new centurion_gm_online_config();
    new centurion_gm_online_commands();
}
