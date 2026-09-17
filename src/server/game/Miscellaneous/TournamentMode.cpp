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

#include "Miscellaneous/TournamentMode.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GameTime.h"
#include "Log.h"
#include "Map.h"
#include "MapManager.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "PetDefines.h"
#include "Player.h"
#include "SpellAuraEffects.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringConvert.h"
#include "Timer.h"
#include "Util.h"
#include "World.h"
#include "WorldSession.h"
#include "WorldPacket.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace Tournament
{
namespace
{
    struct InnateSpell
    {
        uint32 SpellId = 0;
        uint32 ClassMask = 0;   // 0 = every class
    };

    struct Settings
    {
        bool Enabled = false;
        bool WaiveReagents = false;
        bool TournamentWaiveReagents = true;
        uint8 StartLevel = 60;
        uint32 QueueMinLevel = 60;
        bool HasHome = false;
        uint32 HomeMap = 0;
        float HomeX = 0.0f;
        float HomeY = 0.0f;
        float HomeZ = 0.0f;
        float HomeO = 0.0f;
        std::unordered_set<uint32> AllowedZones;
        std::unordered_set<uint32> AllowedCreatures;
        std::unordered_set<uint32> AllowedLootObjects;
        std::vector<InnateSpell> InnateSpells;
        std::vector<uint32> ProfessionSpells;
        uint32 PhaseMask = 0;
        uint32 WorldPhaseMask = 0;
        bool MaxWeaponSkill = true;
        bool MaxSkillForLevel = true;
        int32 DeathSicknessLevel = 61;
        bool ConsumeAmmo = false;
        bool PetHappinessDecay = false;
        bool ResetDuelCooldowns = true;
        bool ResetDuelHealthMana = true;
    };

    Settings Config;

    // A character this close to the home location is never outside the grounds.
    constexpr float HomeRadius = 40.0f;
    // A typo such as "1-900000" must not allocate a set the size of a DBC.
    constexpr uint32 MaxIdRange = 100000;

    // Startup data, read-only once loaded.
    std::unordered_map<uint16, CreateInfo> CreateInfoStore;   // race << 8 | class
    bool KitProcedureExists = false;

    uint16 CreateInfoKey(uint32 race, uint32 playerClass)
    {
        return uint16((race << 8) | playerClass);
    }

    std::string_view Trim(std::string_view token)
    {
        while (!token.empty() && (token.front() == ' ' || token.front() == '\t'))
            token.remove_prefix(1);
        while (!token.empty() && (token.back() == ' ' || token.back() == '\t'))
            token.remove_suffix(1);
        return token;
    }

    // "1, 2,3, 10-12" -> {1, 2, 3, 10, 11, 12}. Neither Trinity::StringTo nor
    // std::from_chars tolerates whitespace, so every token is trimmed first; a
    // token that does not parse is reported rather than silently dropped.
    std::vector<uint32> ParseIdList(char const* key, std::string const& raw)
    {
        std::vector<uint32> ids;
        for (std::string_view token : Trinity::Tokenize(raw, ',', false))
        {
            token = Trim(token);
            if (token.empty())
                continue;

            std::size_t const dash = token.find('-');
            if (dash == std::string_view::npos)
            {
                if (Optional<uint32> id = Trinity::StringTo<uint32>(token))
                    ids.push_back(*id);
                else
                    TC_LOG_ERROR("server.loading", "{}: ignoring '{}', not a number.", key, token);
                continue;
            }

            Optional<uint32> const first = Trinity::StringTo<uint32>(Trim(token.substr(0, dash)));
            Optional<uint32> const last = Trinity::StringTo<uint32>(Trim(token.substr(dash + 1)));
            if (!first || !last || *first > *last || *last - *first >= MaxIdRange)
            {
                TC_LOG_ERROR("server.loading", "{}: ignoring '{}', expected a range first-last of at most {} ids.", key, token, MaxIdRange);
                continue;
            }

            for (uint32 offset = 0; offset <= *last - *first; ++offset)
                ids.push_back(*first + offset);
        }
        return ids;
    }

    bool IsPlainPlayer(Player const* player)
    {
        return player && player->GetSession() && !player->GetSession()->IsVirtualSession();
    }
}

void LoadConfig()
{
    Settings loaded;
    loaded.Enabled = sConfigMgr->GetBoolDefault("Centurion.Tournament.Enable", false);
    loaded.WaiveReagents = sConfigMgr->GetBoolDefault("Centurion.Pvp.WaiveReagentsAndAmmo", false);
    loaded.TournamentWaiveReagents = sConfigMgr->GetBoolDefault("Centurion.Tournament.WaiveReagents", true);
    loaded.StartLevel = uint8(std::clamp<int32>(sConfigMgr->GetIntDefault("Centurion.Tournament.StartLevel", 60), 1, STRONG_MAX_LEVEL));
    loaded.QueueMinLevel = uint32(std::max<int32>(1, sConfigMgr->GetIntDefault("Centurion.Tournament.QueueMinLevel", 60)));

    for (uint32 id : ParseIdList("Centurion.Tournament.AllowedZones", sConfigMgr->GetStringDefault("Centurion.Tournament.AllowedZones", "")))
        loaded.AllowedZones.insert(id);

    for (uint32 id : ParseIdList("Centurion.Tournament.AllowedCreatures", sConfigMgr->GetStringDefault("Centurion.Tournament.AllowedCreatures", "")))
        loaded.AllowedCreatures.insert(id);

    for (uint32 id : ParseIdList("Centurion.Tournament.AllowedLootObjects", sConfigMgr->GetStringDefault("Centurion.Tournament.AllowedLootObjects", "")))
        loaded.AllowedLootObjects.insert(id);

    // "spell[:classmask]"
    std::string const rawInnate = sConfigMgr->GetStringDefault("Centurion.Tournament.InnateSpells", "29073,18610,22734:1494");
    for (std::string_view token : Trinity::Tokenize(rawInnate, ',', false))
    {
        token = Trim(token);
        if (token.empty())
            continue;

        InnateSpell innate;
        std::string_view spellPart = token;
        std::string_view maskPart;
        if (std::size_t colon = token.find(':'); colon != std::string_view::npos)
        {
            spellPart = Trim(token.substr(0, colon));
            maskPart = Trim(token.substr(colon + 1));
        }

        Optional<uint32> spellId = Trinity::StringTo<uint32>(spellPart);
        Optional<uint32> classMask = maskPart.empty() ? Optional<uint32>(0) : Trinity::StringTo<uint32>(maskPart);
        if (!spellId || !classMask)
        {
            TC_LOG_ERROR("server.loading", "Centurion.Tournament.InnateSpells: ignoring '{}', expected spell[:classmask].", token);
            continue;
        }

        innate.SpellId = *spellId;
        innate.ClassMask = *classMask;
        loaded.InnateSpells.push_back(innate);
    }

    loaded.ProfessionSpells = ParseIdList("Centurion.Tournament.ProfessionSpells",
        sConfigMgr->GetStringDefault("Centurion.Tournament.ProfessionSpells", "11611,9785,13920,12656,10662,12180"));

    loaded.PhaseMask = uint32(sConfigMgr->GetIntDefault("Centurion.Tournament.PhaseMask", 0));
    if (loaded.PhaseMask == uint32(PHASEMASK_ANYWHERE))
    {
        TC_LOG_ERROR("server.loading", "Centurion.Tournament.PhaseMask cannot be every phase; tournament phasing is off.");
        loaded.PhaseMask = 0;
    }
    else if (loaded.PhaseMask & PHASEMASK_NORMAL)
    {
        // The normal phase is everyone's: NPCs spawned there are not hidden
        // from world characters, so the bit adds nothing but confusion.
        TC_LOG_WARN("server.loading", "Centurion.Tournament.PhaseMask includes the normal phase (1); ignoring that bit.");
        loaded.PhaseMask &= ~uint32(PHASEMASK_NORMAL);
    }

    // The mirror image: a phase only world characters carry, for world creatures
    // tournament characters must not see (New Hearthglen's stock town under the
    // tournament vendors). The normal phase and the tournament phase are both
    // shared or taken, so neither bit can be in it.
    loaded.WorldPhaseMask = uint32(sConfigMgr->GetIntDefault("Centurion.Tournament.WorldPhaseMask", 0));
    if (loaded.WorldPhaseMask == uint32(PHASEMASK_ANYWHERE))
    {
        TC_LOG_ERROR("server.loading", "Centurion.Tournament.WorldPhaseMask cannot be every phase; world-only phasing is off.");
        loaded.WorldPhaseMask = 0;
    }
    else if (loaded.WorldPhaseMask & (PHASEMASK_NORMAL | loaded.PhaseMask))
    {
        TC_LOG_WARN("server.loading", "Centurion.Tournament.WorldPhaseMask shares bits with the normal or tournament phase; ignoring those bits.");
        loaded.WorldPhaseMask &= ~(uint32(PHASEMASK_NORMAL) | loaded.PhaseMask);
    }

    loaded.MaxWeaponSkill = sConfigMgr->GetBoolDefault("Centurion.Tournament.AlwaysMaxWeaponSkill", true);
    loaded.MaxSkillForLevel = sConfigMgr->GetBoolDefault("Centurion.Tournament.AlwaysMaxSkillForLevel", true);
    loaded.DeathSicknessLevel = sConfigMgr->GetIntDefault("Centurion.Tournament.DeathSicknessLevel", 61);
    loaded.ConsumeAmmo = sConfigMgr->GetBoolDefault("Centurion.Tournament.ConsumeAmmo", false);
    loaded.PetHappinessDecay = sConfigMgr->GetBoolDefault("Centurion.Tournament.PetHappinessDecay", false);
    loaded.ResetDuelCooldowns = sConfigMgr->GetBoolDefault("Centurion.Tournament.ResetDuelCooldowns", true);
    loaded.ResetDuelHealthMana = sConfigMgr->GetBoolDefault("Centurion.Tournament.ResetDuelHealthMana", true);

    // "map x y z o"
    std::string const rawHome = sConfigMgr->GetStringDefault("Centurion.Tournament.HomeLocation", "");
    if (!rawHome.empty())
    {
        std::vector<std::string_view> parts;
        for (std::string_view token : Trinity::Tokenize(rawHome, ' ', false))
            if (!Trim(token).empty())
                parts.push_back(Trim(token));

        Optional<uint32> map = parts.size() >= 4 ? Trinity::StringTo<uint32>(parts[0]) : Optional<uint32>();
        Optional<float> x = parts.size() >= 4 ? Trinity::StringTo<float>(parts[1]) : Optional<float>();
        Optional<float> y = parts.size() >= 4 ? Trinity::StringTo<float>(parts[2]) : Optional<float>();
        Optional<float> z = parts.size() >= 4 ? Trinity::StringTo<float>(parts[3]) : Optional<float>();
        Optional<float> o = parts.size() >= 5 ? Trinity::StringTo<float>(parts[4]) : Optional<float>(0.0f);

        // At startup the config is read before the DBCs load, so the map store
        // is still empty and every map id would look invalid: check only the
        // coordinates then, and the map too on a `.reload config`.
        // LoadCreateInfo checks the map once the DBCs are in.
        bool const mapStoreLoaded = sMapStore.GetNumRows() != 0;
        if (map && x && y && z && o && Trinity::IsValidMapCoord(*x, *y, *z, *o)
            && (!mapStoreLoaded || MapManager::IsValidMapCoord(*map, *x, *y, *z, *o)))
        {
            loaded.HasHome = true;
            loaded.HomeMap = *map;
            loaded.HomeX = *x;
            loaded.HomeY = *y;
            loaded.HomeZ = *z;
            loaded.HomeO = *o;
        }
        else
            TC_LOG_ERROR("server.loading", "Centurion.Tournament.HomeLocation: '{}' is not a valid \"map x y z o\" position.", rawHome);
    }

    if (loaded.Enabled)
    {
        if (loaded.AllowedZones.empty())
            TC_LOG_WARN("server.loading", "Centurion.Tournament.AllowedZones is empty: tournament characters are not confined to any zone.");
        else if (!loaded.HasHome)
            TC_LOG_WARN("server.loading", "Centurion.Tournament.HomeLocation is not set: tournament characters found outside their zones cannot be sent back.");
        if (loaded.AllowedCreatures.empty())
            TC_LOG_WARN("server.loading", "Centurion.Tournament.AllowedCreatures is empty: tournament characters may talk to every NPC.");

        TC_LOG_INFO("server.loading", "Tournament characters enabled: {} zone(s), {} creature(s), {} innate spell(s), {} profession(s), phase mask {}.",
            loaded.AllowedZones.size(), loaded.AllowedCreatures.size(), loaded.InnateSpells.size(), loaded.ProfessionSpells.size(), loaded.PhaseMask);
    }

    Config = std::move(loaded);
}

bool IsEnabled()
{
    return Config.Enabled;
}

bool IsTournamentCharacter(Player const* player)
{
    return Config.Enabled && player && player->HasTournamentModeFlag();
}

bool IsTournamentCharacter(ObjectGuid guid)
{
    if (!Config.Enabled || !guid.IsPlayer())
        return false;

    if (Player const* player = ObjectAccessor::FindConnectedPlayer(guid))
        return player->HasTournamentModeFlag();

    return sCharacterCache->IsTournamentCharacterByGuid(guid);
}

bool AreSeparated(Player const* a, Player const* b)
{
    if (!Config.Enabled || !a || !b)
        return false;

    return IsTournamentCharacter(a) != IsTournamentCharacter(b);
}

bool AreSeparated(ObjectGuid a, ObjectGuid b)
{
    if (!Config.Enabled)
        return false;

    return IsTournamentCharacter(a) != IsTournamentCharacter(b);
}

bool AreKeptFromFighting(Player const* a, Player const* b)
{
    if (a == b || !AreSeparated(a, b))
        return false;

    if (Map const* map = a->FindMap())
        if (map->IsBattlegroundOrArena())
            return false;

    return !(a->IsInGurubashiBattleRing() && b->IsInGurubashiBattleRing());
}

bool QueuesInTournamentPool(Player const* player)
{
    if (!Config.Enabled || !player)
        return false;

    if (IsTournamentCharacter(player))
        return true;

    return player->HasTournamentQueueFlag() && player->GetLevel() >= Config.QueueMinLevel;
}

bool CanToggleTournamentQueue(Player const* player)
{
    if (!Config.Enabled || !player || IsTournamentCharacter(player) || player->GetLevel() < Config.QueueMinLevel)
        return false;

    // A queued group keeps the pool it joined with, so the switch waits until
    // the character is out of every queue and every match.
    return !player->InBattlegroundQueue() && !player->InBattleground();
}

uint32 GetQueueMinLevel()
{
    return Config.QueueMinLevel;
}

void SendQueueState(Player* player)
{
    if (!player || !player->GetSession() || player->GetSession()->IsVirtualSession())
        return;

    bool const on = QueuesInTournamentPool(player);
    bool const locked = !Config.Enabled || IsTournamentCharacter(player) || player->GetLevel() < Config.QueueMinLevel;
    std::string const message = Trinity::StringFormat("CCGAME\tTQUEUE:{}:{}", on ? 1 : 0, locked ? 1 : 0);

    // The GUID overload: the Player* one rewrites LANG_ADDON to Universal and
    // the addon data would show up as a whisper.
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player->GetGUID(), player->GetGUID(),
        message, 0, player->GetName(), player->GetName());
    player->SendDirectMessage(&data);
}

