/*
 * Violet Hold class boons (artifact tier) - see VioletHoldClassBoons.h.
 */

#include "VioletHoldClassBoons.h"

#include "Battleground.h"
#include "BattlegroundVHR.h"
#include "CellImpl.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GameTime.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Group.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellHistory.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Totem.h"
#include "TotemPackets.h"
#include "TemporarySummon.h"
#include "WorldSession.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <list>

namespace VioletHoldClassBoons
{
namespace
{
constexpr float WINDLORD_PROC_CHANCE = 50.0f;

bool Holds(Unit const* unit, uint32 boonSpellId)
{
    return unit && unit->HasAura(boonSpellId);
}

bool Holds(WorldObject const* object, uint32 boonSpellId)
{
    return object && Holds(object->ToUnit(), boonSpellId);
}

// The Violet Hold run a party member (not a clone, not a Fellowship
// champion) is fighting in - what keeps the once-per-wave counts.
BattlegroundVHR* PartyRunOf(Player const* player)
{
    Battleground* bg = player ? player->GetBattleground() : nullptr;
    if (!bg || bg->GetTypeID() != BATTLEGROUND_VHR)
        return nullptr;
    BattlegroundVHR* run = static_cast<BattlegroundVHR*>(bg);
    if (player->GetBGTeam() != run->GetHumanTeam() || run->IsAlly(player->GetGUID()))
        return nullptr;
    return run;
}

// Class spells are matched by family and enUS name, not by id: every rank of a
// chain shares both, and this realm's spell_ranks rows are not to be trusted
// (some were removed on purpose). Only a holder's own casts are ever asked about.
bool NameIs(SpellInfo const* spellInfo, char const* name)
{
    char const* own = spellInfo ? spellInfo->SpellName[LOCALE_enUS] : nullptr;
    return own && std::strcmp(own, name) == 0;
}

bool Is(SpellInfo const* spellInfo, SpellFamilyNames family, char const* name)
{
    return spellInfo && spellInfo->SpellFamilyName == uint32(family) && NameIs(spellInfo, name);
}

// Same spell, any rank.
bool SameSpell(SpellInfo const* a, SpellInfo const* b)
{
    if (!a || !b)
        return false;
    if (a == b)
        return true;
    char const* an = a->SpellName[LOCALE_enUS];
    char const* bn = b->SpellName[LOCALE_enUS];
    return a->SpellFamilyName == b->SpellFamilyName && an && bn && std::strcmp(an, bn) == 0;
}

// Enemies of `attacker` within `radius` of `around` (not `around` itself),
// alive and attackable.
std::vector<Unit*> EnemiesNear(WorldObject* around, Unit* attacker, float radius)
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

// The holder's side: in a battleground everyone on their team there (in the
// Violet Hold that is the party, Fellowship champions included - or the wave,
// for a clone), elsewhere their group on the same map. Always the holder too.
std::vector<Player*> AlliesOf(Unit* unit)
{
    std::vector<Player*> out;
    Player* player = unit ? unit->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
    if (!player || !player->IsInWorld())
        return out;

    if (Battleground* bg = player->GetBattleground())
    {
        uint32 const team = player->GetBGTeam();
        for (auto const& [guid, bgPlayer] : bg->GetPlayers())
        {
            if (bgPlayer.Team != team)
                continue;
            if (Player* member = ObjectAccessor::GetPlayer(*player, guid))
                if (member->IsInWorld() && member->GetMap() == player->GetMap())
                    out.push_back(member);
        }
    }
    else if (Group* group = player->GetGroup())
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource())
                if (member->IsInWorld() && member->GetMap() == player->GetMap())
                    out.push_back(member);
    }

    if (std::find(out.begin(), out.end(), player) == out.end())
        out.push_back(player);
    return out;
}

bool HasDamageOverTime(SpellInfo const* spellInfo)
{
    for (SpellEffectInfo const& effect : spellInfo->GetEffects())
    {
        switch (effect.ApplyAuraName)
        {
            case SPELL_AURA_PERIODIC_DAMAGE:
            case SPELL_AURA_PERIODIC_DAMAGE_PERCENT:
            case SPELL_AURA_PERIODIC_LEECH:
            case SPELL_AURA_PERIODIC_MANA_LEECH:
                return true;
            default:
                break;
        }
    }
    return false;
}

// Contagion: one DoT waiting to jump once the dying unit's list is walked.
struct PendingJump
{
    ObjectGuid caster;
    SpellInfo const* spellInfo;
    uint8 effectMask;
    int32 duration;
};

void JumpDamageOverTime(Unit* victim)
{
    std::vector<PendingJump> jumps;
    for (auto const& [spellId, aurApp] : victim->GetAppliedAuras())
    {
        Aura const* aura = aurApp->GetBase();
        if (aura->IsPassive() || aura->GetCasterGUID() == victim->GetGUID())
            continue;
        SpellInfo const* spellInfo = aura->GetSpellInfo();
        if (spellInfo->IsChanneled() || !HasDamageOverTime(spellInfo))
            continue;
        Unit* caster = aura->GetCaster();
        if (!caster || !Holds(caster, SPELL_CONTAGION) || !caster->IsInMap(victim))
            continue;
        int32 const left = aura->GetDuration();
        if (left == 0)
            continue;
        jumps.push_back({ caster->GetGUID(), spellInfo, aura->GetEffectMask(), left });
    }

    for (PendingJump const& jump : jumps)
    {
        Unit* caster = ObjectAccessor::GetUnit(*victim, jump.caster);
        if (!caster || !caster->IsAlive())
            continue;

        // The nearest enemy of the caster around the corpse that does not
        // already carry this DoT from them.
        Unit* next = nullptr;
        float best = std::numeric_limits<float>::max();
        for (Unit* candidate : EnemiesNear(victim, caster, CONTAGION_RADIUS))
        {
            if (candidate->GetAura(jump.spellInfo->Id, caster->GetGUID()))
                continue;
            float const dist = victim->GetExactDist(candidate);
            if (dist < best)
            {
                best = dist;
                next = candidate;
            }
        }
        if (!next)
            continue;

        // Amounts are worked out fresh against the new target, the time left
        // is what the dead one still had.
        if (Aura* moved = caster->AddAura(jump.spellInfo, jump.effectMask, next))
            if (jump.duration > 0 && jump.duration < moved->GetMaxDuration())
            {
                moved->SetMaxDuration(jump.duration);
                moved->SetDuration(jump.duration);
            }
    }
}

