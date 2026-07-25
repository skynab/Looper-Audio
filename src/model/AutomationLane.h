#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

namespace looper::model
{
struct AutomationPoint
{
    double beat  = 0.0;
    float  value = 0.0f;

    bool operator==(const AutomationPoint&) const = default;
};

/**
    A parameter automation lane: breakpoints of (beat, value), kept sorted by
    beat, with linear interpolation between them. Values before the first point
    hold the first value; after the last, the last. JUCE-free and unit-tested.
*/
class AutomationLane
{
public:
    void clear() { points_.clear(); }
    bool empty() const { return points_.empty(); }

    const std::vector<AutomationPoint>& points() const { return points_; }

    /** Adds a point, keeping the lane sorted; replaces the value at a coincident beat. */
    void addPoint(double beat, float value)
    {
        auto it = std::lower_bound(points_.begin(), points_.end(), beat,
                                   [](const AutomationPoint& p, double b) { return p.beat < b; });

        if (it != points_.end() && std::abs(it->beat - beat) < 1.0e-9)
            it->value = value;
        else
            points_.insert(it, AutomationPoint { beat, value });
    }

    /** Interpolated value at @p beat; @p fallback when the lane is empty. */
    float valueAt(double beat, float fallback = 0.0f) const
    {
        if (points_.empty())
            return fallback;
        if (beat <= points_.front().beat)
            return points_.front().value;
        if (beat >= points_.back().beat)
            return points_.back().value;

        auto hi = std::lower_bound(points_.begin(), points_.end(), beat,
                                   [](const AutomationPoint& p, double b) { return p.beat < b; });
        const AutomationPoint& upper = *hi;
        const AutomationPoint& lower = *(hi - 1);

        const double span = upper.beat - lower.beat;
        if (span <= 0.0)
            return lower.value;

        const double t = (beat - lower.beat) / span;
        return (float) (lower.value + (upper.value - lower.value) * t);
    }

    bool operator==(const AutomationLane&) const = default;

private:
    std::vector<AutomationPoint> points_; // sorted by beat
};

} // namespace looper::model
