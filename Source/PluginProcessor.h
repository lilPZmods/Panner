#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Bake.h"
#include "DriftPrompt.h"
#include "ProToolsBridge.h"
#include "SpatialPanner.h"

class PannerProcessor : public juce::AudioProcessor,
                        private juce::AudioProcessorValueTreeState::Listener,
                        private juce::ChangeListener,
                        private juce::AsyncUpdater
{
public:
    PannerProcessor();
    ~PannerProcessor() override;

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
    void updateTrackProperties (const TrackProperties&) override;

    /** Parameter ID for one axis ('x', 'y' or 'z') of a sequencer step. */
    static juce::String stepParamId (int step, char axis)
    {
        return "s" + juce::String (step) + juce::String::charToString ((juce::juce_wchar) axis);
    }

    // The three answers to a drift prompt, plus the push button. Message thread.
    void pushToProTools()                                   { bridge.requestPush(); }
    void revertToLastPush();
    void unlinkFromProTools()                               { bridge.unlink(); }

    /** Lets the processor move the drift prompt between the editor and its own window. */
    void editorOpenedOrClosed()                             { triggerAsyncUpdate(); }

    juce::AudioProcessorValueTreeState apvts;

    // Position actually being rendered, for the editor.
    std::atomic<float> liveX { 0.0f }, liveY { 0.0f }, liveZ { 0.0f };
    std::atomic<int> liveStep { -1 };

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameters();

    Pattern readPattern() const;
    ProToolsBridge::Snapshot snapshotParameters() const;
    juce::String getTrackName() const;

    void parameterChanged (const juce::String&, float) override   { bridge.requestCheck(); }
    void changeListenerCallback (juce::ChangeBroadcaster*) override { updatePrompt(); }
    void handleAsyncUpdate() override                               { updatePrompt(); }
    void updatePrompt();

    static constexpr int maxInputs = 2;
    static constexpr int chunkSize = 32;

    using Param = std::atomic<float>*;
    Param x, y, z, size, width, gain, seqOn, seqSteps, seqRate, seqGlide, renderMode;
    std::array<std::array<Param, 3>, Sequencer::maxSteps> stepParams;

    SpeakerLayout layout;
    double sampleRate = 44100.0;
    bool snapGains = true;
    std::array<std::array<float, SpeakerLayout::maxChannels>, maxInputs> gains {};

    juce::CriticalSection trackNameLock;
    juce::String trackName;

public:
    ProToolsBridge bridge;

private:
    std::unique_ptr<DriftWindow> driftWindow;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PannerProcessor)
};
