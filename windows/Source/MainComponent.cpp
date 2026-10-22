#include "MainComponent.h"

namespace {

const char* kRunKey = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run\\Code Net Receiver";

// Trims a name to whole characters within the 16-byte source id.
std::string fitSourceId(juce::String name) {
    name = name.trim();
    while (name.getNumBytesAsUTF8() > 16) name = name.dropLastCharacters(1);
    return name.toStdString();
}

juce::String deviceLabel(const codenet::DeviceInfo& d) {
    return (d.kind == 1 ? "MIDI (MTC): " : "Audio (LTC): ") + juce::String::fromUTF8(d.name.c_str());
}

}  // namespace

MainComponent::MainComponent() {
    juce::PropertiesFile::Options options;
    options.applicationName = "Code Net Receiver";
    options.folderName = "Code Net";
    options.filenameSuffix = ".settings";
    options.osxLibrarySubFolder = "Application Support";
    store = std::make_unique<juce::PropertiesFile>(options);
    loadSettings();

    // Host apps can reassign this receiver or set its offset; both arrive on the network thread.
    const juce::Component::SafePointer<MainComponent> self(this);
    engine.onAssigned = [self](const std::string& m, const std::string& b) {
        juce::MessageManager::callAsync([safe = self, m, b] {
            if (safe == nullptr) return;
            safe->engine.noteAssigned(m, b);
            safe->settings.main = m;
            safe->settings.backup = b;
            safe->saveSettings();
            safe->refreshHostChoices();
        });
    };
    engine.onAdvanceSet = [self](double ms) {
        juce::MessageManager::callAsync([safe = self, ms] {
            if (safe == nullptr) return;
            safe->settings.advanceMs = ms;
            safe->offset.setText(juce::String(ms, 2), juce::dontSendNotification);
            safe->saveSettings();
        });
    };

    timecode.setFont(juce::Font(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 40.0f, juce::Font::plain)));
    timecode.setText("--:--:--:--", juce::dontSendNotification);
    detail.setColour(juce::Label::textColourId, juce::Colours::grey);
    detail.setMinimumHorizontalScale(1.0f);
    sources.setJustificationType(juce::Justification::topLeft);
    sources.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    for (auto* c : std::initializer_list<juce::Component*>{
             &timecode, &following, &detail, &nameLabel, &mainLabel, &backupLabel, &outputLabel, &channelLabel,
             &levelLabel, &offsetLabel, &offsetUnits, &name, &offset, &mainBox, &backupBox, &outputBox, &channelBox,
             &refresh, &returnToMain, &level, &multicast, &wifi, &startWithWindows, &sources})
        addAndMakeVisible(c);

    name.setText(settings.name, juce::dontSendNotification);
    auto applyName = [this] {
        const auto n = fitSourceId(name.getText());
        name.setText(n, juce::dontSendNotification);
        if (!n.empty() && n != settings.name) {
            settings.name = n;
            applySettings();
        }
    };
    name.onReturnKey = applyName;
    name.onFocusLost = applyName;

    offset.setText(juce::String(settings.advanceMs, 2), juce::dontSendNotification);
    offset.setInputRestrictions(8, "-0123456789.,");
    offset.setTooltip("Start each frame this much earlier to cancel the output device's delay, so this PC lines "
                      "up with the other receivers. Host apps can also set it from their receiver list.");
    auto applyOffset = [this] {
        const double ms = offset.getText().replaceCharacter(',', '.').getDoubleValue();
        if (std::abs(ms) < 1000 && ms != settings.advanceMs) {
            settings.advanceMs = ms;
            applySettings();
        }
        offset.setText(juce::String(settings.advanceMs, 2), juce::dontSendNotification);
    };
    offset.onReturnKey = applyOffset;
    offset.onFocusLost = applyOffset;

    mainBox.onChange = [this] {
        const auto text = mainBox.getText().toStdString();
        settings.main = text == "None" ? "" : text;
        if (settings.backup == settings.main) settings.backup.clear();
        applySettings();
        refreshHostChoices();
    };
    backupBox.onChange = [this] {
        const auto text = backupBox.getText().toStdString();
        settings.backup = text == "None" ? "" : text;
        applySettings();
    };
    outputBox.onChange = [this] {
        const int index = outputBox.getSelectedId() - 2;
        if (index >= 0 && index < int(devices.size())) {
            const auto& d = devices[size_t(index)];
            settings.outputKind = d.kind;
            settings.deviceId = d.id;
            if (d.kind == 0 && settings.channel >= d.channels) settings.channel = 0;
        } else {
            settings.deviceId.clear();
        }
        refreshDevices();
        applySettings();
    };
    channelBox.onChange = [this] {
        settings.channel = channelBox.getSelectedId() - 2;  // "All" is id 1 -> -1
        applySettings();
    };
    refresh.setTooltip("Look for new audio and MIDI devices");
    refresh.onClick = [this] { refreshDevices(); };
    level.setRange(0.05, 1.0, 0.01);
    level.setValue(settings.level, juce::dontSendNotification);
    level.setTextBoxStyle(juce::Slider::TextBoxRight, false, 50, 20);
    level.onValueChange = [this] {
        settings.level = float(level.getValue());
        applySettings();
    };
    multicast.setToggleState(settings.multicast, juce::dontSendNotification);
    multicast.onClick = [this] {
        settings.multicast = multicast.getToggleState();
        applySettings();
    };
    wifi.setToggleState(settings.wifi, juce::dontSendNotification);
    wifi.setTooltip("Rides through Wi-Fi sized gaps (6 frames). Use wired networking for shows.");
    wifi.onClick = [this] {
        settings.wifi = wifi.getToggleState();
        applySettings();
    };
    returnToMain.onClick = [this] { engine.returnToMain(); };
