#include "PluginProcessor.h"

#include "PluginEditor.h"

#if JUCE_MAC
#include <time.h>
#endif

namespace {
constexpr int kHeartbeatMs = 100;  // tells the app the plugin is alive while there is no LTC
}

int64_t CodeNetTapProcessor::nowNs() {
#if JUCE_MAC
    return int64_t(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
#else
    return juce::Time::getHighResolutionTicks() * 1000000000 / juce::Time::getHighResolutionTicksPerSecond();
#endif
}

CodeNetTapProcessor::CodeNetTapProcessor()
    : AudioProcessor(BusesProperties()
                         .withInput("Input", juce::AudioChannelSet::stereo(), true)
                         .withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      juce::Thread("Code Net sender"),
      parameters(*this, nullptr, "CodeNetTap",
                 {std::make_unique<juce::AudioParameterFloat>(
                      juce::ParameterID{"latencyTrim", 1}, "Latency trim (ms)",
                      juce::NormalisableRange<float>(-50.0f, 50.0f, 0.1f), 0.0f),
                  std::make_unique<juce::AudioParameterBool>(juce::ParameterID{"mute", 1}, "Mute LTC output",
                                                             false)}) {
    latencyTrimMs = parameters.getRawParameterValue("latencyTrim");
    muteOutput = parameters.getRawParameterValue("mute");
    startThread(juce::Thread::Priority::high);
}

CodeNetTapProcessor::~CodeNetTapProcessor() { stopThread(1000); }

bool CodeNetTapProcessor::isBusesLayoutSupported(const BusesLayout& layouts) const {
    const auto& out = layouts.getMainOutputChannelSet();
    return (out == juce::AudioChannelSet::mono() || out == juce::AudioChannelSet::stereo()) &&
           layouts.getMainInputChannelSet() == out;
}

void CodeNetTapProcessor::prepareToPlay(double sampleRate, int) {
    currentSampleRate = sampleRate;
    decoder = std::make_unique<codenet::LtcDecoder>(sampleRate);
}

void CodeNetTapProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer&) {
    juce::ScopedNoDenormals noDenormals;
    const int n = buffer.getNumSamples();
    if (decoder == nullptr || n == 0 || buffer.getNumChannels() == 0) return;

    // Host clock time of this block's first sample. Hosts that provide it (Ableton
    // Live does, through the AU/VST3 timestamp) give it per block; otherwise use now.
    int64_t blockNs = nowNs();
    bool fromHost = false;
    if (auto* head = getPlayHead())
        if (auto pos = head->getPosition())
            if (auto t = pos->getHostTimeNs()) {
                blockNs = int64_t(*t);
                fromHost = true;
            }
    hostTimeAvailable.store(fromHost, std::memory_order_relaxed);
    const int64_t blockStartSample = decoder->position();
    const int64_t trimNs = int64_t(double(latencyTrimMs->load()) * 1.0e6);
    const double sr = currentSampleRate;

    decoder->process(buffer.getReadPointer(0), n, [&](const codenet::DecodedFrame& f) {
        codenet::FramePacket p;
        p.timecode = f.timecode;
        p.userBits = f.userBits;
        p.flags = codenet::kFlagPlaying;
        p.frameStartNs = blockNs + int64_t(double(f.startSample - blockStartSample) * 1.0e9 / sr) + trimNs;
        int start1, size1, start2, size2;
        fifo.prepareToWrite(1, start1, size1, start2, size2);
        if (size1 > 0) {
            pending[size_t(start1)] = p;
            fifo.finishedWrite(1);
        }
        lastFrames.store(f.timecode.frames, std::memory_order_relaxed);
        lastRate.store(f.timecode.rate, std::memory_order_relaxed);
        lastDecodeNs.store(nowNs(), std::memory_order_relaxed);
    });

    if (muteOutput->load() > 0.5f) buffer.clear();
}

void CodeNetTapProcessor::run() {
    int64_t lastSentNs = 0;
    while (!threadShouldExit()) {
        int start1, size1, start2, size2;
        const int ready = fifo.getNumReady();
        if (ready > 0) {
            fifo.prepareToRead(ready, start1, size1, start2, size2);
            for (int i = 0; i < size1; ++i) send(pending[size_t(start1 + i)]);
            for (int i = 0; i < size2; ++i) send(pending[size_t(start2 + i)]);
            fifo.finishedRead(size1 + size2);
            lastSentNs = nowNs();
        } else if (nowNs() - lastSentNs > int64_t(kHeartbeatMs) * 1000000) {
            // No LTC right now: tell the app we are alive with a stopped packet.
            codenet::FramePacket p;
            const uint8_t rate = lastRate.load();
            p.timecode = {lastFrames.load(), uint8_t(rate ? rate : 25)};
            p.frameStartNs = nowNs();
            p.flags = codenet::kFlagNone;
            send(p);
            lastSentNs = nowNs();
        }
        wait(2);
    }
}

void CodeNetTapProcessor::send(const codenet::FramePacket& packet) {
    codenet::FramePacket p = packet;
    p.sequence = ++sequence;
    uint8_t buf[codenet::FramePacket::kSize];
    codenet::encode(p, buf);
    if (socket.write("127.0.0.1", codenet::kPluginPort, buf, int(sizeof buf)) > 0)
        framesSent.fetch_add(p.flags & codenet::kFlagPlaying ? 1u : 0u, std::memory_order_relaxed);
}

void CodeNetTapProcessor::getStateInformation(juce::MemoryBlock& destData) {
    if (auto xml = parameters.copyState().createXml()) copyXmlToBinary(*xml, destData);
}

void CodeNetTapProcessor::setStateInformation(const void* data, int sizeInBytes) {
    if (auto xml = getXmlFromBinary(data, sizeInBytes))
        if (xml->hasTagName(parameters.state.getType())) parameters.replaceState(juce::ValueTree::fromXml(*xml));
}

juce::AudioProcessorEditor* CodeNetTapProcessor::createEditor() { return new CodeNetTapEditor(*this); }

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter() { return new CodeNetTapProcessor(); }
