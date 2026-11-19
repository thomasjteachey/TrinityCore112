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
#include "Map.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Pet.h"
#include "Player.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellHistory.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "Totem.h"
#include "TotemPackets.h"
#include "TemporarySummon.h"
#include "WorldSession.h"
#include "WorldPacket.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <list>
#include <map>
#include <set>
#include <unordered_map>

namespace VioletHoldClassBoons
{
namespace
{
constexpr float WINDLORD_PROC_CHANCE = 50.0f;

// World objects and per-summon clocks owned by the new artifacts. They are
// keyed by full GUIDs, so simultaneous Violet Hold instances never cross.
std::unordered_map<ObjectGuid, ObjectGuid> WarBanners;
struct TotemPulse
{
    ObjectGuid Owner;
    uint32 DueMs = 0;
};
std::unordered_map<ObjectGuid, TotemPulse> LightningRodPulses;

constexpr std::array<uint32, 6> VolleyRanks = { 1510, 14294, 14295, 27022, 58431, 58434 };

// Every crowd-control mechanic (Vengeance's and Unstoppable's immunity masks
// carry the same value, 1226210982, in their aura 147); Slipstream breaks
// whatever of it is on an ally.
constexpr uint32 CROWD_CONTROL_MASK =
      (1 << MECHANIC_CHARM) | (1 << MECHANIC_DISORIENTED) | (1 << MECHANIC_FEAR) | (1 << MECHANIC_ROOT)
    | (1 << MECHANIC_SILENCE) | (1 << MECHANIC_SLEEP) | (1 << MECHANIC_SNARE) | (1 << MECHANIC_STUN)
    | (1 << MECHANIC_FREEZE) | (1 << MECHANIC_KNOCKOUT) | (1 << MECHANIC_POLYMORPH) | (1 << MECHANIC_BANISH)
    | (1 << MECHANIC_SHACKLE) | (1 << MECHANIC_HORROR) | (1 << MECHANIC_DAZE) | (1 << MECHANIC_SAPPED);

// Domination: the faction each dominated unit had before, to give back.
std::unordered_map<ObjectGuid, uint32> DominatedFactions;

// Ice Fortress: (mage, attacker) -> game time (ms) before which that attacker
// cannot be frozen by that mage again.
std::map<std::pair<ObjectGuid, ObjectGuid>, uint32> IceFortressCooldowns;

// Soul Bond: demons already promised to a killing blow this instant, so two
// lethal hits in one pass cannot both be taken by one demon.
std::set<ObjectGuid> SoulBondSpent;

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

uint32 HighestKnown(Player const* player, SpellFamilyNames family, char const* name)
{
    uint32 best = 0;
    uint8 bestRank = 0;
    if (!player)
        return 0;
    for (auto const& [spellId, known] : player->GetSpellMap())
    {
        if (known.state == PLAYERSPELL_REMOVED || known.disabled)
            continue;
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!Is(info, family, name) || info->IsPassive())
            continue;
        if (!best || info->GetRank() >= bestRank)
        {
            best = spellId;
            bestRank = info->GetRank();
        }
    }
    return best;
}

void RemoveWarBanner(Player* warrior)
{
    if (!warrior)
        return;
    auto itr = WarBanners.find(warrior->GetGUID());
    if (itr == WarBanners.end())
        return;
    if (warrior->IsInWorld())
        if (GameObject* banner = warrior->GetMap()->GetGameObject(itr->second))
            banner->Delete();
    WarBanners.erase(itr);
}

void PlantWarBanner(Player* warrior)
{
    RemoveWarBanner(warrior);
    if (!warrior || !warrior->IsInWorld())
        return;

    uint32 const entry = warrior->GetTeam() == ALLIANCE ? GO_WAR_BANNER_ALLIANCE : GO_WAR_BANNER_HORDE;
    QuaternionData const rotation = QuaternionData::fromEulerAnglesZYX(warrior->GetOrientation(), 0.0f, 0.0f);
    if (GameObject* banner = warrior->SummonGameObject(entry, warrior->GetPosition(), rotation,
        Seconds(24 * 60 * 60), GO_SUMMON_TIMED_DESPAWN))
        WarBanners[warrior->GetGUID()] = banner->GetGUID();
}

// Whether `unit` stands within WAR_BANNER_RADIUS of a banner planted by its
// own side (the warrior included).
bool InFriendlyWarBanner(Unit const* unit)
{
    if (!unit || !unit->IsInWorld())
        return false;
    for (auto itr = WarBanners.begin(); itr != WarBanners.end();)
    {
        Player* owner = ObjectAccessor::FindPlayer(itr->first);
        GameObject* banner = owner && owner->IsInWorld() && owner->GetMap() == unit->GetMap()
            ? owner->GetMap()->GetGameObject(itr->second) : nullptr;
        if (!owner || !banner)
        {
            itr = WarBanners.erase(itr);
            continue;
        }
        if ((unit == owner || owner->IsValidAssistTarget(unit)) && banner->IsWithinDistInMap(unit, WAR_BANNER_RADIUS))
            return true;
        ++itr;
    }
    return false;
}

bool CleanseOne(Player* paladin)
{
    uint32 const mask = SpellInfo::GetDispelMask(DISPEL_MAGIC)
        | SpellInfo::GetDispelMask(DISPEL_DISEASE)
        | SpellInfo::GetDispelMask(DISPEL_POISON);

    for (Player* ally : AlliesOf(paladin))
    {
        if (!ally->IsAlive())
            continue;
        DispelChargesList choices;
        ally->GetDispellableAuraList(paladin, mask, choices);
        if (choices.empty())
            continue;

        Aura* aura = choices.front().GetAura();
        if (!aura)
            continue;
        uint32 const auraId = aura->GetId();
        ObjectGuid const auraCaster = aura->GetCasterGUID();

        WorldPacket data(SMSG_SPELLDISPELLOG, 8 + 8 + 4 + 1 + 4 + 5);
        data << ally->GetPackGUID();
        data << paladin->GetPackGUID();
        data << uint32(4987); // Cleanse
        data << uint8(0);
        data << uint32(1);
        data << auraId;
        data << uint8(1);     // cleansed, not offensively dispelled
        paladin->SendMessageToSet(&data, true);

        ally->RemoveAurasDueToSpellByDispel(auraId, 4987, auraCaster, paladin, 1);
        return true;
    }
    return false;
}

void FireLightningRods(Player* shaman)
{
    uint32 const now = GameTime::GetGameTimeMS();
    std::set<ObjectGuid> active;
    for (Totem* totem : TotemsOf(shaman))
    {
        if (!totem || !totem->IsAlive() || !totem->IsInMap(shaman))
            continue;
        active.insert(totem->GetGUID());
        auto [itr, inserted] = LightningRodPulses.try_emplace(totem->GetGUID(), TotemPulse{ shaman->GetGUID(), now + LIGHTNING_ROD_PERIOD_MS });
        if (inserted || int32(now - itr->second.DueMs) < 0)
            continue;
        itr->second.DueMs = now + LIGHTNING_ROD_PERIOD_MS;

        uint32 const bolt = HighestKnown(shaman, SPELLFAMILY_SHAMAN, "Lightning Bolt");
        if (!bolt)
            continue;
        float const reach = shaman->GetExactDist(totem) + LIGHTNING_ROD_WIDTH;
        for (Unit* enemy : EnemiesNear(shaman, shaman, reach))
            if (enemy->IsInBetween(shaman, totem, LIGHTNING_ROD_WIDTH))
                shaman->CastSpell(enemy, bolt, TRIGGERED_FULL_MASK);
    }

    for (auto itr = LightningRodPulses.begin(); itr != LightningRodPulses.end();)
        if (itr->second.Owner == shaman->GetGUID() && !active.count(itr->first))
            itr = LightningRodPulses.erase(itr);
        else
            ++itr;
}
}

