/*
 * Violet Hold boons
 *
 * A wave-clear reward in the Violet Hold survival battleground can be an
 * ethereal broker instead of a floor rune. He rolls THREE offers when first
 * spoken to and shows the same three to everyone in the party; the first
 * player to take one consumes him. Offers come from two pools:
 *
 *  - Boons: permanent self-auras whose base points are ONE unit per stack
 *    (1%, 1 resistance, 1 level), whose Spell.dbc StackAmount is the
 *    advertised cap, and which a pick advances by `grantStacks` - so "+8%
 *    speed, stacks to 255%" is 8 stacks of a 255-stack, 1%-per-stack aura.
 *    The engine multiplies base points by the stack count in
 *    AuraEffect::CalculateAmount, which is what keeps the family data-driven.
 *
 *  - Class spells: three per class (Killing Spree, Lava Burst, ...) taught
 *    for the run only. The rank granted is the highest one a character of
 *    that level could have trained; where no rank existed at 60 a level-60
 *    version was made (90260-90262). They are learned as DEPENDENT spells,
 *    which Player::_SaveSpells never writes, so a logout drops them by
 *    itself; StripAll() unlearns them for every other exit.
 *
 * The pool is filtered by the run's roster, not by whoever is clicking: an
 * offer only rolls while someone on the human team could still use it (a
 * class spell needs a member of that class who does not already know it, a
 * boon needs a matching member below the cap). Whoever clicks an offer meant
 * for another class gets an error and the broker stays put for the right
 * person.
 *
 * Boons only exist inside the Violet Hold. They are stripped by StripAll()
 * when the player leaves the battleground for ANY reason (BattlegroundVHR::
 * RemovePlayer covers the leave button, the match ending, GM teleports and
 * AFK kicks), and by OnLogin() for the one path that has no Player object
 * to strip - logging out inside and being timed out of the run while away.
 *
 * The level boon is the awkward one. The extra levels are real
 * (Player::GiveLevel), so the marker aura 90247 records the level the
 * character had before the first pick in its effect base amount, and
 * StripAll() puts them back on it. That base amount is persisted with the
 * aura in character_aura, which is why a relog cannot lose it.
 */

#ifndef TRINITYCORE_VIOLET_HOLD_BOONS_H
#define TRINITYCORE_VIOLET_HOLD_BOONS_H

#include "Define.h"
#include "SharedDefines.h"
#include <string>
#include <vector>

class Player;
class SpellInfo;
class Unit;

namespace VioletHoldBoons
{
    enum class Boon : uint8
    {
        Speed = 0,
        Damage,
        DamageTaken,
        CcDuration,
        ManaCost,
        RageCost,
        EnergyCost,
        Stamina,        // was Max Health; +% Stamina covers it and shows on the sheet
        ExtraAttack,
        Doublecast,
        Crit,
        SpellCrit,
        AttackSpeed,
        CastSpeed,
        MountSpeed,
        Cooldown,
        Resistance,
        Level,
        Strength,
        Agility,
        Intellect,
        Spirit,
        EnergyRegen,
        RageGeneration,

        // Second batch (2026-08-16).
        Ricochet,       // damage has a chance to leap to a second enemy for half
        Overkill,       // excess killing-blow damage splashes nearby enemies
        Reflection,     // % of melee damage taken thrown back
        Vampire,        // % of damage dealt returned as health
        Phoenix,        // survive one killing blow (unique per pick)
        Greed,          // RUN-WIDE: brokers show an extra option
        Hoarder,        // RUN-WIDE, unique: an extra broker every wave
        Beastmaster,    // pets & guardians +% damage and health
        Menagerie,      // random guardians summoned as each wave opens
        Fellowship,     // RUN-WIDE: a random playerbot joins the party for the run

        Max
    };

    // Spell ids. Contiguous on purpose so IsBoonSpell is a range check.
    enum Spells : uint32
    {
        SPELL_BOON_FIRST        = 90230,
        SPELL_BOON_SPEED        = 90230,
        SPELL_BOON_DAMAGE       = 90231,
        SPELL_BOON_DAMAGE_TAKEN = 90232,
        SPELL_BOON_CC_DURATION  = 90233,
        SPELL_BOON_MANA_COST    = 90234,
        SPELL_BOON_RAGE_COST    = 90235,
        SPELL_BOON_ENERGY_COST  = 90236,
        SPELL_BOON_STAMINA      = 90237,   // reused: was Max Health, now +% Stamina
        SPELL_BOON_EXTRA_ATTACK = 90238,
        SPELL_BOON_DOUBLECAST   = 90239,
        SPELL_BOON_CRIT         = 90240,
        SPELL_BOON_SPELL_CRIT   = 90241,
        SPELL_BOON_ATTACK_SPEED = 90242,
        SPELL_BOON_CAST_SPEED   = 90243,
        SPELL_BOON_MOUNT_SPEED  = 90244,
        SPELL_BOON_COOLDOWN     = 90245,
        SPELL_BOON_RESISTANCE   = 90246,
        SPELL_BOON_LEVEL        = 90247,
        SPELL_BOON_LAST         = 90247,

