#include "PluginProcessor.h"
#include "PluginEditor.h"

PannerProcessor::PannerProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::create7point1point4(), true)),
      apvts (*this, nullptr, "state", createParameters()),
      bridge ({ [this] { return readPattern(); },
                [this] { return getTrackName(); },
                [this] { return snapshotParameters(); } })
{
    auto raw = [this] (const juce::String& id) { return apvts.getRawParameterValue (id); };

    x = raw ("x");
    y = raw ("y");
    z = raw ("z");
    size = raw ("size");
    width = raw ("width");
    gain = raw ("gain");
    seqOn = raw ("seqOn");
    seqSteps = raw ("seqSteps");
    seqRate = raw ("seqRate");
    seqGlide = raw ("seqGlide");
    renderMode = raw ("renderMode");

    for (int i = 0; i < Sequencer::maxSteps; ++i)
        stepParams[(size_t) i] = { raw (stepParamId (i, 'x')), raw (stepParamId (i, 'y')), raw (stepParamId (i, 'z')) };

    // Any change to Panner after a push gets checked against Pro Tools straight away.
    for (auto* p : getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p))
            apvts.addParameterListener (ranged->getParameterID(), this);

    bridge.addChangeListener (this);
    bridge.start();
}

PannerProcessor::~PannerProcessor()
{
    bridge.removeChangeListener (this);
    cancelPendingUpdate();
    driftWindow.reset();

    for (auto* p : getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p))
            apvts.removeParameterListener (ranged->getParameterID(), this);
}

juce::AudioProcessorValueTreeState::ParameterLayout PannerProcessor::createParameters()
{
    using Float = juce::AudioParameterFloat;
    using Range = juce::NormalisableRange<float>;

    auto id = [] (const juce::String& name) { return juce::ParameterID { name, 1 }; };

    juce::AudioProcessorValueTreeState::ParameterLayout params;

    params.add (std::make_unique<juce::AudioParameterChoice> (id ("renderMode"), "Render",
                                                              juce::StringArray { "Object (Pro Tools pans)", "Bed (Panner pans)" }, 0),
                std::make_unique<Float> (id ("x"), "X", Range (-1.0f, 1.0f), 0.0f),
                std::make_unique<Float> (id ("y"), "Y", Range (-1.0f, 1.0f), 1.0f),
                std::make_unique<Float> (id ("z"), "Z", Range (0.0f, 1.0f), 0.0f),
                std::make_unique<Float> (id ("size"), "Size", Range (0.0f, 1.0f), 0.0f),
                std::make_unique<Float> (id ("width"), "Stereo Width", Range (0.0f, 1.0f), 1.0f),
                std::make_unique<Float> (id ("gain"), "Gain", Range (-60.0f, 12.0f, 0.1f), 0.0f,
                                         juce::AudioParameterFloatAttributes().withLabel ("dB")),
                std::make_unique<juce::AudioParameterBool> (id ("seqOn"), "Sequencer", false),
                std::make_unique<juce::AudioParameterInt> (id ("seqSteps"), "Steps", 1, Sequencer::maxSteps, 4),
                std::make_unique<juce::AudioParameterChoice> (id ("seqRate"), "Rate", Sequencer::rateNames, 2),
                std::make_unique<Float> (id ("seqGlide"), "Glide", Range (0.0f, 1.0f), 0.0f));

    // Default pattern: clockwise around the room corners.
    static constexpr float cornerX[] { -1.0f, 1.0f, 1.0f, -1.0f };
    static constexpr float cornerY[] { 1.0f, 1.0f, -1.0f, -1.0f };

    for (int i = 0; i < Sequencer::maxSteps; ++i)
    {
        const auto name = "Step " + juce::String (i + 1);

        params.add (std::make_unique<Float> (id (stepParamId (i, 'x')), name + " X", Range (-1.0f, 1.0f), cornerX[i % 4]),
                    std::make_unique<Float> (id (stepParamId (i, 'y')), name + " Y", Range (-1.0f, 1.0f), cornerY[i % 4]),
                    std::make_unique<Float> (id (stepParamId (i, 'z')), name + " Z", Range (0.0f, 1.0f), 0.0f));
    }

    return params;
}

