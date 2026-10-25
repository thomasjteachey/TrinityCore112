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

#ifndef TRINITYCORE_RIFT_LEAGUE_H
#define TRINITYCORE_RIFT_LEAGUE_H

#include "Define.h"
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

// Summoner's Rift plays League's numbers (Season 3): every unit, champion and
// item stat is League's own, from three world tables -
//
//   rift_league_unit      minions, turrets, inhibitors, the nexus, jungle monsters
//   rift_league_champion  each class's League base stats and rune page
//   rift_league_item      each shop item's League stats
//
// - and becomes WoW's by one conversion for the whole match:
//
//   health, damage, healing, ability power  x Scale(average League level)
//   armor      the WoW armor that gives League's reduction, a / (100 + a), at
//              the match's level (ToWowArmor; Unit::CalcArmorReducedDamage
//              reads the same level - Battleground::GetLeagueCombatLevel)
//   resist     the resistance that gives League's magic reduction (ToWowResist)
//   attack and move speed, crit chance and damage: League's own
//
// so hits and seconds to kill come out as League's. Scale follows WoW's own
// growth (Centurion.Rift.Scale.*; fitted to the classes' base health and
// basic-attack damage against League's, 0.365 at League level 1 to 0.96 at 18),
// so WoW's spells keep their natural weight against health.
namespace RiftLeague
{
    struct UnitStats
    {
        float Health = 0.0f, HealthGrowth = 0.0f;
        float Damage = 0.0f, DamageGrowth = 0.0f, DamageCap = 0.0f;
        float AttackSpeed = 1.0f;
        float Armor = 0.0f, ArmorGrowth = 0.0f, ArmorCap = 0.0f;
        float Resist = 0.0f, ResistGrowth = 0.0f, ResistCap = 0.0f;
        float GrowthStart = 0.0f;       // minutes of game time before growth begins
        float GrowthStep = 0.0f;        // minutes per step (0: growth is per minute, continuous)
        float BonusArmor = 0.0f, BonusArmorFadeStart = 0.0f, BonusArmorFadeEnd = 0.0f;
        float BackdoorArmor = 0.0f, BackdoorResist = 0.0f;   // with no enemy minion near
        float DamageVsChampions = 1.0f, DamageVsStructures = 1.0f;
        float TurretDamageTaken = 1.0f;
        float ArmorPenPct = 0.0f;
    };

    // A unit's League stats at a game time, in League units.
    struct UnitAt
    {
        float Health = 0.0f, Damage = 0.0f, Armor = 0.0f, Resist = 0.0f;
    };

    struct ChampionStats
    {
        float Health = 0.0f, HealthPerLevel = 0.0f;
        float Armor = 0.0f, ArmorPerLevel = 0.0f;
        float Resist = 0.0f, ResistPerLevel = 0.0f;
        float Damage = 0.0f, DamagePerLevel = 0.0f;
        float AttackSpeed = 0.625f, AttackSpeedPerLevel = 0.0f;   // per level: a fraction of the base
        float Mana = 0.0f, ManaPerLevel = 0.0f;
        float RuneDamage = 0.0f, RuneArmor = 0.0f, RuneResistPerLevel = 0.0f;
        float RuneAbilityPower = 0.0f, RuneAbilityPowerPerLevel = 0.0f, RuneMagicPen = 0.0f;
    };

    struct ItemStats
    {
        float Health = 0.0f, Mana = 0.0f, Damage = 0.0f, AbilityPower = 0.0f;
        float Armor = 0.0f, Resist = 0.0f, AttackSpeed = 0.0f, Crit = 0.0f;   // AS and crit in percent
        float MoveSpeed = 0.0f;   // percent
        bool Boots = false;       // only the fastest boots' speed counts
        float HealthRegen = 0.0f, ManaRegen = 0.0f, MagicPen = 0.0f;   // per 5 sec; flat
        float LifeSteal = 0.0f;   // percent of a basic attack's damage, back as health
    };

