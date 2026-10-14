#include "ProToolsBridge.h"

namespace
{
    constexpr int pollIntervalMs = 2000;
    constexpr int renderQuietMs = 1000; // don't talk to Pro Tools while it's bouncing

    double ticksToBeats (const juce::var& location)
    {
        return (double) location.toString().getLargeIntValue() / Bake::ticksPerBeat;
    }

    juce::var ticksLocation (double beat)
    {
        return PtslClient::object ({ { "location", juce::String ((juce::int64) std::llround (beat * Bake::ticksPerBeat)) },
                                     { "time_type", "TLType_Ticks" } });
    }

    float mapRange (float v, float fromMin, float fromMax, float toMin, float toMax)
    {
        return toMin + (juce::jlimit (fromMin, fromMax, v) - fromMin) / (fromMax - fromMin) * (toMax - toMin);
    }
}

ProToolsBridge::ProToolsBridge (Host h)
    : juce::Thread ("Panner Pro Tools bridge"), host (std::move (h))
{
}

ProToolsBridge::~ProToolsBridge()
{
    stopThread (8000); // long enough for an in-flight Pro Tools call to time out
}

void ProToolsBridge::noteOfflineRender() noexcept
{
    lastOfflineRenderMs = juce::Time::getMillisecondCounter();
}

ProToolsBridge::Report ProToolsBridge::getReport() const
{
    const juce::ScopedLock sl (lock);
    return report;
}

ProToolsBridge::Snapshot ProToolsBridge::getRevertSnapshot() const
{
    const juce::ScopedLock sl (lock);
    return pushedSnapshot;
}

void ProToolsBridge::unlink()
{
    linked = false;

    {
        const juce::ScopedLock sl (lock);
        report = {};
        report.message = "Unlinked. Pro Tools' pan lanes are the master; Panner no longer checks them.";
    }

    sendChangeMessage();
}

void ProToolsBridge::publish (Status status, const juce::String& message)
{
    {
        const juce::ScopedLock sl (lock);
        report.status = status;
        report.message = message;
        report.canRevert = ! pushedSnapshot.empty();

        if (status == Status::inSync)
            report.printedUnverified = false;
    }

    sendChangeMessage();
}

void ProToolsBridge::failPush (const juce::String& error)
{
    {
        const juce::ScopedLock sl (lock);
        report.pushError = "Push failed: " + error;
    }

    if (linked)
        check(); // the old push may or may not still match; find out
    else
        publish (Status::notLinked, "Not pushed.");
}

//==============================================================================
void ProToolsBridge::run()
{
    auto lastCheck = juce::Time::getMillisecondCounter();

    while (! threadShouldExit())
    {
        wait (200);

        const auto now = juce::Time::getMillisecondCounter();

        if (now - lastOfflineRenderMs.load() < (juce::uint32) renderQuietMs)
        {
            // Printing. Whatever was last confirmed is what's being printed.
            bool raise = false;

            {
                const juce::ScopedLock sl (lock);

                if (linked && report.status != Status::inSync && ! report.printedUnverified)
                    raise = report.printedUnverified = true;
            }

            if (raise)
                sendChangeMessage();

            continue;
        }

        if (pushRequested.exchange (false))
        {
            push();
            lastCheck = juce::Time::getMillisecondCounter();
        }
        else if (linked && (checkRequested.exchange (false) || now - lastCheck >= (juce::uint32) pollIntervalMs))
        {
            check();
            lastCheck = juce::Time::getMillisecondCounter();
        }
    }
}