void SendGurubashiChestState(Player* player)
{
    if (!player || !player->GetSession() || player->GetSession()->IsVirtualSession())
        return;

    std::string const message = Trinity::StringFormat("CCGAME\tGURUCHEST:{}", player->HasGurubashiChestOptOut() ? 0 : 1);
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player->GetGUID(), player->GetGUID(),
        message, 0, player->GetName(), player->GetName());
    player->SendDirectMessage(&data);
}

namespace
{
    // Written from the world thread (the hourly check, the despawn) and from the
    // chest's map thread (looting), read by whichever thread answers the request.
    std::atomic<int64> GurubashiChestNextCheck{ 0 };
    std::atomic<int64> GurubashiChestExpiresAt{ 0 };
}

void SetGurubashiChestClock(time_t nextCheck, time_t chestExpiresAt)
{
    GurubashiChestNextCheck = int64(nextCheck);
    GurubashiChestExpiresAt = int64(chestExpiresAt);
}

void SendGurubashiChestTimer(Player* player)
{
    if (!player || !player->GetSession() || player->GetSession()->IsVirtualSession())
        return;

    int64 const now = int64(GameTime::GetGameTime());
    int64 const expiresAt = GurubashiChestExpiresAt;
    bool const chestOut = expiresAt > now;
    int64 const until = chestOut ? expiresAt : int64(GurubashiChestNextCheck);
    if (until <= 0)
        return;

    std::string const message = Trinity::StringFormat("CCGAME\tGURUTIMER:{}:{}", chestOut ? 1 : 0, std::max<int64>(0, until - now));
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player->GetGUID(), player->GetGUID(),
        message, 0, player->GetName(), player->GetName());
    player->SendDirectMessage(&data);
}