void ReturnTheDemon(Pet* pet)
{
    if (pet->getPetType() != SUMMON_PET)
        return;
    Player* owner = pet->GetOwner();
    if (!owner || owner->GetClass() != CLASS_WARLOCK || !owner->IsAlive() || !Holds(owner, SPELL_ETERNAL_PACT))
        return;

    uint32 const summonSpell = pet->GetUInt32Value(UNIT_CREATED_BY_SPELL);
    if (!summonSpell || !sSpellMgr->GetSpellInfo(summonSpell))
        return;

    SpellHistory* history = owner->GetSpellHistory();
    if (history->HasCooldown(SPELL_ETERNAL_PACT))
        return;
    history->AddCooldown(SPELL_ETERNAL_PACT, 0, std::chrono::milliseconds(ETERNAL_PACT_COOLDOWN_MS));

    // After the corpse is gone: a warlock pet in corpse state is removed on
    // its next update, and the summon refuses to replace a dead pet of the
    // same kind. Owner-scheduled, so the owner is valid when it runs.
    owner->m_Events.AddEventAtOffset([owner, summonSpell]()
    {
        if (!owner->IsInWorld() || !owner->IsAlive())
            return;
        if (Pet* old = owner->GetPet())
        {
            if (old->IsAlive())
                return;   // someone already raised one
            owner->RemovePet(old, PET_SAVE_NOT_IN_SLOT);
        }
        // Triggered: no cast time, no Soul Shard, no mana.
        owner->CastSpell(owner, summonSpell, TRIGGERED_FULL_MASK);
    }, std::chrono::milliseconds(ETERNAL_PACT_DELAY_MS));
}

// Totem Lord: the totems a holder has out, slotted or not.
std::vector<Totem*> TotemsOf(Unit* owner)
{
    std::vector<Totem*> out;
    for (Unit* unit : owner->m_Controlled)
        if (unit->IsTotem() && unit->GetOwnerGUID() == owner->GetGUID())
            out.push_back(static_cast<Totem*>(unit));
    return out;
}

bool InAnySlot(Unit const* owner, ObjectGuid guid)
{
    for (uint8 i = SUMMON_SLOT_TOTEM_FIRE; i < MAX_TOTEM_SLOT; ++i)
        if (owner->m_SummonSlot[i] == guid)
            return true;
    return false;
}
}

// ---- bookkeeping -------------------------------------------------------------

void OnTaken(Player* player)
{
    RefreshReactives(player);
    // Serenity's first charge comes with the boon; each wave that falls
    // brings the next (OnWaveRestored).
    OnWaveRestored(player);
}

void OnStripped(Player* player)
{
    if (!player)
        return;

    // Relentless kept the Revenge state and a combo point up by hand.
    if (Holds(player, SPELL_RELENTLESS))
    {
        if (player->HasAuraState(AURA_STATE_DEFENSE))
            player->ModifyAuraState(AURA_STATE_DEFENSE, false);
        player->ClearComboPoints();
    }

    // Day-long totems do not leave the Hold with their shaman.
    if (Holds(player, SPELL_TOTEM_LORD) && player->IsInWorld())
        player->UnsummonAllTotems();

    // Serenity's and Second Coming's charges are not boon auras of their own;
    // they go with the boon.
    player->RemoveAurasDueToSpell(SPELL_SERENITY_READY);
    player->RemoveAurasDueToSpell(SPELL_SECOND_COMING_READY);
    player->RemoveAurasDueToSpell(SPELL_PLAGUE_BURDEN);
}

// ---- warrior -----------------------------------------------------------------

bool IsJuggernautStun(uint32 spellId)
{
    switch (spellId)
    {
        case 7922:                                  // Charge Stun
        case 20253: case 20614: case 20615:         // Intercept Stun, ranks 1-3
        case 25273: case 25274:                     // Intercept ranks 4-5 (their stun)
            return true;
        default:
            return false;
    }
}

bool KeepsReactivesLit(Unit const* unit)
{
    return unit && unit->GetClass() == CLASS_WARRIOR && Holds(unit, SPELL_RELENTLESS);
}

void RefreshReactives(Unit* unit)
{
    if (!unit || !unit->IsAlive() || !KeepsReactivesLit(unit))
        return;

    // Revenge: CasterAuraState DEFENSE (the 262 aura already covers the
    // server's check; the state is what lights the client's button).
    if (!unit->HasAuraState(AURA_STATE_DEFENSE))
        unit->ModifyAuraState(AURA_STATE_DEFENSE, true);

    // Overpower: REQ_COMBO_POINTS on whatever is being hit. A warrior's point
    // normally comes from the target's dodge; the holder keeps one on the
    // selected enemy, which is what the client checks before it sends the cast.
    Player* player = unit->ToPlayer();
    if (!player || !player->IsInWorld())
        return;
    Unit* selected = ObjectAccessor::GetUnit(*player, player->GetTarget());
    if (!selected || selected == player || !selected->IsAlive() || !player->IsValidAttackTarget(selected))
        return;
    if (player->GetComboTarget() != selected || !player->GetComboPoints())
        player->AddComboPoints(selected, 1);
}

bool IgnoresComboRequirement(Unit const* caster, SpellInfo const* spellInfo)
{
    return Is(spellInfo, SPELLFAMILY_WARRIOR, "Overpower") && KeepsReactivesLit(caster);
}

bool IsCategoryUnbound(Unit const* owner, SpellInfo const* spellInfo)
{
    if (!owner || !spellInfo || !spellInfo->GetCategory())
        return false;

    switch (spellInfo->SpellFamilyName)
    {
        case SPELLFAMILY_WARRIOR:
            // Shield Wall, Recklessness, Retaliation: category 132, 30 min.
            return spellInfo->GetCategory() == 132 && Holds(owner, SPELL_WARLORD);
        case SPELLFAMILY_HUNTER:
            // The trap-laying spells (family flag 0x80; the trap effects are not).
            return (spellInfo->SpellFamilyFlags[0] & 0x80) && Holds(owner, SPELL_TRAPPER);
        default:
            return false;
    }
}

