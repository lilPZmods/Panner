#include "PluginEditor.h"

namespace colours
{
    const juce::Colour background { 0xff101114 };
    const juce::Colour panel      { 0xff18191e };
    const juce::Colour outline    { 0xff34363f };
    const juce::Colour speaker    { 0xff8a8d99 };
    const juce::Colour text       { 0xffd8d9de };
    const juce::Colour accent     { 0xffff4d2e };
    const juce::Colour good       { 0xff3ccf6e };
    const juce::Colour warning    { 0xffffb020 };
}

//==============================================================================
juce::Point<float> PannerEditor::XYPad::toScreen (float x, float y) const
{
    const auto r = room();
    return { r.getCentreX() + x * r.getWidth() * 0.5f,
             r.getCentreY() - y * r.getHeight() * 0.5f };
}

void PannerEditor::XYPad::paint (juce::Graphics& g)
{
    g.fillAll (colours::panel);
    g.setColour (colours::outline);
    g.drawRect (room(), 1.0f);

    // Speakers of the current output format. Height speakers are drawn inset as rings.
    SpeakerLayout layout;
    layout.set (owner.proc.getBusesLayout().getMainOutputChannelSet());

    g.setColour (colours::speaker);

    for (auto& s : layout.getSpeakers())
    {
        if (s.top)
            g.drawEllipse (juce::Rectangle<float> (12.0f, 12.0f).withCentre (toScreen (s.x * 0.5f, s.y * 0.5f)), 1.5f);
        else
            g.fillRect (juce::Rectangle<float> (10.0f, 10.0f).withCentre (toScreen (s.x, s.y)));
    }

    // Sequencer path
    if (owner.value ("seqOn") > 0.5f)
    {
        const int numSteps = juce::roundToInt (owner.value ("seqSteps"));
        juce::Path path;

        for (int i = 0; i < numSteps; ++i)
        {
            const auto p = toScreen (owner.value (PannerProcessor::stepParamId (i, 'x')),
                                     owner.value (PannerProcessor::stepParamId (i, 'y')));

            if (i == 0)
                path.startNewSubPath (p);
            else
                path.lineTo (p);
        }

        path.closeSubPath();
        g.setColour (colours::accent.withAlpha (0.25f));
        g.strokePath (path, juce::PathStrokeType (1.0f));

        g.setFont (11.0f);

        for (int i = 0; i < numSteps; ++i)
        {
            const auto p = toScreen (owner.value (PannerProcessor::stepParamId (i, 'x')),
                                     owner.value (PannerProcessor::stepParamId (i, 'y')));

            g.setColour (colours::accent.withAlpha (i == owner.proc.liveStep.load() ? 1.0f : 0.5f));
            g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre (p));
            g.setColour (colours::text);
            g.drawText (juce::String (i + 1), juce::Rectangle<float> (20.0f, 14.0f).withCentre (p.translated (0.0f, -12.0f)),
                        juce::Justification::centred);
        }
    }

    // Rendered position: grows with height.
    const float diameter = 16.0f + 14.0f * owner.proc.liveZ.load();
    g.setColour (colours::accent);
    g.fillEllipse (juce::Rectangle<float> (diameter, diameter)
                       .withCentre (toScreen (owner.proc.liveX.load(), owner.proc.liveY.load())));

    // Edit target
    g.setColour (colours::text);
    g.drawEllipse (juce::Rectangle<float> (22.0f, 22.0f)
                       .withCentre (toScreen (owner.value (owner.targetId ('x')), owner.value (owner.targetId ('y')))),
                   1.5f);
}

void PannerEditor::XYPad::mouseDown (const juce::MouseEvent& e)
{
    for (char axis : { 'x', 'y' })
        owner.targetParam (axis)->beginChangeGesture();

    mouseDrag (e);
}

void PannerEditor::XYPad::mouseDrag (const juce::MouseEvent& e)
{
    const auto r = room();

    auto set = [this] (char axis, float v)
    {
        auto* param = owner.targetParam (axis);
        param->setValueNotifyingHost (param->convertTo0to1 (juce::jlimit (-1.0f, 1.0f, v)));
    };

    set ('x', (e.position.x - r.getCentreX()) / (r.getWidth() * 0.5f));
    set ('y', (r.getCentreY() - e.position.y) / (r.getHeight() * 0.5f));
    repaint();
}

void PannerEditor::XYPad::mouseUp (const juce::MouseEvent&)
{
    for (char axis : { 'x', 'y' })
        owner.targetParam (axis)->endChangeGesture();
}

