/*
 * Violet Hold boons - see VioletHoldBoons.h for the design.
 */

#include "VioletHoldBoons.h"

#include "Battleground.h"
#include "Containers.h"
#include "DBCStores.h"
#include "Errors.h"
#include "Log.h"
#include "Player.h"
#include "Random.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"

#include <algorithm>

namespace VioletHoldBoons
{
namespace
{
constexpr uint32 ClassBit(uint8 classId) { return 1u << (classId - 1); }

constexpr uint32 MASK_ALL = CLASSMASK_ALL_PLAYABLE;

// Pure casters have no melee to speak of, so the swing-based boons skip them.
constexpr uint32 MASK_MELEE = MASK_ALL & ~(ClassBit(CLASS_PRIEST) | ClassBit(CLASS_MAGE) | ClassBit(CLASS_WARLOCK));

// Classes with real spellcasting: cast time, spell crit and doublecast. Hunters
// are out - their shots are ranged-class and scale off ranged haste/crit.
constexpr uint32 MASK_CASTER = ClassBit(CLASS_PALADIN) | ClassBit(CLASS_PRIEST) | ClassBit(CLASS_SHAMAN)
                             | ClassBit(CLASS_MAGE) | ClassBit(CLASS_WARLOCK) | ClassBit(CLASS_DRUID);

constexpr uint32 MASK_MANA   = MASK_CASTER | ClassBit(CLASS_HUNTER);
constexpr uint32 MASK_RAGE   = ClassBit(CLASS_WARRIOR) | ClassBit(CLASS_DRUID);
constexpr uint32 MASK_ENERGY = ClassBit(CLASS_ROGUE) | ClassBit(CLASS_DRUID);

// Order matches enum Boon. maxStacks MUST match the spell's StackAmount in
// Spell.dbc - the DBC is what Aura::ModStackAmount clamps against; the value
// here only drives the "cap" readout and the offer filter.
BoonInfo const kBoons[uint8(Boon::Max)] =
{
    { Boon::Speed,        SPELL_BOON_SPEED,        8, 255, MASK_ALL,    "Boon of Swiftness",    "movement speed",                          "%" },
    { Boon::Damage,       SPELL_BOON_DAMAGE,       3, 255, MASK_ALL,    "Boon of Might",        "damage done",                             "%" },
    { Boon::DamageTaken,  SPELL_BOON_DAMAGE_TAKEN, 3,  50, MASK_ALL,    "Boon of Fortitude",    "less damage taken",                       "%" },
    { Boon::CcDuration,   SPELL_BOON_CC_DURATION, 15,  90, MASK_ALL,    "Boon of Resolve",      "shorter crowd control on you",            "%" },
    { Boon::ManaCost,     SPELL_BOON_MANA_COST,   10, 100, MASK_MANA,   "Boon of Clarity",      "cheaper mana costs",                      "%" },
    { Boon::RageCost,     SPELL_BOON_RAGE_COST,   10,  75, MASK_RAGE,   "Boon of Fury",         "cheaper rage costs",                      "%" },
    { Boon::EnergyCost,   SPELL_BOON_ENERGY_COST, 10,  75, MASK_ENERGY, "Boon of Finesse",      "cheaper energy costs",                    "%" },
    { Boon::MaxHealth,    SPELL_BOON_MAX_HEALTH,   5, 255, MASK_ALL,    "Boon of Vitality",     "maximum health",                          "%" },
    { Boon::ExtraAttack,  SPELL_BOON_EXTRA_ATTACK, 3, 100, MASK_MELEE,  "Boon of Flurry",       "chance for attacks to swing again",       "%" },
    { Boon::Doublecast,   SPELL_BOON_DOUBLECAST,   3, 100, MASK_CASTER, "Boon of Echoes",       "chance for damage/heal spells to double", "%" },
    { Boon::Crit,         SPELL_BOON_CRIT,         3, 255, MASK_MELEE,  "Boon of Precision",    "melee and ranged crit",                   "%" },
    { Boon::SpellCrit,    SPELL_BOON_SPELL_CRIT,   3, 255, MASK_CASTER, "Boon of Insight",      "spell crit",                              "%" },
    { Boon::AttackSpeed,  SPELL_BOON_ATTACK_SPEED, 4, 255, MASK_MELEE,  "Boon of Haste",        "attack speed",                            "%" },
    { Boon::CastSpeed,    SPELL_BOON_CAST_SPEED,   4, 100, MASK_CASTER, "Boon of Celerity",     "casting speed",                           "%" },
    { Boon::MountSpeed,   SPELL_BOON_MOUNT_SPEED,  1,   1, MASK_ALL,    "Boon of the Outrider", "mounts summon 3 sec faster (unique)",     "" },
    { Boon::Cooldown,     SPELL_BOON_COOLDOWN,     5, 100, MASK_ALL,    "Boon of Alacrity",     "cooldown reduction",                      "%" },
    { Boon::Resistance,   SPELL_BOON_RESISTANCE,   5, 255, MASK_ALL,    "Boon of Warding",      "to all resistances",                      "" },
    { Boon::Level,        SPELL_BOON_LEVEL,        1, 100, MASK_ALL,    "Boon of Ascension",    "level, with its talent point (until you leave)", "" }
};

// Three per class. spellId is the FIRST rank where a chain exists
// (Shadowfury 30283 -> r2 30413 at 60; Water Shield 52127 -> r6 52138 at 60;
// Penance 47540 is r1 at 60 already) or the single spell. Utility spells that
// only appear past 60 (Master's Call 75, Cloak of Shadows 66, Shadowfiend 66,
// Mass Dispel 70, Demonic Circle 80) have no level-scaled numbers and are
// taught as they are - ResolveRank falls back to the first rank. Lava Burst,
// Nourish and Spell Reflection DO scale (or, for Spell Reflection, no longer
// exist as an active here), so they got level-60 editions.
ClassSpellInfo const kClassSpells[CLASS_SPELL_COUNT] =
{
    { CLASS_HUNTER,  34490, 0,     0,     "Silencing Shot" },
    { CLASS_HUNTER,    781, 0,     0,     "Disengage" },
    { CLASS_HUNTER,  53271, 0,     0,     "Master's Call" },

    { CLASS_MAGE,    11129, 0,     0,     "Combustion" },
    { CLASS_MAGE,    31687, 0,     0,     "Summon Water Elemental" },
    { CLASS_MAGE,    31589, 0,     0,     "Slow" },

    { CLASS_ROGUE,   51690, 0,     0,     "Killing Spree" },
    { CLASS_ROGUE,   31224, 0,     0,     "Cloak of Shadows" },
    { CLASS_ROGUE,   51713, 0,     0,     "Shadow Dance" },

    { CLASS_DRUID,   SPELL_L60_NOURISH, 0, 50464, "Nourish" },
    { CLASS_DRUID,   33831, 0,     0,     "Force of Nature" },
    { CLASS_DRUID,   50334, 0,     0,     "Berserk" },

    { CLASS_PALADIN, 53385, 0,     0,     "Divine Storm" },
    { CLASS_PALADIN, 53595, 0,     0,     "Hammer of the Righteous" },
    { CLASS_PALADIN, 53563, 0,     0,     "Beacon of Light" },

    { CLASS_PRIEST,  34433, 0,     0,     "Shadowfiend" },
    { CLASS_PRIEST,  47540, 0,     0,     "Penance" },
    { CLASS_PRIEST,  32375, 0,     0,     "Mass Dispel" },

    { CLASS_SHAMAN,  51533, 0,     0,     "Feral Spirit" },
    { CLASS_SHAMAN,  SPELL_L60_LAVA_BURST, 0, 51505, "Lava Burst" },
    { CLASS_SHAMAN,  52127, 0,     0,     "Water Shield" },

    { CLASS_WARLOCK, 48018, 48020, 0,     "Demonic Circle" },
    { CLASS_WARLOCK, 30283, 0,     0,     "Shadowfury" },
    { CLASS_WARLOCK, 30146, 0,     0,     "Summon Felguard" },

    { CLASS_WARRIOR, 46924, 0,     0,     "Bladestorm" },
    { CLASS_WARRIOR, SPELL_L60_SPELL_REFLECTION, 0, 0, "Spell Reflection" },
    { CLASS_WARRIOR, 60970, 0,     0,     "Heroic Fury" }
};

// Mechanics that count as crowd control for Boon of Resolve. Slows, dazes,
// disarms and interrupt lockouts are deliberately left out.
constexpr uint32 CC_MECHANIC_MASK =
      (1 << MECHANIC_CHARM)
    | (1 << MECHANIC_DISORIENTED)
    | (1 << MECHANIC_FEAR)
    | (1 << MECHANIC_GRIP)
    | (1 << MECHANIC_ROOT)
    | (1 << MECHANIC_SILENCE)
    | (1 << MECHANIC_SLEEP)
    | (1 << MECHANIC_STUN)
    | (1 << MECHANIC_FREEZE)
    | (1 << MECHANIC_KNOCKOUT)
    | (1 << MECHANIC_POLYMORPH)
    | (1 << MECHANIC_BANISH)
    | (1 << MECHANIC_SHACKLE)
    | (1 << MECHANIC_TURN)
    | (1 << MECHANIC_HORROR)
    | (1 << MECHANIC_SAPPED);

int32 AmountOf(Unit const* unit, uint32 spellId, int32 cap)
{
    if (!unit)
        return 0;

    AuraEffect const* eff = unit->GetAuraEffect(spellId, EFFECT_0);
    if (!eff)
        return 0;

    return std::clamp<int32>(eff->GetAmount(), 0, cap);
}

// Every rank of the chain `firstRankSpellId` heads (just itself when it is
// not ranked), first to last.
std::vector<uint32> ChainOf(uint32 firstRankSpellId)
{
    std::vector<uint32> ranks;
    SpellInfo const* info = sSpellMgr->GetSpellInfo(firstRankSpellId);
    if (!info)
        return ranks;

    for (SpellInfo const* rank = info->GetFirstRankSpell(); rank; rank = rank->GetNextRankSpell())
    {
        ranks.push_back(rank->Id);
        if (ranks.size() > 32)   // a cycle in spell_ranks would be a data bug; do not spin on it
            break;
    }
    return ranks;
}

// All spells that mean "has this class spell": every rank of the spell, of
// its alias, and the companion.
std::vector<uint32> AllIdsOf(ClassSpellInfo const& info)
{
    std::vector<uint32> ids = ChainOf(info.spellId);
    if (info.alias)
        for (uint32 id : ChainOf(info.alias))
            ids.push_back(id);
    if (info.companion)
        ids.push_back(info.companion);
    return ids;
}

bool KnowsAny(Player const* player, ClassSpellInfo const& info)
{
    for (uint32 id : AllIdsOf(info))
        if (player->HasSpell(id))
            return true;
    return false;
}

// Was this spell put on the character by the broker? Taught spells are
// dependent (never saved); trained and talent spells are not.
bool IsTaughtByBroker(Player const* player, uint32 spellId)
{
    PlayerSpellMap const& spells = player->GetSpellMap();
    auto itr = spells.find(spellId);
    if (itr == spells.end())
        return false;
    return itr->second.state != PLAYERSPELL_REMOVED && itr->second.dependent;
}
}

BoonInfo const& GetBoon(Boon boon)
{
    ASSERT(boon < Boon::Max);
    return kBoons[uint8(boon)];
}

ClassSpellInfo const& GetClassSpell(uint8 index)
{
    ASSERT(index < CLASS_SPELL_COUNT);
    return kClassSpells[index];
}

BoonInfo const* FindBySpell(uint32 spellId)
{
    for (BoonInfo const& info : kBoons)
        if (info.spellId == spellId)
            return &info;
    return nullptr;
}

bool IsBoonSpell(uint32 spellId)
{
    return (spellId >= SPELL_BOON_FIRST && spellId <= SPELL_BOON_LAST) || spellId == SPELL_BOON_EXTRA_ATTACK_SWING;
}

bool Offer::Decode(uint32 code, Offer& out)
{
    if (code < uint8(Boon::Max))
    {
        out.kind = Kind::Boon;
        out.index = uint8(code);
        return true;
    }
    if (code >= 100 && code < 100 + CLASS_SPELL_COUNT)
    {
        out.kind = Kind::ClassSpell;
        out.index = uint8(code - 100);
        return true;
    }
    return false;
}

uint8 GetStacks(Player const* player, Boon boon)
{
    if (!player)
        return 0;

    if (Aura const* aura = player->GetAura(GetBoon(boon).spellId))
        return aura->GetStackAmount();
    return 0;
}

uint32 ResolveRank(uint32 firstRankSpellId, uint8 level)
{
    uint32 best = firstRankSpellId;
    for (uint32 id : ChainOf(firstRankSpellId))
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(id);
        if (!info)
            continue;
        if (info->SpellLevel >= 1 && info->SpellLevel <= level)
            best = id;
    }
    return best;
}

