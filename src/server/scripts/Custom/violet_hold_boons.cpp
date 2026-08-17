/*
 * Violet Hold boons - the broker NPC and the aura scripts behind the two
 * chance-based boons and the level marker. See
 * game/Miscellaneous/VioletHoldBoons.h for the design and the boon table.
 */

#include "Battleground.h"
#include "BattlegroundVHR.h"
#include "CellImpl.h"
#include "GridNotifiers.h"
#include "GridNotifiersImpl.h"
#include "Map.h"
#include "Player.h"
#include "Random.h"
#include "ScriptedCreature.h"
#include "ScriptedGossip.h"
#include "ScriptMgr.h"
#include "Spell.h"
#include "SpellAuraEffects.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "SpellScript.h"
#include "VioletHoldBoons.h"
#include "WorldSession.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

using namespace VioletHoldBoons;

namespace
{
constexpr uint32 ACTION_PICK_BASE = GOSSIP_ACTION_INFO_DEF + 100;   // + uint8(Boon)

// "%s takes the %s." - see sql/custom/world/2026_08_15_00_world_violet_hold_boons.sql
constexpr uint32 STRING_BOON_TAKEN = 20101;

// The broker's own run, or nullptr when he is somewhere he should not be.
BattlegroundVHR* GetRun(Creature const* broker)
{
    Map* map = broker->GetMap();
    BattlegroundMap* bgMap = map ? map->ToBattlegroundMap() : nullptr;
    if (!bgMap)
        return nullptr;

    Battleground* bg = bgMap->GetBG();
    if (!bg || bg->GetTypeID() != BATTLEGROUND_VHR)
        return nullptr;

    return static_cast<BattlegroundVHR*>(bg);
}

// Only the party may trade with him: the clones are Players too, and on the
// other team.
bool MayTrade(Player const* player, BattlegroundVHR const* run)
{
    if (!player || !run)
        return false;

    WorldSession const* session = player->GetSession();
    if (!session || session->IsTransientPlayerSession())
        return false;

    if (player->GetBattleground() != run)
        return false;

    return player->GetBGTeam() == run->GetHumanTeam();
}
}

class npc_violet_hold_boon_broker : public CreatureScript
{
public:
    npc_violet_hold_boon_broker() : CreatureScript("npc_violet_hold_boon_broker") { }

    struct npc_violet_hold_boon_brokerAI : public ScriptedAI
    {
        npc_violet_hold_boon_brokerAI(Creature* creature) : ScriptedAI(creature), _rolled(false), _consumed(false) { }

        void Reset() override
        {
            me->SetReactState(REACT_PASSIVE);
        }

        bool OnGossipHello(Player* player) override
        {
            BattlegroundVHR* run = GetRun(me);
            if (!MayTrade(player, run) || _consumed)
            {
                CloseGossipMenuFor(player);
                return true;
            }

            // The board minus whatever has gone dead since it was rolled: a
            // unique boon somebody claimed, a class spell its only owner
            // learned, a legendary already handed out. Lines that merely do
            // not fit THIS reader stay - they are still somebody's - and
            // DescribeOffer labels them.
            std::vector<Offer> const offers = LiveOffersFor(run);
            if (offers.empty())
            {
                // Nothing here helps anyone any more. This broker can never be
                // consumed by a pick, and his slot is one of a fixed few, so
                // retire him now and let the next wave put a fresh one out.
                if (WorldSession* session = player->GetSession())
                    session->SendNotification("The broker has nothing left that would help anyone here.");
                CloseGossipMenuFor(player);
                _consumed = true;
                // Frees the slot and removes the creature at the end of the
                // update. Nothing below may touch `me` after this.
                run->ConsumeBoonBroker(me->GetGUID());
                return true;
            }

            ClearGossipMenuFor(player);
            for (Offer const& offer : offers)
                AddGossipItemFor(player, GOSSIP_ICON_CHAT, DescribeOffer(player, offer),
                    GOSSIP_SENDER_MAIN, ACTION_PICK_BASE + offer.Encode());

            SendGossipMenuFor(player, NPC_TEXT_BOON_BROKER, me->GetGUID());
            return true;
        }

