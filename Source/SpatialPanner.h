#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

/** Room-relative source position, matching the Dolby panner convention:
    x: -1 (left) .. 1 (right), y: -1 (back) .. 1 (front), z: 0 (floor) .. 1 (ceiling). */
struct Position
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

/** Maps a host channel layout onto room positions and computes constant-power
    gains for a source. Speakers are grouped into a floor and a top layer; each
    layer is panned front/back between rows and left/right within a row. */
class SpeakerLayout
{
public:
    static constexpr int maxChannels = 16; // 9.1.6

    struct Speaker
    {
        int channel;
        float x, y;
        bool top;
    };

    void set (const juce::AudioChannelSet& channelSet)
    {
        speakers.clear();
        floorRows.clear();
        topRows.clear();
        numChannels = juce::jmin (channelSet.size(), maxChannels);

        for (int ch = 0; ch < numChannels; ++ch)
        {
            Speaker s { ch, 0.0f, 0.0f, false };

            if (! locate (channelSet.getTypeOfChannel (ch), s))
                continue; // LFE and anything unknown gets no panned signal

            speakers.push_back (s);
            addToRows (s.top ? topRows : floorRows, s);
        }
    }

    int getNumChannels() const                        { return numChannels; }
    const std::vector<Speaker>& getSpeakers() const   { return speakers; }

    /** Writes one gain per output channel. size 0 = point source, 1 = all speakers equally.
        Allocation-free; safe on the audio thread. */
    void computeGains (Position p, float size, float* gains) const
    {
        std::fill (gains, gains + numChannels, 0.0f);

        if (speakers.empty())
            return;

        const float z = topRows.empty() ? 0.0f
                                        : (floorRows.empty() ? 1.0f : juce::jlimit (0.0f, 1.0f, p.z));

        // Accumulate power weights (summing to 1), then take the root for constant power.
        panLayer (floorRows, p.x, p.y, 1.0f - z, gains);
        panLayer (topRows, p.x, p.y, z, gains);

        const float spread = size / (float) speakers.size();

        for (auto& s : speakers)
            gains[s.channel] = std::sqrt (gains[s.channel] * (1.0f - size) + spread);
    }

private:
    struct Row
    {
        float y;
        std::vector<Speaker> speakers; // sorted left to right
    };

    static bool locate (juce::AudioChannelSet::ChannelType type, Speaker& s)
    {
        using CS = juce::AudioChannelSet;

        auto at = [&s] (float x, float y, bool top) { s.x = x; s.y = y; s.top = top; return true; };

        JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wswitch-enum")
        switch (type)
        {
            case CS::left:               return at (-1.0f,  1.0f, false);
            case CS::right:              return at ( 1.0f,  1.0f, false);
            case CS::centre:             return at ( 0.0f,  1.0f, false);
            case CS::wideLeft:           return at (-1.0f,  0.5f, false);
            case CS::wideRight:          return at ( 1.0f,  0.5f, false);
            case CS::leftSurroundSide:   return at (-1.0f,  0.0f, false);
            case CS::rightSurroundSide:  return at ( 1.0f,  0.0f, false);
            case CS::leftSurround:
            case CS::leftSurroundRear:   return at (-1.0f, -1.0f, false);
            case CS::rightSurround:
            case CS::rightSurroundRear:  return at ( 1.0f, -1.0f, false);
            case CS::topFrontLeft:       return at (-1.0f,  1.0f, true);
            case CS::topFrontRight:      return at ( 1.0f,  1.0f, true);
            case CS::topSideLeft:        return at (-1.0f,  0.0f, true);
            case CS::topSideRight:       return at ( 1.0f,  0.0f, true);
            case CS::topRearLeft:        return at (-1.0f, -1.0f, true);
            case CS::topRearRight:       return at ( 1.0f, -1.0f, true);
            default:                     return false;
        }
        JUCE_END_IGNORE_WARNINGS_GCC_LIKE
    }

    static void addToRows (std::vector<Row>& rows, const Speaker& s)
    {
        auto row = std::find_if (rows.begin(), rows.end(), [&s] (const Row& r) { return juce::exactlyEqual (r.y, s.y); });

        if (row == rows.end())
        {
            row = rows.insert (std::find_if (rows.begin(), rows.end(), [&s] (const Row& r) { return r.y > s.y; }),
                               Row { s.y, {} });
        }

        auto& list = row->speakers;
        list.insert (std::find_if (list.begin(), list.end(), [&s] (const Speaker& o) { return o.x > s.x; }), s);
    }

    /** Finds the pair of sorted entries surrounding pos: index of the lower one and the
        0..1 fraction towards the next. Clamps outside the covered range. */
    template <typename Container, typename GetCoord>
    static void bracket (const Container& c, float pos, GetCoord coord, size_t& index, float& fraction)
    {
        index = 0;
        fraction = 0.0f;

        if (pos <= coord (c.front()))
            return;

        if (pos >= coord (c.back()))
        {
            index = c.size() - 1;
            return;
        }

        while (coord (c[index + 1]) <= pos)
            ++index;

        fraction = (pos - coord (c[index])) / (coord (c[index + 1]) - coord (c[index]));
    }

    static void panRow (const Row& row, float x, float weight, float* gains)
    {
        size_t i;
        float t;
        bracket (row.speakers, x, [] (const Speaker& s) { return s.x; }, i, t);

        gains[row.speakers[i].channel] += weight * (1.0f - t);

        if (t > 0.0f)
            gains[row.speakers[i + 1].channel] += weight * t;
    }

    static void panLayer (const std::vector<Row>& rows, float x, float y, float weight, float* gains)
    {
        if (rows.empty() || weight <= 0.0f)
            return;

        size_t i;
        float t;
        bracket (rows, y, [] (const Row& r) { return r.y; }, i, t);

        panRow (rows[i], x, weight * (1.0f - t), gains);

        if (t > 0.0f)
            panRow (rows[i + 1], x, weight * t, gains);
    }

    int numChannels = 0;
    std::vector<Speaker> speakers;
    std::vector<Row> floorRows, topRows; // sorted back to front
};