bool HandleAddonRequest(Player* sender, uint32 lang, std::string const& msg)
{
    if (!sender || lang != LANG_ADDON)
        return false;

    // The Gurubashi chest's clock beside that toggle (read only).
    if (msg == "CCGAMEREQ\tGURUTIMER")
    {
        SendGurubashiChestTimer(sender);
        return true;
    }

    // The Gurubashi chest toggle (Battlegrounds tab): whether the hourly chest
    // counts this character and pulls it into the Battle Ring. Realm-agnostic -
    // every realm on the branch runs the chest.
    static constexpr std::string_view ChestRequest = "CCGAMEREQ\tGURUCHEST";
    if (StringStartsWith(msg, ChestRequest))
    {
        std::string_view const argument = std::string_view(msg).substr(ChestRequest.size());
        if (argument == ":1" || argument == ":0")
            sender->SetGurubashiChestOptOut(argument == ":0");

        SendGurubashiChestState(sender);
        return true;
    }

    static constexpr std::string_view Request = "CCGAMEREQ\tTQUEUE";
    if (!StringStartsWith(msg, Request))
        return false;

    // No answer on a realm without character modes: the Battlegrounds tab only
    // shows the tournament-queue checkbox once the server has answered.
    if (!Config.Enabled)
        return true;

    std::string_view const argument = std::string_view(msg).substr(Request.size());
    if (argument == ":1" || argument == ":0")
    {
        bool const on = argument == ":1";
        // A locked toggle is simply answered with its real state.
        if (CanToggleTournamentQueue(sender))
            sender->SetTournamentQueueFlag(on);
    }

    SendQueueState(sender);
    return true;
}

