// FBKTrainer - RunController.h
//
// Where the safety envelope meets the hardware. Two threads, and the division
// between them is the point.
//
// The control thread gathers an observation, ticks the supervisor, hands the
// resulting permission to the ramp, and sends whatever the ramp asks for. It runs
// at 50 Hz, comfortably faster than the watchdog timeout.
//
// The watchdog thread does nothing but ask the supervisor whether it has been
// ticked recently, and pull the plug if it has not. It shares nothing with the
// control thread except that question. That is what makes it useful: the failure
// it exists to catch is the control thread stopping, and a watchdog that lived on
// the control thread would stop with it. Between them they cover the two shapes
// this failure takes - a stalled control thread, which the watchdog catches, and
// both threads starved together, which is what a machine going to sleep looks
// like and which tick() catches on resume.
//
// The panic path
// --------------
// Panic never waits for anything. It mutes and floors the fader by sending
// repeatedly rather than by sending once and confirming, because there is nothing
// to confirm to: the run is over either way, and the only question is whether the
// console got the message.
#pragma once

#include "AudioEngine.h"
#include "GainRamp.h"
#include "SafetySupervisor.h"
#include "WingConsole.h"

#include <JuceHeader.h>

#include <functional>

namespace fbkt
{
struct RunStatus
{
    SafetyState state { SafetyState::idle };
    AbortReason abortReason { AbortReason::none };

    float commandedGainDb { 0.0f };
    float confirmedGainDb { 0.0f };
    float permittedGainDb { kGainOffDb };
    float measuredSplDb { kSilenceDb };
    float thermalFraction { 0.0f };
    float marginDb { 0.0f };

    bool consoleConnected { false };
    bool audioRunning { false };
    juce::String note;
    juce::int64 elapsedMs { 0 };
};

class RunController final : private juce::Thread
{
public:
    RunController (AudioEngine& audio, WingConsole& console);
    ~RunController() override;

    // The rig will not start unless the configuration is runnable, the microphone
    // is calibrated and the routing self test has passed. All three are checked
    // here rather than trusted from the UI, because the UI is the part a future
    // change is most likely to get wrong.
    bool start (const RigConfig& config, float startFaderDb, juce::String& errorOut);
    void stop();

    void requestGain (float gainDb) { ramp_.setTarget (gainDb); }
    void requestBurst (bool b) { burstRequested_.store (b); }

    // The measured instability point, from a sweep. Until this is set, the
    // supervisor permits no increase at all.
    void setPredictedInstabilityGainDb (float db) { predictedInstability_.store (db); }

    RunStatus status() const;
    bool isRunning() const { return isThreadRunning(); }

    // Called on the message thread when the run stops for any reason.
    std::function<void (AbortReason)> onStopped;

private:
    void run() override;
    void panic();

    class Watchdog final : public juce::Thread
    {
    public:
        explicit Watchdog (RunController& owner);
        void run() override;
    private:
        RunController& owner_;
    };

    AudioEngine&     audio_;
    WingConsole&     console_;
    SafetySupervisor supervisor_;
    GainRamp         ramp_;
    Watchdog         watchdog_ { *this };

    RigConfig config_;
    std::atomic<float> predictedInstability_ { std::numeric_limits<float>::quiet_NaN() };
    std::atomic<bool>  burstRequested_ { false };
    std::atomic<bool>  panicked_ { false };

    juce::int64 startedAtMs_ { 0 };

    mutable juce::CriticalSection statusLock_;
    RunStatus status_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (RunController)
};
} // namespace fbkt
