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
#include "LoginQueryHolder.h"
#include "Log.h"
#include "Map.h"
#include "MapManager.h"
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

struct CustomGameCloneRecord
{
    ObjectGuid cloneGuid;
    ObjectGuid sourceGuid;
    uint32 team = 0;
    uint32 battlegroundInstanceId = 0;
    BattlegroundTypeId battlegroundType = BATTLEGROUND_TYPE_NONE;
};

struct CustomGameLobbyCloneRecord
{
    ObjectGuid cloneGuid;
    ObjectGuid sourceGuid;
    uint32 mapId = 0;
    uint32 lobbyInstanceId = 0;
    uint32 rosterSlotId = 0;
    uint32 team = 0;
    bool isPlayerbot = false;
};

struct PendingCustomGameLobbyClone
{
    ObjectGuid sourceGuid;
    uint32 mapId = 0;
    uint32 lobbyInstanceId = 0;
    uint32 rosterSlotId = 0;
    uint32 team = 0;
    bool isPlayerbot = false;
    Position position;
    std::string displayPrefix;
};

std::mutex g_ObcCloneLock;
ObcCloneConfig g_ObcCloneConfig;

// The sessions are deliberately not registered with World. They only provide
// the Player-facing socketless session interface and are owned here for exactly
// as long as their in-memory clone exists.
std::unordered_map<ObjectGuid, std::unique_ptr<WorldSession>> g_CloneSessions;
std::unordered_map<ObjectGuid, ObcCloneRecord> g_ClonesByHuman;
std::unordered_map<ObjectGuid, ObjectGuid> g_HumanByClone;
std::unordered_map<ObjectGuid, CustomGameCloneRecord> g_CustomGameClones;
std::unordered_map<ObjectGuid, CustomGameLobbyCloneRecord> g_CustomGameLobbyClones;
std::vector<PendingCustomGameLobbyClone> g_PendingCustomGameLobbyClones;

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
    if (!bg || bg->GetTypeID(true) != BATTLEGROUND_OBC || bg->IsCustomGame())
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

std::unordered_map<ObjectGuid, uint32> g_PendingHunterPetSpellQueryByClone;

// Applies the source's trained pet_spell rows (fetched asynchronously, since
// the CHAR_SEL_PET_SPELL statement is async-connection-only) to the clone's
// hunter pet once the query resolves. Re-validates that the clone still has
// the same pet this query was started for -- the periodic mirror sync could
// have replaced or removed it in the meantime (e.g. the source's real pet
// went live and CreateHunterPetMirror took over).
void ApplyLoadedHunterPetSpells(ObjectGuid cloneGuid, uint32 expectedClonePetNumber, PreparedQueryResult result)
{
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        auto itr = g_PendingHunterPetSpellQueryByClone.find(cloneGuid);
        if (itr != g_PendingHunterPetSpellQueryByClone.end() && itr->second == expectedClonePetNumber)
            g_PendingHunterPetSpellQueryByClone.erase(itr);
    }

    if (!result)
        return;

    Player* clone = ObjectAccessor::FindConnectedPlayer(cloneGuid);
    if (!clone || !clone->IsInWorld())
        return;

    Pet* pet = clone->GetPet();
    if (!pet || pet->getPetType() != HUNTER_PET || !pet->GetCharmInfo() ||
        pet->GetCharmInfo()->GetPetNumber() != expectedClonePetNumber)
        return;

    do
    {
        Field* fields = result->Fetch();
        pet->addSpell(fields[0].GetUInt32(), ActiveStates(fields[1].GetUInt8()), PETSPELL_UNCHANGED);
    }
    while (result->NextRow());

    // InitPetCreateSpells() already set up the baseline native action bar;
    // rebuild it now that the taught abilities are also known, so trained
    // spells actually show up as usable rather than just being in memory.
    pet->GetCharmInfo()->InitPetActionBar();
    pet->CastPetAuras(false);
}

