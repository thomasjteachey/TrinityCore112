/*
 * T2 set-bonus hooks that live in the UNIT / PLAYER / MOVEMENT pipeline.
 * Bodies for T2UnitHooks.h. See the header for the contract; this file is the
 * only place the logic lives. Call sites:
 *
 *   BlocksManaGain          Unit::EnergizeBySpell, Player::Regenerate
 *   OnPlayerMovement        WorldSession::HandleMovementOpcodes (own mover)
 *   OnPlayerRemovedFromWorld Player::RemoveFromWorld
 *   OnPlayerControlLost     Unit::SetControlled (stun / root / confuse / fear applied)
 *   IsTemporaryWeapon       Player::CanUseItem(Item*), Player::GetBaseWeaponSkillValue,
 *                           Unit::GetWeaponSkillValue
 *
 * Every entry point early-outs for players without the relevant set-bonus
 * aura, so none of this costs anything measurable for everyone else.
 */

#include "T2UnitHooks.h"
#include "Chat.h"
#include "Log.h"
#include "Common.h"
#include "Item.h"
#include "MovementInfo.h"
#include "ObjectAccessor.h"
#include "Pet.h"
#include "Player.h"
#include "SharedDefines.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "StringFormat.h"
#include "UnitDefines.h"
#include "World.h"
#include "WorldSession.h"
#include <atomic>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace
{
    // Legion of One's escape: 90320 is the 8pc carrier, 90525 the rebirth flash.
    constexpr uint32 SPELL_LEGION_OF_ONE         = 90320;
    constexpr uint32 SPELL_LEGION_REBIRTH_VISUAL = 90525;

    // .gm diagnostics customauras - same channel the custom_t1/t2 scripts use,
    // so a GM who opted in sees this module's decisions next to theirs.
    void SendCustomAuraDiag(std::string const& msg)
    {
        // Also to the log, not just to whoever happens to be watching. A
        // chat-only diagnostic cannot distinguish "the chain never ran" from
        // "no GM had the category enabled", which is precisely how three set
        // bonuses got mis-diagnosed twice. Logger `custom.auras`; free when off.
        TC_LOG_INFO("custom.auras", "{}", msg);

        for (auto const& sessionPair : sWorld->GetAllSessions())
            if (sessionPair.second && sessionPair.second->GetPlayer()
                && sessionPair.second->IsGmDiagnosticEnabled(GmDiagnosticCategory::CustomAuras))
                ChatHandler(sessionPair.second).SendSysMessage(msg);
    }

    // Milliseconds from `from` to `to` on the packet clock, 0 if `to` is not
    // after `from`. Movement times can step back a few ms when the session
    // re-syncs its clock delta; an unsigned subtraction would then read as
    // hours, not as nothing.
    uint32 ElapsedMs(uint32 from, uint32 to)
    {
        return to >= from ? to - from : 0;
    }

    // Smallest absolute angle between two orientations, in radians.
    float FacingDelta(float lhs, float rhs)
    {
        float delta = std::fabs(Position::NormalizeOrientation(lhs) - Position::NormalizeOrientation(rhs));
        if (delta > float(M_PI))
            delta = 2.0f * float(M_PI) - delta;
        return delta;
    }

    // -----------------------------------------------------------------------
    // BLOOD FOR POWER
    // -----------------------------------------------------------------------

    // Life Tap's family flag: spell 1454 (all ranks) and the 31818 energize both
    // carry SpellFamilyName 5 / SpellFamilyFlags[0] 0x40000 (verified against
    // the binary Spell.dbc 2026-08-19).
    constexpr uint32 LIFE_TAP_FAMILY_FLAG = 0x00040000;

    bool IsLifeTapSource(SpellInfo const* source)
    {
        if (!source)
            return false;
        if (source->Id == T2UnitHooks::SPELL_LIFE_TAP_ENERGIZE)
            return true;
        return source->SpellFamilyName == SPELLFAMILY_WARLOCK && (source->SpellFamilyFlags[0] & LIFE_TAP_FAMILY_FLAG) != 0;
    }

    // -----------------------------------------------------------------------
    // MOMENTUM
    //
    // Driven from the player's own movement packets, so a genuine stop, turn
    // or strafe is seen the moment the client reports it, and a heartbeat
    // (~500 ms) bounds how late a silent event (running into a wall) is seen.
    // All timing uses the packet's own movementInfo.time (converted to the
    // server clock by the handler): two heartbeats drained from the queue in
    // one lagged world tick are 500 ms apart by that clock, not 0 ms, so they
    // never read as a position jump.
    //
    // Rules implemented (the design text is in T2UnitHooks.h):
    //   running  = alive, not mounted, not on a taxi, MOVEMENTFLAG_FORWARD set,
    //              none of BACKWARD / STRAFE_* / LEFT / RIGHT (keyboard turn) /
    //              ROOT / WALKING / SWIMMING / FLYING set.
    //   run      = a stretch of running whose heading stays within
    //              MOMENTUM_TURN_TOLERANCE of the heading the RUN started with
    //              (mouse turning only shows up as orientation drift, so this
    //              is what catches it). The reference heading is fixed for the
    //              whole run, never re-based per stack: otherwise a runner
    //              could curve 10 deg/s for ever and keep four stacks.
    //   leg      = MOMENTUM_STACK_MS of a live run. Every leg that actually
    //              covered ground adds one stack of 90392 and opens the next.
    //   break    = anything that is not running, a turn past tolerance, a
    //              position jump (teleport/knockback), loss of control (see
    //              OnPlayerControlLost), or a leg that ran its 3 s without
    //              covering MOMENTUM_MIN_SPEED_FRACTION of run speed (stuck
    //              against geometry). Break removes 90392.
    //   jumping  = does NOT break (FALLING/FALLING_FAR are ignored); a jump
    //              with FORWARD held keeps the leg alive.
    //   mounting = breaks, and nothing builds while mounted (SPELL_AURA_
    //              MOD_SPEED_ALWAYS is not even consulted for mounted speed).
    //   death    = the buff is already stripped by RemoveAllAurasOnDeath; the
    //              tracker resets on the next packet (not alive => not running).
    // -----------------------------------------------------------------------
    constexpr uint32 MOMENTUM_STACK_MS        = 3000;
    constexpr uint32 MOMENTUM_STALE_MS        = 2500;                   // > 4 missed heartbeats: tracker is stale
    constexpr float  MOMENTUM_TURN_TOLERANCE  = float(M_PI) / 6.0f;     // 30 degrees
    constexpr float  MOMENTUM_MIN_SPEED_FRACTION = 0.25f;               // stuck-against-a-wall floor
    constexpr float  MOMENTUM_JUMP_SLACK      = 4.0f;                   // yards of tolerance on the position-jump test
    constexpr uint32 MOMENTUM_BREAK_FLAGS =
        MOVEMENTFLAG_BACKWARD | MOVEMENTFLAG_STRAFE_LEFT | MOVEMENTFLAG_STRAFE_RIGHT |
        MOVEMENTFLAG_LEFT | MOVEMENTFLAG_RIGHT | MOVEMENTFLAG_ROOT | MOVEMENTFLAG_WALKING |
        MOVEMENTFLAG_SWIMMING | MOVEMENTFLAG_FLYING;

    struct MomentumRun
    {
        uint32 legStartMs     = 0;      // 0 = no live run/leg
        float  runOrientation = 0.0f;   // heading the RUN opened with; the 30-degree tolerance is against this
        float  legDistance    = 0.0f;   // ground covered during the live leg
        bool   hasSample      = false;
        float  lastX          = 0.0f;
        float  lastY          = 0.0f;
        uint32 lastSampleMs   = 0;      // movementInfo.time of the previous packet (server clock)
    };

    enum class MomentumVerdict : uint8 { None, Break, Stack };

    // Map updates run on several threads; the tracker is shared, so it is
    // locked. Nothing reaches the lock unless the wearer has the five-piece.
    std::mutex                                  s_momentumMutex;
    std::unordered_map<ObjectGuid, MomentumRun> s_momentum;

    // Everything that needs the tracker, under the lock. Returns what the
    // caller should do to the buff; the caller acts on it OUTSIDE the lock.
    // `freshTracker` is set when this packet created the entry, so the caller
    // can drop a buff left over from before a teleport/relog. `nowMs` is the
    // packet's movementInfo.time, already on the server clock.
    MomentumVerdict AdvanceMomentum(Player* player, MovementInfo const& info, uint32 nowMs, bool& freshTracker)
    {
        std::lock_guard<std::mutex> guard(s_momentumMutex);

        auto [itr, inserted] = s_momentum.try_emplace(player->GetGUID());
        MomentumRun& run = itr->second;
        freshTracker = inserted;

        uint32 const flags = info.GetMovementFlags();
        float const x = info.pos.GetPositionX();
        float const y = info.pos.GetPositionY();
        float const o = info.pos.GetOrientation();

        bool const running = player->IsAlive() && !player->IsMounted() && !player->IsInFlight()
            && (flags & MOVEMENTFLAG_FORWARD) != 0
            && (flags & MOMENTUM_BREAK_FLAGS) == 0;

        // Sample bookkeeping: distance and time since the previous packet.
        float moved = 0.0f;
        uint32 dt = 0;
        bool discontinuity = false;
        if (run.hasSample)
        {
            float const dx = x - run.lastX;
            float const dy = y - run.lastY;
            moved = std::sqrt(dx * dx + dy * dy);
            // A backwards step reads as "no time passed", which the
            // position-jump test below then judges on slack alone.
            dt = ElapsedMs(run.lastSampleMs, nowMs);

            // Stale tracker (nothing heard for a long time) or a position jump
            // the player's own legs cannot explain (teleport, knockback).
            float const explainable = player->GetSpeed(MOVE_RUN) * (float(dt) / 1000.0f) * 2.0f + MOMENTUM_JUMP_SLACK;
            discontinuity = dt > MOMENTUM_STALE_MS || moved > explainable;
        }
        run.lastX = x;
        run.lastY = y;
        run.lastSampleMs = nowMs;
        run.hasSample = true;

        MomentumVerdict verdict = MomentumVerdict::None;

        if (!running || discontinuity)
        {
            verdict = MomentumVerdict::Break;
            run.legStartMs = 0;
            run.legDistance = 0.0f;
        }
        else if (run.legStartMs == 0)
        {
            // First running packet: open a run from this heading. (After a
            // Break above while still running - a teleport mid-run - the next
            // packet lands here.)
            run.legStartMs = nowMs;
            run.runOrientation = o;
            run.legDistance = 0.0f;
        }
        else
        {
            run.legDistance += moved;

            if (FacingDelta(o, run.runOrientation) > MOMENTUM_TURN_TOLERANCE)
            {
                // Turned past tolerance against the heading the whole run
                // started with: stacks go, but the player is still running,
                // so a NEW run opens right now from the new heading.
                verdict = MomentumVerdict::Break;
                run.legStartMs = nowMs;
                run.runOrientation = o;
                run.legDistance = 0.0f;
            }
            else if (ElapsedMs(run.legStartMs, nowMs) >= MOMENTUM_STACK_MS)
            {
                // A full leg. It only counts if it covered ground: FORWARD
                // stays set while running into a wall. The next leg opens
                // against the SAME run heading - deliberately not re-based.
                float const needed = player->GetSpeed(MOVE_RUN) * (float(MOMENTUM_STACK_MS) / 1000.0f) * MOMENTUM_MIN_SPEED_FRACTION;
                verdict = run.legDistance >= needed ? MomentumVerdict::Stack : MomentumVerdict::Break;
                run.legStartMs = nowMs;
                run.legDistance = 0.0f;
            }
        }

        return verdict;
    }

    void ForgetMomentum(ObjectGuid const& guid)
    {
        std::lock_guard<std::mutex> guard(s_momentumMutex);
        s_momentum.erase(guid);
    }

    // -----------------------------------------------------------------------
    // ASHEN CONFISCATION - temporary weapon registry
    //
    // Unit::GetWeaponSkillValue runs on every melee swing of every player, so
    // the empty case must not take the lock: the count is kept in an atomic
    // and checked first.
    // -----------------------------------------------------------------------
    std::mutex                       s_temporaryWeaponMutex;
    std::unordered_set<ObjectGuid>   s_temporaryWeapons;
    std::atomic<uint32>              s_temporaryWeaponCount{ 0 };
}

