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

// The CENTURION_Rift addon's requests, answered by RiftProgression (game/):
//   CCGAMEREQ\tRIFTPICK:<spellId>   learn or rank up that spell with a skill pick
//   CCGAMEREQ\tRIFTSTATE:           send the match state again (after a /reload)
//   CCGAMEREQ\tRIFTBUY:<entry>      buy that shop item, at the fountain (League's shop
//                                   lists more items than a 3.3.5 vendor can)
//
// League's lane minions (npc_rift_minion, creature_template 922110-922117) never
// evade. The stock AI evades the moment its target dies: it turns immune, walks
// back to where the fight started and heals to full there - so waves ran back
// up their lane after every kill, never died, and shrugged off turret shots.
// A minion that runs out of enemies stops where it stands instead, and
// BattlegroundSRF picks its next target or walks it on down its lane.
//
// And no threat, League's way: a minion fights the target the battleground
// names (SetGUID) - the closest enemy - and nothing else moves it. Being hit,
// seeing someone, or whoever did the most damage play no part.
//
// And League's recall: spell 94120 "Recall", taught for the match
// (RiftProgression), an 8 s cast that works in combat and breaks when the caster
// moves or is hit, takes its caster to its own fountain. On Summoner's Rift the
// Hearthstone and a shaman's Astral Recall do the same instead of going home,
// and are ready again at once.

#include "AddonRequestRoutes.h"
#include "Battleground.h"
#include "BattlegroundSRF.h"
#include "Chat.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "MotionMaster.h"
#include "ObjectAccessor.h"
#include "DBCStores.h"
#include "Map.h"
#include "Player.h"
#include "RiftLeague.h"
#include "RiftProgression.h"
#include "ScriptMgr.h"
#include "ScriptedCreature.h"
#include "ScriptedGossip.h"
#include "Spell.h"
#include "SpellHistory.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellScript.h"
#include "ThreatManager.h"
#include "Containers.h"
#include <unordered_map>
#include <cmath>

namespace
{
    Battleground* RiftOf(Unit* caster)
    {
        Player* player = caster ? caster->ToPlayer() : nullptr;
        Battleground* bg = player ? player->GetBattleground() : nullptr;
        return bg && bg->GetTypeID(true) == BATTLEGROUND_SRF && player->GetMapId() == bg->GetMapId() ? bg : nullptr;
    }
}

// BattlegroundSRF's entries: turrets, inhibitors and nexuses; the lane minions.
uint32 constexpr RIFT_STRUCTURE_FIRST = 922100;
uint32 constexpr RIFT_STRUCTURE_LAST = 922105;
// League's minion kind by entry (rift_league_unit): Order 922110 melee, 922111
// caster, 922114 siege, 922115 super; Chaos 922112, 922113, 922116, 922117.
char const* RiftMinionKind(uint32 entry)
{
    switch (entry)
    {
        case 922111: case 922113: return "caster";
        case 922114: case 922116: return "siege";
        case 922115: case 922117: return "super";
        default:                  return "melee";
    }
}

// SetGUID id: a caster keeps its own distance (BattlegroundSRF moves it).
int32 constexpr RIFT_MINION_TARGET_MELEE = 0;
int32 constexpr RIFT_MINION_TARGET_RANGED = 1;

struct npc_rift_minion : public CreatureAI
{
    explicit npc_rift_minion(Creature* creature) : CreatureAI(creature) { }

    // Only the battleground picks a target.
    void AttackStart(Unit* /*who*/) override { }
    void MoveInLineOfSight(Unit* /*who*/) override { }

    void SetGUID(ObjectGuid const& guid, int32 id) override
    {
        _target = guid;
        Unit* victim = guid.IsEmpty() ? nullptr : ObjectAccessor::GetUnit(*me, guid);
        if (!victim)
        {
            me->AttackStop();
            return;
        }
        if (me->GetVictim() == victim)
            return;
        me->Attack(victim, true);
        if (id == RIFT_MINION_TARGET_MELEE)
            me->GetMotionMaster()->MoveChase(victim);
    }

