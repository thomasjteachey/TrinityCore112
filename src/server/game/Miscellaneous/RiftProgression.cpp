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

#include "RiftProgression.h"
#include "RiftLeague.h"

#include "Battleground.h"
#include "BattlegroundSRF.h"
#include "Config.h"
#include "Chat.h"
#include "WorldPacket.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "GameTime.h"
#include "Bag.h"
#include "Item.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Pet.h"
#include "Miscellaneous/TournamentMode.h"
#include "Player.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Util.h"
#include "WorldSession.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    // League's starting purse, and its gold to the character's money: one League
    // gold is one gold, so prices and the "+N gold" popups read as the bags do.
    uint32 constexpr START_GOLD = 475;
    uint32 constexpr COPPER_PER_LEAGUE_GOLD = GOLD;

    // The match starts at level 10, where the first talent point comes, and
    // climbs to the character's real level.
    uint8 constexpr START_LEVEL = 10;

    // League's recall: Centurion spell 94120 "Recall" (8 s cast, usable in
    // combat, broken by moving or taking damage; custom_rift_progression.cpp
    // sends the caster to its fountain). Known only for the match.
    uint32 constexpr SPELL_RIFT_RECALL = 94120;
    uint32 constexpr SPELL_RIFT_FOCUS = 94153;   // the match aura BattlegroundSRF keeps on
    // Valor's Reward, a League item passive (scripts/Custom/custom_rift_items.cpp).
    uint32 constexpr SPELL_VALORS_REWARD = 94141;
    // Stock 46699 "Requires No Ammo" (Thori'dal's passive). The match no longer
    // hands it out - it also stops ammunition being equipped; Spell::TakeAmmo
    // just uses none up in the Rift - but a character that left a match with
    // the match's copy loses it here.
    uint32 constexpr SPELL_NO_AMMO = 46699;

    void TakeNoAmmo(Player* player)
    {
        // only a copy the match put on: Thori'dal's (an item aura) stays
        player->RemoveAppliedAuras(SPELL_NO_AMMO, [](AuraApplication const* aurApp)
        {
            return aurApp->GetBase()->GetCastItemGUID().IsEmpty();
        });
    }

    // The fountain shops' League items (import/rift/scripts/rift_shop.py): bought
    // for the match, gone with it.
    uint32 constexpr SHOP_ITEM_FIRST = 991000;
    uint32 constexpr SHOP_ITEM_LAST = 991299;     // 2026-10-22: every League S3 item (991099 before)

    // League's build paths (world `rift_item_recipe`): an item's components.
    std::unordered_map<uint32, std::vector<uint32>> Recipes;

    // What `entry` costs given the components left in `pool`: its price less its
    // components' prices (the combine cost), plus each component the character
    // does not hold, itself priced this way. Held components leave the pool.
    uint32 RecipeCost(uint32 entry, std::unordered_map<uint32, uint32>& pool, std::vector<uint32>& consumed, uint8 depth = 0)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        uint32 const full = proto ? uint32(std::max<int32>(proto->BuyPrice, 0)) : 0;
        auto const itr = Recipes.find(entry);
        if (itr == Recipes.end() || depth > 4)
            return full;
        uint32 parts = 0;
        for (uint32 component : itr->second)
            if (ItemTemplate const* cp = sObjectMgr->GetItemTemplate(component))
                parts += uint32(std::max<int32>(cp->BuyPrice, 0));
        uint32 cost = full > parts ? full - parts : 0;
        for (uint32 component : itr->second)
        {
            auto const held = pool.find(component);
            if (held != pool.end() && held->second)
            {
                --held->second;
                consumed.push_back(component);
            }
            else
                cost += RecipeCost(component, pool, consumed, depth + 1);
        }
        return cost;
    }

    void TakeShopItems(Player* player)
    {
        for (uint32 entry = SHOP_ITEM_FIRST; entry <= SHOP_ITEM_LAST; ++entry)
            if (uint32 const count = player->GetItemCount(entry, true))
                player->DestroyItemCount(entry, count, true, false);
    }

    // League's experience curve (Season 3): from level n to n+1 takes 180 + 100n,
    // and 18 is the cap. The character's real level is reached at League 18.
    uint8 constexpr LEAGUE_MAX_LEVEL = 18;
    float XpToNext(uint8 leagueLevel) { return 180.0f + 100.0f * float(leagueLevel); }

    // The addon line holds at most this many offers; the rest wait their turn.
    size_t constexpr MAX_OFFERS_SENT = 30;

    struct Chain
    {
        std::vector<uint32> Ranks;   // the ranks the character knows, lowest first
        bool Talent = false;         // its first rank is a talent's (Trueshot Aura): Ranks are the trained ones after it
    };

    struct LearnedTalent
    {
        uint32 Id = 0;               // Talent.dbc row
        uint32 Rank = 0;             // 0-based, as Player::LearnTalent takes it
        uint32 Tier = 0;
    };

    struct SavedButton
    {
        uint8 Button = 0;
        uint32 Action = 0;
        uint8 Type = 0;
    };

    // Where a newly unlocked spell goes when its own button is taken: the main
    // bar, then the bottom-left, bottom-right, right and right-2 bars.
    uint8 constexpr BAR_ORDER[] = { 0, 60, 48, 24, 36 };

    // A held item's League aura: what it gives the champions near the holder (GatherAuras).
    struct AuraEmit
    {
        std::string Name;          // the League name, or "#<entry>" for an unnamed one
        RiftLeague::Unique Stat;
        float Value;
    };
    using UniqueStats = std::array<float, size_t(RiftLeague::Unique::Count)>;

    struct SavedSkill
    {
        uint16 Id = 0, Step = 0, Value = 0, Max = 0;
    };

    struct PetRank
    {
        uint32 Original = 0;     // the rank the pet really knows
        uint32 Current = 0;      // the rank it has in the match, 0 for none yet
        uint8 Active = 0;        // the original's ActiveStates, for its pet_spell row
    };

    struct State
    {
        uint8 RealLevel = 1;
        uint32 RealXp = 0;
        uint32 RealMoney = 0;
        bool Persistent = true;      // false for a transient copy: nothing is written down

        uint8 StartLevel = START_LEVEL;
        float LeagueXp = 0.0f;
        uint8 LeagueLevel = 1;
        uint8 MatchLevel = START_LEVEL;

        std::map<uint32 /*first rank*/, Chain> Chains;
        std::unordered_map<uint32 /*spell*/, uint32 /*first rank*/> SpellToChain;
        std::map<uint32 /*first rank*/, uint8 /*ranks unlocked*/> Unlocked;
        std::unordered_set<uint32> FreeChains;     // whitelisted free: usable from the start
        std::unordered_set<uint32> TalentKnown;    // talent chains whose talent is learned in the match
        std::unordered_set<uint32> Removed;        // class spells off the whitelist: out of the match
        uint32 PendingPicks = 0;

        std::vector<LearnedTalent> RealTalents;   // the active spec's build, put back on the way out
        std::vector<SavedButton> RealButtons;     // the action bars it walked in with
        std::unordered_set<uint8> Returned;       // its buttons standing again (RefillSavedButtons)
        Tournament::RiftWeapons Weapons;          // the armsmaster's choice for the kit's hands
        // Every spell it knew at the door, by lower-case name: what a macro on its
        // bars casts (MacroReady), read before the talents are put aside.
        std::unordered_map<std::string, std::vector<uint32>> SpellsByName;
        // The bars as PublishBars last sent them, and when it last filled each
        // button (KeepsButton).
        std::map<uint8, uint32> SentBars;
        std::map<uint8, uint32> FilledAt;
        bool WasAlive = true;                     // at the last SyncItems: a revive re-checks the kit
        uint32 SentSpeed = 0;                     // the run speed CENTURION_Rift was last told (RIFTSPEED)

        // What the client's spellbook was told against what it would hold
        // normally (every active spell): active spells taken out of it, and
        // inactive ones (a lower rank) put into it. SyncClientSpellbook.
        std::unordered_set<uint32> ClientHidden;
        std::unordered_set<uint32> ClientExtra;

        // The League inventory's on-equip spells now on the champion, by spell:
        // how many held items carry it (SyncItems).
        std::map<uint32, uint32> ItemSpells;
        // The field kit bracket the champion is dressed for (SyncItems swaps it).
        uint8 KitApplied = 0;
        // Riding as the match counts it (the steed held), last applied by SyncItems.
        uint16 Riding = 0;
        // The match's League -> WoW conversion (BattlegroundSRF, SetMatchScale):
        // its scale, and the level armour is read at.
        float MatchScale = 0.0f;
        uint8 CombatLevel = 0;
        // The Bloodthirster's stacks: a unit kill each, up to 30, half lost on death.
        uint8 BloodthirsterStacks = 0;
        // League's flat magic penetration, League's points (SetChampionCarriers).
        float MagicPen = 0.0f;
        // The magic resist the champion's shop items give, League's points: the only magic
        // resist that can make a no-damage spell miss (GearMagicResist).
        float GearResist = 0.0f;
        // League's own numbers for CENTURION_Rift's pane (RIFTSTATS): the line as
        // SetChampionCarriers last made it, and as the client was last sent it.
        std::string StatsLine;
        std::string SentStatsLine;
        // A rogue's poison coats the match taught (AddPoisonChains), as dependent
        // spells - never written down - and taken back on the way out.
        std::unordered_set<uint32> MatchPoisons;
        // A talent's trained ranks the character never trained, picked in the match
        // (HandlePickRequest): taught the same way, taken back the same way.
        std::unordered_set<uint32> MatchRanks;
        // A world character's level-ranged skills (weapons, defense, lockpicking) as they
        // were before the match raised them to its level's maximum (UsesMatchSkills), put
        // back on the way out (owner, 2026-10-23: "just like tournament characters in this
        // mode (but get their skills deranked again at the end)").
        std::vector<SavedSkill> RealSkills;
        // A hunter pet's abilities held at the rank its match level allows (SyncPetRanks),
        // by pet number and first rank: the rank it really knows, and the one it has now
        // (0: none yet at this level). Put back on the way out.
        std::map<uint32, std::map<uint32, PetRank>> PetRanks;
        // League's UNIQUE item effects (HeldItemEffects): the champion's own, the auras its
        // items give the champions near it, and the spell vamp it gets from its allies'
        // (SetChampionCarriers), for the hooks outside SyncItems.
        UniqueStats Unique = { };
        std::vector<AuraEmit> Emits;
        float ReceivedSpellVamp = 0.0f;
        float CooldownReduction = 0.0f;
        // The stacking items: Mejai's Soulstealer and Sword of the Occult (2 a kill, 1 an
        // assist, a third lost on death), Seeker's Armguard (one a unit killed).
        uint8 MejaiStacks = 0;
        uint8 OccultStacks = 0;
        uint8 SeekerStacks = 0;
        float GoldOwed = 0.0f;     // gold every 10 sec: the fractions a tick leaves
    };

    // Recursive: GiveLevel, called with the lock held, comes straight back through
    // InitTalentForLevel into SuppressTalentReset.
    std::recursive_mutex StateMutex;
    std::unordered_map<ObjectGuid::LowType, State> States;
    // Characters with a row in custom_rift_stash. Read at startup and kept with the
    // rows: a character that crashed out of a match loads at its match level, and
    // the talents of its OTHER spec - more than that level pays for - must already
    // be protected while it loads.
    std::unordered_set<ObjectGuid::LowType> Stashed;
    bool StoragePresent = false;
    // custom_rift_stash's `skills` and `pet_spells` columns (2026-10-23): without them the
    // real skills and pet ranks are put back from memory only, never after a crash.
    bool StashSkillsColumn = false;
    bool StashPetColumn = false;
    // The characters whose skills follow the tournament's rules while they are in a match
    // (Tournament::AlwaysMaxWeaponSkill / AlwaysMaxSkillForLevel ask UsesMatchSkills): a
    // leaf lock of its own, never held while calling out, because those are asked from
    // deep inside Player code that other locks may already be wrapped around.
    std::mutex MatchSkillMutex;
    std::unordered_set<ObjectGuid::LowType> MatchSkillPlayers;
    void SetMatchSkills(Player const* player, bool on)
    {
        std::lock_guard<std::mutex> lock(MatchSkillMutex);
        if (on)
            MatchSkillPlayers.insert(player->GetGUID().GetCounter());
        else
            MatchSkillPlayers.erase(player->GetGUID().GetCounter());
    }
    bool WeaponTablePresent = false;   // `custom_rift_weapons`: the armsmaster's choice, kept

    // The core abilities (world `rift_ability_whitelist`): per class, the class
    // spells a skill pick can learn, by Spell.dbc name. A class with no rows keeps
    // every class spell (and a realm without the table, every class's).
    struct WhitelistEntry
    {
        std::string Name;            // lower case; Prefix: Name is the start of the spell's name
        bool Prefix = false;
        bool Free = false;
    };
    std::unordered_map<uint8, std::vector<WhitelistEntry>> Whitelist;

    std::string Normalised(std::string text)
    {
        std::string out;
        for (size_t i = 0; i < text.size(); ++i)
        {
            // a typographic apostrophe (U+2019, as typed in the owner's list) is an apostrophe
            if (i + 2 < text.size() && uint8(text[i]) == 0xE2 && uint8(text[i + 1]) == 0x80 && uint8(text[i + 2]) == 0x99)
            {
                out += '\'';
                i += 2;
                continue;
            }
            out += char(std::tolower(uint8(text[i])));
        }
        return out;
    }

    // -1: the class keeps no list (everything allowed); 0: off the list; 1: a
    // pick learns it; 2: free.
    int32 WhitelistStatus(uint8 playerClass, SpellInfo const* spell)
    {
        auto const itr = Whitelist.find(playerClass);
        if (itr == Whitelist.end())
            return -1;
        std::string const name = Normalised(spell->SpellName[LOCALE_enUS] ? spell->SpellName[LOCALE_enUS] : "");
        for (WhitelistEntry const& e : itr->second)
            if (e.Prefix ? name.compare(0, e.Name.size(), e.Name) == 0 : name == e.Name)
                return e.Free ? 2 : 1;
        return 0;
    }

    bool IsTransient(Player const* player)
    {
        WorldSession const* session = player->GetSession();
        return !session || session->IsTransientPlayerSession();
    }

    // A class ability the match governs: one of the character's own class skill
    // lines, not passive, and trained at a level. What talents teach comes and
    // goes with the talents themselves.
    bool IsGovernedClassSpell(Player const* player, SpellInfo const* spellInfo)
    {
        if (!spellInfo || spellInfo->IsPassive() || spellInfo->SpellLevel == 0 || GetTalentSpellPos(spellInfo->Id))
            return false;

        SkillLineAbilityMapBounds const bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spellInfo->Id);
        for (auto itr = bounds.first; itr != bounds.second; ++itr)
        {
            SkillLineAbilityEntry const* ability = itr->second;
            SkillLineEntry const* skill = sSkillLineStore.LookupEntry(ability->SkillLine);
            if (skill && skill->CategoryID == SKILL_CATEGORY_CLASS && (!ability->ClassMask || (ability->ClassMask & player->GetClassMask())))
                return true;
        }
        return false;
    }

    void BuildState(Player const* player, State& state)
    {
        uint8 const spec = player->GetActiveSpec();

        // ---- the talent build, and the spells it teaches (not the match's to govern)
        std::unordered_set<uint32> taughtByTalents;
        for (TalentEntry const* talent : sTalentStore)
        {
            TalentTabEntry const* tab = talent ? sTalentTabStore.LookupEntry(talent->TabID) : nullptr;
            if (!tab || !(tab->ClassMask & player->GetClassMask()))
                continue;

            for (int32 rank = MAX_TALENT_RANK - 1; rank >= 0; --rank)
            {
                if (!talent->SpellRank[rank] || !player->HasTalent(talent->SpellRank[rank], spec))
                    continue;
                state.RealTalents.push_back({ talent->ID, uint32(rank), talent->TierID });
                if (SpellInfo const* info = sSpellMgr->GetSpellInfo(talent->SpellRank[rank]))
                    for (SpellEffectInfo const& effect : info->GetEffects())
                        if (effect.Effect == SPELL_EFFECT_LEARN_SPELL && effect.TriggerSpell)
                            taughtByTalents.insert(effect.TriggerSpell);
                break;
            }
        }
        std::sort(state.RealTalents.begin(), state.RealTalents.end(), [](LearnedTalent const& a, LearnedTalent const& b)
        {
            return std::tie(a.Tier, a.Id) < std::tie(b.Tier, b.Id);
        });

        // ---- class spells, grouped by rank chain
        for (auto const& [spellId, playerSpell] : player->GetSpellMap())
        {
            if (playerSpell.state == PLAYERSPELL_REMOVED || playerSpell.disabled || taughtByTalents.count(spellId))
                continue;
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
            if (!IsGovernedClassSpell(player, info))
                continue;
            uint32 const first = sSpellMgr->GetFirstSpellInChain(spellId);
            state.Chains[first ? first : spellId].Ranks.push_back(spellId);
        }
        // ---- the trained ranks after a talent (Mortal Strike 2-4, Hemorrhage 2-5): every
        // one is a pick, whatever the real character trained. Only the ranks it had
        // trained AND had active were here: a talent of its other spec has them put aside
        // (disabled), one it never took has none, and either, taken in the match, could
        // never be ranked up. A rank it does not know is taught for the match alone
        // (HandlePickRequest, MatchRanks).
        for (TalentEntry const* talent : sTalentStore)
        {
            TalentTabEntry const* tab = talent ? sTalentTabStore.LookupEntry(talent->TabID) : nullptr;
            if (!tab || !(tab->ClassMask & player->GetClassMask()) || !talent->SpellRank[0])
                continue;
            uint32 const first = talent->SpellRank[0];
            for (uint32 next = sSpellMgr->GetNextSpellInChain(first); next; next = sSpellMgr->GetNextSpellInChain(next))
            {
                if (taughtByTalents.count(next) || !IsGovernedClassSpell(player, sSpellMgr->GetSpellInfo(next)))
                    break;      // a talent's own next rank (Improved X 2/5), or not a trained rank
                std::vector<uint32>& ranks = state.Chains[first].Ranks;
                if (std::find(ranks.begin(), ranks.end(), next) == ranks.end())
                    ranks.push_back(next);
            }
        }
        for (auto itr = state.Chains.begin(); itr != state.Chains.end();)
        {
            Chain& chain = itr->second;
            std::sort(chain.Ranks.begin(), chain.Ranks.end(), [](uint32 a, uint32 b)
            {
                return sSpellMgr->GetSpellRank(a) < sSpellMgr->GetSpellRank(b);
            });
            // Off the core list: out of the match altogether. A talent's chain is not
            // the list's to drop - the talent point is its gate, and only the trained
            // ranks after it are in the chain. The lists name almost none of them
            // (Trueshot Aura, Aimed Shot, Mortal Strike), so a talent learned in the
            // match stayed at rank 1 (owner, 2026-10-23: "i should be able to learn
            // trueshot aura 2/3 with skillpoints").
            SpellInfo const* first = sSpellMgr->GetSpellInfo(itr->first);
            int32 const status = first ? WhitelistStatus(player->GetClass(), first) : -1;
            if (status == 0 && !GetTalentSpellPos(itr->first))
            {
                state.Removed.insert(chain.Ranks.begin(), chain.Ranks.end());
                itr = state.Chains.erase(itr);
                continue;
            }
            // The ranks after a talent are picks, once the talent is learned in
            // the match - never free: learning Trueshot Aura at 30 handed over all
            // three ranks, the level-50 and -60 ones too.
            chain.Talent = GetTalentSpellPos(itr->first) != nullptr;
            if (status == 2 && !chain.Talent)
                state.FreeChains.insert(itr->first);
            for (uint32 spell : chain.Ranks)
                state.SpellToChain[spell] = itr->first;
            ++itr;
        }
    }

    std::string TalentsToString(std::vector<LearnedTalent> const& talents)
    {
        std::string out;
        for (LearnedTalent const& t : talents)
            out += (out.empty() ? "" : ",") + std::to_string(t.Id) + ":" + std::to_string(t.Rank);
        return out;
    }

    std::vector<LearnedTalent> TalentsFromString(std::string const& text)
    {
        std::vector<LearnedTalent> out;
        for (std::string_view part : Trinity::Tokenize(text, ',', false))
        {
            std::vector<std::string_view> const idRank = Trinity::Tokenize(part, ':', false);
            if (idRank.size() != 2)
                continue;
            Optional<uint32> const id = Trinity::StringTo<uint32>(idRank[0]);
            Optional<uint32> const rank = Trinity::StringTo<uint32>(idRank[1]);
            TalentEntry const* talent = id ? sTalentStore.LookupEntry(*id) : nullptr;
            if (!talent || !rank || *rank >= MAX_TALENT_RANK)
                continue;
            out.push_back({ *id, *rank, talent->TierID });
        }
        std::sort(out.begin(), out.end(), [](LearnedTalent const& a, LearnedTalent const& b)
        {
            return std::tie(a.Tier, a.Id) < std::tie(b.Tier, b.Id);
        });
        return out;
    }

    // The match's talents off, the character's own build back, tier by tier so
    // every point it costs is free when it is asked for.
    void RestoreTalents(Player* player, std::vector<LearnedTalent> const& talents)
    {
        player->ResetTalents(true);
        for (LearnedTalent const& t : talents)
            player->LearnTalent(t.Id, t.Rank);
        player->SendTalentsInfoData(false);
    }

    // ---- a world character's skills (SavedSkill): every level-ranged one it knows, as
    // Player::UpdateSkillsForLevel would raise it - weapons, defense, lockpicking.
    std::vector<SavedSkill> SnapshotSkills(Player const* player)
    {
        std::vector<SavedSkill> out;
        for (SkillLineEntry const* skill : sSkillLineStore)
        {
            if (!skill || !player->HasSkill(skill->ID))
                continue;
            SkillRaceClassInfoEntry const* rc = GetSkillRaceClassInfo(skill->ID, player->GetRace(), player->GetClass());
            if (!rc || GetSkillRangeType(rc) != SKILL_RANGE_LEVEL)
                continue;
            out.push_back({ uint16(skill->ID), player->GetSkillStep(skill->ID), player->GetPureSkillValue(skill->ID),
                player->GetPureMaxSkillValue(skill->ID) });
        }
        return out;
    }

    void RestoreSkills(Player* player, std::vector<SavedSkill> const& skills)
    {
        for (SavedSkill const& s : skills)
            if (s.Value && player->HasSkill(s.Id))
                player->SetSkill(s.Id, s.Step, s.Value, s.Max);
    }

    std::string SkillsToString(std::vector<SavedSkill> const& skills)
    {
        std::string out;
        for (SavedSkill const& s : skills)
            out += Trinity::StringFormat("{}{}:{}:{}:{}", out.empty() ? "" : ",", s.Id, s.Step, s.Value, s.Max);
        return out;
    }

    std::vector<SavedSkill> SkillsFromString(std::string const& text)
    {
        std::vector<SavedSkill> out;
        for (std::string_view entry : Trinity::Tokenize(text, ',', false))
        {
            std::vector<std::string_view> f = Trinity::Tokenize(entry, ':', false);
            if (f.size() != 4)
                continue;
            Optional<uint16> id = Trinity::StringTo<uint16>(f[0]), step = Trinity::StringTo<uint16>(f[1]),
                value = Trinity::StringTo<uint16>(f[2]), max = Trinity::StringTo<uint16>(f[3]);
            if (id && step && value && max)
                out.push_back({ *id, *step, *value, *max });
        }
        return out;
    }

    // ---- a hunter pet's ranks (PetRank). This tree's hunter pets keep what Beast
    // Training taught them (Pet::InitLevelupSpellsForLevel learns nothing for them), so a
    // character's level-60 pet brought rank 8 Bite into a level-10 match (owner,
    // 2026-10-23: "my pets know max rank bite and other things"). In a match each ability
    // is held at the highest rank of the pet's own whose level the pet has reached.
    std::string PetRanksToString(std::map<uint32, std::map<uint32, PetRank>> const& pets)
    {
        std::string out;
        for (auto const& [number, chains] : pets)
            for (auto const& [first, r] : chains)
                if (r.Current != r.Original)
                    out += Trinity::StringFormat("{}{}:{}:{}:{}:{}", out.empty() ? "" : ",", number, first, r.Original, r.Current, uint32(r.Active));
        return out;
    }

    std::map<uint32, std::map<uint32, PetRank>> PetRanksFromString(std::string const& text)
    {
        std::map<uint32, std::map<uint32, PetRank>> out;
        for (std::string_view entry : Trinity::Tokenize(text, ',', false))
        {
            std::vector<std::string_view> f = Trinity::Tokenize(entry, ':', false);
            if (f.size() != 5)
                continue;
            Optional<uint32> number = Trinity::StringTo<uint32>(f[0]), first = Trinity::StringTo<uint32>(f[1]),
                original = Trinity::StringTo<uint32>(f[2]), current = Trinity::StringTo<uint32>(f[3]);
            Optional<uint8> active = Trinity::StringTo<uint8>(f[4]);
            if (number && first && original && current && active)
                out[*number][*first] = { *original, *current, *active };
        }
        return out;
    }

    // The ranks back: on the pet, when it is out, else in its pet_spell rows (it was
    // saved with the match's ranks when it was dismissed or stabled).
    void PutBackPetRanks(Player* player, std::map<uint32, std::map<uint32, PetRank>> const& pets)
    {
        Pet* pet = player->GetPet();
        for (auto const& [number, chains] : pets)
        {
            bool const live = pet && pet->IsInWorld() && pet->GetCharmInfo() && pet->GetCharmInfo()->GetPetNumber() == number;
            for (auto const& [first, r] : chains)
            {
                if (r.Current == r.Original)
                    continue;
                if (live)
                {
                    if (r.Current)
                        pet->unlearnSpell(r.Current, false, false);
                    pet->learnSpell(r.Original);
                    continue;
                }
                // at once: a pet loading next (a login) must read the real ranks
                if (r.Current)
                    CharacterDatabase.DirectPExecute("DELETE FROM pet_spell WHERE guid = {} AND spell = {}", number, r.Current);
                CharacterDatabase.DirectPExecute("REPLACE INTO pet_spell (guid, spell, active) VALUES ({}, {}, {})", number, r.Original, uint32(r.Active));
            }
            if (live)
                player->PetSpellInitialize();
        }
    }

    void StashPetRanks(Player const* player, State const& state)
    {
        if (StashPetColumn && state.Persistent)
            CharacterDatabase.PExecute("UPDATE custom_rift_stash SET pet_spells = '{}' WHERE guid = {}",
                PetRanksToString(state.PetRanks), player->GetGUID().GetCounter());
    }

    // The pet out now, at its level: each ability it knows with ranks held at the highest
    // of them the pet has reached - none yet, below the first rank's level - raised as it
    // levels. A pet first seen here is written down as it really is.
    void SyncPetRanks(Player* player, State& state)
    {
        Pet* pet = player->GetPet();
        if (!pet || pet->getPetType() != HUNTER_PET || !pet->IsInWorld() || !pet->GetCharmInfo())
            return;
        std::map<uint32, PetRank>& chains = state.PetRanks[pet->GetCharmInfo()->GetPetNumber()];
        for (auto const& [spellId, known] : pet->m_spells)
        {
            if (known.state == PETSPELL_REMOVED || known.type != PETSPELL_NORMAL)
                continue;
            uint32 const first = sSpellMgr->GetFirstSpellInChain(spellId);
            if (!first || (first == spellId && !sSpellMgr->GetNextSpellInChain(spellId)))
                continue;                          // no ranks: nothing to hold back
            auto const added = chains.try_emplace(first);
            if (added.second)
                added.first->second = { spellId, spellId, uint8(known.active) };
        }
        uint8 const level = pet->GetLevel();
        bool changed = false;
        for (auto& [first, r] : chains)
        {
            uint32 want = r.Original;
            while (want)
            {
                SpellInfo const* info = sSpellMgr->GetSpellInfo(want);
                if (info && info->SpellLevel <= level)
                    break;
                want = sSpellMgr->GetPrevSpellInChain(want);
            }
            if (want == r.Current)
                continue;
            // the bar keeps the button for the rank that replaces it, and loses it when none does
            if (r.Current)
                pet->unlearnSpell(r.Current, false, want == 0);
            if (want)
                pet->learnSpell(want);
            r.Current = want;
            changed = true;
        }
        if (changed)
        {
            player->PetSpellInitialize();
            StashPetRanks(player, state);
        }
    }

    std::string ButtonsToString(std::vector<SavedButton> const& buttons)
    {
        std::string out;
        for (SavedButton const& b : buttons)
            out += (out.empty() ? "" : ",") + std::to_string(b.Button) + ":" + std::to_string(b.Action) + ":" + std::to_string(b.Type);
        return out;
    }

    std::vector<SavedButton> ButtonsFromString(std::string const& text)
    {
        std::vector<SavedButton> out;
        for (std::string_view part : Trinity::Tokenize(text, ',', false))
        {
            std::vector<std::string_view> const f = Trinity::Tokenize(part, ':', false);
            if (f.size() != 3)
                continue;
            Optional<uint32> const button = Trinity::StringTo<uint32>(f[0]);
            Optional<uint32> const action = Trinity::StringTo<uint32>(f[1]);
            Optional<uint32> const type = Trinity::StringTo<uint32>(f[2]);
            if (button && action && type && *button < MAX_ACTION_BUTTONS)
                out.push_back({ uint8(*button), *action, uint8(*type) });
        }
        return out;
    }

    std::vector<SavedButton> SnapshotButtons(Player* player)
    {
        std::vector<SavedButton> out;
        for (uint32 button = 0; button < MAX_ACTION_BUTTONS; ++button)
            if (ActionButton const* ab = player->GetActionButton(uint8(button)))
                out.push_back({ uint8(button), ab->GetAction(), uint8(ab->GetType()) });
        return out;
    }

    // The bars exactly as they were: whatever the match put on them comes off,
    // and every saved button goes back (its spell is known again by now - the
    // talents are restored first). The caller sends them, once the client knows
    // every spell on them again.
    void RestoreButtons(Player* player, std::vector<SavedButton> const& buttons)
    {
        for (uint32 button = 0; button < MAX_ACTION_BUTTONS; ++button)
            player->removeActionButton(uint8(button));
        for (SavedButton const& b : buttons)
            player->addActionButton(b.Button, b.Action, b.Type);
    }

    uint32 TotalSteps(State const& state)
    {
        uint32 total = 0;
        for (auto const& [first, chain] : state.Chains)
            total += uint32(chain.Ranks.size());
        return total;
    }

    uint32 UnlockedSteps(State const& state)
    {
        uint32 total = 0;
        for (auto const& [first, ranks] : state.Unlocked)
            total += ranks;
        return total;
    }

    // One skill pick per level, League's skill point: the starting level's, then
    // one for every level after it.
    uint32 constexpr PICKS_PER_LEVEL = 1;

    // What can be chosen now: each chain's next rank, once the match level has
    // reached the level it is trained at.
    std::vector<uint32> Offers(State const& state)
    {
        std::vector<uint32> offers;
        for (auto const& [first, chain] : state.Chains)
        {
            auto const itr = state.Unlocked.find(first);
            uint8 const have = itr != state.Unlocked.end() ? itr->second : 0;
            if (have >= chain.Ranks.size() || (chain.Talent && !state.TalentKnown.count(first)))
                continue;
            SpellInfo const* next = sSpellMgr->GetSpellInfo(chain.Ranks[have]);
            if (next && next->SpellLevel <= state.MatchLevel)
                offers.push_back(chain.Ranks[have]);
        }
        return offers;
    }

    // A match is self contained: a champion fights with the class's spells, the
    // race's racials and what the shop sells. A spell the character brought from
    // outside - food and drink (the tournament's too), the hearthstone,
    // professions, mounts, companions - is refused and kept off the spellbook
    // and the bars. The class's own are told apart by skill line: class, weapon
    // and generic lines stay, a racial line stays, secondary and primary
    // profession lines do not, and a spell on no line stays only if it belongs
    // to a class family (a talent's) or is a racial that has no line.
    uint32 constexpr RIFT_OWN_FIRST = 94120;
    uint32 constexpr RIFT_OWN_LAST = 94699;
    uint32 constexpr SPELL_MATCH_STEED = 94150;
    uint32 constexpr SPELL_MATCH_SWIFT_STEED = 94151;
    uint32 constexpr SPELL_MATCH_PLAINSRUNNING = 89153;

    bool BroughtIn(uint32 spell)
    {
        if (spell >= RIFT_OWN_FIRST && spell <= RIFT_OWN_LAST)
            return false;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
        if (!info)
            return false;
        // Ridden on the shop's steeds (and Plainsrunning, at the steed's
        // speed), not on the mounts a character brought: buying one is the point.
        if (info->HasAura(SPELL_AURA_MOUNTED))
            return spell != SPELL_MATCH_STEED && spell != SPELL_MATCH_SWIFT_STEED && spell != SPELL_MATCH_PLAINSRUNNING;
        if (info->IsPassive() || spell == SPELL_MATCH_PLAINSRUNNING)
            return false;
        bool onLine = false;
        auto const bounds = sSpellMgr->GetSkillLineAbilityMapBounds(spell);
        for (auto itr = bounds.first; itr != bounds.second; ++itr)
        {
            SkillLineEntry const* line = sSkillLineStore.LookupEntry(itr->second->SkillLine);
            if (!line)
                continue;
            onLine = true;
            if (line->ID == SKILL_MOUNTS || line->ID == SKILL_COMPANIONS)
                return true;
            if (line->CategoryID == SKILL_CATEGORY_PROFESSION)
                continue;
            // Racials share the secondary category with Cooking and First Aid.
            if (line->CategoryID == SKILL_CATEGORY_SECONDARY)
            {
                char const* name = line->DisplayName[0];
                if (name && std::string_view(name).find("Racial") != std::string_view::npos)
                    return false;
                continue;
            }
            return false;
        }
        if (onLine)
            return true;
        uint32 constexpr SPELL_SHADOWMELD = 58984;
        uint32 constexpr SPELL_EVERY_MAN_FOR_HIMSELF = 59752;
        if (spell == SPELL_SHADOWMELD || spell == SPELL_EVERY_MAN_FOR_HIMSELF)
            return false;
        // The hidden Opening spells a locked object is clicked with.
        if (info->HasEffect(SPELL_EFFECT_OPEN_LOCK))
            return false;
        return info->SpellFamilyName == SPELLFAMILY_GENERIC && !GetTalentSpellPos(spell);
    }

    // A spell button the match takes off the bars: a class spell none of whose
    // ranks is unlocked yet, a spell the character no longer knows (taught by a
    // talent the match reset), or one brought in from outside.
    bool HiddenInMatch(Player const* player, State const& state, uint32 spell)
    {
        if (!player->HasSpell(spell) || state.Removed.count(spell) || BroughtIn(spell))
            return true;
        auto const chain = state.SpellToChain.find(spell);
        if (chain == state.SpellToChain.end())
            return false;
        auto const unlocked = state.Unlocked.find(chain->second);
        return unlocked == state.Unlocked.end() || !unlocked->second;
    }

    // Every bar is cleared of what the match refuses: spells as above, any item
    // but the shop's, and every macro and equipment set. A #showtooltip macro
    // draws the spell it casts - a level-60 hunter's Aspect of the Hawk macro
    // stood on the bar of a level-10 match, refused on every click - so a macro
    // comes back once every spell it casts is unlocked (RefillSavedButtons,
    // MacroReady). RestoreButtons puts them all back.
    void HideLockedButtons(Player* player, State const& state)
    {
        for (SavedButton const& b : state.RealButtons)
        {
            bool keep = false;
            if (b.Type == ACTION_BUTTON_SPELL)
                keep = !HiddenInMatch(player, state, b.Action);
            else if (b.Type == ACTION_BUTTON_ITEM)
                keep = b.Action >= SHOP_ITEM_FIRST && b.Action <= SHOP_ITEM_LAST;
            if (!keep)
                player->removeActionButton(b.Button);
        }
    }

    // In a match the spellbook lists the best unlocked rank of each ability and
    // nothing off the whitelist; anything else as usual.
    bool MatchShows(State const& state, uint32 spell, bool active)
    {
        if (state.Removed.count(spell) || BroughtIn(spell))
            return false;
        auto const chain = state.SpellToChain.find(spell);
        if (chain == state.SpellToChain.end())
            return active;
        // Every unlocked rank as WoW lists ranks - a spell whose ranks stack
        // (Arcane Shot) keeps them all; one whose new rank replaces the old
        // (an aspect, a buff) only the best - and nothing past the unlocked.
        // Showing only the best rank took rank 1 off the bar when rank 2 came.
        auto const unlocked = state.Unlocked.find(chain->second);
        uint8 const have = unlocked != state.Unlocked.end() ? unlocked->second : 0;
        std::vector<uint32> const& ranks = state.Chains.at(chain->second).Ranks;
        auto const pos = std::find(ranks.begin(), ranks.end(), spell);
        if (!have || pos == ranks.end() || size_t(pos - ranks.begin()) >= have)
            return false;
        return active || ranks[have - 1] == spell;
    }

    void SendSpellbookChange(Player* player, uint32 spell, bool add)
    {
        if (add)
        {
            WorldPacket data(SMSG_LEARNED_SPELL, 6);
            data << uint32(spell);
            data << uint16(0);
            player->SendDirectMessage(&data);
        }
        else
        {
            WorldPacket data(SMSG_REMOVED_SPELL, 4);
            data << uint32(spell);
            player->SendDirectMessage(&data);
        }
    }

    // The client keeps every spell it was ever sent: a fresh SMSG_INITIAL_SPELLS
    // mid-session only adds (a resent filtered list left a hunter's whole
    // spellbook in place). So the difference is sent spell by spell, the way a
    // talent reset does it; CENTURION_Rift hides the "You have (un)learned"
    // lines that come with it. Spells stay known server side - nothing is
    // written down. A bot has no client: BotSyncSpellbook.
    //
    // Half at a time (PublishBars): the spells the client gains (gains), or the
    // ones it loses.
    void SyncClientSpellbook(Player* player, State& state, bool gains)
    {
        if (!state.Persistent || !player->GetSession())
            return;
        for (auto const& [spell, known] : player->GetSpellMap())
        {
            if (known.state == PLAYERSPELL_REMOVED || known.disabled)
                continue;
            bool const want = MatchShows(state, spell, known.active);
            bool const has = known.active ? !state.ClientHidden.count(spell) : state.ClientExtra.count(spell) != 0;
            if (want == has || want != gains)
                continue;
            SendSpellbookChange(player, spell, want);
            if (known.active)
            {
                if (want)
                    state.ClientHidden.erase(spell);
                else
                    state.ClientHidden.insert(spell);
            }
            else if (want)
                state.ClientExtra.insert(spell);
            else
                state.ClientExtra.erase(spell);
        }
    }

    // The way out: the client is told the spellbook it normally holds - what it
    // gains before the real bars go out (gains), what it loses after them.
    // What a client that logged in while the match held its character was sent:
    // SendInitialSpells asks ShowsInSpellbook, so its list is the match's - the
    // real ranks the match hides are missing (ClientHidden) and the lower ranks
    // it lists are extra (ClientExtra).
    void NoteLoginSpellbook(Player const* player, State& state)
    {
        state.ClientHidden.clear();
        state.ClientExtra.clear();
        for (auto const& [spell, known] : player->GetSpellMap())
        {
            if (known.state == PLAYERSPELL_REMOVED || known.disabled)
                continue;
            bool const shows = MatchShows(state, spell, known.active);
            if (known.active && !shows)
                state.ClientHidden.insert(spell);
            else if (!known.active && shows)
                state.ClientExtra.insert(spell);
        }
    }

    void ResetClientSpellbook(Player* player, State& state, bool gains)
    {
        if (player->GetSession())
        {
            if (gains)
            {
                for (uint32 spell : state.ClientHidden)
                    if (player->HasSpell(spell))
                        SendSpellbookChange(player, spell, true);
            }
            else
                for (uint32 spell : state.ClientExtra)
                    SendSpellbookChange(player, spell, false);
        }
        if (gains)
            state.ClientHidden.clear();
        else
            state.ClientExtra.clear();
    }

    // League's inventory. A shop item is not WoW gear: it sits in the bags (the
    // CENTURION_Rift frame shows it in one of six item slots, or the trinket
    // slot) and works from there. What the held items give is League's own stats
    // (world `rift_league_item`), summed and put on the champion together with
    // its League base stats by six hidden carriers (SetChampionCarriers); the
    // items' UNIQUE passives (the Rift's own spells) go on once each. The stat
    // spells on the items (94200-94299) are only their tooltips' lines.
    uint32 constexpr SPELL_CHAMPION_A = 94147;    // stamina (League's health), armour, resistance
    uint32 constexpr SPELL_CHAMPION_B = 94148;    // attack power, ranged attack power, crit
    uint32 constexpr SPELL_CHAMPION_C = 94154;    // melee and ranged attack speed, spell damage
    uint32 constexpr SPELL_CHAMPION_D = 94155;    // healing, mana, health regeneration
    uint32 constexpr SPELL_CHAMPION_E = 94156;    // mana regeneration, (0: magic penetration is MagicPenetration), run speed
    uint32 constexpr SPELL_CHAMPION_F = 94157;    // the boots' run speed; life steal (tenths of a percent); spell crit
    uint32 constexpr CHAMPION_CARRIERS[] = { SPELL_CHAMPION_A, SPELL_CHAMPION_B, SPELL_CHAMPION_C,
        SPELL_CHAMPION_D, SPELL_CHAMPION_E, SPELL_CHAMPION_F };
    // The stat spells on the items are only their tooltips' lines (rift_shop.py's
    // STAT_SPELL_RANGES).
    bool IsStatSpell(uint32 spell) { return (spell >= 94200 && spell <= 94299) || (spell >= 94400 && spell <= 94699); }
    // League's items, batch 1 (2026-10-22): the champion's whole cooldown reduction as one
    // modifier (spell_rift_cdr), and the stacking items' stacks as Bloodthirster's are shown.
    uint32 constexpr SPELL_COOLDOWN_CARRIER = 94170;
    uint32 constexpr SPELL_MEJAI_STACK = 94174;
    uint32 constexpr SPELL_OCCULT_STACK = 94175;
    uint32 constexpr SPELL_SEEKER_STACK = 94176;
    uint8 constexpr TAKEDOWN_MAX_STACKS = 20;       // Mejai's Soulstealer, Sword of the Occult
    uint8 constexpr SEEKER_MAX_STACKS = 30;
    float constexpr MEJAI_FULL_CDR = 15.0f;         // percent, at 20 stacks
    float constexpr OCCULT_FULL_SPEED = 15.0f;      // percent move speed, at 20 stacks
    // League's item auras: the allies' (Aegis, Emblem, Mana Manipulator, Zeke's, Will of
    // the Ancients) reach 1100 units, the enemies' (Abyssal Scepter, Frozen Heart) 700.
    float constexpr ALLY_AURA_RANGE = RiftLeague::Yards(1100.0f);
    float constexpr ENEMY_AURA_RANGE = RiftLeague::Yards(700.0f);
    float constexpr LEAGUE_BASE_MOVE = 325.0f;      // League's flat speeds as a share of a champion's base
    uint32 constexpr RIFT_MAP_ID = 1690;
    uint32 constexpr SPELL_DEATHCAP = 94140;      // Rabadon's: +30% ability power (League S3)
    uint32 constexpr SPELL_ETERNITY_STACK = 94129; // Rod of Ages: League's +20 health, +20 mana, +2 AP a stack
    // The Bloodthirster (League S3): +1 attack damage and +0.2% life steal a unit
    // kill, up to 30; half are lost on death. 94160 shows the stacks.
    uint32 constexpr ITEM_BLOODTHIRSTER = 991042;
    uint32 constexpr SPELL_BLOODTHIRSTER_STACK = 94160;
    uint8 constexpr BLOODTHIRSTER_MAX_STACKS = 30;
    float constexpr BLOODTHIRSTER_DAMAGE_PER_STACK = 1.0f;
    float constexpr BLOODTHIRSTER_LIFE_STEAL_PER_STACK = 0.2f;
    // League Season 3's camp buffs (BattlegroundSRF puts them on; the mid-2013
    // wiki's numbers): the Crest's cooldown reduction (its first effect's
    // amount, capped at League's 40%) and mana - 25 every 5 sec in League's
    // mana, 0.5% of the pool a second, the same share of energy - and Baron's
    // attack damage and ability power (its first effect's amount) and 3% of
    // health and 1% of mana every 5 sec.
    float constexpr COOLDOWN_REDUCTION_CAP = 40.0f;
    float constexpr CREST_MANA_PER_5 = 25.0f;
    float constexpr CREST_POOL_PER_5 = 0.025f;
    float constexpr BARON_HEALTH_PER_5 = 0.03f;
    float constexpr BARON_MANA_PER_5 = 0.01f;
    // The Blessing's basic attacks: a slow of 8 / 16 / 24% (5 / 10 / 15% at
    // range) from League level 1 / 6 / 11 for 3 sec, and 8 + 2 a level true
    // damage twice (BG_SRF_SPELL_RED_BURN, spell_rift_lizard_burn).
    int32 constexpr LIZARD_BURN_DURATION = 3000;
    // Rift structures: no life steal from them (League).
    uint32 constexpr RIFT_STRUCTURE_FIRST = 922100;
    uint32 constexpr RIFT_STRUCTURE_LAST = 922105;
    uint32 constexpr ITEM_SLOTS = 6;
    uint32 constexpr RIFT_SPELL_FIRST = 94120;
    uint32 constexpr RIFT_SPELL_LAST = 94160;

    bool IsShopItem(uint32 entry) { return entry >= SHOP_ITEM_FIRST && entry <= SHOP_ITEM_LAST; }

    // The Rift's mounts (rift_shop.py's extras): the trinket slot holds one, and
    // they are the only mounts that work in a match.
    uint32 constexpr ITEM_STEED = 991038;          // 60%, spell 94150
    uint32 constexpr ITEM_SWIFT_STEED = 991039;    // 100%, spell 94151, built from the steed
    uint32 constexpr SPELL_STEED = 94150;
    uint32 constexpr SPELL_SWIFT_STEED = 94151;
    uint32 constexpr SPELL_PLAINSRUNNING = 89153;  // the Tauren's run-mount: rides at the steed's speed
    bool IsTrinketItem(uint32 entry) { return entry == ITEM_STEED || entry == ITEM_SWIFT_STEED; }

    // Item slots the held shop items fill, those going into a purchase left out.
    // A stack (League's potions, the biscuit) fills one slot.
    uint32 HeldItemSlots(Player const* player, std::vector<uint32> const& consumed)
    {
        uint32 held = 0;
        for (uint32 entry = SHOP_ITEM_FIRST; entry <= SHOP_ITEM_LAST; ++entry)
        {
            if (IsTrinketItem(entry))
                continue;
            uint32 const count = player->GetItemCount(entry, false);
            ItemTemplate const* proto = count ? sObjectMgr->GetItemTemplate(entry) : nullptr;
            if (!proto)
                continue;
            uint32 const going = uint32(std::count(consumed.begin(), consumed.end(), entry));
            uint32 const left = count > going ? count - going : 0;
            held += proto->GetMaxStackSize() > 1 ? (left ? 1 : 0) : left;
        }
        return held;
    }

    // Riding, as the match counts it: the steed held (75 = 60%, 150 = 100%).
    uint16 MatchRiding(Player const* player)
    {
        if (player->GetItemCount(ITEM_SWIFT_STEED, false))
            return 150;
        if (player->GetItemCount(ITEM_STEED, false))
            return 75;
        return 0;
    }

    struct ItemEffects
    {
        RiftLeague::ItemStats League;      // summed per copy held; MoveSpeed without the boots
        float BootsSpeed = 0.0f;           // the fastest boots'
        std::map<uint32, uint32> Spells;
        UniqueStats Unique = { };          // the uniques, a name's highest, names added up
        std::vector<AuraEmit> Emits;
    };

    // The shop items in the bags, by entry, in one pass: 300 GetItemCount calls twice a
    // second for every champion was most of what SyncItems cost.
    std::map<uint32, uint32> ShopItemCounts(Player const* player)
    {
        std::map<uint32, uint32> held;
        auto add = [&held](Item const* item)
        {
            if (item && IsShopItem(item->GetEntry()))
                held[item->GetEntry()] += item->GetCount();
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            add(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag const* bag = player->GetBagByPos(bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    add(bag->GetItemByPos(uint8(slot)));
        return held;
    }

    ItemEffects HeldItemEffects(Player const* player, RiftLeague::Tables const& tables)
    {
        ItemEffects out;
        std::map<std::pair<std::string, RiftLeague::Unique>, float> named;
        for (auto const& [entry, held] : ShopItemCounts(player))
        {
            int32 const count = int32(held);
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
            if (!proto)
                continue;
            if (auto const uniques = tables.Uniques.find(entry); uniques != tables.Uniques.end())
                for (RiftLeague::ItemUnique const& u : uniques->second)
                {
                    if (u.Name == "*")
                        out.Unique[size_t(u.Stat)] += u.Value * float(count);
                    else
                    {
                        float& best = named[{ u.Name.empty() ? "#" + std::to_string(entry) : u.Name, u.Stat }];
                        best = std::max(best, u.Value);
                    }
                }
            if (RiftLeague::ItemStats const* stats = RiftLeague::FindItem(tables, entry))
            {
                float const n = float(count);
                RiftLeague::ItemStats& sum = out.League;
                sum.Health += stats->Health * n;
                sum.Mana += stats->Mana * n;
                sum.Damage += stats->Damage * n;
                sum.AbilityPower += stats->AbilityPower * n;
                sum.Armor += stats->Armor * n;
                sum.Resist += stats->Resist * n;
                sum.AttackSpeed += stats->AttackSpeed * n;
                sum.Crit += stats->Crit * n;
                sum.HealthRegen += stats->HealthRegen * n;
                sum.ManaRegen += stats->ManaRegen * n;
                sum.MagicPen += stats->MagicPen * n;
                sum.LifeSteal += stats->LifeSteal * n;
                if (stats->Boots)
                    out.BootsSpeed = std::max(out.BootsSpeed, stats->MoveSpeed);
                else
                    sum.MoveSpeed += stats->MoveSpeed * n;
            }
            for (_Spell const& spell : proto->Spells)
                if (spell.SpellId > 0 && spell.SpellTrigger == ITEM_SPELLTRIGGER_ON_EQUIP && !IsStatSpell(uint32(spell.SpellId)))
                    out.Spells[uint32(spell.SpellId)] += uint32(count);
        }
        for (auto const& [key, value] : named)
        {
            if (RiftLeague::IsAura(key.second))
                out.Emits.push_back({ key.first, key.second, value });
            else
                out.Unique[size_t(key.second)] += value;
        }
        // League's UNIQUE passives count once.
        for (auto& [spell, count] : out.Spells)
            if (spell >= RIFT_SPELL_FIRST && spell <= RIFT_SPELL_LAST)
                count = 1;
        return out;
    }

    // What the item auras of the champions near this one give it: its allies' (its own
    // included) within 1100 League units, its enemies' within 700. A name counts once,
    // its highest, however many hold it. A dead champion's auras are off.
    State* Find(Player const* player);   // further down, with the states

    UniqueStats GatherAuras(Player const* player)
    {
        UniqueStats out = { };
        Battleground const* bg = player->GetBattleground();
        if (!bg)
            return out;
        uint32 const team = bg->GetPlayerTeam(player->GetGUID());
        std::map<std::pair<std::string, RiftLeague::Unique>, float> best;
        for (auto const& [guid, bgPlayer] : bg->GetPlayers())
        {
            Player const* other = guid == player->GetGUID() ? player : ObjectAccessor::GetPlayer(*player, guid);
            if (!other || !other->IsAlive())
                continue;
            State const* os = Find(other);
            if (!os || os->Emits.empty())
                continue;
            bool const ally = bg->GetPlayerTeam(guid) == team;
            float const distance = player->GetExactDist2d(other);
            for (AuraEmit const& e : os->Emits)
            {
                bool const enemyAura = RiftLeague::IsEnemyAura(e.Stat);
                if (enemyAura == ally || distance > (enemyAura ? ENEMY_AURA_RANGE : ALLY_AURA_RANGE))
                    continue;
                float& value = best[{ e.Name, e.Stat }];
                value = std::max(value, e.Value);
            }
        }
        for (auto const& [key, value] : best)
            out[size_t(key.second)] += value;
        return out;
    }

    // The stacks a stacking item shows (Bloodthirster's way): gone at none.
    void ShowStacks(Player* player, uint32 spell, uint8 stacks)
    {
        Aura* aura = player->GetAura(spell);
        if (!stacks)
        {
            if (aura)
                player->RemoveAurasDueToSpell(spell);
            return;
        }
        if (!aura && player->IsAlive())
            aura = player->AddAura(spell, player);
        if (aura && aura->GetStackAmount() != stacks)
            aura->SetStackAmount(stacks);
    }

    // A carrier's effects set to `amounts`; gone when they are all zero.
    void SetCarrier(Player* player, uint32 spell, std::initializer_list<int32> amounts)
    {
        bool const any = std::any_of(amounts.begin(), amounts.end(), [](int32 a) { return a != 0; });
        Aura* aura = player->GetAura(spell);
        if (!any)
        {
            if (aura)
                player->RemoveAurasDueToSpell(spell);
            return;
        }
        if (!aura)
            aura = player->AddAura(spell, player);
        if (!aura)
            return;
        uint8 i = 0;
        for (int32 amount : amounts)
        {
            if (AuraEffect* effect = aura->GetEffect(i))
                if (effect->GetAmount() != amount)
                    effect->ChangeAmount(amount);
            ++i;
        }
    }

    int32 Round(float value) { return int32(std::lround(value)); }

    float HealthFromStamina(float stamina) { return std::min(20.0f, stamina) + std::max(0.0f, stamina - 20.0f) * 10.0f; }
    float ManaFromIntellect(float intellect) { return std::min(20.0f, intellect) + std::max(0.0f, intellect - 20.0f) * 15.0f; }

    // The attack power WoW gives the character from its level and BASE stats -
    // StatSystem's formula (Player::UpdateAttackPowerAndDamage) at the create
    // stats: what buffs and talents add to strength and agility stays on top of
    // League's number. The formula is read back from its result, so the druid's
    // forms and Predatory Strikes stay its own.
    float BaseAttackPower(Player const* player, bool ranged)
    {
        float str = 0.0f, agi = 0.0f;    // the formula's weights
        uint8 const cls = player->GetClass();
        ShapeshiftForm const form = player->GetShapeshiftForm();
        if (ranged)
        {
            if (cls == CLASS_HUNTER)
                agi = 2.0f;
            else if (cls != CLASS_DRUID || (form != FORM_CAT && form != FORM_BEAR && form != FORM_DIREBEAR))
                agi = 1.0f;
        }
        else
            switch (cls)
            {
                case CLASS_ROGUE:
                case CLASS_HUNTER:
                    str = 1.0f;
                    agi = 1.0f;
                    break;
                case CLASS_MAGE:
                case CLASS_PRIEST:
                case CLASS_WARLOCK:
                    str = 1.0f;
                    break;
                case CLASS_DRUID:
                    str = 2.0f;
                    agi = form == FORM_CAT ? 1.0f : 0.0f;
                    break;
                default:      // warrior, paladin, death knight, shaman
                    str = 2.0f;
                    break;
            }
        float const formula = player->GetFlatModifierValue(ranged ? UNIT_MOD_ATTACK_POWER_RANGED : UNIT_MOD_ATTACK_POWER, BASE_VALUE);
        return formula - str * (player->GetStat(STAT_STRENGTH) - player->GetCreateStat(STAT_STRENGTH))
            - agi * (player->GetStat(STAT_AGILITY) - player->GetCreateStat(STAT_AGILITY));
    }

    // A basic attack's damage before attack power (Player::CalculateMinMaxDamage):
    // the weapon's, a feral form's, plus the ammunition's for a shot.
    float WeaponPart(Player const* player, WeaponAttackType att)
    {
        float const speed = player->GetAPMultiplier(att, false);
        if (att != RANGED_ATTACK && player->IsInFeralForm())
            return float(std::min<uint8>(player->GetLevel(), 60)) * 1.05f * speed;
        float part = (player->GetWeaponDamageRange(att, MINDAMAGE) + player->GetWeaponDamageRange(att, MAXDAMAGE)) / 2.0f;
        if (att == RANGED_ATTACK)
            part += player->GetAmmoDPS() * speed;
        return part;
    }

    // The attack power that makes a basic attack with `att` hit for `perHit`:
    // weapon + attack power / 14 x the weapon's speed. With two weapons the main
    // hand shares the job with the off hand - half damage, at its own speed - so
    // the pair deals League's damage a second.
    float AttackPowerFor(Player const* player, WeaponAttackType att, float perHit)
    {
        float const speed = player->GetAPMultiplier(att, false);
        if (speed <= 0.0f)
            return 0.0f;
        float const weapon = WeaponPart(player, att);
        if (att == BASE_ATTACK && player->CanDualWield() && player->haveOffhandWeapon())
        {
            float const offSpeed = player->GetAPMultiplier(OFF_ATTACK, false);
            if (offSpeed > 0.0f)
                return 14.0f * (perHit - weapon - 0.5f * WeaponPart(player, OFF_ATTACK) * speed / offSpeed) / (1.5f * speed);
        }
        return 14.0f * (perHit - weapon) / speed;
    }

    // League's champions have no crit of their own: WoW's from base agility
    // (Player::GetMeleeCritFromAgility) comes off.
    float BaseAgilityCrit(Player const* player)
    {
        float const per = player->GetClass() == CLASS_ROGUE ? 29.0f : player->GetClass() == CLASS_HUNTER ? 53.0f : 20.0f;
        return player->GetCreateStat(STAT_AGILITY) / per;
    }

    // League's champion, in WoW's numbers: the class's League base stats at its
    // League level, its rune page and the held items' stats (world
    // `rift_league_champion`, `rift_league_item`), converted at the match's scale
    // and level (RiftLeague). Each carrier fills the gap between what WoW gives
    // the character on its own - its class's base health and stamina, the field
    // kit's armour and weapon, its base attack power and agility crit - and
    // League's number, so talents and buffs still add on top of League's.
    void SetChampionCarriers(Player* player, State& state, ItemEffects const& items, RiftLeague::Tables const& tables)
    {
        RiftLeague::ChampionStats const* cs = RiftLeague::FindChampion(tables, player->GetClass());
        if (!cs || state.MatchScale <= 0.0f || !state.CombatLevel)
        {
            for (uint32 spell : CHAMPION_CARRIERS)
                player->RemoveAurasDueToSpell(spell);
            return;
        }
        float const s = state.MatchScale;
        uint8 const level = state.CombatLevel;
        float const l = float(state.LeagueLevel);
        RiftLeague::ItemStats it = items.League;
        if (Aura const* eternity = player->GetAura(SPELL_ETERNITY_STACK))
        {
            float const stacks = float(eternity->GetStackAmount());
            it.Health += 20.0f * stacks;
            it.Mana += 20.0f * stacks;
            it.AbilityPower += 2.0f * stacks;
        }

        // The Bloodthirster's stacks, held only while it is.
        float const thirst = player->GetItemCount(ITEM_BLOODTHIRSTER, false) ? float(state.BloodthirsterStacks) : 0.0f;
        it.Damage += BLOODTHIRSTER_DAMAGE_PER_STACK * thirst;
        it.LifeSteal += BLOODTHIRSTER_LIFE_STEAL_PER_STACK * thirst;

        // League's UNIQUE item effects (rift_league_item_unique), and the item auras of
        // the champions near this one.
        using RiftLeague::Unique;
        auto const own = [&items](Unique stat) { return items.Unique[size_t(stat)]; };
        UniqueStats const aura = GatherAuras(player);
        auto const near = [&aura](Unique stat) { return aura[size_t(stat)]; };
        state.Unique = items.Unique;
        state.Emits = items.Emits;
        state.ReceivedSpellVamp = near(Unique::AuraAllySpellVamp);
        it.Resist += near(Unique::AuraAllyMr) - near(Unique::AuraEnemyMr);
        it.HealthRegen += near(Unique::AuraAllyHp5);
        it.ManaRegen += near(Unique::AuraAllyMp5);
        it.AbilityPower += near(Unique::AuraAllyAp);
        it.Damage += near(Unique::AuraAllyAd);
        it.LifeSteal += near(Unique::AuraAllyLs);
        // the stacking items
        it.AbilityPower += own(Unique::TakedownAp) * float(state.MejaiStacks);
        it.Damage += own(Unique::TakedownAd) * float(state.OccultStacks);
        if (own(Unique::TakedownAd) > 0.0f && state.OccultStacks >= TAKEDOWN_MAX_STACKS)
            it.MoveSpeed += OCCULT_FULL_SPEED;
        it.Armor += own(Unique::UnitKillArmorAp) * float(state.SeekerStacks);
        it.AbilityPower += own(Unique::UnitKillArmorAp) * float(state.SeekerStacks);

        // The camp buffs (League Season 3).
        Aura* const crest = player->GetAura(BG_SRF_SPELL_BLUE_BUFF);
        AuraEffect const* const baron = player->GetAuraEffect(BG_SRF_SPELL_BARON_BUFF, EFFECT_0);
        if (baron)
        {
            it.Damage += float(baron->GetAmount());
            it.AbilityPower += float(baron->GetAmount());
        }

        float const health = cs->Health + cs->HealthPerLevel * l + it.Health;
        float const armor = cs->Armor + cs->ArmorPerLevel * l + cs->RuneArmor + it.Armor;
        float const resist = cs->Resist + (cs->ResistPerLevel + cs->RuneResistPerLevel) * l + it.Resist;
        // Atma's Impaler: a share of the champion's League maximum health as attack damage.
        float const damage = cs->Damage + cs->DamagePerLevel * l + cs->RuneDamage + it.Damage + own(Unique::AdPerHealth) / 100.0f * health;
        float abilityPower = cs->RuneAbilityPower + cs->RuneAbilityPowerPerLevel * l + it.AbilityPower;
        if (items.Spells.count(SPELL_DEATHCAP))
            abilityPower *= 1.3f;
        // Frozen Heart's aura takes its share off the champion's whole attack speed.
        float const attackSpeed = std::min(RiftLeague::ATTACK_SPEED_CAP,
            cs->AttackSpeed * (1.0f + cs->AttackSpeedPerLevel * (l - 1.0f) + it.AttackSpeed / 100.0f)
            * std::max(0.0f, 1.0f - near(Unique::AuraEnemyAs) / 100.0f));

        // health: WoW's own is the class's base health and its base stamina's
        float const naturalHealth = (player->GetFlatModifierValue(UNIT_MOD_HEALTH, BASE_VALUE) + float(player->GetCreateHealth()))
            * player->GetPctModifierValue(UNIT_MOD_HEALTH, BASE_PCT) + HealthFromStamina(player->GetCreateStat(STAT_STAMINA));
        // through stamina, 10 health a point: an aura that changes health itself
        // (SPELL_AURA_MOD_INCREASE_HEALTH) would put health on a dead champion
        int32 const stamina = Round((s * health - naturalHealth) / 10.0f);
        // armour: the field kit's and base agility's (Player::UpdateArmor)
        float const naturalArmor = player->GetFlatModifierValue(UNIT_MOD_ARMOR, BASE_VALUE) * player->GetPctModifierValue(UNIT_MOD_ARMOR, BASE_PCT)
            + player->GetCreateStat(STAT_AGILITY) * 2.0f;
        int32 const armorGap = Round(RiftLeague::ToWowArmor(armor, level) - naturalArmor);
        int32 const resistance = Round(RiftLeague::ToWowResist(resist));

        // basic attacks: League's damage a hit, League's attacks a second
        float const perHit = s * damage;
        Item const* ranged = player->GetWeaponForAttack(RANGED_ATTACK, true);
        bool const shoots = ranged != nullptr;
        bool const shootsWithPower = shoots && ranged->GetTemplate()->SubClass != ITEM_SUBCLASS_WEAPON_WAND;
        int32 const meleePower = Round(AttackPowerFor(player, BASE_ATTACK, perHit) - BaseAttackPower(player, false));
        int32 const rangedPower = shootsWithPower ? Round(AttackPowerFor(player, RANGED_ATTACK, perHit) - BaseAttackPower(player, true)) : 0;
        int32 const meleeHaste = Round((attackSpeed * player->GetAPMultiplier(BASE_ATTACK, false) - 1.0f) * 100.0f);
        int32 const rangedHaste = shoots ? Round((attackSpeed * player->GetAPMultiplier(RANGED_ATTACK, false) - 1.0f) * 100.0f) : 0;
        int32 const crit = Round(it.Crit - BaseAgilityCrit(player));
        int32 const spellPower = Round(s * abilityPower);

        // mana: League's mana items as the same share of this champion's own pool
        int32 mana = 0, manaRegen = 0;
        if (player->GetPowerType() == POWER_MANA)
        {
            float const leagueMana = cs->Mana + cs->ManaPerLevel * l;
            float const natural = float(player->GetCreateMana()) + ManaFromIntellect(player->GetCreateStat(STAT_INTELLECT));
            float const ratio = leagueMana > 0.0f ? natural / leagueMana : 0.0f;
            mana = Round(it.Mana * ratio);
            float const pool = float(player->GetMaxPower(POWER_MANA));
            float regen = it.ManaRegen * ratio + (crest ? CREST_MANA_PER_5 * ratio + CREST_POOL_PER_5 * pool : 0.0f)
                + (baron ? BARON_MANA_PER_5 * pool : 0.0f);
            // Mana Font (Chalice of Harmony, Athene's): +1% for every 1% of mana missing.
            if (own(Unique::ManaFont) > 0.0f && pool > 0.0f)
                regen *= 2.0f - float(player->GetPower(POWER_MANA)) / pool;
            manaRegen = Round(regen);
        }
        // Health per 5 sec (League's unit) on SPELL_AURA_MOD_HEALTH_REGEN_IN_COMBAT,
        // which adds its whole amount at every 2 sec regeneration tick
        // (Player::RegenerateHealth): 2/5 of it. Carried whole, the items' health
        // regeneration came out 2.5 times League's.
        int32 const healthRegen = Round(0.4f * (s * it.HealthRegen + (baron ? BARON_HEALTH_PER_5 * float(player->GetMaxHealth()) : 0.0f))
            * (1.0f + own(Unique::HealAmp) / 100.0f));      // Spirit Visage
        // The Crest's energy: 0.5% of the pool a second, on WoW's 10 a second.
        if (AuraEffect* energy = crest ? crest->GetEffect(EFFECT_1) : nullptr)
        {
            int32 const want = player->GetPowerType() == POWER_ENERGY ? Round(0.05f * float(player->GetMaxPower(POWER_ENERGY))) : 0;
            if (energy->GetAmount() != want)
                energy->ChangeAmount(want);
        }
        // magic penetration stays in League's points: no WoW spell penetration
        // on the carrier, Unit::CalculateAverageResistReduction takes it off the
        // target's League magic resist (MagicPenetration)
        state.MagicPen = cs->RuneMagicPen + it.MagicPen + own(Unique::MagicPen);

        // Cooldown reduction: the items', the Crest's and Mejai's at 20 stacks, League's 40% at most.
        state.CooldownReduction = std::min(COOLDOWN_REDUCTION_CAP, own(Unique::CooldownReduction)
            + (crest && crest->GetEffect(EFFECT_0) ? float(std::max(0, crest->GetEffect(EFFECT_0)->GetAmount())) : 0.0f)
            + (own(Unique::TakedownAp) > 0.0f && state.MejaiStacks >= TAKEDOWN_MAX_STACKS ? MEJAI_FULL_CDR : 0.0f));

        // Boots of Mobility: their speed rises out of combat.
        float bootsSpeed = items.BootsSpeed;
        if (own(Unique::OutOfCombatSpeed) > 0.0f && !player->IsInCombat())
            bootsSpeed += own(Unique::OutOfCombatSpeed) / LEAGUE_BASE_MOVE * 100.0f;
        state.GearResist = it.Resist;

        // League's numbers as League's stat panel shows them; WoW's attack power
        // is only the dial that makes a basic attack hit for League's damage.
        state.StatsLine = Trinity::StringFormat("RIFTSTATS:{}:{}:{}:{}:{:.1f}", Round(damage), Round(abilityPower),
            Round(armor), Round(resist), it.LifeSteal);

        // A champion keeps its share of health as its maximum moves.
        float const healthPct = player->GetHealthPct();
        uint32 const maxBefore = player->GetMaxHealth();
        SetCarrier(player, SPELL_CHAMPION_A, { stamina, armorGap, resistance });
        if (player->IsAlive() && player->GetMaxHealth() != maxBefore)
            player->SetHealth(std::max<uint32>(1, uint32(float(player->GetMaxHealth()) * healthPct / 100.0f)));
        SetCarrier(player, SPELL_CHAMPION_B, { meleePower, rangedPower, crit });
        SetCarrier(player, SPELL_CHAMPION_C, { meleeHaste, rangedHaste, spellPower });
        SetCarrier(player, SPELL_CHAMPION_D, { spellPower, mana, healthRegen });
        SetCarrier(player, SPELL_CHAMPION_E, { manaRegen, 0, Round(it.MoveSpeed) });
        // the held items' crit is spell crit too: on top of the character's own,
        // where the melee crit above replaces it
        SetCarrier(player, SPELL_CHAMPION_F, { Round(bootsSpeed), Round(it.LifeSteal * 10.0f), Round(it.Crit) });
        SetCarrier(player, SPELL_COOLDOWN_CARRIER, { Round(state.CooldownReduction) });
    }

    void ShowBloodthirster(Player* player, State const& state)
    {
        Aura* aura = player->GetAura(SPELL_BLOODTHIRSTER_STACK);
        if (!state.BloodthirsterStacks)
        {
            if (aura)
                player->RemoveAurasDueToSpell(SPELL_BLOODTHIRSTER_STACK);
            return;
        }
        if (!aura)
            aura = player->AddAura(SPELL_BLOODTHIRSTER_STACK, player);
        if (aura && aura->GetStackAmount() != state.BloodthirsterStacks)
            aura->SetStackAmount(state.BloodthirsterStacks);
    }

    void SyncItemsFor(Player* player, State& state)
    {
        std::shared_ptr<RiftLeague::Tables const> const tables = RiftLeague::Get();
        ItemEffects const want = HeldItemEffects(player, *tables);

        for (auto itr = state.ItemSpells.begin(); itr != state.ItemSpells.end();)
        {
            if (!want.Spells.count(itr->first))
            {
                player->RemoveAurasDueToSpell(itr->first);
                itr = state.ItemSpells.erase(itr);
            }
            else
                ++itr;
        }
        for (auto const& [spell, count] : want.Spells)
        {
            Aura* aura = player->GetAura(spell);
            auto const had = state.ItemSpells.find(spell);
            if (aura && had != state.ItemSpells.end() && had->second == count)
                continue;
            if (!aura)
                aura = player->AddAura(spell, player);
            state.ItemSpells[spell] = count;
            if (!aura || count == 1)
                continue;
            for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
                if (AuraEffect* effect = aura->GetEffect(i))
                    effect->ChangeAmount(aura->GetSpellInfo()->GetEffect(SpellEffIndex(i)).CalcValue(player) * int32(count));
        }

        if (!player->GetItemCount(ITEM_BLOODTHIRSTER, false))
            state.BloodthirsterStacks = 0;
        ShowBloodthirster(player, state);
        // League: the stacks go with the item (sold, or built into another).
        using RiftLeague::Unique;
        if (want.Unique[size_t(Unique::TakedownAp)] <= 0.0f)
            state.MejaiStacks = 0;
        if (want.Unique[size_t(Unique::TakedownAd)] <= 0.0f)
            state.OccultStacks = 0;
        if (want.Unique[size_t(Unique::UnitKillArmorAp)] <= 0.0f)
            state.SeekerStacks = 0;
        ShowStacks(player, SPELL_MEJAI_STACK, state.MejaiStacks);
        ShowStacks(player, SPELL_OCCULT_STACK, state.OccultStacks);
        ShowStacks(player, SPELL_SEEKER_STACK, state.SeekerStacks);
        SetChampionCarriers(player, state, want, *tables);

        // The steed held is the champion's riding: Plainsrunning follows it, and a
        // steed no longer held (sold, built into the swift one) stops carrying it.
        uint16 const riding = MatchRiding(player);
        if (riding != state.Riding)
        {
            state.Riding = riding;
            if (riding < 150)
                player->RemoveAurasDueToSpell(SPELL_SWIFT_STEED);
            if (riding < 75)
                player->RemoveAurasDueToSpell(SPELL_STEED);
            if (player->HasAura(SPELL_PLAINSRUNNING))
                player->UpdateSpeed(MOVE_RUN);
        }
    }

    void TakeItemAuras(Player* player, State& state)
    {
        for (uint32 spell : CHAMPION_CARRIERS)
            player->RemoveAurasDueToSpell(spell);
        player->RemoveAurasDueToSpell(SPELL_BLOODTHIRSTER_STACK);
        state.BloodthirsterStacks = 0;
        for (uint32 spell : { SPELL_COOLDOWN_CARRIER, SPELL_MEJAI_STACK, SPELL_OCCULT_STACK, SPELL_SEEKER_STACK })
            player->RemoveAurasDueToSpell(spell);
        state.MejaiStacks = state.OccultStacks = state.SeekerStacks = 0;
        state.Unique = { };
        state.Emits.clear();
        state.ReceivedSpellVamp = state.CooldownReduction = 0.0f;
        player->RemoveAurasDueToSpell(SPELL_STEED);
        player->RemoveAurasDueToSpell(SPELL_SWIFT_STEED);
        player->RemoveAurasDueToSpell(SPELL_RIFT_FOCUS);
        state.Riding = 0;
        for (auto const& [spell, count] : state.ItemSpells)
            player->RemoveAurasDueToSpell(spell);
        state.ItemSpells.clear();
    }

    // A button holding a rank the spellbook does not list - one not unlocked yet
    // (AdjustCast would cast the unlocked one anyway), or an old rank a new one
    // replaced - is pointed at the best unlocked rank. A lower rank of a spell
    // whose ranks stack stays where the player put it. RestoreButtons puts the
    // real buttons back.
    bool RetargetButtons(Player* player, State const& state)
    {
        bool changed = false;
        for (uint8 button = 0; button < MAX_ACTION_BUTTONS; ++button)
        {
            ActionButton const* ab = player->GetActionButton(button);
            if (!ab || ab->GetType() != ACTION_BUTTON_SPELL)
                continue;
            auto const chain = state.SpellToChain.find(ab->GetAction());
            if (chain == state.SpellToChain.end())
                continue;
            auto const unlocked = state.Unlocked.find(chain->second);
            uint8 const have = unlocked != state.Unlocked.end() ? unlocked->second : 0;
            if (!have)
                continue;
            auto const known = player->GetSpellMap().find(ab->GetAction());
            bool const active = known != player->GetSpellMap().end() && known->second.active;
            if (MatchShows(state, ab->GetAction(), active))
                continue;
            uint32 const want = state.Chains.at(chain->second).Ranks[have - 1];
            if (want != ab->GetAction() && player->HasSpell(want) && player->addActionButton(button, want, ACTION_BUTTON_SPELL))
                changed = true;
        }
        return changed;
    }

    // The recall comes off the bars here (the caller sends them) and out of the
    // spellbook in ForgetRecall, after the bars: the client clears a button
    // whose spell it is told it lost, and tells the server to.
    void TakeRecallButtons(Player* player)
    {
        for (uint8 button = 0; button < MAX_ACTION_BUTTONS; ++button)
            if (ActionButton const* ab = player->GetActionButton(button))
                if (ab->GetType() == ACTION_BUTTON_SPELL && ab->GetAction() == SPELL_RIFT_RECALL)
                    player->removeActionButton(button);
    }

    void ForgetRecall(Player* player)
    {
        if (player->HasSpell(SPELL_RIFT_RECALL))
            player->RemoveSpell(SPELL_RIFT_RECALL, false, false);
        // RemoveSpell is only written at the next save; a crash before it must
        // not leave the recall in the spellbook.
        CharacterDatabase.PExecute("DELETE FROM character_spell WHERE guid = {} AND spell = {}",
            player->GetGUID().GetCounter(), SPELL_RIFT_RECALL);
    }

    std::string_view Trimmed(std::string_view text)
    {
        while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r'))
            text.remove_prefix(1);
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
            text.remove_suffix(1);
        return text;
    }

    // The spells a macro on the bars casts, by lower-case name, read from the
    // macros the client keeps in its account data (settings sync): the account's
    // (GLOBAL_MACROS_CACHE) behind a MACRO button, the character's
    // (PER_CHARACTER_MACROS_CACHE, ids 0x01000000 + the button's action) behind a
    // CMACRO one. Each is "MACRO <id> "<name>" <icon>", its lines, "END"; the
    // names are what /cast, /use, /castsequence and /castrandom take, with the
    // [conditions], "!", "reset=" and "(Rank n)" off and slot numbers skipped.
    // False when the client keeps no such macro.
    bool MacroSpellNames(Player* player, uint8 type, uint32 action, std::vector<std::string>& names)
    {
        WorldSession* session = player->GetSession();
        if (!session)
            return false;
        bool const own = type == ACTION_BUTTON_CMACRO;
        AccountData const* data = session->GetAccountData(own ? PER_CHARACTER_MACROS_CACHE : GLOBAL_MACROS_CACHE);
        std::string const wanted = std::to_string(own ? 0x01000000u + action : action);
        bool inside = false;
        for (std::string_view line : Trinity::Tokenize(data->Data, '\n', false))
        {
            line = Trimmed(line);
            if (!inside)
            {
                if (line.substr(0, 6) == "MACRO ")
                {
                    std::string_view const rest = line.substr(6);
                    inside = rest.substr(0, rest.find(' ')) == wanted;
                }
                continue;
            }
            if (line == "END")
                return true;
            if (line.empty() || line.front() != '/')
                continue;
            size_t const space = line.find(' ');
            if (space == std::string_view::npos)
                continue;
            std::string const command = Normalised(std::string(line.substr(0, space)));
            if (command != "/cast" && command != "/use" && command != "/castsequence" && command != "/castrandom")
                continue;
            std::string plain;
            int32 depth = 0;
            for (char c : line.substr(space + 1))
            {
                if (c == '[')
                    ++depth;
                else if (c == ']')
                    depth = std::max(0, depth - 1);
                else if (!depth)
                    plain += (c == ';' ? ',' : c);
            }
            for (std::string_view piece : Trinity::Tokenize(plain, ',', false))
            {
                piece = Trimmed(piece);
                if (piece.substr(0, 6) == "reset=")
                    piece = Trimmed(piece.find(' ') == std::string_view::npos ? std::string_view() : piece.substr(piece.find(' ')));
                if (!piece.empty() && piece.front() == '!')
                    piece.remove_prefix(1);
                piece = Trimmed(piece.substr(0, piece.find('(')));
                if (piece.empty() || piece.find_first_not_of("0123456789 ") == std::string_view::npos)
                    continue;
                names.push_back(Normalised(std::string(piece)));
            }
        }
        return false;
    }

    // A macro whose spells are all unlocked (a name that is none of the
    // character's spells - an item, a pet's ability - holds nothing back).
    bool MacroReady(Player* player, State const& state, uint8 type, uint32 action)
    {
        std::vector<std::string> names;
        if (!MacroSpellNames(player, type, action, names))
            return false;
        for (std::string const& name : names)
        {
            auto const spells = state.SpellsByName.find(name);
            if (spells == state.SpellsByName.end())
                continue;
            if (std::none_of(spells->second.begin(), spells->second.end(),
                [&](uint32 spell) { return !HiddenInMatch(player, state, spell); }))
                return false;
        }
        return true;
    }

    // Every button the character walked in with whose spell the match has
    // given back - a pick, a talent re-learned - returns to where it was, and a
    // macro once every spell it casts has. A button the player clears afterwards
    // stays cleared (Returned).
    bool RefillSavedButtons(Player* player, State& state)
    {
        bool changed = false;
        for (SavedButton const& b : state.RealButtons)
        {
            if (state.Returned.count(b.Button) || player->GetActionButton(b.Button))
                continue;
            bool ready = false;
            if (b.Type == ACTION_BUTTON_SPELL)
                ready = !HiddenInMatch(player, state, b.Action);
            else if (b.Type == ACTION_BUTTON_MACRO || b.Type == ACTION_BUTTON_CMACRO)
                ready = MacroReady(player, state, b.Type, b.Action);
            if (ready && player->addActionButton(b.Button, b.Action, b.Type))
            {
                state.Returned.insert(b.Button);
                changed = true;
            }
        }
        return changed;
    }

    bool OnBars(Player* player, State const& state, uint32 first, uint32 spell)
    {
        for (uint8 button = 0; button < MAX_ACTION_BUTTONS; ++button)
            if (ActionButton const* ab = player->GetActionButton(button))
                if (ab->GetType() == ACTION_BUTTON_SPELL)
                {
                    if (ab->GetAction() == spell)
                        return true;
                    auto const chain = state.SpellToChain.find(ab->GetAction());
                    if (first && chain != state.SpellToChain.end() && chain->second == first)
                        return true;
                    // a talent's own rank (Mortal Strike 1): no chain holds it, its ranks' chain is keyed by it
                    if (first && ab->GetAction() == first)
                        return true;
                }
        return false;
    }

    // A chain's first unlock puts the spell on the bars: back on its own button,
    // or not at all when a macro on the bars casts it, otherwise on the first free
    // button of the usual bars that no spell still to come back held - the recall and earlier picks took the first free
    // buttons, which were often another ability's, and that one then landed
    // somewhere new.
    // A macro on the bars that casts this spell (by name): it stands in for the
    // spell's own button.
    bool MacroOnBarsCasts(Player* player, uint32 spell)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
        if (!info || !info->SpellName[LOCALE_enUS])
            return false;
        std::string const name = Normalised(info->SpellName[LOCALE_enUS]);
        for (uint8 button = 0; button < MAX_ACTION_BUTTONS; ++button)
        {
            ActionButton const* ab = player->GetActionButton(button);
            if (!ab || (ab->GetType() != ACTION_BUTTON_MACRO && ab->GetType() != ACTION_BUTTON_CMACRO))
                continue;
            std::vector<std::string> names;
            if (MacroSpellNames(player, uint8(ab->GetType()), ab->GetAction(), names)
                && std::find(names.begin(), names.end(), name) != names.end())
                return true;
        }
        return false;
    }

    void PlaceUnlockedSpell(Player* player, State& state, uint32 first, uint32 spell)
    {
        RefillSavedButtons(player, state);
        // a macro that casts it, put back just now or already there, is its button
        if (!OnBars(player, state, first, spell) && !MacroOnBarsCasts(player, spell))
        {
            std::unordered_set<uint8> reserved;
            for (SavedButton const& b : state.RealButtons)
                reserved.insert(b.Button);
            for (bool strict : { true, false })
            {
                for (uint8 start : BAR_ORDER)
                    for (uint8 button = start; button < start + 12; ++button)
                        if (!player->GetActionButton(button) && !(strict && reserved.count(button))
                            && player->addActionButton(button, spell, ACTION_BUTTON_SPELL))
                        {
                            state.Returned.insert(button);
                            return;
                        }
            }
        }
    }

    // What the 3.3.5 client does to its bars on its own (Wow.exe 12340), each
    // time telling the server with a CMSG_SET_ACTION_BUTTON:
    //  - SMSG_ACTION_BUTTONS (handler 0x6D8750): every button whose spell is
    //    not in its spellbook is cleared - and every empty one, "cleared"
    //    again (ClearAction 0x5AAA90 -> 0x5AA390 sends the opcode).
    //  - SMSG_REMOVED_SPELL (0x6E7840 -> 0x5AAB90): every button holding the
    //    spell is cleared.
    //  - SMSG_LEARNED_SPELL (0x6E7B00 -> 0x5AAFD0): a button holding the
    //    same-named spell one rank lower is moved up to the new rank.
    // A pick used to send the bars twice, the saved rank first and the
    // unlocked one after: the client showed the unlocked rank, and then its
    // "that button is empty" for the first packet came in and emptied the
    // server's. The next bars sent (the next pick) took the spell off the
    // client too - Raptor Strike, gone when Wing Clip was learned.
    //
    // So every change to the bars or the spellbook is sent once, at the end,
    // in the order the client needs: the spells it gains, then the bars (each
    // button a spell it knows by then), then the spells it loses. A button
    // filled here keeps its spell through the client's late "empty" for the
    // packet before (KeepsButton).
    uint32 constexpr FILLED_BUTTON_GUARD_MS = 3000;

    void PublishBars(Player* player, State& state)
    {
        SyncClientSpellbook(player, state, true);
        RetargetButtons(player, state);
        uint32 const now = GameTime::GetGameTimeMS();
        for (uint8 button = 0; button < MAX_ACTION_BUTTONS; ++button)
        {
            ActionButton const* ab = player->GetActionButton(button);
            uint32 const packed = ab ? ab->packedData : 0;
            auto const sent = state.SentBars.find(button);
            if (packed && (sent == state.SentBars.end() || sent->second != packed))
                state.FilledAt[button] = now;
            if (packed)
                state.SentBars[button] = packed;
            else
                state.SentBars.erase(button);
        }
        player->SendActionButtons(1);
        SyncClientSpellbook(player, state, false);
    }

    // ------------------------------------------------------------- poisons
    //
    // A rogue's poisons (owner, 2026-10-22: "rogues need a way to apply poisons
    // in summoner's rift", "make the rogues learn it for the same levels they
    // learned it from the poison trainers", "they should learn the actual apply
    // poison spells (11202 for example)", "the player should have to spend a
    // skill point on it"). The bags are shut in a match, so the vials a rogue
    // carries cannot be used. The coats the vials cast - the stock apply spells,
    // item-targeted as ever (cast, then click a weapon on the Character pane) -
    // are skill picks instead: each poison a chain of its own, each rank offered
    // from the level the classic poison trainers taught it (the spells' own
    // SpellLevel, which Offers reads). They are learned at the door, dependent -
    // never written down, so a crash mid-match leaves nothing behind - and the
    // match hides and refuses them until picked, like every class spell.
    struct PoisonRank
    {
        uint8 Level;     // the trainer's level = the apply spell's SpellLevel
        uint32 Spell;
    };
    std::vector<PoisonRank> const POISON_LADDERS[] =
    {
        { { 20, 8679 }, { 28, 8686 }, { 36, 8688 }, { 44, 11338 }, { 52, 11339 }, { 60, 11340 } },  // Instant Poison
        { { 30, 2823 }, { 38, 2824 }, { 46, 11355 }, { 54, 11356 }, { 60, 25351 } },                // Deadly Poison
        { { 20, 3408 }, { 50, 11202 } },                                                             // Crippling Poison
        { { 24, 5761 }, { 38, 8693 }, { 52, 11399 } },                                               // Mind-numbing Poison
        { { 32, 13219 }, { 40, 13225 }, { 48, 13226 }, { 56, 13227 } },                              // Wound Poison
    };
    size_t constexpr POISON_INSTANT = 0;
    size_t constexpr POISON_DEADLY = 1;
    uint32 constexpr POISON_COAT_MS = HOUR * IN_MILLISECONDS;    // Spell::EffectEnchantItemTmp's rogue hour

    // At the door (after BuildState), and again on a relog into the match (a
    // dependent spell is not loaded back): every coat learned - hidden until
    // picked - and one chain per poison for the picks.
    void AddPoisonChains(Player* player, State& state)
    {
        if (player->GetClass() != CLASS_ROGUE || !state.Persistent)
            return;
        for (std::vector<PoisonRank> const& ladder : POISON_LADDERS)
        {
            uint32 const first = ladder.front().Spell;
            Chain& chain = state.Chains[first];
            bool const fresh = chain.Ranks.empty();
            for (PoisonRank const& rank : ladder)
            {
                if (!sSpellMgr->GetSpellInfo(rank.Spell))
                    continue;
                if (!player->HasSpell(rank.Spell))
                {
                    player->LearnSpell(rank.Spell, true);
                    state.MatchPoisons.insert(rank.Spell);
                }
                if (fresh)
                {
                    chain.Ranks.push_back(rank.Spell);
                    state.SpellToChain[rank.Spell] = first;
                }
            }
            if (chain.Ranks.empty())
                state.Chains.erase(first);
        }
    }

    // The rank of a poison a level has reached; nullptr before its first.
    PoisonRank const* BestPoison(std::vector<PoisonRank> const& ladder, uint8 level)
    {
        PoisonRank const* best = nullptr;
        for (PoisonRank const& rank : ladder)
            if (rank.Level <= level)
                best = &rank;
        return best;
    }

    // A bot cannot click a weapon on a Character pane, nor spend a pick on a
    // coat: its weapons are coated for it as it levels (Instant on the main hand,
    // Deadly - Instant before 30 - on the off hand), with what the coat spell
    // itself would lay down.
    void CoatBotWeapon(Player* player, uint8 slot, uint32 coatSpell)
    {
        uint32 enchant = 0;
        if (SpellInfo const* info = sSpellMgr->GetSpellInfo(coatSpell))
            for (SpellEffectInfo const& effect : info->GetEffects())
                if (effect.Effect == SPELL_EFFECT_ENCHANT_ITEM_TEMPORARY)
                    enchant = uint32(effect.MiscValue);
        Item* weapon = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!enchant || !weapon || weapon->GetTemplate()->Class != ITEM_CLASS_WEAPON
            || weapon->GetEnchantmentId(TEMP_ENCHANTMENT_SLOT) == enchant)
            return;
        player->ApplyEnchantment(weapon, TEMP_ENCHANTMENT_SLOT, false);
        weapon->SetEnchantment(TEMP_ENCHANTMENT_SLOT, enchant, POISON_COAT_MS, 0, player->GetGUID());
        player->ApplyEnchantment(weapon, TEMP_ENCHANTMENT_SLOT, true);
    }

    void CoatBotPoisons(Player* player, State const& state)
    {
        if (player->GetClass() != CLASS_ROGUE || state.Persistent)
            return;
        if (PoisonRank const* instant = BestPoison(POISON_LADDERS[POISON_INSTANT], state.MatchLevel))
        {
            PoisonRank const* deadly = BestPoison(POISON_LADDERS[POISON_DEADLY], state.MatchLevel);
            CoatBotWeapon(player, EQUIPMENT_SLOT_MAINHAND, instant->Spell);
            CoatBotWeapon(player, EQUIPMENT_SLOT_OFFHAND, deadly ? deadly->Spell : instant->Spell);
        }
    }

    // The way out, before the real bars go back: the coats' buttons, then the
    // coats the match taught - before ResetClientSpellbook, which hands the
    // client back every hidden spell the character still knows.
    void ForgetPoisons(Player* player, State& state)
    {
        for (uint8 button = 0; button < MAX_ACTION_BUTTONS; ++button)
            if (ActionButton const* ab = player->GetActionButton(button))
                if (ab->GetType() == ACTION_BUTTON_SPELL && state.MatchPoisons.count(ab->GetAction()))
                    player->removeActionButton(button);
        for (uint32 spell : state.MatchPoisons)
            if (player->HasSpell(spell))
                player->RemoveSpell(spell, false, false);
        state.MatchPoisons.clear();
        // and the talent ranks the match taught (a button holding one goes back to
        // the real bars' own on the way out)
        for (uint8 button = 0; button < MAX_ACTION_BUTTONS; ++button)
            if (ActionButton const* ab = player->GetActionButton(button))
                if (ab->GetType() == ACTION_BUTTON_SPELL && state.MatchRanks.count(ab->GetAction()))
                    player->removeActionButton(button);
        for (uint32 spell : state.MatchRanks)
            if (player->HasSpell(spell))
                player->RemoveSpell(spell, false, false);
        state.MatchRanks.clear();
    }

    // A relog into the match: a dependent spell is not loaded back, so the talent
    // ranks it picked that the character never trained are taught again.
    void RelearnMatchRanks(Player* player, State& state)
    {
        for (auto const& [first, chain] : state.Chains)
        {
            if (!chain.Talent)
                continue;
            auto const itr = state.Unlocked.find(first);
            uint8 const have = itr != state.Unlocked.end() ? itr->second : 0;
            for (uint32 k = 0; k < have && k < chain.Ranks.size(); ++k)
                if (state.MatchRanks.count(chain.Ranks[k]) && !player->HasSpell(chain.Ranks[k]))
                    player->LearnSpell(chain.Ranks[k], true);
        }
    }

    // An addon whisper from the character to itself, as Mok'gora's are: the
    // client hands CENTURION_Rift its own name as the sender.
    void SendLine(Player* player, std::string const& line)
    {
        if (IsTransient(player) || !player->GetSession())
            return;
        WorldPacket data;
        ChatHandler::BuildChatPacket(data, CHAT_MSG_WHISPER, LANG_ADDON, player->GetGUID(), player->GetGUID(),
            "CCGAME\t" + line, 0, player->GetName(), player->GetName());
        player->SendDirectMessage(&data);
    }

    void SendState(Player* player, State const& state)
    {
        SendLine(player, Trinity::StringFormat("RIFT:{}:{}:{}:{}", state.MatchLevel, state.RealLevel, state.LeagueLevel, state.PendingPicks));
        std::string offers;
        if (state.PendingPicks)
        {
            std::vector<uint32> const list = Offers(state);
            for (size_t i = 0; i < list.size() && i < MAX_OFFERS_SENT; ++i)
                offers += (i ? "," : "") + std::to_string(list[i]);
        }
        SendLine(player, "RIFTPICKS:" + offers);
    }

    // League progress 0..1: level 1 is 0, level 18 is 1.
    float LeagueProgress(State const& state)
    {
        return (float(state.LeagueLevel - 1)
            + (state.LeagueLevel < LEAGUE_MAX_LEVEL ? state.LeagueXp / XpToNext(state.LeagueLevel) : 0.0f)) / float(LEAGUE_MAX_LEVEL - 1);
    }

    // The match level League progress has reached: the start at League 1, the
    // real level at League 18.
    float MatchLevelFor(State const& state)
    {
        return float(state.StartLevel) + LeagueProgress(state) * float(state.RealLevel - state.StartLevel);
    }

    // The XP bar shows the way to the next match level.
    void ShowProgress(Player* player, State const& state)
    {
        float const frac = std::clamp(MatchLevelFor(state) - float(state.MatchLevel), 0.0f, 1.0f);
        player->SetUInt32Value(PLAYER_XP, uint32(frac * float(player->GetUInt32Value(PLAYER_NEXT_LEVEL_XP))));
    }

    // The field kit is issued for the 5-level bracket the match has reached
    // (10, 15, 20 ...), never above the real level, and swapped as it climbs.
    uint8 constexpr KIT_BRACKET = 5;
    uint8 KitLevelFor(State const& state)
    {
        uint8 const bracket = uint8(state.MatchLevel - state.MatchLevel % KIT_BRACKET);
        return std::min<uint8>(std::max<uint8>(bracket, state.StartLevel), state.RealLevel);
    }

    void SetMatchLevel(Player* player, State& state, uint8 level)
    {
        while (state.MatchLevel < level)
        {
            ++state.MatchLevel;
            state.PendingPicks += PICKS_PER_LEVEL;
        }
        // InitTalentForLevel hands out the new level's talent point. A level heals
        // nothing in the Rift (owner, 2026-10-20): health and mana stay what they
        // were, only the maximums grow.
        uint32 const health = player->GetHealth();
        uint32 const mana = player->GetPower(POWER_MANA);
        // and the pet's: GiveLevel levels it too (Pet::SynchronizeLevelWithOwner), and a
        // pet's new level filled its health (Guardian::InitStatsForLevel) - owner,
        // 2026-10-23: "my pets are getting full healed on level up"
        Pet* pet = player->GetPet();
        bool const petAlive = pet && pet->IsAlive();
        uint32 const petHealth = petAlive ? pet->GetHealth() : 0;
        Powers const petPower = pet ? pet->GetPowerType() : POWER_MANA;
        uint32 const petPowerValue = petAlive ? pet->GetPower(petPower) : 0;
        player->GiveLevel(state.MatchLevel, true);
        player->SetHealth(std::min(std::max<uint32>(health, 1), player->GetMaxHealth()));
        if (player->GetMaxPower(POWER_MANA))
            player->SetPower(POWER_MANA, std::min<uint32>(mana, player->GetMaxPower(POWER_MANA)));
        if (petAlive && player->GetPet() == pet && pet->IsAlive())
        {
            pet->SetHealth(std::min(std::max<uint32>(petHealth, 1), pet->GetMaxHealth()));
            if (pet->GetMaxPower(petPower))
                pet->SetPower(petPower, std::min<uint32>(petPowerValue, pet->GetMaxPower(petPower)));
        }
        SyncPetRanks(player, state);
        CoatBotPoisons(player, state);
        ShowProgress(player, state);
        SendState(player, state);
        if (state.PendingPicks && !Offers(state).empty())
            ChatHandler(player->GetSession()).PSendSysMessage("You have %u skill pick(s) to spend: type /rift.", state.PendingPicks);
    }

    State* Find(Player const* player)
    {
        auto const itr = States.find(player->GetGUID().GetCounter());
        return itr != States.end() ? &itr->second : nullptr;
    }

    bool IsInRiftBattleground(Player const* player)
    {
        Battleground const* bg = player->GetBattleground();
        return bg && bg->GetTypeID(true) == BATTLEGROUND_SRF && bg->GetStatus() != STATUS_WAIT_LEAVE;
    }
}