bool KeepsOneOfEachTrap(Unit const* caster, SpellInfo const* spellInfo)
{
    return spellInfo && spellInfo->SpellFamilyName == SPELLFAMILY_HUNTER
        && (spellInfo->SpellFamilyFlags[0] & 0x80) && Holds(caster, SPELL_TRAPPER);
}

std::vector<GameObject*> SameKindTraps(Unit const* caster, SpellInfo const* spellInfo)
{
    std::vector<GameObject*> out;
    if (!caster || !spellInfo)
        return out;

    // By name, not spell id: a lower rank laid earlier is still the same trap.
    for (GameObject* go : caster->GetAllGameObjects())
        if (go && SameSpell(sSpellMgr->GetSpellInfo(go->GetSpellId()), spellInfo))
            out.push_back(go);
    return out;
}

// ---- paladin -----------------------------------------------------------------

namespace
{
// Seal of Justice (20164) sits outside the seal family-flag collection, the
// same exception spell_pal_judgement and Unit::IsTwistableSeal make.
bool IsSeal(SpellInfo const* spellInfo)
{
    return spellInfo->GetSpellSpecific() == SPELL_SPECIFIC_SEAL || spellInfo->Id == 20164;
}
}

bool AurasStackFreely(Unit const* caster, SpellInfo const* a, SpellInfo const* b)
{
    if (!a || !b)
        return false;
    // Another rank of the same spell still replaces the old one.
    if (SameSpell(a, b))
        return false;

    SpellSpecificType const sa = a->GetSpellSpecific();
    SpellSpecificType const sb = b->GetSpellSpecific();
    if (sa == SPELL_SPECIFIC_AURA && sb == SPELL_SPECIFIC_AURA)
        return Holds(caster, SPELL_RADIANT_HOST);
    // Every seal at once, and every judgement they leave on one target.
    if ((IsSeal(a) && IsSeal(b)) || (sa == SPELL_SPECIFIC_JUDGEMENT && sb == SPELL_SPECIFIC_JUDGEMENT))
        return Holds(caster, SPELL_SEALKEEPER);
    return false;
}

bool JudgesEverySeal(Unit const* caster)
{
    return caster && caster->GetClass() == CLASS_PALADIN && Holds(caster, SPELL_SEALKEEPER);
}

void CollectJudgementSplash(Unit* caster, Unit* mainTarget, std::vector<Unit*>& out)
{
    if (!mainTarget || !Holds(caster, SPELL_JUDGEMENT_DAY))
        return;
    // Around the judged target (EnemiesNear leaves the target itself out).
    for (Unit* enemy : EnemiesNear(mainTarget, caster, JUDGEMENT_DAY_RADIUS))
        out.push_back(enemy);
}

// ---- hunter ------------------------------------------------------------------

bool IgnoresMinRange(Unit const* caster, SpellInfo const* spellInfo)
{
    return spellInfo && spellInfo->SpellFamilyName == SPELLFAMILY_HUNTER && Holds(caster, SPELL_POINT_BLANK);
}

bool ShootsWhileMoving(Unit const* caster, SpellInfo const* spellInfo)
{
    if (!spellInfo || !Holds(caster, SPELL_POINT_BLANK))
        return false;

    // Auto Shot plus every rank of the two aimed casts named by the boon.
    return spellInfo->Id == 75 || Is(spellInfo, SPELLFAMILY_HUNTER, "Aimed Shot")
        || Is(spellInfo, SPELLFAMILY_HUNTER, "Multi-Shot");
}

bool ShootsAllies(WorldObject const* caster, SpellInfo const* spellInfo)
{
    if (!caster || !spellInfo || spellInfo->SpellFamilyName != SPELLFAMILY_HUNTER
        || spellInfo->DmgClass != SPELL_DAMAGE_CLASS_RANGED)
        return false;

    // A shot that only hurts: nothing it does but the hit may land on a
    // friend (no stings, dazes, scatters or Tranquilizing Shot on an ally).
    bool hits = false;
    for (SpellEffectInfo const& effect : spellInfo->GetEffects())
    {
        switch (effect.Effect)
        {
            case SPELL_EFFECT_NONE:
                break;
            case SPELL_EFFECT_SCHOOL_DAMAGE:
            case SPELL_EFFECT_WEAPON_DAMAGE_NOSCHOOL:
            case SPELL_EFFECT_WEAPON_PERCENT_DAMAGE:
            case SPELL_EFFECT_WEAPON_DAMAGE:
            case SPELL_EFFECT_NORMALIZED_WEAPON_DMG:
                hits = true;
                break;
            default:
                return false;
        }
    }
    if (!hits)
        return false;

    Unit const* hunter = caster->ToUnit();
    return hunter && hunter->GetClass() == CLASS_HUNTER && Holds(hunter, SPELL_FIELD_MEDIC);
}

bool IsFieldMedicHeal(WorldObject const* caster, SpellInfo const* spellInfo, Unit const* target)
{
    if (!target || !ShootsAllies(caster, spellInfo))
        return false;
    // An enemy (a duel partner included) is still shot.
    return !caster->IsValidAttackTarget(target, spellInfo) && caster->IsValidAssistTarget(target);
}

bool HoldsThroughDamage(Aura const* aura)
{
    // Freezing Trap Effect, every rank. The trap casts it for its owner, so
    // the aura's caster is the hunter.
    if (!aura || !Is(aura->GetSpellInfo(), SPELLFAMILY_HUNTER, "Freezing Trap Effect"))
        return false;
    return Holds(aura->GetCaster(), SPELL_TRAPPER);
}

// ---- rogue -------------------------------------------------------------------

bool WaivesBehindRequirement(Unit const* caster, SpellInfo const* spellInfo)
{
    return Is(spellInfo, SPELLFAMILY_ROGUE, "Backstab") && Holds(caster, SPELL_ASSASSIN);
}

bool IgnoresAvoidance(Unit const* caster, SpellInfo const* spellInfo)
{
    return WaivesBehindRequirement(caster, spellInfo);
}

int8 GetExtraComboPoints(Unit const* caster, SpellInfo const* spellInfo)
{
    return Is(spellInfo, SPELLFAMILY_ROGUE, "Hemorrhage") && Holds(caster, SPELL_ASSASSIN) ? 1 : 0;
}

