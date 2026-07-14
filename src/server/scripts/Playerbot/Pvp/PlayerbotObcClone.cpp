/*
 * This file is part of the TrinityCore Project. See AUTHORS file for Copyright information
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 */

#include "PlayerbotObcClone.h"

#include "PlayerbotRandomBotParticipation.h"
#include "MotionMaster.h"
#include "Battleground.h"
#include "BattlegroundMgr.h"
#include "CharacterCache.h"
#include "Configuration/Config.h"
#include "DataStores/DBCStores.h"
#include "Duration.h"
#include "GameTime.h"
#include "Globals/ObjectAccessor.h"
#include "Item.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Player.h"
#include "SharedDefines.h"
#include "SpellAuras.h"
#include "World.h"
#include "WorldSession.h"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace
{
constexpr uint32 kBloodlustSpellId = 2825;
constexpr uint32 kBloodlustDurationMs = 60 * IN_MILLISECONDS;
constexpr uint32 kCloneTickThrottleMs = 1000;

struct ObcCloneConfig
{
    bool enabled = false;
};

struct ObcCloneRecord
{
    ObjectGuid cloneGuid;
    uint32 oppositeTeam = 0;
    uint32 battlegroundInstanceId = 0;
};

std::mutex g_ObcCloneLock;
ObcCloneConfig g_ObcCloneConfig;

// The sessions are deliberately not registered with World. They only provide
// the Player-facing socketless session interface and are owned here for exactly
// as long as their in-memory clone exists.
std::unordered_map<ObjectGuid, std::unique_ptr<WorldSession>> g_CloneSessions;
std::unordered_map<ObjectGuid, ObcCloneRecord> g_ClonesByHuman;
std::unordered_map<ObjectGuid, ObjectGuid> g_HumanByClone;

uint32 g_CloneTickAccumulatorMs = 0;

bool IsObcCloneFeatureConfigured()
{
    return g_ObcCloneConfig.enabled;
}

uint32 OppositeTeam(uint32 team)
{
    return team == ALLIANCE ? HORDE : ALLIANCE;
}

Battleground* GetHumanObcBattleground(Player* player)
{
    if (!player || !player->IsInWorld())
        return nullptr;

    WorldSession const* session = player->GetSession();
    if (!session || session->IsVirtualSession())
        return nullptr;

    if (playerbot::IsManagedRandomBot(player))
        return nullptr;

    Battleground* bg = player->GetBattleground();
    if (!bg || bg->GetTypeID(true) != BATTLEGROUND_OBC)
        return nullptr;

    if (bg->GetStatus() == STATUS_WAIT_LEAVE || bg->GetStatus() == STATUS_NONE)
        return nullptr;

    return bg;
}

std::string GenerateCloneInternalName()
{
    static std::atomic<uint32> counter{ 0 };

    for (int attempt = 0; attempt < 2000; ++attempt)
    {
        uint32 value = counter.fetch_add(1, std::memory_order_relaxed) ^ (GameTime::GetGameTimeMS() & 0xFFFFu);
        std::string name = "Obcm";
        for (int i = 0; i < 6; ++i)
        {
            name += static_cast<char>('a' + (value % 26));
            value /= 26;
        }

        if (!sCharacterCache->GetCharacterCacheByName(name))
            return name;
    }

    return std::string();
}

bool CopyEquipment(Player* clone, Player const* human)
{
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        Item const* sourceItem = human->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!sourceItem)
            continue;

        Item* clonedItem = sourceItem->CloneItem(sourceItem->GetCount(), clone);
        if (!clonedItem)
            return false;

        for (uint8 enchantSlot = 0; enchantSlot < MAX_ENCHANTMENT_SLOT; ++enchantSlot)
        {
            EnchantmentSlot const slotId = EnchantmentSlot(enchantSlot);
            uint32 const enchantmentId = sourceItem->GetEnchantmentId(slotId);
            if (enchantmentId)
                clonedItem->SetEnchantment(slotId, enchantmentId, sourceItem->GetEnchantmentDuration(slotId),
                    sourceItem->GetEnchantmentCharges(slotId));
        }

        clonedItem->SetUInt32Value(ITEM_FIELD_DURABILITY, sourceItem->GetUInt32Value(ITEM_FIELD_DURABILITY));
        uint16 const destination = (uint16(INVENTORY_SLOT_BAG_0) << 8) | slot;
        clone->EquipItem(destination, clonedItem, false);
    }

    clone->SetUInt32Value(PLAYER_AMMO_ID, human->GetUInt32Value(PLAYER_AMMO_ID));
    return true;
}

