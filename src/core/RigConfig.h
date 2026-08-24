// FBKTrainer - RigConfig.h
//
// Everything about one physical setup: what is plugged into what, how to reach
// the console, and the limits the rig is never permitted to exceed.
//
// This is a plain value type with no I/O and no framework dependency so that the
// validation below can be unit-tested exhaustively. Loading and saving live in
// the app layer.
//
// On the limits
// -------------
// The safety limits are part of the rig configuration rather than the run
// configuration on purpose. A run is a thing you start, and things you start get
// started carelessly at two in the morning. A rig is a thing you set up once,
// with the boxes in front of you, and the limits belong to the boxes: this
// speaker melts at that level, this room is next to a bedroom, this driver has
// already been cooked once. Making them per-run would mean re-deciding them every
// time, which in practice means defaulting them, which means they stop being
// limits.
#pragma once

#include "Units.h"

#include <string>
#include <vector>

namespace fbkt
{
// ---------------------------------------------------------------------------
// The four assignments from the brief, plus the routing detail each one needs.
//
// Channel indices are 1-based to match every console surface and every piece of
// documentation the operator will read. Audio channel indices are 0-based
// because they index a driver's buffer array. The types are distinct so the two
// cannot be crossed silently.
struct ConsoleChannel
{
    int number { 0 };                  // 1-based console channel, 0 = unassigned
    bool isValid() const noexcept { return number >= 1 && number <= 40; }
};

struct AudioPort
{
    int index { -1 };                  // 0-based index into the audio device
    bool isValid() const noexcept { return index >= 0; }
};

// ---------------------------------------------------------------------------
struct SafetyLimits
{
    // The hard acoustic ceiling. Measured at the measurement microphone, so it is
    // a level at that position rather than at any particular distance from the
    // speaker - which is the right frame of reference, because the microphone is
    // the only place we can actually observe.
    //
    // 100 dB SPL is deliberately conservative: loud enough to excite the room
    // properly, quiet enough that a mistake is unpleasant rather than damaging,
    // and well under the level at which a compression driver is at thermal risk.
    float ceilingSplDb { 100.0f };

    // Where we start pulling gain back rather than waiting for the ceiling. The
    // gap between this and the ceiling is the headroom the ramp needs to stop in,
    // given that the console's own fader smoothing means a command takes tens of
    // milliseconds to take effect.
    float warnSplDb { 94.0f };

    // Absolute cap on the gain the trainer is permitted to command, in dB
    // relative to the operator-set starting point. The supervisor will not exceed
    // this no matter what any algorithm asks for. It exists so that a bug in the
    // search - which is, after all, the part of the program that is supposed to
    // be changing itself - cannot express itself as a fader at +10.
    float maxGainAboveStartDb { 12.0f };

    // Largest single step, and the minimum interval between steps. Small verified
    // steps rather than large ones, because every step is confirmed by read-back
    // before the next is permitted and a dropped packet mid-ramp must not be able
    // to leave the rig somewhere it was never commanded to be.
    float maxStepDb { 0.5f };
    int   minStepIntervalMs { 120 };

    // How long the rig may spend at or above the instability margin below, and
    // how long it must then rest. Feedback is a sustained sinusoid in the band a
    // compression driver is least able to dissipate, so the burst is kept to the
    // length needed to measure a growth rate and no longer.
    int   maxBurstMs { 400 };
    int   minCooldownMs { 4000 };

    // Normal running never comes closer than this to the predicted instability
    // point. Crossing it is a deliberate, separately-permitted act.
    float normalMarginDb { 3.0f };

    // Thermal budget for the high-frequency driver, as a first-order model: HF
    // energy charges an accumulator that discharges with the time constant below.
    // The limit is expressed as the equivalent number of seconds at the ceiling,
    // which is a quantity that can be reasoned about without knowing anything
    // about the driver's actual thermal mass.
    float thermalTimeConstantS { 90.0f };
    float thermalBudgetS { 20.0f };
    float thermalBandLowHz { 1500.0f };

    // If the supervisor is not ticked within this interval it aborts. This is the
    // dead-man's switch: a hung UI thread, a stalled audio callback or a process
    // that has stopped scheduling all present as a gap in ticks, and all three
    // must bring the gain down rather than leave it where it was.
    int   watchdogTimeoutMs { 750 };

    // A commanded gain change must be confirmed by console read-back within this
    // long, or the console is presumed unreachable and the rig is shut down.
    int   consoleAckTimeoutMs { 1000 };
};

// ---------------------------------------------------------------------------
enum class ConsoleModel
{
    wing,          // Behringer WING / WING Rack / WING Compact
    x32            // X32 family. Not implemented yet; present so the config
                   // format does not need a version bump when it is.
};

struct RigConfig
{
    std::string name { "Untitled rig" };
    ConsoleModel console { ConsoleModel::wing };

    // --- the four assignments ---
    AudioPort      speechOutput {};      // feeds the speaker in front of the vocal mic
    std::string    consoleAddress {};    // mixer IP, for OSC
    int            consolePort { 2223 };
    ConsoleChannel vocalChannel {};      // the microphone that will be fed back
    AudioPort      listeningMicInput {}; // measurement mic, our only view of the room

    // The vocal channel as it arrives back at the computer, so the trainer can see
    // what the suppressor sees rather than only what the room sounds like. Not
    // strictly required to run, but without it the only observation of the loop is
    // the measurement microphone, which cannot distinguish the channel's own
    // signal from everything else in the room.
    AudioPort vocalReturnInput {};

    // Which console control the trainer moves to change loop gain.
    //
    // The default is the fader rather than the head amp, and the reason is
    // structural: FBKSuppressor sits on the channel insert, so moving the head amp
    // changes both the loop gain and the level arriving at the plugin's own input,
    // confounding the two things we are trying to measure independently. Moving a
    // control after the insert point changes loop gain alone.
    enum class GainControl { fader, sendLevel, headAmp };
    GainControl gainControl { GainControl::fader };
    int  sendBus { 0 };                  // 1-based, only for sendLevel

    MicCalibration micCalibration {};
    SafetyLimits   limits {};

    double sampleRate { 48000.0 };
    std::string audioDeviceName {};      // one device for everything; see validate()
};

// ---------------------------------------------------------------------------
// Validation results. Two severities, because they mean genuinely different
// things: an error means the rig cannot run at all, a warning means it can run
// but some capability is missing or some setting is unusual.
struct ConfigIssue
{
    enum class Severity { error, warning };
    Severity severity { Severity::error };
    std::string field;
    std::string message;
};

// Returns every problem found rather than the first, so the operator can fix a
// misconfigured rig in one pass instead of playing whack-a-mole.
std::vector<ConfigIssue> validate (const RigConfig& config);

// True if nothing in the list is an error.
bool canRun (const std::vector<ConfigIssue>& issues) noexcept;

// True if the config is complete enough to raise gain. Stricter than canRun:
// this additionally requires a valid microphone calibration, because every
// acoustic limit in SafetyLimits is expressed in dB SPL and without the
// calibration constant those limits do not correspond to anything.
bool canRaiseGain (const RigConfig& config);
} // namespace fbkt