    void UpdateAI(uint32 /*diff*/) override
    {
        if (_target.IsEmpty())
            return;
        Unit* victim = ObjectAccessor::GetUnit(*me, _target);
        if (!victim || !victim->IsAlive() || !me->IsValidAttackTarget(victim))
        {
            _target.Clear();
            me->AttackStop();
            return;
        }
        if (me->GetVictim() != victim)
            me->Attack(victim, true);
        DoMeleeAttackIfReady();
    }

    // League's minion damage (world rift_league_unit): its share against
    // champions (Season 3: 15% less) and against structures (50% more), full
    // against minions, monsters and pets. Its melee and its bolt alike.
    void DamageDealt(Unit* victim, uint32& damage, DamageEffectType /*damageType*/) override
    {
        uint32 const entry = victim->GetTypeId() == TYPEID_UNIT ? victim->GetEntry() : 0;
        bool const champion = victim->GetTypeId() == TYPEID_PLAYER;
        bool const structure = entry >= RIFT_STRUCTURE_FIRST && entry <= RIFT_STRUCTURE_LAST;
        if (!champion && !structure)
            return;
        std::shared_ptr<RiftLeague::Tables const> const tables = RiftLeague::Get();
        if (RiftLeague::UnitStats const* unit = RiftLeague::FindUnit(*tables, RiftMinionKind(me->GetEntry())))
            damage = uint32(std::lround(float(damage) * (champion ? unit->DamageVsChampions : unit->DamageVsStructures)));
    }

    void EnterEvadeMode(EvadeReason /*why*/) override
    {
        _target.Clear();
        me->CombatStop(true);
        me->SetTarget(ObjectGuid::Empty);
        me->GetMotionMaster()->Clear();
        me->StopMoving();
    }

private:
    ObjectGuid _target;
};

// 8690 Hearthstone, 556 Astral Recall
class spell_rift_recall : public SpellScript
{
    PrepareSpellScript(spell_rift_recall);

    void HandleTeleport(SpellEffIndex effIndex)
    {
        Battleground* bg = RiftOf(GetCaster());
        if (!bg)
            return;
        PreventHitDefaultEffect(effIndex);
        Player* player = GetCaster()->ToPlayer();
        if (WorldSafeLocsEntry const* fountain = bg->GetClosestGraveyard(player))
            player->NearTeleportTo(fountain->Loc.X, fountain->Loc.Y, fountain->Loc.Z, player->GetOrientation());
    }

    void HandleAfterCast()
    {
        if (RiftOf(GetCaster()))
            GetCaster()->GetSpellHistory()->ResetCooldown(GetSpellInfo()->Id, true);
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_rift_recall::HandleTeleport, EFFECT_0, SPELL_EFFECT_TELEPORT_UNITS);
        AfterCast += SpellCastFn(spell_rift_recall::HandleAfterCast);
    }
};

// 94120 Recall
class spell_rift_recall_league : public SpellScript
{
    PrepareSpellScript(spell_rift_recall_league);

    void HandleDummy(SpellEffIndex /*effIndex*/)
    {
        Battleground* bg = RiftOf(GetCaster());
        if (!bg)
            return;
        Player* player = GetCaster()->ToPlayer();
        if (WorldSafeLocsEntry const* fountain = bg->GetClosestGraveyard(player))
            player->NearTeleportTo(fountain->Loc.X, fountain->Loc.Y, fountain->Loc.Z, player->GetOrientation());
    }

    void Register() override
    {
        OnEffectHitTarget += SpellEffectFn(spell_rift_recall_league::HandleDummy, EFFECT_0, SPELL_EFFECT_DUMMY);
    }
};

// League's kill credit and assists (BattlegroundSRF::RecordChampionContribution):
// a champion's damage on an enemy champion - its pet's and totems' too - and any
// harmful ability it casts at one.
namespace
{
    BattlegroundSRF* RiftOfVictim(Unit* victim)
    {
        Player* player = victim ? victim->ToPlayer() : nullptr;
        Battleground* bg = player ? player->GetBattleground() : nullptr;
        return bg && bg->GetTypeID(true) == BATTLEGROUND_SRF && player->GetMapId() == bg->GetMapId() ? static_cast<BattlegroundSRF*>(bg) : nullptr;
    }
}