namespace T2UnitHooks
{
    bool BlocksManaGain(Unit const* target, SpellInfo const* source)
    {
        if (!target)
            return false;

        // Cheapest test first: almost nobody carries the aura.
        if (!target->HasAura(SPELL_BLOOD_FOR_POWER))
            return false;

        // Life Tap (any rank, and its 31818 energize) is the one allowed source.
        return !IsLifeTapSource(source);
    }

    void OnPlayerMovement(Player* player, uint16 /*opcode*/, MovementInfo const& newInfo)
    {
        if (!player)
            return;

        if (!player->HasAura(SPELL_MOMENTUM))
        {
            // The set came off (or the aura was lost some other way) while a
            // run was live: the buff must not outlive its carrier. One extra
            // lookup per packet for everyone else; the tracker is not touched.
            if (player->HasAura(SPELL_MOMENTUM_BUFF))
            {
                player->RemoveAurasDueToSpell(SPELL_MOMENTUM_BUFF);
                ForgetMomentum(player->GetGUID());
                SendCustomAuraDiag(Trinity::StringFormat(
                    "[CustomAuras] {}: Momentum carrier gone - stacks cleared", player->GetName()));
            }
            return;
        }

        // Timed off the packet's own clock (the handler has already shifted
        // movementInfo.time onto the server clock), not off GameTime at
        // processing time: a lagged tick drains several queued heartbeats in
        // one go and they must keep their real spacing.
        bool freshTracker = false;
        MomentumVerdict const verdict = AdvanceMomentum(player, newInfo, newInfo.time, freshTracker);

        // A tracker that was just (re)created while the buff is still on the
        // player means the buff survived a far teleport or a relog; it belongs
        // to a run we no longer know anything about.
        if (freshTracker && player->HasAura(SPELL_MOMENTUM_BUFF))
        {
            player->RemoveAurasDueToSpell(SPELL_MOMENTUM_BUFF);
            SendCustomAuraDiag(Trinity::StringFormat(
                "[CustomAuras] {}: Momentum stacks from before a teleport/relog cleared", player->GetName()));
        }

        switch (verdict)
        {
            case MomentumVerdict::Break:
                if (player->HasAura(SPELL_MOMENTUM_BUFF))
                {
                    player->RemoveAurasDueToSpell(SPELL_MOMENTUM_BUFF);
                    SendCustomAuraDiag(Trinity::StringFormat(
                        "[CustomAuras] {}: Momentum broken - stacks cleared", player->GetName()));
                }
                break;
            case MomentumVerdict::Stack:
            {
                // A triggered re-cast of a CumulativeAura-4 spell refreshes and
                // adds one stack; the dbc cap keeps it at 4.
                player->CastSpell(player, SPELL_MOMENTUM_BUFF, true);
                if (Aura const* buff = player->GetAura(SPELL_MOMENTUM_BUFF))
                    SendCustomAuraDiag(Trinity::StringFormat(
                        "[CustomAuras] {}: Momentum now at {} stack(s)",
                        player->GetName(), uint32(buff->GetStackAmount())));
                break;
            }
            case MomentumVerdict::None:
            default:
                break;
        }
    }