bool IsSinisterStrike(SpellInfo const* spellInfo)
{
    return Is(spellInfo, SPELLFAMILY_ROGUE, "Sinister Strike");
}

bool StealthHoldsThroughHits(Unit const* owner, SpellInfo const* stealth)
{
    return stealth && stealth->SpellFamilyName == SPELLFAMILY_ROGUE && Holds(owner, SPELL_UNSEEN);
}

int32 GetDurationMultiplier(WorldObject const* caster, SpellInfo const* spellInfo)
{
    return Is(spellInfo, SPELLFAMILY_ROGUE, "Kidney Shot") && Holds(caster, SPELL_CRIPPLER) ? CRIPPLER_MULTIPLIER : 1;
}

// ---- priest ------------------------------------------------------------------

bool FearHoldsThroughDamage(Unit const* caster, SpellInfo const* fear)
{
    return Is(fear, SPELLFAMILY_PRIEST, "Psychic Scream") && Holds(caster, SPELL_DREAD);
}

// ---- mage --------------------------------------------------------------------

void FreeFromStunAndRoot(Unit* unit)
{
    if (!unit)
        return;
    unit->RemoveAurasWithMechanic((1 << MECHANIC_STUN) | (1 << MECHANIC_ROOT), AURA_REMOVE_BY_DEFAULT, 0);
    unit->RemoveAurasByType(SPELL_AURA_MOD_STUN, ObjectGuid::Empty, nullptr, true, false);
    unit->RemoveAurasByType(SPELL_AURA_MOD_ROOT, ObjectGuid::Empty, nullptr, true, false);
}

std::vector<ObjectGuid> SlipstreamPassengers(Unit* mage)
{
    std::vector<ObjectGuid> out;
    if (!mage || mage->GetClass() != CLASS_MAGE || !Holds(mage, SPELL_SLIPSTREAM))
        return out;
    for (Player* ally : AlliesOf(mage))
        if (ally != mage && ally->IsAlive() && !ally->IsInFlight() && !ally->GetVehicle()
            && ally->IsWithinDistInMap(mage, SLIPSTREAM_RADIUS))
            out.push_back(ally->GetGUID());
    return out;
}

void SlipstreamArrive(Unit* mage, std::vector<ObjectGuid> const& passengers)
{
    if (!mage || !mage->IsInWorld())
        return;
    FreeFromStunAndRoot(mage);
    for (ObjectGuid guid : passengers)
    {
        Player* ally = ObjectAccessor::GetPlayer(*mage, guid);
        if (!ally || !ally->IsInWorld() || !ally->IsAlive() || !ally->IsInMap(mage) || ally->IsBeingTeleported())
            continue;
        FreeFromStunAndRoot(ally);
        ally->NearTeleportTo(mage->GetPositionX(), mage->GetPositionY(), mage->GetPositionZ(), ally->GetOrientation());
    }
}

uint32 GetSingleTargetLimit(Unit const* caster, SpellInfo const* spellInfo)
{
    if (spellInfo && spellInfo->GetSpellSpecific() == SPELL_SPECIFIC_MAGE_POLYMORPH
        && caster && caster->GetClass() == CLASS_MAGE && Holds(caster, SPELL_TRANSMUTATION))
        return TRANSMUTATION_POLYMORPH_TARGETS;
    return 0;
}

uint32 GetFrostNovaRank(Unit const* caster)
{
    Player const* player = caster ? caster->ToPlayer() : nullptr;
    if (!player)
        return 0;

    uint32 best = 0;
    uint32 bestLevel = 0;
    for (auto const& [spellId, known] : player->GetSpellMap())
    {
        if (known.state == PLAYERSPELL_REMOVED || known.disabled)
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!Is(info, SPELLFAMILY_MAGE, "Frost Nova") || info->IsPassive())
            continue;
        if (!best || info->SpellLevel > bestLevel)
        {
            best = spellId;
            bestLevel = info->SpellLevel;
        }
    }
    return best;
}

void OnSuccessfulInterrupt(Unit* caster, SpellInfo const* spellInfo, Unit* interruptedTarget,
    SpellSchoolMask schoolMask, uint32 lockoutTime)
{
    if (!Is(spellInfo, SPELLFAMILY_MAGE, "Counterspell") || !Holds(caster, SPELL_SPELLBREAKER)
        || schoolMask == SPELL_SCHOOL_MASK_NONE || !lockoutTime)
        return;

    // SpellHistory::ClearSchoolLockouts tells each client (SMSG_CLEAR_COOLDOWN
    // per locked spell), so the greyed buttons come back at once.
    for (Player* ally : AlliesOf(caster))
    {
        ally->GetSpellHistory()->ClearSchoolLockouts();
        if (Pet* pet = ally->GetPet())
            pet->GetSpellHistory()->ClearSchoolLockouts();
    }

    Player* mage = caster->GetCharmerOrOwnerPlayerOrPlayerItself();
    Battleground* battleground = mage ? mage->GetBattleground() : nullptr;
    if (!battleground || battleground->GetTypeID() != BATTLEGROUND_VHR)
        return;

    // The opposing Battleground roster is the current clone wave; dead clones
    // from earlier waves may still be present, so only touch living units on
    // this map. The directly interrupted unit already received this exact
    // lockout in Spell::EffectInterruptCast.
    uint32 const mageTeam = mage->GetBGTeam();
    for (auto const& [guid, bgPlayer] : battleground->GetPlayers())
    {
        if (bgPlayer.Team == mageTeam)
            continue;

        Player* enemy = ObjectAccessor::GetPlayer(*mage, guid);
        if (!enemy || !enemy->IsInWorld() || !enemy->IsAlive() || enemy->GetMap() != mage->GetMap())
            continue;

        if (enemy != interruptedTarget)
            enemy->GetSpellHistory()->LockSpellSchool(schoolMask, lockoutTime);

        for (Unit* controlled : enemy->m_Controlled)
            if (controlled && controlled != interruptedTarget && controlled->IsInWorld() && controlled->IsAlive()
                && controlled->GetMap() == mage->GetMap())
                controlled->GetSpellHistory()->LockSpellSchool(schoolMask, lockoutTime);
    }
}

// ---- warlock -----------------------------------------------------------------