class custom_rift_contribution_unit : public UnitScript
{
public:
    custom_rift_contribution_unit() : UnitScript("custom_rift_contribution_unit") { }

    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        if (!damage || !attacker || !victim || victim->GetTypeId() != TYPEID_PLAYER)
            return;
        Player* champion = attacker->GetCharmerOrOwnerPlayerOrPlayerItself();
        if (!champion || champion == victim)
            return;
        if (BattlegroundSRF* bg = RiftOfVictim(victim))
            bg->RecordChampionContribution(champion, victim->ToPlayer());
    }
};

class custom_rift_contribution_player : public PlayerScript
{
public:
    custom_rift_contribution_player() : PlayerScript("custom_rift_contribution_player") { }

    void OnSpellCast(Player* player, Spell* spell, bool /*skipCheck*/) override
    {
        Unit* target = spell ? spell->m_targets.GetUnitTarget() : nullptr;
        if (!target || target == player || !player->IsHostileTo(target))
            return;
        // A harmful spell at any enemy - a minion, a monster, a turret - shows a
        // champion casting it from the brush.
        if (BattlegroundSRF* bg = RiftOfVictim(player))
            bg->NoteBrushAttack(player);
        if (target->GetTypeId() != TYPEID_PLAYER)
            return;
        if (BattlegroundSRF* bg = RiftOfVictim(target))
            bg->RecordChampionContribution(player, target->ToPlayer());
    }
};

// League's brush: a champion's own blow or spell landing on anything it fights
// shows it for a moment (BattlegroundSRF::NoteBrushAttack). Not a damage-over-time
// tick, and not its pet's work: League's brush shows the one who attacks.
class custom_rift_brush_unit : public UnitScript
{
public:
    custom_rift_brush_unit() : UnitScript("custom_rift_brush_unit") { }

    void OnDamage(Unit* attacker, Unit* victim, uint32& damage, DamageEffectType damagetype, CleanDamage const* cleanDamage) override
    {
        if (!attacker || attacker == victim || attacker->GetTypeId() != TYPEID_PLAYER)
            return;
        if (damagetype != DIRECT_DAMAGE && damagetype != SPELL_DIRECT_DAMAGE)
            return;
        if (!damage && !(cleanDamage && cleanDamage->absorbed_damage))
            return;
        if (BattlegroundSRF* bg = RiftOfVictim(attacker))
            bg->NoteBrushAttack(attacker->ToPlayer());
    }
};

// The Rift's mounts - the shop's two steeds and the Tauren's Plainsrunning - come
// off at a direct hit from anyone else (a champion, a minion, a turret, a monster,
// the enemy fountain; a blow the shields took whole too), a mount being cast is
// broken off, and a mount waits 5 sec before it can be ridden again: from the
// moment it is mounted, and from the hit that knocked the champion off (owner,
// 2026-10-23: "getting hit for any reason should dismount and mounting should have
// a 5 second cooldown (summoner's rift). plainstriding needs to be considered too";
// then "it should only dismount on direct hits").
namespace
{
    // 94150 Summoner's Steed, 94151 Swift Summoner's Steed, 89153 Plainsrunning
    uint32 constexpr RIFT_MOUNT_SPELLS[] = { 94150, 94151, 89153 };
    std::chrono::seconds constexpr RIFT_MOUNT_COOLDOWN(5);

    bool IsRiftMountSpell(uint32 spellId)
    {
        return std::find(std::begin(RIFT_MOUNT_SPELLS), std::end(RIFT_MOUNT_SPELLS), spellId) != std::end(RIFT_MOUNT_SPELLS);
    }

    // All three at once, so a Tauren knocked off a steed cannot run off on Plainsrunning.
    void StartRiftMountCooldown(Player* player)
    {
        std::vector<uint32> spells(std::begin(RIFT_MOUNT_SPELLS), std::end(RIFT_MOUNT_SPELLS));
        for (uint32 spell : spells)
            player->GetSpellHistory()->AddCooldown(spell, 0, RIFT_MOUNT_COOLDOWN);
        player->GetSpellHistory()->SendCooldowns(spells);
    }
}

class custom_rift_mount_unit : public UnitScript
{
public:
    custom_rift_mount_unit() : UnitScript("custom_rift_mount_unit") { }

