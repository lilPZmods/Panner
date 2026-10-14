#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "ProToolsBridge.h"

/** The no-way-around-it question shown when Panner and Pro Tools disagree. There is no
    dismiss: it only goes away once one of the answers has brought the two back in line. */
class DriftPrompt : public juce::Component,
                    private juce::Timer
{
public:
    DriftPrompt();

    void setReport (const ProToolsBridge::Report&);

    std::function<void()> onPush, onRevert, onUnlink;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    juce::String headline, details;
    bool flash = false;
    juce::TextButton pushButton { "PANNER WINS: PUSH TO PRO TOOLS" };
    juce::TextButton revertButton { "UNDO PANNER CHANGES SINCE LAST PUSH" };
    juce::TextButton unlinkButton { "PRO TOOLS WINS: UNLINK PANNER" };
};

/** Always-on-top home for the prompt while the plugin window is closed. It has no close button. */
class DriftWindow : public juce::DocumentWindow
{
public:
    explicit DriftWindow (DriftPrompt* prompt);

    DriftPrompt& getPrompt()            { return *static_cast<DriftPrompt*> (getContentComponent()); }
    void closeButtonPressed() override  {}
};