bool ProToolsBridge::findControls (const juce::String& track, juce::String& error)
{
    controls.clear();
    controlsTrack = track;

    struct Axis { const char* parameter; const char* lane; int axis; };
    static constexpr Axis axes[] { { "PCParameter_X", "X", 0 }, { "PCParameter_Y", "Y", 1 },
                                   { "PCParameter_Z", "Z", 2 }, { "PCParameter_Size", "Size", 3 } };

    for (auto& a : axes)
    {
        auto controlId = PtslClient::object ({ { "section", "TSId_Panner" },
                                               { "control_type", "TCType_Pan" },
                                               { "pan", PtslClient::object ({ { "parameter", a.parameter } }) } });

        auto reply = client.call (PtslClient::getTrackControlInfo,
                                  PtslClient::object ({ { "track_names", juce::Array<juce::var> { track } },
                                                        { "control_id", controlId } }));

        if (! reply.ok)
        {
            error = reply.error;
            controls.clear();
            return false;
        }

        if (auto* infos = reply.body.getProperty ("control_info", {}).getArray())
        {
            for (auto& info : *infos)
            {
                const auto id = info.getProperty ("control_id", {});
                const auto pan = id.getProperty ("pan", {});
                const auto parameter = pan.getProperty ("parameter", a.parameter).toString();
                const auto channel = pan.getProperty ("channel", {}).toString();
                const auto min = (float) info.getProperty ("min_value", 0.0f);
                const auto max = (float) info.getProperty ("max_value", 0.0f);

                if (parameter != a.parameter || max <= min)
                    continue;

                const float side = channel == "SChannel_Left" ? -1.0f : (channel == "SChannel_Right" ? 1.0f : 0.0f);
                const juce::String lane = juce::String (a.lane) + (side < 0.0f ? " (L)" : (side > 0.0f ? " (R)" : ""));

                controls.push_back ({ id, lane, a.axis, side, min, max });
            }
        }
    }

    if (std::none_of (controls.begin(), controls.end(), [] (const Control& c) { return c.axis == 0; }))
    {
        error = "Pro Tools reports no 3D object pan controls on track \"" + track
              + "\". Assign the track's output to a Dolby Atmos object.";
        controls.clear();
        return false;
    }

    return true;
}

Bake::Curve ProToolsBridge::bakeFor (const Pattern& pattern, const Control& c, double start, double end) const
{
    return Bake::curve (pattern, start, end, [&] (Position p)
    {
        switch (c.axis)
        {
            case 0:  return mapRange (juce::jlimit (-1.0f, 1.0f, p.x + c.stereoSide * pattern.width), -1.0f, 1.0f, c.min, c.max);
            case 1:  return mapRange (frontIsPositiveY ? p.y : -p.y, -1.0f, 1.0f, c.min, c.max);
            case 2:  return mapRange (p.z, 0.0f, 1.0f, c.min, c.max);
            default: return mapRange (pattern.size, 0.0f, 1.0f, c.min, c.max);
        }
    });
}

void ProToolsBridge::push()
{
    publish (Status::busy, "Pushing to Pro Tools...");

    const auto track = host.trackName();

    if (track.isEmpty())
        return failPush ("Pro Tools hasn't told Panner which track it's on yet.");

    auto selection = client.call (PtslClient::getTimelineSelection,
                                  PtslClient::object ({ { "location_type", "TLType_Ticks" } }));

    if (! selection.ok)
        return failPush (selection.error);

    const double start = ticksToBeats (selection.body.getProperty ("in_time", {}));
    const double end = ticksToBeats (selection.body.getProperty ("out_time", {}));

    if (end <= start)
        return failPush ("Select the part of the Pro Tools timeline to print, then push again.");

    juce::String error;

    if (! findControls (track, error))
        return failPush (error);

    const auto pattern = host.readPattern();

    for (auto& c : controls)
    {
        juce::Array<juce::var> breakpoints;

        for (auto& p : bakeFor (pattern, c, start, end))
            breakpoints.add (PtslClient::object ({ { "time", ticksLocation (p.beat) }, { "value", p.value } }));

        auto reply = client.call (PtslClient::setTrackControlBreakpoints,
                                  PtslClient::object ({ { "track_name", track },
                                                        { "control_id", c.id },
                                                        { "breakpoints", breakpoints },
                                                        { "start_time", ticksLocation (start) },
                                                        { "end_time", ticksLocation (end) } }));

        if (! reply.ok)
            return failPush ("Pro Tools refused the " + c.lane + " lane: " + reply.error);
    }

    {
        const juce::ScopedLock sl (lock);
        startBeat = start;
        endBeat = end;
        pushedSnapshot = host.snapshot();
        report.pushError.clear();
    }

    linked = true;
    check(); // read it straight back: a push only counts once Pro Tools shows it
}