// Builds a working hunter pet for the clone directly from the source's
// persisted pet stable data, without requiring the source to have a live,
// currently-summoned Pet at all. An online player's PetStable stays populated
// (their last-known current/unslotted pet) regardless of whether the pet is
// actually summoned right now, so this covers the common case where the
// source simply hasn't called their pet out yet this match.
//
// Trained pet_spell rows (Beast Training) are not part of PetStable::PetInfo
// and are loaded afterward via an async query keyed on the source's pet
// number (see ApplyLoadedHunterPetSpells); this function gives the mirror
// baseline native family abilities immediately (via Pet::InitPetCreateSpells)
// so it is not left with zero attacks while that query is in flight.
bool CreateHunterPetMirrorFromStable(Player* clone, PetStable::PetInfo const& info, uint32& outClonePetNumber)
{
    outClonePetNumber = 0;
    if (!clone || info.Type != HUNTER_PET || !info.CreatureId)
        return true;

    CreatureTemplate const* creatureTemplate = sObjectMgr->GetCreatureTemplate(info.CreatureId);
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
    pet->SetCreatedBySpell(info.CreatedBySpellId);
    pet->ReplaceAllUnitFlags(UNIT_FLAG_PLAYER_CONTROLLED);

    uint8 const petLevel = info.Level > 0 ? info.Level : uint8(clone->GetLevel());
    if (!pet->InitStatsForLevel(petLevel))
        return false;

    uint32 const petNumber = sObjectMgr->GeneratePetNumber();
    outClonePetNumber = petNumber;
    pet->GetCharmInfo()->SetPetNumber(petNumber, true);
    if (!info.Name.empty())
        pet->SetName(info.Name);
    if (info.DisplayId)
    {
        pet->SetNativeDisplayId(info.DisplayId);
        pet->SetDisplayId(info.DisplayId);
    }
    if (info.ReactState)
        pet->SetReactState(info.ReactState);

    pet->InitPetCreateSpells();

    pet->CastPetAuras(false);
    pet->SetFullHealth();
    for (uint8 power = POWER_MANA; power < MAX_POWERS; ++power)
        if (pet->GetMaxPower(Powers(power)))
            pet->SetFullPower(Powers(power));
    pet->SetPower(POWER_HAPPINESS, 166500);

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

    if (clonePet)
        return;

    if (sourcePet)
    {
        CreateHunterPetMirror(clone, sourcePet);
        return;
    }

    // The source has no pet summoned right now. Fall back to whatever their
    // persisted pet stable still has on record (current or unslotted hunter
    // pet) so the clone is not permanently petless just because the source
    // hasn't called theirs out yet.
    PetStable const* stable = human->GetPetStable();
    if (!stable)
        return;

    std::pair<PetStable::PetInfo const*, PetSaveMode> const loadInfo = Pet::GetLoadPetInfo(*stable, 0, 0, false);
    PetStable::PetInfo const* info = loadInfo.first;
    if (!info || info->Type != HUNTER_PET || info->Health == 0)
        return;

    uint32 const sourcePetNumber = info->PetNumber;
    uint32 clonePetNumber = 0;
    if (!CreateHunterPetMirrorFromStable(clone, *info, clonePetNumber) || !clonePetNumber)
        return;

    // Fetch the source's trained pet_spell rows (Beast Training) so the
    // mirror ends up with the abilities the hunter actually taught it, not
    // just baseline native attacks. CHAR_SEL_PET_SPELL is async-connection
    // only, so this cannot be a blocking Query() call.
    ObjectGuid const cloneGuid = clone->GetGUID();
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        auto& pending = g_PendingHunterPetSpellQueryByClone[cloneGuid];
        if (pending == clonePetNumber)
            return;
        pending = clonePetNumber;
    }

    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_PET_SPELL);
    stmt->setUInt32(0, sourcePetNumber);
    if (WorldSession* session = human->GetSession())
    {
        session->GetQueryProcessor().AddCallback(CharacterDatabase.AsyncQuery(stmt).WithPreparedCallback(
            [cloneGuid, clonePetNumber](PreparedQueryResult result)
            {
                ApplyLoadedHunterPetSpells(cloneGuid, clonePetNumber, result);
            }));
    }
}

