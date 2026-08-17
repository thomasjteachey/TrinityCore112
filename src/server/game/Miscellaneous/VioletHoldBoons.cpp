/*
 * Violet Hold boons - see VioletHoldBoons.h for the design.
 */

#include "VioletHoldBoons.h"

#include "Battleground.h"
#include "BattlegroundVHR.h"
#include "CellImpl.h"
#include "Containers.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Map.h"
#include "PetAI.h"
#include "TemporarySummon.h"
#include "WorldSession.h"
#include "DBCStores.h"
#include "Errors.h"
#include "Item.h"
#include "ItemEnchantmentMgr.h"
#include "ItemTemplate.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Random.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <list>

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

// Classes that ever field a pet or guardian on this realm (no death knights).
constexpr uint32 MASK_PETS   = ClassBit(CLASS_HUNTER) | ClassBit(CLASS_WARLOCK) | ClassBit(CLASS_MAGE)
                             | ClassBit(CLASS_PRIEST) | ClassBit(CLASS_SHAMAN) | ClassBit(CLASS_DRUID);

// Order matches enum Boon. maxStacks MUST match the spell's StackAmount in
// Spell.dbc - the DBC is what Aura::ModStackAmount clamps against; the value
// here only drives the "cap" readout and the offer filter.
// Weights follow the realm's item-budget scale (helper.sp_EstimateItemLevels:
// a primary-stat point is worth 230, 1% crit 3200, 1% spell crit 2600, a
// resist point 230 per school): the cheaper a pick is in that currency, the
// more often it rolls. So +5% stats sit at 10, +3% crit / damage at 5, the
// run-defining ones (Echoes, Alacrity, Ascension) lower still, the pure fluff
// (Outrider) at 2. Class spells roll at CLASS_SPELL_WEIGHT (2), legendaries at 2.
BoonInfo const kBoons[uint8(Boon::Max)] =
{
    { Boon::Speed,        SPELL_BOON_SPEED,        8, 255, MASK_ALL,    10, "Boon of Swiftness",    "movement speed",                          "%" },
    { Boon::Damage,       SPELL_BOON_DAMAGE,       3, 255, MASK_ALL,      5, "Boon of Might",        "damage done",                             "%" },
    { Boon::DamageTaken,  SPELL_BOON_DAMAGE_TAKEN, 3,  50, MASK_ALL,     8, "Boon of Fortitude",    "less damage taken",                       "%" },
    { Boon::CcDuration,   SPELL_BOON_CC_DURATION, 15,  90, MASK_ALL,     7, "Boon of Resolve",      "shorter crowd control (stuns, roots, snares, fears...) and interrupt lockouts on you", "%" },
    { Boon::ManaCost,     SPELL_BOON_MANA_COST,   10, 100, MASK_MANA,    7, "Boon of Clarity",      "cheaper mana costs",                      "%" },
    { Boon::RageCost,     SPELL_BOON_RAGE_COST,   10,  75, MASK_RAGE,    7, "Boon of Fury",         "cheaper rage costs",                      "%" },
    { Boon::EnergyCost,   SPELL_BOON_ENERGY_COST, 10,  75, MASK_ENERGY,  7, "Boon of Finesse",      "cheaper energy costs",                    "%" },
    { Boon::Stamina,      SPELL_BOON_STAMINA,      5, 255, MASK_ALL,     10, "Boon of Vitality",     "Stamina",                                 "%" },
    { Boon::ExtraAttack,  SPELL_BOON_EXTRA_ATTACK, 3, 100, MASK_MELEE,   6, "Boon of Flurry",       "chance for attacks to swing again",       "%" },
    { Boon::Doublecast,   SPELL_BOON_DOUBLECAST,   3, 100, MASK_CASTER,  5, "Boon of Echoes",       "chance for damage/heal spells to double", "%" },
    { Boon::Crit,         SPELL_BOON_CRIT,         3, 100, MASK_MELEE,    5, "Boon of Precision",    "melee and ranged crit",                   "%" },
    { Boon::SpellCrit,    SPELL_BOON_SPELL_CRIT,   3, 100, MASK_CASTER,   5, "Boon of Insight",      "spell crit",                              "%" },
    { Boon::AttackSpeed,  SPELL_BOON_ATTACK_SPEED, 4, 255, MASK_MELEE,    6, "Boon of Haste",        "attack speed",                            "%" },
    { Boon::CastSpeed,    SPELL_BOON_CAST_SPEED,   4, 100, MASK_CASTER,   6, "Boon of Celerity",     "casting speed",                           "%" },
    { Boon::MountSpeed,   SPELL_BOON_MOUNT_SPEED,  1,   1, MASK_ALL,     2, "Boon of the Outrider", "mounts summon instantly, even in combat (unique)", "" },
    { Boon::Cooldown,     SPELL_BOON_COOLDOWN,     5, 100, MASK_ALL,     4, "Boon of Alacrity",     "cooldown reduction",                      "%" },
    { Boon::Resistance,   SPELL_BOON_RESISTANCE,   5, 255, MASK_ALL,      6, "Boon of Warding",      "to all resistances",                      "" },
    { Boon::Level,        SPELL_BOON_LEVEL,        1, 100, MASK_ALL,     3, "Boon of Ascension",    "level, with its talent point (until you leave)", "" },
    { Boon::Strength,     SPELL_BOON_STRENGTH,     5, 255, MASK_ALL,     10, "Boon of Brawn",        "Strength",                                "%" },
    { Boon::Agility,      SPELL_BOON_AGILITY,      5, 255, MASK_ALL,     10, "Boon of Grace",        "Agility",                                 "%" },
    { Boon::Intellect,    SPELL_BOON_INTELLECT,    5, 255, MASK_ALL,     10, "Boon of Wit",          "Intellect",                               "%" },
    { Boon::Spirit,       SPELL_BOON_SPIRIT,       5, 255, MASK_ALL,     10, "Boon of Soul",         "Spirit",                                  "%" },
    { Boon::EnergyRegen,  SPELL_BOON_ENERGY_REGEN, 20, 100, MASK_ENERGY,  7, "Boon of Vigor",        "energy regeneration",                     "%" },
    { Boon::RageGeneration, SPELL_BOON_RAGE_GEN,   20, 100, MASK_RAGE,    7, "Boon of Wrath",        "rage generation",                         "%" },

    // Second batch. The run-wide ones (Greed, Hoarder, Fellowship) count and
    // cap against the RUN, not the taker - see CanTake/GetStacks; their aura on
    // the taker is only the record of who bought them.
    { Boon::Ricochet,     SPELL_BOON_RICOCHET,     5,  50, MASK_ALL,      4, "Boon of Ricochet",     "chance for damage to ricochet to a nearby enemy for half", "%" },
    { Boon::Overkill,     SPELL_BOON_OVERKILL,     1,   1, MASK_ALL,      3, "Boon of Overkill",     "all overkill damage from your killing blows splashes enemies within 15 yd (unique)", "" },
    { Boon::Reflection,   SPELL_BOON_REFLECTION,  10,  50, MASK_ALL,      5, "Boon of Reflection",   "of damage taken reflected",               "%" },
    { Boon::Vampire,      SPELL_BOON_VAMPIRE,      3,  30, MASK_ALL,      4, "Boon of the Vampire",  "of damage dealt returned as health",      "%" },
    { Boon::Phoenix,      SPELL_BOON_PHOENIX,      1,   1, MASK_ALL,      2, "Boon of the Phoenix",  "survive one killing blow at full health and mana (unique)", "" },
    { Boon::Greed,        SPELL_BOON_GREED,        1,   3, MASK_ALL,      3, "Boon of Greed",        "extra option at every broker from now on (run-wide)",    "" },
    { Boon::Hoarder,      SPELL_BOON_HOARDER,      1,   1, MASK_ALL,      2, "Boon of the Hoarder",  "extra broker after every wave from now on (run-wide, unique)", "" },
    { Boon::Beastmaster,  SPELL_BOON_BEASTMASTER, 10, 100, MASK_PETS,     5, "Boon of the Beastmaster", "pet and guardian damage and health",  "%" },
    { Boon::Menagerie,    SPELL_BOON_MENAGERIE,    1,   3, MASK_ALL,      4, "Boon of the Menagerie", "random guardian fights beside you each wave", "" },
    { Boon::Fellowship,   SPELL_BOON_FELLOWSHIP,   1,   3, MASK_ALL,      3, "Boon of Fellowship",   "a random champion joins your side for the rest of the run (run-wide)", "" }
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

// Legendary weapons for the run. Offered while someone of a listed class is
// in the party; the item is a real one in the bags (soulbound, so it cannot be
// traded away inside) and is destroyed on the way out via SPELL_CACHE_MARKER.
ItemGrantInfo const kItemGrants[ITEM_GRANT_COUNT] =
{
    { 19019, ClassBit(CLASS_WARRIOR) | ClassBit(CLASS_ROGUE) | ClassBit(CLASS_HUNTER) | ClassBit(CLASS_PALADIN), 2, "Thunderfury, Blessed Blade of the Windseeker" },
    { 17182, ClassBit(CLASS_WARRIOR) | ClassBit(CLASS_PALADIN) | ClassBit(CLASS_SHAMAN),                          2, "Sulfuras, Hand of Ragnaros" }
};

// Both weapons come with Crusader on them (SpellItemEnchantment 1900).
constexpr uint32 ENCHANT_CRUSADER = 1900;

// Mechanics that count as crowd control for Boon of Resolve. Disarms are
// left out; snares and dazes (Crippling Poison, Hamstring, Frostbolt's slow,
// Concussive Shot) are in. Interrupt lockouts ARE in: Kick /
// Pummel / Counterspell / Spell Lock / Mind Freeze carry MECHANIC_INTERRUPT
// (on the spell or the interrupt effect) and Spell::EffectInterruptCast runs
// the lockout duration through ModSpellDuration with that effect's mask, so
// the same hook shortens the school lock (SpellHistory::LockSpellSchool tells
// the client the exact server duration - nothing to sync).
constexpr uint32 CC_MECHANIC_MASK =
      (1 << MECHANIC_CHARM)
    | (1 << MECHANIC_INTERRUPT)
    | (1 << MECHANIC_SNARE)
    | (1 << MECHANIC_DAZE)
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

// The Violet Hold run a player is standing in, or nullptr.
BattlegroundVHR* GetRunOf(Player const* player)
{
    if (!player)
        return nullptr;
    Battleground* bg = player->GetBattleground();
    if (!bg || bg->GetTypeID() != BATTLEGROUND_VHR)
        return nullptr;
    return static_cast<BattlegroundVHR*>(bg);
}

// Run-wide boons: how much of it the RUN already holds (what the [x/cap]
// readout and the cap check use). -1 for boons that are per player.
int32 RunHeld(Player const* player, Boon boon)
{
    switch (boon)
    {
        case Boon::Greed:
        case Boon::Hoarder:
        case Boon::Fellowship:
            break;
        default:
            return -1;
    }

    BattlegroundVHR const* run = GetRunOf(player);
    if (!run)
        return 0;

    switch (boon)
    {
        case Boon::Greed:      return int32(run->GetBrokerBonusOffers());
        case Boon::Hoarder:    return int32(run->GetBonusBrokersPerWave());
        // Spawned allies plus the requests still with the driver, so a pick
        // already promised is not offered twice and the [x/cap] readout says
        // the same thing the cap check does.
        case Boon::Fellowship: return int32(run->GetAllyCount() + run->GetPendingAllyRequestCount());
        default:               return 0;
    }
}

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

ClassSpellInfo const* FindClassSpell(uint32 tableSpellId)
{
    if (!tableSpellId)
        return nullptr;
    for (ClassSpellInfo const& info : kClassSpells)
        if (info.spellId == tableSpellId)
            return &info;
    return nullptr;
}

// A marker aura's three slots, kept in its effect BASE amounts (the saved,
// stack-independent number): the knowledge marker holds table spellIds of
// what the broker taught, the cache marker the low guids of the items he
// handed out. 0 = empty. Read back here and rewritten by recasting.
constexpr uint8 MARKER_SLOTS = 3;
using MarkerSlots = std::array<uint32, MARKER_SLOTS>;

MarkerSlots ReadMarker(Player const* player, uint32 markerSpellId)
{
    MarkerSlots slots{};
    Aura const* marker = player->GetAura(markerSpellId);
    if (!marker)
        return slots;

    // Reinterpreted, not clamped: an item low guid past 2^31 comes back as a
    // negative base amount and must round-trip untouched.
    for (uint8 i = 0; i < MARKER_SLOTS; ++i)
        if (AuraEffect const* eff = marker->GetEffect(i))
            slots[i] = uint32(eff->GetBaseAmount());
    return slots;
}

// Recasting an existing self-aura overwrites its base amounts with the new
// cast's values (Unit::_TryStackingOrRefreshingExistingAura), which is what
// makes "rewrite the slots" a plain cast.
void WriteMarker(Player* player, uint32 markerSpellId, MarkerSlots const& slots)
{
    CastSpellExtraArgs args(TRIGGERED_FULL_MASK);
    args.AddSpellMod(SPELLVALUE_BASE_POINT0, int32(slots[0]));
    args.AddSpellMod(SPELLVALUE_BASE_POINT1, int32(slots[1]));
    args.AddSpellMod(SPELLVALUE_BASE_POINT2, int32(slots[2]));
    player->CastSpell(player, markerSpellId, args);
}

MarkerSlots ReadKnowledge(Player const* player) { return ReadMarker(player, SPELL_KNOWLEDGE_MARKER); }

void RecordTaught(Player* player, uint32 tableSpellId)
{
    MarkerSlots slots = ReadKnowledge(player);
    for (uint32 slot : slots)
        if (slot == tableSpellId)
            return;

    for (uint32& slot : slots)
    {
        if (!slot)
        {
            slot = tableSpellId;
            WriteMarker(player, SPELL_KNOWLEDGE_MARKER, slots);
            return;
        }
    }

    TC_LOG_ERROR("bg.battleground", "VioletHoldBoons: no free knowledge slot on {} for class spell {}; it will not survive a relog.",
        player->GetGUID().ToString(), tableSpellId);
}

// The item the cache marker says was granted for table slot `index`, if the
// character still has it anywhere in inventory or bank.
Item* GetGrantedItem(Player* player, uint8 index)
{
    if (index >= MARKER_SLOTS)
        return nullptr;
    uint32 const low = ReadMarker(player, SPELL_CACHE_MARKER)[index];
    if (!low)
        return nullptr;
    return player->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(low));
}