bool HasConfinement()
{
    return Config.Enabled && !Config.AllowedZones.empty();
}

bool IsLocationAllowed(uint32 mapId, uint32 zoneId, uint32 areaId)
{
    if (!HasConfinement())
        return true;

    // Queued matches carry their own rules; a battleground or arena is never
    // "leaving" the tournament grounds.
    if (MapEntry const* entry = sMapStore.LookupEntry(mapId))
        if (entry->IsBattlegroundOrArena())
            return true;

    return Config.AllowedZones.count(zoneId) || Config.AllowedZones.count(areaId);
}

bool IsLocationAllowed(uint32 mapId, float x, float y, float z)
{
    if (!HasConfinement())
        return true;

    if (MapEntry const* entry = sMapStore.LookupEntry(mapId))
        if (entry->IsBattlegroundOrArena())
            return true;

    // The home location is always a legal destination, so a misconfigured zone
    // list can never bounce a character between two refusals.
    if (Config.HasHome && mapId == Config.HomeMap &&
        std::abs(x - Config.HomeX) < 1.0f && std::abs(y - Config.HomeY) < 1.0f && std::abs(z - Config.HomeZ) < 5.0f)
        return true;

    uint32 zoneId = 0;
    uint32 areaId = 0;
    sMapMgr->GetZoneAndAreaId(PHASEMASK_NORMAL, zoneId, areaId, mapId, x, y, z);
    return IsLocationAllowed(mapId, zoneId, areaId);
}

bool GetHomeLocation(WorldLocation& out)
{
    if (!Config.HasHome)
        return false;

    out.WorldRelocate(Config.HomeMap, Config.HomeX, Config.HomeY, Config.HomeZ, Config.HomeO);
    return true;
}

bool EnforceConfinement(Player* player)
{
    if (!HasConfinement() || !IsTournamentCharacter(player) || player->IsGameMaster())
        return false;

    if (!player->IsInWorld() || player->IsBeingTeleported() || player->IsInFlight() || !IsPlainPlayer(player))
        return false;

    // Custom games are open to tournament characters, lobby included (it is a
    // private copy of Kalimdor, not a battleground map).
    if (player->IsInCustomGameLobby())
        return false;

    if (IsLocationAllowed(player->GetMapId(), player->GetZoneId(), player->GetAreaId()))
        return false;

    WorldLocation home;
    if (!GetHomeLocation(home))
        return false;

    // Home is never outside: a zone list that leaves it out must not send a
    // character home again every update.
    if (player->GetMapId() == home.GetMapId() && player->GetExactDist2dSq(home.GetPositionX(), home.GetPositionY()) < HomeRadius * HomeRadius)
        return false;

    SendRefusal(player, "leave the tournament grounds");
    player->TeleportTo(home);
    return true;
}

uint32 GetExtraPhaseMask(Player const* player)
{
    if (!Config.Enabled || !player)
        return 0;

    return IsTournamentCharacter(player) ? Config.PhaseMask : Config.WorldPhaseMask;
}

void RefreshPhase(Player* player)
{
    // Game masters see every phase until they leave GM mode, which recomputes
    // the phase through Player::SetPhaseMask anyway.
    if (!player || player->IsGameMaster())
        return;

    // The computation AuraEffect::HandlePhase and Player::SetGameMaster make;
    // Player::SetPhaseMask adds the tournament phase on top.
    uint32 phase = 0;
    for (AuraEffect const* effect : player->GetAuraEffectsByType(SPELL_AURA_PHASE))
        phase |= uint32(effect->GetMiscValue());
    if (!phase)
        phase = PHASEMASK_NORMAL;

    player->SetPhaseMask(phase, player->IsInWorld());
}