bool IsUndispellable(Aura const* aura, Unit const* auraTarget)
{
    if (!aura || aura->GetSpellInfo()->IsPositive())
        return false;
    if (auraTarget && aura->GetCasterGUID() == auraTarget->GetGUID())
        return false;
    Unit const* caster = aura->GetCaster();
    return caster && caster->GetClass() == CLASS_WARLOCK && Holds(caster, SPELL_INDELIBLE);
}

void OnUnitDying(Unit* victim, Unit* attacker)
{
    if (!victim || !victim->IsInWorld())
        return;

    // Vengeance: every kill while berserk heals the warrior (a pet's kill counts).
    if (Unit* killer = attacker ? attacker->GetCharmerOrOwnerOrSelf() : nullptr)
        if (killer != victim && killer->IsAlive() && killer->HasAura(SPELL_VENGEANCE_FURY))
            killer->ModifyHealth(int32(killer->CountPctFromMaxHealth(VENGEANCE_KILL_HEAL_PCT)));

    JumpDamageOverTime(victim);

    if (Pet* pet = victim->ToPet())
        ReturnTheDemon(pet);
}

// ---- druid -------------------------------------------------------------------

bool ShiftsThroughControl(Unit const* caster, SpellInfo const* spellInfo)
{
    return spellInfo && spellInfo->SpellFamilyName == SPELLFAMILY_DRUID && spellInfo->HasAura(SPELL_AURA_MOD_SHAPESHIFT)
        && Holds(caster, SPELL_UNTAMED);
}

namespace
{
// Wildcaller: `buff` on the druid and every ally within WILDCALLER_RANGE.
void RallyParty(Unit* druid, uint32 buff)
{
    if (!druid->IsAlive() || !druid->IsInWorld())
        return;
    for (Player* ally : AlliesOf(druid))
        if (ally->IsAlive() && (ally == druid || ally->IsWithinDistInMap(druid, WILDCALLER_RANGE)))
            druid->AddAura(buff, ally);
}

void LeavePrimalEcho(Unit* druid, uint8 previousForm)
{
    if (!druid || druid->GetClass() != CLASS_DRUID || !druid->IsAlive() || !Holds(druid, SPELL_PRIMAL_ECHO))
        return;

    // One echo at a time. A rapid shift replaces the old memory rather than
    // filling the chamber with twelve-second summons.
    std::list<Creature*> oldEchoes;
    druid->GetCreatureListWithEntryInGrid(oldEchoes, NPC_PRIMAL_ECHO, 100.0f);
    for (Creature* old : oldEchoes)
        if (old && old->IsSummon() && old->ToTempSummon()->GetSummonerGUID() == druid->GetGUID())
            old->DespawnOrUnsummon();

    uint32 const display = druid->GetDisplayId();
    TempSummon* echo = druid->SummonCreature(NPC_PRIMAL_ECHO, *druid, TEMPSUMMON_TIMED_DESPAWN,
        Milliseconds(PRIMAL_ECHO_DURATION_MS));
    if (!echo)
        return;

    echo->SetFaction(druid->GetFaction());
    echo->SetLevel(druid->GetLevel());
    // A humanoid display id only identifies the race/sex model. The skin,
    // face, hair and equipped textures live in the player's mirror-image
    // payload, so copy that payload for a caster-form memory. Animal forms
    // already have their complete appearance in the shapeshift display id.
    if (previousForm == FORM_NONE)
    {
        Player* player = druid->ToPlayer();
        if (!player || !echo->CopyAppearanceFromPlayer(player, false, true, false))
            echo->SetDisplayId(display);
    }
    else
        echo->SetDisplayId(display);
    echo->SetUnitFlag2(UNIT_FLAG2_DO_NOT_FADE_IN);
    echo->SetObjectScale(0.85f);
    uint32 const healthPct = previousForm == FORM_BEAR || previousForm == FORM_DIREBEAR ? 40u : 25u;
    echo->SetMaxHealth(std::max<uint32>(1, druid->CountPctFromMaxHealth(healthPct)));
    echo->SetHealth(echo->GetMaxHealth());
    echo->SetAttackTime(BASE_ATTACK, druid->GetAttackTime(BASE_ATTACK));
    echo->SetBaseWeaponDamage(BASE_ATTACK, MINDAMAGE,
        std::max<float>(1.0f, druid->GetWeaponDamageRange(BASE_ATTACK, MINDAMAGE) * 0.25f));
    echo->SetBaseWeaponDamage(BASE_ATTACK, MAXDAMAGE,
        std::max<float>(2.0f, druid->GetWeaponDamageRange(BASE_ATTACK, MAXDAMAGE) * 0.25f));
    echo->UpdateDamagePhysical(BASE_ATTACK);
    if (echo->IsAIEnabled())
        echo->AI()->SetData(DATA_PRIMAL_ECHO_FORM, previousForm);
}
}

void OnShapeshift(Unit* target, SpellInfo const* form)
{
    if (!target || !form || form->SpellFamilyName != SPELLFAMILY_DRUID)
        return;

    uint8 const previousForm = uint8(target->GetShapeshiftForm());
    LeavePrimalEcho(target, previousForm);

    if (target->GetClass() == CLASS_DRUID && Holds(target, SPELL_WILDCALLER))
    {
        int32 shape = -1;
        for (SpellEffectInfo const& effect : form->GetEffects())
            if (effect.ApplyAuraName == SPELL_AURA_MOD_SHAPESHIFT)
                shape = effect.MiscValue;
        switch (shape)
        {
            case FORM_CAT:      RallyParty(target, SPELL_WILDCALLER_CAT); break;
            case FORM_BEAR:
            case FORM_DIREBEAR: RallyParty(target, SPELL_WILDCALLER_BEAR); break;
            case FORM_MOONKIN:  RallyParty(target, SPELL_OWL_CHOIR); break;
            default: break;
        }
    }

    if (!Holds(target, SPELL_UNTAMED))
        return;

    // Stunned, feared or polymorphed (Polymorph is a confuse aura): the shift
    // breaks it. Hostile auras only.
    target->RemoveAurasByType(SPELL_AURA_MOD_STUN, ObjectGuid::Empty, nullptr, true, false);
    target->RemoveAurasByType(SPELL_AURA_MOD_FEAR, ObjectGuid::Empty, nullptr, true, false);
    target->RemoveAurasByType(SPELL_AURA_MOD_CONFUSE, ObjectGuid::Empty, nullptr, true, false);
}

