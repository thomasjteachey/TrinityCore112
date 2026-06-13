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

#include "Trainer.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "NPCPackets.h"
#include "Player.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldSession.h"

namespace Trainer
{
namespace
{
    struct ClassicPetTrainerTemplateRow
    {
        uint32 SourceSpell = 0;
        bool TrainerTaught = false;
        bool Enabled = false;
    };

    std::vector<ClassicPetTrainerTemplateRow> const& GetClassicPetTrainerTemplateRows()
    {
        static bool loaded = false;
        static std::vector<ClassicPetTrainerTemplateRow> rows;

        if (loaded)
            return rows;

        loaded = true;
        rows.clear();

        QueryResult result = WorldDatabase.PQuery("SELECT source_spell, trainer_taught, enabled FROM classic_pet_training_template");
        if (!result)
            return rows;

        do
        {
            Field* fields = result->Fetch();

            ClassicPetTrainerTemplateRow row;
            row.SourceSpell = fields[0].GetUInt32();
            row.TrainerTaught = fields[1].GetUInt8() != 0;
            row.Enabled = fields[2].GetUInt8() != 0;
            rows.push_back(row);
        }
        while (result->NextRow());

        return rows;
    }

    bool IsClassicPetTrainingSourceSpellEnabledForTrainer(uint32 spellId)
    {
        for (ClassicPetTrainerTemplateRow const& row : GetClassicPetTrainerTemplateRows())
            if (row.SourceSpell == spellId && row.Enabled && row.TrainerTaught)
                return true;

        return false;
    }

    bool IsClassicPetTrainingSourceSpell(uint32 spellId)
    {
        if (spellId == 5149) // Beast Training opener itself, not a trainer-taught recipe.
            return false;

        // Pet trainers may only use source spells that are explicitly enabled
        // and trainer_taught in classic_pet_training_template.  Disabled rows
        // must fall through as non-Classic trainer spells.
        if (!IsClassicPetTrainingSourceSpellEnabledForTrainer(spellId))
            return false;

        SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId);
        if (!spellInfo)
            return false;

        bool teachesPetSpell = false;
        for (SpellEffectInfo const& effect : spellInfo->GetEffects())
        {
            if ((effect.IsEffect(SPELL_EFFECT_LEARN_SPELL) || effect.IsEffect(SPELL_EFFECT_LEARN_PET_SPELL)) && effect.TriggerSpell)
            {
                teachesPetSpell = true;
                break;
            }
        }

        if (!teachesPetSpell)
            return false;

        SkillLineAbilityMapBounds bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellId);
        for (SkillLineAbilityMap::const_iterator itr = bounds.first; itr != bounds.second; ++itr)
            if (itr->second && itr->second->SkillLine == 261)
                return true;