namespace RiftProgression
{
namespace
{
    struct CrestCover;
    CrestCover const& CrestCoverOf(uint8 classId);
    CrestCover const& RangeCoverOf(uint8 classId);
}

void ProbeStorage()
{
    {
        std::lock_guard<std::recursive_mutex> lock(StateMutex);
        Whitelist.clear();
        if (WorldDatabase.Query("SELECT TABLE_NAME FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'rift_ability_whitelist'"))
            if (QueryResult rows = WorldDatabase.Query("SELECT class, name, free FROM rift_ability_whitelist"))
            {
                do
                {
                    Field* f = rows->Fetch();
                    WhitelistEntry e;
                    e.Name = Normalised(f[1].GetString());
                    if (!e.Name.empty() && e.Name.back() == '*')
                    {
                        e.Prefix = true;
                        e.Name.pop_back();
                    }
                    e.Free = f[2].GetUInt8() != 0;
                    Whitelist[f[0].GetUInt8()].push_back(e);
                } while (rows->NextRow());
            }
        TC_LOG_INFO("server.loading", ">> Summoner's Rift core abilities: lists for {} class(es).", Whitelist.size());

        Recipes.clear();
        if (WorldDatabase.Query("SELECT TABLE_NAME FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'rift_item_recipe'"))
            if (QueryResult rows = WorldDatabase.Query("SELECT item, component FROM rift_item_recipe ORDER BY item, slot"))
            {
                do
                {
                    Field* f = rows->Fetch();
                    Recipes[f[0].GetUInt32()].push_back(f[1].GetUInt32());
                } while (rows->NextRow());
            }
        TC_LOG_INFO("server.loading", ">> Summoner's Rift shop: {} item(s) with a build path.", Recipes.size());
    }
    RiftLeague::Load();
    CrestCoverOf(CLASS_WARRIOR);   // the Crest's cooldown modifiers, worked out once (CrestCooldownMod)
    RangeCoverOf(CLASS_WARRIOR);   // and Summoner's Focus's range modifiers (RangeMod)

    StoragePresent = CharacterDatabase.Query("SELECT TABLE_NAME FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'custom_rift_stash'") != nullptr;
    WeaponTablePresent = CharacterDatabase.Query("SELECT TABLE_NAME FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'custom_rift_weapons'") != nullptr;
    StashSkillsColumn = CharacterDatabase.Query("SELECT COLUMN_NAME FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'custom_rift_stash' AND COLUMN_NAME = 'skills'") != nullptr;
    StashPetColumn = CharacterDatabase.Query("SELECT COLUMN_NAME FROM information_schema.COLUMNS WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'custom_rift_stash' AND COLUMN_NAME = 'pet_spells'") != nullptr;
    if (!StoragePresent)
    {
        TC_LOG_INFO("server.loading", ">> Summoner's Rift progression off: `custom_rift_stash` is absent.");
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    Stashed.clear();
    if (QueryResult rows = CharacterDatabase.Query("SELECT guid FROM custom_rift_stash"))
        do
            Stashed.insert(rows->Fetch()[0].GetUInt32());
        while (rows->NextRow());
    TC_LOG_INFO("server.loading", ">> Summoner's Rift progression on; {} character(s) to restore.", Stashed.size());
}

bool IsInMatch(Player const* player)
{
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    return player && Find(player);
}

bool UsesMatchSkills(Player const* player)
{
    if (!player)
        return false;
    std::lock_guard<std::mutex> lock(MatchSkillMutex);
    return MatchSkillPlayers.count(player->GetGUID().GetCounter()) != 0;
}

// The bots' side (further down, before Maintain), and the stash restore (with OnLogin).
namespace
{
    void BotSpendPicks(Player* player, State& state);
    void BotSyncSpellbook(Player* player, State const& state);
    bool RestoreFromStash(Player* player);
}

void Enter(Player* player)
{
    if (!player || !StoragePresent)
        return;

    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    if (State* existing = Find(player))
    {
        // Back in the match it left: level, talents and gear are as it left them.
        // A relog's spell list was already the match's, so that is what the
        // client holds now.
        NoteLoginSpellbook(player, *existing);
        // A rogue's coats are dependent, so the relog did not load them back.
        // Learned after the loop above: the client is told each one, and
        // PublishBars takes the locked ones back out of its spellbook.
        AddPoisonChains(player, *existing);
        RelearnMatchRanks(player, *existing);
        PublishBars(player, *existing);
        SendState(player, *existing);
        return;
    }

    // A stash with no match state: the real character is in that row, not in this
    // Player (its level, talents and bars are the match's). Back from the row first -
    // writing this Player into the stash overwrote Forcskin's build with an empty one.
    if (Stashed.count(player->GetGUID().GetCounter()))
    {
        TC_LOG_ERROR("bg.battleground", "Rift: {} enters with a stash but no match state; restoring the stash first.", player->GetName());
        RestoreFromStash(player);
    }

    State& state = States[player->GetGUID().GetCounter()];
    state.RealLevel = player->GetLevel();
    state.RealXp = player->GetUInt32Value(PLAYER_XP);
    state.RealMoney = player->GetMoney();
    state.Persistent = !IsTransient(player);
    BuildState(player, state);
    AddPoisonChains(player, state);
    for (auto const& [spell, known] : player->GetSpellMap())
        if (known.state != PLAYERSPELL_REMOVED && !known.disabled)
            if (SpellInfo const* info = sSpellMgr->GetSpellInfo(spell); info && info->SpellName[LOCALE_enUS])
                state.SpellsByName[Normalised(info->SpellName[LOCALE_enUS])].push_back(spell);
    state.RealButtons = SnapshotButtons(player);
    if (state.Persistent && WeaponTablePresent)
        if (QueryResult row = CharacterDatabase.PQuery("SELECT shape, subclass FROM custom_rift_weapons WHERE guid = {}", player->GetGUID().GetCounter()))
        {
            Field* f = row->Fetch();
            if (f[0].GetUInt8() <= uint8(Tournament::RiftWeaponShape::OneHand))
                state.Weapons = { Tournament::RiftWeaponShape(f[0].GetUInt8()), f[1].GetInt32() };
        }

    // A world character's skills follow the tournament's rules in the match - its weapons
    // at the match level's maximum - and come back as they were (RestoreSkills).
    if (!Tournament::IsTournamentCharacter(player))
    {
        state.RealSkills = SnapshotSkills(player);
        SetMatchSkills(player, true);
    }

    // Written down before anything changes: from here on every way out restores.
    if (state.Persistent)
    {
        CharacterDatabase.DirectPExecute("REPLACE INTO custom_rift_stash (guid, real_level, real_xp, money, talents, actions) VALUES ({}, {}, {}, {}, '{}', '{}')",
            player->GetGUID().GetCounter(), uint32(state.RealLevel), state.RealXp, state.RealMoney, TalentsToString(state.RealTalents),
            ButtonsToString(state.RealButtons));
        if (StashSkillsColumn)
            CharacterDatabase.DirectPExecute("UPDATE custom_rift_stash SET skills = '{}' WHERE guid = {}",
                SkillsToString(state.RealSkills), player->GetGUID().GetCounter());
        Stashed.insert(player->GetGUID().GetCounter());
    }

    // The temporal gear - the field set issued for the level the match starts at
    // - goes on while the character still has its real level to equip it with.
    Tournament::ApplyRiftLoadout(player, std::min<uint8>(START_LEVEL, state.RealLevel), state.Weapons);
    player->SetMoney(START_GOLD * COPPER_PER_LEAGUE_GOLD);
    player->ResetTalents(true);

    // Only what the core list makes free comes with it (stances, forms, the
    // pet's call); everything else is a skill pick - one now, one a level.
    state.StartLevel = std::min<uint8>(START_LEVEL, state.RealLevel);
    state.MatchLevel = state.StartLevel;
    state.KitApplied = KitLevelFor(state);   // ApplyRiftLoadout dressed it for the start
    for (uint32 first : state.FreeChains)
        state.Unlocked[first] = uint8(state.Chains.at(first).Ranks.size());
    state.PendingPicks = PICKS_PER_LEVEL;
    player->GiveLevel(state.StartLevel, true);
    SyncPetRanks(player, state);
    if (!state.Persistent)
    {
        BotSpendPicks(player, state);
        BotSyncSpellbook(player, state);
    }
    player->SendTalentsInfoData(false);
    HideLockedButtons(player, state);
    for (SavedButton const& b : state.RealButtons)
        if (player->GetActionButton(b.Button))
            state.Returned.insert(b.Button);
    if (!player->HasSpell(SPELL_RIFT_RECALL))
        player->LearnSpell(SPELL_RIFT_RECALL, false);
    PlaceUnlockedSpell(player, state, 0, SPELL_RIFT_RECALL);
    PublishBars(player, state);
    CoatBotPoisons(player, state);
    SyncItemsFor(player, state);
    player->SetFullHealth();
    player->SetFullPower(player->GetPowerType());
    ShowProgress(player, state);
    SendState(player, state);

    TC_LOG_DEBUG("bg.battleground", "Rift: {} enters at level {} (real {}), {} of {} spell steps given, {} talents put aside.",
        player->GetName(), uint32(state.StartLevel), uint32(state.RealLevel), UnlockedSteps(state), TotalSteps(state), state.RealTalents.size());
}

void Leave(Player* player)
{
    if (!player)
        return;

    State state;
    {
        std::lock_guard<std::recursive_mutex> lock(StateMutex);
        auto const itr = States.find(player->GetGUID().GetCounter());
        if (itr == States.end())
            return;
        state = std::move(itr->second);
        States.erase(itr);
        Stashed.erase(player->GetGUID().GetCounter());
    }

    TakeItemAuras(player, state);
    for (uint32 spell : { uint32(BG_SRF_SPELL_BLUE_BUFF), uint32(BG_SRF_SPELL_RED_BUFF), uint32(BG_SRF_SPELL_BARON_BUFF) })
        player->RemoveAurasDueToSpell(spell);
    TakeShopItems(player);
    TakeRecallButtons(player);
    ForgetPoisons(player, state);
    TakeNoAmmo(player);
    SetMatchSkills(player, false);
    player->GiveLevel(state.RealLevel, true);
    RestoreSkills(player, state.RealSkills);
    PutBackPetRanks(player, state.PetRanks);
    player->SetUInt32Value(PLAYER_XP, state.RealXp);
    RestoreTalents(player, state.RealTalents);
    RestoreButtons(player, state.RealButtons);
    player->SetMoney(state.RealMoney);
    // The real bars go out once the client knows every spell on them again (see
    // PublishBars): a button it cannot place is cleared on both sides.
    ResetClientSpellbook(player, state, true);
    player->SendActionButtons(1);
    ResetClientSpellbook(player, state, false);
    ForgetRecall(player);

    if (state.Persistent)
        CharacterDatabase.PExecute("DELETE FROM custom_rift_stash WHERE guid = {}", player->GetGUID().GetCounter());
    SendLine(player, "RIFT:END");
}

uint32 ShopPrice(Player* player, uint32 item, uint32 price, std::vector<uint32>& consumed)
{
    consumed.clear();
    if (!player || !Recipes.count(item))
        return price;
    std::unordered_map<uint32, uint32> pool;
    for (uint32 entry = SHOP_ITEM_FIRST; entry <= SHOP_ITEM_LAST; ++entry)
        if (uint32 const count = player->GetItemCount(entry, false))
            pool[entry] = count;
    uint32 const cost = RecipeCost(item, pool, consumed);
    return std::min(cost, price);
}

void TakeComponents(Player* player, std::vector<uint32> const& consumed)
{
    if (!player)
        return;
    for (uint32 entry : consumed)
        player->DestroyItemCount(entry, 1, true, false);
}

namespace
{
    // The real character back from its `custom_rift_stash` row - level, experience,
    // money, talents, bars - and the match's things off it. The row is the only
    // copy of the real build while a match holds the character.
    bool RestoreFromStash(Player* player)
    {
    QueryResult row = CharacterDatabase.PQuery("SELECT real_level, real_xp, money, talents, actions, {}, {} FROM custom_rift_stash WHERE guid = {}",
        StashSkillsColumn ? "skills" : "''", StashPetColumn ? "pet_spells" : "''", player->GetGUID().GetCounter());
    SetMatchSkills(player, false);
    // A login while the match still held the character got the match's spell list,
    // without the real ranks it hides. The client clears every bar button whose
    // spell it does not know and tells the server, so the real bars go out only
    // after it has been told those spells again, as Leave sends them.
    State shown;
    {
        std::lock_guard<std::recursive_mutex> lock(StateMutex);
        auto const itr = States.find(player->GetGUID().GetCounter());
        if (itr != States.end())
        {
            NoteLoginSpellbook(player, itr->second);
            shown.ClientHidden = std::move(itr->second.ClientHidden);
            shown.ClientExtra = std::move(itr->second.ClientExtra);
            States.erase(itr);
        }
        Stashed.erase(player->GetGUID().GetCounter());
    }
    if (!row)
    {
        ResetClientSpellbook(player, shown, true);
        ResetClientSpellbook(player, shown, false);
        return false;
    }

    // Before the tournament script's login hook hands the gear back: the real
    // level first, so the character can wear it again.
    Field* fields = row->Fetch();
    uint8 const realLevel = fields[0].GetUInt8();
    for (uint32 spell : CHAMPION_CARRIERS)
        player->RemoveAurasDueToSpell(spell);
    for (uint32 spell : { SPELL_BLOODTHIRSTER_STACK, SPELL_COOLDOWN_CARRIER, SPELL_MEJAI_STACK, SPELL_OCCULT_STACK, SPELL_SEEKER_STACK })
        player->RemoveAurasDueToSpell(spell);
    player->RemoveAurasDueToSpell(SPELL_RIFT_FOCUS);
    TakeShopItems(player);
    TakeRecallButtons(player);
    TakeNoAmmo(player);
    player->GiveLevel(realLevel, true);
    RestoreSkills(player, SkillsFromString(fields[5].GetString()));
    PutBackPetRanks(player, PetRanksFromString(fields[6].GetString()));
    player->SetUInt32Value(PLAYER_XP, fields[1].GetUInt32());
    player->SetMoney(fields[2].GetUInt32());
    RestoreTalents(player, TalentsFromString(fields[3].GetString()));
    RestoreButtons(player, ButtonsFromString(fields[4].GetString()));
    ResetClientSpellbook(player, shown, true);
    player->SendActionButtons(1);
    ResetClientSpellbook(player, shown, false);
    ForgetRecall(player);
    CharacterDatabase.PExecute("DELETE FROM custom_rift_stash WHERE guid = {}", player->GetGUID().GetCounter());
    TC_LOG_INFO("bg.battleground", "Rift: {} restored from the stash of a match it left; level {}, experience, money, talents and bars back.", player->GetName(), realLevel);
    return true;
    }
}

void OnLogin(Player* player)
{
    if (!player || !StoragePresent)
        return;

    bool stashed = false, known = false;
    bool const inRift = IsInRiftBattleground(player);
    {
        std::lock_guard<std::recursive_mutex> lock(StateMutex);
        stashed = Stashed.count(player->GetGUID().GetCounter()) != 0;
        known = Find(player) != nullptr;
    }
    // Back in the match it left: BattlegroundSRF::AddPlayer left the entry to this,
    // now that the character has loaded whole.
    if (stashed && known && inRift)
    {
        Enter(player);
        return;
    }
    // A match left without Leave() - a crash, a restart, a logout the match let go.
    if (stashed)
        RestoreFromStash(player);
    // Still in a running match whose state for it is gone: a new champion, from
    // the real character just restored.
    if (inRift)
        Enter(player);
}

bool SuppressTalentReset(Player const* player)
{
    if (!player)
        return false;
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    ObjectGuid::LowType const guid = player->GetGUID().GetCounter();
    return States.count(guid) || Stashed.count(guid);
}

SpellInfo const* AdjustCast(Player const* player, SpellInfo const* spellInfo)
{
    if (!player || !spellInfo)
        return spellInfo;
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    State const* state = Find(player);
    if (!state)
        return spellInfo;
    if (state->Removed.count(spellInfo->Id))
        return nullptr;
    auto const chain = state->SpellToChain.find(spellInfo->Id);
    if (chain == state->SpellToChain.end())
        return spellInfo;
    auto const unlocked = state->Unlocked.find(chain->second);
    uint8 const have = unlocked != state->Unlocked.end() ? unlocked->second : 0;
    if (!have)
    {
        // A talent's trained rank before any is picked: the talent's own rank is cast.
        if (state->Chains.at(chain->second).Talent && player->HasSpell(chain->second))
            return sSpellMgr->GetSpellInfo(chain->second);
        return nullptr;
    }
    std::vector<uint32> const& ranks = state->Chains.at(chain->second).Ranks;
    auto const pos = std::find(ranks.begin(), ranks.end(), spellInfo->Id);
    if (pos != ranks.end() && size_t(pos - ranks.begin()) < have)
        return spellInfo;
    // A higher rank than the one unlocked: the action bar keeps its button and
    // casts the best rank the match has given back.
    return sSpellMgr->GetSpellInfo(ranks[have - 1]);
}

bool IsSpellLocked(Player const* player, SpellInfo const* spellInfo)
{
    if (!player || !spellInfo)
        return false;
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    State const* state = Find(player);
    if (!state)
        return false;
    if (state->Removed.count(spellInfo->Id))
        return true;
    // What the character brought in (BroughtIn). A mount is refused however it
    // is cast; the rest only from the spellbook - an item's spell is the item's
    // to allow (IsItemLocked), and a shop potion casts a stock spell.
    if (spellInfo->HasAura(SPELL_AURA_MOUNTED) || player->HasSpell(spellInfo->Id))
        if (BroughtIn(spellInfo->Id))
            return true;
    auto const chain = state->SpellToChain.find(spellInfo->Id);
    if (chain == state->SpellToChain.end())
        return false;
    auto const unlocked = state->Unlocked.find(chain->second);
    uint8 const have = unlocked != state->Unlocked.end() ? unlocked->second : 0;
    std::vector<uint32> const& ranks = state->Chains.at(chain->second).Ranks;
    auto const pos = std::find(ranks.begin(), ranks.end(), spellInfo->Id);
    return pos == ranks.end() || size_t(pos - ranks.begin()) >= have;
}

// The Blessing of the Lizard Elder on a basic attack: the slow and the burn, put
// on or, already there, made to last 3 sec again without restarting its ticks -
// a new aura every swing would never tick for anyone attacking faster than the
// burn's 1.5 sec.
void LizardBurn(Player* attacker, Unit* victim, bool ranged)
{
    if (!attacker)
        return;
    uint8 const level = GetLeagueLevel(attacker);
    int32 const slow = level >= 11 ? (ranged ? 15 : 24) : level >= 6 ? (ranged ? 10 : 16) : (ranged ? 5 : 8);
    float const scale = MatchScale(attacker);
    int32 const tick = std::max(1, Round((scale > 0.0f ? scale : 1.0f) * (8.0f + 2.0f * float(level))));
    if (Aura* burn = victim->GetAura(BG_SRF_SPELL_RED_BURN, attacker->GetGUID()))
    {
        burn->SetDuration(burn->GetMaxDuration());
        if (AuraEffect* slowed = burn->GetEffect(EFFECT_0); slowed && slowed->GetAmount() != -slow)
            slowed->ChangeAmount(-slow);
        if (AuraEffect* burning = burn->GetEffect(EFFECT_1); burning && burning->GetAmount() != tick)
            burning->ChangeAmount(tick);
        return;
    }
    // Base points, not amounts: the row's die adds 1 (SpellEffectInfo::CalcValue),
    // which had the slow at 7% for League's 8% and every first tick 1 too high.
    CastSpellExtraArgs args(TRIGGERED_FULL_MASK);
    args.AddSpellMod(SPELLVALUE_BASE_POINT0, -slow - 1);
    args.AddSpellMod(SPELLVALUE_BASE_POINT1, tick - 1);
    attacker->CastSpell(victim, BG_SRF_SPELL_RED_BURN, args);
    if (Aura* burn = victim->GetAura(BG_SRF_SPELL_RED_BURN, attacker->GetGUID()))
    {
        burn->SetMaxDuration(LIZARD_BURN_DURATION);
        burn->SetDuration(LIZARD_BURN_DURATION);
    }
}

namespace
{
    // A match modifier for one class (the Crest's cooldowns, Summoner's Focus's
    // ranges): SpellId is a spell of the class's family (SpellInfo::IsAffectedBySpellMod
    // matches a modifier to a family by its spell), Mask the class-mask bits it is on.
    struct CrestCover
    {
        uint32 SpellId = 0;
        flag96 Mask;
    };

    uint32 MaskBits(flag96 const& mask)
    {
        uint32 n = 0;
        for (uint8 i = 0; i < 3; ++i)
            for (uint32 part = mask[i]; part; part &= part - 1)
                ++n;
        return n;
    }

    // The client adds a pct modifier once for every bit of a spell's class mask
    // that carries it (Spell_C_GetSpellModifiers 0x7FD970), the server once, so
    // the Crest's mask gives each class spell with a cooldown exactly one bit.
    // Spells share bits, so it is a cover: the spells with the fewest bits
    // choose first, each taking the first bit of its own that no spell already
    // reached carries, then single-bit flips are kept while they reach more.
    // On 3.3.5's spells that misses Heroic Throw, Inner Focus, Fire Nova and
    // Ice Block (their bits are second bits of spells already reached); those
    // StartCooldown shortens itself (CooldownReduction).
    // The classes that learn a spell: a trainer's or a talent's. With
    // `weaponSkills`, a weapon skill's spell any class may have (Throw) counts
    // for every class.
    uint32 LearnerClasses(uint32 id, bool weaponSkills)
    {
        uint32 classMask = 0;
        SkillLineAbilityMapBounds const bounds = sSpellMgr->GetSkillLineAbilityMapBounds(id);
        for (auto itr = bounds.first; itr != bounds.second; ++itr)
        {
            uint32 lineClasses = itr->second->ClassMask;
            if (!lineClasses && weaponSkills)
                if (SkillLineEntry const* line = sSkillLineStore.LookupEntry(itr->second->SkillLine); line && line->CategoryID == SKILL_CATEGORY_WEAPON)
                    lineClasses = ~0u;
            classMask |= lineClasses;
        }
        if (TalentSpellPos const* pos = GetTalentSpellPos(id))
            if (TalentEntry const* talent = sTalentStore.LookupEntry(pos->talent_id))
                if (TalentTabEntry const* tab = sTalentTabStore.LookupEntry(talent->TabID))
                    classMask |= tab->ClassMask;
        return classMask;
    }

    flag96 MaskBit(uint8 bit)
    {
        return flag96(bit < 32 ? 1u << bit : 0, bit >= 32 && bit < 64 ? 1u << (bit - 32) : 0, bit >= 64 ? 1u << (bit - 64) : 0);
    }

    // A modifier's mask over one class's spells (`own`, their class masks) that
    // gives as many of them as it can exactly one bit, off the `barred` bits. The
    // spells with the fewest bits choose first, each taking the first bit of its
    // own that no spell already reached carries, then single-bit flips are kept
    // while they reach more. With `noDoubles` a spell left on two bits counts
    // against the mask: the client would take the modifier off it twice.
    flag96 BuildCover(std::vector<flag96>& own, flag96 const& barred, bool noDoubles)
    {
        flag96 mask;
        auto reached = [&own, noDoubles](flag96 const& m)
        {
            int32 score = 0;
            for (flag96 const& s : own)
            {
                uint32 const bits = MaskBits(s & m);
                if (bits == 1)
                    ++score;
                else if (bits > 1 && noDoubles)
                    score -= 1000;
            }
            return score;
        };
        std::stable_sort(own.begin(), own.end(), [](flag96 const& a, flag96 const& b) { return MaskBits(a) < MaskBits(b); });
        for (flag96 const& spell : own)
        {
            if (spell & mask)
                continue;
            for (uint8 bit = 0; bit < 96; ++bit)
            {
                flag96 const one = MaskBit(bit);
                if (!(spell & one) || (barred & one))
                    continue;
                if (std::none_of(own.begin(), own.end(), [&](flag96 const& s) { return (s & one) && (s & mask); }))
                {
                    mask |= one;
                    break;
                }
            }
        }
        for (bool improved = true; improved; )
        {
            improved = false;
            for (uint8 bit = 0; bit < 96; ++bit)
            {
                flag96 const one = MaskBit(bit);
                if (barred & one)
                    continue;
                if (reached(mask ^ one) > reached(mask))
                {
                    mask ^= one;
                    improved = true;
                }
            }
        }
        return mask;
    }

    uint32 ExactlyOnce(std::vector<flag96> const& own, flag96 const& mask)
    {
        return uint32(std::count_if(own.begin(), own.end(), [&mask](flag96 const& s) { return MaskBits(s & mask) == 1; }));
    }

    std::array<CrestCover, MAX_CLASSES> BuildCrestCovers()
    {
        std::array<CrestCover, MAX_CLASSES> covers{};
        std::array<std::vector<flag96>, MAX_CLASSES> spells;
        for (uint32 id = 1; id < sSpellMgr->GetSpellInfoStoreSize(); ++id)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
            if (!info || !info->SpellFamilyName || !info->SpellFamilyFlags || (!info->RecoveryTime && !info->CategoryRecoveryTime))
                continue;
            uint32 const classMask = LearnerClasses(id, false);
            for (uint8 classId = CLASS_WARRIOR; classId < MAX_CLASSES; ++classId)
            {
                ChrClassesEntry const* entry = sChrClassesStore.LookupEntry(classId);
                if (!entry || entry->SpellClassSet != info->SpellFamilyName || !(classMask & (1u << (classId - 1))))
                    continue;
                if (!covers[classId].SpellId)
                    covers[classId].SpellId = id;
                spells[classId].push_back(info->SpellFamilyFlags);
            }
        }

        for (uint8 classId = CLASS_WARRIOR; classId < MAX_CLASSES; ++classId)
        {
            std::vector<flag96>& own = spells[classId];
            covers[classId].Mask = BuildCover(own, flag96(), false);
            TC_LOG_INFO("server.loading", "RiftProgression: the Crest's cooldown modifier for class {} reaches {} of its {} class spells with a cooldown.",
                classId, ExactlyOnce(own, covers[classId].Mask), own.size());
        }
        return covers;
    }

    CrestCover const& CrestCoverOf(uint8 classId)
    {
        static std::array<CrestCover, MAX_CLASSES> const covers = BuildCrestCovers();
        static CrestCover const none;
        return classId < MAX_CLASSES ? covers[classId] : none;
    }

    // Summoner's Focus's range modifier: every class spell cast at a range, on
    // one bit each. Melee spells (SPELL_RANGE_MELEE) keep their reach, so no bit
    // of theirs is on the mask. Self-only and "anywhere" spells do not matter.
    // On 3.3.5's spells that leaves out Heroic Throw, some ranks of the Blessings
    // of Might and Wisdom, Divine Spirit, Fear Ward, Power Infusion, Polymorph:
    // Rabbit and Curse of Doom's last ranks - they keep their range, both sides.
    // Generic spells (wand and gun Shoot, a warrior's Throw) are not the class's
    // family, which the client's modifiers never reach.
    std::array<CrestCover, MAX_CLASSES> BuildRangeCovers()
    {
        std::array<CrestCover, MAX_CLASSES> covers{};
        std::array<std::vector<flag96>, MAX_CLASSES> ranged;
        std::array<flag96, MAX_CLASSES> melee{};
        for (uint32 id = 1; id < sSpellMgr->GetSpellInfoStoreSize(); ++id)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
            if (!info || !info->SpellFamilyName || !info->SpellFamilyFlags || !info->RangeEntry)
                continue;
            bool const isMelee = (info->RangeEntry->Flags & SPELL_RANGE_MELEE) != 0;
            float const reach = std::max(info->RangeEntry->RangeMax[0], info->RangeEntry->RangeMax[1]);
            if (!isMelee && (reach <= 0.0f || reach >= 50000.0f))
                continue;
            uint32 const classMask = LearnerClasses(id, true);
            for (uint8 classId = CLASS_WARRIOR; classId < MAX_CLASSES; ++classId)
            {
                ChrClassesEntry const* entry = sChrClassesStore.LookupEntry(classId);
                if (!entry || entry->SpellClassSet != info->SpellFamilyName || !(classMask & (1u << (classId - 1))))
                    continue;
                if (!covers[classId].SpellId)
                    covers[classId].SpellId = id;
                if (isMelee)
                    melee[classId] |= info->SpellFamilyFlags;
                else
                    ranged[classId].push_back(info->SpellFamilyFlags);
            }
        }

        for (uint8 classId = CLASS_WARRIOR; classId < MAX_CLASSES; ++classId)
        {
            std::vector<flag96>& own = ranged[classId];
            covers[classId].Mask = BuildCover(own, melee[classId], true);
            TC_LOG_INFO("server.loading", "RiftProgression: Summoner's Focus's range modifier for class {} reaches {} of its {} ranged class spells.",
                classId, ExactlyOnce(own, covers[classId].Mask), own.size());
        }
        return covers;
    }

