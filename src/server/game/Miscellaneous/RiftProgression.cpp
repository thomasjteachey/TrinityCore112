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

#include "Battleground.h"
#include "BattlegroundSRF.h"
#include "Chat.h"
#include "WorldPacket.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "Item.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Miscellaneous/TournamentMode.h"
#include "Player.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringConvert.h"
#include "StringFormat.h"
#include "Util.h"
#include "WorldSession.h"

#include <algorithm>
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
    uint32 constexpr SHOP_ITEM_LAST = 991099;

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
        std::unordered_set<uint32> Removed;        // class spells off the whitelist: out of the match
        uint32 PendingPicks = 0;

        std::vector<LearnedTalent> RealTalents;   // the active spec's build, put back on the way out
        std::vector<SavedButton> RealButtons;     // the action bars it walked in with
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
        for (auto itr = state.Chains.begin(); itr != state.Chains.end();)
        {
            Chain& chain = itr->second;
            std::sort(chain.Ranks.begin(), chain.Ranks.end(), [](uint32 a, uint32 b)
            {
                return sSpellMgr->GetSpellRank(a) < sSpellMgr->GetSpellRank(b);
            });
            // Off the core list: out of the match altogether.
            SpellInfo const* first = sSpellMgr->GetSpellInfo(itr->first);
            int32 const status = first ? WhitelistStatus(player->GetClass(), first) : -1;
            if (status == 0)
            {
                state.Removed.insert(chain.Ranks.begin(), chain.Ranks.end());
                itr = state.Chains.erase(itr);
                continue;
            }
            if (status == 2)
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
    // talents are restored first).
    void RestoreButtons(Player* player, std::vector<SavedButton> const& buttons)
    {
        for (uint32 button = 0; button < MAX_ACTION_BUTTONS; ++button)
            player->removeActionButton(uint8(button));
        for (SavedButton const& b : buttons)
            player->addActionButton(b.Button, b.Action, b.Type);
        player->SendActionButtons(1);
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
            if (have >= chain.Ranks.size())
                continue;
            SpellInfo const* next = sSpellMgr->GetSpellInfo(chain.Ranks[have]);
            if (next && next->SpellLevel <= state.MatchLevel)
                offers.push_back(chain.Ranks[have]);
        }
        return offers;
    }

    // A spell button the match takes off the bars: a class spell none of whose
    // ranks is unlocked yet, or a spell the character no longer knows (taught
    // by a talent the match reset).
    bool HiddenInMatch(Player const* player, State const& state, uint32 spell)
    {
        if (!player->HasSpell(spell) || state.Removed.count(spell))
            return true;
        auto const chain = state.SpellToChain.find(spell);
        if (chain == state.SpellToChain.end())
            return false;
        auto const unlocked = state.Unlocked.find(chain->second);
        return unlocked == state.Unlocked.end() || !unlocked->second;
    }

    // Macros come off too: the server cannot read them (they live client side),
    // and a #showtooltip macro draws the spell it casts - a level-60 hunter's
    // Aspect of the Hawk macro stood on the bar of a level-10 match, refused on
    // every click. RestoreButtons puts them back with the rest.
    void HideLockedButtons(Player* player, State const& state)
    {
        for (SavedButton const& b : state.RealButtons)
            if ((b.Type == ACTION_BUTTON_SPELL && HiddenInMatch(player, state, b.Action))
                || b.Type == ACTION_BUTTON_MACRO || b.Type == ACTION_BUTTON_CMACRO)
                player->removeActionButton(b.Button);
        player->SendActionButtons(1);
    }

    void TakeRecall(Player* player)
    {
        bool removed = false;
        for (uint8 button = 0; button < MAX_ACTION_BUTTONS; ++button)
            if (ActionButton const* ab = player->GetActionButton(button))
                if (ab->GetType() == ACTION_BUTTON_SPELL && ab->GetAction() == SPELL_RIFT_RECALL)
                {
                    player->removeActionButton(button);
                    removed = true;
                }
        if (removed)
            player->SendActionButtons(1);
        if (player->HasSpell(SPELL_RIFT_RECALL))
            player->RemoveSpell(SPELL_RIFT_RECALL, false, false);
        // RemoveSpell is only written at the next save; a crash before it must
        // not leave the recall in the spellbook.
        CharacterDatabase.PExecute("DELETE FROM character_spell WHERE guid = {} AND spell = {}",
            player->GetGUID().GetCounter(), SPELL_RIFT_RECALL);
    }

    // A chain's first unlock puts the spell on the bars: back on its own button
    // when that is free, otherwise on the first free button of the usual bars.
    void PlaceUnlockedSpell(Player* player, State const& state, uint32 first, uint32 spell)
    {
        for (SavedButton const& b : state.RealButtons)
        {
            if (b.Type != ACTION_BUTTON_SPELL)
                continue;
            auto const chain = state.SpellToChain.find(b.Action);
            if (chain == state.SpellToChain.end() || chain->second != first)
                continue;
            if (!player->GetActionButton(b.Button) && player->addActionButton(b.Button, b.Action, ACTION_BUTTON_SPELL))
            {
                player->SendActionButtons(1);
                return;
            }
        }
        for (uint8 start : BAR_ORDER)
            for (uint8 button = start; button < start + 12; ++button)
                if (!player->GetActionButton(button))
                {
                    if (player->addActionButton(button, spell, ACTION_BUTTON_SPELL))
                        player->SendActionButtons(1);
                    return;
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
        player->GiveLevel(state.MatchLevel, true);
        player->SetHealth(std::min(std::max<uint32>(health, 1), player->GetMaxHealth()));
        if (player->GetMaxPower(POWER_MANA))
            player->SetPower(POWER_MANA, std::min<uint32>(mana, player->GetMaxPower(POWER_MANA)));
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

    StoragePresent = CharacterDatabase.Query("SELECT TABLE_NAME FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'custom_rift_stash'") != nullptr;
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

// The bots' side (further down, before Maintain).
namespace
{
    void BotSpendPicks(Player* player, State& state);
    void BotSyncSpellbook(Player* player, State const& state);
}

void Enter(Player* player)
{
    if (!player || !StoragePresent)
        return;

    std::lock_guard<std::recursive_mutex> lock(StateMutex);
    if (State* existing = Find(player))
    {
        // Back in the match it left: level, talents and gear are as it left them.
        SendState(player, *existing);
        return;
    }

    State& state = States[player->GetGUID().GetCounter()];
    state.RealLevel = player->GetLevel();
    state.RealXp = player->GetUInt32Value(PLAYER_XP);
    state.RealMoney = player->GetMoney();
    state.Persistent = !IsTransient(player);
    BuildState(player, state);
    state.RealButtons = SnapshotButtons(player);

    // Written down before anything changes: from here on every way out restores.
    if (state.Persistent)
    {
        CharacterDatabase.DirectPExecute("REPLACE INTO custom_rift_stash (guid, real_level, real_xp, money, talents, actions) VALUES ({}, {}, {}, {}, '{}', '{}')",
            player->GetGUID().GetCounter(), uint32(state.RealLevel), state.RealXp, state.RealMoney, TalentsToString(state.RealTalents),
            ButtonsToString(state.RealButtons));
        Stashed.insert(player->GetGUID().GetCounter());
    }

    // The temporal gear - the field set issued for the level the match starts at
    // - goes on while the character still has its real level to equip it with.
    Tournament::ApplyRiftLoadout(player, std::min<uint8>(START_LEVEL, state.RealLevel));
    player->SetMoney(START_GOLD * COPPER_PER_LEAGUE_GOLD);
    player->ResetTalents(true);

    // Only what the core list makes free comes with it (stances, forms, the
    // pet's call); everything else is a skill pick - one now, one a level.
    state.StartLevel = std::min<uint8>(START_LEVEL, state.RealLevel);
    state.MatchLevel = state.StartLevel;
    for (uint32 first : state.FreeChains)
        state.Unlocked[first] = uint8(state.Chains.at(first).Ranks.size());
    state.PendingPicks = PICKS_PER_LEVEL;
    player->GiveLevel(state.StartLevel, true);
    if (!state.Persistent)
    {
        BotSpendPicks(player, state);
        BotSyncSpellbook(player, state);
    }
    player->SendTalentsInfoData(false);
    HideLockedButtons(player, state);
    if (!player->HasSpell(SPELL_RIFT_RECALL))
        player->LearnSpell(SPELL_RIFT_RECALL, false);
    PlaceUnlockedSpell(player, state, 0, SPELL_RIFT_RECALL);
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

    TakeShopItems(player);
    TakeRecall(player);
    TakeNoAmmo(player);
    player->GiveLevel(state.RealLevel, true);
    player->SetUInt32Value(PLAYER_XP, state.RealXp);
    RestoreTalents(player, state.RealTalents);
    RestoreButtons(player, state.RealButtons);
    player->SetMoney(state.RealMoney);

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

void OnLogin(Player* player)
{
    if (!player || !StoragePresent)
        return;

    {
        std::lock_guard<std::recursive_mutex> lock(StateMutex);
        if (!Stashed.count(player->GetGUID().GetCounter()))
            return;
        if (Find(player) && IsInRiftBattleground(player))
            return;   // BattlegroundSRF::AddPlayer re-applies the rules on the way back in
    }

    QueryResult row = CharacterDatabase.PQuery("SELECT real_level, real_xp, money, talents, actions FROM custom_rift_stash WHERE guid = {}", player->GetGUID().GetCounter());
    {
        std::lock_guard<std::recursive_mutex> lock(StateMutex);
        States.erase(player->GetGUID().GetCounter());
        Stashed.erase(player->GetGUID().GetCounter());
    }
    if (!row)
        return;

    // Before the tournament script's login hook hands the gear back: the real
    // level first, so the character can wear it again.
    Field* fields = row->Fetch();
    uint8 const realLevel = fields[0].GetUInt8();
    TakeShopItems(player);
    TakeRecall(player);
    TakeNoAmmo(player);
    player->GiveLevel(realLevel, true);
    player->SetUInt32Value(PLAYER_XP, fields[1].GetUInt32());
    player->SetMoney(fields[2].GetUInt32());
    RestoreTalents(player, TalentsFromString(fields[3].GetString()));
    RestoreButtons(player, ButtonsFromString(fields[4].GetString()));
    CharacterDatabase.PExecute("DELETE FROM custom_rift_stash WHERE guid = {}", player->GetGUID().GetCounter());
    TC_LOG_INFO("bg.battleground", "Rift: {} logged in outside the match it left; level {}, experience, money and talents restored.", player->GetName(), realLevel);
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
        return nullptr;
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
    auto const chain = state->SpellToChain.find(spellInfo->Id);
    if (chain == state->SpellToChain.end())
        return false;
    auto const unlocked = state->Unlocked.find(chain->second);
    uint8 const have = unlocked != state->Unlocked.end() ? unlocked->second : 0;
    std::vector<uint32> const& ranks = state->Chains.at(chain->second).Ranks;
    auto const pos = std::find(ranks.begin(), ranks.end(), spellInfo->Id);
    return pos == ranks.end() || size_t(pos - ranks.begin()) >= have;
}

bool MayEquip(Player const* player, uint32 entry, uint8 slot)
{
    if (slot >= EQUIPMENT_SLOT_END || slot == EQUIPMENT_SLOT_BODY || slot == EQUIPMENT_SLOT_TABARD)
        return true;
    if ((entry >= SHOP_ITEM_FIRST && entry <= SHOP_ITEM_LAST) || Tournament::IsFieldKitPiece(entry))
        return true;
    return !IsInMatch(player);
}

void AddExperience(Player* player, float leagueXp)
{
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
    // Valor's Reward (Catalyst of the Protector and what builds from it).
    if (player->HasAura(SPELL_VALORS_REWARD) && player->IsAlive())
    {
        player->ModifyHealth(150);
        if (player->GetMaxPower(POWER_MANA) > 0)
            player->ModifyPower(POWER_MANA, 200);
    }
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

    // The equipment slot a shop item goes into, preferring an empty one of a pair.
    uint8 BotSlotFor(Player* player, ItemTemplate const* proto)
    {
        auto pick = [player](uint8 a, uint8 b)
        {
            if (!player->GetItemByPos(INVENTORY_SLOT_BAG_0, a))
                return a;
            if (!player->GetItemByPos(INVENTORY_SLOT_BAG_0, b))
                return b;
            // both taken: the one holding the cheaper shop item, or a kit piece
            Item* ia = player->GetItemByPos(INVENTORY_SLOT_BAG_0, a);
            Item* ib = player->GetItemByPos(INVENTORY_SLOT_BAG_0, b);
            auto worth = [](Item* item)
            {
                uint32 const entry = item->GetEntry();
                return entry >= SHOP_ITEM_FIRST && entry <= SHOP_ITEM_LAST ? item->GetTemplate()->BuyPrice : 0;
            };
            return worth(ia) <= worth(ib) ? a : b;
        };
        switch (proto->InventoryType)
        {
            case INVTYPE_HEAD:      return EQUIPMENT_SLOT_HEAD;
            case INVTYPE_NECK:      return EQUIPMENT_SLOT_NECK;
            case INVTYPE_SHOULDERS: return EQUIPMENT_SLOT_SHOULDERS;
            case INVTYPE_CHEST:
            case INVTYPE_ROBE:      return EQUIPMENT_SLOT_CHEST;
            case INVTYPE_WAIST:     return EQUIPMENT_SLOT_WAIST;
            case INVTYPE_LEGS:      return EQUIPMENT_SLOT_LEGS;
            case INVTYPE_FEET:      return EQUIPMENT_SLOT_FEET;
            case INVTYPE_WRISTS:    return EQUIPMENT_SLOT_WRISTS;
            case INVTYPE_HANDS:     return EQUIPMENT_SLOT_HANDS;
            case INVTYPE_CLOAK:     return EQUIPMENT_SLOT_BACK;
            case INVTYPE_FINGER:    return pick(EQUIPMENT_SLOT_FINGER1, EQUIPMENT_SLOT_FINGER2);
            case INVTYPE_TRINKET:   return pick(EQUIPMENT_SLOT_TRINKET1, EQUIPMENT_SLOT_TRINKET2);
            default:                return NULL_SLOT;
        }
    }

    // Buys one shop item the way the vendor would (League's price, components
    // into it) and wears it. Whatever it displaces - a field kit piece, or a
    // shop item it outgrew - goes, a shop item sold back at League's 70%.
    bool BotBuy(Player* player, uint32 entry)
    {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
        if (!proto || proto->BuyPrice <= 0)
            return false;
        std::vector<uint32> consumed;
        uint32 const price = ShopPrice(player, entry, uint32(proto->BuyPrice), consumed);
        if (!player->HasEnoughMoney(price))
            return false;
        player->ModifyMoney(-int64(price));
        TakeComponents(player, consumed);

        uint8 const slot = BotSlotFor(player, proto);
        if (slot != NULL_SLOT)
            if (Item* old = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            {
                uint32 const oldEntry = old->GetEntry();
                if (oldEntry >= SHOP_ITEM_FIRST && oldEntry <= SHOP_ITEM_LAST)
                    player->ModifyMoney(int64(old->GetTemplate()->SellPrice));
                player->DestroyItem(INVENTORY_SLOT_BAG_0, slot, true);
            }
        uint16 dest = 0;
        if (slot != NULL_SLOT && player->CanEquipNewItem(slot, dest, entry, false) == EQUIP_ERR_OK)
            player->EquipNewItem(dest, entry, true);
        else
        {
            ItemPosCountVec store;
            if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, store, entry, 1) == EQUIP_ERR_OK)
                player->StoreNewItem(store, entry, true);
        }
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
            if (++state->Unlocked[first] == 1)
                PlaceUnlockedSpell(player, *state, first, *spellId);
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
    }
}

void SendBase(Player* player, bool atBase)
{
    if (player && IsInMatch(player))
        SendLine(player, atBase ? "RIFTBASE:1" : "RIFTBASE:0");
}
}
