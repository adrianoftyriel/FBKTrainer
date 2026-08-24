#include "GainRamp.h"
#include "WingProtocol.h"

#include <algorithm>

namespace fbkt
{
float GainRamp::clampFaderDbLocal (float db) noexcept
{
    return clampFaderDb (db);
}

void GainRamp::configure (const SafetyLimits& limits, float startFaderDb) noexcept
{
    limits_ = limits;
    startFaderDb_ = clampFaderDbLocal (startFaderDb);
    reset();
}

void GainRamp::reset() noexcept
{
    target_ = 0.0f;
    commanded_ = 0.0f;
    confirmed_ = 0.0f;
    awaiting_ = false;
    haveStepped_ = false;
    lastStepMs_ = 0;
    panicked_ = false;
}

void GainRamp::panic() noexcept
{
    panicked_ = true;
    target_ = kGainOffDb;
    awaiting_ = false;      // an in-flight increase is abandoned, not waited for
}

void GainRamp::notifyConfirmed (float gainDb, long long timeMs) noexcept
{
    confirmed_ = gainDb;
    // Only a confirmation that matches what we asked for clears the flag. A
    // console reporting some other value - because someone touched the surface,
    // or because a different command landed - leaves us still waiting, and the
    // supervisor's mismatch check is what turns that into an abort.
    if (faderAgrees (commanded_, gainDb, limits_.maxStepDb * 0.5f + 0.1f))
    {
        awaiting_ = false;
        lastStepMs_ = timeMs;
    }
}

GainCommand GainRamp::update (long long timeMs, float permittedGainDb) noexcept
{
    GainCommand cmd;

    const float ceiling = panicked_ ? kGainOffDb : permittedGainDb;
    const float want = std::min (panicked_ ? kGainOffDb : target_, ceiling);

    // --- reductions: immediate, unbounded, never waiting on anything ---------
    //
    // Deliberately checked before the rate limiter and before the confirmation
    // gate, so that neither can delay a move downwards. This is the path the
    // abort takes.
    if (want < commanded_ - 0.01f)
    {
        commanded_ = want;
        awaiting_ = false;
        lastStepMs_ = timeMs;
        haveStepped_ = true;

        cmd.issue = true;
        cmd.gainDb = commanded_;
        cmd.faderDb = faderForGain (commanded_);
        cmd.isReduction = true;
        return cmd;
    }

    if (panicked_)
        return cmd;

    // --- increases: one confirmed step at a time -----------------------------
    if (awaiting_)
        return cmd;

    if (haveStepped_ && (timeMs - lastStepMs_) < limits_.minStepIntervalMs)
        return cmd;

    if (want <= commanded_ + 0.01f)
        return cmd;                   // already there

    const float step = std::min (limits_.maxStepDb, want - commanded_);
    commanded_ = std::min (commanded_ + step, ceiling);

    awaiting_ = true;
    haveStepped_ = true;
    lastStepMs_ = timeMs;

    cmd.issue = true;
    cmd.gainDb = commanded_;
    cmd.faderDb = faderForGain (commanded_);
    cmd.isReduction = false;
    return cmd;
}
} // namespace fbkt