void DestroyUnseatedClone(std::unique_ptr<WorldSession>& session, Player* clone)
{
    if (!clone)
        return;

    // Offline clone sources are populated through Player::LoadFromDB, which
    // restores persistent auras and may establish cross-unit references even
    // though the temporary Player is never added to the world. Run the same
    // pre-destruction cleanup used by normal logout/map removal before the
    // Unit destructor asserts that every applied aura has been detached.
    if (clone->GetGUID())
        clone->CleanupsBeforeDelete();

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
    clone->SetFactionForRace(oppositeTeam == HORDE ? RACE_BLOODELF : RACE_HUMAN);
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

void TeardownCustomGameClone(ObjectGuid cloneGuid)
{
    CustomGameCloneRecord record;
    std::unique_ptr<WorldSession> session;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        auto recordItr = g_CustomGameClones.find(cloneGuid);
        if (recordItr == g_CustomGameClones.end())
            return;

        record = recordItr->second;
        g_CustomGameClones.erase(recordItr);

        auto sessionItr = g_CloneSessions.find(cloneGuid);
        if (sessionItr != g_CloneSessions.end())
        {
            session = std::move(sessionItr->second);
            g_CloneSessions.erase(sessionItr);
        }
    }

    Player* clone = session ? session->GetPlayer() : nullptr;
    if (!clone)
        return;

    playerbot::RandomBotParticipationManager::OnPlayerLogout(clone);

    if (Battleground* bg = sBattlegroundMgr->GetBattleground(record.battlegroundInstanceId, record.battlegroundType))
        if (bg->IsPlayerInBattleground(cloneGuid))
            bg->RemovePlayerAtLeave(cloneGuid, false, false);

    std::string cachedName;
    sCharacterCache->GetCharacterNameByGuid(cloneGuid, cachedName);
    if (Map* map = clone->FindMap())
        map->RemovePlayerFromMap(clone, true);
    else
    {
        ObjectAccessor::RemoveObject(clone);
        delete clone;
    }
    session->SetPlayer(nullptr);

    if (!cachedName.empty())
        sCharacterCache->DeleteCharacterCacheEntry(cloneGuid, cachedName);
}

void TeardownAllCustomGameClones()
{
    std::vector<ObjectGuid> cloneGuids;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        cloneGuids.reserve(g_CustomGameClones.size());
        for (auto const& [cloneGuid, record] : g_CustomGameClones)
            cloneGuids.push_back(cloneGuid);
    }

    for (ObjectGuid cloneGuid : cloneGuids)
        TeardownCustomGameClone(cloneGuid);
}

bool MatchesLobbyClone(CustomGameLobbyCloneRecord const& record, uint32 lobbyInstanceId, uint32 rosterSlotId)
{
    return record.lobbyInstanceId == lobbyInstanceId && record.rosterSlotId == rosterSlotId;
}

bool MatchesLobbyClone(PendingCustomGameLobbyClone const& record, uint32 lobbyInstanceId, uint32 rosterSlotId)
{
    return record.lobbyInstanceId == lobbyInstanceId && record.rosterSlotId == rosterSlotId;
}

bool HasCustomGameLobbyClone(uint32 lobbyInstanceId, uint32 rosterSlotId)
{
    std::lock_guard<std::mutex> lock(g_ObcCloneLock);
    for (auto const& [cloneGuid, record] : g_CustomGameLobbyClones)
        if (MatchesLobbyClone(record, lobbyInstanceId, rosterSlotId))
            return true;

    return std::any_of(g_PendingCustomGameLobbyClones.begin(), g_PendingCustomGameLobbyClones.end(),
        [&](PendingCustomGameLobbyClone const& record)
        {
            return MatchesLobbyClone(record, lobbyInstanceId, rosterSlotId);
        });
}