    void OnDamage(Unit* attacker, Unit* victim, uint32& damage, DamageEffectType damagetype, CleanDamage const* cleanDamage) override
    {
        // A direct hit: a weapon blow or a spell's own impact from someone else that
        // landed, all of it on the shields included. Not a damage-over-time tick, a
        // miss, a fall, or anything the champion did to itself.
        if (!attacker || attacker == victim)
            return;
        if (damagetype != DIRECT_DAMAGE && damagetype != SPELL_DIRECT_DAMAGE)
            return;
        if (!damage && !(cleanDamage && cleanDamage->absorbed_damage))
            return;
        if (!RiftOfVictim(victim))
            return;
        Player* player = victim->ToPlayer();
        bool knocked = false;
        if (player->HasAuraType(SPELL_AURA_MOUNTED))
        {
            player->RemoveAurasByType(SPELL_AURA_MOUNTED);
            knocked = true;
        }
        if (Spell const* spell = player->GetCurrentSpell(CURRENT_GENERIC_SPELL); spell && IsRiftMountSpell(spell->GetSpellInfo()->Id))
        {
            player->InterruptSpell(CURRENT_GENERIC_SPELL, false);
            knocked = true;
        }
        if (knocked)
            StartRiftMountCooldown(player);
    }
};

class custom_rift_mount_player : public PlayerScript
{
public:
    custom_rift_mount_player() : PlayerScript("custom_rift_mount_player") { }

    // This hook runs as the cast bar completes but BEFORE the cast's last check
    // (Spell::_cast), so a cooldown put on the mounts here refused the very cast that
    // set it: "ability isn't ready yet" at the end of every mount's cast bar. The
    // cooldown starts once the cast has mounted the champion, on its next update.
    void OnSpellCast(Player* player, Spell* spell, bool /*skipCheck*/) override
    {
        if (!spell || !IsRiftMountSpell(spell->GetSpellInfo()->Id) || !RiftOf(player))
            return;
        uint32 const mount = spell->GetSpellInfo()->Id;
        player->m_Events.AddEventAtOffset([player, mount]()
        {
            if (RiftOf(player) && player->HasAura(mount))
                StartRiftMountCooldown(player);
        }, 1ms);
    }
};

// The armsmaster at each fountain (922152 Order, 922153 Chaos): the field kit's
// weapons as the champion wants them - a one-hander and a shield, two
// one-handers, a two-hander, two two-handers (Titan's Grip, which Bloodthirsty
// Grip teaches), a one-hander alone - and which kind. Only what the character
// can use now is offered; the kit keeps the choice through its level-ups and
// for the champion's next matches (RiftProgression::ChooseWeapons).
struct npc_rift_armsmaster : public CreatureAI
{
    explicit npc_rift_armsmaster(Creature* creature) : CreatureAI(creature) { }

    static uint32 constexpr ACTION_SHAPES = 1;
    static uint32 constexpr ACTION_SHAPE = 100;    // + shape: its weapon types
    static uint32 constexpr ACTION_CHOOSE = 1000;  // + shape * 100 + type + 1 (0: the best of any)

    void UpdateAI(uint32 /*diff*/) override { }