// ---- bookkeeping -------------------------------------------------------------

void OnTaken(Player* player)
{
    RefreshReactives(player);
    // Serenity's first charge comes with the boon; each wave that falls
    // brings the next (OnWaveRestored).
    OnWaveRestored(player);
    // Alpha: the pet already out gets it now, later ones on summon.
    if (player)
        if (Pet* pet = player->GetPet())
            OnMinionAdded(player, pet);
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

    if (Holds(player, SPELL_UNENDING_VOLLEY))
        for (uint32 spellId : VolleyRanks)
            player->RemoveDynObject(spellId);

    if (Holds(player, SPELL_WAR_BANNER))
        RemoveWarBanner(player);

    for (auto itr = LightningRodPulses.begin(); itr != LightningRodPulses.end();)
        if (itr->second.Owner == player->GetGUID())
            itr = LightningRodPulses.erase(itr);
        else
            ++itr;

    // Serenity's and Second Coming's charges are not boon auras of their own;
    // they go with the boon.
    player->RemoveAurasDueToSpell(SPELL_SERENITY_READY);
    player->RemoveAurasDueToSpell(SPELL_SECOND_COMING_READY);
    player->RemoveAurasDueToSpell(SPELL_PLAGUE_BURDEN);
    player->RemoveAurasDueToSpell(SPELL_UNSTOPPABLE_BUFF);
    player->RemoveAurasDueToSpell(SPELL_SHADOWSTALKER_VEIL);
    for (uint32 command : { SPELL_COMMAND_BATTLE, SPELL_COMMAND_DEFENSIVE, SPELL_COMMAND_BERSERKER })
        player->RemoveAurasDueToSpell(command);

    // Alpha rides on the pet, which may be dead or dismissed: the blessing is
    // death-persistent like Beastmaster's.
    if (Pet* pet = player->GetPet())
        pet->RemoveAurasDueToSpell(SPELL_ALPHA_PET);
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
    if (sa == SPELL_SPECIFIC_ASPECT && sb == SPELL_SPECIFIC_ASPECT)
        return Holds(caster, SPELL_ASPECT_SAVANT);
    if (sa == SPELL_SPECIFIC_CURSE && sb == SPELL_SPECIFIC_CURSE)
        return Holds(caster, SPELL_CURSEWEAVER);
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

bool IsBarrage(Unit const* caster, SpellInfo const* spellInfo)
{
    return Holds(caster, SPELL_BARRAGE) && Is(spellInfo, SPELLFAMILY_HUNTER, "Multi-Shot");
}

bool BarrageAvoidsTarget(Unit const* caster, SpellInfo const* spellInfo, Unit const* target)
{
    if (!IsBarrage(caster, spellInfo) || !target || !caster->IsValidAttackTarget(target, spellInfo))
        return false;

    // The core helper catches Polymorph, Sap, Freezing Trap, breakable roots,
    // and similar auras carrying TAKE_DAMAGE. Fears on this realm break through
    // their proc instead, so include fear/confuse/transform explicitly.
    return target->HasBreakableByDamageCrowdControlAura()
        || target->HasAuraType(SPELL_AURA_MOD_FEAR)
        || target->HasAuraType(SPELL_AURA_MOD_CONFUSE)
        || target->HasAuraType(SPELL_AURA_TRANSFORM);
}

bool IsUnendingVolley(Unit const* caster, SpellInfo const* spellInfo)
{
    return Is(spellInfo, SPELLFAMILY_HUNTER, "Volley") && Holds(caster, SPELL_UNENDING_VOLLEY);
}

void BeforeUnendingVolleyPlaced(Unit* caster)
{
    if (!caster)
        return;
    for (uint32 spellId : VolleyRanks)
        caster->RemoveDynObject(spellId);
}

void MakeUnendingVolleyPermanent(Unit const* caster, SpellInfo const* spellInfo, Aura* aura)
{
    if (!aura || !IsUnendingVolley(caster, spellInfo))
        return;
    aura->SetMaxDuration(-1);
    aura->SetDuration(-1);
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
    if (!aura)
        return false;
    SpellInfo const* spellInfo = aura->GetSpellInfo();
    // Freezing Trap Effect, every rank. The trap casts it for its owner, so
    // the aura's caster is the hunter.
    if (Is(spellInfo, SPELLFAMILY_HUNTER, "Freezing Trap Effect"))
        return Holds(aura->GetCaster(), SPELL_TRAPPER);
    // Woolen Prison: every mage Polymorph (sheep, pig, turtle, cow, the rank
    // copies) - whatever SpellInfo calls SPELL_SPECIFIC_MAGE_POLYMORPH.
    if (spellInfo->SpellFamilyName == SPELLFAMILY_MAGE && spellInfo->GetSpellSpecific() == SPELL_SPECIFIC_MAGE_POLYMORPH)
        return Holds(aura->GetCaster(), SPELL_WOOLEN_PRISON);
    return false;
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

bool DreadEmpowersPsychicScream(Unit const* caster, SpellInfo const* fear)
{
    return Is(fear, SPELLFAMILY_PRIEST, "Psychic Scream") && Holds(caster, SPELL_DREAD);
}

// ---- mage --------------------------------------------------------------------

void FreeFromCrowdControl(Unit* unit)
{
    if (!unit)
        return;
    unit->RemoveAurasWithMechanic(CROWD_CONTROL_MASK, AURA_REMOVE_BY_DEFAULT, 0);
    // By aura type too: plenty of crowd control carries no mechanic of its own.
    // Hostile auras only - a druid's own root-breaking form or a friendly
    // shapeshift is not crowd control.
    for (AuraType type : { SPELL_AURA_MOD_STUN, SPELL_AURA_MOD_ROOT, SPELL_AURA_MOD_FEAR, SPELL_AURA_MOD_CONFUSE,
                           SPELL_AURA_MOD_CHARM, SPELL_AURA_MOD_POSSESS, SPELL_AURA_TRANSFORM, SPELL_AURA_MOD_SILENCE,
                           SPELL_AURA_MOD_PACIFY_SILENCE, SPELL_AURA_MOD_DECREASE_SPEED, SPELL_AURA_MOD_PACIFY })
        unit->RemoveAurasByType(type, ObjectGuid::Empty, nullptr, true, false);
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
    FreeFromCrowdControl(mage);
    for (ObjectGuid guid : passengers)
    {
        Player* ally = ObjectAccessor::GetPlayer(*mage, guid);
        if (!ally || !ally->IsInWorld() || !ally->IsAlive() || !ally->IsInMap(mage) || ally->IsBeingTeleported())
            continue;
        FreeFromCrowdControl(ally);
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

void OnSpellCanceled(Spell const* spell)
{
    Player* mage = spell && spell->GetCaster() ? spell->GetCaster()->ToPlayer() : nullptr;
    SpellInfo const* info = spell ? spell->GetSpellInfo() : nullptr;
    if (!mage || !info || mage->GetClass() != CLASS_MAGE || !Holds(mage, SPELL_MIRROR_IMAGE)
        || spell->IsTriggered() || (spell->GetCastTime() <= 0 && !info->IsChanneled())
        || mage->GetSpellHistory()->HasCooldown(SPELL_MIRROR_IMAGE))
        return;

    int32 const remaining = std::max<int32>(1, spell->GetRemainingCastTime());
    Milliseconds const lifetime(std::max<int32>(3000, remaining + 2000));
    TempSummon* image = mage->SummonCreature(NPC_MIRROR_IMAGE, mage->GetPosition(), TEMPSUMMON_TIMED_DESPAWN, lifetime);
    if (!image)
        return;

    mage->GetSpellHistory()->AddCooldown(SPELL_MIRROR_IMAGE, 0, std::chrono::milliseconds(MIRROR_IMAGE_COOLDOWN_MS));
    image->SetFaction(mage->GetFaction());
    image->SetLevel(mage->GetLevel());
    image->SetReactState(REACT_PASSIVE);
    image->SetUnitFlag(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_UNINTERACTIBLE);
    image->SetOwnerGUID(mage->GetGUID());
    image->CopyAppearanceFromPlayer(mage, false, true, false);
    // Take over the time left on the interrupted bar instead of restarting a
    // fresh full cast. UNIT_MOD_CAST_SPEED is a multiplier in this core.
    int32 const fullCast = std::max<int32>(1, info->IsChanneled() ? info->GetDuration() : spell->GetCastTime());
    image->SetModCastingSpeed(std::clamp(float(remaining) / float(fullCast), 0.01f, 1.0f));

    TriggerCastFlags const flags = TriggerCastFlags(TRIGGERED_IGNORE_GCD
        | TRIGGERED_IGNORE_SPELL_AND_CATEGORY_CD | TRIGGERED_IGNORE_POWER_AND_REAGENT_COST
        | TRIGGERED_IGNORE_CAST_IN_PROGRESS | TRIGGERED_IGNORE_COMBO_POINTS
        | TRIGGERED_IGNORE_SHAPESHIFT | TRIGGERED_IGNORE_CASTER_AURASTATE
        | TRIGGERED_IGNORE_CASTER_MOUNTED_OR_ON_VEHICLE | TRIGGERED_IGNORE_EQUIPPED_ITEM_REQUIREMENT);
    CastSpellExtraArgs args(flags);
    args.SetOriginalCaster(mage->GetGUID());

    SpellCastTargets const& original = spell->GetCastTargets();
    if (original.HasDst())
        image->CastSpell(CastSpellTargetArg(*original.GetDstPos()), info->Id, args);
    else if (ObjectGuid const targetGuid = original.GetUnitTargetGUID())
    {
        Unit* target = targetGuid == mage->GetGUID() ? mage : ObjectAccessor::GetUnit(*mage, targetGuid);
        if (target)
            image->CastSpell(target, info->Id, args);
    }
    else
        image->CastSpell(mage, info->Id, args);
}

void OnEvocationTick(Unit* caster)
{
    if (!caster || !Holds(caster, SPELL_MANA_CONDUIT))
        return;
    for (Player* ally : AlliesOf(caster))
    {
        if (ally == caster || !ally->IsAlive() || !ally->IsWithinDistInMap(caster, MANA_CONDUIT_RADIUS))
            continue;
        Powers const power = ally->GetPowerType();
        int32 const maximum = ally->GetMaxPower(power);
        if (maximum > 0)
            ally->ModifyPower(power, std::max<int32>(1, CalculatePct(maximum, 5)));
    }
}

// ---- warlock -----------------------------------------------------------------

bool IsUndispellable(Aura const* aura, Unit const* auraTarget)
{
    if (!aura)
        return false;
    // Sacrifice: a holder's Hand of Sacrifice stays on through every dispel.
    if (Is(aura->GetSpellInfo(), SPELLFAMILY_PALADIN, "Hand of Sacrifice"))
        return Holds(aura->GetCaster(), SPELL_SACRIFICE);
    // Wild Kinship: and a holder's Mark (or Gift) of the Wild.
    if (Is(aura->GetSpellInfo(), SPELLFAMILY_DRUID, "Mark of the Wild") || Is(aura->GetSpellInfo(), SPELLFAMILY_DRUID, "Gift of the Wild"))
        return Holds(aura->GetCaster(), SPELL_WILD_KINSHIP);
    if (aura->GetSpellInfo()->IsPositive())
        return false;
    if (auraTarget && aura->GetCasterGUID() == auraTarget->GetGUID())
        return false;
    Unit const* caster = aura->GetCaster();
    if (DreadEmpowersPsychicScream(caster, aura->GetSpellInfo()))
        return true;
    return caster && caster->GetClass() == CLASS_WARLOCK && Holds(caster, SPELL_INDELIBLE);
}

namespace
{
// Spell Mirror. The Grounding Totem button is the wrapper 81478 (15 sec
// cooldown); 8177, and the Autumn set's 81481, are what it casts, triggered.
constexpr std::array<uint32, 3> GROUNDING_TOTEM_SPELLS = { 81478, 8177, 81481 };
// How long a totem outlives the hit of the spell it caught.
constexpr Milliseconds SPELL_MIRROR_LINGER = Milliseconds(1000);

Unit* SpellMirrorOwner(Unit const* totem)
{
    if (!totem || !totem->IsTotem() || !totem->IsMagnet())
        return nullptr;
    Unit* owner = totem->GetOwner();
    return owner && owner->GetClass() == CLASS_SHAMAN && Holds(owner, SPELL_SPELL_MIRROR) ? owner : nullptr;
}

void ResetGroundingTotem(Unit* shaman)
{
    for (uint32 spellId : GROUNDING_TOTEM_SPELLS)
        shaman->GetSpellHistory()->ResetCooldown(spellId, true);
}
}

void OnUnitDying(Unit* victim, Unit* attacker)
{
    if (!victim || !victim->IsInWorld())
        return;

    // Spell Mirror: a holder's Grounding Totem that falls is ready again.
    if (Unit* shaman = SpellMirrorOwner(victim))
        ResetGroundingTotem(shaman);

    // Vengeance: every kill while berserk heals the warrior (a pet's kill counts).
    if (Unit* killer = attacker ? attacker->GetCharmerOrOwnerOrSelf() : nullptr)
        if (killer != victim && killer->IsAlive() && killer->HasAura(SPELL_VENGEANCE_FURY))
            killer->ModifyHealth(int32(killer->CountPctFromMaxHealth(VENGEANCE_KILL_HEAL_PCT)));

    JumpDamageOverTime(victim);

    if (Pet* pet = victim->ToPet())
        ReturnTheDemon(pet);

    // Marked Prey: a holder's Hunter's Mark moves on to the nearest enemy of
    // the hunter around the fallen one. Collected first: casting while
    // walking the victim's aura list would be walking a list that can change.
    std::vector<std::pair<Unit*, uint32>> marks;
    for (AuraEffect const* stalk : victim->GetAuraEffectsByType(SPELL_AURA_MOD_STALKED))
        if (Is(stalk->GetSpellInfo(), SPELLFAMILY_HUNTER, "Hunter's Mark"))
            if (Unit* hunter = stalk->GetCaster())
                if (hunter != victim && hunter->IsAlive() && Holds(hunter, SPELL_MARKED_PREY))
                    marks.emplace_back(hunter, stalk->GetId());
    for (auto const& [hunter, markId] : marks)
    {
        Unit* next = nullptr;
        for (Unit* enemy : EnemiesNear(victim, hunter, MARKED_PREY_JUMP_RADIUS))
            if (!next || victim->GetExactDist(enemy) < victim->GetExactDist(next))
                next = enemy;
        if (next)
            hunter->CastSpell(next, markId, TRIGGERED_FULL_MASK);
    }

    // Shadowstalker: the rogue's own killing blow (not a pet's) puts them
    // straight back in Stealth, under 3 sec of total immunity so the fight
    // around them cannot knock it off at once. Triggered: in combat, off
    // Stealth's cooldown, and starting none.
    Player* rogue = attacker ? attacker->ToPlayer() : nullptr;
    if (rogue && rogue != victim && rogue->IsAlive() && rogue->GetClass() == CLASS_ROGUE
        && Holds(rogue, SPELL_SHADOWSTALKER))
    {
        rogue->CastSpell(rogue, SPELL_SHADOWSTALKER_VEIL, TRIGGERED_FULL_MASK);
        if (!rogue->HasStealthAura())
            if (uint32 stealth = HighestKnown(rogue, SPELLFAMILY_ROGUE, "Stealth"))
                rogue->CastSpell(rogue, stealth, TRIGGERED_FULL_MASK);
    }
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
    switch (previousForm)
    {
        case FORM_CAT:
            echo->SetPowerType(POWER_ENERGY);
            echo->SetMaxPower(POWER_ENERGY, 100);
            echo->SetPower(POWER_ENERGY, 100);
            break;
        case FORM_BEAR:
        case FORM_DIREBEAR:
            echo->SetPowerType(POWER_RAGE);
            echo->SetMaxPower(POWER_RAGE, 1000);
            // Creature echoes do not receive player-style rage from their
            // outgoing white damage. Seed a full bar so the short-lived bear
            // can actually pay for Charge, Bash and queued Mauls.
            echo->SetPower(POWER_RAGE, 1000);
            break;
        default:
            echo->SetPowerType(POWER_MANA);
            echo->SetMaxPower(POWER_MANA, std::max<uint32>(1, druid->GetMaxPower(POWER_MANA)));
            echo->SetPower(POWER_MANA, echo->GetMaxPower(POWER_MANA));
            break;
    }
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

bool PausesHealingOverTime(Aura const* aura, Unit const* target)
{
    if (!aura || !target || aura->GetDuration() <= 0 || !target->IsAlive()
        || target->GetHealth() < target->GetMaxHealth())
        return false;
    Unit const* caster = aura->GetCaster();
    if (!caster || caster->GetClass() != CLASS_DRUID || !Holds(caster, SPELL_PATIENT_GROWTH)
        || !aura->GetSpellInfo()->IsPositive())
        return false;
    for (SpellEffectInfo const& effect : aura->GetSpellInfo()->GetEffects())
        if (effect.ApplyAuraName == SPELL_AURA_PERIODIC_HEAL)
            return true;
    return false;
}

// ---- the rest of the second block -------------------------------------------

namespace
{
// Vengeance's crowd-control immunity (the fury's aura 147 carries the same
// mask); what is already on the warrior is broken too.
constexpr uint32 VENGEANCE_CC_MASK = CROWD_CONTROL_MASK;

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

// Inner Light and Supercharged Shield: a self-only buff cast with a friendly
// player selected goes to that player. The client always casts it on the
// caster, so it is moved one event later, once it has landed (the same rank,
// with the caster still its caster, so the caster's spellmods and boons keep
// shaping it). Called from OnSpellCast, before the cast lands: whatever the
// caster wore that this cast replaces on them - their own earlier copy, or for
// an elemental shield the other shield - is noted here and put back.
void CastOnSelectedAlly(Unit* caster, SpellInfo const* spellInfo, float range)
{
    Player* self = caster ? caster->ToPlayer() : nullptr;
    Unit* ally = self ? ObjectAccessor::GetUnit(*self, self->GetTarget()) : nullptr;
    if (!ally || ally == self || ally->GetTypeId() != TYPEID_PLAYER || !ally->IsAlive() || !self->IsValidAssistTarget(ally)
        || !ally->IsWithinDistInMap(self, range) || !self->IsWithinLOSInMap(ally))
        return;

    SpellSpecificType const specific = spellInfo->GetSpellSpecific();
    uint32 oldId = 0;
    int32 oldDuration = 0;
    uint8 oldCharges = 0;
    for (auto const& [id, app] : self->GetAppliedAuras())
    {
        Aura const* aura = app->GetBase();
        if (aura->GetCasterGUID() != self->GetGUID())
            continue;
        if (SameSpell(aura->GetSpellInfo(), spellInfo)
            || (specific != SPELL_SPECIFIC_NORMAL && aura->GetSpellInfo()->GetSpellSpecific() == specific))
        {
            oldId = id;
            oldDuration = aura->GetDuration();
            oldCharges = aura->GetCharges();
            break;
        }
    }

    ObjectGuid const allyGuid = ally->GetGUID();
    uint32 const spellId = spellInfo->Id;
    self->m_Events.AddEventAtOffset([self, allyGuid, spellId, oldId, oldDuration, oldCharges]()
    {
        Unit* target = ObjectAccessor::GetUnit(*self, allyGuid);
        if (!target || !target->IsAlive() || !target->IsInMap(self))
            return;
        self->RemoveAurasDueToSpell(spellId, self->GetGUID());
        self->AddAura(spellId, target);
        if (oldId)
            if (Aura* back = self->AddAura(oldId, self))
            {
                back->SetDuration(oldDuration);
                if (oldCharges)
                    back->SetCharges(oldCharges);
            }
    }, std::chrono::milliseconds(1));
}
}

void OnSpellCast(Unit* caster, SpellInfo const* spellInfo, Unit* explicitTarget)
{
    if (!caster || !spellInfo)
        return;

    if (Is(spellInfo, SPELLFAMILY_PALADIN, "Divine Shield") && Holds(caster, SPELL_AEGIS))
        for (Player* ally : AlliesOf(caster))
            if (ally != caster && ally->IsAlive() && ally->IsWithinDistInMap(caster, AEGIS_RADIUS))
                caster->AddAura(SPELL_AEGIS_SHIELD, ally);

    if (Is(spellInfo, SPELLFAMILY_WARRIOR, "Battle Shout") && Holds(caster, SPELL_WAR_BANNER))
        if (Player* warrior = caster->ToPlayer())
            PlantWarBanner(warrior);

    // Unstoppable: every Heroic Strike (it lands on the next swing, which is
    // when this runs) renews the 30 sec immunity and breaks what is on now.
    if (Is(spellInfo, SPELLFAMILY_WARRIOR, "Heroic Strike") && Holds(caster, SPELL_UNSTOPPABLE))
    {
        caster->RemoveAurasWithMechanic(CROWD_CONTROL_MASK, AURA_REMOVE_BY_DEFAULT, 0);
        caster->CastSpell(caster, SPELL_UNSTOPPABLE_BUFF, TRIGGERED_FULL_MASK);
    }

    // Smoke Bomb: Vanish takes the whole party with it, wherever they stand.
    if (Is(spellInfo, SPELLFAMILY_ROGUE, "Vanish") && caster->GetClass() == CLASS_ROGUE && Holds(caster, SPELL_SMOKE_BOMB))
        for (Player* ally : AlliesOf(caster))
            if (ally != caster && ally->IsAlive())
                caster->AddAura(SPELL_SMOKE_BOMB_VEIL, ally);

    // Holy Pyre: the burn (Holy Fire's periodic effect, with the priest's
    // bonuses) also lands on every enemy around the target.
    if (Is(spellInfo, SPELLFAMILY_PRIEST, "Holy Fire") && caster->GetClass() == CLASS_PRIEST && explicitTarget
        && Holds(caster, SPELL_HOLY_PYRE) && caster->IsValidAttackTarget(explicitTarget))
    {
        uint8 burnMask = 0;
        for (SpellEffectInfo const& effect : spellInfo->GetEffects())
            if (effect.ApplyAuraName == SPELL_AURA_PERIODIC_DAMAGE)
                burnMask |= uint8(1 << effect.EffectIndex);
        if (burnMask)
            for (Unit* enemy : EnemiesNear(explicitTarget, caster, HOLY_PYRE_RADIUS))
                caster->AddAura(spellInfo, burnMask, enemy);
    }

    // Blood Pact: Bloodrage shares itself out as each ally's own resource.
    if (Is(spellInfo, SPELLFAMILY_WARRIOR, "Bloodrage") && caster->GetClass() == CLASS_WARRIOR && Holds(caster, SPELL_BLOOD_PACT))
        for (Player* ally : AlliesOf(caster))
        {
            if (ally == caster || !ally->IsAlive() || !ally->IsWithinDistInMap(caster, BLOOD_PACT_RADIUS))
                continue;
            Powers const power = ally->GetPowerType();
            int32 const maximum = ally->GetMaxPower(power);
            if (maximum > 0)
                ally->ModifyPower(power, std::max<int32>(1, CalculatePct(maximum, BLOOD_PACT_PCT)));
        }

    // Blade Dance: once the new Slice and Dice is on the rogue (one event on),
    // allies within 30 yd get its haste for as long as it lasts, plus 20% casting.
    if (Is(spellInfo, SPELLFAMILY_ROGUE, "Slice and Dice") && caster->GetClass() == CLASS_ROGUE && Holds(caster, SPELL_BLADE_DANCE))
        if (Player* rogue = caster->ToPlayer())
            rogue->m_Events.AddEventAtOffset([rogue]()
            {
                AuraEffect const* slice = nullptr;
                for (AuraEffect const* haste : rogue->GetAuraEffectsByType(SPELL_AURA_MOD_MELEE_HASTE))
                    if (haste->GetCasterGUID() == rogue->GetGUID() && Is(haste->GetSpellInfo(), SPELLFAMILY_ROGUE, "Slice and Dice"))
                        slice = haste;
                if (!slice || !rogue->IsAlive())
                    return;
                int32 const amount = slice->GetAmount();
                int32 const duration = slice->GetBase()->GetDuration();
                for (Player* ally : AlliesOf(rogue))
                {
                    if (ally == rogue || !ally->IsAlive() || !ally->IsWithinDistInMap(rogue, BLADE_DANCE_RADIUS))
                        continue;
                    Aura* dance = rogue->AddAura(SPELL_BLADE_DANCE_BUFF, ally);
                    if (!dance)
                        continue;
                    if (duration > 0)
                    {
                        dance->SetMaxDuration(duration);
                        dance->SetDuration(duration);
                    }
                    for (SpellEffIndex melee : { EFFECT_0, EFFECT_1 })
                        if (AuraEffect* haste = dance->GetEffect(melee))
                            haste->ChangeAmount(amount);
                }
            }, std::chrono::milliseconds(1));

    // Soul Swap: Death Coil trades health percentages with its target first.
    if (Is(spellInfo, SPELLFAMILY_WARLOCK, "Death Coil") && caster->GetClass() == CLASS_WARLOCK && explicitTarget
        && explicitTarget->IsAlive() && caster->IsValidAttackTarget(explicitTarget) && Holds(caster, SPELL_SOUL_SWAP))
    {
        float const mine = caster->GetHealthPct();
        float const theirs = explicitTarget->GetHealthPct();
        caster->SetHealth(std::max<uint32>(1, uint32(caster->GetMaxHealth() * theirs / 100.0f)));
        explicitTarget->SetHealth(std::max<uint32>(1, uint32(explicitTarget->GetMaxHealth() * mine / 100.0f)));
    }

    // Inner Light: Inner Fire cast with a friendly player selected goes to them.
    if (Is(spellInfo, SPELLFAMILY_PRIEST, "Inner Fire") && caster->GetClass() == CLASS_PRIEST && Holds(caster, SPELL_INNER_LIGHT))
        CastOnSelectedAlly(caster, spellInfo, INNER_LIGHT_RANGE);

    // Supercharged Shield: so does Lightning Shield (the shield itself; its
    // orbs are triggered casts and never reach OnSpellCast).
    if (Is(spellInfo, SPELLFAMILY_SHAMAN, "Lightning Shield") && spellInfo->HasAura(SPELL_AURA_PROC_TRIGGER_SPELL)
        && caster->GetClass() == CLASS_SHAMAN && Holds(caster, SPELL_SUPERCHARGED_SHIELD))
        CastOnSelectedAlly(caster, spellInfo, SUPERCHARGED_SHIELD_RANGE);

    // Unburdened: Lay on Hands leaves the paladin full instead of empty.
    if (Is(spellInfo, SPELLFAMILY_PALADIN, "Lay on Hands") && caster->GetClass() == CLASS_PALADIN
        && Holds(caster, SPELL_UNBURDENED))
        caster->SetPower(POWER_MANA, caster->GetMaxPower(POWER_MANA));

    // Domination: Mind Control's 10 sec cooldown, on every rank the priest
    // knows (the DBC gives it none, so the client is told explicitly).
    if (IsDomination(caster, spellInfo))
        if (Player* priest = caster->ToPlayer())
        {
            std::vector<uint32> ranks;
            for (auto const& [id, known] : priest->GetSpellMap())
            {
                if (known.state == PLAYERSPELL_REMOVED || known.disabled)
                    continue;
                if (SameSpell(sSpellMgr->GetSpellInfo(id), spellInfo))
                    ranks.push_back(id);
            }
            if (std::find(ranks.begin(), ranks.end(), spellInfo->Id) == ranks.end())
                ranks.push_back(spellInfo->Id);
            for (uint32 id : ranks)
                priest->GetSpellHistory()->AddCooldown(id, 0, std::chrono::milliseconds(DOMINATION_COOLDOWN_MS));
            priest->GetSpellHistory()->SendCooldowns(ranks);
        }

    Player* warlock = caster->ToPlayer();
    if (!warlock || !explicitTarget || spellInfo->GetSpellSpecific() != SPELL_SPECIFIC_CURSE
        || !Holds(warlock, SPELL_CURSEWEAVER) || !caster->IsValidAttackTarget(explicitTarget))
        return;

    // Highest known rank of every curse chain. Triggered copies do not return
    // through OnSpellCast, so casting one curse applies the set exactly once.
    std::map<uint32, std::pair<uint8, uint32>> curses;
    for (auto const& [spellId, known] : warlock->GetSpellMap())
    {
        if (known.state == PLAYERSPELL_REMOVED || known.disabled)
            continue;
        SpellInfo const* curse = sSpellMgr->GetSpellInfo(spellId);
        if (!curse || curse->IsPassive() || curse->GetSpellSpecific() != SPELL_SPECIFIC_CURSE)
            continue;
        uint32 const chain = sSpellMgr->GetFirstSpellInChain(spellId);
        auto& best = curses[chain ? chain : spellId];
        if (!best.second || curse->GetRank() >= best.first)
            best = { curse->GetRank(), spellId };
    }
    uint32 const castChain = sSpellMgr->GetFirstSpellInChain(spellInfo->Id);
    for (auto const& [chain, rankAndId] : curses)
        if (chain != (castChain ? castChain : spellInfo->Id))
            warlock->CastSpell(explicitTarget, rankAndId.second, TRIGGERED_FULL_MASK);
}

uint32 ModifyWarBannerDamage(Unit* attacker, Unit* victim, uint32 damage)
{
    if (!damage)
        return 0;
    if (InFriendlyWarBanner(attacker))
        damage += CalculatePct(damage, 10);
    if (InFriendlyWarBanner(victim))
        damage -= CalculatePct(damage, 10);
    return damage;
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

        if (tick % 6 == 0 && player->GetClass() == CLASS_PALADIN && player->IsAlive() && Holds(player, SPELL_CLEANSE))
            CleanseOne(player);

        if (player->GetClass() == CLASS_SHAMAN && player->IsAlive() && Holds(player, SPELL_LIGHTNING_ROD))
            FireLightningRods(player);

        if (player->GetClass() == CLASS_WARRIOR && Holds(player, SPELL_COMMANDER))
            SyncCommand(player);

        // Wild Kinship: every 2 sec the druid, and every ally wearing the
        // druid's Mark (or Gift) of the Wild, heals 1% of maximum health.
        if (tick % 2 == 0 && player->GetClass() == CLASS_DRUID && player->IsAlive() && Holds(player, SPELL_WILD_KINSHIP))
        {
            SpellInfo const* kinship = sSpellMgr->GetSpellInfo(SPELL_WILD_KINSHIP);
            for (Player* ally : AlliesOf(player))
            {
                if (!ally->IsAlive() || ally->GetHealth() >= ally->GetMaxHealth())
                    continue;
                bool marked = ally == player;
                for (AuraEffect const* mark : ally->GetAuraEffectsByType(SPELL_AURA_MOD_RESISTANCE))
                    if (!marked && mark->GetCasterGUID() == player->GetGUID()
                        && (Is(mark->GetSpellInfo(), SPELLFAMILY_DRUID, "Mark of the Wild")
                            || Is(mark->GetSpellInfo(), SPELLFAMILY_DRUID, "Gift of the Wild")))
                        marked = true;
                if (!marked || !kinship)
                    continue;
                HealInfo heal(player, ally, std::max<uint32>(1, uint32(ally->CountPctFromMaxHealth(WILD_KINSHIP_PCT))),
                    kinship, kinship->GetSchoolMask());
                player->HealBySpell(heal);
            }
        }
    }
}

// ---- shaman ------------------------------------------------------------------

bool DispelsEverything(WorldObject const* caster, SpellInfo const* spellInfo)
{
    return Is(spellInfo, SPELLFAMILY_SHAMAN, "Purge") && Holds(caster, SPELL_STORMCALLER);
}

bool PreventsAspectDaze(Aura const* aura)
{
    if (!aura || !Holds(aura->GetCaster(), SPELL_ASPECT_SAVANT))
        return false;
    return Is(aura->GetSpellInfo(), SPELLFAMILY_HUNTER, "Aspect of the Cheetah")
        || Is(aura->GetSpellInfo(), SPELLFAMILY_HUNTER, "Aspect of the Pack");
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

// ---- the fourth block (2026-11-18) -------------------------------------------

bool IgnoresForbearance(Unit const* caster, Unit const* target)
{
    return (caster && caster->GetClass() == CLASS_PALADIN && Holds(caster, SPELL_UNBURDENED))
        || Holds(target, SPELL_UNBURDENED);
}

uint32 GetGlobalCooldownCategory(Unit const* owner, SpellInfo const* spellInfo)
{
    if (!spellInfo)
        return 0;
    // Asked on every cast and GCD check: the class and family flag first,
    // the aura last. Flash of Light is paladin family flag 0x40000000.
    if (spellInfo->StartRecoveryCategory && owner && owner->GetClass() == CLASS_PALADIN
        && spellInfo->SpellFamilyName == SPELLFAMILY_PALADIN && (spellInfo->SpellFamilyFlags[0] & 0x40000000)
        && NameIs(spellInfo, "Flash of Light") && Holds(owner, SPELL_SWIFT_LIGHT))
        return SPELL_SWIFT_LIGHT;
    return spellInfo->StartRecoveryCategory;
}

void OnMinionAdded(Player* owner, Unit* minion)
{
    if (!owner || !minion)
        return;
    Pet* pet = minion->ToPet();
    bool const blessed = pet && pet->getPetType() == HUNTER_PET && owner->GetClass() == CLASS_HUNTER
        && Holds(owner, SPELL_ALPHA);
    if (blessed == minion->HasAura(SPELL_ALPHA_PET))
        return;
    if (!blessed)
    {
        minion->RemoveAurasDueToSpell(SPELL_ALPHA_PET);
        return;
    }
    // AddAura, as Beastmaster does: SetMinion runs before the pet is in its map.
    owner->AddAura(SPELL_ALPHA_PET, minion);
    if (minion->IsInWorld() && !minion->CanModifyStats())
        minion->UpdateAllStats();
}

void SpendComboPoints(Unit* caster)
{
    if (!caster)
        return;
    if (caster->GetClass() == CLASS_ROGUE && caster->GetComboPoints() > BUTCHER_COMBO_POINTS && Holds(caster, SPELL_BUTCHER))
        caster->AddComboPoints(-BUTCHER_COMBO_POINTS);
    else
        caster->ClearComboPoints();
}

float GetExtraMeleeReach(Unit const* unit)
{
    // Every melee check on the server comes through here: players only, then
    // the class, then the aura.
    if (!unit || unit->GetTypeId() != TYPEID_PLAYER || unit->GetClass() != CLASS_ROGUE)
        return 0.0f;
    return Holds(unit, SPELL_LONG_REACH) ? LONG_REACH_YARDS : 0.0f;
}

bool DivertsIntoStun(Unit* caster, Unit* target)
{
    if (!caster || !target || caster == target || !target->IsAlive() || caster->GetClass() != CLASS_ROGUE
        || !Holds(caster, SPELL_DIVERSION) || !caster->IsValidAttackTarget(target))
        return false;
    caster->CastSpell(target, SPELL_DIVERSION_STUN, TRIGGERED_FULL_MASK);
    return true;
}

bool IsDomination(Unit const* caster, SpellInfo const* spellInfo)
{
    return caster && caster->GetClass() == CLASS_PRIEST && Is(spellInfo, SPELLFAMILY_PRIEST, "Mind Control")
        && Holds(caster, SPELL_DOMINATION);
}

void Dominate(Unit* priest, Unit* victim)
{
    if (!priest || !victim || priest == victim || !victim->IsAlive() || victim->HasAura(SPELL_DOMINATED))
        return;

    // The victim keeps its own brain - a wave clone goes on running its
    // playerbot logic, a creature its own AI (the owner: "use our custom
    // playerbot logic, not the pet logic of wow"). It only changes sides:
    // the bot picks fights by faction (IsValidAttackTarget), so taking the
    // priest's faction turns its old side into its enemies. Its battleground
    // team is left alone, so the wave still counts it as one of its own.
    DominatedFactions[victim->GetGUID()] = victim->GetFaction();
    victim->SetFaction(priest->GetFaction());
    for (Unit* controlled : victim->m_Controlled)
        if (controlled && controlled->GetOwnerGUID() == victim->GetGUID())
            controlled->SetFaction(priest->GetFaction());

    // Whatever it was hitting is now a friend, and whoever was hitting it
    // must find somebody else.
    victim->AttackStop();
    victim->CombatStop(true);
    if (Player* bot = victim->ToPlayer())
        bot->SetSelection(ObjectGuid::Empty);

    priest->AddAura(SPELL_DOMINATED, victim);
}

void EndDomination(Unit* victim)
{
    if (!victim)
        return;
    auto itr = DominatedFactions.find(victim->GetGUID());
    if (itr == DominatedFactions.end())
        return;
    uint32 const faction = itr->second;
    DominatedFactions.erase(itr);

    victim->SetFaction(faction);
    for (Unit* controlled : victim->m_Controlled)
        if (controlled && controlled->GetOwnerGUID() == victim->GetGUID())
            controlled->SetFaction(faction);
    victim->AttackStop();
    victim->CombatStop(true);
    if (Player* bot = victim->ToPlayer())
        bot->SetSelection(ObjectGuid::Empty);
}

// ---- the fifth block (2026-11-18, later) -------------------------------------

void OnAllyStruck(Unit* attacker, Unit* victim, uint32 damage)
{
    // Every hit on the server passes here: the cheap tests first.
    if (!damage || !attacker || !victim || attacker == victim || victim->GetTypeId() != TYPEID_PLAYER || !victim->IsAlive())
        return;
    uint32 const health = victim->GetHealth();
    uint32 const half = uint32(victim->CountPctFromMaxHealth(GUARDIAN_BEAST_HEALTH_PCT));
    if (damage >= health || health < half || health - damage >= half)
        return;
    Player* ally = victim->ToPlayer();
    Battleground* bg = ally->GetBattleground();
    if (!bg || bg->GetTypeID() != BATTLEGROUND_VHR)
        return;

    for (Player* hunter : AlliesOf(ally))
    {
        if (hunter->GetClass() != CLASS_HUNTER || !hunter->IsAlive() || !Holds(hunter, SPELL_GUARDIAN_BEAST)
            || hunter->GetSpellHistory()->HasCooldown(SPELL_GUARDIAN_BEAST))
            continue;
        Pet* pet = hunter->GetPet();
        if (!pet || !pet->IsAlive() || !pet->IsInMap(attacker) || !pet->IsValidAttackTarget(attacker))
            continue;

        hunter->GetSpellHistory()->AddCooldown(SPELL_GUARDIAN_BEAST, 0, std::chrono::milliseconds(GUARDIAN_BEAST_THROTTLE_MS));
        // After the hit that called it has landed: nothing moves in the
        // middle of the damage pass (Body Double's rule).
        ObjectGuid const petGuid = pet->GetGUID();
        ObjectGuid const foeGuid = attacker->GetGUID();
        hunter->m_Events.AddEventAtOffset([hunter, petGuid, foeGuid]()
        {
            Pet* beast = hunter->GetPet();
            if (!beast || beast->GetGUID() != petGuid || !beast->IsAlive() || !beast->IsInWorld())
                return;
            Unit* foe = ObjectAccessor::GetUnit(*beast, foeGuid);
            if (!foe || !foe->IsAlive() || !foe->IsInMap(beast) || !beast->IsValidAttackTarget(foe))
                return;
            Position const spot = foe->GetNearPosition(1.0f, foe->GetRelativeAngle(beast));
            beast->NearTeleportTo(spot.GetPositionX(), spot.GetPositionY(), spot.GetPositionZ(), beast->GetAbsoluteAngle(foe));
            // The realm's player-safe taunt: ATTACK_ME + FEAR (Compel's shape).
            beast->CastSpell(foe, SPELL_GUARDIAN_TAUNT, TRIGGERED_FULL_MASK);
            if (beast->IsAIEnabled())
                beast->AI()->AttackStart(foe);
        }, std::chrono::milliseconds(1));
        return;
    }
}

uint32 ModifyMarkedPreyDamage(Unit* attacker, Unit* victim, uint32 damage)
{
    // Only a unit wearing a stalking aura (Hunter's Mark) is looked at.
    if (!damage || !attacker || !victim || !victim->HasAuraType(SPELL_AURA_MOD_STALKED))
        return damage;
    Unit* dealer = attacker->GetCharmerOrOwnerOrSelf();
    for (AuraEffect const* stalk : victim->GetAuraEffectsByType(SPELL_AURA_MOD_STALKED))
    {
        if (!Is(stalk->GetSpellInfo(), SPELLFAMILY_HUNTER, "Hunter's Mark"))
            continue;
        Unit* hunter = stalk->GetCaster();
        if (!hunter || !Holds(hunter, SPELL_MARKED_PREY))
            continue;
        if (dealer == hunter || (hunter->IsInMap(dealer) && hunter->IsValidAssistTarget(dealer)))
            return damage + CalculatePct(damage, MARKED_PREY_BONUS_PCT);
    }
    return damage;
}

bool LifeTapIsFree(Unit const* caster)
{
    return caster && caster->GetClass() == CLASS_WARLOCK && Holds(caster, SPELL_BOTTOMLESS_PACT);
}

Unit* GetLeechHealRecipient(Unit* caster, SpellInfo const* spellInfo)
{
    if (!caster || caster->GetClass() != CLASS_WARLOCK || !Is(spellInfo, SPELLFAMILY_WARLOCK, "Drain Life")
        || !Holds(caster, SPELL_SIPHON_SOUL))
        return caster;

    Player* lowest = nullptr;
    for (Player* ally : AlliesOf(caster))
    {
        if (ally == caster || !ally->IsAlive() || ally->GetHealth() >= ally->GetMaxHealth()
            || !ally->IsWithinDistInMap(caster, SIPHON_SOUL_RANGE))
            continue;
        if (!lowest || uint64(ally->GetHealth()) * lowest->GetMaxHealth() < uint64(lowest->GetHealth()) * ally->GetMaxHealth())
            lowest = ally;
    }
    return lowest ? static_cast<Unit*>(lowest) : caster;
}

void SyncCommand(Player* warrior)
{
    if (!warrior || !warrior->IsInWorld())
        return;
    uint32 wanted = 0;
    if (warrior->IsAlive() && Holds(warrior, SPELL_COMMANDER))
    {
        switch (warrior->GetShapeshiftForm())
        {
            case FORM_BATTLESTANCE:    wanted = SPELL_COMMAND_BATTLE;    break;
            case FORM_DEFENSIVESTANCE: wanted = SPELL_COMMAND_DEFENSIVE; break;
            case FORM_BERSERKERSTANCE: wanted = SPELL_COMMAND_BERSERKER; break;
            default: break;
        }
    }
    for (uint32 command : { SPELL_COMMAND_BATTLE, SPELL_COMMAND_DEFENSIVE, SPELL_COMMAND_BERSERKER })
        if (command != wanted && warrior->HasAura(command))
            warrior->RemoveAurasDueToSpell(command);
    // A 30 yd party area aura from the warrior: the aura engine adds and
    // drops allies as they come and go.
    if (wanted && !warrior->HasAura(wanted))
        warrior->CastSpell(warrior, wanted, TRIGGERED_FULL_MASK);
}

// ---- the seventh block (2026-11-18) ------------------------------------------

void OnMeleeStruck(Unit* attacker, Unit* victim)
{
    // Every melee swing on the server passes here: the cheap tests first.
    if (!attacker || !victim || attacker == victim || victim->GetTypeId() != TYPEID_PLAYER
        || victim->GetClass() != CLASS_MAGE || !attacker->IsAlive() || !Holds(victim, SPELL_ICE_FORTRESS))
        return;
    bool armored = false;
    for (AuraEffect const* armor : victim->GetAuraEffectsByType(SPELL_AURA_MOD_RESISTANCE))
        if (Is(armor->GetSpellInfo(), SPELLFAMILY_MAGE, "Ice Armor"))
            armored = true;
    if (!armored || !victim->IsValidAttackTarget(attacker))
        return;

    uint32 const now = GameTime::GetGameTimeMS();
    auto const key = std::make_pair(victim->GetGUID(), attacker->GetGUID());
    auto itr = IceFortressCooldowns.find(key);
    if (itr != IceFortressCooldowns.end() && int32(now - itr->second) < 0)
        return;
    if (IceFortressCooldowns.size() > 512)   // forget long-expired pairs now and then
        for (auto it = IceFortressCooldowns.begin(); it != IceFortressCooldowns.end();)
            it = int32(now - it->second) >= 0 ? IceFortressCooldowns.erase(it) : std::next(it);
    IceFortressCooldowns[key] = now + ICE_FORTRESS_COOLDOWN_MS;

    // Not in the middle of the swing: one event on.
    Player* mage = victim->ToPlayer();
    ObjectGuid const foeGuid = attacker->GetGUID();
    mage->m_Events.AddEventAtOffset([mage, foeGuid]()
    {
        Unit* foe = ObjectAccessor::GetUnit(*mage, foeGuid);
        if (foe && foe->IsAlive() && foe->IsInMap(mage) && mage->IsValidAttackTarget(foe))
            mage->CastSpell(foe, SPELL_ICE_FORTRESS_FREEZE, TRIGGERED_FULL_MASK);
    }, std::chrono::milliseconds(1));
}

bool TrySoulBond(Unit* victim)
{
    Player* ally = victim ? victim->ToPlayer() : nullptr;
    if (!ally || !ally->IsAlive())
        return false;
    Battleground* bg = ally->GetBattleground();
    if (!bg || bg->GetTypeID() != BATTLEGROUND_VHR)
        return false;

    for (Player* lock : AlliesOf(ally))
    {
        if (lock->GetClass() != CLASS_WARLOCK || !lock->IsAlive() || !Holds(lock, SPELL_SOUL_BOND)
            || !lock->IsWithinDistInMap(ally, SOUL_BOND_RANGE))
            continue;
        Pet* demon = lock->GetPet();
        if (!demon || !demon->IsAlive() || SoulBondSpent.count(demon->GetGUID()))
            continue;

        ObjectGuid const demonGuid = demon->GetGUID();
        SoulBondSpent.insert(demonGuid);
        // The demon dies after this damage pass, not inside it (OnUnitDying's
        // Eternal Pact may then bring it back).
        lock->m_Events.AddEventAtOffset([lock, demonGuid]()
        {
            SoulBondSpent.erase(demonGuid);
            Pet* bonded = lock->GetPet();
            if (bonded && bonded->GetGUID() == demonGuid && bonded->IsAlive())
                bonded->KillSelf(false);
        }, std::chrono::milliseconds(1));
        return true;
    }
    return false;
}

// ---- the eighth block (2026-11-18) -------------------------------------------

bool AbsorbsEverySchool(AuraEffect const* absorb)
{
    if (!absorb)
        return false;
    SpellInfo const* ward = absorb->GetSpellInfo();
    if (!Is(ward, SPELLFAMILY_MAGE, "Fire Ward") && !Is(ward, SPELLFAMILY_MAGE, "Frost Ward"))
        return false;
    return Holds(absorb->GetCaster(), SPELL_ELEMENTAL_WARD);
}

bool MakesAuraEndless(Unit const* caster, SpellInfo const* spellInfo)
{
    if (!caster || !spellInfo || caster->GetClass() != CLASS_DRUID || spellInfo->SpellFamilyName != SPELLFAMILY_DRUID)
        return false;
    return (NameIs(spellInfo, "Rip") || NameIs(spellInfo, "Rejuvenation") || NameIs(spellInfo, "Moonfire"))
        && Holds(caster, SPELL_ENDLESS_CYCLE);
}

// ---- the ninth block (2026-11-18) ----------------------------------------

bool IsWoolenPrisoner(Unit const* target)
{
    if (!target || !target->IsPolymorphed())
        return false;
    for (AuraEffect const* effect : target->GetAuraEffectsByType(SPELL_AURA_TRANSFORM))
        if (effect->GetSpellInfo()->GetSpellSpecific() == SPELL_SPECIFIC_MAGE_POLYMORPH
            && Holds(effect->GetCaster(), SPELL_WOOLEN_PRISON))
            return true;
    return false;
}

std::vector<Unit*> StormCleaveTargets(Unit* shaman, Unit* victim)
{
    if (!shaman || !victim || shaman->GetClass() != CLASS_SHAMAN || !Holds(shaman, SPELL_STORM_CLEAVE))
        return {};
    return EnemiesNear(victim, shaman, STORM_CLEAVE_RADIUS);
}

int32 ModifyShieldOrbDamage(Unit const* caster, SpellInfo const* spellInfo, int32 damage)
{
    // The orb is every rank's "Lightning Shield" SCHOOL_DAMAGE spell (26364
    // and on); the shield itself is a PROC_TRIGGER_SPELL aura and never gets here.
    if (damage <= 0 || !caster || caster->GetClass() != CLASS_SHAMAN
        || !Is(spellInfo, SPELLFAMILY_SHAMAN, "Lightning Shield") || !spellInfo->HasEffect(SPELL_EFFECT_SCHOOL_DAMAGE)
        || !Holds(caster, SPELL_SUPERCHARGED_SHIELD))
        return damage;
    return damage * SUPERCHARGED_SHIELD_FACTOR;
}

// ---- the tenth block (2026-11-18) ----------------------------------------

bool MirrorsSpells(Unit const* target)
{
    return SpellMirrorOwner(target) != nullptr;
}

void EndSpellMirror(Unit* totem, uint64 hitDelayMs)
{
    if (!SpellMirrorOwner(totem))
        return;
    totem->m_Events.AddEventAtOffset([totem]()
    {
        Totem* self = totem->ToTotem();
        if (!self || !self->IsAlive())
            return;
        // UnSummon, not a kill, so OnUnitDying does not reset it a second time.
        if (Unit* shaman = SpellMirrorOwner(self))
            ResetGroundingTotem(shaman);
        self->UnSummon();
    }, Milliseconds(hitDelayMs) + SPELL_MIRROR_LINGER);
}

void OnSpellDamageDealt(Unit* caster, Unit* victim, SpellInfo const* spellInfo)
{
    // The Searing Totem bolt, every rank: shaman family "Attack", cast by the totem.
    if (!caster || !victim || !caster->IsTotem() || !Is(spellInfo, SPELLFAMILY_SHAMAN, "Attack")
        || !(spellInfo->SpellFamilyFlags[0] & 0x40000000) || !victim->IsAlive())
        return;
    Unit* shaman = caster->GetOwner();
    if (!shaman || shaman->GetClass() != CLASS_SHAMAN || !Holds(shaman, SPELL_GUARDIAN_FLAME))
        return;
    shaman->AddAura(SPELL_GUARDIAN_FLAME_SAP, victim);
}
}