        // Later additions, past the marker/swing ids.
        SPELL_BOON_STRENGTH     = 90251,
        SPELL_BOON_AGILITY      = 90252,
        SPELL_BOON_INTELLECT    = 90253,
        SPELL_BOON_SPIRIT       = 90254,
        SPELL_BOON_ENERGY_REGEN = 90255,   // MOD_POWER_REGEN_PERCENT on energy
        SPELL_BOON_RAGE_GEN     = 90256,   // dummy read by Unit::RewardRage
        SPELL_BOON_EXTRA_FIRST  = 90251,
        SPELL_BOON_EXTRA_LAST   = 90256,

        // Boon of Alacrity is a real SPELLMOD_COOLDOWN pct spellmod so the
        // CLIENT computes the shortened cooldowns too (its own timers and its
        // "not ready" pre-check). Spellmods work by spell family, so there is
        // one variant per class: id = SPELL_BOON_COOLDOWN_CLASS_BASE + ChrClasses
        // id (90271 warrior ... 90281 druid; 90276/90280 unused). 90245 is the
        // old dummy, kept only so StripAll can clean it off old characters.
        SPELL_BOON_COOLDOWN_CLASS_BASE  = 90270,
        SPELL_BOON_COOLDOWN_CLASS_FIRST = 90271,
        SPELL_BOON_COOLDOWN_CLASS_LAST  = 90281,

        // Second batch. 90283-90286 are the spells the procs and hooks FIRE
        // (hostile single-target holy damage that ignores hit/crit/done-bonus
        // and cannot itself proc anything, plus one heal) - deliberately not
        // boon auras and not pinned positive. 90287 is what Beastmaster puts on
        // the pets. 90288-90297 are the boon auras (dummies; the work happens
        // in scripts/Custom/violet_hold_boons.cpp procs, Unit::DealDamage,
        // Unit::SetMinion and BattlegroundVHR).
        SPELL_RICOCHET_DAMAGE      = 90283,
        SPELL_OVERKILL_DAMAGE      = 90284,
        SPELL_REFLECTION_DAMAGE    = 90285,
        SPELL_VAMPIRE_HEAL         = 90286,
        SPELL_BEASTMASTER_BLESSING = 90287,
        SPELL_BOON_RICOCHET     = 90288,
        SPELL_BOON_OVERKILL     = 90289,
        SPELL_BOON_REFLECTION   = 90290,
        SPELL_BOON_VAMPIRE      = 90291,
        SPELL_BOON_PHOENIX      = 90292,
        SPELL_BOON_GREED        = 90293,
        SPELL_BOON_HOARDER      = 90294,
        SPELL_BOON_BEASTMASTER  = 90295,
        SPELL_BOON_MENAGERIE    = 90296,
        SPELL_BOON_FELLOWSHIP   = 90297,
        SPELL_BOON_BATCH2_FIRST = 90287,
        SPELL_BOON_BATCH2_LAST  = 90297,

        // Stock "Resurrection Impact Visual", played on a Phoenix rescue.
        SPELL_PHOENIX_VISUAL    = 24171,

        // Hidden marker whose three effect base amounts hold the class spells
        // (table spellId, first rank) the broker taught this character - a
        // class has exactly three, so three slots is the whole answer. Persists
        // in character_aura, which is how a relog INTO a live run re-teaches
        // them (dependent spells themselves are never saved).
        SPELL_KNOWLEDGE_MARKER = 90248,

        // Same idea for the legendary weapons the broker hands out: effect base
        // amounts hold the low guids of the granted items, one slot per entry
        // in the item table, so StripAll destroys exactly those items and never
        // a copy the character genuinely owns.
        SPELL_CACHE_MARKER = 90257,

        // The extra swing granted by SPELL_BOON_EXTRA_ATTACK's proc.
        SPELL_BOON_EXTRA_ATTACK_SWING = 90250,

        // Level-60 editions of class spells that only exist at 64+ in the
        // stock data. See sql/custom/dbc/2026_08_15_01_dbc_violet_hold_class_spells.sql.
        SPELL_L60_SPELL_REFLECTION = 90260,   // 23920 was repurposed as the passive "Shield Reflection" here
        SPELL_L60_LAVA_BURST       = 90261,
        SPELL_L60_NOURISH          = 90262
    };