    void ShowShapes(Player* player)
    {
        ClearGossipMenuFor(player);
        uint8 const current = RiftProgression::CurrentWeapons(player).first;
        for (uint8 shape : RiftProgression::WeaponShapes(player))
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, std::string(RiftProgression::WeaponShapeName(shape)) + (shape == current ? " (now)" : ""),
                GOSSIP_SENDER_MAIN, ACTION_SHAPE + shape);
        SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, me->GetGUID());
    }

    bool OnGossipHello(Player* player) override
    {
        ShowShapes(player);
        return true;
    }

    bool OnGossipSelect(Player* player, uint32 /*menuId*/, uint32 gossipListId) override
    {
        uint32 const action = GetGossipActionFor(player, gossipListId);
        if (action == ACTION_SHAPES)
        {
            ShowShapes(player);
            return true;
        }
        if (action >= ACTION_SHAPE && action < ACTION_CHOOSE)
        {
            uint8 const shape = uint8(action - ACTION_SHAPE);
            std::pair<uint8, int32> const current = RiftProgression::CurrentWeapons(player);
            ClearGossipMenuFor(player);
            AddGossipItemFor(player, GOSSIP_ICON_BATTLE, std::string("The best I can carry") + (current.first == shape && current.second < 0 ? " (now)" : ""),
                GOSSIP_SENDER_MAIN, ACTION_CHOOSE + shape * 100);
            for (uint32 type : RiftProgression::WeaponTypes(player, shape))
                AddGossipItemFor(player, GOSSIP_ICON_BATTLE, std::string(RiftProgression::WeaponTypeName(type)) + (current.first == shape && current.second == int32(type) ? " (now)" : ""),
                    GOSSIP_SENDER_MAIN, ACTION_CHOOSE + shape * 100 + type + 1);
            AddGossipItemFor(player, GOSSIP_ICON_CHAT, "Back", GOSSIP_SENDER_MAIN, ACTION_SHAPES);
            SendGossipMenuFor(player, DEFAULT_GOSSIP_MESSAGE, me->GetGUID());
            return true;
        }
        CloseGossipMenuFor(player);
        if (action >= ACTION_CHOOSE)
        {
            uint8 const shape = uint8((action - ACTION_CHOOSE) / 100);
            int32 const type = int32((action - ACTION_CHOOSE) % 100) - 1;
            if (RiftProgression::ChooseWeapons(player, shape, type))
                ChatHandler(player->GetSession()).PSendSysMessage("Your weapons: %s%s%s.", RiftProgression::WeaponShapeName(shape),
                    type >= 0 ? ", " : "", type >= 0 ? RiftProgression::WeaponTypeName(uint32(type)) : "");
        }
        return true;
    }
};

// League Season 3's Dragon (creature_template 922140): a ranged basic attack at
// 350 League units that never swings in melee. It closes to its reach - the
// Season 3 Dragon walks, and can be pulled out of its pit - and fires on its own
// attack timer; BattlegroundSRF::DragonAttack makes the shot and the burn.
struct npc_rift_dragon : public ScriptedAI
{
    explicit npc_rift_dragon(Creature* creature) : ScriptedAI(creature) { }

    BattlegroundSRF* Rift() const
    {
        BattlegroundMap* map = me->GetMap() ? me->GetMap()->ToBattlegroundMap() : nullptr;
        Battleground* bg = map ? map->GetBG() : nullptr;
        return bg && bg->GetTypeID(true) == BATTLEGROUND_SRF ? static_cast<BattlegroundSRF*>(bg) : nullptr;
    }

    void AttackStart(Unit* who) override
    {
        BattlegroundSRF* rift = Rift();
        // Attack without the melee swing
        if (who && rift && me->Attack(who, false))
            me->GetMotionMaster()->MoveChase(who, rift->DragonRange() * 0.8f);
    }

    void Reset() override
    {
        _attackTimer = 0;
    }

    void UpdateAI(uint32 diff) override
    {
        if (!UpdateVictim())
            return;
        BattlegroundSRF* rift = Rift();
        Unit* victim = me->GetVictim();
        if (!rift || !victim)
            return;
        _attackTimer = _attackTimer > diff ? _attackTimer - diff : 0;
        // Edge to edge, as League measures attack range and as MoveChase stops:
        // centre to centre, the chase parked the drake (combat reach 9 yd) about
        // 19 yd from its target while the shot waited for 10.5, so it never fired.
        float const gap = me->GetExactDist2d(victim) - me->GetCombatReach() - victim->GetCombatReach();
        if (gap > rift->DragonRange())
        {
            if (me->GetMotionMaster()->GetCurrentMovementGeneratorType() != CHASE_MOTION_TYPE)
                me->GetMotionMaster()->MoveChase(victim, rift->DragonRange() * 0.8f);
            return;
        }
        if (_attackTimer || me->HasUnitState(UNIT_STATE_CASTING))
            return;
        me->SetFacingToObject(victim);
        _attackTimer = me->GetAttackTime(BASE_ATTACK);
        rift->DragonAttack(me, victim);
    }

private:
    uint32 _attackTimer = 0;
};

// League Season 3's Baron Nashor (creature_template 922141; the wiki, mid-2013): rooted,
// immune to crowd control, and ranged - nothing within 955 League units of it is safe.
// It attacks the nearest champion that has fought it ("Baron primarily attacks the
// target closest to him"). Each attack is Wrath of the Ancients - ten acid bolts half a
// second apart, each a stack of +25% magic damage taken, never at someone stealthed -
// unless that champion is hurting it mostly with physical damage and is not under
// Voracious Corrosion; then it is Corrosion, with Wrath between. Every 12-16 sec it mixes
// in Sweeping Blow, when anyone is close, or Propelled. BattlegroundSRF makes the shots.
struct npc_rift_baron : public ScriptedAI
{
    explicit npc_rift_baron(Creature* creature) : ScriptedAI(creature) { }