        bool OnGossipSelect(Player* player, uint32 /*menuId*/, uint32 gossipListId) override
        {
            uint32 const action = player->PlayerTalkClass->GetGossipOptionAction(gossipListId);
            CloseGossipMenuFor(player);

            BattlegroundVHR* run = GetRun(me);
            if (!MayTrade(player, run) || _consumed)
                return true;

            Offer offer;
            if (action < ACTION_PICK_BASE || !Offer::Decode(action - ACTION_PICK_BASE, offer))
                return true;

            // The pick has to be one of the three this broker is actually
            // showing, so a hand-crafted gossip packet cannot shop the whole list.
            std::vector<Offer> const& offers = OffersFor(run);
            if (std::find(offers.begin(), offers.end(), offer) == offers.end())
                return true;

            PickResult const result = Take(player, offer);
            if (result != PickResult::Ok)
            {
                // Wrong class, already known, capped: the broker stays for
                // whoever it does fit. Nothing was changed.
                if (WorldSession* session = player->GetSession())
                    session->SendNotification("%s", GetPickResultText(result));
                return true;
            }

            _consumed = true;

            char const* name = offer.kind == Offer::Kind::Boon ? GetBoon(Boon(offer.index)).name
                             : offer.kind == Offer::Kind::ClassSpell ? GetClassSpell(offer.index).name
                             : GetItemGrant(offer.index).name;
            if (WorldSession* session = player->GetSession())
                session->SendNotification("You receive %s.", name);
            run->PSendMessageToAll(STRING_BOON_TAKEN, CHAT_MSG_BG_SYSTEM_NEUTRAL, nullptr, player->GetName().c_str(), name);

            // Frees the slot and removes the creature at the end of the update.
            // Nothing below may touch `me` after this.
            run->ConsumeBoonBroker(me->GetGUID());
            return true;
        }

    private:
        // Rolled once, on the first visit, against the party as it stands then,
        // and shown unchanged to everyone after - closing and reopening the
        // window is not a reroll, and a hunter ability stays on the board for
        // the hunter even if a mage looked first.
        // The cached board, filtered to what at least one member of the team
        // could still take. Recomputed on every visit: the roll is fixed, but
        // whether an offer is still worth anything is not.
        std::vector<Offer> LiveOffersFor(BattlegroundVHR const* run)
        {
            std::vector<Player const*> roster;
            run->CollectHumanPlayers(roster);

            std::vector<Offer> live;
            for (Offer const& offer : OffersFor(run))
                for (Player const* member : roster)
                    if (member && CanTake(member, offer) == PickResult::Ok)
                    {
                        live.push_back(offer);
                        break;
                    }

            return live;
        }

        std::vector<Offer> const& OffersFor(BattlegroundVHR const* run)
        {
            if (!_rolled)
            {
                std::vector<Player const*> roster;
                run->CollectHumanPlayers(roster);
                // Boon of Greed widens every broker rolled after it.
                _offers = RollOffers(roster, uint8(std::min<uint32>(OFFERS_PER_BROKER + run->GetBrokerBonusOffers(), 12)));
                _rolled = true;
            }
            return _offers;
        }

        std::vector<Offer> _offers;
        bool _rolled;
        bool _consumed;
    };

    CreatureAI* GetAI(Creature* creature) const override
    {
        return new npc_violet_hold_boon_brokerAI(creature);
    }
};

// Boon of Flurry (90238): SPELL_AURA_PROC_TRIGGER_SPELL firing 90250 (add one
// extra attack) at 1% per stack. spell_proc gives it melee-only proc flags
// at 100% and this script supplies the real chance from the stacked amount.
class spell_vhr_boon_flurry : public AuraScript
{
    PrepareAuraScript(spell_vhr_boon_flurry);