void CopySkills(Player* clone, Player const* human)
{
    for (uint16 offset = 0; offset < PLAYER_MAX_SKILLS * 3; ++offset)
        clone->SetUInt32Value(PLAYER_SKILL_INFO_1_1 + offset, human->GetUInt32Value(PLAYER_SKILL_INFO_1_1 + offset));
}

void CopySpellsTalentsAndGlyphs(Player* clone, Player* human)
{
    // Replace the creation-template spellbook with the human's live spellbook.
    // AddSpell's learning path is entirely in memory and also applies passive
    // talent auras and spell modifiers.
    clone->RemoveAllAuras();
    clone->GetSpellMap().clear();
    clone->SetSpecsCount(1);
    clone->SetActiveSpec(0);

    for (auto const& [spellId, playerSpell] : human->GetSpellMap())
    {
        if (playerSpell.state == PLAYERSPELL_REMOVED)
            continue;

        clone->AddSpell(spellId, playerSpell.active, true, playerSpell.dependent, playerSpell.disabled);
        if (!playerSpell.disabled && GetTalentSpellCost(spellId) > 0)
            clone->AddTalent(spellId, 0, true);
    }

    clone->SetFreeTalentPoints(human->GetFreeTalentPoints());
    for (uint8 slot = 0; slot < MAX_GLYPH_SLOT_INDEX; ++slot)
    {
        clone->SetGlyphSlot(slot, human->GetGlyphSlot(slot));
        clone->SetGlyph(slot, human->GetGlyph(slot));
    }
    clone->ApplyGlyphAuras();
}

bool CreateHunterPetMirror(Player* clone, Pet* sourcePet)
{
    if (!clone || !sourcePet || sourcePet->getPetType() != HUNTER_PET)
        return true;

    CreatureTemplate const* creatureTemplate = sourcePet->GetCreatureTemplate();
    if (!creatureTemplate)
        return false;

    Map* map = clone->FindMap();
    if (!map || !clone->IsInWorld())
        return false;

    PetStable& petStable = clone->GetOrInitPetStable();
    if (petStable.CurrentPet)
        return false;

    std::unique_ptr<Pet> pet = std::make_unique<Pet>(clone, HUNTER_PET);
    if (!pet->CreateBaseAtCreatureInfo(creatureTemplate, clone))
        return false;

    pet->SetCreatorGUID(clone->GetGUID());
    pet->SetFaction(clone->GetFaction());
    pet->SetCreatedBySpell(sourcePet->GetUInt32Value(UNIT_CREATED_BY_SPELL));
    pet->ReplaceAllUnitFlags(UNIT_FLAG_PLAYER_CONTROLLED);

    uint8 const petLevel = sourcePet->GetLevel();
    if (!pet->InitStatsForLevel(petLevel))
        return false;

    uint32 const petNumber = sObjectMgr->GeneratePetNumber();
    pet->GetCharmInfo()->SetPetNumber(petNumber, true);
    pet->GetCharmInfo()->InitPetActionBar();
    pet->SetName(sourcePet->GetName());
    pet->SetNativeDisplayId(sourcePet->GetNativeDisplayId());
    pet->SetDisplayId(sourcePet->GetDisplayId());
    pet->SetReactState(sourcePet->GetReactState());
    pet->SetPetNameTimestamp(sourcePet->GetUInt32Value(UNIT_FIELD_PET_NAME_TIMESTAMP));
    pet->SetPetExperience(sourcePet->GetUInt32Value(UNIT_FIELD_PETEXPERIENCE));
    pet->SetPetNextLevelExperience(sourcePet->GetUInt32Value(UNIT_FIELD_PETNEXTLEVELEXP));

    pet->RemoveAllAuras();
    pet->m_spells.clear();
    pet->m_autospells.clear();
    pet->m_teachspells = sourcePet->m_teachspells;
    pet->m_usedTalentCount = 0;
    for (auto const& [spellId, petSpell] : sourcePet->m_spells)
    {
        if (petSpell.state == PETSPELL_REMOVED)
            continue;

        pet->addSpell(spellId, petSpell.active, PETSPELL_NEW, petSpell.type);
    }

    for (uint8 slot = ACTION_BAR_INDEX_START; slot < ACTION_BAR_INDEX_END; ++slot)
    {
        UnitActionBarEntry const* sourceAction = sourcePet->GetCharmInfo()->GetActionBarEntry(slot);
        pet->GetCharmInfo()->SetActionBar(slot, sourceAction->GetAction(), sourceAction->GetType());
    }

    pet->CastPetAuras(false);
    pet->SetFullHealth();
    for (uint8 power = POWER_MANA; power < MAX_POWERS; ++power)
        if (pet->GetMaxPower(Powers(power)))
            pet->SetFullPower(Powers(power));
    pet->SetPower(POWER_HAPPINESS, sourcePet->GetPower(POWER_HAPPINESS));

    Pet* petRaw = pet.get();
    if (!map->AddToMap(petRaw->ToCreature()))
        return false;

    pet->FillPetInfo(&petStable.CurrentPet.emplace());
    clone->SetMinion(petRaw, true);
    pet.release();
    return true;
}