    static uint8 constexpr WRATH_BOLTS = 10;
    static uint32 constexpr WRATH_INTERVAL = 500;
    static uint32 constexpr SPECIAL_MIN = 12000, SPECIAL_MAX = 16000;
    static uint32 constexpr DAMAGE_DECAY_TICK = 1000;

    BattlegroundSRF* Rift() const
    {
        BattlegroundMap* map = me->GetMap() ? me->GetMap()->ToBattlegroundMap() : nullptr;
        Battleground* bg = map ? map->GetBG() : nullptr;
        return bg && bg->GetTypeID(true) == BATTLEGROUND_SRF ? static_cast<BattlegroundSRF*>(bg) : nullptr;
    }

    // Never a swing: it does not reach, and it does not move.
    void AttackStart(Unit* who) override
    {
        if (who)
            me->Attack(who, false);
    }

    void Reset() override
    {
        _attackTimer = 0;
        _wrathLeft = 0;
        _wrathTimer = 0;
        _specialTimer = urand(SPECIAL_MIN, SPECIAL_MAX);
        _decayTimer = DAMAGE_DECAY_TICK;
        _lastWasCorrosion = false;
        _physical.clear();
        _magic.clear();
    }

    // Voracious Corrosion halves what its target (and that one's pet) deals Baron;
    // what each deals, physical or not, decides when it is used.
    void DamageTaken(Unit* attacker, uint32& damage, DamageEffectType /*damageType*/, SpellInfo const* spellInfo) override
    {
        if (!attacker || !damage)
            return;
        Unit* source = attacker->GetCharmerOrOwnerOrSelf();
        if (source->HasAura(BG_SRF_SPELL_BARON_CORROSION, me->GetGUID()) || attacker->HasAura(BG_SRF_SPELL_BARON_CORROSION, me->GetGUID()))
            damage /= 2;
        bool const physical = !spellInfo || (spellInfo->GetSchoolMask() & SPELL_SCHOOL_MASK_NORMAL);
        (physical ? _physical : _magic)[source->GetGUID()] += float(damage);
    }

    void UpdateAI(uint32 diff) override
    {
        if (!UpdateVictim())
            return;
        BattlegroundSRF* rift = Rift();
        if (!rift)
            return;

        // the last five seconds or so of who hit it with what
        if (_decayTimer <= diff)
        {
            _decayTimer = DAMAGE_DECAY_TICK;
            for (auto* tally : { &_physical, &_magic })
                for (auto& [guid, amount] : *tally)
                    amount *= 0.8f;
        }
        else
            _decayTimer -= diff;

        _attackTimer = _attackTimer > diff ? _attackTimer - diff : 0;
        _specialTimer = _specialTimer > diff ? _specialTimer - diff : 0;

        // the stream under way
        if (_wrathLeft)
        {
            if (_wrathTimer > diff)
            {
                _wrathTimer -= diff;
                return;
            }
            _wrathTimer = WRATH_INTERVAL;
            Unit* target = ObjectAccessor::GetUnit(*me, _wrathTarget);
            if (!target || !Valid(target, rift, true))
            {
                _wrathLeft = 0;
                return;
            }
            --_wrathLeft;
            me->SetFacingToObject(target);
            rift->BaronWrath(me, target);
            return;
        }

        if (!_specialTimer)
        {
            _specialTimer = urand(SPECIAL_MIN, SPECIAL_MAX);
            if (Special(rift))
                return;
        }

        if (_attackTimer || me->HasUnitState(UNIT_STATE_CASTING))
            return;
        Unit* target = Nearest(rift, false);
        if (!target)
            return;
        me->SetFacingToObject(target);
        _attackTimer = me->GetAttackTime(BASE_ATTACK);
        if (WantsCorrosion(target))
        {
            _lastWasCorrosion = true;
            rift->BaronCorrosion(me, target);
            return;
        }
        _lastWasCorrosion = false;
        if (Stealthed(target))
            return;
        _wrathTarget = target->GetGUID();
        _wrathLeft = WRATH_BOLTS;
        _wrathTimer = 0;
    }

private:
    static bool Stealthed(Unit const* unit)
    {
        return unit->HasStealthAura() || unit->HasInvisibilityAura();
    }

