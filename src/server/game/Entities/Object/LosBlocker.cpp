#include "LosBlocker.h"
#include "Object.h"
#include "Player.h"
#include "ObjectAccessor.h"
#include "World.h"

#include <atomic>
#include <cmath>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    // Blockers are rare (a mode-specific buff), so a registry plus an atomic
    // count is plenty: the count makes the common "nobody has it" case free, and
    // the registry is only walked when the count is non-zero.
    //
    // PARTITIONED BY MAP+INSTANCE, and it has to be. A creature's low guid is
    // generated per map (Map::GenerateLowGuid), so two parallel instances of the
    // same battleground hand out the SAME guid to their respective blocks; a
    // flat guid-keyed map would silently drop the second registration and then
    // deregister both on the first removal. Player guids are globally unique, so
    // this only started to matter when non-player blockers became possible.
    // Partitioning also means a lookup walks only the caster's own instance.
    std::mutex                              s_mutex;
    using BlockerBucket = std::unordered_map<ObjectGuid, uint32>;   // guid -> aura spell id
    std::unordered_map<uint64, BlockerBucket> s_blockers;           // map+instance -> bucket
    std::atomic<uint32>                     s_count{ 0 };

    // Map id in the high half, instance id in the low half. Instance id alone is
    // not enough: it is 0 for every non-instanceable map.
    uint64 MapKeyOf(WorldObject const* who)
    {
        return (uint64(who->GetMapId()) << 32) | uint64(who->GetInstanceId());
    }

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
        float dx = bx - ax, dy = by - ay;

        // Walk the registry, not the grid. GetPlayerListInGrid only ever yields
        // *alive players*, so a blocker that is not a player could never be
        // found however faithfully it registered itself - which is what kept
        // Tomb of Ice's summon (90522) inert. It is also the cheaper of the two:
        // the registry holds a handful of entries at most, where a grid search
        // touches every player in range.
        //
        // Snapshot under the lock and resolve outside it. Resolving a guid walks
        // the map, and holding s_mutex across that would invert the lock order
        // against an Add/Remove firing from an aura hook on another map update.
        std::vector<std::pair<ObjectGuid, uint32>> candidates;
        {
            std::lock_guard<std::mutex> guard(s_mutex);
            auto bucket = s_blockers.find(MapKeyOf(caster));
            if (bucket == s_blockers.end())
                return false;
            candidates.assign(bucket->second.begin(), bucket->second.end());
        }

        for (auto const& candidate : candidates)
        {
            WorldObject* blocker = ObjectAccessor::GetWorldObject(*caster, candidate.first);
            if (!blocker || blocker == caster || blocker == exclude)
                continue;
            if (!blocker->IsInWorld() || !blocker->IsInMap(caster))
                continue;

            // The registry is only a fast filter. Verify the aura for real, so a
            // stale entry - a disconnect that skipped the remove handler, say -
            // can never make somebody block without the buff.
            Unit const* blockerUnit = blocker->ToUnit();
            if (!blockerUnit || !blockerUnit->HasAura(candidate.second))
                continue;

            if (Player const* blockerPlayer = blocker->ToPlayer())
                if (blockerPlayer->IsGameMaster())
                    continue;

            // Only enemies are eclipsed by you; allies shoot through freely.
            if (!caster->IsHostileTo(blocker))
                continue;

            float t = 0.0f;
            float d = DistanceToSegment2D(ax, ay, bx, by,
                                          blocker->GetPositionX(), blocker->GetPositionY(), t);
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
            float footZ = blocker->GetPositionZ();
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
        if (s_blockers[MapKeyOf(who)].emplace(who->GetGUID(), spellId).second)
            s_count.fetch_add(1, std::memory_order_relaxed);
    }

    void Remove(WorldObject const* who)
    {
        if (!who)
            return;
        std::lock_guard<std::mutex> guard(s_mutex);

        ObjectGuid const guid = who->GetGUID();

        // Try the bucket it is on NOW first, then fall back to a full sweep: a
        // player keeps his auras across a map change, so the aura that
        // registered him on one map can just as easily be removed on another,
        // and a missed erase would leave a permanent phantom blocker plus a
        // count that never returns to zero. The registry holds a handful of
        // entries, so the fallback is free.
        auto bucket = s_blockers.find(MapKeyOf(who));
        if (bucket != s_blockers.end() && bucket->second.erase(guid))
        {
            if (bucket->second.empty())
                s_blockers.erase(bucket);
            s_count.fetch_sub(1, std::memory_order_relaxed);
            return;
        }

        for (auto itr = s_blockers.begin(); itr != s_blockers.end(); ++itr)
        {
            if (!itr->second.erase(guid))
                continue;
            if (itr->second.empty())
                s_blockers.erase(itr);
            s_count.fetch_sub(1, std::memory_order_relaxed);
            return;
        }
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
