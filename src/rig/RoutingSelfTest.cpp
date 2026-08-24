#include "RoutingSelfTest.h"

#include <cmath>

namespace fbkt
{
namespace
{
// The fader move used to prove the channel assignment. Small enough to be
// inaudible as a change in the room, large enough to be well clear of the
// measurement noise.
constexpr float kProbeDeltaDb = 6.0f;

// Level of the test signal. Conversational, from a reference fader position well
// below any working level.
constexpr float kTestLevelDbFS = -30.0f;

constexpr int kSettleMs = 700;
constexpr int kMeasureMs = 2000;
constexpr int kSilenceMeasureMs = 1500;
} // namespace

RoutingSelfTest::RoutingSelfTest (AudioEngine& audio, WingConsole& console)
    : juce::Thread ("FBKTrainer routing self test"), audio_ (audio), console_ (console)
{
}

RoutingSelfTest::~RoutingSelfTest()
{
    stopThread (4000);
}

void RoutingSelfTest::startTest (const RigConfig& config, float referenceFaderDb)
{
    stopThread (4000);

    config_ = config;
    referenceFaderDb_ = clampFaderDb (referenceFaderDb);
    progress_.store (0.0f);

    {
        const juce::ScopedLock lock (stepLock_);
        log_.clear();
        step_ = "Starting";
    }

    startThread();
}

void RoutingSelfTest::setStep (const juce::String& s)
{
    const juce::ScopedLock lock (stepLock_);
    step_ = s;
}

juce::String RoutingSelfTest::currentStep() const
{
    const juce::ScopedLock lock (stepLock_);
    return step_;
}

void RoutingSelfTest::log (const juce::String& s)
{
    const juce::ScopedLock lock (stepLock_);
    log_.add (s);
}

bool RoutingSelfTest::waitForFader (float expectedDb, int timeoutMs)
{
    const auto deadline = juce::Time::getMillisecondCounter() + static_cast<juce::uint32> (timeoutMs);
    while (juce::Time::getMillisecondCounter() < deadline)
    {
        if (threadShouldExit())
            return false;

        const float readBack = console_.confirmedFaderDb (config_.vocalChannel.number);
        if (std::isfinite (readBack) && faderAgrees (expectedDb, readBack, 1.0f))
            return true;

        console_.queryFader (config_.vocalChannel.number);
        wait (40);
    }
    return false;
}

MeasuredLevels RoutingSelfTest::measureFor (int milliseconds)
{
    audio_.beginMeasurement();

    const auto deadline = juce::Time::getMillisecondCounter() + static_cast<juce::uint32> (milliseconds);
    while (juce::Time::getMillisecondCounter() < deadline)
    {
        if (threadShouldExit())
            break;
        wait (25);
    }

    return audio_.endMeasurement();
}

void RoutingSelfTest::run()
{
    RoutingVerdict verdict;
    RoutingMeasurements m;
    m.faderDeltaDb = kProbeDeltaDb;

    auto finish = [&]
    {
        // Whatever happened, put the rig back where it started. A self test that
        // leaves a fader raised because it failed halfway is worse than no self
        // test at all.
        audio_.setTestSignal (TestSignal::none, kTestLevelDbFS);
        console_.setFaderDb (config_.vocalChannel.number, referenceFaderDb_);

        juce::String logText;
        {
            const juce::ScopedLock lock (stepLock_);
            logText = log_.joinIntoString ("\n");
        }

        progress_.store (1.0f);
        setStep ("Finished");

        auto callback = onFinished;
        if (callback)
            juce::MessageManager::callAsync ([callback, verdict, logText] { callback (verdict, logText); });
    };

    // --- preconditions -----------------------------------------------------
    setStep ("Checking preconditions");

    if (! audio_.isRunning())
    {
        verdict.failures.push_back ("The audio device is not running.");
        finish();
        return;
    }

    if (! console_.isConnected() || ! console_.resolved().valid)
    {
        verdict.failures.push_back ("The console is not connected, or discovery has not resolved its addresses yet.");
        finish();
        return;
    }

    // --- reference position ------------------------------------------------
    setStep ("Setting the vocal fader to the reference position");
    log ("Reference fader position: " + juce::String (referenceFaderDb_, 1) + " dB");

    if (! console_.setFaderDb (config_.vocalChannel.number, referenceFaderDb_))
    {
        verdict.failures.push_back ("Could not send a fader command to the console.");
        finish();
        return;
    }

    console_.setMuted (config_.vocalChannel.number, false);

    m.consoleAcknowledged = waitForFader (referenceFaderDb_, 2500);
    if (! m.consoleAcknowledged)
    {
        log ("The console never confirmed the reference fader position.");
        finish();
        return;
    }

    const float referenceReadBack = console_.confirmedFaderDb (config_.vocalChannel.number);
    log ("Console confirms " + juce::String (referenceReadBack, 2) + " dB");
    progress_.store (0.15f);

    // --- silence -----------------------------------------------------------
    setStep ("Measuring the room in silence - please keep quiet");
    audio_.setTestSignal (TestSignal::none, kTestLevelDbFS);
    audio_.setSpeechPlaying (false);
    wait (kSettleMs);

    const auto silent = measureFor (kSilenceMeasureMs);
    if (threadShouldExit()) { finish(); return; }

    m.silentMicDbFS = silent.micDbFS;
    m.silentVocalDbFS = silent.vocalDbFS;
    log ("Room noise: listening mic " + juce::String (silent.micDbFS, 1)
         + " dBFS, vocal " + juce::String (silent.vocalDbFS, 1) + " dBFS");
    progress_.store (0.4f);

    // --- test signal at the reference position -----------------------------
    setStep ("Measuring with the test signal");
    audio_.setTestSignal (TestSignal::bandLimitedNoise, kTestLevelDbFS);
    wait (kSettleMs);

    const auto atReference = measureFor (kMeasureMs);
    if (threadShouldExit()) { finish(); return; }

    m.testMicDbFS = atReference.micDbFS;
    m.testVocalDbFS = atReference.vocalDbFS;
    log ("With test signal: listening mic " + juce::String (atReference.micDbFS, 1)
         + " dBFS, vocal " + juce::String (atReference.vocalDbFS, 1) + " dBFS");
    progress_.store (0.65f);

    // --- the probe move ----------------------------------------------------
    //
    // The check the whole self test exists for. If the configured vocal channel
    // is not the channel the microphone is on, everything above still passes and
    // this does not.
    setStep ("Raising the vocal fader by 6 dB");
    const float raisedDb = clampFaderDb (referenceFaderDb_ + kProbeDeltaDb);

    if (! console_.setFaderDb (config_.vocalChannel.number, raisedDb))
    {
        verdict.failures.push_back ("Could not send the probe fader command.");
        finish();
        return;
    }

    const bool acked = waitForFader (raisedDb, 2500);
    m.consoleAcknowledged = m.consoleAcknowledged && acked;

    const float raisedReadBack = console_.confirmedFaderDb (config_.vocalChannel.number);
    m.readBackFaderDeltaDb = (std::isfinite (raisedReadBack) && std::isfinite (referenceReadBack))
                                 ? raisedReadBack - referenceReadBack
                                 : 0.0f;
    log ("Console confirms " + juce::String (raisedReadBack, 2) + " dB, a move of "
         + juce::String (m.readBackFaderDeltaDb, 2) + " dB");

    wait (kSettleMs);
    const auto raised = measureFor (kMeasureMs);
    if (threadShouldExit()) { finish(); return; }

    m.raisedMicDbFS = raised.micDbFS;
    log ("With the fader raised: listening mic " + juce::String (raised.micDbFS, 1) + " dBFS");
    progress_.store (0.9f);

    // --- verdict -----------------------------------------------------------
    setStep ("Evaluating");
    verdict = evaluateRouting (m);

    log ("");
    log (verdict.passed ? "PASSED - the rig is wired as configured."
                        : "FAILED - the rig will not be permitted to raise gain.");
    log ("  listening mic signal-to-noise: " + juce::String (verdict.micSignalToNoiseDb, 1) + " dB");
    log ("  vocal channel signal-to-noise: " + juce::String (verdict.vocalSignalToNoiseDb, 1) + " dB");
    log ("  room level followed the fader by: " + juce::String (verdict.observedFaderResponseDb, 1)
         + " dB of the " + juce::String (kProbeDeltaDb, 1) + " dB commanded");

    for (const auto& failure : verdict.failures)
        log ("  - " + juce::String (failure));

    finish();
}
} // namespace fbkt