void RecordGranted(Player* player, uint8 index, ObjectGuid itemGuid)
{
    if (index >= MARKER_SLOTS)
        return;
    MarkerSlots slots = ReadMarker(player, SPELL_CACHE_MARKER);
    slots[index] = uint32(itemGuid.GetCounter());
    WriteMarker(player, SPELL_CACHE_MARKER, slots);
}

// The character is about to drop back to `baseLevel`. Take back, newest
// first, exactly as many run-spent talent ranks as needed for the spent total
// to fit that level's allowance - so InitTalentForLevel finds nothing over
// budget and never reaches for its full reset. Ranks bought with points the
// character owned before the run are left standing (they are undone last, and
// only if the arithmetic demands it, which it will not).
void UndoRunTalentsFor(Player* player, uint8 baseLevel)
{
    uint32 const allowed = player->CalculateTalentsPointsForLevel(baseLevel);
    uint32 guard = 0;
    while (player->GetUsedTalentCount() > allowed && !player->GetVioletHoldRunTalents().empty() && guard++ < 128)
    {
        VioletHoldRunTalent const entry = player->GetVioletHoldRunTalents().back();
        player->PopVioletHoldRunTalent();

        uint32 const before = player->GetUsedTalentCount();
        player->UnlearnTalentSpell(entry.spell, entry.previousSpell, baseLevel);
        if (player->GetUsedTalentCount() >= before)
        {
            // Nothing came back (rank already gone?) - keep going, the loop
            // is bounded by the ledger and the guard either way.
            TC_LOG_DEBUG("bg.battleground", "VioletHoldBoons: undoing run talent {} on {} refunded nothing.",
                entry.spell, player->GetGUID().ToString());
        }
    }

    if (player->GetUsedTalentCount() > allowed)
        TC_LOG_ERROR("bg.battleground", "VioletHoldBoons: {} still has {} talent points spent against {} allowed at level {} after undoing the run's talents; InitTalentForLevel will reset.",
            player->GetGUID().ToString(), player->GetUsedTalentCount(), allowed, uint32(baseLevel));
}