    CrestCover const& RangeCoverOf(uint8 classId)
    {
        static std::array<CrestCover, MAX_CLASSES> const covers = BuildRangeCovers();
        static CrestCover const none;
        return classId < MAX_CLASSES ? covers[classId] : none;
    }

    // The Crest's share off one of the champion's class spells, and how many of
    // its class-mask bits the Crest's modifier is on (0 when the modifier cannot
    // reach it at all).
    float CrestShare(Player const* player, SpellInfo const* spellInfo, uint32* bits)
    {
        if (bits)
            *bits = 0;
        if (!player || !spellInfo)
            return 0.0f;
        AuraEffect const* cdr = player->GetAuraEffect(SPELL_COOLDOWN_CARRIER, EFFECT_0);
        if (!cdr || cdr->GetAmount() <= 0)
            return 0.0f;
        ChrClassesEntry const* playerClass = sChrClassesStore.LookupEntry(player->GetClass());
        if (!playerClass || !spellInfo->SpellFamilyName || spellInfo->SpellFamilyName != playerClass->SpellClassSet || !IsInMatch(player))
            return 0.0f;
        if (bits)
            *bits = MaskBits(spellInfo->SpellFamilyFlags & CrestCoverOf(player->GetClass()).Mask);
        return std::min(float(cdr->GetAmount()), COOLDOWN_REDUCTION_CAP) / 100.0f;
    }
}