PickResult CanTake(Player const* player, Offer offer)
{
    if (!player)
        return PickResult::Failed;

    if (offer.kind == Offer::Kind::Boon)
    {
        if (offer.index >= uint8(Boon::Max))
            return PickResult::Failed;

        BoonInfo const& info = kBoons[offer.index];
        if (!(info.classMask & player->GetClassMask()))
            return PickResult::WrongClass;

        if (info.boon == Boon::Level)
            return player->GetLevel() < LEVEL_BOON_CEILING ? PickResult::Ok : PickResult::LevelCeiling;

        return GetStacks(player, info.boon) < info.maxStacks ? PickResult::Ok : PickResult::AtCap;
    }

    if (offer.index >= CLASS_SPELL_COUNT)
        return PickResult::Failed;

    ClassSpellInfo const& info = kClassSpells[offer.index];
    if (player->GetClass() != info.classId)
        return PickResult::WrongClass;

    if (KnowsAny(player, info))
        return PickResult::AlreadyKnown;

    return PickResult::Ok;
}

std::vector<Offer> RollOffers(std::vector<Player const*> const& roster)
{
    // "Only rolls when it makes sense": something is in the pool while at
    // least one person on the team could take it. Two hunters and one who
    // already has Silencing Shot still leaves it rollable for the other.
    auto anyoneCan = [&roster](Offer const& offer)
    {
        for (Player const* member : roster)
            if (member && CanTake(member, offer) == PickResult::Ok)
                return true;
        return false;
    };

    std::vector<Offer> pool;
    pool.reserve(uint8(Boon::Max) + CLASS_SPELL_COUNT);

    for (uint8 i = 0; i < uint8(Boon::Max); ++i)
    {
        Offer offer{ Offer::Kind::Boon, i };
        if (anyoneCan(offer))
            pool.push_back(offer);
    }
    for (uint8 i = 0; i < CLASS_SPELL_COUNT; ++i)
    {
        Offer offer{ Offer::Kind::ClassSpell, i };
        if (anyoneCan(offer))
            pool.push_back(offer);
    }

    Trinity::Containers::RandomShuffle(pool);
    if (pool.size() > OFFERS_PER_BROKER)
        pool.resize(OFFERS_PER_BROKER);

    return pool;
}