        return false;
    }
}

    bool Spell::IsCastable() const
    {
        return sSpellMgr->AssertSpellInfo(SpellId)->HasEffect(SPELL_EFFECT_LEARN_SPELL);
    }

    Trainer::Trainer(uint32 trainerId, Type type, uint32 requirement, std::string greeting, std::vector<Spell> spells) : _trainerId(trainerId), _type(type), _requirement(requirement), _spells(std::move(spells))
    {
        _greeting[DEFAULT_LOCALE] = std::move(greeting);
    }

    void Trainer::SendSpells(Creature const* npc, Player const* player, LocaleConstant locale) const
    {
        float reputationDiscount = player->GetReputationPriceDiscount(npc);

        WorldPackets::NPC::TrainerList trainerList;
        trainerList.TrainerGUID = npc->GetGUID();
        trainerList.TrainerType = AsUnderlyingType(_type);
        trainerList.Greeting = GetGreeting(locale);
        trainerList.Spells.reserve(_spells.size());
        for (Spell const& trainerSpell : _spells)
        {
            if (!player->IsSpellFitByClassAndRace(trainerSpell.SpellId))
                continue;

            SpellInfo const* trainerSpellInfo = sSpellMgr->AssertSpellInfo(trainerSpell.SpellId);

            bool primaryProfessionFirstRank = false;
            for (SpellEffectInfo const& spellEffectInfo : trainerSpellInfo->GetEffects())
            {
                if (!spellEffectInfo.IsEffect(SPELL_EFFECT_LEARN_SPELL))
                    continue;

                SpellInfo const* learnedSpellInfo = sSpellMgr->GetSpellInfo(spellEffectInfo.TriggerSpell);
                if (learnedSpellInfo && learnedSpellInfo->IsPrimaryProfessionFirstRank())
                    primaryProfessionFirstRank = true;
            }

            trainerList.Spells.emplace_back();
            WorldPackets::NPC::TrainerListSpell& trainerListSpell = trainerList.Spells.back();
            trainerListSpell.SpellID = trainerSpell.SpellId;
            trainerListSpell.Usable = AsUnderlyingType(GetSpellState(player, &trainerSpell));
            trainerListSpell.MoneyCost = int32(trainerSpell.MoneyCost * reputationDiscount);
            trainerListSpell.PointCost[0] = 0; // spells don't cost talent points
            trainerListSpell.PointCost[1] = (primaryProfessionFirstRank ? 1 : 0);
            trainerListSpell.ReqLevel = trainerSpell.ReqLevel;
            trainerListSpell.ReqSkillLine = trainerSpell.ReqSkillLine;
            trainerListSpell.ReqSkillRank = trainerSpell.ReqSkillRank;
            std::copy(trainerSpell.ReqAbility.begin(), trainerSpell.ReqAbility.end(), trainerListSpell.ReqAbility.begin());
        }

        player->SendDirectMessage(trainerList.Write());
    }

    void Trainer::TeachSpell(Creature const* npc, Player* player, uint32 spellId) const
    {
        if (!IsTrainerValidForPlayer(player))
            return;

        Spell const* trainerSpell = GetSpell(spellId);
        if (!trainerSpell)
        {
            SendTeachFailure(npc, player, spellId, FailReason::Unavailable);
            return;
        }

        if (!CanTeachSpell(player, trainerSpell))
        {
            SendTeachFailure(npc, player, spellId, FailReason::NotEnoughSkill);
            return;
        }

        float reputationDiscount = player->GetReputationPriceDiscount(npc);
        int32 moneyCost = int32(trainerSpell->MoneyCost * reputationDiscount);
        if (!player->HasEnoughMoney(moneyCost))
        {
            SendTeachFailure(npc, player, spellId, FailReason::NotEnoughMoney);
            return;
        }

        player->ModifyMoney(-moneyCost);

        npc->SendPlaySpellVisual(179);
        npc->SendPlaySpellImpact(player->GetGUID(), 362);

        // Classic Beast Training trainer rows are source/teaching spells.
        // Buying them should only add the source spell to the hunter's known
        // Beast Training catalog.  Do NOT cast the source spell here, because
        // its LEARN_SPELL / LEARN_PET_SPELL effect would immediately teach
        // the current pet and skip the Beast Training frame step.
        if (IsClassicPetTrainingSourceSpell(trainerSpell->SpellId))
            player->LearnSpell(trainerSpell->SpellId, false);
        // learn explicitly or cast explicitly
        else if (trainerSpell->IsCastable())
            player->CastSpell(player, trainerSpell->SpellId, true);
        else
            player->LearnSpell(trainerSpell->SpellId, false);

        SendTeachSucceeded(npc, player, spellId);

        // The 3.3.5 client mutates trainer-list colors locally after a successful
        // purchase. With mixed Classic/Wrath trainer data, that local refresh can
        // mark unavailable ranks as green until the window is reopened. Re-send the
        // authoritative trainer list immediately so every row is recalculated by
        // GetSpellState() after each purchase.
        SendSpells(npc, player, player->GetSession()->GetSessionDbLocaleIndex());
    }

    Spell const* Trainer::GetSpell(uint32 spellId) const
    {
        auto itr = std::find_if(_spells.begin(), _spells.end(), [spellId](Spell const& trainerSpell)
        {
            return trainerSpell.SpellId == spellId;
        });

        if (itr != _spells.end())
            return &(*itr);

        return nullptr;
    }

    bool Trainer::CanTeachSpell(Player const* player, Spell const* trainerSpell) const
    {
        SpellState state = GetSpellState(player, trainerSpell);
        if (state != SpellState::Available)
            return false;

        SpellInfo const* trainerSpellInfo = sSpellMgr->AssertSpellInfo(trainerSpell->SpellId);

        for (SpellEffectInfo const& spellEffectInfo : trainerSpellInfo->GetEffects())
        {
            if (!spellEffectInfo.IsEffect(SPELL_EFFECT_LEARN_SPELL))
                continue;

            SpellInfo const* learnedSpellInfo = sSpellMgr->GetSpellInfo(spellEffectInfo.TriggerSpell);
            if (learnedSpellInfo && learnedSpellInfo->IsPrimaryProfessionFirstRank() && !player->GetFreePrimaryProfessionPoints())
                return false;
        }

        return true;
    }

    SpellState Trainer::GetSpellState(Player const* player, Spell const* trainerSpell) const
    {
        if (player->HasSpell(trainerSpell->SpellId))
            return SpellState::Known;

        // check race/class requirement
        if (!player->IsSpellFitByClassAndRace(trainerSpell->SpellId))
            return SpellState::Unavailable;

        // check skill requirement
        if (trainerSpell->ReqSkillLine && player->GetBaseSkillValue(trainerSpell->ReqSkillLine) < trainerSpell->ReqSkillRank)
            return SpellState::Unavailable;

        for (int32 reqAbility : trainerSpell->ReqAbility)
            if (reqAbility && !player->HasSpell(reqAbility))
                return SpellState::Unavailable;

        // check level requirement
        if (player->GetLevel() < trainerSpell->ReqLevel)
            return SpellState::Unavailable;

        // Classic Beast Training trainer rows teach the hunter a source/catalog spell.
        // Do not apply normal LEARN_SPELL rank validation here: these source spells
        // trigger pet spells, and the player is not supposed to know pet spell ranks.
        // Rank prerequisites for these rows must come from trainer_spell.ReqAbility*.
        if (IsClassicPetTrainingSourceSpell(trainerSpell->SpellId))
            return SpellState::Available;

        // check ranks
        bool hasLearnSpellEffect = false;
        bool knowsAllLearnedSpells = true;
        for (SpellEffectInfo const& spellEffectInfo : sSpellMgr->AssertSpellInfo(trainerSpell->SpellId)->GetEffects())
        {
            if (!spellEffectInfo.IsEffect(SPELL_EFFECT_LEARN_SPELL))
                continue;

            hasLearnSpellEffect = true;
            if (!player->HasSpell(spellEffectInfo.TriggerSpell))
                knowsAllLearnedSpells = false;

            if (uint32 previousRankSpellId = sSpellMgr->GetPrevSpellInChain(spellEffectInfo.TriggerSpell))
                if (!player->HasSpell(previousRankSpellId))
                    return SpellState::Unavailable;
        }

        if (!hasLearnSpellEffect)
        {
            if (uint32 previousRankSpellId = sSpellMgr->GetPrevSpellInChain(trainerSpell->SpellId))
                if (!player->HasSpell(previousRankSpellId))
                    return SpellState::Unavailable;
        }
        else if (knowsAllLearnedSpells)
            return SpellState::Known;

        // check additional spell requirement
        for (auto const& requirePair : sSpellMgr->GetSpellsRequiredForSpellBounds(trainerSpell->SpellId))
            if (!player->HasSpell(requirePair.second))
                return SpellState::Unavailable;

        return SpellState::Available;
    }

    bool Trainer::IsTrainerValidForPlayer(Player const* player) const
    {
        if (!GetTrainerRequirement())
            return true;

        switch (GetTrainerType())
        {
            case Type::Class:
            case Type::Pet:
                // check class for class trainers
                return player->GetClass() == GetTrainerRequirement();
            case Type::Mount:
                // check race for mount trainers
                return player->GetRace() == GetTrainerRequirement();
            case Type::Tradeskill:
                // check spell for profession trainers
                return player->HasSpell(GetTrainerRequirement());
            default:
                break;
        }

        return true;
    }

    void Trainer::SendTeachFailure(Creature const* npc, Player const* player, uint32 spellId, FailReason reason) const
    {
        WorldPackets::NPC::TrainerBuyFailed trainerBuyFailed;
        trainerBuyFailed.TrainerGUID = npc->GetGUID();
        trainerBuyFailed.SpellID = spellId;
        trainerBuyFailed.TrainerFailedReason = AsUnderlyingType(reason);
        player->SendDirectMessage(trainerBuyFailed.Write());
    }

    void Trainer::SendTeachSucceeded(Creature const* npc, Player const* player, uint32 spellId) const
    {
        WorldPackets::NPC::TrainerBuySucceeded trainerBuySucceeded;
        trainerBuySucceeded.TrainerGUID = npc->GetGUID();
        trainerBuySucceeded.SpellID = spellId;
        player->SendDirectMessage(trainerBuySucceeded.Write());
    }

    std::string const& Trainer::GetGreeting(LocaleConstant locale) const
    {
        if (_greeting[locale].empty())
            return _greeting[DEFAULT_LOCALE];

        return _greeting[locale];
    }

    void Trainer::AddGreetingLocale(LocaleConstant locale, std::string greeting)
    {
        _greeting[locale] = std::move(greeting);
    }
}
