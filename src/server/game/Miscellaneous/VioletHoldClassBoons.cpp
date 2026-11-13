/*
 * Violet Hold class boons (artifact tier) - see VioletHoldClassBoons.h.
 */

#include "VioletHoldClassBoons.h"

#include "Battleground.h"
#include "BattlegroundVHR.h"
#include "CellImpl.h"
#include "DBCStores.h"
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

// ---- paladin -----------------------------------------------------------------

bool IgnoresForbearance(Unit const* caster, Unit const* target)
{
    return Holds(caster, SPELL_UNBOUND_LIGHT) || Holds(target, SPELL_UNBOUND_LIGHT);
}

bool AurasStackFreely(Unit const* caster, SpellInfo const* a, SpellInfo const* b)
{
    if (!a || !b || a->GetSpellSpecific() != SPELL_SPECIFIC_AURA || b->GetSpellSpecific() != SPELL_SPECIFIC_AURA)
        return false;
    // Another rank of the same aura still replaces the old one.
    if (SameSpell(a, b))
        return false;
    return Holds(caster, SPELL_RADIANT_HOST);
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
    // Auto Shot only: wands and the hunter's other casts keep their rules.
    return spellInfo && spellInfo->Id == 75 && Holds(caster, SPELL_SKIRMISHER);
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

void OnSuccessfulInterrupt(Unit* caster, SpellInfo const* spellInfo)
{
    if (!Is(spellInfo, SPELLFAMILY_MAGE, "Counterspell") || !Holds(caster, SPELL_SPELLBREAKER))
        return;

    // SpellHistory::ClearSchoolLockouts tells each client (SMSG_CLEAR_COOLDOWN
    // per locked spell), so the greyed buttons come back at once.
    for (Player* ally : AlliesOf(caster))
    {
        ally->GetSpellHistory()->ClearSchoolLockouts();
        if (Pet* pet = ally->GetPet())
            pet->GetSpellHistory()->ClearSchoolLockouts();
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

void OnUnitDying(Unit* victim)
{
    if (!victim || !victim->IsInWorld())
        return;

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

void OnShapeshift(Unit* target, SpellInfo const* form)
{
    if (!target || !form || form->SpellFamilyName != SPELLFAMILY_DRUID || !Holds(target, SPELL_UNTAMED))
        return;

    // Stunned, feared or polymorphed (Polymorph is a confuse aura): the shift
    // breaks it. Hostile auras only.
    target->RemoveAurasByType(SPELL_AURA_MOD_STUN, ObjectGuid::Empty, nullptr, true, false);
    target->RemoveAurasByType(SPELL_AURA_MOD_FEAR, ObjectGuid::Empty, nullptr, true, false);
    target->RemoveAurasByType(SPELL_AURA_MOD_CONFUSE, ObjectGuid::Empty, nullptr, true, false);
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
