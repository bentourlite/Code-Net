#include "PluginEditor.h"

#include <algorithm>

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
    startTimerHz(60);
}

void CodeNetTapEditor::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(0xff15171a));
    const auto age = CodeNetTapProcessor::nowNs() - processor.lastDecodeNs.load();
    const bool live = processor.lastDecodeNs.load() != 0 && age < 200'000'000;
    const uint8_t rate = processor.lastRate.load();

    g.setColour(juce::Colours::white);
    g.setFont(juce::FontOptions(14.0f, juce::Font::bold));
    g.drawText("CODE NET TAP", 16, 10, 300, 20, juce::Justification::left);
    g.setColour(juce::Colours::grey);
    g.setFont(juce::FontOptions(12.0f));
    g.drawText("v" CODENET_VERSION, getWidth() - 116, 10, 100, 20, juce::Justification::right);

    juce::String tc = "--:--:--:--";
    if (rate != 0) {
        // Count on from the last decoded frame by the clock, rather than showing whichever
        // frame the last redraw happened to catch.
        uint32_t frames;
        int64_t startNs;
        do {
            frames = processor.lastFrames.load();
            startNs = processor.lastFrameStartNs.load();
        } while (frames != processor.lastFrames.load());
        uint32_t ahead = 0;
        if (live) {
            const int64_t elapsed = (CodeNetTapProcessor::nowNs() - startNs) * rate / 1'000'000'000;
            ahead = uint32_t(std::clamp<int64_t>(elapsed, 0, rate / 4));
        }
        tc = codenet::Timecode{frames, rate}.next(ahead).toString();
    }
    g.setColour(live ? juce::Colour(0xff4cd964) : juce::Colours::grey);
    g.setFont(juce::FontOptions(40.0f));
    g.drawText(tc, timecodeArea, juce::Justification::left);

    g.setFont(juce::FontOptions(13.0f));
    g.setColour(juce::Colours::lightgrey);
    const float level = processor.inputLevel.load();
    const juce::String dB = level > 0.0001f ? juce::String(juce::Decibels::gainToDecibels(level), 0) + " dBFS"
                                             : juce::String("silent");
    juce::String line;
    if (live) {
        line = juce::String("Reading LTC at ") + juce::String(int(rate)) + " fps, " +
               (processor.ltcChannel.load() == 1 ? "right" : "left") + " channel, " + dB;
    } else if (level < 0.001f) {
        line = "No audio on this track. Press play?";
    } else {
        g.setColour(juce::Colours::orange);
        line = "Audio at " + dB + ", but no LTC found. Is Warp off?";
    }
    g.drawText(line, 16, 84, 400, 18, juce::Justification::left);
    g.setColour(juce::Colours::lightgrey);
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