#if JUCE_WINDOWS
    startWithWindows.setToggleState(juce::WindowsRegistry::valueExists(kRunKey), juce::dontSendNotification);
    startWithWindows.onClick = [this] {
        if (startWithWindows.getToggleState()) {
            const auto exe = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getFullPathName();
            juce::WindowsRegistry::setValue(kRunKey, "\"" + exe + "\" --minimised");
        } else {
            juce::WindowsRegistry::deleteValue(kRunKey);
        }
    };
#else
    startWithWindows.setEnabled(false);
#endif

    refreshDevices();
    refreshHostChoices();
    applySettings();
    setSize(560, 470);
    startTimerHz(60);
}

MainComponent::~MainComponent() {
    stopTimer();
    engine.onAssigned = nullptr;
    engine.onAdvanceSet = nullptr;
    engine.stop();
}

void MainComponent::loadSettings() {
    settings.name = store->getValue("name", settings.name).toStdString();
    settings.main = store->getValue("main").toStdString();
    settings.backup = store->getValue("backup").toStdString();
    settings.outputKind = store->getIntValue("outputKind", 0);
    settings.deviceId = store->getValue("deviceId").toStdString();
    settings.channel = store->getIntValue("channel", 0);
    settings.level = float(store->getDoubleValue("level", 0.5));
    settings.multicast = store->getBoolValue("multicast", true);
    settings.wifi = store->getBoolValue("wifi", false);
    settings.advanceMs = store->getDoubleValue("advanceMs", 0.0);
}

void MainComponent::saveSettings() {
    store->setValue("name", juce::String(settings.name));
    store->setValue("main", juce::String(settings.main));
    store->setValue("backup", juce::String(settings.backup));
    store->setValue("outputKind", settings.outputKind);
    store->setValue("deviceId", juce::String(settings.deviceId));
    store->setValue("channel", settings.channel);
    store->setValue("level", double(settings.level));
    store->setValue("multicast", settings.multicast);
    store->setValue("wifi", settings.wifi);
    store->setValue("advanceMs", settings.advanceMs);
    store->saveIfNeeded();
}