bool IsPhaseStale(Player const* player)
{
    if (!player || player->IsGameMaster())
        return false;

    // Carries its own mode's phase and not the other mode's.
    uint32 const wanted = GetExtraPhaseMask(player);
    uint32 const unwanted = (Config.PhaseMask | Config.WorldPhaseMask) & ~wanted;
    return (player->GetPhaseMask() & wanted) != wanted || (player->GetPhaseMask() & unwanted) != 0;
}

bool CanInteractWithCreature(Player const* player, Creature const* creature, uint32 npcFlags)
{
    if (!IsTournamentCharacter(player) || player->IsGameMaster() || !creature)
        return true;

    // Battleground spirit guides, battlemasters inside the match and other
    // match furniture belong to the match, not to the world; the custom-game
    // lobby's staff belong to custom games, which are open to everyone.
    if (Map const* map = player->FindMap())
        if (map->IsBattlegroundOrArena())
            return true;
    if (player->IsInCustomGameLobby())
        return true;

    // A character's own pet or vehicle is not an NPC in any sense that matters.
    if (creature->GetOwnerGUID() == player->GetGUID() || creature->GetCharmerOrOwnerGUID() == player->GetGUID())
        return true;

    // The auction house and flight masters are closed to tournament characters
    // whatever the list says.
    if (npcFlags & UNIT_NPC_FLAG_AUCTIONEER)
    {
        SendRefusal(player, "use the auction house");
        return false;
    }
    if (npcFlags & UNIT_NPC_FLAG_FLIGHTMASTER)
    {
        SendRefusal(player, "use flight masters");
        return false;
    }

    if (Config.AllowedCreatures.empty())
        return true;

    // Nobody is left unable to resurrect or to queue.
    if (creature->HasNpcFlag(NPCFlags(UNIT_NPC_FLAG_SPIRITHEALER | UNIT_NPC_FLAG_SPIRITGUIDE | UNIT_NPC_FLAG_BATTLEMASTER)))
        return true;

    if (Config.AllowedCreatures.count(creature->GetEntry()))
        return true;

    SendRefusal(player, "deal with this NPC");
    return false;
}

bool CanOpenLoot(Player const* player, ObjectGuid lootGuid)
{
    if (!IsTournamentCharacter(player) || player->IsGameMaster())
        return true;

    // Its own items: containers, disenchanting, prospecting, milling. Gear on
    // the tournament vendors arrives in boxes.
    if (lootGuid.IsItem())
        return true;

    // Player bones are spoils of a fight between people, not PvE loot.
    if (lootGuid.IsCorpse())
        return true;

    // Everything inside a battleground or arena belongs to the match.
    if (MayReceiveGroupLoot(player))
        return true;

    if (lootGuid.IsGameObject() && !Config.AllowedLootObjects.empty())
        if (GameObject const* go = player->GetMap()->GetGameObject(lootGuid))
            if (Config.AllowedLootObjects.count(go->GetEntry()))
                return true;

    SendRefusal(player, "loot anything outside battlegrounds and arenas");
    return false;
}

bool MayReceiveGroupLoot(Player const* player)
{
    if (!IsTournamentCharacter(player) || player->IsGameMaster())
        return true;

    Map const* map = player->FindMap();
    return map && map->IsBattlegroundOrArena();
}

bool CanUseGuildBank(Player const* player)
{
    if (!IsTournamentCharacter(player) || player->IsGameMaster())
        return true;

    SendRefusal(player, "use the guild bank");
    return false;
}

uint8 GetStartLevel()
{
    return Config.StartLevel;
}

bool IsTournamentNameMarker(std::string const& rawName)
{
    std::wstring name;
    if (!Utf8toWStr(rawName, name) || name.size() < 2)
        return false;

    // A letter is lowercase when it has an uppercase form, and the other way
    // round; scripts without case never match, so they always create a world
    // character.
    auto const isLower = [](wchar_t c) { return wcharToUpper(c) != c; };
    auto const isUpper = [](wchar_t c) { return wcharToLower(c) != c; };

    if (!isLower(name[0]))
        return false;

    for (std::size_t i = 1; i < name.size(); ++i)
        if (!isUpper(name[i]))
            return false;

    return true;
}

void ApplyCharacterKit(Player* player)
{
    if (!IsTournamentCharacter(player))
        return;

    uint32 const classMask = player->GetClassMask();
    for (InnateSpell const& innate : Config.InnateSpells)
    {
        if (innate.ClassMask && !(innate.ClassMask & classMask))
            continue;

        if (player->HasSpell(innate.SpellId))
            continue;

        if (!sSpellMgr->GetSpellInfo(innate.SpellId))
        {
            TC_LOG_ERROR("entities.player", "Centurion.Tournament.InnateSpells: spell {} does not exist.", innate.SpellId);
            continue;
        }

        player->LearnSpell(innate.SpellId, false);
    }

    // Each entry is a profession RANK spell (Artisan Blacksmithing 9785...).
    // Its SPELL_EFFECT_SKILL names the skill and the rank's cap; learning it
    // opens the profession, and the skill is then raised straight to that cap
    // so profession-gated gear (reflectors, engineering trinkets) is usable.
    for (uint32 rankSpell : Config.ProfessionSpells)
    {
        SpellLearnSkillNode const* node = sSpellMgr->GetSpellLearnSkill(rankSpell);
        if (!node || !node->skill)
        {
            TC_LOG_ERROR("entities.player", "Centurion.Tournament.ProfessionSpells: spell {} does not teach a skill.", rankSpell);
            continue;
        }

        if (!player->HasSpell(rankSpell))
            player->LearnSpell(rankSpell, false);

        uint16 const cap = node->maxvalue ? node->maxvalue : uint16(player->GetMaxSkillValueForLevel());
        if (player->GetPureSkillValue(node->skill) < cap || player->GetPureMaxSkillValue(node->skill) < cap)
            player->SetSkill(node->skill, std::max<uint16>(node->step, player->GetSkillStep(node->skill)), cap, cap);
    }
}

