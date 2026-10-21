// Code Net Tap: decodes the LTC audio on its track and hands each frame to the
// Code Net app on this Mac, stamped with the host clock time it leaves the output.
#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_core/juce_core.h>

#include <array>
#include <atomic>

#include "codenet/ltc.hpp"
#include "codenet/protocol.hpp"

class CodeNetTapProcessor : public juce::AudioProcessor, private juce::Thread {
public:
    CodeNetTapProcessor();
    ~CodeNetTapProcessor() override;

    void prepareToPlay(double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported(const BusesLayout& layouts) const override;
    void processBlock(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return "Code Net Tap"; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}

    void getStateInformation(juce::MemoryBlock& destData) override;
    void setStateInformation(const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState parameters;

    // Read by the editor.
    std::atomic<uint32_t> lastFrames{0};
    std::atomic<uint8_t> lastRate{0};
    std::atomic<int64_t> lastDecodeNs{0};
    std::atomic<int64_t> lastFrameStartNs{0};  // host clock, as sent; stored before lastFrames
    std::atomic<bool> hostTimeAvailable{false};
    std::atomic<uint32_t> framesSent{0};
    std::atomic<float> inputLevel{0.0f};  // recent peak of the louder channel, 0..1
    std::atomic<int> ltcChannel{-1};      // channel the LTC was last read from

    static int64_t nowNs();  // the Mac host clock (mach_absolute_time) in ns

private:
    void run() override;  // background sender
    void send(const codenet::FramePacket& packet);

    // One decoder per input channel, so LTC on either side of a stereo clip is read.
    std::array<std::unique_ptr<codenet::LtcDecoder>, 2> decoders;
    int activeChannel = -1;
    int64_t activeLastFrameEnd = 0;  // sample position where the active channel's last frame ended
    double currentSampleRate = 48000.0;
    juce::AbstractFifo fifo{256};
    std::array<codenet::FramePacket, 256> pending;
    juce::DatagramSocket socket{false};
    uint32_t sequence = 0;
    std::atomic<float>* latencyTrimMs = nullptr;
    std::atomic<float>* muteOutput = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CodeNetTapProcessor)
};
