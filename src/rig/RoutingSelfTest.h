// FBKTrainer - RoutingSelfTest.h
//
// Proves the rig is wired the way the configuration claims, before any gain is
// permitted.
//
// The sequence is deliberately dull: measure the room in silence, measure it with
// a quiet band-limited test signal, raise the assigned vocal fader by a known
// small amount, measure again, put everything back. Nothing here goes above the
// level of a conversation, and the fader move is 6 dB from a reference position
// well below any working level.
//
// The verdict comes from RoutingCheck, which is pure arithmetic and tested
// separately. This class only arranges the measurements.
#pragma once

#include "AudioEngine.h"
#include "RoutingCheck.h"
#include "WingConsole.h"

#include <JuceHeader.h>

#include <functional>

namespace fbkt
{
class RoutingSelfTest final : public juce::Thread
{
public:
    RoutingSelfTest (AudioEngine& audio, WingConsole& console);
    ~RoutingSelfTest() override;

    // referenceFaderDb is where the vocal fader sits for the test. It should be
    // well below the working level: the test needs a channel that is audible, not
    // one that is anywhere near feeding back.
    void startTest (const RigConfig& config, float referenceFaderDb = -20.0f);

    void run() override;

    // Called on the message thread when the test finishes, for any reason.
    std::function<void (const RoutingVerdict&, const juce::String& log)> onFinished;

    // Progress, for the UI. 0..1.
    float progress() const noexcept { return progress_.load(); }
    juce::String currentStep() const;

private:
    bool waitForFader (float expectedDb, int timeoutMs);
    MeasuredLevels measureFor (int milliseconds);
    void setStep (const juce::String&);
    void log (const juce::String&);

    AudioEngine& audio_;
    WingConsole& console_;

    RigConfig config_;
    float referenceFaderDb_ { -20.0f };

    std::atomic<float> progress_ { 0.0f };

    mutable juce::CriticalSection stepLock_;
    juce::String step_;
    juce::StringArray log_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RoutingSelfTest)
};
} // namespace fbkt