    struct BoonInfo
    {
        Boon boon;
        uint32 spellId;
        uint8 grantStacks;      // stacks one pick adds
        uint8 maxStacks;        // must equal the spell's StackAmount
        // Classes the boon is offered to, as a ChrClasses bitmask (1 << (class-1)).
        // A rage discount is noise to a mage; the roll skips it instead.
        uint32 classMask;
        // Relative roll weight in the broker's pool (see RollOffers). Class
        // spells use CLASS_SPELL_WEIGHT, items their own; a plain useful boon
        // sits around 8-10, the run-defining ones lower.
        uint8 weight;
        char const* name;
        char const* summary;    // one line for the gossip menu, without numbers
        char const* unit;       // "%", " resistance", " level" - what one stack is
    };

    // A class spell the broker can teach for the run. `spellId` is the first
    // rank of the chain (or the standalone spell); the rank actually taught is
    // picked per player level by ResolveRank. `companion` is a second spell
    // taught alongside (Demonic Circle's teleport). `alias` is a stock spell
    // that counts as "already knowing this" (the level-80 Nourish for our
    // level-60 edition), including every rank of its chain.
    struct ClassSpellInfo
    {
        uint8 classId;
        uint32 spellId;
        uint32 companion;
        uint32 alias;
        char const* name;
    };

    constexpr uint8 CLASS_SPELL_COUNT = 27;

    // A weapon the broker can hand out for the run (a real item, destroyed on
    // the way out - its guid is kept in SPELL_CACHE_MARKER's slot `index`).
    struct ItemGrantInfo
    {
        uint32 itemEntry;
        uint32 classMask;   // who it is offered to (who could wield it)
        uint8 weight;
        char const* name;
    };

    constexpr uint8 ITEM_GRANT_COUNT = 2;
    // Every class spell rolls at this weight. Three per present class add up
    // fast, so it sits at the bottom of the scale (with the legendaries):
    // a full party's spells make up well under a fifth of the pool.
    constexpr uint8 CLASS_SPELL_WEIGHT = 2;

    // What the broker actually shows: a boon, a class spell or an item.
    struct Offer
    {
        enum class Kind : uint8 { Boon, ClassSpell, Item };

        Kind kind = Kind::Boon;
        uint8 index = 0;   // enum Boon value, or index into the class-spell / item table

        // Packed for gossip actions and back. Codes are < 300.
        uint32 Encode() const { return kind == Kind::Boon ? index : kind == Kind::ClassSpell ? 100 + index : 200 + index; }
        static bool Decode(uint32 code, Offer& out);

        bool operator==(Offer const& other) const { return kind == other.kind && index == other.index; }
    };

    enum class PickResult : uint8
    {
        Ok,
        WrongClass,     // a class spell / item / class-gated boon for a class the picker is not
        AlreadyKnown,   // the picker already knows some rank of the spell, or already has the item
        AtCap,          // the boon is at its stack cap for the picker
        LevelCeiling,   // the level boon at LEVEL_BOON_CEILING
        NoRoom,         // the item does not fit in the picker's bags
        Failed          // the grant itself did not take
    };

    constexpr uint32 NPC_BOON_BROKER = 900117;
    constexpr uint32 NPC_TEXT_BOON_BROKER = 900117;
    constexpr uint8 OFFERS_PER_BROKER = 3;
    // The level boon is refused past this. Stats already stop scaling at the
    // realm cap; this only keeps the client-side level display sane.
    constexpr uint8 LEVEL_BOON_CEILING = 80;

    TC_GAME_API BoonInfo const& GetBoon(Boon boon);
    TC_GAME_API ClassSpellInfo const& GetClassSpell(uint8 index);
    TC_GAME_API ItemGrantInfo const& GetItemGrant(uint8 index);
    TC_GAME_API BoonInfo const* FindBySpell(uint32 spellId);
    TC_GAME_API bool IsBoonSpell(uint32 spellId);

    // The aura a boon puts on THIS player - the same for everyone except the
    // per-class spellmod variants (Alacrity).
    TC_GAME_API uint32 GetBoonSpellFor(Boon boon, uint8 classId);

    // Stacks the player currently holds of a boon (0 if none).
    TC_GAME_API uint8 GetStacks(Player const* player, Boon boon);

    // Would granting this to THIS player work right now? Ok, or the reason not.
    TC_GAME_API PickResult CanTake(Player const* player, Offer offer);

    // Highest rank of `firstRankSpellId`'s chain trainable at `level`
    // (SpellLevel in 1..level); the first rank when nothing qualifies, which
    // is what makes a level-64+ utility spell teachable at 60 unchanged.
    TC_GAME_API uint32 ResolveRank(uint32 firstRankSpellId, uint8 level);

    // `count` distinct offers (OFFERS_PER_BROKER plus the run's Greed bonus)
    // that at least one member of `roster` (the run's human team) could take.
    // Fewer if the pool has run dry.
    TC_GAME_API std::vector<Offer> RollOffers(std::vector<Player const*> const& roster, uint8 count = OFFERS_PER_BROKER);

