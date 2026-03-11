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

#include "ScriptMgr.h"
#include "Player.h"
#include "Group.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "DatabaseEnv.h"
#include "Chat.h"
#include "Log.h"
#include "StringFormat.h"
#include <string_view>

#include <unordered_map>

namespace
{
    uint32 constexpr StormwindCityZoneId = 1519;
    uint32 constexpr GurubashiArenaFallbackMap = 0;
    float constexpr GurubashiArenaFallbackX = -13204.609f;
    float constexpr GurubashiArenaFallbackY = 272.2056f;
    float constexpr GurubashiArenaFallbackZ = 21.858f;
    float constexpr GurubashiArenaFallbackO = 1.022f;

    void WhisperFromChromie(Player* player, std::string_view message)
    {
        if (!player)
            return;

        ObjectGuid chromieGuid = ObjectGuid::Create<HighGuid::Player>(1);
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER_FOREIGN, LANG_UNIVERSAL, chromieGuid, player->GetGUID(), message, 0, "Chromie");
        player->SendDirectMessage(&data);
    }

    struct ZoneAreaKey
    {
        uint32 zoneId = 0;
        uint32 areaId = 0;

        bool operator==(ZoneAreaKey const& other) const
        {
            return zoneId == other.zoneId && areaId == other.areaId;
        }
    };

    struct ZoneAreaKeyHash
    {
        std::size_t operator()(ZoneAreaKey const& key) const noexcept
        {
            uint64 combined = (static_cast<uint64>(key.zoneId) << 32) | static_cast<uint64>(key.areaId);
            return std::hash<uint64>{}(combined);
        }
    };
}

struct ZoneGroupRule
{
    uint32 zoneId = 0;
    uint32 areaId = 0;
    uint8 minMembers = 0;
    uint8 maxMembers = 0;
    uint32 tpMap = 0;
    float tpX = 0.0f;
    float tpY = 0.0f;
    float tpZ = 0.0f;
    float tpO = 0.0f;
};

class ZoneGroupRuleMgr
{
public:
    static ZoneGroupRuleMgr* Instance()
    {
        static ZoneGroupRuleMgr instance;
        return &instance;
    }

    void Load()
    {
        _rulesByZone.clear();
        _rulesByZoneArea.clear();

        TC_LOG_INFO("server.loading", "Loading custom zone group rules...");

        QueryResult result = WorldDatabase.Query("SELECT zone_id, area_id, min_members, max_members, teleport_map, teleport_x, teleport_y, teleport_z, teleport_o FROM custom_zone_group_rules");
        if (!result)
        {
            TC_LOG_INFO("server.loading", ">> Loaded 0 custom zone group rules. DB table is empty.");
            return;
        }

        uint32 count = 0;
        do
        {
            Field* fields = result->Fetch();

            ZoneGroupRule rule;
            rule.zoneId = fields[0].GetUInt32();
            rule.areaId = fields[1].GetUInt32();
            rule.minMembers = fields[2].GetUInt8();
            rule.maxMembers = fields[3].GetUInt8();
            rule.tpMap = fields[4].GetUInt32();
            rule.tpX = fields[5].GetFloat();
            rule.tpY = fields[6].GetFloat();
            rule.tpZ = fields[7].GetFloat();
            rule.tpO = fields[8].GetFloat();

            if (rule.areaId == 0)
                _rulesByZone[rule.zoneId] = rule;
            else
                _rulesByZoneArea[ZoneAreaKey{ rule.zoneId, rule.areaId }] = rule;

            ++count;
        }
        while (result->NextRow());

        TC_LOG_INFO("server.loading", ">> Loaded {} custom zone group rules.", count);
    }

    bool GetRule(uint32 zoneId, uint32 areaId, ZoneGroupRule& outRule) const
    {
        if (areaId != 0)
        {
            auto const areaIt = _rulesByZoneArea.find(ZoneAreaKey{ zoneId, areaId });
            if (areaIt != _rulesByZoneArea.end())
            {
                outRule = areaIt->second;
                return true;
            }
        }

        auto const zoneIt = _rulesByZone.find(zoneId);
        if (zoneIt != _rulesByZone.end())
        {
            outRule = zoneIt->second;
            return true;
        }

        return false;
    }

private:
    ZoneGroupRuleMgr() = default;

    std::unordered_map<uint32, ZoneGroupRule> _rulesByZone;
    std::unordered_map<ZoneAreaKey, ZoneGroupRule, ZoneAreaKeyHash> _rulesByZoneArea;
};