Player* CreateCustomGameLobbyClone(Player* source, uint32 mapId, uint32 lobbyInstanceId, uint32 team,
    uint32 rosterSlotId, bool isPlayerbot, Position const& position, std::string const& displayPrefix)
{
    if (!source || (team != ALLIANCE && team != HORDE))
        return nullptr;

    Map* lobbyMap = sMapMgr->FindMap(mapId, lobbyInstanceId);
    if (!lobbyMap || HasCustomGameLobbyClone(lobbyInstanceId, rosterSlotId))
        return nullptr;

    std::string const internalName = GenerateCloneInternalName();
    if (internalName.empty())
        return nullptr;

    uint8 const expansion = static_cast<uint8>(sWorld->getIntConfig(CONFIG_EXPANSION));
    auto session = std::make_unique<WorldSession>(0, "custom_game_lobby_preview", nullptr, SEC_PLAYER, expansion, 0,
        Minutes(0), LOCALE_enUS, 0, false);
    session->SetTransientPlayerSession();

    Player* clone = new Player(session.get());
    CharacterCreateInfo createInfo;
    createInfo.SetName(internalName)
        .SetRace(source->GetRace()).SetClass(source->GetClass()).SetGender(source->GetNativeGender())
        .SetSkin(source->GetSkinId()).SetFace(source->GetFaceId()).SetHairStyle(source->GetHairStyleId())
        .SetHairColor(source->GetHairColorId()).SetFacialHair(source->GetFacialStyle()).SetOutfitId(0);

    ObjectGuid::LowType const cloneLowGuid = sObjectMgr->GetGenerator<HighGuid::Player>().Generate();
    if (!clone->Create(cloneLowGuid, &createInfo, false, false))
    {
        DestroyUnseatedClone(session, clone);
        return nullptr;
    }

    clone->SetGender(source->GetNativeGender());
    clone->SetNativeGender(source->GetNativeGender());
    clone->InitDisplayIds();
    session->SetPlayer(clone);
    clone->GetMotionMaster()->Initialize();
    clone->SetLevel(source->GetLevel(), false);
    clone->InitStatsForLevel();
    clone->InitTalentForLevel();
    clone->InitGlyphsForLevel();
    CopySkills(clone, source);
    CopySpellsTalentsAndGlyphs(clone, source);
    if (!CopyEquipment(clone, source))
    {
        DestroyUnseatedClone(session, clone);
        return nullptr;
    }

    clone->UpdateAllStats();
    clone->SetFullHealth();
    for (uint8 power = POWER_MANA; power < MAX_POWERS; ++power)
        clone->SetFullPower(Powers(power));

    clone->SetBGTeam(team);
    // Lobby previews are roster mannequins: players must be able to target them,
    // see their names, and inspect their copied gear/talents.  A friendly faction
    // exposes the normal player interaction menu regardless of the source race;
    // the team flag remains the visual indication of the assigned custom team.
    clone->SetFaction(FACTION_FRIENDLY);
    clone->SetUnitFlag(UNIT_FLAG_PACIFIED | UNIT_FLAG_IMMUNE_TO_PC | UNIT_FLAG_IMMUNE_TO_NPC);
    clone->RemoveUnitFlag(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_ATTACKABLE_1 |
        UNIT_FLAG_NON_ATTACKABLE_2 | UNIT_FLAG_UNINTERACTIBLE);
    clone->ClearUnitState(UNIT_STATE_UNATTACKABLE);
    clone->AddUnitState(UNIT_STATE_ROOT);
    clone->SetWorldSubMap(mapId, lobbyInstanceId);
    clone->ResetMap();
    clone->Relocate(position);
    clone->SetMap(lobbyMap);
    if (!lobbyMap->AddPlayerToMap(clone))
    {
        DestroyUnseatedClone(session, clone);
        return nullptr;
    }

    ObjectGuid const cloneGuid = clone->GetGUID();
    std::string displayName = displayPrefix + source->GetName();
    sCharacterCache->AddCharacterCacheEntry(cloneGuid, 0, displayName, clone->GetNativeGender(), clone->GetRace(),
        clone->GetClass(), clone->GetLevel(), false);
    ObjectAccessor::AddObject(clone);
    clone->SetInGameTime(GameTime::GetGameTimeMS());
    clone->CastSpell(clone, team == ALLIANCE ? 32609 : 32610, true);
    // Reassert this after the flag aura is applied as well. Any aura-side
    // targeting state must not turn the roster mannequin into scenery.
    clone->RemoveUnitFlag(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_NOT_ATTACKABLE_1 |
        UNIT_FLAG_NON_ATTACKABLE_2 | UNIT_FLAG_UNINTERACTIBLE);
    clone->ClearUnitState(UNIT_STATE_UNATTACKABLE);

    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        g_CloneSessions.emplace(cloneGuid, std::move(session));
        g_CustomGameLobbyClones.emplace(cloneGuid, CustomGameLobbyCloneRecord{
            cloneGuid, source->GetGUID(), mapId, lobbyInstanceId, rosterSlotId, team, isPlayerbot });
    }

    return clone;
}

void TeardownCustomGameLobbyClone(ObjectGuid cloneGuid)
{
    std::unique_ptr<WorldSession> session;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        auto recordItr = g_CustomGameLobbyClones.find(cloneGuid);
        if (recordItr == g_CustomGameLobbyClones.end())
            return;
        g_CustomGameLobbyClones.erase(recordItr);

        auto sessionItr = g_CloneSessions.find(cloneGuid);
        if (sessionItr != g_CloneSessions.end())
        {
            session = std::move(sessionItr->second);
            g_CloneSessions.erase(sessionItr);
        }
    }

    Player* clone = session ? session->GetPlayer() : nullptr;
    if (!clone)
        return;

    std::string cachedName;
    sCharacterCache->GetCharacterNameByGuid(cloneGuid, cachedName);
    if (Map* map = clone->FindMap())
        map->RemovePlayerFromMap(clone, true);
    else
    {
        ObjectAccessor::RemoveObject(clone);
        delete clone;
    }
    session->SetPlayer(nullptr);

    if (!cachedName.empty())
        sCharacterCache->DeleteCharacterCacheEntry(cloneGuid, cachedName);
}

