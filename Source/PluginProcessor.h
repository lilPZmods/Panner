#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Sequencer.h"
#include "SpatialPanner.h"

class PannerProcessor : public juce::AudioProcessor
{
public:
    PannerProcessor();

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout&) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override                         { return true; }

    const juce::String getName() const override             { return JucePlugin_Name; }
    bool acceptsMidi() const override                       { return false; }
    bool producesMidi() const override                      { return false; }
    double getTailLengthSeconds() const override            { return 0.0; }

    int getNumPrograms() override                           { return 1; }
    int getCurrentProgram() override                        { return 0; }
    void setCurrentProgram (int) override                   {}
    const juce::String getProgramName (int) override        { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    /** Parameter ID for one axis ('x', 'y' or 'z') of a sequencer step. */
    static juce::String stepParamId (int step, char axis)
    {
        return "s" + juce::String (step) + juce::String::charToString ((juce::juce_wchar) axis);
    }

    juce::AudioProcessorValueTreeState apvts;

    // Position actually being rendered, for the editor.
    std::atomic<float> liveX { 0.0f }, liveY { 0.0f }, liveZ { 0.0f };
    std::atomic<int> liveStep { -1 };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameters();

    static constexpr int maxInputs = 2;
    static constexpr int chunkSize = 32;

    using Param = std::atomic<float>*;
    Param x, y, z, size, width, gain, seqOn, seqSteps, seqRate, seqGlide;
    std::array<std::array<Param, 3>, Sequencer::maxSteps> stepParams;

    SpeakerLayout layout;
    double sampleRate = 44100.0;
    bool snapGains = true;
    std::array<std::array<float, SpeakerLayout::maxChannels>, maxInputs> gains {};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PannerProcessor)
};
