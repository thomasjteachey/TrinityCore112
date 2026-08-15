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

#include "SpellHistory.h"
#include <algorithm>
#include "DatabaseEnv.h"
#include "Item.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Pet.h"
#include "Player.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "VioletHoldBoons.h"
#include "WorldPacket.h"

SpellHistory::Clock::duration const SpellHistory::InfinityCooldownDelay = std::chrono::duration_cast<SpellHistory::Clock::duration>(std::chrono::seconds(MONTH));
SpellHistory::Clock::duration const SpellHistory::InfinityCooldownDelayCheck = std::chrono::duration_cast<SpellHistory::Clock::duration>(std::chrono::seconds(MONTH / 2));

template<>
struct SpellHistory::PersistenceHelper<Player>
{
    static CharacterDatabaseStatements const CooldownsDeleteStatement = CHAR_DEL_CHAR_SPELL_COOLDOWNS;
    static CharacterDatabaseStatements const CooldownsInsertStatement = CHAR_INS_CHAR_SPELL_COOLDOWN;

    static void SetIdentifier(PreparedStatementBase* stmt, uint8 index, Unit* owner) { stmt->setUInt32(index, owner->GetGUID().GetCounter()); }

    static bool ReadCooldown(Field* fields, uint32* spellId, CooldownEntry* cooldownEntry)
    {
        *spellId = fields[0].GetUInt32();
        if (!sSpellMgr->GetSpellInfo(*spellId))
            return false;

        cooldownEntry->SpellId = *spellId;
        cooldownEntry->CooldownEnd = Clock::from_time_t(time_t(fields[2].GetUInt32()));
        cooldownEntry->ItemId = fields[1].GetUInt32();
        cooldownEntry->CategoryId = fields[3].GetUInt32();
        cooldownEntry->CategoryEnd = Clock::from_time_t(time_t(fields[4].GetUInt32()));
        return true;
    }

    static void WriteCooldown(PreparedStatementBase* stmt, uint8& index, CooldownStorageType::value_type const& cooldown)
    {
        stmt->setUInt32(index++, cooldown.first);
        stmt->setUInt32(index++, cooldown.second.ItemId);
        stmt->setUInt32(index++, uint32(Clock::to_time_t(cooldown.second.CooldownEnd)));
        stmt->setUInt32(index++, cooldown.second.CategoryId);
        stmt->setUInt32(index++, uint32(Clock::to_time_t(cooldown.second.CategoryEnd)));
    }
};

template<>
struct SpellHistory::PersistenceHelper<Pet>
{
    static CharacterDatabaseStatements const CooldownsDeleteStatement = CHAR_DEL_PET_SPELL_COOLDOWNS;
    static CharacterDatabaseStatements const CooldownsInsertStatement = CHAR_INS_PET_SPELL_COOLDOWN;

    static void SetIdentifier(PreparedStatementBase* stmt, uint8 index, Unit* owner) { stmt->setUInt32(index, owner->GetCharmInfo()->GetPetNumber()); }

    static bool ReadCooldown(Field* fields, uint32* spellId, CooldownEntry* cooldownEntry)
    {
        *spellId = fields[0].GetUInt32();
        if (!sSpellMgr->GetSpellInfo(*spellId))
            return false;

        cooldownEntry->SpellId = *spellId;
        cooldownEntry->CooldownEnd = Clock::from_time_t(time_t(fields[1].GetUInt32()));
        cooldownEntry->ItemId = 0;
        cooldownEntry->CategoryId = fields[2].GetUInt32();
        cooldownEntry->CategoryEnd = Clock::from_time_t(time_t(fields[3].GetUInt32()));
        return true;
    }

    static void WriteCooldown(PreparedStatementBase* stmt, uint8& index, CooldownStorageType::value_type const& cooldown)
    {
        stmt->setUInt32(index++, cooldown.first);
        stmt->setUInt32(index++, uint32(Clock::to_time_t(cooldown.second.CooldownEnd)));
        stmt->setUInt32(index++, cooldown.second.CategoryId);
        stmt->setUInt32(index++, uint32(Clock::to_time_t(cooldown.second.CategoryEnd)));
    }
};