    void OnPlayerRemovedFromWorld(Player* player)
    {
        if (!player)
            return;
        // Only the bookkeeping. Whether the buff survives the map change is
        // settled by the first packet on the other side (see freshTracker).
        ForgetMomentum(player->GetGUID());
    }

    void OnPlayerControlLost(Player* player)
    {
        if (!player)
            return;

        // Cheapest test first: almost nobody carries the five-piece.
        if (!player->HasAura(SPELL_MOMENTUM))
            return;

        // The run is over whatever happens next; the next packet (when the
        // player can move again) opens a fresh tracker, which on its own
        // already drops a buff it does not know about.
        ForgetMomentum(player->GetGUID());

        if (!player->HasAura(SPELL_MOMENTUM_BUFF))
            return;

        // We are inside Unit::SetControlled, i.e. inside the stun / root /
        // fear / confuse aura's own apply handler - removing another aura
        // from there is exactly what the open heap-corruption rule forbids.
        // The removal goes onto the player's event queue instead and runs
        // from the top of the next Unit::Update (the queue dies with the
        // player, so the captured pointer cannot dangle). Until then the
        // stacks linger for at most one world tick.
        player->m_Events.AddEventAtOffset([player]()
        {
            if (!player->HasAura(SPELL_MOMENTUM_BUFF))
                return;
            player->RemoveAurasDueToSpell(SPELL_MOMENTUM_BUFF);
            SendCustomAuraDiag(Trinity::StringFormat(
                "[CustomAuras] {}: Momentum broken by loss of control - stacks cleared", player->GetName()));
        }, Milliseconds(0));
    }