// Teach the rank a player of this level gets, plus the companion. The
// caller has already decided the player may have it.
bool TeachClassSpell(Player* player, ClassSpellInfo const& info)
{
    uint32 const rank = ResolveRank(info.spellId, player->GetLevel());
    if (!sSpellMgr->GetSpellInfo(rank))
    {
        TC_LOG_ERROR("bg.battleground", "VioletHoldBoons: class spell {} ({}) resolved to unknown spell {}.",
            info.name, info.spellId, rank);
        return false;
    }

    // Player::AddSpell books a talent spell's cost against
    // m_usedTalentCount, and a strip would not give it back - that is a
    // guaranteed talent wipe at the next InitTalentForLevel. None of the
    // 27 sit in this realm's (classic) Talent.dbc today; refuse rather
    // than corrupt if that ever changes.
    if (GetTalentSpellCost(rank) > 0 || (info.companion && GetTalentSpellCost(info.companion) > 0))
    {
        TC_LOG_ERROR("bg.battleground", "VioletHoldBoons: class spell {} ({}) is a talent spell on this realm; not teaching it.",
            info.name, rank);
        return false;
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
        TC_LOG_ERROR("bg.battleground", "VioletHoldBoons: {} did not learn {} ({}).",
            player->GetGUID().ToString(), info.name, rank);
        return false;
    }
    return true;
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

ItemGrantInfo const& GetItemGrant(uint8 index)
{
    ASSERT(index < ITEM_GRANT_COUNT);
    return kItemGrants[index];
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
    return (spellId >= SPELL_BOON_FIRST && spellId <= SPELL_BOON_LAST)
        || (spellId >= SPELL_BOON_EXTRA_FIRST && spellId <= SPELL_BOON_EXTRA_LAST)
        || (spellId >= SPELL_BOON_COOLDOWN_CLASS_FIRST && spellId <= SPELL_BOON_COOLDOWN_CLASS_LAST)
        || (spellId >= SPELL_BOON_BATCH2_FIRST && spellId <= SPELL_BOON_BATCH2_LAST)
        || spellId == SPELL_KNOWLEDGE_MARKER
        || spellId == SPELL_BOON_EXTRA_ATTACK_SWING;
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
    if (code >= 200 && code < 200 + ITEM_GRANT_COUNT)
    {
        out.kind = Kind::Item;
        out.index = uint8(code - 200);
        return true;
    }
    return false;
}

uint32 GetBoonSpellFor(Boon boon, uint8 classId)
{
    if (boon == Boon::Cooldown && classId >= CLASS_WARRIOR && classId < MAX_CLASSES)
        return SPELL_BOON_COOLDOWN_CLASS_BASE + classId;
    return GetBoon(boon).spellId;
}

uint8 GetStacks(Player const* player, Boon boon)
{
    if (!player)
        return 0;

    // Run-wide boons read the run, not the taker's aura.
    if (int32 held = RunHeld(player, boon); held >= 0)
        return uint8(std::min<int32>(held, 255));

    if (Aura const* aura = player->GetAura(GetBoonSpellFor(boon, player->GetClass())))
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

        // Run-wide boons live on the battleground; without one there is nothing
        // to apply them to (never the case for a broker offer, but CanTake is
        // public).
        if (RunHeld(player, info.boon) >= 0 && !GetRunOf(player))
            return PickResult::Failed;

        return GetStacks(player, info.boon) < info.maxStacks ? PickResult::Ok : PickResult::AtCap;
    }

    if (offer.kind == Offer::Kind::Item)
    {
        if (offer.index >= ITEM_GRANT_COUNT)
            return PickResult::Failed;

        ItemGrantInfo const& info = kItemGrants[offer.index];
        if (!(info.classMask & player->GetClassMask()))
            return PickResult::WrongClass;

        // One each: a broker copy still in the bags, or a genuinely owned one
        // (the legendaries are unique anyway).
        if (GetGrantedItem(const_cast<Player*>(player), offer.index) || player->HasItemCount(info.itemEntry, 1, true))
            return PickResult::AlreadyKnown;

        ItemPosCountVec dest;
        if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, info.itemEntry, 1) != EQUIP_ERR_OK)
            return PickResult::NoRoom;

        return PickResult::Ok;
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