template<class OwnerType>
void SpellHistory::LoadFromDB(PreparedQueryResult cooldownsResult)
{
    typedef PersistenceHelper<OwnerType> StatementInfo;

    if (cooldownsResult)
    {
        do
        {
            uint32 spellId;
            CooldownEntry cooldown;
            if (StatementInfo::ReadCooldown(cooldownsResult->Fetch(), &spellId, &cooldown))
            {
                _spellCooldowns[spellId] = cooldown;
                if (cooldown.CategoryId)
                    _categoryCooldowns[cooldown.CategoryId] = &_spellCooldowns[spellId];
            }

        } while (cooldownsResult->NextRow());
    }
}

template<class OwnerType>
void SpellHistory::SaveToDB(CharacterDatabaseTransaction trans)
{
    typedef PersistenceHelper<OwnerType> StatementInfo;

    uint8 index = 0;
    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(StatementInfo::CooldownsDeleteStatement);
    StatementInfo::SetIdentifier(stmt, index++, _owner);
    trans->Append(stmt);

    for (auto const& p : _spellCooldowns)
    {
        if (!p.second.OnHold)
        {
            index = 0;
            stmt = CharacterDatabase.GetPreparedStatement(StatementInfo::CooldownsInsertStatement);
            StatementInfo::SetIdentifier(stmt, index++, _owner);
            StatementInfo::WriteCooldown(stmt, index, p);
            trans->Append(stmt);
        }
    }
}

void SpellHistory::Update()
{
    Clock::time_point now = GameTime::GetSystemTime();
    for (auto itr = _categoryCooldowns.begin(); itr != _categoryCooldowns.end();)
    {
        if (itr->second->CategoryEnd < now)
            itr = _categoryCooldowns.erase(itr);
        else
            ++itr;
    }

    for (auto itr = _spellCooldowns.begin(); itr != _spellCooldowns.end();)
    {
        if (itr->second.CooldownEnd < now)
            itr = EraseCooldown(itr);
        else
            ++itr;
    }
}

void SpellHistory::HandleCooldowns(SpellInfo const* spellInfo, Item const* item, Spell* spell /*= nullptr*/)
{
    HandleCooldowns(spellInfo, item ? item->GetEntry() : 0, spell);
}

void SpellHistory::HandleCooldowns(SpellInfo const* spellInfo, uint32 itemID, Spell* spell /*= nullptr*/)
{
    if (spell && spell->IsIgnoringCooldowns())
        return;

    if (Player* player = _owner->ToPlayer())
    {
        // potions start cooldown until exiting combat
        if (ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(itemID))
        {
            if (itemTemplate->IsPotion() || spellInfo->IsCooldownStartedOnEvent())
            {
                player->SetLastPotionId(itemID);
                return;
            }
        }
    }

    if (spellInfo->IsCooldownStartedOnEvent() || spellInfo->IsPassive())
        return;

    StartCooldown(spellInfo, itemID, spell);
}

bool SpellHistory::IsReady(SpellInfo const* spellInfo, uint32 itemId /*= 0*/, bool ignoreCategoryCooldown /*= false*/) const
{
    if (IsSchoolLocked(spellInfo->GetSchoolMask()))
        return false;

    if (HasCooldown(spellInfo->Id, itemId, ignoreCategoryCooldown))
        return false;

    return true;
}

template<>
void SpellHistory::WritePacket<Pet>(WorldPacket& packet) const
{
    Clock::time_point now = GameTime::GetSystemTime();

    uint8 cooldownsCount = uint8(_spellCooldowns.size());
    packet << uint8(cooldownsCount);

    for (auto const& spellCooldown : _spellCooldowns)
    {
        packet << uint32(spellCooldown.first);              // spell ID
        packet << uint16(spellCooldown.second.CategoryId);  // spell category
        if (!spellCooldown.second.OnHold)
        {
            std::chrono::milliseconds cooldownDuration = std::chrono::duration_cast<std::chrono::milliseconds>(spellCooldown.second.CooldownEnd - now);
            if (cooldownDuration.count() <= 0)
            {
                packet << uint32(0);
                packet << uint32(0);
                continue;
            }

            std::chrono::milliseconds categoryDuration = std::chrono::duration_cast<std::chrono::milliseconds>(spellCooldown.second.CategoryEnd - now);
            if (categoryDuration.count() > 0)
            {
                packet << uint32(0);
                packet << uint32(categoryDuration.count());
            }
            else
            {
                packet << uint32(cooldownDuration.count());
                packet << uint32(0);
            }
        }
    }
}