void TeardownAllCustomGameLobbyClones()
{
    std::vector<ObjectGuid> cloneGuids;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        cloneGuids.reserve(g_CustomGameLobbyClones.size());
        for (auto const& [cloneGuid, record] : g_CustomGameLobbyClones)
            cloneGuids.push_back(cloneGuid);
        g_PendingCustomGameLobbyClones.clear();
    }

    for (ObjectGuid cloneGuid : cloneGuids)
        TeardownCustomGameLobbyClone(cloneGuid);
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
    TeardownAllCustomGameLobbyClones();
    TeardownAllCustomGameClones();
    TeardownAllClones();
}

void PlayerbotObcCloneManager::OnWorldUpdate(uint32 diffMs)
{
    std::vector<ObjectGuid> expiredLobbyClones;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        for (auto const& [cloneGuid, record] : g_CustomGameLobbyClones)
            if (!sMapMgr->FindMap(record.mapId, record.lobbyInstanceId))
                expiredLobbyClones.push_back(cloneGuid);
    }
    for (ObjectGuid cloneGuid : expiredLobbyClones)
        TeardownCustomGameLobbyClone(cloneGuid);

    std::vector<ObjectGuid> expiredCustomClones;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        for (auto const& [cloneGuid, record] : g_CustomGameClones)
        {
            Battleground* bg = sBattlegroundMgr->GetBattleground(record.battlegroundInstanceId, record.battlegroundType);
            if (!bg || bg->GetStatus() == STATUS_NONE || bg->GetStatus() == STATUS_WAIT_LEAVE)
                expiredCustomClones.push_back(cloneGuid);
        }
    }
    for (ObjectGuid cloneGuid : expiredCustomClones)
        TeardownCustomGameClone(cloneGuid);

    // Custom-game clones (see CreateCustomGameClone) only ran the hunter pet
    // mirror once, at clone creation. If the source hunter's pet was not
    // actively summoned at that exact instant (dismissed, still logging in,
    // etc.) the clone was petless for the entire match with no way to catch
    // up. This must run unconditionally here, alongside the custom-game
    // teardown logic above and before the IsObcCloneFeatureConfigured() gate
    // below -- that config flag only controls the dedicated OBC auto-clone
    // queue feature and has nothing to do with custom-game clones, which are
    // driven entirely by custom_game_lobby.cpp. Gating this on that flag (as
    // an earlier version of this fix did) made it dead code for any server
    // running custom games without the OBC auto-queue feature also enabled.
    std::vector<CustomGameCloneRecord> customGameCloneRecords;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        customGameCloneRecords.reserve(g_CustomGameClones.size());
        for (auto const& [cloneGuid, record] : g_CustomGameClones)
            customGameCloneRecords.push_back(record);
    }
    for (CustomGameCloneRecord const& record : customGameCloneRecords)
    {
        Player* source = ObjectAccessor::FindConnectedPlayer(record.sourceGuid);
        Player* clone = ObjectAccessor::FindConnectedPlayer(record.cloneGuid);
        if (source && clone && clone->IsInWorld())
            SynchronizeHunterPetMirror(source, clone);
    }

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