void LoadCreateInfo()
{
    uint32 const oldMSTime = getMSTime();
    CreateInfoStore.clear();
    KitProcedureExists = false;

    // LoadConfig could only check HomeLocation's coordinates: the DBCs had not
    // loaded yet at startup.
    if (Config.HasHome && !MapManager::IsValidMapCoord(Config.HomeMap, Config.HomeX, Config.HomeY, Config.HomeZ, Config.HomeO))
    {
        TC_LOG_ERROR("server.loading", "Centurion.Tournament.HomeLocation: map {} does not exist on this realm.", Config.HomeMap);
        Config.HasHome = false;
    }

    // Probe before querying: a missing table aborts the server
    // (MySQLConnection, ER_NO_SUCH_TABLE), and only the Centurion realms have
    // these. Every other realm on the branch stops right here.
    std::unordered_set<std::string> tables;
    if (QueryResult result = WorldDatabase.Query("SELECT TABLE_NAME FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME IN "
        "('playercreateinfo_tournament', 'playercreateinfo_item_tournament', 'playercreateinfo_spell_custom_tournament', 'playercreateinfo_outfit_tournament')"))
    {
        do
            tables.insert(result->Fetch()[0].GetString());
        while (result->NextRow());
    }

    if (CharacterDatabase.Query("SELECT 1 FROM information_schema.ROUTINES WHERE ROUTINE_SCHEMA = DATABASE() AND ROUTINE_TYPE = 'PROCEDURE' AND ROUTINE_NAME = 'createTournamentKit'"))
        KitProcedureExists = true;

    if (tables.empty())
    {
        TC_LOG_INFO("server.loading", ">> No tournament create data on this realm (playercreateinfo_*_tournament absent).");
        return;
    }

    bool const hasItems = tables.count("playercreateinfo_item_tournament") != 0;
    bool const hasSpells = tables.count("playercreateinfo_spell_custom_tournament") != 0;

    // An entry for every pair the world create data can make, so a table that
    // exists but has no row for a pair still replaces the world list - with an
    // empty one - rather than falling back to it.
    for (uint32 race = RACE_HUMAN; race < MAX_RACES; ++race)
    {
        for (uint32 playerClass = CLASS_WARRIOR; playerClass < MAX_CLASSES; ++playerClass)
        {
            if (!sObjectMgr->GetPlayerInfo(race, playerClass))
                continue;

            CreateInfo& info = CreateInfoStore[CreateInfoKey(race, playerClass)];
            info.HasItems = hasItems;
            info.HasCustomSpells = hasSpells;
        }
    }

    auto const forEachPair = [](uint32 race, uint32 playerClass, std::function<void(CreateInfo&)> const& apply)
    {
        for (auto& [key, info] : CreateInfoStore)
        {
            if ((race && uint32(key >> 8) != race) || (playerClass && uint32(key & 0xFF) != playerClass))
                continue;

            apply(info);
        }
    };

    uint32 positions = 0;
    if (tables.count("playercreateinfo_tournament"))
    {
        //                                                  0     1      2    3           4           5           6
        if (QueryResult result = WorldDatabase.Query("SELECT race, class, map, position_x, position_y, position_z, orientation FROM playercreateinfo_tournament"))
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 const race = fields[0].GetUInt8();
                uint32 const playerClass = fields[1].GetUInt8();
                uint32 const mapId = fields[2].GetUInt16();
                float const x = fields[3].GetFloat();
                float const y = fields[4].GetFloat();
                float const z = fields[5].GetFloat();
                float const o = fields[6].GetFloat();

                // Legionnaire+ lists pairs this realm cannot create; those are
                // simply not needed here.
                auto itr = CreateInfoStore.find(CreateInfoKey(race, playerClass));
                if (itr == CreateInfoStore.end())
                    continue;

                if (!MapManager::IsValidMapCoord(mapId, x, y, z, o) || sMapStore.LookupEntry(mapId)->Instanceable())
                {
                    TC_LOG_ERROR("sql.sql", "Invalid or instanced start position for race {} class {} in `playercreateinfo_tournament`, ignoring.", race, playerClass);
                    continue;
                }

                CreateInfo& info = itr->second;
                info.HasPosition = true;
                info.MapId = mapId;
                info.PositionX = x;
                info.PositionY = y;
                info.PositionZ = z;
                info.Orientation = o;
                ++positions;
            }
            while (result->NextRow());
        }
    }

    uint32 items = 0;
    if (hasItems)
    {
        //                                                  0     1      2       3
        if (QueryResult result = WorldDatabase.Query("SELECT race, class, itemid, amount FROM playercreateinfo_item_tournament"))
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 const race = fields[0].GetUInt8();
                uint32 const playerClass = fields[1].GetUInt8();
                uint32 const itemId = fields[2].GetUInt32();
                int32 const amount = fields[3].GetInt8();

                if (!sObjectMgr->GetItemTemplate(itemId))
                {
                    TC_LOG_ERROR("sql.sql", "Item {} (race {} class {}) in `playercreateinfo_item_tournament` does not exist, ignoring.", itemId, race, playerClass);
                    continue;
                }

                if (!amount)
                {
                    TC_LOG_ERROR("sql.sql", "Item {} (race {} class {}) in `playercreateinfo_item_tournament` has amount 0, ignoring.", itemId, race, playerClass);
                    continue;
                }

                // The world loader answers a negative amount by editing the DBC
                // starting outfit itself, for every character of the pair; a
                // tournament row may only take the item out of tournament
                // characters' outfits.
                forEachPair(race, playerClass, [&](CreateInfo& info)
                {
                    if (amount > 0)
                        info.Items.emplace_back(itemId, uint32(amount));
                    else
                        info.RemovedOutfitItems.push_back(itemId);
                });
                ++items;
            }
            while (result->NextRow());
        }
    }

    uint32 spells = 0;
    if (hasSpells)
    {
        //                                                  0          1          2
        if (QueryResult result = WorldDatabase.Query("SELECT racemask, classmask, Spell FROM playercreateinfo_spell_custom_tournament"))
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 const raceMask = fields[0].GetUInt32();
                uint32 const classMask = fields[1].GetUInt32();
                uint32 const spellId = fields[2].GetUInt32();

                if ((raceMask && !(raceMask & RACEMASK_ALL_PLAYABLE)) || (classMask && !(classMask & CLASSMASK_ALL_PLAYABLE)))
                {
                    TC_LOG_ERROR("sql.sql", "Wrong race/class mask {}/{} for spell {} in `playercreateinfo_spell_custom_tournament`, ignoring.", raceMask, classMask, spellId);
                    continue;
                }

                if (!sSpellMgr->GetSpellInfo(spellId))
                {
                    TC_LOG_ERROR("sql.sql", "Spell {} in `playercreateinfo_spell_custom_tournament` does not exist, ignoring.", spellId);
                    continue;
                }

                for (auto& [key, info] : CreateInfoStore)
                {
                    uint32 const race = key >> 8;
                    uint32 const playerClass = key & 0xFF;
                    if ((raceMask && !(raceMask & (1 << (race - 1)))) || (classMask && !(classMask & (1 << (playerClass - 1)))))
                        continue;

                    info.CustomSpells.push_back(spellId);
                }
                ++spells;
            }
            while (result->NextRow());
        }
    }

    // Legionnaire+'s CharStartOutfit.dbc, which geared its characters at creation;
    // Centurion's own DBC outfits are Barracks+'s. Item ids are already mapped to
    // the tournament copies in the table.
    uint32 outfitItems = 0;
    if (tables.count("playercreateinfo_outfit_tournament"))
    {
        //                                                  0     1      2       3
        if (QueryResult result = WorldDatabase.Query("SELECT race, class, gender, itemid FROM playercreateinfo_outfit_tournament ORDER BY race, class, gender, slot"))
        {
            do
            {
                Field* fields = result->Fetch();
                uint32 const race = fields[0].GetUInt8();
                uint32 const playerClass = fields[1].GetUInt8();
                uint8 const gender = fields[2].GetUInt8();
                uint32 const itemId = fields[3].GetUInt32();

                // Pairs this realm cannot create are simply not needed.
                auto itr = CreateInfoStore.find(CreateInfoKey(race, playerClass));
                if (itr == CreateInfoStore.end())
                    continue;

                if (gender >= itr->second.Outfit.size())
                {
                    TC_LOG_ERROR("sql.sql", "Invalid gender {} for race {} class {} in `playercreateinfo_outfit_tournament`, ignoring.", uint32(gender), race, playerClass);
                    continue;
                }

                if (!sObjectMgr->GetItemTemplate(itemId))
                {
                    TC_LOG_ERROR("sql.sql", "Item {} (race {} class {} gender {}) in `playercreateinfo_outfit_tournament` does not exist, ignoring.", itemId, race, playerClass, uint32(gender));
                    continue;
                }

                itr->second.Outfit[gender].push_back(itemId);
                ++outfitItems;
            }
            while (result->NextRow());
        }
    }

    TC_LOG_INFO("server.loading", ">> Loaded tournament create data: {} start position(s), {} item row(s), {} spell row(s), {} outfit item(s) for {} race/class pair(s); kit procedure {} in {} ms",
        positions, items, spells, outfitItems, CreateInfoStore.size(), KitProcedureExists ? "present" : "ABSENT", GetMSTimeDiffToNow(oldMSTime));
}

