#pragma once

#include <juce_gui_extra/juce_gui_extra.h>

#include "ReceiverEngine.h"

// The receiver's window: status at the top, then the same settings a Pi has.
class MainComponent : public juce::Component, private juce::Timer {
public:
    MainComponent();
    ~MainComponent() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void loadSettings();
    void saveSettings();
    void applySettings();
    void refreshDevices();
    void refreshHostChoices();
    void updateStatus();
    void fillHostBox(juce::ComboBox& box, const std::string& current, const std::string& exclude);

    ReceiverEngine engine;
    ReceiverSettings settings;
    std::unique_ptr<juce::PropertiesFile> store;
    std::vector<codenet::DeviceInfo> devices;
    std::vector<std::string> hostChoices;
    juce::var status;
    juce::String error;
    int ticks = 0;

    juce::Label timecode, following, detail;
    juce::Label nameLabel{{}, "Receiver name"}, mainLabel{{}, "Main"}, backupLabel{{}, "Backup"},
        outputLabel{{}, "Output"}, channelLabel{{}, "Channel"}, levelLabel{{}, "Level"},
        offsetLabel{{}, "Output offset"}, offsetUnits{{}, "ms earlier"};
    juce::TextEditor name, offset;
    juce::ComboBox mainBox, backupBox, outputBox, channelBox;
    juce::TextButton refresh{"Refresh"}, returnToMain{"Return to main"};
    juce::Slider level;
    juce::ToggleButton multicast{"Multicast"}, wifi{"Wi-Fi mode"}, startWithWindows{"Start with Windows"};
    juce::Label sources;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};