void OnCasterForm(Unit* target, uint8 previousForm)
{
    LeavePrimalEcho(target, previousForm);
    if (target && target->GetClass() == CLASS_DRUID && Holds(target, SPELL_WILDCALLER))
        RallyParty(target, SPELL_WILDCALLER_CLARITY);
}

float ModProcChance(Aura const* aura, float chance)
{
    if (aura && aura->GetId() == SPELL_WINDFURY_WEAPON_PASSIVE && Holds(aura->GetUnitOwner(), SPELL_WINDLORD))
        return WINDLORD_PROC_CHANCE;
    return chance;
}

float ModWindfuryTotemProcChance(Player const* wielder, uint32 enchantId, float chance)
{
    // Windfury Totem Effect's temporary enchants (ranks 1-3), the same ids
    // Unit.cpp's IsWindfuryTotemEnchant knows.
    if (!wielder || (enchantId != 1783 && enchantId != 563 && enchantId != 564))
        return chance;
    for (Player* ally : AlliesOf(const_cast<Player*>(wielder)))
        if (ally->GetClass() == CLASS_SHAMAN && ally->IsAlive() && Holds(ally, SPELL_WINDLORD))
            return WINDLORD_PROC_CHANCE;
    return chance;
}

bool KeepsWindfuryWithSeals(Unit const* unit, SpellInfo const* spellInfo)
{
    if (spellInfo && !Is(spellInfo, SPELLFAMILY_SHAMAN, "Windfury Totem Effect"))
        return false;
    Player const* player = unit ? unit->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
    Battleground const* bg = player ? player->GetBattleground() : nullptr;
    return bg && bg->GetTypeID() == BATTLEGROUND_VHR;
}

int32 ModManaCost(Unit const* caster, int32 cost)
{
    if (cost <= 0 || !caster || !caster->HasAura(SPELL_WILDCALLER_CLARITY))
        return cost;
    return cost - int32(CalculatePct(cost, WILDCALLER_MANA_CUT_PCT));
}

void OnManaChange(Unit* unit, int32 before, int32 after)
{
    if (!unit || after >= before || !Holds(unit, SPELL_WELLSPRING))
        return;
    Player* player = unit->ToPlayer();
    if (!player || !player->IsAlive() || !player->IsInWorld())
        return;

    int32 const threshold = int32(uint64(player->GetMaxPower(POWER_MANA)) * WELLSPRING_THRESHOLD_PCT / 100);
    if (before < threshold || after >= threshold)
        return;

    // Once per wave, counted by the party's run.
    BattlegroundVHR* run = PartyRunOf(player);
    if (!run || !run->ClaimOncePerWave(SPELL_WELLSPRING, player->GetGUID()))
        return;

    for (Player* ally : AlliesOf(player))
    {
        if (!ally->IsAlive() || !ally->GetMaxPower(POWER_MANA))
            continue;
        uint32 const perTick = ally->GetCreateMana() * WELLSPRING_PERMILLE_PER_TICK / 1000;
        if (!perTick)
            continue;
        CastSpellExtraArgs args(TRIGGERED_FULL_MASK);
        args.AddSpellBP0(int32(perTick));
        player->CastSpell(ally, SPELL_WELLSPRING_MANA, args);
    }
}

void OnWaveRestored(Player* player)
{
    if (!player)
        return;
    if (player->GetClass() == CLASS_DRUID && Holds(player, SPELL_SERENITY) && !player->HasAura(SPELL_SERENITY_READY))
        player->CastSpell(player, SPELL_SERENITY_READY, TRIGGERED_FULL_MASK);
    if (player->GetClass() == CLASS_PRIEST && Holds(player, SPELL_SECOND_COMING) && !player->HasAura(SPELL_SECOND_COMING_READY))
        player->CastSpell(player, SPELL_SECOND_COMING_READY, TRIGGERED_FULL_MASK);
}

bool IsSecondComingResurrection(Unit const* caster, SpellInfo const* spellInfo)
{
    return spellInfo && caster && spellInfo->SpellFamilyName == SPELLFAMILY_PRIEST
        && spellInfo->HasEffect(SPELL_EFFECT_RESURRECT_NEW) && caster->HasAura(SPELL_SECOND_COMING_READY);
}

// ---- the rest of the second block -------------------------------------------