bool PannerProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    using CS = juce::AudioChannelSet;

    const auto in = layouts.getMainInputChannelSet();
    const auto out = layouts.getMainOutputChannelSet();

    if (in != CS::mono() && in != CS::stereo())
        return false;

    if (out == in)
        return true; // insert on a Pro Tools object track

    static const CS outputs[] { CS::stereo(),
                                CS::create5point1(),
                                CS::create7point1(),
                                CS::create5point1point2(),
                                CS::create5point1point4(),
                                CS::create7point1point2(),
                                CS::create7point1point4(),
                                CS::create7point1point6(),
                                CS::create9point1point6() };

    return std::find (std::begin (outputs), std::end (outputs), out) != std::end (outputs);
}

void PannerProcessor::prepareToPlay (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    layout.set (getBusesLayout().getMainOutputChannelSet());
    snapGains = true;
}

Pattern PannerProcessor::readPattern() const
{
    Pattern p;
    p.manual = { x->load(), y->load(), z->load() };
    p.size = size->load();
    p.width = width->load();
    p.sequenced = seqOn->load() > 0.5f;
    p.numSteps = juce::jlimit (1, Sequencer::maxSteps, juce::roundToInt (seqSteps->load()));
    p.stepBeats = Sequencer::rateInBeats (juce::roundToInt (seqRate->load()));
    p.glide = seqGlide->load();

    for (size_t i = 0; i < (size_t) p.numSteps; ++i)
        p.steps[i] = { stepParams[i][0]->load(), stepParams[i][1]->load(), stepParams[i][2]->load() };

    return p;
}

void PannerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    if (isNonRealtime() && bridge.isLinked())
        bridge.noteOfflineRender();

    const int numIn = juce::jmin (getTotalNumInputChannels(), maxInputs);
    const int numOut = juce::jmin (layout.getNumChannels(), buffer.getNumChannels());
    const int numSamples = buffer.getNumSamples();

    // The sequencer follows the host timeline rather than counting samples, so a bounce
    // from any start point (realtime or offline) lands on the same positions.
    double ppq = 0.0, ppqPerSample = 0.0;
    bool hasTimeline = false;

    if (auto* playHead = getPlayHead())
    {
        if (auto info = playHead->getPosition())
        {
            if (auto hostPpq = info->getPpqPosition())
            {
                ppq = *hostPpq;
                hasTimeline = true;

                if (info->getIsPlaying())
                    if (auto bpm = info->getBpm())
                        ppqPerSample = *bpm / (60.0 * sampleRate);
            }
        }
    }

    auto pattern = readPattern();
    pattern.sequenced = pattern.sequenced && hasTimeline;

    auto publishLive = [&] (Position p)
    {
        liveX = p.x;
        liveY = p.y;
        liveZ = p.z;
        liveStep = pattern.sequenced ? Sequencer::stepAt (ppq, pattern.stepBeats, pattern.numSteps) : -1;
    };

    if (renderMode->load() < 0.5f)
    {
        // Object mode: Pro Tools' own panner places the audio from the pushed lanes.
        for (int ch = numIn; ch < buffer.getNumChannels(); ++ch)
            buffer.clear (ch, 0, numSamples);

        publishLive (pattern.at (ppq));
        return;
    }

    const float halfWidth = numIn == 2 ? pattern.width : 0.0f;
    const float outputGain = juce::Decibels::decibelsToGain (gain->load(), -60.0f);

    Position position = pattern.manual;
    float input[maxInputs][chunkSize];
    float target[SpeakerLayout::maxChannels];

    for (int start = 0; start < numSamples; start += chunkSize)
    {
        const int n = juce::jmin (chunkSize, numSamples - start);
        position = pattern.at (ppq + (start + n) * ppqPerSample);

        // Input and output share the buffer, so take a copy before overwriting it.
        for (int ch = 0; ch < numIn; ++ch)
            std::copy_n (buffer.getReadPointer (ch, start), n, input[ch]);

        for (int ch = 0; ch < buffer.getNumChannels(); ++ch)
            buffer.clear (ch, start, n);

        // ~10 ms one-pole on the gains keeps hard steps and parameter jumps click-free.
        const float smoothing = snapGains ? 1.0f : 1.0f - std::exp (-(float) n / (0.010f * (float) sampleRate));

        for (int ch = 0; ch < numIn; ++ch)
        {
            auto source = position;
            source.x = juce::jlimit (-1.0f, 1.0f, source.x + (ch == 0 ? -halfWidth : halfWidth));
            layout.computeGains (source, pattern.size, target);

            auto& current = gains[(size_t) ch];

            for (int out = 0; out < numOut; ++out)
            {
                const float startGain = snapGains ? target[out] * outputGain : current[(size_t) out];
                const float endGain = startGain + (target[out] * outputGain - startGain) * smoothing;
                current[(size_t) out] = endGain;

                if (juce::exactlyEqual (startGain, 0.0f) && juce::exactlyEqual (endGain, 0.0f))
                    continue;

                buffer.addFromWithRamp (out, start, input[ch], n, startGain, endGain);
            }
        }

        snapGains = false;
    }

    publishLive (position);
}