//==============================================================================
PannerEditor::PannerEditor (PannerProcessor& p)
    : AudioProcessorEditor (p), proc (p)
{
    auto& lf = getLookAndFeel();
    lf.setColour (juce::Slider::thumbColourId, colours::accent);
    lf.setColour (juce::Slider::rotarySliderFillColourId, colours::accent);
    lf.setColour (juce::Slider::trackColourId, colours::accent.withAlpha (0.6f));
    lf.setColour (juce::TextButton::buttonOnColourId, colours::accent);
    lf.setColour (juce::ToggleButton::tickColourId, colours::accent);

    addAndMakeVisible (pad);

    setUpSlider (zSlider, zLabel, "Height", juce::Slider::LinearVertical);
    setUpSlider (sizeSlider, sizeLabel, "Size", juce::Slider::RotaryHorizontalVerticalDrag);
    setUpSlider (widthSlider, widthLabel, "Width", juce::Slider::RotaryHorizontalVerticalDrag);
    setUpSlider (gainSlider, gainLabel, "Gain", juce::Slider::RotaryHorizontalVerticalDrag);
    setUpSlider (stepsSlider, stepsLabel, "Steps", juce::Slider::LinearHorizontal);
    setUpSlider (glideSlider, glideLabel, "Glide", juce::Slider::LinearHorizontal);

    auto attach = [this] (const juce::String& id, juce::Slider& s)
    {
        sliderAttachments.push_back (std::make_unique<SliderAttachment> (proc.apvts, id, s));
    };

    attach ("size", sizeSlider);
    attach ("width", widthSlider);
    attach ("gain", gainSlider);
    attach ("seqSteps", stepsSlider);
    attach ("seqGlide", glideSlider);

    rateBox.addItemList (Sequencer::rateNames, 1);
    addAndMakeVisible (rateBox);
    rateLabel.setText ("Rate", juce::dontSendNotification);
    rateLabel.attachToComponent (&rateBox, false);
    rateAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "seqRate", rateBox);

    addAndMakeVisible (seqButton);
    seqAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ButtonAttachment> (proc.apvts, "seqOn", seqButton);

    manualButton.setClickingTogglesState (true);
    manualButton.setRadioGroupId (1);
    manualButton.setToggleState (true, juce::dontSendNotification);
    manualButton.onClick = [this] { if (manualButton.getToggleState()) selectTarget (-1); };
    addAndMakeVisible (manualButton);

    for (int i = 0; i < Sequencer::maxSteps; ++i)
    {
        auto& b = stepButtons[(size_t) i];
        b.setButtonText (juce::String (i + 1));
        b.setClickingTogglesState (true);
        b.setRadioGroupId (1);
        b.onClick = [this, i] { if (stepButtons[(size_t) i].getToggleState()) selectTarget (i); };
        addAndMakeVisible (b);
    }

    renderBox.addItemList (proc.apvts.getParameter ("renderMode")->getAllValueStrings(), 1);
    addAndMakeVisible (renderBox);
    renderAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (proc.apvts, "renderMode", renderBox);

    bridgeStatus.setFont (juce::FontOptions (14.0f, juce::Font::bold));
    bridgeStatus.setMinimumHorizontalScale (0.6f);
    addAndMakeVisible (bridgeStatus);

    pushButton.onClick = [this] { proc.pushToProTools(); };
    addAndMakeVisible (pushButton);

    // Added last so it sits over everything and takes every click while it's up.
    driftPrompt.onPush = [this] { proc.pushToProTools(); };
    driftPrompt.onRevert = [this] { proc.revertToLastPush(); };
    driftPrompt.onUnlink = [this] { proc.unlinkFromProTools(); };
    addChildComponent (driftPrompt);

    selectTarget (-1);
    setSize (780, 566);
    startTimerHz (30);

    proc.bridge.addChangeListener (this);
    refreshBridgeStatus();
    proc.editorOpenedOrClosed();
}

PannerEditor::~PannerEditor()
{
    proc.bridge.removeChangeListener (this);
    proc.editorOpenedOrClosed();
}

