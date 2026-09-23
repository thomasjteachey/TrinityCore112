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

#include "RaceChange.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "SharedDefines.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TournamentMode.h"
#include "World.h"
#include <array>
#include <set>
#include <string>

namespace
{
    // The priest racials. Each race's pair is a quest reward at 10 and 20, on a
    // skill line every race can use, so SkillLineAbility cannot say whose they
    // are. The mapping is the one 2026_09_03_03_characters_repair_priest_racials
    // settled, read off what each quest rewards and cross-checked against the
    // priest block in Legionnaire+'s createCopyOfChar:
    //
    //   race       level 10                   level 20
    //   Human      Desperate Prayer   13908   Chastise          81350
    //   Dwarf      Abolish Magic      81349   Fear Ward          6346
    //   Night Elf  Wisp Form          81352   Elune's Grace     81351
    //   Undead     Wyrm's Shadow      81357   Devouring Curse    2944
    //   Troll      Hex of Weakness     9035   Shadowguard       18137
    //
    // Ranks are listed in full rather than walked through spell_ranks, because
    // Desperate Prayer has no chain there. The quests are the ones that hand the
    // ability out; Night Elves have two for Wisp Form, in one exclusive group.
    struct RacialAbility
    {
        uint8 Race;
        uint8 Class;
        uint8 Level;
        std::array<uint32, 2> Quests;
        std::array<uint32, 7> Ranks;
    };

    constexpr RacialAbility RacialAbilities[] =
    {
        { RACE_HUMAN,         CLASS_PRIEST, 10, { 5635, 0    }, { 13908, 19236, 19238, 19240, 19241, 19242, 19243 } }, // Desperate Prayer
        { RACE_HUMAN,         CLASS_PRIEST, 20, { 5676, 0    }, { 81350 } },                                        // Chastise
        { RACE_DWARF,         CLASS_PRIEST, 10, { 5637, 0    }, { 81349 } },                                        // Abolish Magic
        { RACE_DWARF,         CLASS_PRIEST, 20, { 5645, 0    }, { 6346 } },                                         // Fear Ward
        { RACE_NIGHTELF,      CLASS_PRIEST, 10, { 5627, 5629 }, { 81352 } },                                        // Wisp Form
        { RACE_NIGHTELF,      CLASS_PRIEST, 20, { 5674, 0    }, { 81351 } },                                        // Elune's Grace
        { RACE_UNDEAD_PLAYER, CLASS_PRIEST, 10, { 5658, 0    }, { 81357 } },                                        // Wyrm's Shadow
        { RACE_UNDEAD_PLAYER, CLASS_PRIEST, 20, { 5679, 0    }, { 2944, 19276, 19277, 19278, 19279, 19280 } },      // Devouring Curse
        { RACE_TROLL,         CLASS_PRIEST, 10, { 5652, 0    }, { 9035, 19281, 19282, 19283, 19284, 19285 } },      // Hex of Weakness
        { RACE_TROLL,         CLASS_PRIEST, 20, { 5680, 0    }, { 18137, 19308, 19309, 19310, 19311, 19312 } },     // Shadowguard
    };

    // The ranks of one racial that `level` reaches: the first at the quest's
    // level, every later one at its own spell level.
    void InsertRanksAtLevel(RacialAbility const& ability, uint8 level, std::set<uint32>& out)
    {
        for (std::size_t i = 0; i < ability.Ranks.size() && ability.Ranks[i]; ++i)
        {
            uint32 const spellId = ability.Ranks[i];
            SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
            if (!spellInfo)
                continue;

            uint32 const requiredLevel = i == 0 ? ability.Level : spellInfo->SpellLevel;
            if (level >= requiredLevel)
                out.insert(spellId);
        }
    }

    // Whether SkillLineAbility lets this race and class have the spell at all.
    // A spell with no row is nobody's racial.
    bool FitsRace(uint32 spellId, uint32 raceMask, uint32 classMask)
    {
        SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
        if (bounds.first == bounds.second)
            return true;

        for (auto itr = bounds.first; itr != bounds.second; ++itr)
        {
            SkillLineAbilityEntry const* ability = itr->second;
            if (ability->ClassMask && !(ability->ClassMask & classMask))
                continue;
            if (ability->RaceMask && !(ability->RaceMask & raceMask))
                continue;
            return true;
        }

        return false;
    }

    // The list LearnCustomSpells teaches at every login, for a race.
    std::vector<uint32> const* StartSpells(uint8 race, uint8 playerClass, bool tournament)
    {
        if (tournament)
        {
            Tournament::CreateInfo const* info = Tournament::GetCreateInfo(race, playerClass);
            return (info && info->HasCustomSpells) ? &info->CustomSpells : nullptr;
        }

        if (!sWorld->getBoolConfig(CONFIG_START_ALL_SPELLS))
            return nullptr;

        PlayerInfo const* info = sObjectMgr->GetPlayerInfo(race, playerClass);
        return info ? &info->customSpells : nullptr;
    }

    std::string Join(std::set<uint32> const& ids)
    {
        std::string out;
        for (uint32 id : ids)
        {
            if (!out.empty())
                out += ',';
            out += std::to_string(id);
        }
        return out;
    }
}