    // LEGION OF ONE (90320). See the header. Returns true when the imp was
    // spent to save the warlock, in which case Unit::DealDamage must not kill him.
    bool OnWouldBeLethalDamage(Unit* victim)
    {
        Player* warlock = victim ? victim->ToPlayer() : nullptr;
        if (!warlock || !warlock->IsInWorld())
            return false;

        if (!warlock->HasAura(SPELL_LEGION_OF_ONE))
            return false;

        Pet* imp = warlock->GetPet();
        if (!imp || !imp->IsAlive() || !imp->IsInWorld() || imp->GetMap() != warlock->GetMap())
            return false;

        // The imp's remaining health becomes the warlock's, clamped to what he
        // can hold and floored at 1. Mana is deliberately not mentioned: he
        // never dies, and Unit::setDeathState is the only thing that zeroes it.
        uint32 const health = std::max<uint32>(1u,
            std::min<uint32>(imp->GetHealth(), warlock->GetMaxHealth()));
        Position const at = imp->GetPosition();
        ObjectGuid const impGuid = imp->GetGUID();
        ObjectGuid const warlockGuid = warlock->GetGUID();

        // Survive HERE, synchronously: the caller is about to decide whether to
        // call Unit::Kill and has to see a living unit.
        warlock->SetHealth(health);

        // Everything else is deferred. This runs inside the ATTACKER's damage
        // frame, so unsummoning a pet, teleporting or casting from here would
        // re-enter object and aura machinery that is mid-walk. The event
        // processor drains from the top of Unit::Update, clear of all of it,
        // and dies with the player on logout or a far teleport.
        warlock->m_Events.AddEventAtOffset([warlockGuid, impGuid, at]()
        {
            Player* owner = ObjectAccessor::FindPlayer(warlockGuid);
            if (!owner || !owner->IsInWorld() || !owner->IsAlive())
                return;

            if (Pet* pet = owner->GetPet())
                if (pet->GetGUID() == impGuid)
                    owner->RemovePet(pet, PET_SAVE_NOT_IN_SLOT, false);

            owner->NearTeleportTo(at, false);

            // A resurrection flash, so it reads as a rebirth and not a blink.
            if (sSpellMgr->GetSpellInfo(SPELL_LEGION_REBIRTH_VISUAL))
                owner->CastSpell(owner, SPELL_LEGION_REBIRTH_VISUAL, true);

            SendCustomAuraDiag(Trinity::StringFormat(
                "[CustomAuras] {}: Legion of One - lethal damage taken; the imp was spent, reborn at its spot on {} health",
                owner->GetName(), owner->GetHealth()));
        }, Milliseconds(1));

        return true;
    }

    void RegisterTemporaryWeapon(ObjectGuid itemGuid)
    {
        if (itemGuid.IsEmpty())
            return;
        std::lock_guard<std::mutex> guard(s_temporaryWeaponMutex);
        if (s_temporaryWeapons.insert(itemGuid).second)
            s_temporaryWeaponCount.store(uint32(s_temporaryWeapons.size()), std::memory_order_release);
    }

    void UnregisterTemporaryWeapon(ObjectGuid itemGuid)
    {
        std::lock_guard<std::mutex> guard(s_temporaryWeaponMutex);
        if (s_temporaryWeapons.erase(itemGuid))
            s_temporaryWeaponCount.store(uint32(s_temporaryWeapons.size()), std::memory_order_release);
    }

    bool IsTemporaryWeapon(Item const* item)
    {
        if (!item)
            return false;
        if (s_temporaryWeaponCount.load(std::memory_order_acquire) == 0)
            return false;
        std::lock_guard<std::mutex> guard(s_temporaryWeaponMutex);
        return s_temporaryWeapons.find(item->GetGUID()) != s_temporaryWeapons.end();
    }
}
