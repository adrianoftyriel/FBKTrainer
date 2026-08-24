#include "RunController.h"

#include <cmath>

namespace fbkt
{
namespace
{
constexpr int kControlIntervalMs = 20;    // 50 Hz
constexpr int kWatchdogIntervalMs = 50;
} // namespace

RunController::RunController (AudioEngine& audio, WingConsole& console)
    : juce::Thread ("FBKTrainer control"), audio_ (audio), console_ (console)
{
}

RunController::~RunController()
{
    stop();
}

RunController::Watchdog::Watchdog (RunController& owner)
    : juce::Thread ("FBKTrainer watchdog"), owner_ (owner)
{
}

void RunController::Watchdog::run()
{
    while (! threadShouldExit())
    {
        // The only question this thread asks. Everything else it might usefully
        // check is checked by the control thread, and checking it here too would
        // mean two things could disagree about whether to stop.
        if (owner_.supervisor_.isOverdue (static_cast<long long> (juce::Time::getMillisecondCounter())))
        {
            owner_.supervisor_.abort (AbortReason::watchdogTimeout,
                                      static_cast<long long> (juce::Time::getMillisecondCounter()));
            owner_.panic();
            break;
        }
        wait (kWatchdogIntervalMs);
    }
}

// ---------------------------------------------------------------------------
bool RunController::start (const RigConfig& config, float startFaderDb, juce::String& errorOut)
{
    stop();

    const auto issues = validate (config);
    if (! canRun (issues))
    {
        errorOut = "The rig configuration is not runnable.";
        return false;
    }

    if (! config.micCalibration.valid)
    {
        errorOut = "The measurement microphone is not calibrated, so no acoustic limit can be enforced.";
        return false;
    }

    if (! audio_.isRunning())
    {
        errorOut = "The audio device is not running.";
        return false;
    }

    if (! console_.isConnected() || ! console_.resolved().valid)
    {
        errorOut = "The console is not connected, or its addresses have not been resolved.";
        return false;
    }

    config_ = config;
    supervisor_.reset();
    supervisor_.configure (config);
    ramp_.configure (config.limits, startFaderDb);
    panicked_.store (false);
    burstRequested_.store (false);
    predictedInstability_.store (std::numeric_limits<float>::quiet_NaN());

    startedAtMs_ = static_cast<juce::int64> (juce::Time::getMillisecondCounter());
    supervisor_.arm (startedAtMs_);

    // Put the console where we think it is before claiming to control it.
    console_.setMuted (config.vocalChannel.number, false);
    console_.setFaderDb (config.vocalChannel.number, startFaderDb);

    startThread();
    watchdog_.startThread();
    return true;
}

void RunController::stop()
{
    watchdog_.signalThreadShouldExit();
    signalThreadShouldExit();
    watchdog_.stopThread (2000);
    stopThread (3000);
}

void RunController::panic()
{
    if (panicked_.exchange (true))
        return;

    ramp_.panic();
    console_.panic (config_.vocalChannel.number);
    audio_.setSpeechPlaying (false);
    audio_.setTestSignal (TestSignal::none, -60.0f);
}

// ---------------------------------------------------------------------------
void RunController::run()
{
    while (! threadShouldExit())
    {
        const auto nowMs = static_cast<long long> (juce::Time::getMillisecondCounter());
        const auto levels = audio_.levels();

        console_.keepAlive();
        console_.queryFader (config_.vocalChannel.number);

        const float confirmed = console_.confirmedFaderDb (config_.vocalChannel.number);
        const float confirmedGain = std::isfinite (confirmed) ? confirmed - ramp_.startFaderDb()
                                                              : ramp_.commandedGainDb();
        if (std::isfinite (confirmed))
            ramp_.notifyConfirmed (confirmedGain, nowMs);

        SafetyObservation obs;
        obs.timeMs = nowMs;
        obs.audioRunning = audio_.isRunning();
        obs.listeningMicDbFS = levels.micPeakDbFS;
        obs.listeningHfDbFS = levels.micHfDbFS;
        obs.consoleConnected = console_.isConnected() && console_.sawAnyReply();
        obs.commandedGainDb = ramp_.commandedGainDb();
        obs.confirmedGainDb = confirmedGain;
        obs.lastConsoleReplyMs = static_cast<long long> (console_.lastReplyMs());
        obs.predictedInstabilityGainDb = predictedInstability_.load();
        obs.burstRequested = burstRequested_.load();

        const auto decision = supervisor_.tick (obs);

        if (decision.state == SafetyState::aborted)
        {
            panic();

            {
                const juce::ScopedLock lock (statusLock_);
                status_.state = decision.state;
                status_.abortReason = decision.reason;
                status_.note = describe (decision.reason);
                status_.measuredSplDb = decision.measuredSplDb;
            }

            auto callback = onStopped;
            const auto reason = decision.reason;
            if (callback)
                juce::MessageManager::callAsync ([callback, reason] { callback (reason); });
            break;
        }

        const auto command = ramp_.update (nowMs, decision.permittedGainDb);
        if (command.issue)
            console_.setFaderDb (config_.vocalChannel.number, command.faderDb);

        {
            const juce::ScopedLock lock (statusLock_);
            status_.state = decision.state;
            status_.abortReason = AbortReason::none;
            status_.commandedGainDb = ramp_.commandedGainDb();
            status_.confirmedGainDb = confirmedGain;
            status_.permittedGainDb = decision.permittedGainDb;
            status_.measuredSplDb = decision.measuredSplDb;
            status_.thermalFraction = decision.thermalFraction;
            status_.marginDb = decision.marginDb;
            status_.consoleConnected = obs.consoleConnected;
            status_.audioRunning = obs.audioRunning;
            status_.note = decision.note;
            status_.elapsedMs = static_cast<juce::int64> (nowMs) - startedAtMs_;
        }

        wait (kControlIntervalMs);
    }

    // Leaving this loop for any reason - abort, or a plain stop - means the run
    // is over, and a run that is over does not leave a fader up.
    panic();
    watchdog_.signalThreadShouldExit();
}

RunStatus RunController::status() const
{
    const juce::ScopedLock lock (statusLock_);
    return status_;
}
} // namespace fbkt