uint32 GetOfferWeight(Offer const& offer)
{
    switch (offer.kind)
    {
        case Offer::Kind::Boon:       return offer.index < uint8(Boon::Max) ? kBoons[offer.index].weight : 0;
        case Offer::Kind::ClassSpell: return offer.index < CLASS_SPELL_COUNT ? CLASS_SPELL_WEIGHT : 0;
        case Offer::Kind::Item:       return offer.index < ITEM_GRANT_COUNT ? kItemGrants[offer.index].weight : 0;
    }
    return 0;
}

std::vector<Offer> RollOffers(std::vector<Player const*> const& roster, uint8 count)
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
    pool.reserve(uint8(Boon::Max) + CLASS_SPELL_COUNT + ITEM_GRANT_COUNT);

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
    for (uint8 i = 0; i < ITEM_GRANT_COUNT; ++i)
    {
        Offer offer{ Offer::Kind::Item, i };
        if (anyoneCan(offer))
            pool.push_back(offer);
    }

    // Weighted draw without replacement: each pick's odds are its weight over
    // the weight still in the pool.
    std::vector<Offer> picked;
    while (picked.size() < count && !pool.empty())
    {
        uint32 total = 0;
        for (Offer const& offer : pool)
            total += GetOfferWeight(offer);
        if (!total)
            break;

        uint32 roll = urand(1, total);
        size_t chosen = 0;
        for (size_t i = 0; i < pool.size(); ++i)
        {
            uint32 const w = GetOfferWeight(pool[i]);
            if (roll <= w)
            {
                chosen = i;
                break;
            }
            roll -= w;
        }

        picked.push_back(pool[chosen]);
        pool.erase(pool.begin() + chosen);
    }

    return picked;
}