template<>
void SpellHistory::WritePacket<Player>(WorldPacket& packet) const
{
    Clock::time_point now = GameTime::GetSystemTime();
    Clock::time_point infTime = now + InfinityCooldownDelayCheck;

    packet << uint16(_spellCooldowns.size());

    for (auto const& spellCooldown : _spellCooldowns)
    {
        packet << uint32(spellCooldown.first);
        packet << uint16(spellCooldown.second.ItemId);        // cast item id
        packet << uint16(spellCooldown.second.CategoryId);    // spell category

        // send infinity cooldown in special format
        if (spellCooldown.second.CooldownEnd >= infTime)
        {
            packet << uint32(1);                              // cooldown
            packet << uint32(0x80000000);                     // category cooldown
            continue;
        }

        std::chrono::milliseconds cooldownDuration = std::chrono::duration_cast<std::chrono::milliseconds>(spellCooldown.second.CooldownEnd - now);
        if (cooldownDuration.count() <= 0)
        {
            packet << uint32(0);
            packet << uint32(0);
            continue;
        }

        std::chrono::milliseconds categoryDuration = std::chrono::duration_cast<std::chrono::milliseconds>(spellCooldown.second.CategoryEnd - now);
        if (categoryDuration.count() >= 0)
        {
            packet << uint32(0);                              // cooldown
            packet << uint32(categoryDuration.count());       // category cooldown
        }
        else
        {
            packet << uint32(cooldownDuration.count());       // cooldown
            packet << uint32(0);                              // category cooldown
        }
    }
}

void SpellHistory::StartCooldown(SpellInfo const* spellInfo, uint32 itemId, Spell* spell /*= nullptr*/, bool onHold /*= false*/)
{
    // init cooldown values
    uint32 categoryId = 0;
    int32 cooldown = -1;
    int32 categoryCooldown = -1;

    GetCooldownDurations(spellInfo, itemId, &cooldown, &categoryId, &categoryCooldown);

    Clock::time_point curTime = GameTime::GetSystemTime();
    Clock::time_point catrecTime;
    Clock::time_point recTime;
    bool needsCooldownPacket = false;

    // overwrite time for selected category
    if (onHold)
    {
        // use +MONTH as infinite cooldown marker
        catrecTime = categoryCooldown > 0 ? (curTime + InfinityCooldownDelay) : curTime;
        recTime = cooldown > 0 ? (curTime + InfinityCooldownDelay) : catrecTime;
    }
    else
    {
        // shoot spells used equipped item cooldown values already assigned in GetAttackTime(RANGED_ATTACK)
        // prevent 0 cooldowns set by another way
        if (cooldown <= 0 && categoryCooldown <= 0 && (categoryId == 76 || (spellInfo->IsAutoRepeatRangedSpell() && spellInfo->Id != 75)))
            cooldown = _owner->GetAttackTime(RANGED_ATTACK);

        // Now we have cooldown data (if found any), time to apply mods
        if (Player* modOwner = _owner->GetSpellModOwner())
        {
            if (cooldown >= 0)
                modOwner->ApplySpellMod(spellInfo->Id, SPELLMOD_COOLDOWN, cooldown, spell);

            if (categoryCooldown >= 0 && !spellInfo->HasAttribute(SPELL_ATTR6_IGNORE_CATEGORY_COOLDOWN_MODS))
                modOwner->ApplySpellMod(spellInfo->Id, SPELLMOD_COOLDOWN, categoryCooldown, spell);
        }

        if (int32 cooldownMod = _owner->GetTotalAuraModifier(SPELL_AURA_MOD_COOLDOWN))
        {
            // Apply SPELL_AURA_MOD_COOLDOWN only to own spells
            Player* playerOwner = GetPlayerOwner();
            if (!playerOwner || playerOwner->HasSpell(spellInfo->Id))
            {
                needsCooldownPacket = true;
                cooldown += cooldownMod * IN_MILLISECONDS;   // SPELL_AURA_MOD_COOLDOWN does not affect category cooldows, verified with shaman shocks
            }
        }

        // Violet Hold "Boon of Alacrity": a percentage off the holder's spell
        // cooldowns, self and category alike. Item-use cooldowns (potions,
        // trinkets) are left alone. The client predicts the DBC value on its
        // own, so it is told the real one, exactly as MOD_COOLDOWN does.
        if (!itemId && (cooldown > 0 || categoryCooldown > 0))
        {
            if (int32 boonPct = VioletHoldBoons::GetCooldownReductionPct(_owner))
            {
                if (cooldown > 0)
                    cooldown = cooldown * (100 - boonPct) / 100;
                if (categoryCooldown > 0)
                    categoryCooldown = categoryCooldown * (100 - boonPct) / 100;
                needsCooldownPacket = true;
            }
        }

        // replace negative cooldowns by 0
        if (cooldown < 0)
            cooldown = 0;

        if (categoryCooldown < 0)
            categoryCooldown = 0;

        // no cooldown after applying spell mods
        if (cooldown == 0 && categoryCooldown == 0)
            return;

        catrecTime = categoryCooldown ? curTime + std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(categoryCooldown)) : curTime;
        recTime = cooldown ? curTime + std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(cooldown)) : catrecTime;
    }

    // self spell cooldown
    if (recTime != curTime)
    {
        AddCooldown(spellInfo->Id, itemId, recTime, categoryId, catrecTime, onHold);

        if (needsCooldownPacket)
        {
            if (Player* playerOwner = GetPlayerOwner())
            {
                WorldPacket spellCooldown;
                // A category-only spell (shocks, traps) has cooldown 0 here and
                // lives on catrecTime; telling the client "0" would clear its
                // own prediction, so send the category value for that case.
                BuildCooldownPacket(spellCooldown, SPELL_COOLDOWN_FLAG_NONE, spellInfo->Id, cooldown ? cooldown : categoryCooldown);
                playerOwner->SendDirectMessage(&spellCooldown);
            }
        }
    }
}

