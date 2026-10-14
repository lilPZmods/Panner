#pragma once

#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"

class PannerEditor : public juce::AudioProcessorEditor,
                     private juce::Timer,
                     private juce::ChangeListener
{
public:
    explicit PannerEditor (PannerProcessor&);
    ~PannerEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    /** Top-down view of the room. Dragging moves whichever target is selected. */
    class XYPad : public juce::Component
    {
    public:
        explicit XYPad (PannerEditor& e) : owner (e) {}

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;

    private:
        juce::Rectangle<float> room() const   { return getLocalBounds().toFloat().reduced (18.0f); }
        juce::Point<float> toScreen (float x, float y) const;

        PannerEditor& owner;
    };

    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;

    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override  { refreshBridgeStatus(); }
    void refreshBridgeStatus();
    void setUpSlider (juce::Slider&, juce::Label&, const juce::String& text, juce::Slider::SliderStyle);

    /** step < 0 selects the manual position; otherwise a sequencer step. */
    void selectTarget (int step);
    juce::String targetId (char axis) const;
    juce::RangedAudioParameter* targetParam (char axis) const   { return proc.apvts.getParameter (targetId (axis)); }
    float value (const juce::String& id) const                  { return proc.apvts.getRawParameterValue (id)->load(); }

    PannerProcessor& proc;
    int selected = -1;

    XYPad pad { *this };
    juce::Slider zSlider, sizeSlider, widthSlider, gainSlider, stepsSlider, glideSlider;
    juce::Label zLabel, sizeLabel, widthLabel, gainLabel, stepsLabel, glideLabel, rateLabel;
    juce::ComboBox rateBox, renderBox;
    juce::Label bridgeStatus;
    juce::TextButton pushButton { "PUSH TO PRO TOOLS" };
    DriftPrompt driftPrompt;
    juce::ToggleButton seqButton { "Sequencer" };
    juce::TextButton manualButton { "Manual" };
    std::array<juce::TextButton, Sequencer::maxSteps> stepButtons;

    std::unique_ptr<SliderAttachment> zAttachment;
    std::vector<std::unique_ptr<SliderAttachment>> sliderAttachments;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> rateAttachment, renderAttachment;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ButtonAttachment> seqAttachment;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PannerEditor)
};