PickResult Take(Player* player, Offer offer)
{
    PickResult const check = CanTake(player, offer);
    if (check != PickResult::Ok)
        return check;

    if (offer.kind == Offer::Kind::ClassSpell)
    {
        ClassSpellInfo const& info = kClassSpells[offer.index];
        if (!TeachClassSpell(player, info))
            return PickResult::Failed;

        // Remembered in the persistent marker so a relog into the still-running
        // run can teach it again (RestoreTaughtSpells).
        RecordTaught(player, info.spellId);
        return PickResult::Ok;
    }

    if (offer.kind == Offer::Kind::Item)
    {
        ItemGrantInfo const& info = kItemGrants[offer.index];

        ItemPosCountVec dest;
        if (player->CanStoreNewItem(NULL_BAG, NULL_SLOT, dest, info.itemEntry, 1) != EQUIP_ERR_OK)
            return PickResult::NoRoom;

        Item* item = player->StoreNewItem(dest, info.itemEntry, true, GenerateItemRandomPropertyId(info.itemEntry));
        if (!item)
        {
            TC_LOG_ERROR("bg.battleground", "VioletHoldBoons::Take: could not create item {} for {}.",
                info.itemEntry, player->GetGUID().ToString());
            return PickResult::Failed;
        }

        // Crusader, as a permanent enchant. It is applied when the weapon is
        // equipped, exactly like a scroll would be.
        item->SetEnchantment(PERM_ENCHANTMENT_SLOT, ENCHANT_CRUSADER, 0, 0, player->GetGUID());
        player->SendNewItem(item, 1, true, false);

        // The guid, not the entry: StripAll must never touch a Thunderfury the
        // character actually owns.
        RecordGranted(player, offer.index, item->GetGUID());
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

        player->GiveLevel(uint8(oldLevel + 1), /*borrowed*/ true);
        return PickResult::Ok;
    }

    uint32 const boonSpellId = GetBoonSpellFor(info.boon, player->GetClass());
    Aura* aura = player->GetAura(boonSpellId);
    if (!aura)
    {
        player->CastSpell(player, boonSpellId, true);
        aura = player->GetAura(boonSpellId);
        if (!aura)
        {
            TC_LOG_ERROR("bg.battleground", "VioletHoldBoons::Take: boon spell {} did not apply to {}.",
                boonSpellId, player->GetGUID().ToString());
            return PickResult::Failed;
        }

        // The cast made stack one; ModStackAmount clamps to the DBC cap.
        if (info.grantStacks > 1)
            aura->ModStackAmount(info.grantStacks - 1);
    }
    else
        aura->ModStackAmount(info.grantStacks);

    // Boons whose effect is not the aura itself.
    switch (info.boon)
    {
        case Boon::Greed:
            if (BattlegroundVHR* run = GetRunOf(player))
                run->AddBrokerBonusOffers(info.grantStacks);
            break;
        case Boon::Hoarder:
            if (BattlegroundVHR* run = GetRunOf(player))
                run->AddBonusBrokersPerWave(info.grantStacks);
            break;
        case Boon::Fellowship:
            if (BattlegroundVHR* run = GetRunOf(player))
                for (uint8 i = 0; i < info.grantStacks; ++i)
                    run->RequestAlly(player);
            break;
        case Boon::Beastmaster:
            RefreshBeastmaster(player);
            break;
        default:
            break;
    }

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
        case PickResult::NoRoom:       return "You have no room for that.";
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
                RollbackBorrowedLevels(player, uint8(baseLevel));
        }
    }

    // Whatever was spent in here that the levels above did not force out
    // was paid with points the character had anyway - it stays. Either way
    // the run's ledger is closed.
    player->ClearVioletHoldRunTalents();

    // Granted weapons go back, wherever they ended up (equipped, bags, bank).
    for (uint8 i = 0; i < ITEM_GRANT_COUNT; ++i)
        if (Item* item = GetGrantedItem(player, i))
            player->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);

    // Taught class spells: ONLY the chains the knowledge marker says the broker
    // taught (read before the marker goes), top rank first so RemoveSpell's
    // own "unlearn higher ranks" recursion never surprises us. The dependent
    // flag alone is NOT proof of a broker teach - the core marks every lower
    // rank of a genuinely trained chain dependent too, and RemoveSpell on one
    // of those would climb the chain and delete the real top rank. So a chain
    // is also left alone if any known rank in it is non-dependent (a real
    // rank owns it); RecordTaught only ever happens on a chain with no known
    // rank at all, so a recorded chain is entirely the broker's.
    MarkerSlots const taught = ReadKnowledge(player);
    for (uint32 tableSpellId : taught)
    {
        ClassSpellInfo const* info = FindClassSpell(tableSpellId);
        if (!info)
            continue;

        std::vector<uint32> ids = AllIdsOf(*info);

        bool ownsRealRank = false;
        for (uint32 id : ids)
        {
            PlayerSpellMap const& spells = player->GetSpellMap();
            auto itr = spells.find(id);
            if (itr != spells.end() && itr->second.state != PLAYERSPELL_REMOVED && !itr->second.dependent)
            {
                ownsRealRank = true;
                break;
            }
        }
        if (ownsRealRank)
        {
            TC_LOG_ERROR("bg.battleground", "VioletHoldBoons::StripAll: {} owns a real rank of taught class spell {}; leaving the chain alone.",
                player->GetGUID().ToString(), tableSpellId);
            continue;
        }

        for (auto itr = ids.rbegin(); itr != ids.rend(); ++itr)
            if (IsTaughtByBroker(player, *itr))
                player->RemoveSpell(*itr, false, false);
    }

    for (BoonInfo const& info : kBoons)
        player->RemoveAurasDueToSpell(info.spellId);
    for (uint32 id = SPELL_BOON_COOLDOWN_CLASS_FIRST; id <= SPELL_BOON_COOLDOWN_CLASS_LAST; ++id)
        player->RemoveAurasDueToSpell(id);
    player->RemoveAurasDueToSpell(SPELL_KNOWLEDGE_MARKER);
    player->RemoveAurasDueToSpell(SPELL_CACHE_MARKER);

    // The Beastmaster blessing rides on the pets, not the player.
    RefreshBeastmaster(player);
}