    bool CheckProc(ProcEventInfo& /*eventInfo*/)
    {
        AuraEffect const* eff = GetEffect(EFFECT_0);
        return eff && roll_chance_i(eff->GetAmount());
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_vhr_boon_flurry::CheckProc);
    }
};

// Boon of Echoes (90239): a dummy with cast-phase proc flags for damage-class
// magic/none spells. When it fires, the spell that was just cast is cast again
// at the same targets, free and instant.
class spell_vhr_boon_echoes : public AuraScript
{
    PrepareAuraScript(spell_vhr_boon_echoes);

    static bool IsDamageOrHeal(SpellInfo const* info)
    {
        for (SpellEffectInfo const& effect : info->GetEffects())
        {
            if (!effect.IsEffect())
                continue;

            switch (effect.Effect)
            {
                case SPELL_EFFECT_SCHOOL_DAMAGE:
                case SPELL_EFFECT_HEALTH_LEECH:
                case SPELL_EFFECT_HEAL:
                case SPELL_EFFECT_HEAL_PCT:
                case SPELL_EFFECT_HEAL_MECHANICAL:
                    return true;
                case SPELL_EFFECT_APPLY_AURA:
                case SPELL_EFFECT_APPLY_AREA_AURA_PARTY:
                case SPELL_EFFECT_APPLY_AREA_AURA_RAID:
                    switch (effect.ApplyAuraName)
                    {
                        case SPELL_AURA_PERIODIC_DAMAGE:
                        case SPELL_AURA_PERIODIC_DAMAGE_PERCENT:
                        case SPELL_AURA_PERIODIC_HEAL:
                        case SPELL_AURA_PERIODIC_LEECH:
                        case SPELL_AURA_OBS_MOD_HEALTH:
                            return true;
                        default:
                            break;
                    }
                    break;
                default:
                    break;
            }
        }
        return false;
    }

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        Spell const* spell = eventInfo.GetProcSpell();
        SpellInfo const* info = eventInfo.GetSpellInfo();
        if (!spell || !info)
            return false;

        // The echo itself is a triggered cast; never echo an echo, and leave
        // every other triggered cast (procs) alone too. Item uses (potions,
        // healthstones, explosives) are NOT triggered casts and would otherwise
        // be doubled for free - out as well. A consumed charge has already
        // nulled m_CastItem by the time the cast-phase proc runs (TakeCastItem
        // precedes it), so the reliable test is the spellbook: only a spell
        // the caster actually knows may echo.
        if (spell->IsTriggered() || spell->m_CastItem || spell->m_castItemGUID)
            return false;

        Player const* casterPlayer = GetTarget() ? GetTarget()->ToPlayer() : nullptr;
        if (!casterPlayer || !casterPlayer->HasActiveSpell(info->Id))
            return false;

        if (info->IsChanneled() || info->IsPassive() || info->NeedsComboPoints())
            return false;

        if (IsBoonSpell(info->Id))
            return false;

        if (!IsDamageOrHeal(info))
            return false;

        AuraEffect const* eff = GetEffect(EFFECT_0);
        return eff && roll_chance_i(eff->GetAmount());
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        Spell const* spell = eventInfo.GetProcSpell();
        Unit* caster = GetTarget();
        if (!spell || !caster)
            return;

        SpellCastTargets targets = spell->m_targets;
        caster->CastSpell(std::move(targets), spell->GetSpellInfo()->Id, TRIGGERED_FULL_MASK);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_vhr_boon_echoes::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_vhr_boon_echoes::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// Boon of Ascension (90247): the marker whose base amount is the level to go
// back to. VioletHoldBoons::StripAll does the rollback deliberately, before
// removing this; this is the safety net for the marker vanishing any other
// way while the character is in the world (a GM .unaura, a spell reset).
// A LOGOUT must not trigger it - the levels have to survive a relog into the
// run - and in this fork logout cleanup removes auras BEFORE RemoveFromWorld
// (Unit::CleanupBeforeRemoveFromMap -> DefensiveCleanupAurasBeforeDelete),
// after the character was already saved, so IsInWorld alone is not enough:
// the session's logout flag is what says "this is a logout".
class spell_vhr_boon_ascension : public AuraScript
{
    PrepareAuraScript(spell_vhr_boon_ascension);

