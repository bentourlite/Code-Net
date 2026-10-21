#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"

class CodeNetTapEditor : public juce::AudioProcessorEditor, private juce::Timer {
public:
    explicit CodeNetTapEditor(CodeNetTapProcessor&);
    ~CodeNetTapEditor() override { stopTimer(); }

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override { repaint(); }

    CodeNetTapProcessor& processor;
    juce::Slider trim;
    juce::ToggleButton mute{"Mute LTC on this track's output"};
    juce::AudioProcessorValueTreeState::SliderAttachment trimAttachment;
    juce::AudioProcessorValueTreeState::ButtonAttachment muteAttachment;
    juce::Label trimLabel{{}, "Latency trim (ms)"};
};