void RestoreTaughtSpells(Player* player)
{
    if (!player)
        return;

    for (uint32 tableSpellId : ReadKnowledge(player))
    {
        ClassSpellInfo const* info = FindClassSpell(tableSpellId);
        if (!info)
            continue;

        // The marker is per character, so a class mismatch means bad data.
        if (info->classId != player->GetClass())
            continue;

        // Already there (a relog while the run kept the spell somehow, or a
        // real rank picked up since) - leave it alone.
        if (KnowsAny(player, *info))
            continue;

        TeachClassSpell(player, *info);
    }
}

void RollbackBorrowedLevels(Player* player, uint8 baseLevel)
{
    if (!player || !baseLevel || baseLevel >= player->GetLevel())
        return;

    // Talents first, so the level drop below never finds points over budget.
    UndoRunTalentsFor(player, baseLevel);
    player->GiveLevel(baseLevel, /*borrowed*/ true);
}

uint8 GetBaseLevel(Player const* player)
{
    if (!player)
        return 0;

    if (Aura const* marker = player->GetAura(SPELL_BOON_LEVEL))
        if (AuraEffect const* eff = marker->GetEffect(EFFECT_0))
            if (eff->GetBaseAmount() > 0 && eff->GetBaseAmount() < int32(player->GetLevel()))
                return uint8(eff->GetBaseAmount());

    return player->GetLevel();
}

bool DestroyGrantedItemByGuidLow(Player* player, uint32 itemGuidLow)
{
    if (!player || !itemGuidLow)
        return false;

    Item* item = player->GetItemByGuid(ObjectGuid::Create<HighGuid::Item>(itemGuidLow));
    if (!item)
        return false;

    player->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
    return true;
}

void OnTalentLearnedInRun(Player* player, uint32 talentSpellId, uint32 previousRankSpellId)
{
    if (!player || !talentSpellId)
        return;
    player->RecordVioletHoldRunTalent(talentSpellId, previousRankSpellId);
}

