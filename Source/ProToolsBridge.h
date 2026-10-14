#pragma once

#include <juce_events/juce_events.h>
#include <juce_data_structures/juce_data_structures.h>
#include "Bake.h"
#include "PtslClient.h"

/** Keeps the plugin's pattern and the track's native Pro Tools pan automation in step.

    Push writes the pattern into the track's X / Y / Z / Size lanes over the Pro Tools
    timeline selection. After that the lanes are read back every couple of seconds (and
    right after any Panner parameter changes) and compared with the pattern. Any
    difference, from either side, is reported as drift and needs an answer. */
class ProToolsBridge : private juce::Thread,
                       public juce::ChangeBroadcaster
{
public:
    enum class Status { notLinked, busy, inSync, drifted, cantVerify };

    struct Report
    {
        Status status = Status::notLinked;
        juce::String message;
        juce::String pushError;
        bool printedUnverified = false; // an offline bounce ran while not confirmed in sync
        bool canRevert = false;

        bool needsAnswer() const
        {
            return status == Status::drifted || (printedUnverified && status != Status::inSync);
        }
    };

    using Snapshot = std::vector<std::pair<juce::String, float>>; // parameter ID, value

    /** What the bridge reads from the plugin. Called on the bridge thread. */
    struct Host
    {
        std::function<Pattern()> readPattern;
        std::function<juce::String()> trackName;
        std::function<Snapshot()> snapshot;
    };

    explicit ProToolsBridge (Host);
    ~ProToolsBridge() override;

    void start()                                { startThread(); }

    void requestPush()                          { pushRequested = true; notify(); }
    void requestCheck() noexcept                { checkRequested = true; }      // any thread, including audio
    void noteOfflineRender() noexcept;                                          // audio thread
    void unlink();

    bool isLinked() const noexcept              { return linked; }
    Report getReport() const;
    Snapshot getRevertSnapshot() const;

    juce::ValueTree saveState() const;
    void loadState (const juce::ValueTree&);

    /** Pro Tools 3D panner range of motion, assumed to have front at positive Y. */
    static constexpr bool frontIsPositiveY = true;

private:
    struct Control
    {
        juce::var id;          // PTSL TrackControlId, as Pro Tools described it
        juce::String lane;
        int axis;              // 0 x, 1 y, 2 z, 3 size
        float stereoSide;      // -1 left channel, 1 right, 0 mono
        float min, max;
    };

    void run() override;
    void push();
    void check();
    bool findControls (const juce::String& track, juce::String& error);
    Bake::Curve bakeFor (const Pattern&, const Control&, double start, double end) const;
    void publish (Status, const juce::String& message);
    void failPush (const juce::String& error);

    Host host;
    PtslClient client;                  // bridge thread only
    std::vector<Control> controls;      // bridge thread only
    juce::String controlsTrack;         // bridge thread only

    mutable juce::CriticalSection lock;
    Report report;
    Snapshot pushedSnapshot;
    double startBeat = 0.0, endBeat = 0.0;

    std::atomic<bool> linked { false }, pushRequested { false }, checkRequested { false };
    std::atomic<juce::uint32> lastOfflineRenderMs { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ProToolsBridge)
};