void SynchronizeHunterPetMirror(Player* human, Player* clone)
{
    if (!human || !clone || human->GetClass() != CLASS_HUNTER || clone->GetClass() != CLASS_HUNTER)
        return;

    Pet* sourcePet = human->GetPet();
    if (sourcePet && sourcePet->getPetType() != HUNTER_PET)
        sourcePet = nullptr;

    Pet* clonePet = clone->GetPet();
    if (clonePet && (!sourcePet || clonePet->GetEntry() != sourcePet->GetEntry()))
    {
        clone->RemovePet(clonePet, PET_SAVE_AS_DELETED);
        clonePet = nullptr;
    }

    if (!sourcePet || clonePet)
        return;

    CreateHunterPetMirror(clone, sourcePet);
}

void DestroyUnseatedClone(std::unique_ptr<WorldSession>& session, Player* clone)
{
    if (!clone)
        return;

    delete clone;
    session->SetPlayer(nullptr);
}

bool ProvisionCloneForHuman(Player* human, Battleground* bg)
{
    if (!human || !bg)
        return false;

    ObjectGuid const humanGuid = human->GetGUID();
    uint32 const oppositeTeam = OppositeTeam(human->GetBGTeam());
    std::string const internalName = GenerateCloneInternalName();
    if (internalName.empty())
        return false;

    uint8 const expansion = static_cast<uint8>(sWorld->getIntConfig(CONFIG_EXPANSION));
    auto session = std::make_unique<WorldSession>(0, "obc_in_memory_clone", nullptr, SEC_PLAYER, expansion, 0,
        Minutes(0), LOCALE_enUS, 0, false);
    session->SetTransientPlayerSession();

    Player* clone = new Player(session.get());

    CharacterCreateInfo createInfo;
    createInfo.SetName(internalName)
        .SetRace(human->GetRace())
        .SetClass(human->GetClass())
        .SetGender(human->GetNativeGender())
        .SetSkin(human->GetSkinId())
        .SetFace(human->GetFaceId())
        .SetHairStyle(human->GetHairStyleId())
        .SetHairColor(human->GetHairColorId())
        .SetFacialHair(human->GetFacialStyle())
        .SetOutfitId(0);

    ObjectGuid::LowType const cloneLowGuid = sObjectMgr->GetGenerator<HighGuid::Player>().Generate();
    // This mirrors an existing character's live appearance, which may include
    // barber-shop-only sections that are valid in game but unavailable during
    // initial character creation.
    if (!clone->Create(cloneLowGuid, &createInfo, false, false))
    {
        DestroyUnseatedClone(session, clone);
        return false;
    }

    // Player::Create initializes display IDs before it writes PLAYER_BYTES_3,
    // which is where Player::GetNativeGender reads from. Persistent characters
    // get another display initialization during loading, but these transient
    // clones do not, so explicitly synchronize gender and rebuild the native
    // model after creation.
    Gender const nativeGender = human->GetNativeGender();
    clone->SetGender(nativeGender);
    clone->SetNativeGender(nativeGender);
    clone->InitDisplayIds();

    // WorldSession::SetPlayer reads the player's GUID immediately. Player's
    // update-field storage and GUID do not exist until Player::Create succeeds.
    session->SetPlayer(clone);

    clone->GetMotionMaster()->Initialize();
    clone->SetLevel(human->GetLevel(), false);
    clone->InitStatsForLevel();
    clone->InitTalentForLevel();
    clone->InitGlyphsForLevel();
    CopySkills(clone, human);
    CopySpellsTalentsAndGlyphs(clone, human);

    if (!CopyEquipment(clone, human))
    {
        DestroyUnseatedClone(session, clone);
        return false;
    }

    clone->UpdateAllStats();
    clone->SetFullHealth();
    for (uint8 power = POWER_MANA; power < MAX_POWERS; ++power)
        clone->SetFullPower(Powers(power));

    BattlegroundQueueTypeId const queueTypeId = BattlegroundMgr::BGQueueTypeId(BATTLEGROUND_OBC, 0);
    Position const* start = bg->GetTeamStartPosition(Battleground::GetTeamIndexByTeamId(oppositeTeam));
    if (queueTypeId == BATTLEGROUND_QUEUE_NONE || !start ||
        clone->AddBattlegroundQueueId(queueTypeId) >= PLAYER_MAX_BATTLEGROUND_QUEUES)
    {
        DestroyUnseatedClone(session, clone);
        return false;
    }

    clone->SetInviteForBattlegroundQueueType(queueTypeId, bg->GetInstanceID());
    clone->SetBattlegroundId(bg->GetInstanceID(), BATTLEGROUND_OBC);
    clone->SetBGTeam(oppositeTeam);
    clone->ResetMap();
    clone->Relocate(*start);
    clone->SetMap(bg->GetBgMap());

    bg->IncreaseInvitedCount(oppositeTeam);
    if (!bg->GetBgMap()->AddPlayerToMap(clone))
    {
        bg->DecreaseInvitedCount(oppositeTeam);
        DestroyUnseatedClone(session, clone);
        return false;
    }

    ObjectGuid const cloneGuid = clone->GetGUID();
    std::string const displayName = "Dark " + human->GetName();
    sCharacterCache->AddCharacterCacheEntry(cloneGuid, 0, displayName, clone->GetNativeGender(), clone->GetRace(),
        clone->GetClass(), clone->GetLevel());
    ObjectAccessor::AddObject(clone);
    bg->AddPlayer(clone);
    clone->SetInGameTime(GameTime::GetGameTimeMS());
    SynchronizeHunterPetMirror(human, clone);

    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        g_CloneSessions.emplace(cloneGuid, std::move(session));
        g_ClonesByHuman[humanGuid] = { cloneGuid, oppositeTeam, bg->GetInstanceID() };
        g_HumanByClone[cloneGuid] = humanGuid;
    }

    TC_LOG_INFO("playerbots.population", "OBC clone: created in-memory '{}' ({}) for human {} in bg instance {}.",
        displayName, cloneGuid.ToString(), humanGuid.ToString(), bg->GetInstanceID());
    return true;
}

