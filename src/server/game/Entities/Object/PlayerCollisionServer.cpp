#include "PlayerCollisionServer.h"
#include "Player.h"
#include "Position.h"

#include <atomic>
#include <cmath>
#include <list>
#include <mutex>
#include <unordered_map>

namespace
{
    struct Entry
    {
        uint32 spellId;
        PlayerCollisionServer::Rule rule;
    };

    // A player can hold more than one of the four auras at once, so this is a
    // multimap keyed by guid.
    std::mutex                                              s_mutex;
    std::unordered_multimap<ObjectGuid, Entry>              s_blockers;
    std::atomic<uint32>                                     s_count{ 0 };

    // Matches the client tweak's shipped defaults: Radius 1.0 means two bodies
    // block at 2.0 yd apart, and Height 2.0 keeps a player on the floor above
    // from blocking. Keep these in step with the DLL or bots and humans will
    // disagree about where the walls are.
    constexpr float BLOCK_DISTANCE = 2.0f;      // 2 * Radius
    constexpr float BLOCK_HEIGHT   = 2.0f;

    bool HasRule(Player const* who, PlayerCollisionServer::Rule rule)
    {
        if (!who)
            return false;
        std::lock_guard<std::mutex> guard(s_mutex);
        auto range = s_blockers.equal_range(who->GetGUID());
        for (auto itr = range.first; itr != range.second; ++itr)
            if (itr->second.rule == rule && who->HasAura(itr->second.spellId))
                return true;
        return false;
    }

    bool IsRegistered(Player const* who)
    {
        if (!who)
            return false;
        std::lock_guard<std::mutex> guard(s_mutex);
        return s_blockers.find(who->GetGUID()) != s_blockers.end();
    }

    // Earliest point along from->to at which the mover comes within `radius` of
    // `centre`, as a fraction in [0,1]. Same quadratic the client uses.
    bool SegmentHitsCircle(float sx, float sy, float dx, float dy,
                           float cx, float cy, float radius, float& tOut)
    {
        float fx = sx - cx, fy = sy - cy;
        float a = dx * dx + dy * dy;
        if (a < 1e-6f)
            return false;
        float c = fx * fx + fy * fy - radius * radius;
        if (c <= 0.0f)                      // already inside
        {
            tOut = 0.0f;
            return true;
        }
        float b = 2.0f * (fx * dx + fy * dy);
        float disc = b * b - 4.0f * a * c;
        if (disc < 0.0f)
            return false;
        float t = (-b - std::sqrt(disc)) / (2.0f * a);
        if (t < 0.0f || t > 1.0f)
            return false;
        tOut = t;
        return true;
    }
}

namespace PlayerCollisionServer
{
    void Add(WorldObject const* who, uint32 spellId, Rule rule)
    {
        if (!who)
            return;
        std::lock_guard<std::mutex> guard(s_mutex);
        s_blockers.emplace(who->GetGUID(), Entry{ spellId, rule });
        s_count.fetch_add(1, std::memory_order_relaxed);
    }

    void Remove(WorldObject const* who, uint32 spellId)
    {
        if (!who)
            return;
        std::lock_guard<std::mutex> guard(s_mutex);
        auto range = s_blockers.equal_range(who->GetGUID());
        for (auto itr = range.first; itr != range.second; ++itr)
        {
            if (itr->second.spellId == spellId)
            {
                s_blockers.erase(itr);
                s_count.fetch_sub(1, std::memory_order_relaxed);
                return;
            }
        }
    }

    bool Active()
    {
        return s_count.load(std::memory_order_relaxed) != 0;
    }

    bool Blocks(Player const* mover, Player const* blocker)
    {
        if (!mover || !blocker || mover == blocker)
            return false;

        // Unconditional rules first - no reaction lookup needed.
        if (HasRule(blocker, RULE_BLOCK_ALL))
            return true;
        if (HasRule(blocker, RULE_COLLIDE_ALL) || HasRule(mover, RULE_COLLIDE_ALL))
            return true;

        bool const wantsHostile =
            HasRule(blocker, RULE_BLOCK_ENEMIES) ||
            HasRule(blocker, RULE_COLLIDE_ENEMIES) ||
            HasRule(mover, RULE_COLLIDE_ENEMIES);

        return wantsHostile && mover->IsHostileTo(blocker);
    }

    bool ClipSegment(Player const* mover, Position const& from, Position& dest)
    {
        if (!Active() || !mover || !mover->IsInWorld())
            return false;

        float sx = from.GetPositionX(), sy = from.GetPositionY(), sz = from.GetPositionZ();
        float dx = dest.GetPositionX() - sx;
        float dy = dest.GetPositionY() - sy;
        float segLen = std::sqrt(dx * dx + dy * dy);
        if (segLen < 0.01f)
            return false;

        std::list<Player*> nearby;
        mover->GetPlayerListInGrid(nearby, segLen + BLOCK_DISTANCE + 1.0f);

        float bestT = 1.0f;
        bool  hit = false;
        for (Player* p : nearby)
        {
            if (!p || p == mover || !p->IsInWorld() || p->IsGameMaster())
                continue;
            if (!IsRegistered(p) && !IsRegistered(mover))
                continue;                       // neither side carries an aura
            if (std::fabs(p->GetPositionZ() - sz) >= BLOCK_HEIGHT)
                continue;                       // different floor / overhead
            if (!Blocks(mover, p))
                continue;

            float t;
            if (SegmentHitsCircle(sx, sy, dx, dy,
                                  p->GetPositionX(), p->GetPositionY(),
                                  BLOCK_DISTANCE, t) && t < bestT)
            {
                bestT = t;
                hit = true;
            }
        }

        if (!hit)
            return false;

        // Stop a hair short of contact so the bot rests against the body rather
        // than exactly on the boundary, where float noise would flip the test.
        float stopT = bestT - (0.05f / segLen);
        if (stopT < 0.0f)
            stopT = 0.0f;

        dest.Relocate(sx + dx * stopT, sy + dy * stopT, dest.GetPositionZ(),
                      dest.GetOrientation());
        return true;
    }
}