namespace
{
// Vengeance's crowd-control immunity (the fury's aura 147 carries the same
// mask); what is already on the warrior is broken too.
constexpr uint32 VENGEANCE_CC_MASK =
      (1 << MECHANIC_CHARM) | (1 << MECHANIC_DISORIENTED) | (1 << MECHANIC_FEAR) | (1 << MECHANIC_ROOT)
    | (1 << MECHANIC_SILENCE) | (1 << MECHANIC_SLEEP) | (1 << MECHANIC_SNARE) | (1 << MECHANIC_STUN)
    | (1 << MECHANIC_FREEZE) | (1 << MECHANIC_KNOCKOUT) | (1 << MECHANIC_POLYMORPH) | (1 << MECHANIC_BANISH)
    | (1 << MECHANIC_SHACKLE) | (1 << MECHANIC_HORROR) | (1 << MECHANIC_DAZE) | (1 << MECHANIC_SAPPED);

// A harmful effect Plaguebearer can take or counts: cast on `bearer` by an
// enemy, timed, not passive and not one that outlives death.
bool IsCarriedHarm(Aura const* aura, Unit const* bearer)
{
    SpellInfo const* info = aura->GetSpellInfo();
    if (aura->IsPassive() || info->IsPositive() || info->IsDeathPersistent() || aura->GetDuration() <= 0)
        return false;
    if (aura->GetCasterGUID() == bearer->GetGUID())
        return false;
    Unit const* caster = aura->GetCaster();
    return caster && caster->IsHostileTo(bearer);
}

void SyncPlagueBurden(Player* lock)
{
    uint32 carried = 0;
    for (auto const& [spellId, aurApp] : lock->GetAppliedAuras())
        if (IsCarriedHarm(aurApp->GetBase(), lock))
            ++carried;
    carried = std::min<uint32>(carried, PLAGUEBEARER_MAX_STACKS);

    Aura* burden = lock->GetAura(SPELL_PLAGUE_BURDEN);
    if (!carried)
    {
        if (burden)
            burden->Remove();
        return;
    }
    if (!burden)
        burden = lock->AddAura(SPELL_PLAGUE_BURDEN, lock);
    if (burden && burden->GetStackAmount() != carried)
        burden->SetStackAmount(uint8(carried));
}

// One harmful effect off the nearest-listed ally onto the warlock, with the
// time it had left. Its caster must still be there to put it back on.
void PullOnePlague(Player* lock)
{
    for (Player* ally : AlliesOf(lock))
    {
        if (ally == lock || !ally->IsAlive() || !ally->IsWithinDistInMap(lock, PLAGUEBEARER_RADIUS))
            continue;

        Aura* taken = nullptr;
        for (auto const& [spellId, aurApp] : ally->GetAppliedAuras())
            if (IsCarriedHarm(aurApp->GetBase(), ally) && aurApp->GetBase()->GetCaster()->IsInMap(lock))
            {
                taken = aurApp->GetBase();
                break;
            }
        if (!taken)
            continue;

        Unit* caster = taken->GetCaster();
        SpellInfo const* info = taken->GetSpellInfo();
        int32 const left = taken->GetDuration();
        uint8 const stacks = taken->GetStackAmount();
        uint8 const effMask = taken->GetEffectMask();
        if (Aura* moved = caster->AddAura(info, effMask, lock))
        {
            if (left > 0 && left < moved->GetMaxDuration())
            {
                moved->SetMaxDuration(left);
                moved->SetDuration(left);
            }
            if (stacks > 1)
                moved->SetStackAmount(stacks);
            ally->RemoveAura(taken);
        }
        return;
    }
}

void EvenOutHealth(Player* shaman)
{
    std::vector<Player*> linked;
    for (Player* ally : AlliesOf(shaman))
        if (ally->IsAlive() && (ally == shaman || ally->IsWithinDistInMap(shaman, SPIRIT_LINK_RADIUS)))
            linked.push_back(ally);
    if (linked.size() < 2)
        return;

    uint64 health = 0, maxHealth = 0;
    for (Player* member : linked)
    {
        health += member->GetHealth();
        maxHealth += member->GetMaxHealth();
    }
    if (!maxHealth)
        return;
    double const share = double(health) / double(maxHealth);
    for (Player* member : linked)
        member->SetHealth(std::max<uint32>(1, uint32(member->GetMaxHealth() * share)));
}
}

void OnSpellCast(Unit* caster, SpellInfo const* spellInfo)
{
    if (!caster || !Is(spellInfo, SPELLFAMILY_PALADIN, "Divine Shield") || !Holds(caster, SPELL_AEGIS))
        return;
    for (Player* ally : AlliesOf(caster))
        if (ally != caster && ally->IsAlive() && ally->IsWithinDistInMap(caster, AEGIS_RADIUS))
            caster->AddAura(SPELL_AEGIS_SHIELD, ally);
}

void OnAllyDied(Player* dead)
{
    if (!dead)
        return;
    for (Player* warrior : AlliesOf(dead))
    {
        if (warrior == dead || warrior->GetClass() != CLASS_WARRIOR || !warrior->IsAlive()
            || !Holds(warrior, SPELL_VENGEANCE))
            continue;
        warrior->RemoveAurasWithMechanic(VENGEANCE_CC_MASK, AURA_REMOVE_BY_DEFAULT, 0);
        warrior->CastSpell(warrior, SPELL_VENGEANCE_FURY, TRIGGERED_FULL_MASK);
    }
}

void OnAllyDamaged(Unit* victim, uint32 damage)
{
    // Every hit on the server passes here: the cheap tests first.
    if (!damage || !victim || victim->GetTypeId() != TYPEID_PLAYER || !victim->IsAlive())
        return;
    uint32 const health = victim->GetHealth();
    uint32 const threshold = uint32(victim->CountPctFromMaxHealth(BODY_DOUBLE_HEALTH_PCT));
    if (damage >= health || health <= threshold || health - damage > threshold)
        return;
    Player* ally = victim->ToPlayer();
    Battleground* bg = ally->GetBattleground();
    if (!bg || bg->GetTypeID() != BATTLEGROUND_VHR)
        return;

    for (Player* rogue : AlliesOf(ally))
    {
        if (rogue == ally || rogue->GetClass() != CLASS_ROGUE || !rogue->IsAlive() || !Holds(rogue, SPELL_BODY_DOUBLE)
            || !rogue->IsWithinDistInMap(ally, BODY_DOUBLE_RANGE) || rogue->GetSpellHistory()->HasCooldown(SPELL_BODY_DOUBLE))
            continue;

        rogue->GetSpellHistory()->AddCooldown(SPELL_BODY_DOUBLE, 0, std::chrono::milliseconds(BODY_DOUBLE_COOLDOWN_MS));
        // After the hit that called it has landed: nothing moves in the
        // middle of the damage pass.
        ObjectGuid const allyGuid = ally->GetGUID();
        rogue->m_Events.AddEventAtOffset([rogue, allyGuid]()
        {
            Player* saved = ObjectAccessor::GetPlayer(*rogue, allyGuid);
            if (!saved || !saved->IsAlive() || !rogue->IsAlive() || !saved->IsInMap(rogue)
                || saved->IsBeingTeleported() || rogue->IsBeingTeleported())
                return;
            Position const roguePos = rogue->GetPosition();
            Position const allyPos = saved->GetPosition();
            rogue->NearTeleportTo(allyPos.GetPositionX(), allyPos.GetPositionY(), allyPos.GetPositionZ(), rogue->GetOrientation());
            saved->NearTeleportTo(roguePos.GetPositionX(), roguePos.GetPositionY(), roguePos.GetPositionZ(), saved->GetOrientation());
            rogue->AddAura(SPELL_BODY_DOUBLE_VEIL, saved);
            rogue->CastSpell(rogue, SPELL_ROGUE_EVASION, TRIGGERED_FULL_MASK);
        }, std::chrono::milliseconds(1));
        return;
    }
}