float MagicPenetration(Player const* player)
{
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    State const* state = player ? Find(player) : nullptr;
    return state ? state->MagicPen : 0.0f;
}

float GearMagicResist(Player const* player)
{
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    State const* state = player ? Find(player) : nullptr;
    return state ? state->GearResist : 0.0f;
}

void CooldownMod(AuraEffect const* carrier, SpellModifier*& mod)
{
    Player* player = carrier ? carrier->GetBase()->GetUnitOwner()->ToPlayer() : nullptr;
    if (!player)
        return;
    if (!mod)
    {
        CrestCover const& cover = CrestCoverOf(player->GetClass());
        if (!cover.SpellId || !cover.Mask)
            return;
        mod = new SpellModifier(carrier->GetBase());
        mod->op = SPELLMOD_COOLDOWN;
        mod->type = SPELLMOD_PCT;
        mod->spellId = cover.SpellId;
        mod->mask = cover.Mask;
    }
    mod->value = -int32(std::min(float(std::max(carrier->GetAmount(), 0)), COOLDOWN_REDUCTION_CAP));
}

void RangeMod(AuraEffect const* carrier, SpellModifier*& mod)
{
    Player* player = carrier ? carrier->GetBase()->GetUnitOwner()->ToPlayer() : nullptr;
    if (!player)
        return;
    if (!mod)
    {
        CrestCover const& cover = RangeCoverOf(player->GetClass());
        if (!cover.SpellId || !cover.Mask)
            return;
        mod = new SpellModifier(carrier->GetBase());
        mod->op = SPELLMOD_RANGE;
        mod->type = SPELLMOD_PCT;
        mod->spellId = cover.SpellId;
        mod->mask = cover.Mask;
    }
    mod->value = carrier->GetAmount();
}