Player* PlayerbotObcCloneManager::CreateCustomGameClone(Player* source, Battleground* bg, uint32 team, std::string const& displayPrefix)
{
    if (!source || !bg || (team != ALLIANCE && team != HORDE))
        return nullptr;

    std::string const internalName = GenerateCloneInternalName();
    if (internalName.empty())
        return nullptr;

    uint8 const expansion = static_cast<uint8>(sWorld->getIntConfig(CONFIG_EXPANSION));
    auto session = std::make_unique<WorldSession>(0, "custom_game_in_memory_clone", nullptr, SEC_PLAYER, expansion, 0,
        Minutes(0), LOCALE_enUS, 0, false);
    session->SetTransientPlayerSession();

    Player* clone = new Player(session.get());
    CharacterCreateInfo createInfo;
    createInfo.SetName(internalName)
        .SetRace(source->GetRace()).SetClass(source->GetClass()).SetGender(source->GetNativeGender())
        .SetSkin(source->GetSkinId()).SetFace(source->GetFaceId()).SetHairStyle(source->GetHairStyleId())
        .SetHairColor(source->GetHairColorId()).SetFacialHair(source->GetFacialStyle()).SetOutfitId(0);

    ObjectGuid::LowType const cloneLowGuid = sObjectMgr->GetGenerator<HighGuid::Player>().Generate();
    if (!clone->Create(cloneLowGuid, &createInfo, false, false))
    {
        DestroyUnseatedClone(session, clone);
        return nullptr;
    }

    clone->SetGender(source->GetNativeGender());
    clone->SetNativeGender(source->GetNativeGender());
    clone->InitDisplayIds();
    session->SetPlayer(clone);
    clone->GetMotionMaster()->Initialize();
    clone->SetLevel(source->GetLevel(), false);
    clone->InitStatsForLevel();
    clone->InitTalentForLevel();
    clone->InitGlyphsForLevel();
    CopySkills(clone, source);
    CopySpellsTalentsAndGlyphs(clone, source);
    if (!CopyEquipment(clone, source))
    {
        DestroyUnseatedClone(session, clone);
        return nullptr;
    }

    clone->UpdateAllStats();
    clone->SetFullHealth();
    for (uint8 power = POWER_MANA; power < MAX_POWERS; ++power)
        clone->SetFullPower(Powers(power));

    BattlegroundQueueTypeId const queueTypeId = BattlegroundMgr::BGQueueTypeId(bg->GetTypeID(), bg->GetArenaType());
    Position const* start = bg->GetTeamStartPosition(Battleground::GetTeamIndexByTeamId(team));
    if (queueTypeId == BATTLEGROUND_QUEUE_NONE || !start || clone->AddBattlegroundQueueId(queueTypeId) >= PLAYER_MAX_BATTLEGROUND_QUEUES)
    {
        DestroyUnseatedClone(session, clone);
        return nullptr;
    }

    clone->SetInviteForBattlegroundQueueType(queueTypeId, bg->GetInstanceID());
    clone->SetBattlegroundId(bg->GetInstanceID(), bg->GetTypeID());
    clone->SetFactionForRace(team == HORDE ? RACE_BLOODELF : RACE_HUMAN);
    clone->SetBGTeam(team);
    clone->ResetMap();
    clone->Relocate(*start);
    clone->SetMap(bg->GetBgMap());
    bg->IncreaseInvitedCount(team);
    if (!bg->GetBgMap()->AddPlayerToMap(clone))
    {
        bg->DecreaseInvitedCount(team);
        DestroyUnseatedClone(session, clone);
        return nullptr;
    }

    ObjectGuid const cloneGuid = clone->GetGUID();
    std::string displayName = displayPrefix + source->GetName();
    sCharacterCache->AddCharacterCacheEntry(cloneGuid, 0, displayName, clone->GetNativeGender(), clone->GetRace(), clone->GetClass(), clone->GetLevel(), false);
    ObjectAccessor::AddObject(clone);
    bg->AddPlayer(clone);
    clone->SetInGameTime(GameTime::GetGameTimeMS());
    SynchronizeHunterPetMirror(source, clone);

    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        g_CloneSessions.emplace(cloneGuid, std::move(session));
        g_CustomGameClones.emplace(cloneGuid, CustomGameCloneRecord{ cloneGuid, source->GetGUID(), team, bg->GetInstanceID(), bg->GetTypeID() });
    }

    return clone;
}

bool PlayerbotObcCloneManager::QueueCustomGameClone(ObjectGuid sourceGuid, WorldSession* callbackSession, Battleground* bg,
    uint32 team, std::string const& displayPrefix)
{
    if (!sourceGuid || !bg || (team != ALLIANCE && team != HORDE))
    {
        if (bg)
            bg->ResolveCustomGamePendingClone();
        return false;
    }

    if (Player* onlineSource = ObjectAccessor::FindConnectedPlayer(sourceGuid))
    {
        bool const created = CreateCustomGameClone(onlineSource, bg, team, displayPrefix) != nullptr;
        bg->ResolveCustomGamePendingClone();
        return created;
    }

    if (!callbackSession)
    {
        bg->ResolveCustomGamePendingClone();
        return false;
    }

    CharacterCacheEntry const* characterInfo = sCharacterCache->GetCharacterCacheByGuid(sourceGuid);
    if (!characterInfo)
    {
        bg->ResolveCustomGamePendingClone();
        return false;
    }

    auto holder = std::make_shared<LoginQueryHolder>(characterInfo->AccountId, sourceGuid);
    if (!holder->Initialize())
    {
        bg->ResolveCustomGamePendingClone();
        return false;
    }

    uint32 const battlegroundInstanceId = bg->GetInstanceID();
    BattlegroundTypeId const battlegroundType = bg->GetTypeID();
    SQLQueryHolderCallback& callback = callbackSession->AddQueryHolderCallback(CharacterDatabase.DelayQueryHolder(holder));
    callback.AfterComplete([sourceGuid, battlegroundInstanceId, battlegroundType, team, displayPrefix](SQLQueryHolderBase const& queryHolder)
    {
        Battleground* targetBg = sBattlegroundMgr->GetBattleground(battlegroundInstanceId, battlegroundType);
        if (!targetBg)
            return;

        if (targetBg->GetStatus() != STATUS_NONE && targetBg->GetStatus() != STATUS_WAIT_LEAVE)
        {
            if (Player* onlineSource = ObjectAccessor::FindConnectedPlayer(sourceGuid))
                PlayerbotObcCloneManager::CreateCustomGameClone(onlineSource, targetBg, team, displayPrefix);
            else
            {
                LoginQueryHolder const& loginHolder = static_cast<LoginQueryHolder const&>(queryHolder);
                uint8 const expansion = static_cast<uint8>(sWorld->getIntConfig(CONFIG_EXPANSION));
                auto sourceSession = std::make_unique<WorldSession>(loginHolder.GetAccountId(), "custom_game_offline_clone_source",
                    nullptr, SEC_PLAYER, expansion, 0, Minutes(0), LOCALE_enUS, 0, false);
                sourceSession->SetTransientPlayerSession();

                Player* offlineSource = new Player(sourceSession.get());
                if (offlineSource->LoadFromDB(sourceGuid, loginHolder, false))
                {
                    offlineSource->GetMotionMaster()->Initialize();
                    // Dark copies only read identity, spells, talents, glyph ids, and
                    // equipment from this temporary source. Runtime auras are neither
                    // copied nor safe to retain on a never-world offline Player.
                    offlineSource->RemoveAllAuras();
                    PlayerbotObcCloneManager::CreateCustomGameClone(offlineSource, targetBg, team, displayPrefix);
                }

                DestroyUnseatedClone(sourceSession, offlineSource);
            }
        }

        targetBg->ResolveCustomGamePendingClone();
    });

    return true;
}