void ProToolsBridge::check()
{
    const auto track = host.trackName();

    if (track.isEmpty())
        return publish (Status::cantVerify, "Pro Tools hasn't told Panner which track it's on.");

    if (controls.empty() || track != controlsTrack)
    {
        juce::String error;

        if (! findControls (track, error))
            return publish (Status::cantVerify, error);
    }

    double start, end;

    {
        const juce::ScopedLock sl (lock);
        start = startBeat;
        end = endBeat;
    }

    const auto pattern = host.readPattern();
    juce::StringArray drifted;

    for (auto& c : controls)
    {
        auto reply = client.call (PtslClient::getTrackControlBreakpoints,
                                  PtslClient::object ({ { "track_name", track }, { "control_id", c.id } }));

        if (! reply.ok)
        {
            const auto message = "Can't read the " + c.lane + " lane from Pro Tools: " + reply.error;
            controls.clear(); // c dies here; Pro Tools gets asked afresh next time
            return publish (Status::cantVerify, message);
        }

        Bake::Curve actual;

        if (auto* points = reply.body.getProperty ("breakpoints", {}).getArray())
        {
            for (auto& p : *points)
            {
                const auto time = p.getProperty ("time", {});
                const auto type = time.getProperty ("time_type", "TLType_Ticks").toString();

                if (type != "TLType_Ticks")
                    return publish (Status::cantVerify, "Pro Tools returned " + c.lane + " breakpoints as " + type
                                                        + "; Panner can only compare ticks.");

                actual.push_back ({ ticksToBeats (time.getProperty ("location", {})), (float) p.getProperty ("value", 0.0f) });
            }
        }

        std::sort (actual.begin(), actual.end(), [] (const Bake::Point& a, const Bake::Point& b) { return a.beat < b.beat; });

        const float tolerance = 0.01f * (c.max - c.min);

        if (! Bake::matches (bakeFor (pattern, c, start, end), actual, start, end, tolerance))
            drifted.add (c.lane);
    }

    if (drifted.isEmpty())
        publish (Status::inSync, "In sync with \"" + track + "\" in Pro Tools.");
    else
        publish (Status::drifted, "Pro Tools' pan automation on \"" + track + "\" no longer matches Panner. Lanes that differ: "
                                  + drifted.joinIntoString (", ") + ".");
}

//==============================================================================
juce::ValueTree ProToolsBridge::saveState() const
{
    const juce::ScopedLock sl (lock);

    juce::ValueTree state ("ProToolsBridge");
    state.setProperty ("linked", linked.load(), nullptr);
    state.setProperty ("startBeat", startBeat, nullptr);
    state.setProperty ("endBeat", endBeat, nullptr);

    juce::ValueTree snapshot ("Snapshot");

    for (auto& [id, value] : pushedSnapshot)
        snapshot.setProperty (id, value, nullptr);

    state.appendChild (snapshot, nullptr);
    return state;
}

void ProToolsBridge::loadState (const juce::ValueTree& state)
{
    if (! state.hasType ("ProToolsBridge"))
        return;

    {
        const juce::ScopedLock sl (lock);
        startBeat = state.getProperty ("startBeat", 0.0);
        endBeat = state.getProperty ("endBeat", 0.0);
        pushedSnapshot.clear();

        const auto snapshot = state.getChildWithName ("Snapshot");

        for (int i = 0; i < snapshot.getNumProperties(); ++i)
        {
            const auto id = snapshot.getPropertyName (i);
            pushedSnapshot.push_back ({ id.toString(), (float) snapshot.getProperty (id) });
        }

        report = {};
        report.canRevert = ! pushedSnapshot.empty();
    }

    linked = (bool) state.getProperty ("linked", false);

    if (linked)
    {
        publish (Status::cantVerify, "Checking Pro Tools...");
        requestCheck();
    }
    else
    {
        sendChangeMessage();
    }
}