float CooldownReduction(Player const* player, SpellInfo const* spellInfo, bool* clientMisreads)
{
    uint32 bits = 0;
    float const share = CrestShare(player, spellInfo, &bits);
    // the Crest's modifier takes it off once server-side (Player::ApplySpellMod)
    bool const modReaches = bits && spellInfo->IsAffectedBySpellMods();
    if (clientMisreads)
        *clientMisreads = share > 0.0f && !(modReaches && bits == 1);
    return modReaches ? 0.0f : share;
}

std::vector<uint8> WeaponShapes(Player* player)
{
    std::vector<uint8> out;
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    if (State const* state = player ? Find(player) : nullptr)
        for (Tournament::RiftWeaponShape shape : Tournament::RiftWeaponShapes(player, state->KitApplied))
            out.push_back(uint8(shape));
    return out;
}

std::vector<uint32> WeaponTypes(Player* player, uint8 shape)
{
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    State const* state = player ? Find(player) : nullptr;
    return state ? Tournament::RiftWeaponTypes(player, Tournament::RiftWeaponShape(shape), state->KitApplied) : std::vector<uint32>();
}

std::pair<uint8, int32> CurrentWeapons(Player* player)
{
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    State const* state = player ? Find(player) : nullptr;
    return state ? std::make_pair(uint8(state->Weapons.Shape), state->Weapons.SubClass) : std::make_pair(uint8(0), int32(-1));
}