void SpellHistory::SendCooldownEvent(SpellInfo const* spellInfo, uint32 itemId /*= 0*/, Spell* spell /*= nullptr*/, bool startCooldown /*= true*/)
{
    // Send activate cooldown timer (possible 0) at client side
    if (Player* player = GetPlayerOwner())
    {
        uint32 category = spellInfo->GetCategory();
        GetCooldownDurations(spellInfo, itemId, nullptr, &category, nullptr);

        auto categoryItr = _categoryCooldowns.find(category);
        if (categoryItr != _categoryCooldowns.end() && categoryItr->second->SpellId != spellInfo->Id)
        {
            WorldPacket data(SMSG_COOLDOWN_EVENT, 4 + 8);
            data << uint32(categoryItr->second->SpellId);
            data << uint64(_owner->GetGUID());
            player->SendDirectMessage(&data);

            if (startCooldown)
                StartCooldown(sSpellMgr->AssertSpellInfo(categoryItr->second->SpellId), itemId, spell);
        }

        WorldPacket data(SMSG_COOLDOWN_EVENT, 4 + 8);
        data << uint32(spellInfo->Id);
        data << uint64(_owner->GetGUID());
        player->SendDirectMessage(&data);
    }

    // start cooldowns at server side, if any
    if (startCooldown)
        StartCooldown(spellInfo, itemId, spell);
}

void SpellHistory::AddCooldown(uint32 spellId, uint32 itemId, Clock::time_point cooldownEnd, uint32 categoryId, Clock::time_point categoryEnd, bool onHold /*= false*/)
{
    CooldownEntry& cooldownEntry = _spellCooldowns[spellId];
    cooldownEntry.SpellId = spellId;
    cooldownEntry.CooldownEnd = cooldownEnd;
    cooldownEntry.ItemId = itemId;
    cooldownEntry.CategoryId = categoryId;
    cooldownEntry.CategoryEnd = categoryEnd;
    cooldownEntry.OnHold = onHold;

    if (categoryId)
        _categoryCooldowns[categoryId] = &cooldownEntry;
}

void SpellHistory::ModifyCooldown(uint32 spellId, int32 cooldownModMs)
{
    auto itr = _spellCooldowns.find(spellId);
    if (!cooldownModMs || itr == _spellCooldowns.end())
        return;

    Clock::time_point now = GameTime::GetSystemTime();
    Clock::duration offset = std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(cooldownModMs));
    if (itr->second.CooldownEnd + offset > now)
    {
        itr->second.CooldownEnd += offset;
        if (itr->second.CategoryId)
        {
            Clock::time_point newCategoryEnd = itr->second.CategoryEnd + offset;
            itr->second.CategoryEnd = newCategoryEnd > now ? newCategoryEnd : now;
        }
    }
    else
        EraseCooldown(itr);

    if (Player* playerOwner = GetPlayerOwner())
    {
        WorldPacket modifyCooldown(SMSG_MODIFY_COOLDOWN, 4 + 8 + 4);
        modifyCooldown << uint32(spellId);
        modifyCooldown << uint64(_owner->GetGUID());
        modifyCooldown << int32(cooldownModMs);
        playerOwner->SendDirectMessage(&modifyCooldown);
    }
}