juce::AudioProcessorEditor* PannerProcessor::createEditor()
{
    return new PannerEditor (*this);
}

//==============================================================================
ProToolsBridge::Snapshot PannerProcessor::snapshotParameters() const
{
    ProToolsBridge::Snapshot snapshot;

    for (auto* p : getParameters())
        if (auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (p))
            snapshot.push_back ({ ranged->getParameterID(), ranged->convertFrom0to1 (ranged->getValue()) });

    return snapshot;
}

void PannerProcessor::revertToLastPush()
{
    for (auto& [id, value] : bridge.getRevertSnapshot())
    {
        if (auto* p = apvts.getParameter (id))
        {
            p->beginChangeGesture();
            p->setValueNotifyingHost (p->convertTo0to1 (value));
            p->endChangeGesture();
        }
    }

    bridge.requestCheck();
}

juce::String PannerProcessor::getTrackName() const
{
    const juce::ScopedLock sl (trackNameLock);
    return trackName;
}

void PannerProcessor::updateTrackProperties (const TrackProperties& properties)
{
    {
        const juce::ScopedLock sl (trackNameLock);
        trackName = properties.name.value_or (juce::String());
    }

    bridge.requestCheck();
}

void PannerProcessor::updatePrompt()
{
    const auto report = bridge.getReport();

    // With the editor open the prompt covers the editor; otherwise it gets its own window.
    if (! report.needsAnswer() || getActiveEditor() != nullptr)
    {
        driftWindow.reset();
        return;
    }

    if (driftWindow == nullptr)
    {
        auto* prompt = new DriftPrompt();
        prompt->onPush = [this] { pushToProTools(); };
        prompt->onRevert = [this] { revertToLastPush(); };
        prompt->onUnlink = [this] { unlinkFromProTools(); };
        driftWindow = std::make_unique<DriftWindow> (prompt);
    }

    driftWindow->getPrompt().setReport (report);
}

//==============================================================================
void PannerProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    auto state = apvts.copyState();
    state.appendChild (bridge.saveState(), nullptr);

    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void PannerProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    auto xml = getXmlFromBinary (data, sizeInBytes);

    if (xml == nullptr || ! xml->hasTagName (apvts.state.getType()))
        return;

    auto state = juce::ValueTree::fromXml (*xml);
    const auto bridgeState = state.getChildWithName ("ProToolsBridge");
    state.removeChild (bridgeState, nullptr);

    apvts.replaceState (state);
    bridge.loadState (bridgeState);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PannerProcessor();
}