void MainComponent::applySettings() {
    saveSettings();
    const auto err = engine.apply(settings);
    error = err ? juce::String(*err) : juce::String();
    updateStatus();
}

void MainComponent::refreshDevices() {
    devices = ReceiverEngine::devices();
    outputBox.clear(juce::dontSendNotification);
    outputBox.addItem("None", 1);
    int selected = 1;
    const codenet::DeviceInfo* current = nullptr;
    for (size_t i = 0; i < devices.size(); ++i) {
        outputBox.addItem(deviceLabel(devices[i]), int(i) + 2);
        if (devices[i].kind == settings.outputKind && devices[i].id == settings.deviceId) {
            selected = int(i) + 2;
            current = &devices[i];
        }
    }
    if (!settings.deviceId.empty() && current == nullptr) {
        // Keep showing a saved device that is unplugged, so it comes back when reconnected.
        outputBox.addItem("(not connected) " + juce::String(settings.deviceId), 1000);
        selected = 1000;
    }
    outputBox.setSelectedId(selected, juce::dontSendNotification);

    channelBox.clear(juce::dontSendNotification);
    channelBox.addItem("All", 1);
    const int channels = current ? std::max(current->channels, 1) : 2;
    for (int c = 0; c < channels; ++c) channelBox.addItem(juce::String(c + 1), c + 2);
    channelBox.setSelectedId(settings.channel + 2, juce::dontSendNotification);
    const bool audio = settings.outputKind == 0;
    channelBox.setVisible(audio);
    channelLabel.setVisible(audio);
    level.setVisible(audio);
    levelLabel.setVisible(audio);
}

void MainComponent::fillHostBox(juce::ComboBox& box, const std::string& current, const std::string& exclude) {
    box.clear(juce::dontSendNotification);
    box.addItem("None", 1);
    int id = 2, selected = 1;
    for (const auto& h : hostChoices) {
        if (h == exclude) continue;
        box.addItem(juce::String::fromUTF8(h.c_str()), id);
        if (h == current) selected = id;
        ++id;
    }
    box.setSelectedId(selected, juce::dontSendNotification);
}

void MainComponent::refreshHostChoices() {
    // Hosts this receiver hears or follows, plus whatever is assigned.
    std::vector<std::string> names;
    auto add = [&names](const std::string& n) {
        if (!n.empty() && std::find(names.begin(), names.end(), n) == names.end()) names.push_back(n);
    };
    if (auto* heard = status["heard"].getArray())
        for (auto& h : *heard) add(h.toString().toStdString());
    if (auto* list = status["sources"].getArray())
        for (auto& s : *list) add(s["name"].toString().toStdString());
    add(settings.main);
    add(settings.backup);
    std::sort(names.begin(), names.end());
    if (names == hostChoices && mainBox.getNumItems() > 0) {
        if (mainBox.getText().toStdString() != (settings.main.empty() ? "None" : settings.main) ||
            backupBox.getText().toStdString() != (settings.backup.empty() ? "None" : settings.backup)) {
            fillHostBox(mainBox, settings.main, {});
            fillHostBox(backupBox, settings.backup, settings.main);
        }
        return;
    }
    hostChoices = names;
    fillHostBox(mainBox, settings.main, {});
    fillHostBox(backupBox, settings.backup, settings.main);
}