void TeardownCloneForHuman(ObjectGuid humanGuid)
{
    ObcCloneRecord record;
    std::unique_ptr<WorldSession> session;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        auto itr = g_ClonesByHuman.find(humanGuid);
        if (itr == g_ClonesByHuman.end())
            return;

        record = itr->second;
        g_ClonesByHuman.erase(itr);
        g_HumanByClone.erase(record.cloneGuid);

        auto sessionItr = g_CloneSessions.find(record.cloneGuid);
        if (sessionItr != g_CloneSessions.end())
        {
            session = std::move(sessionItr->second);
            g_CloneSessions.erase(sessionItr);
        }
    }

    Player* clone = session ? session->GetPlayer() : nullptr;
    if (clone)
    {
        if (Battleground* bg = sBattlegroundMgr->GetBattleground(record.battlegroundInstanceId, BATTLEGROUND_OBC))
            if (bg->IsPlayerInBattleground(record.cloneGuid))
                bg->RemovePlayerAtLeave(record.cloneGuid, false, false);

        std::string cachedName;
        sCharacterCache->GetCharacterNameByGuid(record.cloneGuid, cachedName);

        if (Map* map = clone->FindMap())
            map->RemovePlayerFromMap(clone, true);
        else
        {
            ObjectAccessor::RemoveObject(clone);
            delete clone;
        }
        session->SetPlayer(nullptr);

        if (!cachedName.empty())
            sCharacterCache->DeleteCharacterCacheEntry(record.cloneGuid, cachedName);
    }

    TC_LOG_INFO("playerbots.population", "OBC clone: destroyed in-memory clone {} for human {}.",
        record.cloneGuid.ToString(), humanGuid.ToString());
}

