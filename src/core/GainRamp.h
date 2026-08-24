// FBKTrainer - GainRamp.h
//
// Turns a desired gain into a sequence of small, individually confirmed console
// commands.
//
// The asymmetry is the whole point
// --------------------------------
// Going up is slow, bounded and verified: one step of at most limits.maxStepDb,
// then wait for the console to confirm it before the next one is permitted. Going
// down is immediate and unbounded: a reduction is always safe, and rate-limiting
// it would mean the one moment we most need the gain to move is the moment it
// moves slowest.
//
// That asymmetry is also why the confirmation requirement does not deadlock. If
// the console stops confirming, no further increase is issued - the ramp simply
// stops climbing - and the supervisor separately notices the silence and aborts.
// The ramp never has to decide that something is wrong; it only ever declines to
// proceed, which is the safe half of the decision.
#pragma once

#include "RigConfig.h"
#include "Units.h"

namespace fbkt
{
struct GainCommand
{
    bool  issue { false };      // whether anything should be sent this tick
    float gainDb { 0.0f };      // relative to the start point
    float faderDb { 0.0f };     // absolute, ready for the console
    bool  isReduction { false };
};

class GainRamp
{
public:
    // startFaderDb is the operator-set working point: the fader position the rig
    // begins at and which every relative gain is measured from.
    void configure (const SafetyLimits& limits, float startFaderDb) noexcept;

    void reset() noexcept;

    // Where the run controller would like to be. Clamped by the supervisor's
    // permission on every update, so asking for something forbidden is harmless.
    void setTarget (float gainDb) noexcept { target_ = gainDb; }
    float target() const noexcept { return target_; }

    // Immediately target the console's floor and forget any in-flight step. Used
    // by the abort path.
    void panic() noexcept;

    // The console has confirmed it is at this gain.
    void notifyConfirmed (float gainDb, long long timeMs) noexcept;

    // Called every tick. permittedGainDb comes from the supervisor and is a hard
    // ceiling; the ramp will move down to meet it immediately if it is below
    // where we currently are.
    GainCommand update (long long timeMs, float permittedGainDb) noexcept;

    float commandedGainDb() const noexcept { return commanded_; }
    float confirmedGainDb() const noexcept { return confirmed_; }
    float startFaderDb() const noexcept { return startFaderDb_; }

    // True while a step has been issued and not yet confirmed.
    bool awaitingConfirmation() const noexcept { return awaiting_; }

    float faderForGain (float gainDb) const noexcept
    {
        return clampFaderDbLocal (startFaderDb_ + gainDb);
    }

private:
    static float clampFaderDbLocal (float db) noexcept;

    SafetyLimits limits_ {};
    float startFaderDb_ { -10.0f };

    float target_ { 0.0f };
    float commanded_ { 0.0f };
    float confirmed_ { 0.0f };

    bool      awaiting_ { false };
    long long lastStepMs_ { 0 };
    bool      haveStepped_ { false };
    bool      panicked_ { false };
};
} // namespace fbkt