void MainComponent::updateStatus() {
    status = juce::JSON::parse(juce::String::fromUTF8(engine.statusJson().c_str()));
    const auto followingRole = status["following"].toString();
    juce::String line;
    if (followingRole.isNotEmpty()) {
        juce::String host = followingRole;
        if (auto* list = status["sources"].getArray())
            for (auto& s : *list)
                if (s["role"].toString() == followingRole) host = s["name"].toString();
        line = "following " + followingRole + " (" + host + ")";
    }
    following.setText(line, juce::dontSendNotification);

    if (error.isNotEmpty()) {
        detail.setColour(juce::Label::textColourId, juce::Colours::orange);
        detail.setText(error, juce::dontSendNotification);
    } else {
        detail.setColour(juce::Label::textColourId, juce::Colours::grey);
        const auto idle = status["idle"];
        detail.setText(idle.isString() ? idle.toString() : status["device"].toString(), juce::dontSendNotification);
    }

    juce::String lines;
    if (auto* list = status["sources"].getArray()) {
        for (auto& s : *list) {
            lines << s["role"].toString() << ": " << s["name"].toString();
            if (s["address"].isString()) lines << " at " << s["address"].toString();
            lines << ((bool) s["valid"] ? ", receiving" : ", not valid");
            if (!(bool) s["playing"]) lines << ", stopped";
            if (s["rtt_ms"].isDouble() || s["rtt_ms"].isInt()) lines << ", round trip " << juce::String(double(s["rtt_ms"]), 2) << " ms";
            lines << "\n";
        }
    }
    sources.setText(lines, juce::dontSendNotification);
    refreshHostChoices();
}

void MainComponent::timerCallback() {
    const auto frame = engine.frameNow();
    const juce::String tc = frame ? juce::String(frame->timecode.toString()) : juce::String("--:--:--:--");
    if (tc != timecode.getText()) timecode.setText(tc, juce::dontSendNotification);
    timecode.setColour(juce::Label::textColourId, frame ? juce::Colour(0xff4cd964) : juce::Colours::grey);
    if (++ticks % 15 == 0) updateStatus();
}

void MainComponent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(0xff15171a));
    g.setColour(juce::Colours::grey);
    g.setFont(juce::FontOptions(12.0f));
    g.drawText("v" CODENET_VERSION, getWidth() - 116, 8, 100, 18, juce::Justification::right);
}

void MainComponent::resized() {
    auto area = getLocalBounds().reduced(16);
    timecode.setBounds(area.removeFromTop(50));
    following.setBounds(area.removeFromTop(20));
    detail.setBounds(area.removeFromTop(34));
    area.removeFromTop(8);
    auto row = [&area](int height = 26) {
        auto r = area.removeFromTop(height);
        area.removeFromTop(6);
        return r;
    };
    constexpr int labelWidth = 110;
    auto r = row();
    nameLabel.setBounds(r.removeFromLeft(labelWidth));
    name.setBounds(r.removeFromLeft(180));
    r = row();
    mainLabel.setBounds(r.removeFromLeft(labelWidth));
    mainBox.setBounds(r.removeFromLeft(150));
    r.removeFromLeft(16);
    backupLabel.setBounds(r.removeFromLeft(60));
    backupBox.setBounds(r.removeFromLeft(150));
    r = row();
    outputLabel.setBounds(r.removeFromLeft(labelWidth));
    refresh.setBounds(r.removeFromRight(80));
    r.removeFromRight(8);
    outputBox.setBounds(r);
    r = row();
    channelLabel.setBounds(r.removeFromLeft(labelWidth));
    channelBox.setBounds(r.removeFromLeft(90));
    r.removeFromLeft(16);
    levelLabel.setBounds(r.removeFromLeft(50));
    level.setBounds(r);
    r = row();
    offsetLabel.setBounds(r.removeFromLeft(labelWidth));
    offset.setBounds(r.removeFromLeft(70));
    r.removeFromLeft(6);
    offsetUnits.setBounds(r.removeFromLeft(100));
    r = row();
    multicast.setBounds(r.removeFromLeft(110));
    wifi.setBounds(r.removeFromLeft(110));
    returnToMain.setBounds(r.removeFromRight(130));
    r = row();
    startWithWindows.setBounds(r.removeFromLeft(220));
    sources.setBounds(area);
}
