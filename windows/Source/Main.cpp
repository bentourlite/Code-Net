// Code Net Receiver for Windows: a window and a tray icon around the receiver engine.
#include <juce_gui_extra/juce_gui_extra.h>

#include "MainComponent.h"

#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>
#endif

namespace {

// A small antenna mark for the window and the tray.
juce::Image makeIcon(int size) {
    juce::Image image(juce::Image::ARGB, size, size, true);
    juce::Graphics g(image);
    const float s = float(size);
    g.setColour(juce::Colour(0xff4cd964));
    g.fillEllipse(s * 0.38f, s * 0.38f, s * 0.24f, s * 0.24f);
    for (int i = 1; i <= 2; ++i) {
        const float r = s * (0.18f + 0.16f * float(i));
        juce::Path arc;
        arc.addCentredArc(s / 2, s / 2, r, r, 0.0f, -0.9f, 0.9f, true);
        g.strokePath(arc, juce::PathStrokeType(s * 0.08f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        juce::Path left;
        left.addCentredArc(s / 2, s / 2, r, r, 0.0f, juce::MathConstants<float>::pi - 0.9f,
                           juce::MathConstants<float>::pi + 0.9f, true);
        g.strokePath(left, juce::PathStrokeType(s * 0.08f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }
    return image;
}

class MainWindow : public juce::DocumentWindow {
public:
    explicit MainWindow(const juce::String& name)
        : DocumentWindow(name, juce::Colour(0xff15171a), DocumentWindow::closeButton | DocumentWindow::minimiseButton) {
        setUsingNativeTitleBar(true);
        setContentOwned(new MainComponent(), true);
        setResizable(false, false);
        setIcon(makeIcon(64));
        centreWithSize(getWidth(), getHeight());
    }

    // Closing hides the window; the receiver keeps running from the tray.
    void closeButtonPressed() override { setVisible(false); }
};

class TrayIcon : public juce::SystemTrayIconComponent {
public:
    explicit TrayIcon(MainWindow& w) : window(w) {
        const auto icon = makeIcon(32);
        setIconImage(icon, icon);
        setIconTooltip("Code Net Receiver");
    }

    void mouseDown(const juce::MouseEvent& e) override {
        if (e.mods.isPopupMenu()) {
            juce::PopupMenu menu;
            menu.addItem("Show Code Net Receiver", [this] { show(); });
            menu.addSeparator();
            menu.addItem("Quit (stops the output)", [] { juce::JUCEApplication::getInstance()->systemRequestedQuit(); });
            menu.showMenuAsync(juce::PopupMenu::Options());
        } else {
            show();
        }
    }

private:
    void show() {
        window.setVisible(true);
        window.setMinimised(false);
        window.toFront(true);
    }

    MainWindow& window;
};

}  // namespace

class CodeNetReceiverApp : public juce::JUCEApplication {
public:
    const juce::String getApplicationName() override { return "Code Net Receiver"; }
    const juce::String getApplicationVersion() override { return CODENET_VERSION; }
    bool moreThanOneInstanceAllowed() override { return false; }

    void initialise(const juce::String& commandLine) override {
#if JUCE_WINDOWS
        // 1 ms timer resolution keeps the network loop's failover checks prompt.
        timeBeginPeriod(1);
#endif
        window = std::make_unique<MainWindow>(getApplicationName());
        tray = std::make_unique<TrayIcon>(*window);
        window->setVisible(!commandLine.contains("--minimised"));
    }

    void shutdown() override {
        tray.reset();
        window.reset();
#if JUCE_WINDOWS
        timeEndPeriod(1);
#endif
    }

    void systemRequestedQuit() override { quit(); }

    void anotherInstanceStarted(const juce::String&) override {
        if (window) {
            window->setVisible(true);
            window->toFront(true);
        }
    }

private:
    std::unique_ptr<MainWindow> window;
    std::unique_ptr<TrayIcon> tray;
};

START_JUCE_APPLICATION(CodeNetReceiverApp)