uint32 SpellHistory::GetCategoryCooldownSpellId(uint32 categoryId) const
{
    auto itr = _categoryCooldowns.find(categoryId);
    if (itr == _categoryCooldowns.end() || !itr->second)
        return 0;

    return itr->second->SpellId;
}

void SpellHistory::ResetCooldown(uint32 spellId, bool update /*= false*/)
{
    auto itr = _spellCooldowns.find(spellId);
    if (itr == _spellCooldowns.end())
        return;

    ResetCooldown(itr, update);
}

void SpellHistory::ResetCooldown(CooldownStorageType::iterator& itr, bool update /*= false*/)
{
    if (update)
    {
        if (Player* playerOwner = GetPlayerOwner())
        {
            WorldPacket data(SMSG_CLEAR_COOLDOWN, 4 + 8);
            data << uint32(itr->first);
            data << uint64(_owner->GetGUID());
            playerOwner->SendDirectMessage(&data);
        }
    }

    itr = EraseCooldown(itr);
}

uint32 SpellHistory::ResetCategoryCooldown(uint32 categoryId, bool update /*= false*/)
{
    auto categoryItr = _categoryCooldowns.find(categoryId);
    if (categoryItr == _categoryCooldowns.end())
        return 0;

    uint32 removedSpellId = categoryItr->second->SpellId;

    std::vector<int32> resetCooldowns;
    resetCooldowns.reserve(_spellCooldowns.size());

    for (auto itr = _spellCooldowns.begin(); itr != _spellCooldowns.end();)
    {
        if (itr->second.CategoryId != categoryId)
        {
            ++itr;
            continue;
        }

        if (update)
            resetCooldowns.push_back(itr->first);

        itr = EraseCooldown(itr);
    }

    _categoryCooldowns.erase(categoryId);

    if (update && !resetCooldowns.empty())
        SendClearCooldowns(resetCooldowns);

    return removedSpellId;
}

void SpellHistory::ResetAllCooldowns()
{
    if (GetPlayerOwner())
    {
        std::vector<int32> cooldowns;
        cooldowns.reserve(_spellCooldowns.size());
        for (auto const& p : _spellCooldowns)
            cooldowns.push_back(p.first);

        SendClearCooldowns(cooldowns);
    }

    _categoryCooldowns.clear();
    _spellCooldowns.clear();
}

bool SpellHistory::HasCooldown(SpellInfo const* spellInfo, uint32 itemId /*= 0*/, bool ignoreCategoryCooldown /*= false*/) const
{
    if (_spellCooldowns.count(spellInfo->Id) != 0)
        return true;

    if (ignoreCategoryCooldown)
        return false;

    uint32 category = 0;
    GetCooldownDurations(spellInfo, itemId, nullptr, &category, nullptr);
    if (!category)
        return false;

    return _categoryCooldowns.count(category) != 0;
}

bool SpellHistory::HasCooldown(uint32 spellId, uint32 itemId /*= 0*/, bool ignoreCategoryCooldown /*= false*/) const
{
    return HasCooldown(sSpellMgr->AssertSpellInfo(spellId), itemId, ignoreCategoryCooldown);
}

uint32 SpellHistory::GetRemainingCooldown(SpellInfo const* spellInfo) const
{
    Clock::time_point end;
    auto itr = _spellCooldowns.find(spellInfo->Id);
    if (itr != _spellCooldowns.end())
        end = itr->second.CooldownEnd;
    else
    {
        auto catItr = _categoryCooldowns.find(spellInfo->GetCategory());
        if (catItr == _categoryCooldowns.end())
            return 0;

        end = catItr->second->CategoryEnd;
    }

    Clock::time_point now = GameTime::GetSystemTime();
    if (end < now)
        return 0;

    Clock::duration remaining = end - now;
    return uint32(std::chrono::duration_cast<std::chrono::milliseconds>(remaining).count());
}

