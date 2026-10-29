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

// .spawn creature|gameobject <entry> <map> <x> <y> <z> [o] [phasemask]
//
// A permanent spawn at a position the command names, so it works from the
// console and through SOAP - .npc add and .gobject add stand on the typing
// character, and a console has none (owner 2026-10-29: "please add the ability
// to add gameobjects/creatures through soap"). It is their path exactly: the
// object is created, saved (a spawn id of the server's own numbering, which
// Outdoor PvP's shutdown clean-up can never take for one of its own), and
// loaded back from that row onto the map, the grid loaded first. Open-world
// maps only, phase mask 1 unless given.
//
// A template added to the database while the realm runs is loaded first with
// .reload creature_template <entry> / .reload gameobject_template <entry>.
//
// Console commands run in World::Update after the map update has finished, so
// touching the map here is as safe as a GM's own command.

#include "Chat.h"
#include "ChatCommand.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "Map.h"
#include "MapManager.h"
#include "ObjectMgr.h"
#include "RBAC.h"
#include "ScriptMgr.h"
#include "StringFormat.h"

using namespace Trinity::ChatCommands;

namespace
{
    // The open-world map a spawn goes on, its grid loaded; nullptr (and the
    // reason said) when the map or the position will not take one.
    Map* SpawnMap(ChatHandler* handler, uint32 mapId, float x, float y, float z)
    {
        MapEntry const* mapEntry = sMapStore.LookupEntry(mapId);
        if (!mapEntry)
        {
            handler->SendSysMessage(Trinity::StringFormat("There is no map {}.", mapId).c_str());
            return nullptr;
        }
        if (mapEntry->Instanceable())
        {
            handler->SendSysMessage(Trinity::StringFormat("Map {} is an instance map: spawn there from inside it with .npc add / .gobject add.", mapId).c_str());
            return nullptr;
        }
        if (!MapManager::IsValidMapCoord(mapId, x, y, z))
        {
            handler->SendSysMessage(Trinity::StringFormat("({:.2f}, {:.2f}, {:.2f}) is not a position on map {}.", x, y, z, mapId).c_str());
            return nullptr;
        }
        Map* map = sMapMgr->CreateBaseMap(mapId);
        if (!map)
            return nullptr;
        map->LoadGrid(x, y);
        return map;
    }
}

class custom_spawn_commandscript : public CommandScript
{
public:
    custom_spawn_commandscript() : CommandScript("custom_spawn_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable spawnCommandTable =
        {
            { "creature",   HandleSpawnCreature,    rbac::RBAC_PERM_COMMAND_NPC_ADD,        Console::Yes },
            { "gameobject", HandleSpawnGameObject,  rbac::RBAC_PERM_COMMAND_GOBJECT_ADD,    Console::Yes },
        };
        static ChatCommandTable commandTable =
        {
            { "spawn", spawnCommandTable },
        };
        return commandTable;
    }

    static bool HandleSpawnCreature(ChatHandler* handler, uint32 entry, uint32 mapId, float x, float y, float z,
        Optional<float> orientation, Optional<uint32> phaseMask)
    {
        if (!sObjectMgr->GetCreatureTemplate(entry))
        {
            handler->SendSysMessage(Trinity::StringFormat("No creature template {} loaded - a new one needs .reload creature_template {} first.", entry, entry).c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        Map* map = SpawnMap(handler, mapId, x, y, z);
        if (!map)
        {
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint32 const phase = phaseMask.value_or(PHASEMASK_NORMAL);
        Position const pos(x, y, z, orientation.value_or(0.0f));

        Creature* creature = new Creature();
        if (!creature->Create(map->GenerateLowGuid<HighGuid::Unit>(), map, phase, entry, pos))
        {
            delete creature;
            handler->SendSysMessage(Trinity::StringFormat("Creature {} could not be created there.", entry).c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        creature->SaveToDB(map->GetId(), (1 << map->GetSpawnMode()), phase);
        ObjectGuid::LowType const spawnId = creature->GetSpawnId();

        // A fresh instance loaded from the row, as .npc add does: the first one
        // still carries what Create set up.
        creature->CleanupsBeforeDelete();
        delete creature;
        creature = new Creature();
        if (!creature->LoadFromDB(spawnId, map, true, true))
        {
            delete creature;
            handler->SendSysMessage(Trinity::StringFormat("Creature {} saved as spawn {}, but it could not be placed now; it appears at the next restart.", entry, spawnId).c_str());
            return true;
        }
        sObjectMgr->AddCreatureToGrid(spawnId, sObjectMgr->GetCreatureData(spawnId));

        handler->SendSysMessage(Trinity::StringFormat("Creature {} ({}) spawned: guid {} on map {} at ({:.2f}, {:.2f}, {:.2f}, {:.2f}), phase mask {}.",
            entry, creature->GetName(), spawnId, mapId, x, y, z, pos.GetOrientation(), phase).c_str());
        return true;
    }

    static bool HandleSpawnGameObject(ChatHandler* handler, uint32 entry, uint32 mapId, float x, float y, float z,
        Optional<float> orientation, Optional<uint32> phaseMask)
    {
        GameObjectTemplate const* info = sObjectMgr->GetGameObjectTemplate(entry);
        if (!info)
        {
            handler->SendSysMessage(Trinity::StringFormat("No gameobject template {} loaded - a new one needs .reload gameobject_template {} first.", entry, entry).c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }
        if (info->displayId && !sGameObjectDisplayInfoStore.LookupEntry(info->displayId))
        {
            handler->SendSysMessage(Trinity::StringFormat("Gameobject {} has display {}, which GameObjectDisplayInfo.dbc does not have.", entry, info->displayId).c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        Map* map = SpawnMap(handler, mapId, x, y, z);
        if (!map)
        {
            handler->SetSentErrorMessage(true);
            return false;
        }

        uint32 const phase = phaseMask.value_or(PHASEMASK_NORMAL);
        Position const pos(x, y, z, orientation.value_or(0.0f));
        QuaternionData const rot = QuaternionData::fromEulerAnglesZYX(pos.GetOrientation(), 0.f, 0.f);

        GameObject* object = new GameObject();
        if (!object->Create(map->GenerateLowGuid<HighGuid::GameObject>(), entry, map, phase, pos, rot, 255, GO_STATE_READY))
        {
            delete object;
            handler->SendSysMessage(Trinity::StringFormat("Gameobject {} could not be created there.", entry).c_str());
            handler->SetSentErrorMessage(true);
            return false;
        }

        object->SaveToDB(map->GetId(), (1 << map->GetSpawnMode()), phase);
        ObjectGuid::LowType const spawnId = object->GetSpawnId();

        delete object;
        object = new GameObject();
        if (!object->LoadFromDB(spawnId, map, true))
        {
            delete object;
            handler->SendSysMessage(Trinity::StringFormat("Gameobject {} saved as spawn {}, but it could not be placed now; it appears at the next restart.", entry, spawnId).c_str());
            return true;
        }
        sObjectMgr->AddGameobjectToGrid(spawnId, sObjectMgr->GetGameObjectData(spawnId));

        handler->SendSysMessage(Trinity::StringFormat("Gameobject {} ({}) spawned: guid {} on map {} at ({:.2f}, {:.2f}, {:.2f}, {:.2f}), phase mask {}.",
            entry, info->name, spawnId, mapId, x, y, z, pos.GetOrientation(), phase).c_str());
        return true;
    }
};

void AddSC_custom_spawn_commands()
{
    new custom_spawn_commandscript();
}