PickResult Take(Player* player, Offer offer)
{
    PickResult const check = CanTake(player, offer);
    if (check != PickResult::Ok)
        return check;

    if (offer.kind == Offer::Kind::ClassSpell)
    {
        ClassSpellInfo const& info = kClassSpells[offer.index];
        uint32 const rank = ResolveRank(info.spellId, player->GetLevel());
        if (!sSpellMgr->GetSpellInfo(rank))
        {
            TC_LOG_ERROR("bg.battleground", "VioletHoldBoons::Take: class spell {} ({}) resolved to unknown spell {}.",
                info.name, info.spellId, rank);
            return PickResult::Failed;
        }

        // Player::AddSpell books a talent spell's cost against
        // m_usedTalentCount, and a strip would not give it back - that is a
        // guaranteed talent wipe at the next InitTalentForLevel. None of the
        // 27 sit in this realm's (classic) Talent.dbc today; refuse rather
        // than corrupt if that ever changes.
        if (GetTalentSpellCost(rank) > 0 || (info.companion && GetTalentSpellCost(info.companion) > 0))
        {
            TC_LOG_ERROR("bg.battleground", "VioletHoldBoons::Take: class spell {} ({}) is a talent spell on this realm; not teaching it.",
                info.name, rank);
            return PickResult::Failed;
        }

        // Dependent: never written to character_spell, so it evaporates on
        // logout. AddSpell also teaches the lower ranks the same way, which is
        // why KnowsAny refuses anyone who already has ANY rank - a real rank
        // would otherwise be flipped to dependent and lost on save.
        player->LearnSpell(rank, true);
        if (info.companion)
            player->LearnSpell(info.companion, true);

        if (!player->HasSpell(rank))
        {
            TC_LOG_ERROR("bg.battleground", "VioletHoldBoons::Take: {} did not learn {} ({}).",
                player->GetGUID().ToString(), info.name, rank);
            return PickResult::Failed;
        }
        return PickResult::Ok;
    }

    BoonInfo const& info = kBoons[offer.index];

    if (info.boon == Boon::Level)
    {
        uint8 const oldLevel = player->GetLevel();

        // The marker goes on first, carrying the level being left behind in its
        // base amount, so a crash between the two calls can only ever be undone
        // in the player's favour.
        if (Aura* marker = player->GetAura(SPELL_BOON_LEVEL))
            marker->ModStackAmount(1);
        else
        {
            CastSpellExtraArgs args(TRIGGERED_FULL_MASK);
            args.AddSpellBP0(int32(oldLevel));
            player->CastSpell(player, SPELL_BOON_LEVEL, args);
            if (!player->HasAura(SPELL_BOON_LEVEL))
            {
                TC_LOG_ERROR("bg.battleground", "VioletHoldBoons::Take: level marker {} did not apply to {}; not levelling.",
                    uint32(SPELL_BOON_LEVEL), player->GetGUID().ToString());
                return PickResult::Failed;
            }
        }

        player->GiveLevel(uint8(oldLevel + 1));
        return PickResult::Ok;
    }

    Aura* aura = player->GetAura(info.spellId);
    if (!aura)
    {
        player->CastSpell(player, info.spellId, true);
        aura = player->GetAura(info.spellId);
        if (!aura)
        {
            TC_LOG_ERROR("bg.battleground", "VioletHoldBoons::Take: boon spell {} did not apply to {}.",
                info.spellId, player->GetGUID().ToString());
            return PickResult::Failed;
        }

        // The cast made stack one; ModStackAmount clamps to the DBC cap.
        if (info.grantStacks > 1)
            aura->ModStackAmount(info.grantStacks - 1);
    }
    else
        aura->ModStackAmount(info.grantStacks);

    return PickResult::Ok;
}

