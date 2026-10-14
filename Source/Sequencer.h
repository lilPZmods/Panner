#pragma once

#include "SpatialPanner.h"

/** Tempo-synced step sequencer for source position. Stateless: the position is a pure
    function of the host's musical position, so offline bounces match realtime playback. */
struct Sequencer
{
    static constexpr int maxSteps = 16;
    using Steps = std::array<Position, maxSteps>;

    static inline const juce::StringArray rateNames { "1/1", "1/2", "1/4", "1/8", "1/16", "1/32",
                                                      "1/4T", "1/8T", "1/16T", "1/4.", "1/8." };

    /** Step length in quarter notes for an index into rateNames. */
    static double rateInBeats (int rateIndex)
    {
        static constexpr double beats[] { 4.0, 2.0, 1.0, 0.5, 0.25, 0.125,
                                          2.0 / 3.0, 1.0 / 3.0, 1.0 / 6.0, 1.5, 0.75 };
        return beats[juce::jlimit (0, (int) std::size (beats) - 1, rateIndex)];
    }

    static int stepAt (double ppq, double stepBeats, int numSteps)
    {
        const auto index = (juce::int64) std::floor (ppq / stepBeats);
        return (int) (((index % numSteps) + numSteps) % numSteps); // ppq is negative during pre-roll
    }

    /** glide is the fraction (0..1) of each step spent travelling to the next one. */
    static Position positionAt (double ppq, double stepBeats, int numSteps, float glide, const Steps& steps)
    {
        const double t = ppq / stepBeats;
        const auto phase = (float) (t - std::floor (t));
        const int current = stepAt (ppq, stepBeats, numSteps);
        const float holdEnd = 1.0f - glide;

        if (glide <= 0.0f || phase <= holdEnd)
            return steps[(size_t) current];

        const auto& a = steps[(size_t) current];
        const auto& b = steps[(size_t) ((current + 1) % numSteps)];
        const float mix = (phase - holdEnd) / glide;

        return { a.x + (b.x - a.x) * mix,
                 a.y + (b.y - a.y) * mix,
                 a.z + (b.z - a.z) * mix };
    }
};
