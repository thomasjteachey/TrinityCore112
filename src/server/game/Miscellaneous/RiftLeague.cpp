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

#include "RiftLeague.h"
#include "RiftProgression.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include <algorithm>
#include <cmath>
#include <mutex>

namespace
{
    std::mutex TablesMutex;
    std::shared_ptr<RiftLeague::Tables const> Current = std::make_shared<RiftLeague::Tables const>();

    bool HasTable(char const* name)
    {
        return WorldDatabase.PQuery("SELECT TABLE_NAME FROM information_schema.TABLES WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = '{}'", name) != nullptr;
    }

    // Growth steps (or minutes, when the growth is continuous) a unit has had by `minutes`.
    float Steps(RiftLeague::UnitStats const& u, float minutes)
    {
        float const since = std::max(0.0f, minutes - u.GrowthStart);
        return u.GrowthStep > 0.0f ? std::floor(since / u.GrowthStep) : since;
    }

    float Capped(float value, float cap)
    {
        return cap > 0.0f ? std::min(value, cap) : value;
    }
}

namespace RiftLeague
{
void Load()
{
    if (!HasTable("rift_league_unit") || !HasTable("rift_league_champion") || !HasTable("rift_league_item"))
    {
        TC_LOG_INFO("server.loading", ">> Summoner's Rift League numbers: the rift_league_* tables are absent.");
        return;
    }

    auto tables = std::make_shared<Tables>();
    if (QueryResult rows = WorldDatabase.Query("SELECT kind, health, health_growth, damage, damage_growth, damage_cap, attack_speed, "
        "armor, armor_growth, armor_cap, resist, resist_growth, resist_cap, growth_start, growth_step, "
        "bonus_armor, bonus_armor_fade_start, bonus_armor_fade_end, backdoor_armor, backdoor_resist, "
        "damage_vs_champions, damage_vs_structures, turret_damage_taken, armor_pen_pct FROM rift_league_unit"))
    {
        do
        {
            Field* f = rows->Fetch();
            UnitStats u;
            u.Health = f[1].GetFloat();             u.HealthGrowth = f[2].GetFloat();
            u.Damage = f[3].GetFloat();             u.DamageGrowth = f[4].GetFloat();      u.DamageCap = f[5].GetFloat();
            u.AttackSpeed = std::max(0.05f, f[6].GetFloat());
            u.Armor = f[7].GetFloat();              u.ArmorGrowth = f[8].GetFloat();       u.ArmorCap = f[9].GetFloat();
            u.Resist = f[10].GetFloat();            u.ResistGrowth = f[11].GetFloat();     u.ResistCap = f[12].GetFloat();
            u.GrowthStart = f[13].GetFloat();       u.GrowthStep = f[14].GetFloat();
            u.BonusArmor = f[15].GetFloat();        u.BonusArmorFadeStart = f[16].GetFloat(); u.BonusArmorFadeEnd = f[17].GetFloat();
            u.BackdoorArmor = f[18].GetFloat();     u.BackdoorResist = f[19].GetFloat();
            u.DamageVsChampions = f[20].GetFloat(); u.DamageVsStructures = f[21].GetFloat();
            u.TurretDamageTaken = f[22].GetFloat(); u.ArmorPenPct = f[23].GetFloat();
            tables->Units[f[0].GetString()] = u;
        } while (rows->NextRow());
    }

    if (QueryResult rows = WorldDatabase.Query("SELECT class, health, health_per_level, armor, armor_per_level, resist, resist_per_level, "
        "damage, damage_per_level, attack_speed, attack_speed_per_level, mana, mana_per_level, "
        "rune_damage, rune_armor, rune_resist_per_level, rune_ability_power, rune_ability_power_per_level, rune_magic_pen "
        "FROM rift_league_champion"))
    {
        do
        {
            Field* f = rows->Fetch();
            ChampionStats c;
            c.Health = f[1].GetFloat();      c.HealthPerLevel = f[2].GetFloat();
            c.Armor = f[3].GetFloat();       c.ArmorPerLevel = f[4].GetFloat();
            c.Resist = f[5].GetFloat();      c.ResistPerLevel = f[6].GetFloat();
            c.Damage = f[7].GetFloat();      c.DamagePerLevel = f[8].GetFloat();
            c.AttackSpeed = std::max(0.1f, f[9].GetFloat()); c.AttackSpeedPerLevel = f[10].GetFloat();
            c.Mana = f[11].GetFloat();       c.ManaPerLevel = f[12].GetFloat();
            c.RuneDamage = f[13].GetFloat(); c.RuneArmor = f[14].GetFloat(); c.RuneResistPerLevel = f[15].GetFloat();
            c.RuneAbilityPower = f[16].GetFloat(); c.RuneAbilityPowerPerLevel = f[17].GetFloat(); c.RuneMagicPen = f[18].GetFloat();
            tables->Champions[f[0].GetUInt8()] = c;
        } while (rows->NextRow());
    }

    if (QueryResult rows = WorldDatabase.Query("SELECT entry, health, mana, damage, ability_power, armor, resist, attack_speed, crit, "
        "move_speed, boots, health_regen, mana_regen, magic_pen, life_steal FROM rift_league_item"))
    {
        do
        {
            Field* f = rows->Fetch();
            ItemStats i;
            i.Health = f[1].GetFloat();       i.Mana = f[2].GetFloat();
            i.Damage = f[3].GetFloat();       i.AbilityPower = f[4].GetFloat();
            i.Armor = f[5].GetFloat();        i.Resist = f[6].GetFloat();
            i.AttackSpeed = f[7].GetFloat();  i.Crit = f[8].GetFloat();
            i.MoveSpeed = f[9].GetFloat();    i.Boots = f[10].GetUInt8() != 0;
            i.HealthRegen = f[11].GetFloat(); i.ManaRegen = f[12].GetFloat(); i.MagicPen = f[13].GetFloat();
            i.LifeSteal = f[14].GetFloat();
            tables->Items[f[0].GetUInt32()] = i;
        } while (rows->NextRow());
    }

    // The items' UNIQUE effects (rift_shop.py, gen_league_sql.py). A realm without
    // the table plays the items' plain stats only.
    size_t uniques = 0;
    if (HasTable("rift_league_item_unique"))
        if (QueryResult rows = WorldDatabase.Query("SELECT entry, name, stat, value FROM rift_league_item_unique"))
        {
            static std::unordered_map<std::string, Unique> const names =
            {
                { "cdr", Unique::CooldownReduction }, { "armor_pen", Unique::ArmorPen }, { "magic_pen", Unique::MagicPen },
                { "magic_pen_pct", Unique::MagicPenPct }, { "spell_vamp", Unique::SpellVamp }, { "tenacity", Unique::Tenacity },
                { "gold10", Unique::GoldPer10 }, { "greed", Unique::Greed }, { "slow_resist", Unique::SlowResist },
                { "ms_ooc", Unique::OutOfCombatSpeed }, { "basic_block_pct", Unique::BasicBlockPct },
                { "champ_basic_block", Unique::ChampionBasicBlock }, { "on_hit_heal", Unique::OnHitHeal }, { "maim", Unique::Maim },
                { "butcher", Unique::Butcher }, { "ad_per_health", Unique::AdPerHealth }, { "mana_font", Unique::ManaFont },
                { "heal_amp", Unique::HealAmp }, { "champ_hit_regen", Unique::ChampionHitRegen },
                { "unit_kill_armor_ap", Unique::UnitKillArmorAp }, { "takedown_ap", Unique::TakedownAp },
                { "takedown_ad", Unique::TakedownAd }, { "takedown_mana_pct", Unique::TakedownManaPct },
                { "cold_steel_as", Unique::ColdSteelAs }, { "cold_steel_ms", Unique::ColdSteelMs },
                { "aura_ally_mr", Unique::AuraAllyMr }, { "aura_ally_hp5", Unique::AuraAllyHp5 }, { "aura_ally_mp5", Unique::AuraAllyMp5 },
                { "aura_ally_ap", Unique::AuraAllyAp }, { "aura_ally_spell_vamp", Unique::AuraAllySpellVamp },
                { "aura_ally_ad", Unique::AuraAllyAd }, { "aura_ally_ls", Unique::AuraAllyLs },
                { "aura_enemy_mr", Unique::AuraEnemyMr }, { "aura_enemy_as", Unique::AuraEnemyAs },
            };
            do
            {
                Field* f = rows->Fetch();
                auto const stat = names.find(f[2].GetString());
                if (stat == names.end())
                {
                    TC_LOG_ERROR("server.loading", "rift_league_item_unique: item {} has stat `{}`, which RiftProgression does not know - skipped.",
                        f[0].GetUInt32(), f[2].GetString());
                    continue;
                }
                tables->Uniques[f[0].GetUInt32()].push_back({ f[1].GetString(), stat->second, f[3].GetFloat() });
                ++uniques;
            } while (rows->NextRow());
        }

    TC_LOG_INFO("server.loading", ">> Summoner's Rift League numbers: {} unit kind(s), {} class(es), {} item(s), {} unique effect(s).",
        tables->Units.size(), tables->Champions.size(), tables->Items.size(), uniques);
    std::lock_guard<std::mutex> lock(TablesMutex);
    Current = std::move(tables);
}

std::shared_ptr<Tables const> Get()
{
    std::lock_guard<std::mutex> lock(TablesMutex);
    return Current;
}

UnitStats const* FindUnit(Tables const& tables, std::string const& kind)
{
    auto itr = tables.Units.find(kind);
    return itr != tables.Units.end() ? &itr->second : nullptr;
}

ChampionStats const* FindChampion(Tables const& tables, uint8 playerClass)
{
    auto itr = tables.Champions.find(playerClass);
    return itr != tables.Champions.end() ? &itr->second : nullptr;
}

ItemStats const* FindItem(Tables const& tables, uint32 entry)
{
    auto itr = tables.Items.find(entry);
    return itr != tables.Items.end() ? &itr->second : nullptr;
}

UnitAt At(UnitStats const& u, float minutes)
{
    float const n = Steps(u, minutes);
    UnitAt at;
    at.Health = u.Health + u.HealthGrowth * n;
    at.Damage = Capped(u.Damage + u.DamageGrowth * n, u.DamageCap);
    at.Armor = Capped(u.Armor + u.ArmorGrowth * n, u.ArmorCap);
    at.Resist = Capped(u.Resist + u.ResistGrowth * n, u.ResistCap);
    // An outer turret's early plating: full until the fade starts, gone at its end.
    if (u.BonusArmor > 0.0f)
    {
        float const span = u.BonusArmorFadeEnd - u.BonusArmorFadeStart;
        float const left = span > 0.0f ? std::clamp((u.BonusArmorFadeEnd - minutes) / span, 0.0f, 1.0f)
            : (minutes < u.BonusArmorFadeEnd ? 1.0f : 0.0f);
        at.Armor += u.BonusArmor * left;
    }
    return at;
}

// Centurion.Rift.Scale.* is fitted along the climb the match first had - WoW level
// 10 at League level 1, the realm's cap at 18 - where it gives a champion its class's
// own base health for its WoW level. A match that starts elsewhere
// (Centurion.Rift.StartLevel) reaches each WoW level at another League level, where
// League's champions have more or less health, so the scale is restated by that
// share: a champion of a WoW level keeps the health the fit gives it there, and WoW's
// spells their weight. League's own ratios stay exact (one scale for the match). At
// the 40 start it runs from about 2.0 at League level 1 down to 0.96 at 18.
float Scale(float leagueLevel)
{
    float constexpr FIT_START_LEVEL = 10.0f;
    float constexpr LEAGUE_TOP = 18.0f;
    float const base = sConfigMgr->GetFloatDefault("Centurion.Rift.Scale.Base", 0.365f);
    float const perLevel = sConfigMgr->GetFloatDefault("Centurion.Rift.Scale.PerLevel", 0.0352f);
    float const l = std::clamp(leagueLevel, 1.0f, LEAGUE_TOP);

    float const start = float(RiftProgression::MatchStartLevel());
    float const top = float(RiftProgression::ClimbTopLevel());
    float fitted = l;   // the League level the fit's climb is at this WoW level
    if (start != FIT_START_LEVEL && top > FIT_START_LEVEL)
    {
        float const wowLevel = start + (l - 1.0f) / (LEAGUE_TOP - 1.0f) * (top - start);
        fitted = std::clamp(1.0f + (wowLevel - FIT_START_LEVEL) / (top - FIT_START_LEVEL) * (LEAGUE_TOP - 1.0f), 1.0f, LEAGUE_TOP);
    }

    float scale = base + perLevel * (fitted - 1.0f);
    if (fitted != l)
    {
        // League's champion health at a League level, the classes' average
        std::shared_ptr<Tables const> const tables = Get();
        auto const health = [&tables](float level)
        {
            float total = 0.0f;
            for (auto const& [playerClass, champion] : tables->Champions)
                total += champion.Health + champion.HealthPerLevel * level;
            return tables->Champions.empty() ? 0.0f : total / float(tables->Champions.size());
        };
        if (float const here = health(l); here > 0.0f)
            scale *= health(fitted) / here;
    }
    return std::max(0.01f, scale);
}

float ArmorConstant(uint8 level)
{
    float l = float(std::max<uint8>(level, 1));
    if (l > 59.0f)
        l = l + 4.5f * (l - 59.0f);
    return 85.0f * l + 400.0f;
}

float ToWowArmor(float leagueArmor, uint8 level)
{
    return std::max(0.0f, leagueArmor) * ArmorConstant(level) / 100.0f;
}

float ToWowResist(float leagueResist)
{
    float const m = std::max(0.0f, leagueResist);
    return 300.0f * m / (100.0f + m);
}

float FromWowResist(float wowResist)
{
    // ToWowResist never reaches 300; a WoW buff on top could
    float const r = std::clamp(wowResist, 0.0f, 299.0f);
    return 100.0f * r / (300.0f - r);
}

float ArmorMultiplier(float wowArmor, uint8 level)
{
    float const a = std::max(0.0f, wowArmor);
    return 1.0f - a / (a + ArmorConstant(level));
}
}