char const* GetPickResultText(PickResult result)
{
    switch (result)
    {
        case PickResult::Ok:           return "Done.";
        case PickResult::WrongClass:   return "That is not something you can learn or use. Someone else in your party can.";
        case PickResult::AlreadyKnown: return "You already know that.";
        case PickResult::AtCap:        return "You already hold as much of that as you can.";
        case PickResult::LevelCeiling: return "You cannot be raised any higher.";
        default:                       return "The broker's offer fizzles.";
    }
}

void StripAll(Player* player)
{
    if (!player)
        return;

    // Levels first, while the marker still exists to say where "back" is.
    if (Aura const* marker = player->GetAura(SPELL_BOON_LEVEL))
    {
        if (AuraEffect const* eff = marker->GetEffect(EFFECT_0))
        {
            int32 const baseLevel = eff->GetBaseAmount();
            if (baseLevel > 0 && baseLevel < int32(player->GetLevel()))
                player->GiveLevel(uint8(baseLevel));
        }
    }

    for (BoonInfo const& info : kBoons)
        player->RemoveAurasDueToSpell(info.spellId);

    // Taught class spells: every rank the broker put on, top rank first so
    // RemoveSpell's own "unlearn higher ranks" recursion never surprises us.
    // Only dependent entries go - a genuinely trained rank is never dependent.
    for (ClassSpellInfo const& info : kClassSpells)
    {
        std::vector<uint32> ids = AllIdsOf(info);
        for (auto itr = ids.rbegin(); itr != ids.rend(); ++itr)
            if (IsTaughtByBroker(player, *itr))
                player->RemoveSpell(*itr, false, false);
    }
}