void UpdateRun(BattlegroundVHR* run, uint32 tick)
{
    if (!run)
        return;
    for (auto const& [guid, bgPlayer] : run->GetPlayers())
    {
        if (bgPlayer.Team != run->GetHumanTeam())
            continue;
        Player* player = ObjectAccessor::FindPlayer(guid);
        if (!player || !player->IsInWorld())
            continue;

        if (player->GetClass() == CLASS_WARLOCK && Holds(player, SPELL_PLAGUEBEARER))
        {
            if (player->IsAlive() && tick % 3 == 0)
                PullOnePlague(player);
            SyncPlagueBurden(player);
        }

        if (tick % 2 == 0 && player->GetClass() == CLASS_PRIEST && player->IsAlive() && Holds(player, SPELL_SPIRIT_LINK))
            EvenOutHealth(player);
    }
}

// ---- shaman ------------------------------------------------------------------

bool DispelsEverything(WorldObject const* caster, SpellInfo const* spellInfo)
{
    return Is(spellInfo, SPELLFAMILY_SHAMAN, "Purge") && Holds(caster, SPELL_STORMCALLER);
}

void BeforeTotemSummon(Unit* owner, SpellInfo const* totemSpell, int32& duration)
{
    if (!owner || !totemSpell || !Holds(owner, SPELL_TOTEM_LORD))
        return;

    duration = int32(TOTEM_LORD_DURATION_MS);

    // Recasting replaces only the earlier copy of THIS totem (any rank). The
    // slot no longer does it (KeepsDisplacedTotem).
    for (Totem* totem : TotemsOf(owner))
        if (SameSpell(sSpellMgr->GetSpellInfo(totem->GetUInt32Value(UNIT_CREATED_BY_SPELL)), totemSpell))
            totem->UnSummon();
}

bool KeepsDisplacedTotem(Unit const* owner, Creature const* displaced)
{
    return displaced && displaced->IsTotem() && Holds(owner, SPELL_TOTEM_LORD);
}

void OnTotemSlotFreed(Unit* owner, uint8 slot, Totem const* leaving)
{
    if (!owner || slot < SUMMON_SLOT_TOTEM_FIRE || slot >= MAX_TOTEM_SLOT || !Holds(owner, SPELL_TOTEM_LORD))
        return;
    Player* player = owner->ToPlayer();
    if (!player || owner->m_SummonSlot[slot])
        return;

    // The newest still-standing totem of that element takes the bar back.
    Totem* heir = nullptr;
    for (Totem* totem : TotemsOf(owner))
    {
        if (totem == leaving || !totem->IsAlive() || !totem->m_Properties || totem->m_Properties->Slot != slot)
            continue;
        if (InAnySlot(owner, totem->GetGUID()))
            continue;
        if (!heir || totem->GetTotemDuration() > heir->GetTotemDuration())
            heir = totem;
    }
    if (!heir)
        return;

    owner->m_SummonSlot[slot] = heir->GetGUID();
    WorldPackets::Totem::TotemCreated data;
    data.Totem = heir->GetGUID();
    data.Slot = slot - SUMMON_SLOT_TOTEM_FIRE;
    data.Duration = heir->GetTotemDuration();
    data.SpellID = heir->GetUInt32Value(UNIT_CREATED_BY_SPELL);
    player->SendDirectMessage(data.Write());
}

void UpdateTotemFollow(Totem* totem)
{
    Unit* owner = totem ? totem->GetOwner() : nullptr;
    if (!owner || !owner->IsAlive() || !Holds(owner, SPELL_TOTEM_LORD) || !totem->IsInWorld() || !totem->IsInMap(owner))
        return;

    if (totem->isMoving() || totem->GetExactDist2d(owner) <= TOTEM_LORD_FOLLOW_DISTANCE)
        return;

    if (totem->GetSpeedRate(MOVE_RUN) != TOTEM_LORD_SPEED_RATE)
        totem->SetSpeedRate(MOVE_RUN, TOTEM_LORD_SPEED_RATE);
    if (totem->HasUnitState(UNIT_STATE_ROOT))
        totem->SetControlled(false, UNIT_STATE_ROOT);

    // A spot a little short of the shaman, on the side the totem comes from.
    float const angle = owner->GetAbsoluteAngle(totem);
    float x = owner->GetPositionX() + 3.0f * std::cos(angle);
    float y = owner->GetPositionY() + 3.0f * std::sin(angle);
    float z = owner->GetPositionZ();
    owner->UpdateAllowedPositionZ(x, y, z);
    totem->GetMotionMaster()->MovePoint(0, x, y, z);
}

void UnsummonLooseTotems(Unit* owner)
{
    if (!owner || !Holds(owner, SPELL_TOTEM_LORD))
        return;
    // Everything still standing, slotted or not: unsummoning a slotted one
    // hands its slot to an older one (OnTotemSlotFreed), which must go too.
    for (Totem* totem : TotemsOf(owner))
        if (totem->IsAlive())
            totem->UnSummon();
}

bool TryAncestors(Unit* victim)
{
    Player* player = victim ? victim->ToPlayer() : nullptr;
    if (!player || !player->IsAlive() || !Holds(player, SPELL_ANCESTORS))
        return false;

    // Spent now, before the damage lands (Phoenix's rule), and only the
    // party's run keeps the once-a-wave count.
    BattlegroundVHR* run = PartyRunOf(player);
    return run && run->ClaimOncePerWave(SPELL_ANCESTORS, player->GetGUID());
}

void CompleteAncestors(Unit* victim)
{
    Player* player = victim ? victim->ToPlayer() : nullptr;
    if (!player || !player->IsAlive())
        return;

    // Reincarnation's own amount (20%), raised by Improved Reincarnation: its
    // second effect is a flat ALL_EFFECTS modifier on Reincarnation (+10/+20),
    // which CalcValue applies (ApplyEffectModifiers).
    int32 pct = 20;
    if (SpellInfo const* reincarnation = sSpellMgr->GetSpellInfo(SPELL_REINCARNATION_RESURRECT))
        pct = reincarnation->GetEffect(EFFECT_0).CalcValue(player);
    pct = std::clamp<int32>(pct, 1, 100);

    player->SetHealth(std::max<uint32>(1, player->CountPctFromMaxHealth(pct)));
    if (player->GetMaxPower(POWER_MANA))
        player->SetPower(POWER_MANA, player->CountPctFromMaxPower(POWER_MANA, pct));

    player->CastSpell(player, SPELL_ANCESTORS_VISUAL, TRIGGERED_FULL_MASK);
    if (WorldSession* session = player->GetSession())
        session->SendNotification("The ancestors return you to the fight!");
}
}