void TeardownAllClones()
{
    std::vector<ObjectGuid> humanGuids;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        humanGuids.reserve(g_ClonesByHuman.size());
        for (auto const& [humanGuid, record] : g_ClonesByHuman)
            humanGuids.push_back(humanGuid);
    }

    for (ObjectGuid const& humanGuid : humanGuids)
        TeardownCloneForHuman(humanGuid);
}
}

namespace playerbot
{
void PlayerbotObcCloneManager::LoadConfig()
{
    int32 const legacyEnableValue = std::max<int32>(0,
        sConfigMgr->GetIntDefault("Playerbot.PvpLifecycle.ObsidianColosseum.CloneAccountId", 0));

    std::lock_guard<std::mutex> lock(g_ObcCloneLock);
    g_ObcCloneConfig.enabled = sConfigMgr->GetBoolDefault(
        "Playerbot.PvpLifecycle.ObsidianColosseum.Clone.Enable", legacyEnableValue != 0);

    TC_LOG_INFO("server.loading", "OBC in-memory clone mirror config: enabled={}.",
        g_ObcCloneConfig.enabled ? "true" : "false");
}

void PlayerbotObcCloneManager::OnStartupSweep()
{
    // In-memory clones cannot survive a process restart and have no persistent
    // rows to sweep.
}

void PlayerbotObcCloneManager::OnShutdown()
{
    TeardownAllClones();
}

void PlayerbotObcCloneManager::OnWorldUpdate(uint32 diffMs)
{
    if (!IsObcCloneFeatureConfigured())
    {
        TeardownAllClones();
        return;
    }

    g_CloneTickAccumulatorMs += diffMs;
    if (g_CloneTickAccumulatorMs < kCloneTickThrottleMs)
        return;
    g_CloneTickAccumulatorMs = 0;

    std::vector<ObjectGuid> trackedHumans;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        trackedHumans.reserve(g_ClonesByHuman.size());
        for (auto const& [humanGuid, record] : g_ClonesByHuman)
            trackedHumans.push_back(humanGuid);
    }