void OnLogin(Player* player)
{
    if (!player)
        return;

    if (Battleground const* bg = player->GetBattleground())
        if (bg->GetTypeID() == BATTLEGROUND_VHR)
            return;

    bool carrying = false;
    for (BoonInfo const& info : kBoons)
        if (player->HasAura(info.spellId))
        {
            carrying = true;
            break;
        }

    if (!carrying)
        return;

    TC_LOG_DEBUG("bg.battleground", "VioletHoldBoons::OnLogin: {} logged in outside the Violet Hold carrying boons; stripping.",
        player->GetGUID().ToString());
    StripAll(player);
}

std::string DescribeOffer(Player const* viewer, Offer offer)
{
    if (offer.kind == Offer::Kind::ClassSpell)
    {
        if (offer.index >= CLASS_SPELL_COUNT)
            return "?";
        ClassSpellInfo const& info = kClassSpells[offer.index];
        char const* className = "?";
        if (ChrClassesEntry const* entry = sChrClassesStore.LookupEntry(info.classId))
            className = entry->Name[LOCALE_enUS];
        return Trinity::StringFormat("Learn {} ({})", info.name, className);
    }

    if (offer.index >= uint8(Boon::Max))
        return "?";

    BoonInfo const& info = kBoons[offer.index];
    uint8 const held = GetStacks(viewer, info.boon);

    switch (info.boon)
    {
        case Boon::MountSpeed:
            return Trinity::StringFormat("{}: {}", info.name, info.summary);
        case Boon::Level:
            return Trinity::StringFormat("{}: +1 {}", info.name, info.summary);
        default:
            return Trinity::StringFormat("{}: +{}{} {} [{}/{}{}]", info.name, uint32(info.grantStacks), info.unit,
                info.summary, uint32(held), uint32(info.maxStacks), info.unit);
    }
}

int32 GetCcDurationReductionPct(Unit const* target, uint32 mechanicMask)
{
    if (!(mechanicMask & CC_MECHANIC_MASK))
        return 0;
    return AmountOf(target, SPELL_BOON_CC_DURATION, 90);
}

int32 GetPowerCostReductionPct(Unit const* caster, Powers power)
{
    switch (power)
    {
        case POWER_MANA:   return AmountOf(caster, SPELL_BOON_MANA_COST, 100);
        case POWER_RAGE:   return AmountOf(caster, SPELL_BOON_RAGE_COST, 100);
        case POWER_ENERGY: return AmountOf(caster, SPELL_BOON_ENERGY_COST, 100);
        default:           return 0;
    }
}

int32 GetMountCastTimeReductionMs(Unit const* caster)
{
    return AmountOf(caster, SPELL_BOON_MOUNT_SPEED, 3000);
}

int32 GetCooldownReductionPct(Unit const* caster)
{
    return AmountOf(caster, SPELL_BOON_COOLDOWN, 100);
}
}
