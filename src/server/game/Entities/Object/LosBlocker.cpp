#include "LosBlocker.h"
#include "Object.h"
#include "Player.h"
#include "ObjectAccessor.h"
#include "World.h"

#include <atomic>
#include <cmath>
#include <list>
#include <mutex>
#include <unordered_map>

namespace
{
    // Blockers are rare (a mode-specific buff), so a global set plus an atomic
    // count is plenty: the count makes the common "nobody has it" case free, and
    // the set is only walked when the count is non-zero.
    std::mutex                             s_mutex;
    std::unordered_map<ObjectGuid, uint32>  s_blockers;   // guid -> aura spell id
    std::atomic<uint32>                     s_count{ 0 };

    // How wide a body is, for the purposes of eclipsing a shot. Kept a little
    // generous compared with the client-side collision radius: collision decides
    // where you may stand, this decides whether you eclipse a line, and a body
    // that is visually in the way should block even if the centres are not
    // perfectly aligned.
    constexpr float BLOCK_RADIUS = 1.2f;

    // Vertical extent, matching the client tweak's Height: a player upstairs or
    // flying overhead must not eclipse anything.
    constexpr float BLOCK_HEIGHT = 2.5f;

    // Distance from the shot's own endpoints within which a blocker is ignored,
    // so a body standing right on top of the caster or the target cannot block.
    constexpr float ENDPOINT_SLACK = 0.5f;

    // Shortest distance from point P to the segment AB, in 2-D, plus where along
    // the segment that happens (0..1). 2-D because bodies are vertical columns.
    float DistanceToSegment2D(float ax, float ay, float bx, float by,
                              float px, float py, float& tOut)
    {
        float dx = bx - ax, dy = by - ay;
        float len2 = dx * dx + dy * dy;
        if (len2 < 0.0001f)                          // caster and target coincide
        {
            tOut = 0.0f;
            float ex = px - ax, ey = py - ay;
            return std::sqrt(ex * ex + ey * ey);
        }
        float t = ((px - ax) * dx + (py - ay) * dy) / len2;
        if (t < 0.0f) t = 0.0f;
        else if (t > 1.0f) t = 1.0f;
        tOut = t;
        float cx = ax + dx * t, cy = ay + dy * t;
        float ex = px - cx, ey = py - cy;
        return std::sqrt(ex * ex + ey * ey);
    }

    bool SegmentEclipsedBy(WorldObject const* caster,
                           float ax, float ay, float az,
                           float bx, float by, float bz,
                           WorldObject const* exclude)
    {
        std::list<Player*> nearby;
        // Search from the caster; a blocker further than the shot itself cannot
        // stand between the two ends of it.
        float dx = bx - ax, dy = by - ay;
        float range = std::sqrt(dx * dx + dy * dy) + BLOCK_RADIUS + 1.0f;
        caster->GetPlayerListInGrid(nearby, range);

        for (Player* p : nearby)
        {
            if (!p || p == caster || p == exclude)
                continue;
            if (!p->IsInWorld() || p->IsGameMaster())
                continue;

            uint32 auraId = 0;
            {
                std::lock_guard<std::mutex> guard(s_mutex);
                auto itr = s_blockers.find(p->GetGUID());
                if (itr == s_blockers.end())
                    continue;
                auraId = itr->second;
            }

            // The registry is only a fast filter. Verify the aura for real, so a
            // stale entry - a disconnect that skipped the remove handler, say -
            // can never make somebody block without the buff.
            if (!p->HasAura(auraId))
                continue;

            // Only enemies are eclipsed by you; allies shoot through freely.
            if (!caster->IsHostileTo(p))
                continue;

            float t = 0.0f;
            float d = DistanceToSegment2D(ax, ay, bx, by,
                                          p->GetPositionX(), p->GetPositionY(), t);
            if (d >= BLOCK_RADIUS)
                continue;

            // Ignore a body sitting on either endpoint: standing on the caster
            // or hugging the target must not blank the shot.
            float segLen = std::sqrt(dx * dx + dy * dy);
            if (segLen > 0.0001f)
            {
                float along = t * segLen;
                if (along < ENDPOINT_SLACK || (segLen - along) < ENDPOINT_SLACK)
                    continue;
            }

            // Height: the shot's z where the blocker stands, against the
            // blocker's own column.
            float shotZ = az + (bz - az) * t;
            float footZ = p->GetPositionZ();
            if (shotZ < footZ - 0.5f || shotZ > footZ + BLOCK_HEIGHT)
                continue;

            return true;
        }
        return false;
    }
}

namespace LosBlocker
{
    void Add(WorldObject const* who, uint32 spellId)
    {
        if (!who)
            return;
        std::lock_guard<std::mutex> guard(s_mutex);
        if (s_blockers.emplace(who->GetGUID(), spellId).second)
            s_count.fetch_add(1, std::memory_order_relaxed);
    }

    void Remove(WorldObject const* who)
    {
        if (!who)
            return;
        std::lock_guard<std::mutex> guard(s_mutex);
        if (s_blockers.erase(who->GetGUID()))
            s_count.fetch_sub(1, std::memory_order_relaxed);
    }

    bool Active()
    {
        return s_count.load(std::memory_order_relaxed) != 0;
    }

    bool Blocks(WorldObject const* caster, WorldObject const* target)
    {
        if (!Active() || !caster || !target || caster == target)
            return false;
        if (!caster->IsInWorld() || !target->IsInWorld() || !caster->IsInMap(target))
            return false;

        // `target` is excluded: a spell aimed straight at the blocker still lands.
        return SegmentEclipsedBy(caster,
                                 caster->GetPositionX(), caster->GetPositionY(),
                                 caster->GetPositionZ() + caster->GetCollisionHeight(),
                                 target->GetPositionX(), target->GetPositionY(),
                                 target->GetPositionZ() + target->GetCollisionHeight(),
                                 target);
    }

    bool BlocksPosition(WorldObject const* caster, float x, float y, float z)
    {
        if (!Active() || !caster || !caster->IsInWorld())
            return false;

        return SegmentEclipsedBy(caster,
                                 caster->GetPositionX(), caster->GetPositionY(),
                                 caster->GetPositionZ() + caster->GetCollisionHeight(),
                                 x, y, z, nullptr);
    }
}
