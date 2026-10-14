#pragma once

#include "Sequencer.h"

/** Everything that decides where the source is at a given musical time. */
struct Pattern
{
    Position manual;
    float size = 0.0f;
    float width = 0.0f;   // stereo offset each side, applied to x
    bool sequenced = false;
    int numSteps = 1;
    double stepBeats = 1.0;
    float glide = 0.0f;
    Sequencer::Steps steps {};

    Position at (double beat) const
    {
        return sequenced ? Sequencer::positionAt (beat, stepBeats, numSteps, glide, steps) : manual;
    }
};

/** Turns a pattern into automation breakpoints, and checks breakpoints against a pattern.

    Between the times returned by keyTimes() the pattern only ever moves in straight lines,
    so a host that joins breakpoints with straight lines reproduces it exactly. */
namespace Bake
{
    constexpr double ticksPerBeat = 960000.0; // Pro Tools ticks per quarter note
    constexpr double oneTick = 1.0 / ticksPerBeat;

    struct Point
    {
        double beat;
        float value;
    };

    using Curve = std::vector<Point>;

    inline std::vector<double> keyTimes (const Pattern& p, double start, double end)
    {
        std::vector<double> times { start, end };

        if (p.sequenced)
        {
            for (double k = std::floor (start / p.stepBeats); k * p.stepBeats <= end; k += 1.0)
            {
                const double boundary = k * p.stepBeats;

                if (p.glide > 0.0f)
                {
                    times.push_back (boundary);                                    // arrive at step
                    times.push_back (boundary + (1.0 - p.glide) * p.stepBeats);    // start gliding on
                }
                else
                {
                    times.push_back (boundary - oneTick);                          // last tick of previous step
                    times.push_back (boundary);                                    // jump
                }
            }
        }

        times.erase (std::remove_if (times.begin(), times.end(), [=] (double t) { return t < start || t > end; }),
                     times.end());
        std::sort (times.begin(), times.end());
        times.erase (std::unique (times.begin(), times.end(), [] (double a, double b) { return b - a < oneTick * 0.5; }),
                     times.end());
        return times;
    }

    inline Curve curve (const Pattern& p, double start, double end, const std::function<float (Position)>& value)
    {
        Curve c;

        for (auto t : keyTimes (p, start, end))
            c.push_back ({ t, value (p.at (t)) });

        return c;
    }

    /** Value of a curve joined with straight lines, held flat beyond its ends. */
    inline float valueAt (const Curve& c, double beat)
    {
        if (c.empty())
            return 0.0f;

        if (beat <= c.front().beat)
            return c.front().value;

        if (beat >= c.back().beat)
            return c.back().value;

        auto next = std::upper_bound (c.begin(), c.end(), beat, [] (double b, const Point& p) { return b < p.beat; });
        auto prev = std::prev (next);
        const double span = next->beat - prev->beat;

        if (span <= 0.0)
            return next->value;

        return prev->value + (next->value - prev->value) * (float) ((beat - prev->beat) / span);
    }

    /** True when the two curves agree everywhere in [start, end] to within tolerance. Allows a
        couple of ticks of timing slop so a host rounding breakpoint times isn't called drift. */
    inline bool matches (const Curve& expected, const Curve& actual, double start, double end, float tolerance)
    {
        if (actual.empty())
            return false;

        constexpr double slop = 2.0 * oneTick;

        auto agreesAt = [&] (const Curve& a, const Curve& b, double t)
        {
            const float v = valueAt (a, t);

            for (double offset : { 0.0, -slop, slop })
                if (std::abs (valueAt (b, t + offset) - v) <= tolerance)
                    return true;

            return false;
        };

        for (auto& p : expected)
            if (p.beat >= start && p.beat <= end && ! agreesAt (expected, actual, p.beat))
                return false;

        for (auto& p : actual)
            if (p.beat >= start && p.beat <= end && ! agreesAt (actual, expected, p.beat))
                return false;

        return true;
    }
}
