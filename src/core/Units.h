// FBKTrainer - Units.h
//
// Level arithmetic, kept in one place because the safety layer depends on it and
// a decibel convention error in a program that raises PA gain on purpose is not a
// cosmetic bug.
//
// Conventions used throughout, without exception:
//
//   dBFS   full-scale digital level. 0 dBFS is a unity-amplitude sine's peak.
//   dBSPL  acoustic level at the measurement microphone.
//   dB     a difference between two levels, or a gain. Never an absolute.
//
// The bridge between dBFS and dBSPL is a single measured constant per rig: the
// SPL that a full-scale signal at the measurement microphone corresponds to. It
// has to be measured with an acoustic calibrator; it cannot be guessed, and the
// safety layer refuses to permit any gain increase until it has been supplied.
#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

namespace fbkt
{
// The level we use to mean "silence". Everything clamps to this rather than to
// negative infinity, so that a level can be compared, averaged and stored in a
// plain float without producing a NaN three operations later.
inline constexpr float kSilenceDb = -140.0f;

// A gain of "off". Distinct from kSilenceDb because it is a gain, not a level,
// and because the console's own fader floor is what it actually maps to.
inline constexpr float kGainOffDb = -144.0f;

inline float clampf (float v, float lo, float hi) noexcept
{
    return std::max (lo, std::min (hi, v));
}

inline bool isFinitef (float v) noexcept
{
    return std::isfinite (v);
}

// Amplitude ratio to decibels. Guarded at the bottom so a zero or denormal input
// returns kSilenceDb rather than -inf.
inline float linearToDb (float linear) noexcept
{
    const float a = std::abs (linear);
    if (! (a > 1.0e-7f))
        return kSilenceDb;
    return 20.0f * std::log10 (a);
}

inline float dbToLinear (float db) noexcept
{
    if (db <= kSilenceDb)
        return 0.0f;
    return std::pow (10.0f, db * 0.05f);
}

// Power ratio to decibels, for anything already squared (band energies, mean
// squares). Using linearToDb on a power value is a factor-of-two error in dB,
// which is exactly the kind of mistake that makes a limit twice as permissive as
// intended, so the two are named separately and neither takes the other's input.
inline float powerToDb (float power) noexcept
{
    if (! (power > 1.0e-14f))
        return kSilenceDb;
    return 10.0f * std::log10 (power);
}

// Sum of two levels expressed in dB, as incoherent power addition. Two equal
// levels sum to +3 dB, which is the right answer for uncorrelated content and
// the conservative answer for correlated content.
inline float sumDb (float aDb, float bDb) noexcept
{
    if (aDb <= kSilenceDb) return bDb;
    if (bDb <= kSilenceDb) return aDb;
    const float hi = std::max (aDb, bDb);
    const float lo = std::min (aDb, bDb);
    return hi + 10.0f * std::log10 (1.0f + std::pow (10.0f, (lo - hi) * 0.1f));
}

// ---------------------------------------------------------------------------
// The one calibrated constant that ties the digital domain to the room.
//
// splAtFullScaleDb is the sound pressure level, at the measurement microphone,
// that a full-scale digital signal on that input corresponds to. Measure it by
// presenting a calibrator of known output (94 dB SPL at 1 kHz is the usual one)
// to the capsule and reading the resulting dBFS: the constant is then
//
//     splAtFullScale = calibratorSpl - measuredDbFS
//
// which is a positive number of the order of 120 for a typical measurement
// microphone and preamp.
struct MicCalibration
{
    bool  valid { false };
    float splAtFullScaleDb { 0.0f };

    // What the calibrator was presenting, and what we read back. Kept so the
    // number can be audited later rather than merely trusted.
    float referenceSplDb { 94.0f };
    float measuredDbFS { 0.0f };

    float toSplDb (float dbFS) const noexcept
    {
        if (! valid || dbFS <= kSilenceDb)
            return kSilenceDb;
        return dbFS + splAtFullScaleDb;
    }

    float toDbFS (float splDb) const noexcept
    {
        if (! valid || splDb <= kSilenceDb)
            return kSilenceDb;
        return splDb - splAtFullScaleDb;
    }

    static MicCalibration fromCalibrator (float referenceSplDb, float measuredDbFS) noexcept
    {
        MicCalibration c;
        c.referenceSplDb = referenceSplDb;
        c.measuredDbFS = measuredDbFS;
        c.splAtFullScaleDb = referenceSplDb - measuredDbFS;
        // A plausible range for a real measurement chain. Outside it, something
        // is wrong with the calibration - wrong input, wrong gain, no calibrator
        // actually fitted - and accepting the number would scale every safety
        // limit in the program by the size of the mistake.
        c.valid = std::isfinite (c.splAtFullScaleDb)
                  && c.splAtFullScaleDb > 60.0f
                  && c.splAtFullScaleDb < 180.0f;
        return c;
    }
};
} // namespace fbkt