    void HandleRemove(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget() ? GetTarget()->ToPlayer() : nullptr;
        if (!player || !player->IsInWorld())
            return;

        WorldSession const* session = player->GetSession();
        if (!session || session->PlayerLogout())
            return;

        int32 const baseLevel = aurEff->GetBaseAmount();
        if (baseLevel > 0 && baseLevel < int32(player->GetLevel()))
            RollbackBorrowedLevels(player, uint8(baseLevel));
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(spell_vhr_boon_ascension::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

// Boon Broker's Cache (90257): each effect's base amount is the low guid of a
// weapon the broker handed out. Same safety net as the Ascension marker: if
// the aura goes any way other than StripAll while the character is in the
// world (and not logging out), the weapon goes with it - otherwise a stray
// `.unaura all` would leave someone a permanent Thunderfury.
class spell_vhr_boon_cache : public AuraScript
{
    PrepareAuraScript(spell_vhr_boon_cache);

    void HandleRemove(AuraEffect const* aurEff, AuraEffectHandleModes /*mode*/)
    {
        Player* player = GetTarget() ? GetTarget()->ToPlayer() : nullptr;
        if (!player || !player->IsInWorld())
            return;

        WorldSession const* session = player->GetSession();
        if (!session || session->PlayerLogout())
            return;

        DestroyGrantedItemByGuidLow(player, uint32(aurEff->GetBaseAmount()));
    }

    void Register() override
    {
        AfterEffectRemove += AuraEffectRemoveFn(spell_vhr_boon_cache::HandleRemove, EFFECT_0, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_vhr_boon_cache::HandleRemove, EFFECT_1, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
        AfterEffectRemove += AuraEffectRemoveFn(spell_vhr_boon_cache::HandleRemove, EFFECT_2, SPELL_AURA_DUMMY, AURA_EFFECT_HANDLE_REAL);
    }
};

namespace
{
// Living hostile units within `radius` of `around` that `attacker` may hit,
// excluding `around`. Same rule as the game-lib Overkill splash.
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

// The proc-fired spells never feed the procs again (they carry
// CANT_TRIGGER_PROC too; this is the belt to that brace).
bool IsBoonTriggeredSpell(SpellInfo const* spellInfo)
{
    if (!spellInfo)
        return false;
    switch (spellInfo->Id)
    {
        case SPELL_RICOCHET_DAMAGE:
        case SPELL_OVERKILL_DAMAGE:
        case SPELL_REFLECTION_DAMAGE:
        case SPELL_VAMPIRE_HEAL:
        case SPELL_BOON_EXTRA_ATTACK_SWING:
            return true;
        default:
            return false;
    }
}

uint32 ProcDamage(ProcEventInfo const& eventInfo)
{
    DamageInfo const* damageInfo = eventInfo.GetDamageInfo();
    return damageInfo ? damageInfo->GetDamage() : 0;
}
}

// 90288 Boon of Ricochet: stacks% chance for a damaging hit to leap to the
// nearest other enemy within RICOCHET_RADIUS of the victim for half.
class spell_vhr_boon_ricochet : public AuraScript
{
    PrepareAuraScript(spell_vhr_boon_ricochet);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        if (IsBoonTriggeredSpell(eventInfo.GetSpellInfo()))
            return false;
        // The boons are death-persistent and procs run after Kill(): a hit
        // that lands on a holder's corpse must not fire from it.
        if (!GetTarget()->IsAlive())
            return false;
        if (!ProcDamage(eventInfo) || !eventInfo.GetProcTarget())
            return false;
        return roll_chance_i(GetStackAmount());
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        Unit* caster = GetTarget();
        Unit* victim = eventInfo.GetProcTarget();
        uint32 const damage = ProcDamage(eventInfo);
        if (!caster || !victim || !damage)
            return;

        Unit* next = nullptr;
        float best = 0.0f;
        for (Unit* candidate : HostilesAround(victim, caster, RICOCHET_RADIUS))
        {
            float const dist = victim->GetExactDist2d(candidate);
            if (!next || dist < best)
            {
                next = candidate;
                best = dist;
            }
        }
        if (!next)
            return;

        CastSpellExtraArgs args(TRIGGERED_FULL_MASK);
        args.AddSpellBP0(int32(std::max<uint32>(1, damage / 2)));
        caster->CastSpell(next, SPELL_RICOCHET_DAMAGE, args);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_vhr_boon_ricochet::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_vhr_boon_ricochet::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// 90290 Boon of Reflection: stacks% of any damage taken (physical or magic,
// direct or periodic - PROC_FLAG_TAKEN_DAMAGE) thrown back at the attacker.
class spell_vhr_boon_reflection : public AuraScript
{
    PrepareAuraScript(spell_vhr_boon_reflection);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        if (IsBoonTriggeredSpell(eventInfo.GetSpellInfo()))
            return false;
        Unit* attacker = eventInfo.GetActor();
        return GetTarget()->IsAlive() && attacker && attacker != GetTarget() && attacker->IsAlive() && ProcDamage(eventInfo);
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        Unit* attacker = eventInfo.GetActor();
        uint32 const reflected = CalculatePct(ProcDamage(eventInfo), uint32(GetStackAmount()));
        if (!attacker || !reflected)
            return;

        CastSpellExtraArgs args(TRIGGERED_FULL_MASK);
        args.AddSpellBP0(int32(reflected));
        GetTarget()->CastSpell(attacker, SPELL_REFLECTION_DAMAGE, args);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_vhr_boon_reflection::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_vhr_boon_reflection::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

// 90291 Boon of the Vampire: stacks% of damage dealt returned as health.
class spell_vhr_boon_vampire : public AuraScript
{
    PrepareAuraScript(spell_vhr_boon_vampire);

    bool CheckProc(ProcEventInfo& eventInfo)
    {
        if (IsBoonTriggeredSpell(eventInfo.GetSpellInfo()))
            return false;
        return GetTarget()->IsAlive() && ProcDamage(eventInfo) > 0;
    }

    void HandleProc(AuraEffect const* /*aurEff*/, ProcEventInfo& eventInfo)
    {
        PreventDefaultAction();

        uint32 const heal = CalculatePct(ProcDamage(eventInfo), uint32(GetStackAmount()));
        if (!heal)
            return;

        CastSpellExtraArgs args(TRIGGERED_FULL_MASK);
        args.AddSpellBP0(int32(heal));
        GetTarget()->CastSpell(GetTarget(), SPELL_VAMPIRE_HEAL, args);
    }

    void Register() override
    {
        DoCheckProc += AuraCheckProcFn(spell_vhr_boon_vampire::CheckProc);
        OnEffectProc += AuraEffectProcFn(spell_vhr_boon_vampire::HandleProc, EFFECT_0, SPELL_AURA_DUMMY);
    }
};

void AddSC_violet_hold_boons()
{
    new npc_violet_hold_boon_broker();
    RegisterSpellScript(spell_vhr_boon_flurry);
    RegisterSpellScript(spell_vhr_boon_echoes);
    RegisterSpellScript(spell_vhr_boon_ascension);
    RegisterSpellScript(spell_vhr_boon_cache);
    RegisterSpellScript(spell_vhr_boon_ricochet);
    RegisterSpellScript(spell_vhr_boon_reflection);
    RegisterSpellScript(spell_vhr_boon_vampire);
}