void PlayerbotObcCloneManager::DestroyCustomGameClones(uint32 battlegroundInstanceId)
{
    std::vector<ObjectGuid> cloneGuids;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        for (auto const& [cloneGuid, record] : g_CustomGameClones)
            if (record.battlegroundInstanceId == battlegroundInstanceId)
                cloneGuids.push_back(cloneGuid);
    }

    for (ObjectGuid cloneGuid : cloneGuids)
        TeardownCustomGameClone(cloneGuid);
}

bool PlayerbotObcCloneManager::QueueCustomGameLobbyClone(ObjectGuid sourceGuid, WorldSession* callbackSession,
    uint32 mapId, uint32 lobbyInstanceId, uint32 rosterSlotId, uint32 team, bool isPlayerbot, Position const& position,
    std::string const& displayPrefix)
{
    if (!sourceGuid || (team != ALLIANCE && team != HORDE) ||
        HasCustomGameLobbyClone(lobbyInstanceId, rosterSlotId))
        return false;

    if (Player* onlineSource = ObjectAccessor::FindConnectedPlayer(sourceGuid))
        return CreateCustomGameLobbyClone(onlineSource, mapId, lobbyInstanceId, team, rosterSlotId, isPlayerbot, position,
            displayPrefix) != nullptr;

    if (!callbackSession)
        return false;

    CharacterCacheEntry const* characterInfo = sCharacterCache->GetCharacterCacheByGuid(sourceGuid);
    if (!characterInfo)
        return false;

    auto holder = std::make_shared<LoginQueryHolder>(characterInfo->AccountId, sourceGuid);
    if (!holder->Initialize())
        return false;

    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        g_PendingCustomGameLobbyClones.push_back({ sourceGuid, mapId, lobbyInstanceId, rosterSlotId, team, isPlayerbot,
            position, displayPrefix });
    }

    SQLQueryHolderCallback& callback = callbackSession->AddQueryHolderCallback(CharacterDatabase.DelayQueryHolder(holder));
    callback.AfterComplete([sourceGuid, lobbyInstanceId, rosterSlotId](SQLQueryHolderBase const& queryHolder)
    {
        PendingCustomGameLobbyClone request;
        {
            std::lock_guard<std::mutex> lock(g_ObcCloneLock);
            auto itr = std::find_if(g_PendingCustomGameLobbyClones.begin(), g_PendingCustomGameLobbyClones.end(),
                [&](PendingCustomGameLobbyClone const& pending)
                {
                    return MatchesLobbyClone(pending, lobbyInstanceId, rosterSlotId);
                });
            if (itr == g_PendingCustomGameLobbyClones.end())
                return;

            request = *itr;
            g_PendingCustomGameLobbyClones.erase(itr);
        }

        if (!sMapMgr->FindMap(request.mapId, request.lobbyInstanceId))
            return;

        if (Player* onlineSource = ObjectAccessor::FindConnectedPlayer(sourceGuid))
        {
            CreateCustomGameLobbyClone(onlineSource, request.mapId, request.lobbyInstanceId, request.team,
                request.rosterSlotId, request.isPlayerbot, request.position, request.displayPrefix);
            return;
        }

        LoginQueryHolder const& loginHolder = static_cast<LoginQueryHolder const&>(queryHolder);
        uint8 const expansion = static_cast<uint8>(sWorld->getIntConfig(CONFIG_EXPANSION));
        auto sourceSession = std::make_unique<WorldSession>(loginHolder.GetAccountId(), "custom_game_lobby_offline_source",
            nullptr, SEC_PLAYER, expansion, 0, Minutes(0), LOCALE_enUS, 0, false);
        sourceSession->SetTransientPlayerSession();

        Player* offlineSource = new Player(sourceSession.get());
        if (offlineSource->LoadFromDB(sourceGuid, loginHolder, false))
        {
            offlineSource->GetMotionMaster()->Initialize();
            // Equipment loading can still apply passive effects even when
            // persisted and glyph auras are skipped. The preview copy does
            // not consume those effects, so detach them before copying.
            offlineSource->RemoveAllAuras();
            CreateCustomGameLobbyClone(offlineSource, request.mapId, request.lobbyInstanceId, request.team,
                request.rosterSlotId, request.isPlayerbot, request.position, request.displayPrefix);
        }

        DestroyUnseatedClone(sourceSession, offlineSource);
    });

    return true;
}