void PannerEditor::refreshBridgeStatus()
{
    using Status = ProToolsBridge::Status;
    const auto report = proc.bridge.getReport();

    juce::Colour colour = colours::speaker;
    juce::String text;

    switch (report.status)
    {
        case Status::notLinked:  text = "NOT PUSHED. " + report.message; break;
        case Status::busy:       text = report.message; colour = colours::text; break;
        case Status::inSync:     text = "IN SYNC. " + report.message; colour = colours::good; break;
        case Status::drifted:    text = "DRIFT. " + report.message; colour = colours::accent; break;
        case Status::cantVerify: text = "CAN'T VERIFY. " + report.message; colour = colours::warning; break;
    }

    if (report.pushError.isNotEmpty())
    {
        text = report.pushError + "  |  " + text;
        colour = colours::warning;
    }

    bridgeStatus.setText (text, juce::dontSendNotification);
    bridgeStatus.setTooltip (text);
    bridgeStatus.setColour (juce::Label::textColourId, colour);
    pushButton.setEnabled (report.status != Status::busy);

    driftPrompt.setReport (report);
    driftPrompt.setVisible (report.needsAnswer());

    if (driftPrompt.isVisible())
        driftPrompt.toFront (true);
}

void PannerEditor::setUpSlider (juce::Slider& s, juce::Label& l, const juce::String& text, juce::Slider::SliderStyle style)
{
    s.setSliderStyle (style);
    s.setTextBoxStyle (style == juce::Slider::LinearHorizontal ? juce::Slider::TextBoxRight : juce::Slider::TextBoxBelow,
                       false, 56, 18);
    addAndMakeVisible (s);

    l.setText (text, juce::dontSendNotification);
    l.setJustificationType (style == juce::Slider::LinearHorizontal ? juce::Justification::centredLeft
                                                                    : juce::Justification::centred);
    l.attachToComponent (&s, false);
}

juce::String PannerEditor::targetId (char axis) const
{
    return selected < 0 ? juce::String::charToString ((juce::juce_wchar) axis)
                        : PannerProcessor::stepParamId (selected, axis);
}

void PannerEditor::selectTarget (int step)
{
    selected = step;
    zAttachment.reset();
    zAttachment = std::make_unique<SliderAttachment> (proc.apvts, targetId ('z'), zSlider);
    pad.repaint();
}

void PannerEditor::timerCallback()
{
    const int numSteps = juce::roundToInt (value ("seqSteps"));

    for (int i = 0; i < Sequencer::maxSteps; ++i)
        stepButtons[(size_t) i].setEnabled (i < numSteps);

    if (selected >= numSteps)
        manualButton.setToggleState (true, juce::sendNotification);

    pad.repaint();
}

void PannerEditor::paint (juce::Graphics& g)
{
    g.fillAll (colours::background);
}

void PannerEditor::resized()
{
    driftPrompt.setBounds (getLocalBounds());

    auto r = getLocalBounds().reduced (12);

    auto bridgeRow = r.removeFromTop (32);
    renderBox.setBounds (bridgeRow.removeFromLeft (190));
    pushButton.setBounds (bridgeRow.removeFromRight (170));
    bridgeRow.removeFromLeft (8);
    bridgeRow.removeFromRight (8);
    bridgeStatus.setBounds (bridgeRow);
    r.removeFromTop (14);

    auto stepRow = r.removeFromBottom (32);
    r.removeFromBottom (10);
    manualButton.setBounds (stepRow.removeFromLeft (80));
    stepRow.removeFromLeft (8);

    const int stepWidth = stepRow.getWidth() / Sequencer::maxSteps;

    for (auto& b : stepButtons)
        b.setBounds (stepRow.removeFromLeft (stepWidth).reduced (2, 0));

    pad.setBounds (r.removeFromLeft (r.getHeight()));
    r.removeFromLeft (8);

    auto zArea = r.removeFromLeft (56);
    zArea.removeFromTop (22);
    zSlider.setBounds (zArea);
    r.removeFromLeft (12);

    auto knobs = r.removeFromTop (120);
    knobs.removeFromTop (22);
    const int knobWidth = knobs.getWidth() / 3;
    sizeSlider.setBounds (knobs.removeFromLeft (knobWidth));
    widthSlider.setBounds (knobs.removeFromLeft (knobWidth));
    gainSlider.setBounds (knobs);

    r.removeFromTop (20);
    seqButton.setBounds (r.removeFromTop (24));

    r.removeFromTop (30);
    rateBox.setBounds (r.removeFromTop (24));

    r.removeFromTop (30);
    stepsSlider.setBounds (r.removeFromTop (24));

    r.removeFromTop (30);
    glideSlider.setBounds (r.removeFromTop (24));
}
