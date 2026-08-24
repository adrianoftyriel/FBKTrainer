#include "SafetySupervisor.h"

#include <cmath>

namespace fbkt
{
namespace
{
// The measurement microphone is our only view of the room. If it stops reporting
// while the rig is making noise, every acoustic limit becomes unenforceable, so
// the rig is blind and must stop. Confirmed over an interval rather than on one
// tick, because a single quiet block during a pause in speech is normal.
constexpr float kMicDeadDbFS  = -80.0f;
constexpr int   kMicDeadHoldMs = 1500;

// How far past the predicted instability point a burst is permitted to reach.
// Crossing the point at all is what produces oscillation; going far past it only
// makes the oscillation grow faster, which buys no information and costs driver
// life. One decibel is enough to be unambiguously across.
constexpr float kBurstOvershootDb = 1.0f;

// A commanded change takes time to appear at the console, so a difference this
// size is a step in flight rather than a disagreement.
constexpr float kMismatchSlackDb = 0.25f;
} // namespace

const char* toString (SafetyState s) noexcept
{
    switch (s)
    {
        case SafetyState::idle:     return "idle";
        case SafetyState::armed:    return "armed";
        case SafetyState::running:  return "running";
        case SafetyState::burst:    return "burst";
        case SafetyState::cooldown: return "cooldown";
        case SafetyState::aborted:  return "aborted";
    }
    return "?";
}

const char* toString (AbortReason r) noexcept
{
    switch (r)
    {
        case AbortReason::none:               return "none";
        case AbortReason::splCeiling:         return "splCeiling";
        case AbortReason::micDead:            return "micDead";
        case AbortReason::micUncalibrated:    return "micUncalibrated";
        case AbortReason::watchdogTimeout:    return "watchdogTimeout";
        case AbortReason::consoleUnreachable: return "consoleUnreachable";
        case AbortReason::consoleMismatch:    return "consoleMismatch";
        case AbortReason::audioStopped:       return "audioStopped";
        case AbortReason::thermalBudget:      return "thermalBudget";
        case AbortReason::configInvalid:      return "configInvalid";
        case AbortReason::operatorStop:       return "operatorStop";
    }
    return "?";
}

const char* describe (AbortReason r) noexcept
{
    switch (r)
    {
        case AbortReason::none:               return "No abort.";
        case AbortReason::splCeiling:         return "Measured level reached the hard ceiling at the measurement microphone.";
        case AbortReason::micDead:            return "The measurement microphone stopped reporting while the rig was making noise. Check the cable and the input assignment.";
        case AbortReason::micUncalibrated:    return "The measurement microphone is not calibrated, so no acoustic limit can be evaluated.";
        case AbortReason::watchdogTimeout:    return "The supervisor was not ticked within the watchdog interval. A thread or the process stalled.";
        case AbortReason::consoleUnreachable: return "The mixer stopped answering. Check the network cable and the mixer IP.";
        case AbortReason::consoleMismatch:    return "The mixer is not at the gain it was commanded to. Commands are being lost.";
        case AbortReason::audioStopped:       return "The audio device stopped.";
        case AbortReason::thermalBudget:      return "The high-frequency energy budget was exhausted. The rig stopped to let the driver cool.";
        case AbortReason::configInvalid:      return "The rig configuration is no longer runnable.";
        case AbortReason::operatorStop:       return "Stopped by the operator.";
    }
    return "";
}

// ---------------------------------------------------------------------------
void SafetySupervisor::configure (const RigConfig& config) noexcept
{
    config_ = config;
    limits_ = config.limits;
    configured_ = true;
    configRunnable_ = canRun (validate (config));
    calibrated_ = config.micCalibration.valid;
}

void SafetySupervisor::reset() noexcept
{
    state_ = SafetyState::idle;
    reason_ = AbortReason::none;
    haveTicked_ = false;
    lastTickMs_ = 0;
    burstStartedMs_ = 0;
    cooldownStartedMs_ = 0;
    burstTotalMs_ = 0;
    micSilent_ = false;
    micSilentSinceMs_ = 0;
    consoleMismatched_ = false;
    consoleMismatchSinceMs_ = 0;
    thermalCharge_ = 0.0f;
}

void SafetySupervisor::arm (long long timeMs) noexcept
{
    if (state_ == SafetyState::aborted)
        return;                       // a latched abort needs reset(), not arm()

    state_ = SafetyState::armed;
    reason_ = AbortReason::none;
    lastTickMs_ = timeMs;
    haveTicked_ = true;
    micSilent_ = false;
    consoleMismatched_ = false;
}

void SafetySupervisor::disarm() noexcept
{
    if (state_ != SafetyState::aborted)
        state_ = SafetyState::idle;
}

void SafetySupervisor::abort (AbortReason reason, long long timeMs) noexcept
{
    lastTickMs_ = timeMs;
    raiseAbort (reason);
}

void SafetySupervisor::raiseAbort (AbortReason r) noexcept
{
    // First reason wins. A cascade - the console going unreachable because the
    // machine stalled, say - should report the thing that happened, not the last
    // consequence to be noticed.
    if (state_ == SafetyState::aborted)
        return;

    state_ = SafetyState::aborted;
    reason_ = r;
}

bool SafetySupervisor::isOverdue (long long nowMs) const noexcept
{
    if (! haveTicked_)
        return false;
    if (state_ == SafetyState::idle || state_ == SafetyState::aborted)
        return false;
    return (nowMs - lastTickMs_) > limits_.watchdogTimeoutMs;
}

float SafetySupervisor::thermalFraction() const noexcept
{
    if (limits_.thermalBudgetS <= 0.0f)
        return 0.0f;
    return clampf (thermalCharge_ / limits_.thermalBudgetS, 0.0f, 4.0f);
}

void SafetySupervisor::updateThermal (const SafetyObservation& obs, float dtSeconds) noexcept
{
    if (dtSeconds <= 0.0f || limits_.thermalTimeConstantS <= 0.0f)
        return;

    // Charge in proportion to how much high-frequency energy is present relative
    // to the ceiling, so a burst at the ceiling costs one second of budget per
    // second and anything quieter costs proportionally less. Then discharge with
    // the driver's assumed thermal time constant.
    if (calibrated_ && obs.listeningHfDbFS > kSilenceDb)
    {
        const float hfSpl = config_.micCalibration.toSplDb (obs.listeningHfDbFS);
        const float relativeDb = hfSpl - limits_.ceilingSplDb;
        const float ratio = std::pow (10.0f, clampf (relativeDb, -60.0f, 12.0f) * 0.1f);
        thermalCharge_ += ratio * dtSeconds;
    }

    thermalCharge_ *= std::exp (-dtSeconds / limits_.thermalTimeConstantS);
    if (! std::isfinite (thermalCharge_) || thermalCharge_ < 0.0f)
        thermalCharge_ = 0.0f;
}

SafetyDecision SafetySupervisor::denyAll (SafetyState s, AbortReason r,
                                          const char* note, float spl) noexcept
{
    SafetyDecision d;
    d.state = s;
    d.reason = r;
    d.permittedGainDb = kGainOffDb;
    d.mayIncrease = false;
    d.measuredSplDb = spl;
    d.thermalFraction = thermalFraction();
    d.note = note;
    return d;
}

// ---------------------------------------------------------------------------
SafetyDecision SafetySupervisor::tick (const SafetyObservation& obs) noexcept
{
    const float spl = calibrated_ ? config_.micCalibration.toSplDb (obs.listeningMicDbFS)
                                  : kSilenceDb;

    if (state_ == SafetyState::aborted)
        return denyAll (SafetyState::aborted, reason_, "latched - reset required", spl);

    // --- watchdog, checked before anything else grants permission ---------
    //
    // This only ever fires on the tick that resumes after a gap, which is by
    // definition after the gap has already happened. That is why isOverdue()
    // exists and is called from a separate thread: this check is the second line,
    // not the first. It is still worth having, because a gap that the watchdog
    // thread also missed - both threads starved together, which is what a machine
    // going to sleep looks like - shows up here and nowhere else.
    const bool active = (state_ == SafetyState::armed || state_ == SafetyState::running
                         || state_ == SafetyState::burst || state_ == SafetyState::cooldown);

    if (haveTicked_ && active && (obs.timeMs - lastTickMs_) > limits_.watchdogTimeoutMs)
    {
        raiseAbort (AbortReason::watchdogTimeout);
        SafetyDecision d = denyAll (SafetyState::aborted, reason_, "watchdog", spl);
        d.abortRaised = true;
        return d;
    }

    const float dt = haveTicked_
                         ? clampf (static_cast<float> (obs.timeMs - lastTickMs_) * 0.001f, 0.0f, 1.0f)
                         : 0.0f;
    lastTickMs_ = obs.timeMs;
    haveTicked_ = true;

    // The thermal model runs even when idle, because a driver cools whether or
    // not we are looking at it and the budget should recover across a pause.
    updateThermal (obs, dt);

    if (state_ == SafetyState::idle)
        return denyAll (SafetyState::idle, AbortReason::none, "idle", spl);

    // --- conditions that end the run outright -----------------------------
    if (! configured_ || ! configRunnable_)
    {
        raiseAbort (AbortReason::configInvalid);
        SafetyDecision d = denyAll (SafetyState::aborted, reason_, "config", spl);
        d.abortRaised = true;
        return d;
    }

    if (! calibrated_)
    {
        raiseAbort (AbortReason::micUncalibrated);
        SafetyDecision d = denyAll (SafetyState::aborted, reason_, "uncalibrated", spl);
        d.abortRaised = true;
        return d;
    }

    if (! obs.audioRunning)
    {
        raiseAbort (AbortReason::audioStopped);
        SafetyDecision d = denyAll (SafetyState::aborted, reason_, "audio stopped", spl);
        d.abortRaised = true;
        return d;
    }

    if (! obs.consoleConnected
        || (obs.timeMs - obs.lastConsoleReplyMs) > limits_.consoleAckTimeoutMs)
    {
        raiseAbort (AbortReason::consoleUnreachable);
        SafetyDecision d = denyAll (SafetyState::aborted, reason_, "console silent", spl);
        d.abortRaised = true;
        return d;
    }

    // The console being somewhere we did not put it is more dangerous than it not
    // answering, because it means commands are being applied unpredictably rather
    // than not at all. Held over the acknowledgement window so that a step in
    // flight is not mistaken for a disagreement.
    const float mismatch = std::abs (obs.commandedGainDb - obs.confirmedGainDb);
    if (mismatch > limits_.maxStepDb + kMismatchSlackDb)
    {
        if (! consoleMismatched_)
        {
            consoleMismatched_ = true;
            consoleMismatchSinceMs_ = obs.timeMs;
        }
        else if ((obs.timeMs - consoleMismatchSinceMs_) > limits_.consoleAckTimeoutMs)
        {
            raiseAbort (AbortReason::consoleMismatch);
            SafetyDecision d = denyAll (SafetyState::aborted, reason_, "console mismatch", spl);
            d.abortRaised = true;
            return d;
        }
    }
    else
    {
        consoleMismatched_ = false;
    }

    // The hard acoustic ceiling. No hysteresis, no confirmation interval: this is
    // the limit the whole envelope exists to defend, and a level that has reached
    // it has already been reached.
    if (spl >= limits_.ceilingSplDb)
    {
        raiseAbort (AbortReason::splCeiling);
        SafetyDecision d = denyAll (SafetyState::aborted, reason_, "SPL ceiling", spl);
        d.abortRaised = true;
        d.measuredSplDb = spl;
        return d;
    }

    // A microphone that has gone quiet while we are making noise.
    if (state_ == SafetyState::running || state_ == SafetyState::burst)
    {
        if (obs.listeningMicDbFS < kMicDeadDbFS)
        {
            if (! micSilent_)
            {
                micSilent_ = true;
                micSilentSinceMs_ = obs.timeMs;
            }
            else if ((obs.timeMs - micSilentSinceMs_) > kMicDeadHoldMs)
            {
                raiseAbort (AbortReason::micDead);
                SafetyDecision d = denyAll (SafetyState::aborted, reason_, "mic dead", spl);
                d.abortRaised = true;
                return d;
            }
        }
        else
        {
            micSilent_ = false;
        }
    }
    else
    {
        micSilent_ = false;
    }

    // --- state transitions -------------------------------------------------
    const float thermal = thermalFraction();
    const bool thermalExhausted = thermal >= 1.0f;

    if (state_ == SafetyState::burst)
    {
        burstTotalMs_ += static_cast<long long> (dt * 1000.0f);

        const bool expired = (obs.timeMs - burstStartedMs_) >= limits_.maxBurstMs;
        if (expired || ! obs.burstRequested || thermalExhausted)
        {
            state_ = SafetyState::cooldown;
            cooldownStartedMs_ = obs.timeMs;
        }
    }
    else if (state_ == SafetyState::cooldown)
    {
        if ((obs.timeMs - cooldownStartedMs_) >= limits_.minCooldownMs && ! thermalExhausted)
            state_ = SafetyState::running;
    }
    else if (state_ == SafetyState::armed)
    {
        state_ = SafetyState::running;
    }

    // --- how much gain is permitted ----------------------------------------
    //
    // Built up from the most restrictive thing first, so that every reduction is
    // visible and none of them can be skipped by an early return.
    float permitted = limits_.maxGainAboveStartDb;
    const char* note = "";
    bool mayIncrease = true;

    const bool haveMeasurement = std::isfinite (obs.predictedInstabilityGainDb);
    float margin = 0.0f;

    if (! haveMeasurement)
    {
        // No sweep yet. The instability point could be anywhere, including below
        // where we already are, so the rig is not permitted above its start point.
        permitted = std::min (permitted, 0.0f);
        mayIncrease = false;
        note = "no loop measurement yet";
    }
    else
    {
        margin = obs.predictedInstabilityGainDb - obs.commandedGainDb;

        const bool burstAllowed = obs.burstRequested
                                  && ! thermalExhausted
                                  && state_ != SafetyState::cooldown;

        if (state_ == SafetyState::burst || (burstAllowed && state_ == SafetyState::running))
        {
            if (state_ != SafetyState::burst)
            {
                state_ = SafetyState::burst;
                burstStartedMs_ = obs.timeMs;
            }
            permitted = std::min (permitted, obs.predictedInstabilityGainDb + kBurstOvershootDb);
            note = "burst";
        }
        else
        {
            permitted = std::min (permitted,
                                  obs.predictedInstabilityGainDb - limits_.normalMarginDb);
            if (state_ == SafetyState::cooldown)
            {
                mayIncrease = false;
                note = "cooldown";
            }
        }
    }

    // Acoustic governor. Above the warning level we stop asking for more and
    // start giving some back, in proportion to the overshoot, so the pullback is
    // gentle near the warning level and aggressive near the ceiling.
    if (spl > limits_.warnSplDb)
    {
        permitted = std::min (permitted, obs.commandedGainDb - (spl - limits_.warnSplDb));
        mayIncrease = false;
        note = "above warning level";
    }

    if (thermalExhausted)
    {
        mayIncrease = false;
        permitted = std::min (permitted, obs.commandedGainDb);
        note = "thermal budget exhausted";
    }

    // Nothing above may raise the absolute cap, and nothing may drive the
    // permission below the console's floor.
    permitted = clampf (permitted, kGainOffDb, limits_.maxGainAboveStartDb);

    SafetyDecision d;
    d.state = state_;
    d.reason = AbortReason::none;
    d.permittedGainDb = permitted;
    d.mayIncrease = mayIncrease && permitted > obs.commandedGainDb;
    d.measuredSplDb = spl;
    d.thermalFraction = thermal;
    d.marginDb = haveMeasurement ? margin : 0.0f;
    d.note = note;
    return d;
}
} // namespace fbkt