bool ChooseWeapons(Player* player, uint8 shape, int32 subClass)
{
    if (!player)
        return false;
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    State* state = Find(player);
    if (!state)
        return false;
    std::vector<Tournament::RiftWeaponShape> const shapes = Tournament::RiftWeaponShapes(player, state->KitApplied);
    if (std::find(shapes.begin(), shapes.end(), Tournament::RiftWeaponShape(shape)) == shapes.end())
        return false;
    if (subClass >= 0)
    {
        std::vector<uint32> const types = Tournament::RiftWeaponTypes(player, Tournament::RiftWeaponShape(shape), state->KitApplied);
        if (std::find(types.begin(), types.end(), uint32(subClass)) == types.end())
            return false;
    }
    state->Weapons = { Tournament::RiftWeaponShape(shape), subClass };
    if (state->Persistent && WeaponTablePresent)
        CharacterDatabase.PExecute("REPLACE INTO custom_rift_weapons (guid, shape, subclass) VALUES ({}, {}, {})",
            player->GetGUID().GetCounter(), uint32(shape), subClass);
    Tournament::SwapRiftWeapons(player, state->KitApplied, state->Weapons);
    SyncItemsFor(player, *state);
    return true;
}

char const* WeaponShapeName(uint8 shape)
{
    switch (Tournament::RiftWeaponShape(shape))
    {
        case Tournament::RiftWeaponShape::OneHandShield: return "A one-hander and a shield";
        case Tournament::RiftWeaponShape::DualWield:     return "Two one-handers";
        case Tournament::RiftWeaponShape::TwoHand:       return "A two-hander";
        case Tournament::RiftWeaponShape::DualTwoHand:   return "Two two-handers";
        case Tournament::RiftWeaponShape::OneHand:       return "A one-hander alone";
        default:                                         return "Whatever the armory hands out";
    }
}