    // Alive, attackable, seen, and within its reach, edge to edge as League measures it.
    bool Valid(Unit* unit, BattlegroundSRF const* rift, bool wrath) const
    {
        if (!unit->IsAlive() || !me->IsValidAttackTarget(unit) || !me->CanSeeOrDetect(unit) || (wrath && Stealthed(unit)))
            return false;
        return me->GetExactDist2d(unit) - me->GetCombatReach() - unit->GetCombatReach() <= rift->BaronRange();
    }

    // The nearest one that has fought it.
    Unit* Nearest(BattlegroundSRF const* rift, bool wrath) const
    {
        Unit* best = nullptr;
        for (ThreatReference const* ref : me->GetThreatManager().GetUnsortedThreatList())
            if (Unit* unit = ref->GetVictim())
                if (Valid(unit, rift, wrath) && (!best || me->GetExactDist2d(unit) < me->GetExactDist2d(best)))
                    best = unit;
        return best;
    }

    // Corrosion when its target hurts it mostly physically and is not under it (or it
    // is about to run out) - and never twice running: Wrath goes between.
    bool WantsCorrosion(Unit* target) const
    {
        if (_lastWasCorrosion)
            return false;
        Unit* source = target->GetCharmerOrOwnerOrSelf();
        auto const tally = [source](std::unordered_map<ObjectGuid, float> const& map)
        {
            auto const itr = map.find(source->GetGUID());
            return itr != map.end() ? itr->second : 0.0f;
        };
        float const physical = tally(_physical), magic = tally(_magic);
        if (physical <= 0.0f || physical < magic)
            return false;
        Aura const* corroded = target->GetAura(BG_SRF_SPELL_BARON_CORROSION, me->GetGUID());
        return !corroded || corroded->GetDuration() < int32(me->GetAttackTime(BASE_ATTACK));
    }

    // Sweeping Blow when a champion is close, else Propelled under one in reach.
    bool Special(BattlegroundSRF* rift)
    {
        std::vector<Unit*> inReach;
        bool close = false;
        for (ThreatReference const* ref : me->GetThreatManager().GetUnsortedThreatList())
            if (Unit* unit = ref->GetVictim())
                if (unit->GetTypeId() == TYPEID_PLAYER && Valid(unit, rift, false))
                {
                    inReach.push_back(unit);
                    if (me->GetExactDist2d(unit) - me->GetCombatReach() - unit->GetCombatReach() <= rift->BaronSweepReach())
                        close = true;
                }
        if (inReach.empty())
            return false;
        if (close)
            rift->BaronSweep(me);
        else
            rift->BaronPropel(me, Trinity::Containers::SelectRandomContainerElement(inReach));
        return true;
    }

    uint32 _attackTimer = 0;
    uint32 _wrathTimer = 0;
    uint32 _specialTimer = 0;
    uint32 _decayTimer = 0;
    uint8 _wrathLeft = 0;
    bool _lastWasCorrosion = false;
    ObjectGuid _wrathTarget;
    std::unordered_map<ObjectGuid, float> _physical;
    std::unordered_map<ObjectGuid, float> _magic;
};

void AddSC_custom_rift_progression()
{
    new custom_rift_contribution_unit();
    new custom_rift_contribution_player();
    new custom_rift_brush_unit();
    new custom_rift_mount_unit();
    new custom_rift_mount_player();
    AddonRequestRoutes::Register("RIFTPICK", &RiftProgression::HandlePickRequest);
    AddonRequestRoutes::Register("RIFTSTATE", &RiftProgression::HandleStateRequest);
    AddonRequestRoutes::Register("RIFTBUY", &RiftProgression::HandleBuyRequest);
    RegisterSpellScript(spell_rift_recall);
    RegisterSpellScript(spell_rift_recall_league);
    RegisterCreatureAI(npc_rift_minion);
    RegisterCreatureAI(npc_rift_dragon);
    RegisterCreatureAI(npc_rift_baron);
    RegisterCreatureAI(npc_rift_armsmaster);
}