CreateInfo const* GetCreateInfo(uint8 race, uint8 playerClass)
{
    auto itr = CreateInfoStore.find(CreateInfoKey(race, playerClass));
    return itr != CreateInfoStore.end() ? &itr->second : nullptr;
}

bool GetStartLocation(uint8 race, uint8 playerClass, WorldLocation& out)
{
    if (CreateInfo const* info = GetCreateInfo(race, playerClass))
    {
        if (info->HasPosition)
        {
            out.WorldRelocate(info->MapId, info->PositionX, info->PositionY, info->PositionZ, info->Orientation);
            return true;
        }
    }

    return GetHomeLocation(out);
}

std::vector<uint32> const* GetCustomSpells(Player const* player)
{
    if (!IsTournamentCharacter(player))
        return nullptr;

    CreateInfo const* info = GetCreateInfo(player->GetRace(), player->GetClass());
    return (info && info->HasCustomSpells) ? &info->CustomSpells : nullptr;
}

void RunKitProcedure(Player const* newCharacter)
{
    if (!newCharacter)
        return;

    if (!KitProcedureExists)
    {
        TC_LOG_ERROR("entities.player.character", "Tournament character {} ({}) was created without its template kit: the characters database has no createTournamentKit procedure.",
            newCharacter->GetName(), newCharacter->GetGUID().ToString());
        return;
    }

    // Pet numbers come from the server's generator, never from MAX(id)+1 in
    // SQL, which would race every pet tamed or summoned since the last save.
    // A hunter template holds at most its current pet and a full stable.
    std::string petIds;
    if (newCharacter->GetClass() == CLASS_HUNTER)
    {
        for (uint8 i = 0; i <= MAX_PET_STABLES; ++i)
        {
            if (i)
                petIds += ',';
            petIds += std::to_string(sObjectMgr->GeneratePetNumber());
        }
    }

    CharacterDatabase.DirectPExecute("CALL createTournamentKit({}, {}, {}, '{}')",
        uint32(newCharacter->GetClass()), uint32(newCharacter->GetRace()), newCharacter->GetGUID().GetCounter(), petIds);
}