void SpellHistory::LockSpellSchool(SpellSchoolMask schoolMask, uint32 lockoutTime)
{
    Clock::time_point now = GameTime::GetSystemTime();
    Clock::time_point lockoutEnd = now + std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(lockoutTime));
    for (uint32 i = 0; i < MAX_SPELL_SCHOOL; ++i)
        if (SpellSchoolMask(1 << i) & schoolMask)
            _schoolLockouts[i] = lockoutEnd;

    std::set<uint32> knownSpells;
    if (Player* plrOwner = _owner->ToPlayer())
    {
        for (auto const& p : plrOwner->GetSpellMap())
            if (p.second.state != PLAYERSPELL_REMOVED)
                knownSpells.insert(p.first);
    }
    else if (Pet* petOwner = _owner->ToPet())
    {
        for (auto const& p : petOwner->m_spells)
            if (p.second.state != PETSPELL_REMOVED)
                knownSpells.insert(p.first);
    }
    else
    {
        Creature* creatureOwner = _owner->ToCreature();
        for (uint8 i = 0; i < MAX_CREATURE_SPELLS; ++i)
            if (creatureOwner->m_spells[i])
                knownSpells.insert(creatureOwner->m_spells[i]);
    }

    PacketCooldowns cooldowns;
    WorldPacket spellCooldowns;
    for (uint32 spellId : knownSpells)
    {
        SpellInfo const* spellInfo = sSpellMgr->AssertSpellInfo(spellId);
        if (spellInfo->IsCooldownStartedOnEvent())
            continue;

        if ((schoolMask & spellInfo->GetSchoolMask()) && GetRemainingCooldown(spellInfo) < lockoutTime)
        {
            cooldowns[spellId] = lockoutTime;
            AddCooldown(spellId, 0, lockoutEnd, 0, now);
        }
    }

    if (Player* player = GetPlayerOwner())
    {
        if (!cooldowns.empty())
        {
            BuildCooldownPacket(spellCooldowns, SPELL_COOLDOWN_FLAG_NONE, cooldowns);
            player->SendDirectMessage(&spellCooldowns);
        }
    }
}

bool SpellHistory::IsSchoolLocked(SpellSchoolMask schoolMask) const
{
    Clock::time_point now = GameTime::GetSystemTime();
    for (uint32 i = 0; i < MAX_SPELL_SCHOOL; ++i)
        if (SpellSchoolMask(1 << i) & schoolMask)
            if (_schoolLockouts[i] > now)
                return true;

    return false;
}

bool SpellHistory::HasGlobalCooldown(SpellInfo const* spellInfo) const
{
    auto itr = _globalCooldowns.find(spellInfo->StartRecoveryCategory);
    return itr != _globalCooldowns.end() && itr->second > GameTime::GetSystemTime();
}

void SpellHistory::AddGlobalCooldown(SpellInfo const* spellInfo, uint32 duration)
{
    _globalCooldowns[spellInfo->StartRecoveryCategory] = GameTime::GetSystemTime() + std::chrono::duration_cast<Clock::duration>(std::chrono::milliseconds(duration));
}

void SpellHistory::CancelGlobalCooldown(SpellInfo const* spellInfo)
{
    _globalCooldowns[spellInfo->StartRecoveryCategory] = Clock::time_point(Clock::duration(0));
}

uint32 SpellHistory::GetRemainingGlobalCooldown(SpellInfo const* spellInfo) const
{
    if (!spellInfo || !spellInfo->StartRecoveryCategory)
        return 0;

    auto itr = _globalCooldowns.find(spellInfo->StartRecoveryCategory);
    if (itr == _globalCooldowns.end())
        return 0;

    Clock::time_point now = GameTime::GetSystemTime();
    if (itr->second <= now)
        return 0;

    return std::chrono::duration_cast<std::chrono::milliseconds>(itr->second - now).count();
}

