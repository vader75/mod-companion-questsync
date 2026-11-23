#pragma once

#include <cmath>

namespace VendorErrandPolicy
{
    constexpr unsigned FreeSlotThreshold = 8;
    constexpr float Leash = 12.0f;
    constexpr float MaxPathLength = 14.0f;
    constexpr float ArrivalTolerance = 0.75f;
    constexpr float StopTolerance = 0.1f;

    struct Point { float x, y, z; };
    struct Stop { unsigned map, instance; Point position; };

    inline float DistanceSquared(Point a, Point b)
    {
        float const dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
        return dx * dx + dy * dy + dz * dz;
    }

    inline bool Within(Point a, Point b, float radius)
    {
        return DistanceSquared(a, b) <= radius * radius; // NaNs fail closed
    }

    inline bool SameStop(Stop a, Stop b)
    {
        return a.map == b.map && a.instance == b.instance && Within(a.position, b.position, StopTolerance);
    }

    inline bool MayStart(unsigned freeSlots, bool ownerAvailable)
    {
        return freeSlots < FreeSlotThreshold && ownerAvailable;
    }

    struct CanceledWalk { Stop ownerStart, botStop; };

    inline bool ChangedStop(Stop before, Stop after, float minDistance)
    {
        return before.map != after.map || before.instance != after.instance ||
            DistanceSquared(before.position, after.position) >= minDistance * minDistance;
    }

    inline bool MayRearm(CanceledWalk canceled, Stop owner, Stop bot, bool ownerSettled, bool followSettled)
    {
        // Turning in place, combat ending, reopening a merchant, or a timeout is not a new stop.
        return ownerSettled && followSettled && ChangedStop(canceled.ownerStart, owner, 3.0f) &&
            ChangedStop(canceled.botStop, bot, 1.0f);
    }

    inline bool AllowsOrdinarySale(bool leased, bool canceled)
    {
        return !leased && !canceled; // applies to every merchant, not just the errand target
    }

    template <class Points>
    bool IsBoundedPath(Points const& points, Point start, Point owner, Point destination)
    {
        if (points.size() < 2)
            return false;
        auto point = [](auto const& p) { return Point{p.x, p.y, p.z}; };
        if (!Within(point(points.front()), start, ArrivalTolerance) ||
            !Within(point(points.back()), destination, ArrivalTolerance))
            return false;
        float length = 0.0f;
        Point last = start;
        for (auto const& p : points)
        {
            Point const next = point(p);
            if (!Within(next, owner, Leash))
                return false;
            length += std::sqrt(DistanceSquared(last, next));
            last = next;
        }
        return length > ArrivalTolerance && length <= MaxPathLength;
    }

    enum class Step { Cancel, Wait, Sell };

    inline Step NextStep(bool safe, bool sameSpline, bool finalized, bool arrived)
    {
        if (!safe || !sameSpline)
            return Step::Cancel;
        if (!finalized)
            return Step::Wait;
        return arrived ? Step::Sell : Step::Cancel;
    }
}