    for (ObjectGuid const& humanGuid : trackedHumans)
    {
        Player* human = ObjectAccessor::FindConnectedPlayer(humanGuid);
        Battleground* bg = human ? GetHumanObcBattleground(human) : nullptr;
        if (!bg)
        {
            TeardownCloneForHuman(humanGuid);
            continue;
        }

        ObcCloneRecord record;
        bool foundRecord = false;
        {
            std::lock_guard<std::mutex> lock(g_ObcCloneLock);
            auto itr = g_ClonesByHuman.find(humanGuid);
            if (itr != g_ClonesByHuman.end())
            {
                record = itr->second;
                foundRecord = true;
            }
        }

        if (!foundRecord)
            continue;

        Player* clone = ObjectAccessor::FindConnectedPlayer(record.cloneGuid);
        uint32 const expectedCloneTeam = OppositeTeam(human->GetBGTeam());
        bool const wrongInstance = record.battlegroundInstanceId != bg->GetInstanceID();
        bool const wrongTeam = record.oppositeTeam != expectedCloneTeam;
        bool const missingClone = !clone || !clone->IsInWorld();
        bool const wrongMap = clone && clone->FindMap() != bg->GetBgMap();
        bool const missingRosterEntry = !bg->IsPlayerInBattleground(record.cloneGuid);
        bool const wrongRosterTeam = !missingRosterEntry && bg->GetPlayerTeam(record.cloneGuid) != record.oppositeTeam;
        bool const wrongCloneTeam = clone && clone->GetBGTeam() != record.oppositeTeam;

        if (wrongInstance || wrongTeam || missingClone || wrongMap || missingRosterEntry || wrongRosterTeam || wrongCloneTeam)
        {
            TC_LOG_WARN("playerbots.population",
                "OBC clone: rebuilding invalid mirror for human {} (clone={}, instance={}/{}, team={}/{}, missingClone={}, wrongMap={}, rosterPresent={}, rosterTeamOk={}, cloneTeamOk={}).",
                humanGuid.ToString(), record.cloneGuid.ToString(), record.battlegroundInstanceId, bg->GetInstanceID(),
                record.oppositeTeam, expectedCloneTeam, missingClone ? 1 : 0, wrongMap ? 1 : 0,
                missingRosterEntry ? 0 : 1, wrongRosterTeam ? 0 : 1, wrongCloneTeam ? 0 : 1);
            TeardownCloneForHuman(humanGuid);
        }
        else
            SynchronizeHunterPetMirror(human, clone);
    }

    std::vector<ObjectGuid> obcHumans;
    {
        std::shared_lock<std::shared_mutex> lock(*HashMapHolder<Player>::GetLock());
        for (auto const& [guid, player] : ObjectAccessor::GetPlayers())
            if (GetHumanObcBattleground(player))
                obcHumans.push_back(guid);
    }

    for (ObjectGuid const& humanGuid : obcHumans)
    {
        {
            std::lock_guard<std::mutex> lock(g_ObcCloneLock);
            if (g_ClonesByHuman.find(humanGuid) != g_ClonesByHuman.end())
                continue;
        }

        Player* human = ObjectAccessor::FindConnectedPlayer(humanGuid);
        Battleground* bg = human ? GetHumanObcBattleground(human) : nullptr;
        if (human && bg)
            ProvisionCloneForHuman(human, bg);
    }
}

void PlayerbotObcCloneManager::OnPvpKill(Player* killer, Player* killed)
{
    if (!killer || !killed || killer == killed)
        return;

    bool areCounterparts = false;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);

        auto humanItr = g_ClonesByHuman.find(killer->GetGUID());
        if (humanItr != g_ClonesByHuman.end() && humanItr->second.cloneGuid == killed->GetGUID())
            areCounterparts = true;

        if (!areCounterparts)
        {
            auto cloneItr = g_HumanByClone.find(killer->GetGUID());
            if (cloneItr != g_HumanByClone.end() && cloneItr->second == killed->GetGUID())
                areCounterparts = true;
        }
    }

    if (areCounterparts)
        if (Aura* aura = killer->AddAura(kBloodlustSpellId, killer))
        {
            aura->SetMaxDuration(kBloodlustDurationMs);
            aura->SetDuration(kBloodlustDurationMs);
        }
}

void PlayerbotObcCloneManager::OnPlayerLogout(Player const* player)
{
    if (!player)
        return;

    bool hasClone = false;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        hasClone = g_ClonesByHuman.find(player->GetGUID()) != g_ClonesByHuman.end();
    }

    if (hasClone)
        TeardownCloneForHuman(player->GetGUID());
}

bool PlayerbotObcCloneManager::IsActiveClone(Player const* player)
{
    if (!player)
        return false;

    // The transient-session marker is assigned before the clone is inserted
    // into the manager maps and remains set while teardown detaches it. It
    // closes both small windows where generic virtual-bot lifecycle code could
    // otherwise mistake the clone for a persistent managed bot.
    if (WorldSession const* session = player->GetSession(); session && session->IsTransientPlayerSession())
        return true;

    std::lock_guard<std::mutex> lock(g_ObcCloneLock);
    return g_HumanByClone.find(player->GetGUID()) != g_HumanByClone.end();
}
}