void SpellHistory::ReduceGlobalCooldown(SpellInfo const* spellInfo, std::chrono::milliseconds reduction)
{
    if (!spellInfo || !spellInfo->StartRecoveryCategory || !spellInfo->StartRecoveryTime || reduction.count() <= 0)
        return;

    uint32 const remainingMs = GetRemainingGlobalCooldown(spellInfo);
    if (!remainingMs)
        return;

    auto gcdItr = _globalCooldowns.find(spellInfo->StartRecoveryCategory);
    if (gcdItr == _globalCooldowns.end())
        return;

    uint32 const newRemainingMs = reduction.count() >= remainingMs ? 0u : remainingMs - reduction.count();

    if (!newRemainingMs)
    {
        _globalCooldowns.erase(gcdItr);
    }
    else
    {
        Clock::duration const reductionDuration = std::chrono::duration_cast<Clock::duration>(reduction);
        gcdItr->second -= reductionDuration;
    }

    Player* player = GetPlayerOwner();
    if (!player)
        return;

    std::vector<uint32> eligibleSpells;
    eligibleSpells.reserve(8);

    auto tryAddSpell = [this, spellInfo, &eligibleSpells](SpellInfo const* otherInfo)
    {
        if (!otherInfo)
            return;

        if (otherInfo->StartRecoveryCategory != spellInfo->StartRecoveryCategory)
            return;

        if (!otherInfo->StartRecoveryTime)
            return;

        uint32 const spellId = otherInfo->Id;

        if (std::find(eligibleSpells.begin(), eligibleSpells.end(), spellId) != eligibleSpells.end())
            return;

        auto const spellCooldown = _spellCooldowns.find(spellId);
        if (spellCooldown != _spellCooldowns.end() && spellCooldown->second.CooldownEnd > GameTime::GetSystemTime())
            return;

        if (uint32 categoryId = otherInfo->GetCategory())
        {
            auto const categoryCooldown = _categoryCooldowns.find(categoryId);
            if (categoryCooldown != _categoryCooldowns.end())
            {
                if (CooldownEntry const* entry = categoryCooldown->second)
                    if (entry->CategoryEnd > GameTime::GetSystemTime())
                        return;
            }
        }

        eligibleSpells.push_back(spellId);
    };

    tryAddSpell(spellInfo);

    for (auto const& spellPair : player->GetSpellMap())
    {
        if (spellPair.second.state == PLAYERSPELL_REMOVED)
            continue;

        tryAddSpell(sSpellMgr->GetSpellInfo(spellPair.first));
    }

    if (eligibleSpells.empty())
        return;

    if (!newRemainingMs)
    {
        std::vector<int32> clearedCooldowns;
        clearedCooldowns.reserve(eligibleSpells.size());
        for (uint32 spellId : eligibleSpells)
            clearedCooldowns.push_back(int32(spellId));

        SendClearCooldowns(clearedCooldowns);
        return;
    }

    PacketCooldowns gcdUpdates;
    gcdUpdates.reserve(eligibleSpells.size());
    for (uint32 spellId : eligibleSpells)
        gcdUpdates.emplace(spellId, newRemainingMs);

    WorldPacket data;
    BuildCooldownPacket(data, SPELL_COOLDOWN_FLAG_INCLUDE_GCD, gcdUpdates);
    player->SendDirectMessage(&data);
}

void SpellHistory::ClampGlobalCooldown(SpellInfo const* spellInfo, std::chrono::milliseconds duration)
{
    if (!spellInfo || !spellInfo->StartRecoveryCategory || duration.count() <= 0)
        return;

    auto gcdItr = _globalCooldowns.find(spellInfo->StartRecoveryCategory);
    if (gcdItr == _globalCooldowns.end())
        return;

    Clock::time_point now = GameTime::GetSystemTime();
    Clock::time_point currentEnd = gcdItr->second;
    if (currentEnd <= now)
        return;

    Clock::time_point desiredEnd = now + std::chrono::duration_cast<Clock::duration>(duration);
    if (currentEnd > desiredEnd)
    {
        auto reduction = std::chrono::duration_cast<std::chrono::milliseconds>(currentEnd - desiredEnd);
        ReduceGlobalCooldown(spellInfo, reduction);
    }
}

Player* SpellHistory::GetPlayerOwner() const
{
    return _owner->GetCharmerOrOwnerPlayerOrPlayerItself();
}

void SpellHistory::SendClearCooldowns(std::vector<int32> const& cooldowns) const
{
    if (Player* playerOwner = GetPlayerOwner())
    {
        for (int32 spell : cooldowns)
        {
            if (SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(uint32(spell)))
            {
                if (spellInfo->GetCategory())
                {
                    WorldPacket event(SMSG_COOLDOWN_EVENT, 4 + 8);
                    event << uint32(spell);
                    event << uint64(_owner->GetGUID());
                    playerOwner->SendDirectMessage(&event);
                }
            }

            WorldPacket data(SMSG_CLEAR_COOLDOWN, 4 + 8);
            data << uint32(spell);
            data << uint64(_owner->GetGUID());
            playerOwner->SendDirectMessage(&data);
        }
    }
}

void SpellHistory::BuildCooldownPacket(WorldPacket& data, uint8 flags, uint32 spellId, uint32 cooldown) const
{
    data.Initialize(SMSG_SPELL_COOLDOWN, 8 + 1 + 4 + 4);
    data << uint64(_owner->GetGUID());
    data << uint8(flags);
    data << uint32(spellId);
    data << uint32(cooldown);
}