void RaceChange::AppendSpellSwap(CharacterDatabaseTransaction trans, ObjectGuid guid, uint8 oldRace, uint8 newRace,
    uint8 playerClass, uint8 level, bool tournament)
{
    if (oldRace == newRace || !oldRace || !newRace || !playerClass)
        return;

    uint32 const lowGuid = guid.GetCounter();
    uint32 const oldRaceMask = 1u << (oldRace - 1);
    uint32 const newRaceMask = 1u << (newRace - 1);
    uint32 const classMask = 1u << (playerClass - 1);

    std::set<uint32> drop;
    std::set<uint32> grant;
    std::set<uint32> dropSkills;
    std::set<uint32> forgetQuests;
    std::set<uint32> rewardQuests;

    // 1. SkillLineAbility rows that were the old race's and are not the new
    //    race's. Rows are read per spell, so a spell another row still grants
    //    the new race (the five Shadowmeld Passive rows, the language rows) is
    //    kept.
    for (SkillLineAbilityEntry const* ability : sSkillLineAbilityStore)
    {
        if (!ability->RaceMask || !(ability->RaceMask & oldRaceMask) || (ability->RaceMask & newRaceMask))
            continue;
        if (ability->ClassMask && !(ability->ClassMask & classMask))
            continue;
        if (FitsRace(ability->Spell, newRaceMask, classMask))
            continue;

        drop.insert(ability->Spell);
        if (!GetSkillRaceClassInfo(ability->SkillLine, newRace, playerClass))
            dropSkills.insert(ability->SkillLine);
    }

    // The engineering mounts and the like have a counterpart for the other
    // side in player_factionchange_spells. Every race is one team here, so the
    // stock faction change never runs; trade them the same way instead.
    for (uint32 spellId : std::set<uint32>(drop))
    {
        uint32 counterpart = 0;
        for (auto const& [allianceSpell, hordeSpell] : sObjectMgr->FactionChangeSpells)
        {
            if (allianceSpell == spellId)
                counterpart = hordeSpell;
            else if (hordeSpell == spellId)
                counterpart = allianceSpell;
            if (counterpart)
                break;
        }

        if (counterpart && !drop.count(counterpart) && FitsRace(counterpart, newRaceMask, classMask))
            grant.insert(counterpart);
    }

    // 2. The login start list, where the two races' lists differ. The new
    //    race's own list is taught at the next login by LearnCustomSpells.
    if (std::vector<uint32> const* oldStart = StartSpells(oldRace, playerClass, tournament))
    {
        std::vector<uint32> const* newStart = StartSpells(newRace, playerClass, tournament);
        std::set<uint32> const keep = newStart ? std::set<uint32>(newStart->begin(), newStart->end()) : std::set<uint32>();
        for (uint32 spellId : *oldStart)
            if (!keep.count(spellId))
                drop.insert(spellId);
    }

    // 3. The priest racials. Everybody else's are taken, not just the old
    //    race's: the September repair found priests holding three races' worth,
    //    and a race change is the moment to leave them holding exactly their own.
    for (RacialAbility const& ability : RacialAbilities)
    {
        if (ability.Class != playerClass)
            continue;

        if (ability.Race != newRace)
        {
            for (uint32 spellId : ability.Ranks)
                if (spellId)
                    drop.insert(spellId);
            for (uint32 questId : ability.Quests)
                if (questId)
                    forgetQuests.insert(questId);
            continue;
        }

        if (level < ability.Level)
            continue;

        // Every rank the level reaches: the old race's ranks were paid for,
        // and there is no trainer here that sells these.
        InsertRanksAtLevel(ability, level, grant);

        for (uint32 questId : ability.Quests)
            if (questId)
                rewardQuests.insert(questId);
    }

    for (uint32 spellId : grant)
        drop.erase(spellId);

    if (!drop.empty())
    {
        std::string const ids = Join(drop);
        trans->PAppend("DELETE FROM `character_spell` WHERE `guid` = {} AND `spell` IN ({})", lowGuid, ids);
        // An old racial left on a bar is a red, dead button.
        trans->PAppend("DELETE FROM `character_action` WHERE `guid` = {} AND `type` = 0 AND `action` IN ({})", lowGuid, ids);
    }

    // _LoadSkills would drop these as forbidden at the next login anyway; doing
    // it here keeps the error out of the log.
    if (!dropSkills.empty())
        trans->PAppend("DELETE FROM `character_skills` WHERE `guid` = {} AND `skill` IN ({})", lowGuid, Join(dropSkills));

    for (uint32 spellId : grant)
        trans->PAppend("INSERT IGNORE INTO `character_spell` (`guid`, `spell`, `active`, `disabled`) VALUES ({}, {}, 1, 0)", lowGuid, spellId);

    if (!forgetQuests.empty())
    {
        std::string const ids = Join(forgetQuests);
        trans->PAppend("DELETE FROM `character_queststatus_rewarded` WHERE `guid` = {} AND `quest` IN ({})", lowGuid, ids);
        trans->PAppend("DELETE FROM `character_queststatus` WHERE `guid` = {} AND `quest` IN ({})", lowGuid, ids);
    }

    // Done, so the new race's quest is not offered for an ability already held,
    // and so its exclusive group stays closed.
    for (uint32 questId : rewardQuests)
    {
        trans->PAppend("DELETE FROM `character_queststatus` WHERE `guid` = {} AND `quest` = {}", lowGuid, questId);
        trans->PAppend("INSERT IGNORE INTO `character_queststatus_rewarded` (`guid`, `quest`, `active`) VALUES ({}, {}, 1)", lowGuid, questId);
    }

    TC_LOG_INFO("entities.player", "Race change {}: race {} -> {}, class {}, level {}{}: {} spell(s) removed, {} granted, {} skill line(s) dropped.",
        guid.ToString(), oldRace, newRace, playerClass, level, tournament ? " (tournament)" : "", drop.size(), grant.size(), dropSkills.size());
}

std::set<uint32> RaceChange::RacialAbilitiesAtLevel(uint8 race, uint8 playerClass, uint8 level)
{
    std::set<uint32> spells;
    for (RacialAbility const& ability : RacialAbilities)
        if (ability.Race == race && ability.Class == playerClass && level >= ability.Level)
            InsertRanksAtLevel(ability, level, spells);
    return spells;
}
