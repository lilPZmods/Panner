#include "DriftPrompt.h"

DriftPrompt::DriftPrompt()
{
    for (auto* b : { &pushButton, &revertButton, &unlinkButton })
    {
        b->setColour (juce::TextButton::buttonColourId, juce::Colours::black);
        b->setColour (juce::TextButton::textColourOffId, juce::Colours::white);
        addAndMakeVisible (b);
    }

    pushButton.onClick = [this] { if (onPush) onPush(); };
    revertButton.onClick = [this] { if (onRevert) onRevert(); };
    unlinkButton.onClick = [this] { if (onUnlink) onUnlink(); };

    setWantsKeyboardFocus (true); // swallow keys too, not just clicks
    startTimer (400);
}

void DriftPrompt::setReport (const ProToolsBridge::Report& report)
{
    if (report.status == ProToolsBridge::Status::drifted)
    {
        headline = "PRO TOOLS DOES NOT MATCH PANNER";
        details = report.message;
    }
    else
    {
        headline = "YOU PRINTED WITHOUT A CONFIRMED MATCH";
        details = "An offline bounce ran while Panner could not confirm Pro Tools' pan lanes match it. "
                  + report.message;
    }

    if (report.pushError.isNotEmpty())
        details << "\n\n" << report.pushError;

    details << "\n\nChoose one. This does not go away until Panner and Pro Tools agree.";

    pushButton.setEnabled (report.status != ProToolsBridge::Status::busy);
    revertButton.setEnabled (report.canRevert);
    repaint();
}

void DriftPrompt::timerCallback()
{
    flash = ! flash;
    repaint();
}

void DriftPrompt::paint (juce::Graphics& g)
{
    g.fillAll (flash ? juce::Colour (0xffd00000) : juce::Colour (0xff7a0000));

    auto r = getLocalBounds().reduced (24);
    g.setColour (juce::Colours::white);
    g.setFont (juce::FontOptions (28.0f, juce::Font::bold));
    g.drawFittedText (headline, r.removeFromTop (40), juce::Justification::centred, 1);

    r.removeFromTop (12);
    g.setFont (juce::FontOptions (16.0f));
    g.drawFittedText (details, r.removeFromTop (getHeight() / 2), juce::Justification::centredTop, 8);
}

void DriftPrompt::resized()
{
    auto r = getLocalBounds().reduced (24);
    auto buttons = r.removeFromBottom (3 * 40 + 2 * 10);

    for (auto* b : { &pushButton, &revertButton, &unlinkButton })
    {
        b->setBounds (buttons.removeFromTop (40).withSizeKeepingCentre (juce::jmin (420, r.getWidth()), 40));
        buttons.removeFromTop (10);
    }
}

//==============================================================================
DriftWindow::DriftWindow (DriftPrompt* prompt)
    : juce::DocumentWindow ("Panner: Pro Tools does not match", juce::Colours::darkred, 0)
{
    setUsingNativeTitleBar (true);
    setContentOwned (prompt, false);
    setAlwaysOnTop (true);
    centreWithSize (600, 380);
    setVisible (true);
    toFront (true);
}