void SpellHistory::BuildCooldownPacket(WorldPacket& data, uint8 flags, PacketCooldowns const& cooldowns) const
{
    data.Initialize(SMSG_SPELL_COOLDOWN, 8 + 1 + (4 + 4) * cooldowns.size());
    data << uint64(_owner->GetGUID());
    data << uint8(flags);
    for (auto const& cooldown : cooldowns)
    {
        data << cooldown.first;
        data << cooldown.second;
    }
}

void SpellHistory::GetCooldownDurations(SpellInfo const* spellInfo, uint32 itemId, int32* cooldown, uint32* categoryId, int32* categoryCooldown)
{
    ASSERT(cooldown || categoryId || categoryCooldown);
    int32 tmpCooldown = -1;
    uint32 tmpCategoryId = 0;
    int32 tmpCategoryCooldown = -1;

    // some special item spells without correct cooldown in SpellInfo
    // cooldown information stored in item prototype
    if (itemId)
    {
        if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId))
        {
            for (uint8 idx = 0; idx < MAX_ITEM_PROTO_SPELLS; ++idx)
            {
                if (uint32(proto->Spells[idx].SpellId) == spellInfo->Id)
                {
                    tmpCooldown = proto->Spells[idx].SpellCooldown;
                    tmpCategoryId = proto->Spells[idx].SpellCategory;
                    tmpCategoryCooldown = proto->Spells[idx].SpellCategoryCooldown;
                    break;
                }
            }
        }
    }

    // if no cooldown found above then base at DBC data
    if (tmpCooldown < 0 && tmpCategoryCooldown < 0)
    {
        tmpCooldown = spellInfo->RecoveryTime;
        tmpCategoryId = spellInfo->GetCategory();
        tmpCategoryCooldown = spellInfo->CategoryRecoveryTime;
    }

    if (cooldown)
        *cooldown = tmpCooldown;
    if (categoryId)
        *categoryId = tmpCategoryId;
    if (categoryCooldown)
        *categoryCooldown = tmpCategoryCooldown;
}

void SpellHistory::SaveCooldownStateBeforeDuel()
{
    _spellCooldownsBeforeDuel = _spellCooldowns;
}

void SpellHistory::RestoreCooldownStateAfterDuel()
{
    // category cooldows are not preserved.
    if (Player* player = _owner->ToPlayer())
    {
        // add all profession CDs created while in duel (if any)
        for (auto itr = _spellCooldowns.begin(); itr != _spellCooldowns.end(); ++itr)
        {
            SpellInfo const* spellInfo = sSpellMgr->AssertSpellInfo(itr->first);

            if (spellInfo->RecoveryTime > 10 * MINUTE * IN_MILLISECONDS ||
                spellInfo->CategoryRecoveryTime > 10 * MINUTE * IN_MILLISECONDS)
                _spellCooldownsBeforeDuel[itr->first] = _spellCooldowns[itr->first];
        }

        // check for spell with onHold active before and during the duel
        for (auto itr = _spellCooldownsBeforeDuel.begin(); itr != _spellCooldownsBeforeDuel.end(); ++itr)
        {
            if (!itr->second.OnHold && !_spellCooldowns[itr->first].OnHold)
                _spellCooldowns[itr->first] = _spellCooldownsBeforeDuel[itr->first];
        }

        // update the client: restore old cooldowns
        PacketCooldowns cooldowns;

        for (auto itr = _spellCooldowns.begin(); itr != _spellCooldowns.end(); ++itr)
        {
            Clock::time_point now = GameTime::GetSystemTime();
            uint32 cooldownDuration = itr->second.CooldownEnd > now ? std::chrono::duration_cast<std::chrono::milliseconds>(itr->second.CooldownEnd - now).count() : 0;

            // cooldownDuration must be between 0 and 10 minutes in order to avoid any visual bugs
            if (cooldownDuration <= 0 || cooldownDuration > 10 * MINUTE * IN_MILLISECONDS || itr->second.OnHold)
                continue;

            cooldowns[itr->first] = cooldownDuration;
        }

        WorldPacket data;
        BuildCooldownPacket(data, SPELL_COOLDOWN_FLAG_INCLUDE_EVENT_COOLDOWNS, cooldowns);
        player->SendDirectMessage(&data);
    }
}

template void SpellHistory::LoadFromDB<Player>(PreparedQueryResult cooldownsResult);
template void SpellHistory::LoadFromDB<Pet>(PreparedQueryResult cooldownsResult);
template void SpellHistory::SaveToDB<Player>(CharacterDatabaseTransaction trans);
template void SpellHistory::SaveToDB<Pet>(CharacterDatabaseTransaction trans);