bool AlwaysMaxWeaponSkill(Player const* player)
{
    if (IsTournamentCharacter(player))
        return Config.MaxWeaponSkill;

    return sWorld->getBoolConfig(CONFIG_ALWAYS_MAXSKILL);
}

bool AlwaysMaxSkillForLevel(Player const* player)
{
    if (IsTournamentCharacter(player))
        return Config.MaxSkillForLevel;

    return sWorld->getBoolConfig(CONFIG_ALWAYS_MAX_SKILL_FOR_LEVEL);
}

int32 GetDeathSicknessLevel(Player const* player)
{
    if (IsTournamentCharacter(player))
        return Config.DeathSicknessLevel;

    return int32(sWorld->getIntConfig(CONFIG_DEATH_SICKNESS_LEVEL));
}

bool ConsumesAmmo(Player const* player)
{
    if (IsTournamentCharacter(player))
        return Config.ConsumeAmmo;

    return sWorld->getBoolConfig(CONFIG_CENTURION_CLASSIC_CONSUME_AMMO);
}

bool PetHappinessDecays(Player const* owner)
{
    if (IsTournamentCharacter(owner))
        return Config.PetHappinessDecay;

    return sWorld->getBoolConfig(CONFIG_CENTURION_CLASSIC_PET_HAPPINESS_DECAY);
}

bool ResetsDuelCooldowns(Player const* a, Player const* b)
{
    if (IsTournamentCharacter(a) || IsTournamentCharacter(b))
        return Config.ResetDuelCooldowns;

    return sWorld->getBoolConfig(CONFIG_RESET_DUEL_COOLDOWNS);
}

bool ResetsDuelHealthMana(Player const* a, Player const* b)
{
    if (IsTournamentCharacter(a) || IsTournamentCharacter(b))
        return Config.ResetDuelHealthMana;

    return sWorld->getBoolConfig(CONFIG_RESET_DUEL_HEALTH_MANA);
}

bool IsFreeReagentContext(Player const* player)
{
    if (!Config.WaiveReagents || !player)
        return false;

    if (Map const* map = player->FindMap())
        if (map->IsBattlegroundOrArena())
            return true;

    return player->duel && player->duel->State == DUEL_STATE_IN_PROGRESS;
}

bool HasReagentWaiver(Player const* player)
{
    if (Config.TournamentWaiveReagents && IsTournamentCharacter(player))
        return true;

    return IsFreeReagentContext(player);
}

bool IsReagentWaived(Player const* player, SpellInfo const* spellInfo)
{
    if (!spellInfo || !HasReagentWaiver(player))
        return false;

    // Combat spells only. The waiver is there so a fight is not decided by who
    // remembered their Blinding Powder - it must not turn a duel into a free
    // workshop, a free enchanter or a free portal.
    if (spellInfo->HasAttribute(SPELL_ATTR0_TRADESPELL))
        return false;

    for (SpellEffectInfo const& effect : spellInfo->GetEffects())
    {
        switch (effect.Effect)
        {
            case SPELL_EFFECT_CREATE_ITEM:
            case SPELL_EFFECT_CREATE_ITEM_2:
            case SPELL_EFFECT_CREATE_RANDOM_ITEM:
            case SPELL_EFFECT_CREATE_MANA_GEM:
            case SPELL_EFFECT_ENCHANT_ITEM:
            case SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY:
            case SPELL_EFFECT_ENCHANT_ITEM_PRISMATIC:
            case SPELL_EFFECT_ENCHANT_HELD_ITEM:
            case SPELL_EFFECT_DISENCHANT:
            case SPELL_EFFECT_PROSPECTING:
            case SPELL_EFFECT_MILLING:
            case SPELL_EFFECT_TELEPORT_UNITS:
            case SPELL_EFFECT_TRANS_DOOR:
            case SPELL_EFFECT_SUMMON_OBJECT_WILD:
            case SPELL_EFFECT_SUMMON_PLAYER:
                return false;
            default:
                break;
        }
    }

    return true;
}

void SendRefusal(Player const* player, char const* what)
{
    if (!player || !player->GetSession() || player->GetSession()->IsVirtualSession())
        return;

    // One notice per character per two seconds: several checks can refuse the
    // same click (a banker is looked up twice), and confinement retries every
    // update while a teleport is being set up. Handlers run on map threads,
    // hence the lock; this is only ever reached for a refusal.
    uint32 const nowMs = GameTime::GetGameTimeMS();
    {
        static std::mutex lock;
        static std::unordered_map<ObjectGuid::LowType, uint32> lastSentMs;

        std::lock_guard<std::mutex> guard(lock);
        uint32& last = lastSentMs[player->GetGUID().GetCounter()];
        if (last && getMSTimeDiff(last, nowMs) < 2000)
            return;
        last = nowMs;
    }

    player->GetSession()->SendNotification("Tournament characters cannot %s.", what);
}
}