char const* WeaponTypeName(uint32 subClass)
{
    switch (subClass)
    {
        case ITEM_SUBCLASS_WEAPON_AXE:    return "Axes";
        case ITEM_SUBCLASS_WEAPON_AXE2:   return "Two-handed axes";
        case ITEM_SUBCLASS_WEAPON_MACE:   return "Maces";
        case ITEM_SUBCLASS_WEAPON_MACE2:  return "Two-handed maces";
        case ITEM_SUBCLASS_WEAPON_POLEARM: return "Polearms";
        case ITEM_SUBCLASS_WEAPON_SWORD:  return "Swords";
        case ITEM_SUBCLASS_WEAPON_SWORD2: return "Two-handed swords";
        case ITEM_SUBCLASS_WEAPON_STAFF:  return "Staves";
        case ITEM_SUBCLASS_WEAPON_FIST:   return "Fist weapons";
        case ITEM_SUBCLASS_WEAPON_DAGGER: return "Daggers";
        default:                          return "Weapons";
    }
}

bool KeepsButton(Player* player, uint8 button)
{
    if (!player)
        return false;
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    State const* state = Find(player);
    if (!state)
        return false;
    auto const filled = state->FilledAt.find(button);
    return filled != state->FilledAt.end() && GameTime::GetGameTimeMS() - filled->second < FILLED_BUTTON_GUARD_MS
        && player->GetActionButton(button);
}

bool LearnsHigherRanks(Player const* player)
{
    return !IsInMatch(player);
}

// A talent learned in the match: its old button comes back too, and a spell it
// gives that is on no button yet - the talent's own, or what it teaches when it
// is a passive - goes on the first free one, as a pick's does.
void OnTalentLearned(Player* player, uint32 talentSpell)
{
    if (!player)
        return;
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    State* state = Find(player);
    if (!state)
        return;
    // A talent with trained ranks after it: the talent point pays for its own
    // rank, and the trained ranks after it are picks from now on (the core no
    // longer hands them back with the talent in a match: Player::LearnSpell),
    // each at its trainer's level. The talent used to count as the first of
    // them too: rank 2 came with the talent, before its level, put aside and
    // so never castable, and the first pick went straight to rank 3.
    for (auto const& [first, chain] : state->Chains)
        if (chain.Talent && player->HasSpell(first))
            state->TalentKnown.insert(first);
    if (IsTransient(player))
        return;
    RefillSavedButtons(player, *state);
    std::vector<uint32> given;
    if (SpellInfo const* info = sSpellMgr->GetSpellInfo(talentSpell))
    {
        if (!info->IsPassive())
            given.push_back(talentSpell);
        else
            for (SpellEffectInfo const& effect : info->GetEffects())
                if (effect.Effect == SPELL_EFFECT_LEARN_SPELL && effect.TriggerSpell)
                    if (SpellInfo const* taught = sSpellMgr->GetSpellInfo(effect.TriggerSpell); taught && !taught->IsPassive())
                        given.push_back(effect.TriggerSpell);
    }
    for (uint32 spell : given)
    {
        if (!player->HasSpell(spell))
            continue;
        uint32 const first = sSpellMgr->GetFirstSpellInChain(spell);
        bool onBars = false;
        for (uint8 button = 0; button < MAX_ACTION_BUTTONS && !onBars; ++button)
            if (ActionButton const* ab = player->GetActionButton(button))
                onBars = ab->GetType() == ACTION_BUTTON_SPELL && sSpellMgr->GetFirstSpellInChain(ab->GetAction()) == first;
        if (onBars)
            continue;
        // Back where the real bars had it - their button held a trained rank, which
        // stays locked until it is picked, so it was never put back.
        bool placed = false;
        for (SavedButton const& b : state->RealButtons)
            if (b.Type == ACTION_BUTTON_SPELL && sSpellMgr->GetFirstSpellInChain(b.Action) == first
                && !player->GetActionButton(b.Button) && player->addActionButton(b.Button, spell, ACTION_BUTTON_SPELL))
            {
                state->Returned.insert(b.Button);
                placed = true;
                break;
            }
        if (!placed)
            PlaceUnlockedSpell(player, *state, 0, spell);
    }
    PublishBars(player, *state);
    SendState(player, *state);
}

int32 MatchRidingSkill(Player const* player)
{
    if (!player)
        return -1;
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    return Find(player) ? int32(MatchRiding(player)) : -1;
}

bool ShowsInSpellbook(Player const* player, uint32 spell, bool active)
{
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    State const* state = Find(player);
    return state ? MatchShows(*state, spell, active) : active;
}

void SyncItems(Player* player)
{
    if (!player)
        return;
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    State* state = Find(player);
    if (!state)
        return;
    SyncItemsFor(player, *state);
    if (!state->StatsLine.empty() && state->StatsLine != state->SentStatsLine)
    {
        state->SentStatsLine = state->StatsLine;
        SendLine(player, state->StatsLine);
    }
    // The run speed CENTURION_Rift shows on its inventory pane, as a share of the
    // normal run (WoW's addon API has only the speed of the moment): sent when it changes.
    uint32 const speed = uint32(player->GetSpeedRate(MOVE_RUN) * 100.0f + 0.5f);
    if (speed != state->SentSpeed)
    {
        state->SentSpeed = speed;
        SendLine(player, Trinity::StringFormat("RIFTSPEED:{}", speed));
    }
    // The kit follows the match level. Armour goes on in combat in a match
    // (Player::CanEquipItem asks IsInMatch): the level-up is a minion kill,
    // mid-fight, and the swap once took every piece off and put none back.
    // Not while casting, stunned or charmed: nothing can be put on then, the old
    // piece included. The next sync tries again.
    // Not while dead either: it waits for the revive.
    uint8 const kit = KitLevelFor(*state);
    bool const alive = player->IsAlive();
    bool const free = alive && !player->IsNonMeleeSpellCast(false) && !player->HasUnitState(UNIT_STATE_STUNNED) && !player->IsCharmed();
    bool fill = alive && !state->WasAlive;   // just revived: is everything on?
    if (kit != state->KitApplied && free)
    {
        Tournament::UpgradeRiftKit(player, kit, state->Weapons);
        state->KitApplied = kit;
        fill = true;
    }
    if (fill && free)
        Tournament::FillRiftKit(player, state->KitApplied, state->Weapons);
    if (!fill || free)
        state->WasAlive = alive;
}

