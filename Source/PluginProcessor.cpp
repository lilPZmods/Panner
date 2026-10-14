#include "PluginProcessor.h"
#include "PluginEditor.h"

PannerProcessor::PannerProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::create7point1point4(), true)),
      apvts (*this, nullptr, "state", createParameters())
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

    for (int i = 0; i < Sequencer::maxSteps; ++i)
        stepParams[(size_t) i] = { raw (stepParamId (i, 'x')), raw (stepParamId (i, 'y')), raw (stepParamId (i, 'z')) };
}

juce::AudioProcessorValueTreeState::ParameterLayout PannerProcessor::createParameters()
{
    using Float = juce::AudioParameterFloat;
    using Range = juce::NormalisableRange<float>;

    auto id = [] (const juce::String& name) { return juce::ParameterID { name, 1 }; };

    juce::AudioProcessorValueTreeState::ParameterLayout params;

    params.add (std::make_unique<Float> (id ("x"), "X", Range (-1.0f, 1.0f), 0.0f),
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

    if (in != CS::mono() && in != CS::stereo())
        return false;

    static const CS outputs[] { CS::stereo(),
                                CS::create5point1(),
                                CS::create7point1(),
                                CS::create5point1point2(),
                                CS::create5point1point4(),
                                CS::create7point1point2(),
                                CS::create7point1point4(),
                                CS::create7point1point6(),
                                CS::create9point1point6() };

    const auto out = layouts.getMainOutputChannelSet();
    return std::find (std::begin (outputs), std::end (outputs), out) != std::end (outputs);
}

void PannerProcessor::prepareToPlay (double newSampleRate, int)
{
    sampleRate = newSampleRate;
    layout.set (getBusesLayout().getMainOutputChannelSet());
    snapGains = true;
}

void PannerProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

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

    const bool sequencing = seqOn->load() > 0.5f && hasTimeline;
    const int numSteps = juce::jlimit (1, Sequencer::maxSteps, juce::roundToInt (seqSteps->load()));
    const double stepBeats = Sequencer::rateInBeats (juce::roundToInt (seqRate->load()));
    const float glide = seqGlide->load();
    const float sourceSize = size->load();
    const float halfWidth = numIn == 2 ? width->load() : 0.0f;
    const float outputGain = juce::Decibels::decibelsToGain (gain->load(), -60.0f);

    Position position { x->load(), y->load(), z->load() };
    Sequencer::Steps steps;

    if (sequencing)
        for (size_t i = 0; i < (size_t) numSteps; ++i)
            steps[i] = { stepParams[i][0]->load(), stepParams[i][1]->load(), stepParams[i][2]->load() };

    float input[maxInputs][chunkSize];
    float target[SpeakerLayout::maxChannels];

    for (int start = 0; start < numSamples; start += chunkSize)
    {
        const int n = juce::jmin (chunkSize, numSamples - start);

        if (sequencing)
            position = Sequencer::positionAt (ppq + (start + n) * ppqPerSample, stepBeats, numSteps, glide, steps);

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
            layout.computeGains (source, sourceSize, target);

            auto& current = gains[(size_t) ch];

            for (int out = 0; out < numOut; ++out)
            {
                const float startGain = snapGains ? target[out] * outputGain : current[(size_t) out];
                const float endGain = startGain + (target[out] * outputGain - startGain) * smoothing;
                current[(size_t) out] = endGain;

                if (startGain == 0.0f && endGain == 0.0f)
                    continue;

                buffer.addFromWithRamp (out, start, input[ch], n, startGain, endGain);
            }
        }

        snapGains = false;
    }

    liveX = position.x;
    liveY = position.y;
    liveZ = position.z;
    liveStep = sequencing ? Sequencer::stepAt (ppq, stepBeats, numSteps) : -1;
}

juce::AudioProcessorEditor* PannerProcessor::createEditor()
{
    return new PannerEditor (*this);
}

void PannerProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void PannerProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new PannerProcessor();
}
