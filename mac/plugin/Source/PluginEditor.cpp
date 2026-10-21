#include "PluginEditor.h"

CodeNetTapEditor::CodeNetTapEditor(CodeNetTapProcessor& p)
    : AudioProcessorEditor(p),
      processor(p),
      trimAttachment(p.parameters, "latencyTrim", trim),
      muteAttachment(p.parameters, "mute", mute) {
    trim.setSliderStyle(juce::Slider::LinearHorizontal);
    trim.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 20);
    addAndMakeVisible(trim);
    addAndMakeVisible(trimLabel);
    addAndMakeVisible(mute);
    setSize(420, 220);
    startTimerHz(15);
}

void CodeNetTapEditor::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(0xff15171a));
    const auto age = CodeNetTapProcessor::nowNs() - processor.lastDecodeNs.load();
    const bool live = processor.lastDecodeNs.load() != 0 && age < 200'000'000;
    const uint8_t rate = processor.lastRate.load();

    g.setColour(juce::Colours::white);
    g.setFont(juce::FontOptions(14.0f, juce::Font::bold));
    g.drawText("CODE NET TAP", 16, 10, 300, 20, juce::Justification::left);

    juce::String tc = "--:--:--:--";
    if (rate != 0) tc = codenet::Timecode{processor.lastFrames.load(), rate}.toString();
    g.setColour(live ? juce::Colour(0xff4cd964) : juce::Colours::grey);
    g.setFont(juce::FontOptions(40.0f));
    g.drawText(tc, 16, 34, 300, 48, juce::Justification::left);

    g.setFont(juce::FontOptions(13.0f));
    g.setColour(juce::Colours::lightgrey);
    juce::String line = live ? juce::String("Reading LTC at ") + juce::String(int(rate)) + " fps"
                             : juce::String("No LTC on this track");
    g.drawText(line, 16, 84, 400, 18, juce::Justification::left);
    g.drawText(juce::String("Frames sent to the Code Net app: ") + juce::String(processor.framesSent.load()), 16,
               104, 400, 18, juce::Justification::left);
    if (!processor.hostTimeAvailable.load()) {
        g.setColour(juce::Colours::orange);
        g.drawText("Host gives no timestamps; timing uses the clock instead", 16, 124, 400, 18,
                   juce::Justification::left);
    }
}

void CodeNetTapEditor::resized() {
    trimLabel.setBounds(16, 150, 130, 22);
    trim.setBounds(150, 150, 254, 22);
    mute.setBounds(12, 182, 380, 24);
}