static void EnforceZoneRuleForPlayer(Player* player)
{
    if (!player)
        return;

    uint32 const zoneId = player->GetZoneId();
    uint32 const areaId = player->GetAreaId();

    ZoneGroupRule rule;
    if (!ZoneGroupRuleMgr::Instance()->GetRule(zoneId, areaId, rule))
        return;

    uint8 groupCount = 1;
    if (Group* group = player->GetGroup())
        groupCount = group->GetMembersCount();

    if (groupCount < rule.minMembers || groupCount > rule.maxMembers)
    {
        if (!player->IsBeingTeleported())
        {
            uint32 teleportMap = rule.tpMap;
            float teleportX = rule.tpX;
            float teleportY = rule.tpY;
            float teleportZ = rule.tpZ;
            float teleportO = rule.tpO;

            if (teleportMap == 0 && teleportX == 0.0f && teleportY == 0.0f && teleportZ == 0.0f && teleportO == 0.0f && rule.zoneId == StormwindCityZoneId)
            {
                teleportMap = GurubashiArenaFallbackMap;
                teleportX = GurubashiArenaFallbackX;
                teleportY = GurubashiArenaFallbackY;
                teleportZ = GurubashiArenaFallbackZ;
                teleportO = GurubashiArenaFallbackO;
            }

            player->TeleportTo(teleportMap, teleportX, teleportY, teleportZ, teleportO);

            std::string requirementText;
            if (rule.minMembers == rule.maxMembers)
            {
                requirementText = Trinity::StringFormat("This zone only allows groups of {} adventurer{}.",
                    rule.minMembers, rule.minMembers == 1 ? "" : "s");
            }
            else if (rule.minMembers == 0)
            {
                requirementText = Trinity::StringFormat("This zone only allows groups of up to {} adventurer{}.",
                    rule.maxMembers, rule.maxMembers == 1 ? "" : "s");
            }
            else if (rule.maxMembers == 0)
            {
                requirementText = Trinity::StringFormat("This zone only allows groups of at least {} adventurer{}.",
                    rule.minMembers, rule.minMembers == 1 ? "" : "s");
            }
            else
            {
                requirementText = Trinity::StringFormat("This zone only allows groups of {} to {} adventurers.",
                    rule.minMembers, rule.maxMembers);
            }

            std::string message = Trinity::StringFormat("Spacetime is fragile! {} I've sent you back to safety.", requirementText);

            WhisperFromChromie(player, message);
        }
    }
}

static void EnforceZoneRuleForGroup(Group* group)
{
    if (!group)
        return;

    for (GroupReference* reference = group->GetFirstMember(); reference; reference = reference->next())
        if (Player* member = reference->GetSource())
            EnforceZoneRuleForPlayer(member);
}

class zone_group_rules_world : public WorldScript
{
public:
    zone_group_rules_world() : WorldScript("zone_group_rules_world") { }

    void OnStartup() override
    {
        ZoneGroupRuleMgr::Instance()->Load();
    }
};

class zone_group_rules_player : public PlayerScript
{
public:
    zone_group_rules_player() : PlayerScript("zone_group_rules_player") { }

    void OnLogin(Player* player, bool /*firstLogin*/) override
    {
        EnforceZoneRuleForPlayer(player);
    }

    void OnUpdateZone(Player* player, uint32 /*newZone*/, uint32 /*newArea*/) override
    {
        EnforceZoneRuleForPlayer(player);
    }

    void OnMapChanged(Player* player) override
    {
        EnforceZoneRuleForPlayer(player);
    }
};

class zone_group_rules_group : public GroupScript
{
public:
    zone_group_rules_group() : GroupScript("zone_group_rules_group") { }

    void OnAddMember(Group* group, ObjectGuid guid) override
    {
        if (Player* player = ObjectAccessor::FindPlayer(guid))
            EnforceZoneRuleForPlayer(player);

        EnforceZoneRuleForGroup(group);
    }

    void OnInviteMember(Group* group, ObjectGuid guid) override
    {
        if (Player* player = ObjectAccessor::FindPlayer(guid))
            EnforceZoneRuleForPlayer(player);

        EnforceZoneRuleForGroup(group);
    }
};

void AddSC_custom_zone_group_rules()
{
    new zone_group_rules_world();
    new zone_group_rules_player();
    new zone_group_rules_group();
}