std::vector<uint32> HeldShopItems(Player const* player)
{
    std::vector<uint32> out;
    if (!player)
        return out;
    auto take = [&out](Item const* item)
    {
        if (item && IsShopItem(item->GetEntry()))
            out.push_back(item->GetEntry());
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        take(player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
        if (Bag const* container = player->GetBagByPos(bag))
            for (uint32 slot = 0; slot < container->GetBagSize(); ++slot)
                take(container->GetItemByPos(uint8(slot)));
    return out;
}

void Tell(Player* player, std::string const& line)
{
    if (player)
        SendLine(player, line);
}

bool HasRoomFor(Player const* player, uint32 entry, std::vector<uint32> const& consumed)
{
    if (!player || !IsShopItem(entry) || !IsInMatch(player))
        return true;
    if (IsTrinketItem(entry))
    {
        uint32 trinkets = 0;
        for (uint32 e = SHOP_ITEM_FIRST; e <= SHOP_ITEM_LAST; ++e)
            if (IsTrinketItem(e))
                trinkets += player->GetItemCount(e, false);
        // One built from the trinket held takes its place: the swift steed was
        // refused to a champion holding the steed it is built from.
        for (uint32 component : consumed)
            if (IsTrinketItem(component) && trinkets)
                --trinkets;
        return trinkets == 0;
    }
    return HeldItemSlots(player, consumed) < ITEM_SLOTS;
}

bool MayEquip(Player const* player, uint32 entry, uint8 slot)
{
    if (slot >= EQUIPMENT_SLOT_END || slot == EQUIPMENT_SLOT_BODY || slot == EQUIPMENT_SLOT_TABARD)
        return true;
    if ((entry >= SHOP_ITEM_FIRST && entry <= SHOP_ITEM_LAST) || IsFieldKitDuplicateEntry(entry))
        return true;
    return !IsInMatch(player);
}

// Self contained: in a match only the shop's items can be used - a
// character's own food, potions, hearthstone and trinkets stay shut.
bool IsItemLocked(Player const* player, uint32 entry)
{
    if (entry >= SHOP_ITEM_FIRST && entry <= SHOP_ITEM_LAST)
        return false;
    return IsInMatch(player);
}

// What champions earn, as config (.reload config takes a change at once):
// Centurion.Rift.PlayerXpRate / PlayerGoldRate, 1.5 each (owner, 2026-10-21).
void AddExperience(Player* player, float leagueXp)
{
    leagueXp *= sConfigMgr->GetFloatDefault("Centurion.Rift.PlayerXpRate", 1.5f);
    if (!player || leagueXp <= 0.0f)
        return;
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    State* state = Find(player);
    if (!state)
        return;

    state->LeagueXp += leagueXp;
    while (state->LeagueLevel < LEAGUE_MAX_LEVEL && state->LeagueXp >= XpToNext(state->LeagueLevel))
    {
        state->LeagueXp -= XpToNext(state->LeagueLevel);
        ++state->LeagueLevel;
    }
    if (state->LeagueLevel >= LEAGUE_MAX_LEVEL)
        state->LeagueXp = 0.0f;

    uint8 const target = std::min<uint8>(state->RealLevel, uint8(std::floor(MatchLevelFor(*state) + 1e-4f)));
    if (target > state->MatchLevel)
        SetMatchLevel(player, *state, target);
    else
        ShowProgress(player, *state);
}

void AddGold(Player* player, uint32 leagueGold, bool popup)
{
    leagueGold = uint32(std::lround(float(leagueGold) * sConfigMgr->GetFloatDefault("Centurion.Rift.PlayerGoldRate", 1.5f)));
    if (!player || !leagueGold || !IsInMatch(player))
        return;
    player->ModifyMoney(int32(leagueGold * COPPER_PER_LEAGUE_GOLD), false);
    if (popup)
        SendLine(player, "RIFTGOLD:" + std::to_string(leagueGold));
}

uint8 GetLeagueLevel(Player const* player)
{
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    State const* state = player ? Find(player) : nullptr;
    return state ? state->LeagueLevel : 1;
}

void SetMatchScale(Player* player, float scale, uint8 combatLevel)
{
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    if (State* state = player ? Find(player) : nullptr)
    {
        state->MatchScale = scale;
        state->CombatLevel = combatLevel;
    }
}

float MatchScale(Player const* player)
{
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    State const* state = player ? Find(player) : nullptr;
    return state ? state->MatchScale : 0.0f;
}

bool IsRangedBasicAttack(SpellInfo const* info)
{
    if (!info)
        return false;
    if (info->IsAutoRepeatRangedSpell())
        return true;
    if (!info->HasAttribute(SPELL_ATTR0_REQ_AMMO) || info->ManaCost || info->ManaCostPercentage)
        return false;
    bool weapon = false;
    for (uint8 i = 0; i < MAX_SPELL_EFFECTS; ++i)
    {
        uint32 const effect = info->GetEffect(SpellEffIndex(i)).Effect;
        if (effect == SPELL_EFFECT_WEAPON_DAMAGE || effect == SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL)
            weapon = true;
        else if (effect)
            return false;
    }
    return weapon;
}

void OnBasicAttackDamage(Unit* attacker, Unit* victim, uint32 damage, SpellInfo const* spell)
{
    // Cheapest first: this runs after every swing and shot in the world. The
    // life steal rides on the champion's carrier (SetChampionCarriers).
    if (!damage || !attacker || !victim || attacker == victim || attacker->GetTypeId() != TYPEID_PLAYER)
        return;
    // An ability's damage: spell vamp (League items).
    if (spell && !IsRangedBasicAttack(spell))
    {
        if (attacker->GetMapId() == RIFT_MAP_ID)
            SpellVamp(attacker->ToPlayer(), victim, damage, spell);
        return;
    }
    AuraEffect const* lifeSteal = attacker->GetAuraEffect(SPELL_CHAMPION_F, EFFECT_1);
    bool const blessed = attacker->HasAura(BG_SRF_SPELL_RED_BUFF);
    if ((!blessed && (!lifeSteal || lifeSteal->GetAmount() <= 0)) || !attacker->IsAlive())
        return;
    if (victim->GetTypeId() == TYPEID_UNIT && victim->GetEntry() >= RIFT_STRUCTURE_FIRST && victim->GetEntry() <= RIFT_STRUCTURE_LAST)
        return;
    if (spell && !IsRangedBasicAttack(spell))
        return;
    if (blessed && victim->IsAlive())
        LizardBurn(attacker->ToPlayer(), victim, spell != nullptr);
    if (!lifeSteal || lifeSteal->GetAmount() <= 0)
        return;
    // Healing cuts on the champion (Mortal Strike's) cut it too, as Grievous Wounds does in League.
    float heal = float(damage) * float(lifeSteal->GetAmount()) / 1000.0f;
    // Spirit Visage
    if (float const amp = ItemHooksOf(attacker->ToPlayer()).HealAmp; amp > 0.0f)
        heal *= 1.0f + amp / 100.0f;
    if (int32 const cut = attacker->GetMaxNegativeAuraModifier(SPELL_AURA_MOD_HEALING_PCT))
        AddPct(heal, cut);
    if (heal < 1.0f)
        return;
    HealInfo healInfo(attacker, attacker, uint32(heal), nullptr, SPELL_SCHOOL_MASK_NORMAL);
    Unit::DealHeal(healInfo);
}

void SpellVamp(Player* player, Unit* victim, uint32 damage, SpellInfo const* spell)
{
    if (!player || !victim || !damage || !player->IsAlive())
        return;
    if (victim->GetTypeId() == TYPEID_UNIT && victim->GetEntry() >= RIFT_STRUCTURE_FIRST && victim->GetEntry() <= RIFT_STRUCTURE_LAST)
        return;
    // abilities only: not an item's passive (Thornmail, Sunfire, Maim) nor a camp buff's burn
    if (spell && spell->Id >= RIFT_OWN_FIRST && spell->Id <= RIFT_OWN_LAST)
        return;
    ItemHooks const hooks = ItemHooksOf(player);
    if (hooks.SpellVamp <= 0.0f)
        return;
    // League: an area ability heals a third as much; Spirit Visage adds its share.
    float heal = float(damage) * hooks.SpellVamp / 100.0f * (spell && spell->IsAffectingArea() ? 1.0f / 3.0f : 1.0f)
        * (1.0f + hooks.HealAmp / 100.0f);
    if (int32 const cut = player->GetMaxNegativeAuraModifier(SPELL_AURA_MOD_HEALING_PCT))
        AddPct(heal, cut);
    if (heal < 1.0f)
        return;
    HealInfo healInfo(player, player, uint32(heal), nullptr, SPELL_SCHOOL_MASK_NORMAL);
    Unit::DealHeal(healInfo);
}

void OnUnitKill(Player* player)
{
    if (!player)
        return;
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    State* state = Find(player);
    if (!state)
        return;
    if (player->GetItemCount(ITEM_BLOODTHIRSTER, false) && state->BloodthirsterStacks < BLOODTHIRSTER_MAX_STACKS)
    {
        ++state->BloodthirsterStacks;
        ShowBloodthirster(player, *state);
    }
    using RiftLeague::Unique;
    // Seeker's Armguard: a stack a unit killed.
    if (state->Unique[size_t(Unique::UnitKillArmorAp)] > 0.0f && state->SeekerStacks < SEEKER_MAX_STACKS)
    {
        ++state->SeekerStacks;
        ShowStacks(player, SPELL_SEEKER_STACK, state->SeekerStacks);
    }
    // Avarice Blade's Greed: gold a unit killed.
    if (float const greed = state->Unique[size_t(Unique::Greed)]; greed > 0.0f)
        AddGold(player, uint32(std::lround(greed)), false);
}

void OnChampionTakedown(Player* player, bool kill)
{
    if (!player)
        return;
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    State* state = Find(player);
    if (!state)
        return;
    using RiftLeague::Unique;
    uint8 const gain = kill ? 2 : 1;
    if (state->Unique[size_t(Unique::TakedownAp)] > 0.0f)
    {
        state->MejaiStacks = std::min<uint8>(TAKEDOWN_MAX_STACKS, state->MejaiStacks + gain);
        ShowStacks(player, SPELL_MEJAI_STACK, state->MejaiStacks);
    }
    if (state->Unique[size_t(Unique::TakedownAd)] > 0.0f)
    {
        state->OccultStacks = std::min<uint8>(TAKEDOWN_MAX_STACKS, state->OccultStacks + gain);
        ShowStacks(player, SPELL_OCCULT_STACK, state->OccultStacks);
    }
    // Athene's Unholy Grail: a share of maximum mana back.
    if (float const pct = state->Unique[size_t(Unique::TakedownManaPct)]; pct > 0.0f && player->IsAlive() && player->GetMaxPower(POWER_MANA))
        player->ModifyPower(POWER_MANA, Round(float(player->GetMaxPower(POWER_MANA)) * pct / 100.0f));
}

void OnDeath(Player* player)
{
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    State* state = player ? Find(player) : nullptr;
    if (!state)
        return;
    if (state->BloodthirsterStacks)
    {
        state->BloodthirsterStacks = uint8(state->BloodthirsterStacks / 2);
        ShowBloodthirster(player, *state);
    }
    // Mejai's Soulstealer, Sword of the Occult: a third of the stacks lost.
    state->MejaiStacks = uint8(state->MejaiStacks - state->MejaiStacks / 3);
    state->OccultStacks = uint8(state->OccultStacks - state->OccultStacks / 3);
    ShowStacks(player, SPELL_MEJAI_STACK, state->MejaiStacks);
    ShowStacks(player, SPELL_OCCULT_STACK, state->OccultStacks);
}

void ItemGoldTick(Player* player, uint32 diff)
{
    if (!player)
        return;
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    State* state = Find(player);
    if (!state)
        return;
    float const per10 = state->Unique[size_t(RiftLeague::Unique::GoldPer10)];
    if (per10 <= 0.0f)
        return;
    state->GoldOwed += per10 * float(diff) / 10000.0f;
    if (uint32 const whole = uint32(state->GoldOwed); whole)
    {
        state->GoldOwed -= float(whole);
        AddGold(player, whole, false);
    }
}

float ArmorPenetration(Player const* player)
{
    if (!player || player->GetMapId() != RIFT_MAP_ID)
        return {};
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    State const* state = Find(player);
    return state ? state->Unique[size_t(RiftLeague::Unique::ArmorPen)] : 0.0f;
}

float MagicPenetrationPct(Player const* player)
{
    if (!player || player->GetMapId() != RIFT_MAP_ID)
        return {};
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    State const* state = Find(player);
    return state ? std::min(100.0f, state->Unique[size_t(RiftLeague::Unique::MagicPenPct)]) : 0.0f;
}

float SlowResist(Player const* player)
{
    if (!player || player->GetMapId() != RIFT_MAP_ID)
        return {};
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    State const* state = Find(player);
    return state ? std::min(100.0f, state->Unique[size_t(RiftLeague::Unique::SlowResist)]) : 0.0f;
}

// Tenacity (Mercury's Treads, Spirit of the Ancient Golem): off League's crowd
// control - stuns, slows, taunts, fears, silences, blinds, immobilizes, and in
// WoW's terms charms, sleeps, polymorphs, horrors and saps.
float Tenacity(Player const* player, SpellInfo const* spellInfo, uint32 effectMask)
{
    if (!player || !spellInfo || player->GetMapId() != RIFT_MAP_ID)
        return 0.0f;
    uint32 constexpr CC_MECHANICS = (1u << MECHANIC_CHARM) | (1u << MECHANIC_DISORIENTED) | (1u << MECHANIC_FEAR)
        | (1u << MECHANIC_ROOT) | (1u << MECHANIC_SILENCE) | (1u << MECHANIC_SLEEP) | (1u << MECHANIC_SNARE)
        | (1u << MECHANIC_STUN) | (1u << MECHANIC_FREEZE) | (1u << MECHANIC_KNOCKOUT) | (1u << MECHANIC_POLYMORPH)
        | (1u << MECHANIC_HORROR) | (1u << MECHANIC_DAZE) | (1u << MECHANIC_SAPPED);
    bool control = (uint32(spellInfo->GetSpellMechanicMaskByEffectMask(effectMask)) & CC_MECHANICS) != 0;
    for (SpellEffectInfo const& effect : spellInfo->GetEffects())
        if (!control && (effectMask & (1u << effect.EffectIndex)))
            for (AuraType type : { SPELL_AURA_MOD_STUN, SPELL_AURA_MOD_ROOT, SPELL_AURA_MOD_FEAR, SPELL_AURA_MOD_CONFUSE,
                SPELL_AURA_MOD_SILENCE, SPELL_AURA_MOD_PACIFY_SILENCE, SPELL_AURA_MOD_DECREASE_SPEED, SPELL_AURA_MOD_TAUNT,
                SPELL_AURA_MOD_CHARM })
                if (effect.IsAura(type))
                    control = true;
    if (!control)
        return 0.0f;
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    State const* state = Find(player);
    return state ? std::min(100.0f, state->Unique[size_t(RiftLeague::Unique::Tenacity)]) : 0.0f;
}

ItemHooks ItemHooksOf(Player const* player)
{
    ItemHooks out;
    if (!player || player->GetMapId() != RIFT_MAP_ID)
        return {};
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    State const* state = Find(player);
    if (!state)
        return out;
    using RiftLeague::Unique;
    auto const own = [state](Unique stat) { return state->Unique[size_t(stat)]; };
    out.InMatch = true;
    out.BasicBlockPct = own(Unique::BasicBlockPct);
    out.ChampionBasicBlock = own(Unique::ChampionBasicBlock);
    out.OnHitHeal = own(Unique::OnHitHeal);
    out.Maim = own(Unique::Maim);
    out.Butcher = own(Unique::Butcher);
    out.HealAmp = own(Unique::HealAmp);
    out.ChampionHitRegen = own(Unique::ChampionHitRegen);
    out.ColdSteelAs = own(Unique::ColdSteelAs);
    out.ColdSteelMs = own(Unique::ColdSteelMs);
    out.SpellVamp = own(Unique::SpellVamp) + state->ReceivedSpellVamp;
    out.Scale = state->MatchScale > 0.0f ? state->MatchScale : 1.0f;
    return out;
}

float BaseAttackDamage(Player const* player)
{
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    State const* state = player ? Find(player) : nullptr;
    if (!state)
        return 0.0f;
    std::shared_ptr<RiftLeague::Tables const> const tables = RiftLeague::Get();
    RiftLeague::ChampionStats const* cs = RiftLeague::FindChampion(*tables, player->GetClass());
    return cs ? state->MatchScale * (cs->Damage + cs->DamagePerLevel * float(state->LeagueLevel)) : 0.0f;
}

bool CanSpendPoints(Player* player, bool tell)
{
    if (!player || !IsInMatch(player))
        return true;
    Battleground* bg = player->GetBattleground();
    if (bg && bg->GetTypeID(true) == BATTLEGROUND_SRF && static_cast<BattlegroundSRF const*>(bg)->IsAtBase(player))
        return true;
    if (tell && player->GetSession())
        ChatHandler(player->GetSession()).SendSysMessage("Skill picks and talent points are spent at your base: return to your fountain.");
    return false;
}

void OnLevelChanged(Player* player)
{
    if (!player)
        return;
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    State* state = Find(player);
    if (!state)
        return;
    // SetMatchLevel's own GiveLevel comes back here with the level already matched.
    uint8 const level = std::min<uint8>(player->GetLevel(), state->RealLevel);
    if (level <= state->MatchLevel)
        return;
    while (state->MatchLevel < level)
    {
        ++state->MatchLevel;
        state->PendingPicks += PICKS_PER_LEVEL;
    }
    // Valor's Reward (Catalyst of the Protector and what builds from it): League's
    // 150 health at the match's scale, and League's 200 mana as the same share of
    // this champion's pool as of a League champion's at its level.
    if (player->HasAura(SPELL_VALORS_REWARD) && player->IsAlive())
    {
        player->ModifyHealth(std::max<int32>(1, Round(150.0f * (state->MatchScale > 0.0f ? state->MatchScale : 1.0f))));
        std::shared_ptr<RiftLeague::Tables const> const tables = RiftLeague::Get();
        RiftLeague::ChampionStats const* cs = RiftLeague::FindChampion(*tables, player->GetClass());
        float const leagueMana = cs ? cs->Mana + cs->ManaPerLevel * float(state->LeagueLevel) : 0.0f;
        if (player->GetMaxPower(POWER_MANA) > 0 && leagueMana > 0.0f)
            player->ModifyPower(POWER_MANA, std::max<int32>(1, Round(float(player->GetMaxPower(POWER_MANA)) * 200.0f / leagueMana)));
    }
    CoatBotPoisons(player, *state);
    ShowProgress(player, *state);
    SendState(player, *state);
    if (state->PendingPicks)
        ChatHandler(player->GetSession()).PSendSysMessage("You have %u skill pick(s) to spend: type /rift.", state->PendingPicks);
}

// ------------------------------------------------------------------ bots
//
// A bot (a battleground fill clone: a transient session) cannot send the
// addon's pick request or click a talent or a vendor, so League's bots are
// played here instead, from Maintain, by the same rules a player plays by:
// points and gold are spent at the fountain only (CanSpendPoints / IsAtBase).
namespace
{
    // What a bot builds toward, by what it does (League's bot item lists, in the
    // shop's own items - rift_shop.py's entries, SHOP_ITEM_FIRST + index).
    enum class BotRole : uint8 { Melee, Ranged, Caster, Healer, Tank };

    uint32 constexpr Shop(uint32 index) { return SHOP_ITEM_FIRST + index; }
    // finished items, bought in this order
    std::vector<uint32> const BOT_BUILDS[] =
    {
        /* Melee  */ { Shop(35), Shop(23), Shop(36), Shop(31), Shop(34), Shop(32) },  // Trinity Force, Berserker's, Infinity Edge, Last Whisper, Guardian Angel, Phantom Dancer
        /* Ranged */ { Shop(36), Shop(23), Shop(32), Shop(31), Shop(34), Shop(35) },  // Infinity Edge, Berserker's, Phantom Dancer, Last Whisper, Guardian Angel, Trinity Force
        /* Caster */ { Shop(25), Shop(33), Shop(24), Shop(26), Shop(37), Shop(29) },  // Doran's Ring, Rod of Ages, Sorcerer's, Rabadon's, Zhonya's, Banshee's
        /* Healer */ { Shop(25), Shop(24), Shop(33), Shop(29), Shop(37), Shop(34) },  // Doran's Ring, Sorcerer's, Rod of Ages, Banshee's, Zhonya's, Guardian Angel
        /* Tank   */ { Shop(23), Shop(30), Shop(28), Shop(27), Shop(29), Shop(34) },  // Berserker's, Sunfire, Warmog's, Thornmail, Banshee's, Guardian Angel
    };

    // The tree the bot's real talents went into most: 0, 1 or 2.
    int32 MainTree(State const& state)
    {
        uint32 points[3] = { 0, 0, 0 };
        for (LearnedTalent const& t : state.RealTalents)
            if (TalentEntry const* talent = sTalentStore.LookupEntry(t.Id))
                if (TalentTabEntry const* tab = sTalentTabStore.LookupEntry(talent->TabID))
                    if (tab->OrderIndex < 3)
                        points[tab->OrderIndex] += t.Rank + 1;
        if (!points[0] && !points[1] && !points[2])
            return -1;
        return points[0] >= points[1] && points[0] >= points[2] ? 0 : points[1] >= points[2] ? 1 : 2;
    }

    BotRole RoleOf(Player const* player, State const& state)
    {
        int32 const tree = MainTree(state);
        switch (player->GetClass())
        {
            case CLASS_WARRIOR:      return tree == 2 ? BotRole::Tank : BotRole::Melee;
            case CLASS_PALADIN:      return tree == 0 ? BotRole::Healer : tree == 1 ? BotRole::Tank : BotRole::Melee;
            case CLASS_HUNTER:       return BotRole::Ranged;
            case CLASS_ROGUE:        return BotRole::Melee;
            case CLASS_PRIEST:       return tree == 2 ? BotRole::Caster : BotRole::Healer;
            case CLASS_DEATH_KNIGHT: return tree == 0 ? BotRole::Tank : BotRole::Melee;
            case CLASS_SHAMAN:       return tree == 0 ? BotRole::Caster : tree == 1 ? BotRole::Melee : BotRole::Healer;
            case CLASS_DRUID:        return tree == 0 ? BotRole::Caster : tree == 1 ? BotRole::Melee : BotRole::Healer;
            default:                 return BotRole::Caster;    // mage, warlock
        }
    }

    // League's bots level their abilities as they come: a new one first (the
    // lowest-level one on offer), then the newest rank of what they have.
    // What a pick is worth to a bot: something that hurts first (its spell layer
    // fights with those), then something that heals, then the rest. A buff
    // first left a level-10 mage with Frost Armor and nothing to cast at minions.
    int32 PickWorth(SpellInfo const* info)
    {
        for (SpellEffectInfo const& effect : info->GetEffects())
        {
            switch (effect.Effect)
            {
                case SPELL_EFFECT_SCHOOL_DAMAGE:
                case SPELL_EFFECT_WEAPON_DAMAGE:
                case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
                case SPELL_EFFECT_WEAPON_PERCENT_DAMAGE:
                case SPELL_EFFECT_NORMALIZED_WEAPON_DMG:
                case SPELL_EFFECT_HEALTH_LEECH:
                    return 3;
                default:
                    break;
            }
            if (effect.IsAura(SPELL_AURA_PERIODIC_DAMAGE) || effect.IsAura(SPELL_AURA_PERIODIC_LEECH)
                || effect.IsAura(SPELL_AURA_PERIODIC_DAMAGE_PERCENT))
                return 3;
        }
        for (SpellEffectInfo const& effect : info->GetEffects())
            if (effect.Effect == SPELL_EFFECT_HEAL || effect.IsAura(SPELL_AURA_PERIODIC_HEAL))
                return 2;
        return 1;
    }

    // League's bots level their abilities as they come: a new one first (the most
    // useful, lowest level first), then the newest rank of the most useful.
    void BotSpendPicks(Player* /*player*/, State& state)
    {
        while (state.PendingPicks)
        {
            std::vector<uint32> const offers = Offers(state);
            uint32 pick = 0;
            std::tuple<bool, int32, int32> best;     // new, worth, level (as a preference)
            for (uint32 spell : offers)
            {
                SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
                if (!info)
                    continue;
                uint32 const first = state.SpellToChain.at(spell);
                auto const unlocked = state.Unlocked.find(first);
                bool const isNew = unlocked == state.Unlocked.end() || !unlocked->second;
                int32 const level = isNew ? -int32(info->SpellLevel) : int32(info->SpellLevel);
                std::tuple<bool, int32, int32> const key(isNew, PickWorth(info), level);
                if (!pick || key > best)
                {
                    pick = spell;
                    best = key;
                }
            }
            if (!pick)
                break;
            ++state.Unlocked[state.SpellToChain.at(pick)];
            --state.PendingPicks;
        }
    }

    // A bot's spellbook holds what it has unlocked and nothing else: its spell
    // layer picks from the spells it knows, and one it knows but may not cast
    // (Spell::CheckCast, IsSpellLocked) is a cast that fails every time - the
    // bot stood in lane doing nothing. A copy writes nothing down, so this is
    // only ever done to bots.
    void BotSyncSpellbook(Player* player, State const& state)
    {
        for (auto const& [first, chain] : state.Chains)
        {
            auto const itr = state.Unlocked.find(first);
            uint8 const unlocked = itr != state.Unlocked.end() ? itr->second : 0;
            for (uint32 k = 0; k < chain.Ranks.size(); ++k)
            {
                uint32 const spell = chain.Ranks[k];
                bool const want = k < unlocked && !state.Removed.count(spell);
                if (want && !player->HasSpell(spell))
                    player->LearnSpell(spell, false);
                else if (!want && player->HasSpell(spell))
                    player->RemoveSpell(spell, false, false);
            }
        }
    }

    // The bot's own build, tier by tier and rank by rank, as far as its points go.
    void BotSpendTalents(Player* player, State const& state)
    {
        if (!player->GetFreeTalentPoints() || state.RealTalents.empty())
            return;
        std::vector<LearnedTalent> build = state.RealTalents;
        std::stable_sort(build.begin(), build.end(), [](LearnedTalent const& a, LearnedTalent const& b) { return a.Tier < b.Tier; });
        uint32 const before = player->GetFreeTalentPoints();
        for (LearnedTalent const& t : build)
            for (uint32 rank = 0; rank <= t.Rank && player->GetFreeTalentPoints(); ++rank)
                player->LearnTalent(t.Id, rank);
        if (player->GetFreeTalentPoints() != before)
            player->SendTalentsInfoData(false);
    }

    // Buys one shop item the way the vendor would (League's price, components
    // into it) into the League inventory. With the six item slots full, the
    // cheapest held item not going into it is sold back first, at League's 70%.
    bool BotBuy(Player* player, uint32 entry)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        if (!proto || proto->BuyPrice <= 0)
            return false;
        std::vector<uint32> consumed;
        uint32 const price = ShopPrice(player, entry, uint32(proto->BuyPrice), consumed);
        if (!player->HasEnoughMoney(price))
            return false;

        if (!HasRoomFor(player, entry, consumed))
        {
            std::unordered_map<uint32, uint32> going;
            for (uint32 part : consumed)
                ++going[part];
            uint32 cheapest = 0;
            int32 cheapestPrice = 0;
            for (uint32 held = SHOP_ITEM_FIRST; held <= SHOP_ITEM_LAST; ++held)
            {
                if (IsTrinketItem(held) || player->GetItemCount(held, false) <= going[held])
                    continue;
                ItemTemplate const* hp = sObjectMgr->GetItemTemplate(held);
                if (hp && (!cheapest || hp->BuyPrice < cheapestPrice))
                {
                    cheapest = held;
                    cheapestPrice = hp->BuyPrice;
                }
            }
            if (!cheapest)
                return false;
            player->ModifyMoney(int64(sObjectMgr->GetItemTemplate(cheapest)->SellPrice));
            player->DestroyItemCount(cheapest, 1, true, false);
            if (!HasRoomFor(player, entry, consumed))
                return false;
        }

        ItemPosCountVec store;
        if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, store, entry, 1) != EQUIP_ERR_OK)
            return false;
        player->ModifyMoney(-int64(price));
        TakeComponents(player, consumed);
        player->StoreNewItem(store, entry, true);
        return true;
    }

    // League's bots shop at the fountain: the next finished item of their build,
    // or - while saving up for it - the dearest piece of it they can afford now.
    void BotShop(Player* player, State const& state)
    {
        for (uint32 target : BOT_BUILDS[uint8(RoleOf(player, state))])
        {
            if (player->GetItemCount(target, false))
                continue;
            std::vector<uint32> consumed;
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(target);
            if (!proto)
                continue;
            if (player->HasEnoughMoney(ShopPrice(player, target, uint32(proto->BuyPrice), consumed)))
            {
                BotBuy(player, target);
                return;
            }
            // the components still to buy, the whole tree down
            std::vector<uint32> todo;
            std::vector<uint32> open = { target };
            std::unordered_map<uint32, uint32> held;
            while (!open.empty())
            {
                uint32 const item = open.back();
                open.pop_back();
                auto const recipe = Recipes.find(item);
                if (recipe == Recipes.end())
                    continue;
                for (uint32 part : recipe->second)
                {
                    uint32& used = held[part];
                    if (player->GetItemCount(part, false) > used)
                    {
                        ++used;
                        continue;
                    }
                    todo.push_back(part);
                    open.push_back(part);
                }
            }
            uint32 best = 0;
            int32 bestPrice = -1;
            for (uint32 part : todo)
            {
                ItemTemplate const* pp = sObjectMgr->GetItemTemplate(part);
                std::vector<uint32> c;
                if (pp && pp->BuyPrice > bestPrice && player->HasEnoughMoney(ShopPrice(player, part, uint32(pp->BuyPrice), c)))
                {
                    best = part;
                    bestPrice = pp->BuyPrice;
                }
            }
            if (best)
                BotBuy(player, best);
            return;     // saving for this one: nothing past it
        }
    }
}

void Maintain(Player* player)
{
    // Talents are the character's own in the match now (spent from its own
    // points), so nothing the engine re-applies needs taking off again. A bot's
    // points and gold are spent for it, at the fountain.
    if (!player || !player->IsAlive())
        return;
    // anyone still wearing the no-ammo aura an earlier build handed out
    if (player->HasAura(SPELL_NO_AMMO))
        TakeNoAmmo(player);
    // a hunter pet called (or brought back) since the last tick: its ranks for its level
    {
        std::lock_guard<std::recursive_mutex> lock(StateMutex);
        if (State* state = Find(player))
            SyncPetRanks(player, *state);
    }
    if (!IsTransient(player) || !CanSpendPoints(player, false))
        return;
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    State* state = Find(player);
    if (!state)
        return;
    BotSpendPicks(player, *state);
    BotSyncSpellbook(player, *state);
    BotSpendTalents(player, *state);
    BotShop(player, *state);
}

uint8 BuildRole(Player const* player)
{
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    State const* state = player ? Find(player) : nullptr;
    return state ? uint8(RoleOf(player, *state)) : 0;
}

// The shop's buy request (CENTURION_Rift, "RIFTBUY:<entry>"): what the vendor
// window did, without it - a 3.3.5 vendor lists 150 items at most
// (MAX_VENDOR_ITEMS) and League's shop is longer than that. At the fountain only;
// League's build path prices it, the components held go into it.
void HandleBuyRequest(Player* player, std::string_view argument)
{
    if (!player)
        return;
    Optional<uint32> const entry = Trinity::StringTo<uint32>(argument);
    if (!entry || !IsShopItem(*entry) || !IsInMatch(player))
        return;
    ChatHandler chat(player->GetSession());
    Battleground* bg = player->GetBattleground();
    // League: at the fountain, or anywhere while dead
    if (!bg || bg->GetTypeID(true) != BATTLEGROUND_SRF || (player->IsAlive() && !static_cast<BattlegroundSRF const*>(bg)->IsAtBase(player)))
    {
        chat.SendSysMessage("The shop sells at your base: return to your fountain.");
        return;
    }
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(*entry);
    if (!proto || !(proto->AllowableClass & player->GetClassMask()))
        return;
    std::vector<uint32> consumed;
    uint32 const price = ShopPrice(player, *entry, uint32(proto->BuyPrice), consumed);
    if (!HasRoomFor(player, *entry, consumed))
    {
        chat.SendSysMessage("Your six item slots are full: sell something first.");
        return;
    }
    if (!player->HasEnoughMoney(price))
    {
        player->SendBuyError(BUY_ERR_NOT_ENOUGHT_MONEY, nullptr, *entry, 0);
        return;
    }
    ItemPosCountVec dest;
    InventoryResult const result = player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, *entry, 1);
    if (result != EQUIP_ERR_OK)
    {
        player->SendEquipError(result, nullptr, nullptr, *entry);
        return;
    }
    Item* item = player->StoreNewItem(dest, *entry, true);
    if (!item)
        return;
    player->ModifyMoney(-int32(price));
    player->SendNewItem(item, 1, true, false);
    TakeComponents(player, consumed);
    SyncItems(player);
}

void HandlePickRequest(Player* player, std::string_view argument)
{
    if (!player)
        return;
    Optional<uint32> const spellId = Trinity::StringTo<uint32>(argument);
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    State* state = Find(player);
    if (!state || !spellId)
        return;
    if (state->PendingPicks && CanSpendPoints(player, true))
    {
        std::vector<uint32> const offers = Offers(*state);
        if (std::find(offers.begin(), offers.end(), *spellId) != offers.end())
        {
            uint32 const first = state->SpellToChain.at(*spellId);
            bool const firstUnlock = ++state->Unlocked[first] == 1;
            // A talent's trained rank the match reset put aside (disabled with
            // the talent) is learned again now, alone; one the character never
            // trained is taught for the match only (dependent: never written down).
            auto const known = player->GetSpellMap().find(*spellId);
            if (known != player->GetSpellMap().end() && known->second.state != PLAYERSPELL_REMOVED)
            {
                if (known->second.disabled)
                    player->LearnSpell(*spellId, false);
            }
            else
            {
                player->LearnSpell(*spellId, true);
                state->MatchRanks.insert(*spellId);
            }
            if (firstUnlock)
                PlaceUnlockedSpell(player, *state, first, *spellId);
            PublishBars(player, *state);
            --state->PendingPicks;
        }
    }
    SendState(player, *state);
}

void HandleStateRequest(Player* player, std::string_view /*argument*/)
{
    if (!player)
        return;
    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    if (State const* state = Find(player))
    {
        SendState(player, *state);
        SendBase(player, CanSpendPoints(player, false));
        SendLine(player, Trinity::StringFormat("RIFTSPEED:{}", uint32(player->GetSpeedRate(MOVE_RUN) * 100.0f + 0.5f)));
        if (!state->StatsLine.empty())
            SendLine(player, state->StatsLine);
    }
}

void SendBase(Player* player, bool atBase)
{
    if (player && IsInMatch(player))
        SendLine(player, atBase ? "RIFTBASE:1" : "RIFTBASE:0");
}
}