    // An item's League UNIQUE effects (world `rift_league_item_unique`), as the
    // stats RiftProgression gives them: the highest of a name's rows counts and
    // different names add up; an empty name is the item's own (a second copy
    // adds nothing), "*" counts every copy held. The Aura stats are what the
    // holder gives the champions near it, allies or enemies; a name never
    // stacks there either, from however many holders.
    enum class Unique : uint8
    {
        CooldownReduction,      // percent; with the Crest and Mejai's, capped at 40
        ArmorPen,               // flat, League armor
        MagicPen,               // flat, League magic resist (a unique: Eyes of Pain)
        MagicPenPct,            // percent, before the flat
        SpellVamp,              // percent of an ability's damage as health (a third for an area one)
        Tenacity,               // percent off crowd control
        GoldPer10,              // League gold every 10 sec
        Greed,                  // League gold for a unit killed
        SlowResist,             // percent off slows
        OutOfCombatSpeed,       // League flat move speed out of combat (Boots of Mobility)
        BasicBlockPct,          // percent off basic attacks taken (Ninja Tabi)
        ChampionBasicBlock,     // League damage off champions' basic attacks (Doran's Shield)
        OnHitHeal,              // League health a basic attack (Doran's Blade)
        Maim,                   // League magic damage a basic attack does monsters
        Butcher,                // percent more damage to monsters
        AdPerHealth,            // percent of League maximum health as attack damage (Atma's)
        ManaFont,               // mana regeneration +1% for every 1% of mana missing
        HealAmp,                // percent more self-healing, regeneration, life steal, spell vamp
        ChampionHitRegen,       // League health regen per 5 sec for 10 sec after an enemy champion's damage
        UnitKillArmorAp,        // League armor and ability power a unit kill, 30 stacks (Seeker's)
        TakedownAp,             // League ability power a stack, 20 stacks (Mejai's)
        TakedownAd,             // League attack damage a stack, 20 stacks (Sword of the Occult)
        TakedownManaPct,        // percent of maximum mana on a kill or assist (Athene's)
        ColdSteelAs,            // percent attack speed off a basic attacker, 1 sec
        ColdSteelMs,            // percent move speed off it (Randuin's)
        AuraAllyMr, AuraAllyHp5, AuraAllyMp5, AuraAllyAp, AuraAllySpellVamp, AuraAllyAd, AuraAllyLs,
        AuraEnemyMr,            // League magic resist off nearby enemies
        AuraEnemyAs,            // percent attack speed off nearby enemies
        Count
    };
    inline bool IsAura(Unique stat) { return stat >= Unique::AuraAllyMr && stat < Unique::Count; }
    inline bool IsEnemyAura(Unique stat) { return stat == Unique::AuraEnemyMr || stat == Unique::AuraEnemyAs; }

    struct ItemUnique
    {
        std::string Name;
        Unique Stat = Unique::Count;
        float Value = 0.0f;
    };

    struct Tables
    {
        std::unordered_map<std::string, UnitStats> Units;
        std::unordered_map<uint8, ChampionStats> Champions;
        std::unordered_map<uint32, ItemStats> Items;
        std::unordered_map<uint32, std::vector<ItemUnique>> Uniques;
    };

    // Reads the three tables (world startup, and every Rift match as it is set up,
    // so a table edit plays from the next match). A realm without them keeps the
    // previous snapshot - empty, and the Rift then has no League numbers to play.
    TC_GAME_API void Load();
    TC_GAME_API std::shared_ptr<Tables const> Get();

    TC_GAME_API UnitStats const* FindUnit(Tables const& tables, std::string const& kind);
    TC_GAME_API ChampionStats const* FindChampion(Tables const& tables, uint8 playerClass);
    TC_GAME_API ItemStats const* FindItem(Tables const& tables, uint32 entry);

    TC_GAME_API UnitAt At(UnitStats const& unit, float minutes);

    // League -> WoW. Scale from the average League level (1..18, fractional).
    TC_GAME_API float Scale(float leagueLevel);
    // WoW's armor constant for an attacker of `level`: reduction = A / (A + K).
    TC_GAME_API float ArmorConstant(uint8 level);
    TC_GAME_API float ToWowArmor(float leagueArmor, uint8 level);
    // This fork's resistance reduction is R / 300 (Unit::CalculateAverageResistReduction).
    TC_GAME_API float ToWowResist(float leagueResist);
    // And back: the League magic resist a resistance stands for, so League's
    // magic penetration comes off in League's points.
    TC_GAME_API float FromWowResist(float wowResist);
    // What survives `wowArmor` at `level`, League's 100 / (100 + a), uncapped.
    TC_GAME_API float ArmorMultiplier(float wowArmor, uint8 level);
    // League's attack speed cap.
    float constexpr ATTACK_SPEED_CAP = 2.5f;
    // League units to yards on the Rift's map: League's size x MAP_SCALE, 50 units a
    // yard at 1. The one figure for the map's size on the server (BattlegroundSRF lays
    // the map out with it); the map, its terrain and navmesh were converted with the
    // same number (wow/import/rift/scripts/rift_scale.py): change one, change both.
    // 1.5 from 2026-10-19, 1.125 (a quarter smaller) from 2026-10-25.
    float constexpr MAP_SCALE = 1.125f;
    float constexpr YARDS_PER_UNIT = MAP_SCALE / 50.0f;
    constexpr float Yards(float units) { return units * YARDS_PER_UNIT; }
}

#endif