void OnLogin(Player* player)
{
    if (!player)
        return;

    if (Battleground const* bg = player->GetBattleground())
    {
        if (bg->GetTypeID() == BATTLEGROUND_VHR)
        {
            // Back into the run: boons and levels came with the character;
            // the taught spells did not (dependent, never saved) - put them
            // back from the marker.
            RestoreTaughtSpells(player);

            // Boon of the Outrider is a spellmod now (was a dummy). A copy
            // saved by an older build carries the dummy's amount (2999) into
            // the spellmod slot, which would ADD three seconds. Re-cast so the
            // amounts come from the current DBC. Unique, so nothing is lost.
            if (player->HasAura(SPELL_BOON_MOUNT_SPEED))
            {
                player->RemoveAurasDueToSpell(SPELL_BOON_MOUNT_SPEED);
                player->CastSpell(player, SPELL_BOON_MOUNT_SPEED, true);
            }
            return;
        }
    }

    bool carrying = player->HasAura(SPELL_KNOWLEDGE_MARKER) || player->HasAura(SPELL_CACHE_MARKER)
        || player->HasAura(GetBoonSpellFor(Boon::Cooldown, player->GetClass()));
    for (BoonInfo const& info : kBoons)
        if (carrying || player->HasAura(info.spellId))
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

// Why `viewer` cannot take this offer, as a short suffix for the gossip line.
// The offers are SHARED by everyone at a broker, so a line that is useless to
// the reader (a warrior looking at a mage spell, or at a unique boon he has
// already taken) still has to be shown for whoever it does fit - it just says
// so rather than letting the player find out by clicking.
static char const* OfferBlockedSuffix(Player const* viewer, Offer offer)
{
    switch (CanTake(viewer, offer))
    {
        case PickResult::Ok:           return "";
        case PickResult::WrongClass:   return " - not for you";
        case PickResult::AlreadyKnown: return " - already yours";
        case PickResult::AtCap:        return " - already at your cap";
        case PickResult::LevelCeiling: return " - you can go no higher";
        case PickResult::NoRoom:       return " - your bags are full";
        default:                       return "";
    }
}

std::string DescribeOffer(Player const* viewer, Offer offer)
{
    char const* blocked = OfferBlockedSuffix(viewer, offer);

    if (offer.kind == Offer::Kind::Item)
    {
        if (offer.index >= ITEM_GRANT_COUNT)
            return "?";
        ItemGrantInfo const& info = kItemGrants[offer.index];
        char const* name = info.name;
        if (ItemTemplate const* proto = sObjectMgr->GetItemTemplate(info.itemEntry))
            name = proto->Name1.c_str();
        return Trinity::StringFormat("Take up {} (with Crusader){}", name, blocked);
    }

    if (offer.kind == Offer::Kind::ClassSpell)
    {
        if (offer.index >= CLASS_SPELL_COUNT)
            return "?";
        ClassSpellInfo const& info = kClassSpells[offer.index];
        char const* className = "?";
        if (ChrClassesEntry const* entry = sChrClassesStore.LookupEntry(info.classId))
            className = entry->Name[LOCALE_enUS];
        return Trinity::StringFormat("Learn {} ({}){}", info.name, className, blocked);
    }

    if (offer.index >= uint8(Boon::Max))
        return "?";

    BoonInfo const& info = kBoons[offer.index];
    uint8 const held = GetStacks(viewer, info.boon);

    switch (info.boon)
    {
        case Boon::MountSpeed:
        case Boon::Phoenix:
        case Boon::Hoarder:
        case Boon::Overkill:
            return Trinity::StringFormat("{}: {}{}", info.name, info.summary, blocked);
        case Boon::Level:
            return Trinity::StringFormat("{}: +1 {}{}", info.name, info.summary, blocked);
        case Boon::Greed:
        case Boon::Fellowship:
        case Boon::Menagerie:
            return Trinity::StringFormat("{}: {} [{}/{}]{}", info.name, info.summary, uint32(held), uint32(info.maxStacks), blocked);
        default:
            return Trinity::StringFormat("{}: +{}{} {} [{}/{}{}]{}", info.name, uint32(info.grantStacks), info.unit,
                info.summary, uint32(held), uint32(info.maxStacks), info.unit, blocked);
    }
}

int32 GetCcDurationReductionPct(Unit const* target, uint32 mechanicMask)
{
    if (!(mechanicMask & CC_MECHANIC_MASK))
        return 0;
    return AmountOf(target, SPELL_BOON_CC_DURATION, 90);
}

int32 GetMountCastTimeReductionMs(Unit const* caster)
{
    return AmountOf(caster, SPELL_BOON_MOUNT_SPEED, 3000);
}

bool AllowsMountingInCombat(Unit const* caster)
{
    return caster && caster->HasAura(SPELL_BOON_MOUNT_SPEED);
}

int32 GetRageGenerationPct(Unit const* unit)
{
    return AmountOf(unit, SPELL_BOON_RAGE_GEN, 100);
}

// --- second batch ------------------------------------------------------------

namespace
{
// Living hostile units within `radius` of `around` that `attacker` may hit,
// excluding `around` itself. Behind-the-gate wave clones are NON_ATTACKABLE
// during preparation and fall out through IsValidAttackTarget.
std::vector<Unit*> HostilesAround(Unit* around, Unit* attacker, float radius)
{
    std::vector<Unit*> out;
    if (!around || !attacker || !around->IsInWorld())
        return out;

    std::list<Unit*> found;
    Trinity::AnyUnfriendlyUnitInObjectRangeCheck check(around, attacker, radius);
    Trinity::UnitListSearcher<Trinity::AnyUnfriendlyUnitInObjectRangeCheck> searcher(around, found, check);
    Cell::VisitAllObjects(around, searcher, radius);

    for (Unit* unit : found)
        if (unit != around && unit->IsAlive() && attacker->IsValidAttackTarget(unit))
            out.push_back(unit);
    return out;
}

// The player who owns a boon-relevant unit: the player itself, or a pet's /
// guardian's owner (Overkill and Beastmaster count pet kills and pet stacks).
Player* BoonOwnerOf(Unit* unit)
{
    return unit ? unit->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
}

void ApplyBeastmasterBlessing(Player* owner, Unit* minion, uint8 stacks)
{
    if (!minion)
        return;

    // Dead pets are handled too: the blessing is death-persistent (so a pet
    // that dies keeps it for Revive Pet) and CAN_TARGET_DEAD (so a pet that
    // is dead at pick time gets it, and one that is dead at strip time loses
    // it). It is never saved to pet_aura (SPELL_ATTR0_CU_AURA_CANNOT_BE_SAVED,
    // pinned in SpellMgr::LoadSpellInfoCustomAttributes), so it cannot follow
    // a pet out of the Hold through the database.
    // "Pets and guardians": totems are neither, and their health is set by
    // the summon spell rather than by unit mods anyway.
    if (minion->IsTotem())
        return;

    bool changed = false;
    if (!stacks)
    {
        changed = minion->HasAura(SPELL_BEASTMASTER_BLESSING);
        if (changed)
            minion->RemoveAurasDueToSpell(SPELL_BEASTMASTER_BLESSING);
    }
    else
    {
        // AddAura rather than CastSpell: SetMinion runs before the summon is
        // added to its map, and an aura can be created on a not-yet-in-world
        // unit while a cast cannot target one. The stack count is the owner's,
        // so the pet's tooltip and numbers say the same thing the owner's boon
        // does.
        Aura* aura = minion->GetAura(SPELL_BEASTMASTER_BLESSING);
        if (!aura)
        {
            aura = owner->AddAura(SPELL_BEASTMASTER_BLESSING, minion);
            changed = aura != nullptr;
        }
        if (aura && aura->GetStackAmount() != stacks)
        {
            aura->SetStackAmount(stacks);
            changed = true;
        }
    }

    // Guardians (treants, elementals, mirror images, the Menagerie) and pets
    // that were not loaded from the database never get CanModifyStats, so a
    // percent aura changing on one that is already out only stores the
    // modifier - recompute so max health and melee damage actually move.
    // ONLY when the blessing itself moved: this also runs from SetMinion for
    // every pet on the realm whose owner holds no stacks, and must be a no-op
    // there. Fresh summons are covered by InitStatsForLevel.
    if (changed && minion->IsInWorld() && !minion->CanModifyStats())
        minion->UpdateAllStats();
}

struct MenagerieEntry
{
    uint32 entry;
    char const* name;
};

// Every one of these has a Guardian::InitStatsForLevel special case that
// scales it from the owner's attack/spell power, so a level-60 guardian hits
// like a level-60 guardian whoever summoned it. Water elementals and warlock
// demons are missing on purpose: they scale through pet_levelstats as PETS
// and would come out as bare creatures here.
MenagerieEntry const kMenagerie[] =
{
    { 1964,  "Treant" },
    { 29264, "Spirit Wolf" },
    { 19668, "Shadowfiend" },
    { 15352, "Greater Earth Elemental" },
    { 15438, "Greater Fire Elemental" },
    { 31216, "Mirror Image" }
};

// SummonProperties 61: Control ALLY, Title GUARDIAN. A plain guardian, so it
// never competes with the owner's real pet for the pet slot (SetMinion
// dismisses a different-entry "guardian pet", which the pet-control
// properties the class spells use would make every one of these).
constexpr uint32 MENAGERIE_SUMMON_PROPERTIES = 61;
}

bool TryPhoenixRescue(Unit* victim)
{
    Player* player = victim ? victim->ToPlayer() : nullptr;
    if (!player || !player->IsAlive() || !player->HasAura(SPELL_BOON_PHOENIX))
        return false;

    // Spent now, before the damage lands, so nothing between here and
    // CompletePhoenixRescue can spend it twice.
    player->RemoveAurasDueToSpell(SPELL_BOON_PHOENIX);
    return true;
}

void CompletePhoenixRescue(Unit* victim)
{
    Player* player = victim ? victim->ToPlayer() : nullptr;
    if (!player || !player->IsAlive())
        return;

    player->SetHealth(std::max<uint32>(1, player->CountPctFromMaxHealth(PHOENIX_RESTORE_PCT)));
    if (player->GetMaxPower(POWER_MANA))
        player->SetPower(POWER_MANA, player->CountPctFromMaxPower(POWER_MANA, PHOENIX_RESTORE_PCT));

    player->CastSpell(player, SPELL_PHOENIX_VISUAL, TRIGGERED_FULL_MASK);
    if (WorldSession* session = player->GetSession())
        session->SendNotification("The phoenix rises!");
}

void OnKillingBlow(Unit* attacker, Unit* victim, uint32 overkill, SpellInfo const* spellProto)
{
    // Damage landing on a corpse (a second damage shield after the first one
    // killed the attacker) is not a killing blow, and neither is dying to
    // your own fall damage or Hellfire.
    if (!attacker || !victim || !overkill || attacker == victim || !victim->IsAlive())
        return;

    // Splash never splashes: a chain of overkills would otherwise recurse
    // through DealDamage until nothing in the room was left standing.
    if (spellProto && (spellProto->Id == SPELL_OVERKILL_DAMAGE || spellProto->Id == SPELL_RICOCHET_DAMAGE))
        return;

    Player* owner = BoonOwnerOf(attacker);
    if (!owner)
        return;

    if (!GetStacks(owner, Boon::Overkill))
        return;

    // Unique boon: the entire excess, on everyone in range.
    uint32 const splash = overkill;

    // Collected before any of it is dealt: a splash that kills may reshape the
    // neighbourhood mid-loop otherwise.
    std::vector<Unit*> targets = HostilesAround(victim, owner, OVERKILL_SPLASH_RADIUS);
    for (Unit* target : targets)
    {
        CastSpellExtraArgs args(TRIGGERED_FULL_MASK);
        args.AddSpellBP0(int32(std::min<uint32>(splash, uint32(INT32_MAX))));
        owner->CastSpell(target, SPELL_OVERKILL_DAMAGE, args);
    }
}

void OnMinionAdded(Unit* owner, Unit* minion)
{
    Player* player = owner ? owner->ToPlayer() : nullptr;
    if (!player || !minion)
        return;

    // Unconditional: with no stacks this scrubs a stale blessing off a pet
    // that somehow arrived carrying one.
    ApplyBeastmasterBlessing(player, minion, GetStacks(player, Boon::Beastmaster));
}

void RefreshBeastmaster(Player* player)
{
    if (!player)
        return;

    uint8 const stacks = GetStacks(player, Boon::Beastmaster);
    // Copy first: SetStackAmount / RemoveAuras may not touch the list, but a
    // dying totem in the middle of an aura update could. Only OWNED minions -
    // m_Controlled also holds charmed units (a mind-controlled wave clone),
    // which SetMinion never blessed and which must not walk away with it.
    std::vector<Unit*> controlled(player->m_Controlled.begin(), player->m_Controlled.end());
    for (Unit* unit : controlled)
        if (unit->GetTypeId() == TYPEID_UNIT && unit->GetOwnerGUID() == player->GetGUID())
            ApplyBeastmasterBlessing(player, unit, stacks);
}

void SummonMenagerie(Player* owner, uint8 count, std::vector<ObjectGuid>& out)
{
    if (!owner || !count || !owner->IsInWorld() || !owner->IsAlive())
        return;

    Map* map = owner->GetMap();
    SummonPropertiesEntry const* properties = sSummonPropertiesStore.LookupEntry(MENAGERIE_SUMMON_PROPERTIES);
    if (!map || !properties)
        return;

    for (uint8 i = 0; i < count; ++i)
    {
        MenagerieEntry const& pick = kMenagerie[urand(0, uint32(std::size(kMenagerie)) - 1)];

        Position pos = owner->GetNearPosition(3.0f, frand(0.0f, 2.0f * float(M_PI)));
        // Duration 0: they live until the battleground takes them down (or the
        // owner leaves the map, which unsummons everything it controls).
        TempSummon* summon = map->SummonCreature(pick.entry, pos, properties, 0, owner);
        if (!summon)
        {
            TC_LOG_ERROR("bg.battleground", "VioletHoldBoons::SummonMenagerie: could not summon {} ({}) for {}.",
                pick.name, pick.entry, owner->GetGUID().ToString());
            continue;
        }

        // A Title-GUARDIAN summon has no CharmInfo, and PetAI (and the
        // PetAI-derived pet scripts) bail out without one. Give it the charm
        // info and re-pick the AI: scripted entries keep their script, the
        // rest get PetAI so they follow the owner and take the owner's fights.
        summon->InitCharmInfo();
        if (summon->GetScriptId())
            summon->AIM_Initialize();
        else
            summon->AIM_Initialize(new PetAI(summon));
        summon->SetReactState(REACT_AGGRESSIVE);

        out.push_back(summon->GetGUID());
    }
}
}
