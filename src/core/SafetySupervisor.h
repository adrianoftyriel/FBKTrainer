// FBKTrainer - SafetySupervisor.h
//
// The component that decides how much gain the rig is allowed to have, and takes
// it away when it should not.
//
// Why this is a pure state machine
// --------------------------------
// It performs no I/O, owns no thread, opens no socket and reads no clock. Every
// input arrives in an Observation and every output leaves in a Decision. That is
// not architectural tidiness for its own sake: it means the entire safety
// envelope can be exercised in unit tests, at any timing, including the timings
// that are impossible to arrange deliberately in a room - the packet that never
// arrives, the microphone that goes dead at the moment gain crosses the margin,
// the tick that lands 4 seconds late because Windows decided to install
// something. None of those can be tested against real hardware, and all of them
// will eventually happen during a multi-day unattended run.
//
// The default is no
// -----------------
// permittedGainDb is a permission, not a limit. It starts at "off", and every
// tick has to re-establish it from scratch out of the observation. There is no
// path through this code that leaves a stale permission standing: if a check
// cannot be evaluated - because the console has not answered, or the microphone
// is not reporting, or the calibration is missing - the permission is not
// granted. A supervisor that is confused is a supervisor that turns the gain
// down.
//
// Abort latches
// -------------
// Once aborted the supervisor stays aborted until a human calls reset(). An
// automatic recovery would mean the rig could re-enter the condition that caused
// the abort, indefinitely, unattended, which is precisely the scenario the abort
// exists to prevent. Three days of no data is a disappointment; three days of a
// speaker howling into an empty building is a repair bill.
#pragma once

#include "RigConfig.h"
#include "Units.h"

namespace fbkt
{
enum class SafetyState
{
    idle,        // not running; gain permission is off
    armed,       // checks passing, permission granted, gain at the start point
    running,     // operating below the normal margin
    burst,       // deliberately past the margin, time-limited
    cooldown,    // resting after a burst; no increase permitted
    aborted      // latched. Requires reset().
};

enum class AbortReason
{
    none,
    splCeiling,          // measured level reached the hard ceiling
    micDead,             // the measurement microphone stopped reporting
    micUncalibrated,     // acoustic limits cannot be evaluated
    watchdogTimeout,     // we were not ticked; the process or a thread stalled
    consoleUnreachable,  // no answer from the mixer
    consoleMismatch,     // the mixer is not where we commanded it to be
    audioStopped,        // the audio device went away underneath us
    thermalBudget,       // high-frequency energy budget exhausted
    configInvalid,       // configuration stopped being runnable
    operatorStop         // a human pressed stop
};

const char* toString (SafetyState) noexcept;
const char* toString (AbortReason) noexcept;
const char* describe (AbortReason) noexcept;

// ---------------------------------------------------------------------------
// What the rig reports, once per tick. Every field is required; there is no
// "unknown" that the supervisor will treat charitably.
struct SafetyObservation
{
    long long timeMs { 0 };            // monotonic milliseconds, never wall clock

    bool  audioRunning { false };
    float listeningMicDbFS { kSilenceDb };   // fast peak, measurement microphone
    float listeningHfDbFS { kSilenceDb };    // energy above limits.thermalBandLowHz

    bool  consoleConnected { false };
    float commandedGainDb { 0.0f };    // relative to the operator's start point
    float confirmedGainDb { 0.0f };    // as read back from the console
    long long lastConsoleReplyMs { 0 };

    // Gain, relative to the start point, at which the most recent measurement
    // predicts the loop goes unstable. Not finite when no measurement has been
    // made yet - in which case the supervisor allows no increase at all, because
    // an unmeasured loop is one whose instability point could be anywhere.
    float predictedInstabilityGainDb { std::numeric_limits<float>::quiet_NaN() };

    // Set by the run controller when it wants to cross the margin deliberately.
    // A request, not an instruction: the supervisor grants it only if every other
    // condition is met and the thermal and cooldown budgets allow.
    bool  burstRequested { false };
};

// ---------------------------------------------------------------------------
struct SafetyDecision
{
    SafetyState state { SafetyState::idle };
    AbortReason reason { AbortReason::none };

    // The most gain, relative to the start point, that may be commanded right
    // now. kGainOffDb means the fader goes to its floor immediately.
    float permittedGainDb { kGainOffDb };

    // Whether the run controller may ask for more gain than it currently has.
    // False does not mean stop - it means hold or come down.
    bool mayIncrease { false };

    // True on the tick where an abort is raised, so the caller can act once
    // rather than repeatedly. The state stays aborted afterwards.
    bool abortRaised { false };

    // Diagnostics for the run log and the operator display.
    float measuredSplDb { kSilenceDb };
    float thermalFraction { 0.0f };    // 0..1 of the budget consumed
    float marginDb { 0.0f };           // dB below the predicted instability point
    const char* note { "" };
};

// ---------------------------------------------------------------------------
class SafetySupervisor
{
public:
    // Limits are copied, so a live edit to the config cannot change the envelope
    // underneath a running rig. Applying new limits is an explicit act.
    void configure (const RigConfig& config) noexcept;

    void reset() noexcept;                 // clears a latched abort
    void arm (long long timeMs) noexcept;  // idle -> armed
    void disarm() noexcept;                // back to idle, without latching

    // Raise an abort from outside - an operator stop, or a check the supervisor
    // cannot make for itself.
    void abort (AbortReason reason, long long timeMs) noexcept;

    // The main entry point. Call this at a steady rate, faster than the watchdog
    // timeout; 20-50 Hz is the intended range.
    SafetyDecision tick (const SafetyObservation& obs) noexcept;

    // Called from an independent watchdog thread, so a stall in the thread that
    // normally calls tick() is detected by something that is not itself stalled.
    // This is the actual dead-man's switch: tick() can only ever notice a gap
    // once it resumes, which by definition is too late.
    bool isOverdue (long long nowMs) const noexcept;

    SafetyState state() const noexcept { return state_; }
    AbortReason abortReason() const noexcept { return reason_; }
    bool isAborted() const noexcept { return state_ == SafetyState::aborted; }

    // Total time spent above the normal margin, for the run log.
    long long burstMillisTotal() const noexcept { return burstTotalMs_; }
    float thermalFraction() const noexcept;

private:
    SafetyDecision denyAll (SafetyState s, AbortReason r, const char* note, float spl) noexcept;
    void raiseAbort (AbortReason r) noexcept;
    void updateThermal (const SafetyObservation& obs, float dtSeconds) noexcept;

    RigConfig    config_ {};
    SafetyLimits limits_ {};
    bool         configured_ { false };
    bool         configRunnable_ { false };
    bool         calibrated_ { false };

    SafetyState state_ { SafetyState::idle };
    AbortReason reason_ { AbortReason::none };

    long long lastTickMs_ { 0 };
    bool      haveTicked_ { false };

    long long burstStartedMs_ { 0 };
    long long cooldownStartedMs_ { 0 };
    long long burstTotalMs_ { 0 };

    // The measurement microphone going quiet is only suspicious once we are
    // actually making noise, so the dead-microphone check is armed by level and
    // confirmed by duration rather than tripping on a single quiet tick.
    long long micSilentSinceMs_ { 0 };
    bool      micSilent_ { false };

    long long consoleMismatchSinceMs_ { 0 };
    bool      consoleMismatched_ { false };

    // Thermal accumulator, in "equivalent seconds at the ceiling".
    float thermalCharge_ { 0.0f };
};
} // namespace fbkt