void PlayerbotObcCloneManager::SetCustomGameLobbyClonePosition(uint32 lobbyInstanceId, uint32 rosterSlotId,
    Position const& position)
{
    ObjectGuid cloneGuid;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        for (auto& pending : g_PendingCustomGameLobbyClones)
            if (MatchesLobbyClone(pending, lobbyInstanceId, rosterSlotId))
                pending.position = position;

        for (auto const& [guid, record] : g_CustomGameLobbyClones)
            if (MatchesLobbyClone(record, lobbyInstanceId, rosterSlotId))
            {
                cloneGuid = guid;
                break;
            }
    }

    if (Player* clone = ObjectAccessor::FindConnectedPlayer(cloneGuid))
    {
        // These server-owned preview players have no client to acknowledge a
        // normal Player::NearTeleportTo.  Relocate them immediately and only
        // when their roster slot actually changed; the teleport packet keeps
        // nearby real clients visually synchronized without setting a pending
        // player-teleport semaphore.
        if (clone->GetExactDistSq(position) > 0.01f)
        {
            clone->SendTeleportPacket(position);
            clone->UpdatePosition(position, true);
            clone->UpdateObjectVisibility();
        }
    }
}

void PlayerbotObcCloneManager::DestroyCustomGameLobbyClone(uint32 lobbyInstanceId, uint32 rosterSlotId)
{
    std::vector<ObjectGuid> cloneGuids;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        g_PendingCustomGameLobbyClones.erase(std::remove_if(g_PendingCustomGameLobbyClones.begin(),
            g_PendingCustomGameLobbyClones.end(), [&](PendingCustomGameLobbyClone const& pending)
            {
                return MatchesLobbyClone(pending, lobbyInstanceId, rosterSlotId);
            }), g_PendingCustomGameLobbyClones.end());

        for (auto const& [cloneGuid, record] : g_CustomGameLobbyClones)
            if (MatchesLobbyClone(record, lobbyInstanceId, rosterSlotId))
                cloneGuids.push_back(cloneGuid);
    }

    for (ObjectGuid cloneGuid : cloneGuids)
        TeardownCustomGameLobbyClone(cloneGuid);
}

void PlayerbotObcCloneManager::DestroyCustomGameLobbyClones(uint32 lobbyInstanceId)
{
    std::vector<ObjectGuid> cloneGuids;
    {
        std::lock_guard<std::mutex> lock(g_ObcCloneLock);
        g_PendingCustomGameLobbyClones.erase(std::remove_if(g_PendingCustomGameLobbyClones.begin(),
            g_PendingCustomGameLobbyClones.end(), [lobbyInstanceId](PendingCustomGameLobbyClone const& pending)
            {
                return pending.lobbyInstanceId == lobbyInstanceId;
            }), g_PendingCustomGameLobbyClones.end());

        for (auto const& [cloneGuid, record] : g_CustomGameLobbyClones)
            if (record.lobbyInstanceId == lobbyInstanceId)
                cloneGuids.push_back(cloneGuid);
    }

    for (ObjectGuid cloneGuid : cloneGuids)
        TeardownCustomGameLobbyClone(cloneGuid);
}
}
