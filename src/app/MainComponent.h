// FBKTrainer - MainComponent.h
//
// Four panels, in the order the rig has to be brought up: wire it, reach the
// console, prove it, run it.
//
// The order is enforced rather than suggested. The Run panel will not start
// until the configuration validates, the microphone is calibrated and the routing
// self test has passed against this exact wiring. Those checks are repeated in
// RunController for real; the greyed-out button here is a convenience, not the
// guarantee.
#pragma once

#include "AudioEngine.h"
#include "ConfigStore.h"
#include "RoutingSelfTest.h"
#include "SpeechFetcher.h"
#include "RunController.h"
#include "WingConsole.h"

#include <JuceHeader.h>

namespace fbkt
{
class RigPanel;
class ConsolePanel;
class SpeechPanel;
class CheckPanel;
class RunPanel;

// Shared state, so the panels do not have to know about each other.
struct AppState
{
    StoredRig rig;
    AudioEngine audio;
    WingConsole console;
    std::unique_ptr<RoutingSelfTest> selfTest;
    std::unique_ptr<RunController> run;
    SpeechFetcher speech;

    juce::File rigFile { defaultRigFile() };

    void save();
    // Anything that changes the wiring invalidates a previous self-test pass.
    void wiringChanged();

    // Rebuilds the engine's playlist from the fetched cache plus any local
    // folder, and pushes it to the audio engine.
    void refreshPlaylist();
    void applySpeechSettings();

    std::function<void()> onChanged;
};

class MainComponent final : public juce::Component
{
public:
    MainComponent();
    ~MainComponent() override;

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    AppState state_;
    juce::TabbedComponent tabs_ { juce::TabbedButtonBar::TabsAtTop };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
} // namespace fbkt