    // Apply one pick. Ok on success; otherwise nothing changed.
    TC_GAME_API PickResult Take(Player* player, Offer offer);

    // Player-facing text for a non-Ok result.
    TC_GAME_API char const* GetPickResultText(PickResult result);

    // Remove every boon, undo the level boon and unlearn every class spell the
    // broker taught. Idempotent.
    TC_GAME_API void StripAll(Player* player);

    // Login-time sweep: a character carrying boons who is not being placed
    // back into a Violet Hold run loses them here; one who IS placed back
    // gets the taught class spells re-learned from the knowledge marker.
    TC_GAME_API void OnLogin(Player* player);

    // Re-teach whatever SPELL_KNOWLEDGE_MARKER says the broker taught (the
    // dependent spells themselves do not survive a logout).
    TC_GAME_API void RestoreTaughtSpells(Player* player);

    // Player::LearnTalent reports every talent rank spent inside a Violet Hold
    // run here; StripAll undoes them newest-first, only as far as the lost
    // levels' points require.
    TC_GAME_API void OnTalentLearnedInRun(Player* player, uint32 talentSpellId, uint32 previousRankSpellId);

    // Drop the character back to `baseLevel`, first undoing just enough of the
    // run-spent talents (newest first) that InitTalentForLevel never has to
    // fall back to its full reset. Used by StripAll and the marker's script.
    TC_GAME_API void RollbackBorrowedLevels(Player* player, uint8 baseLevel);

    // The level the character REALLY has: the Ascension marker's recorded
    // pre-boon level while boosted, GetLevel() otherwise.
    TC_GAME_API uint8 GetBaseLevel(Player const* player);

    // Destroy one broker-granted weapon by its item low guid (cache marker
    // slot content). False if the character no longer has it.
    TC_GAME_API bool DestroyGrantedItemByGuidLow(Player* player, uint32 itemGuidLow);

    // Gossip line for `viewer`: "Boon of Might: +3% damage done [6/255%]" or
    // "Lava Burst (Shaman)".
    TC_GAME_API std::string DescribeOffer(Player const* viewer, Offer offer);

    // --- hooks consulted by the core -----------------------------------------
    // All return 0 when the unit holds no such boon, so callers stay cheap.

    // Percent taken off the duration of a hostile aura with a crowd-control
    // mechanic (WorldObject::ModSpellDuration).
    TC_GAME_API int32 GetCcDurationReductionPct(Unit const* target, uint32 mechanicMask);
    // Milliseconds taken off a mount summon (WorldObject::ModSpellCastTime).
    TC_GAME_API int32 GetMountCastTimeReductionMs(Unit const* caster);
    // Boon of the Outrider also lets mounts be summoned in combat (Spell::CheckCast).
    TC_GAME_API bool AllowsMountingInCombat(Unit const* caster);
    // Percent added to rage gained from dealing and taking damage (Unit::RewardRage).
    TC_GAME_API int32 GetRageGenerationPct(Unit const* unit);

    // --- second-batch hooks -------------------------------------------------
    // Ricochet, Reflection and Vampire are aura procs (scripts/Custom). These
    // are the ones the core itself has to call.

    // Unit::DealDamage, when `damage` would kill `victim`: a Phoenix holder
    // spends the boon and the caller clamps the blow to leave 1 health, then
    // calls CompletePhoenixRescue once the damage has been applied. True when
    // the boon was spent.
    TC_GAME_API bool TryPhoenixRescue(Unit* victim);
    TC_GAME_API void CompletePhoenixRescue(Unit* victim);
    constexpr uint8 PHOENIX_RESTORE_PCT = 30;

    // Unit::DealDamage, on a killing blow: Overkill splashes `overkill` (the
    // damage past the victim's health) onto enemies near the victim.
    TC_GAME_API void OnKillingBlow(Unit* attacker, Unit* victim, uint32 overkill, SpellInfo const* spellProto);
    constexpr float OVERKILL_SPLASH_RADIUS = 8.0f;
    constexpr float RICOCHET_RADIUS = 10.0f;

    // Unit::SetMinion(apply): a Beastmaster owner's blessing goes on the new
    // pet/guardian, at the owner's stack count.
    TC_GAME_API void OnMinionAdded(Unit* owner, Unit* minion);
    // Re-sync the blessing on everything the player controls (after a pick).
    TC_GAME_API void RefreshBeastmaster(Player* player);

    // Menagerie: `count` random guardians appear beside `owner`, fight with
    // PetAI at the owner's level, and are appended to `out` for the
    // battleground to take down when the wave is over.
    TC_GAME_API void SummonMenagerie(Player* owner, uint8 count, std::vector<ObjectGuid>& out);
}

#endif // TRINITYCORE_VIOLET_HOLD_BOONS_H
