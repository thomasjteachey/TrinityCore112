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
// Replies ride the CCGAME addon whisper everything else uses. One reply is:
//
//   GMOB:<seq>|<withBots>                           begin; the addon starts a fresh list
//   GMOR:<realmId>|<name>|<people>|<bots>|<live>     one per realm; live = from memory
//   GMOZ:<zoneId>,<name>;...                        names of the zones the rows use
//   GMOP:<realmId>|<name>,<lvl>,<class>,<race>,<zone>,<flags>,<account>;...
//   GMOE:<seq>                                      end; the addon swaps the list in
//
// flags: 1 bot, 2 GM account, 4 in a battleground or arena, 8 dead.
//
// Config:
//   Centurion.GMOnline.Enable            1
//   Centurion.GMOnline.Realms            "5:bpluscharacters,4:lpluscharacters"
//                                        realm id : characters schema, every realm
//                                        to show; this realm's own entry is served
//                                        from memory and its schema is not used
//   Centurion.GMOnline.BotAccountPrefix  "PLAYERBOT"

#include "Chat.h"
#include "ChatCommand.h"
#include "Configuration/Config.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Log.h"
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
#include <mutex>
#include <set>
#include <sstream>
#include <string>
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
        ROW_BOT  = 1,
        ROW_GM   = 2,
        ROW_BG   = 4,
        ROW_DEAD = 8,
    };

    bool s_enabled = true;
    std::vector<OnlineRealm> s_realms;
    std::string s_botPrefix = "PLAYERBOT";
    std::string s_authSchema;
    uint32 s_sequence = 0;

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

        std::lock_guard<std::mutex> guard(s_realmNameLock);
        s_realmNamesLoaded = false;
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
            row.name = player->GetName();
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
            sql << "SELECT CAST(" << other.id << " AS UNSIGNED), c.name, a.username, c.level, c.class, c.race, c.zone, c.map, "
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
            row.name = fields[1].GetString();
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

    // From the viewer to the viewer, the way the core's own addon channel
    // replies (AddonChannelCommandHandler::Send). The addon accepts the feed only
    // as a whisper from its own character: nobody else can send one of those,
    // so a player cannot plant rows in a GM's list.
    void SendTagged(Player* viewer, std::string const& tag, std::string const& payload)
    {
        std::string const message = "CCGAME\t" + tag + ":" + payload;
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, viewer, viewer, message);
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
            for (std::string_view word : Trinity::Tokenize(args, ' ', false))
            {
                if (word == "addon")
                    addon = true;
                else if (word == "bots")
                    withBots = true;
            }

            uint32 const sequence = ++s_sequence;

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
    };
}

void AddSC_centurion_gm_online_feed()
{
    new centurion_gm_online_config();
    new centurion_gm_online_commands();
}
