// FBKTrainer - WingProtocol.h
//
// The address and value mapping for a Behringer WING, with no transport in it.
// Pure string and number manipulation, so it can be tested without a console on
// the desk.
//
// Why this is table-driven rather than hardcoded
// ----------------------------------------------
// The WING's OSC tree is not the X32's, and it has moved between firmware
// versions. Hardcoding an address that turns out to be wrong does not produce a
// clean failure - it produces a program that sends commands into the void while
// believing it is controlling a fader, which in a rig whose whole purpose is
// raising gain towards instability is an unacceptable failure mode.
//
// So every logical control carries a list of candidate addresses in preference
// order, and WingDiscovery resolves which one this particular console actually
// answers on. The resolved map is stored with the rig, so discovery is a
// setup-time act rather than something that happens on every run. If none of the
// candidates answers, the rig does not run - which is the correct outcome, and
// the one a hardcoded address would have hidden.
//
// Read-back is not optional
// -------------------------
// Every setter here has a matching query, and the run loop is required to
// confirm a commanded value before commanding the next one. OSC is UDP: a
// dropped packet is normal, not exceptional. A gain ramp that assumes its
// commands arrived is a gain ramp that can believe it is 6 dB lower than it is.
#pragma once

#include "Units.h"

#include <string>
#include <vector>

namespace fbkt
{
// The default OSC port for the WING. Configurable, because this is exactly the
// kind of detail that changes with firmware and the program should not have to
// be rebuilt to follow it.
inline constexpr int kWingDefaultOscPort = 2223;

// WING fader law limits, in dB. The floor is the console's own "-oo".
inline constexpr float kWingFaderMinDb = -144.0f;
inline constexpr float kWingFaderMaxDb = 10.0f;

// ---------------------------------------------------------------------------
enum class ConsoleControl
{
    channelFader,
    channelMute,
    channelHeadAmpGain,
    channelSendLevel,
    channelName
};

const char* toString (ConsoleControl) noexcept;

// How the console expects the value on the wire.
enum class ValueEncoding
{
    decibelsFloat,     // float, in dB, using the console's own fader law
    normalisedFloat,   // float in [0,1] across the fader law
    integerFlag        // int 0/1, for mutes and similar
};

struct AddressCandidate
{
    const char*   pattern;    // may contain {ch} and {bus}
    ValueEncoding encoding;

    // Some consoles express a mute as "muted = 1" and others as "on = 1". Getting
    // this backwards would mean the abort path unmutes the channel it is trying
    // to silence, so the sense is carried explicitly per candidate rather than
    // assumed from the control's name.
    bool invertedFlag { false };
};

// Candidates for one control, in the order they should be tried.
std::vector<AddressCandidate> candidatesFor (ConsoleControl control);

// Substitutes {ch} and {bus}. Returns an empty string if the pattern needs a
// placeholder that was not supplied, rather than emitting a malformed address -
// an address with a literal "{ch}" in it would be silently ignored by the
// console, which is the failure mode this whole file exists to avoid.
std::string formatAddress (const char* pattern, int channel, int bus = 0);

// ---------------------------------------------------------------------------
// What discovery worked out about this particular console. Stored with the rig.
struct ResolvedAddresses
{
    bool valid { false };
    std::string firmwareNote;         // whatever the console told us about itself

    struct Entry
    {
        bool          resolved { false };
        std::string   pattern;
        ValueEncoding encoding { ValueEncoding::decibelsFloat };
        bool          invertedFlag { false };
    };

    Entry fader, mute, headAmpGain, sendLevel;

    const Entry& forControl (ConsoleControl c) const noexcept;
    Entry& forControl (ConsoleControl c) noexcept;
};

// ---------------------------------------------------------------------------
// Value conversion. Kept separate from the address so that a console which uses
// a normalised fader on one control and dB on another is representable.

float clampFaderDb (float db) noexcept;

// The WING fader law is piecewise: it is not a straight line from -144 to +10,
// because that would put half the usable travel in the bottom 3 dB. This is the
// standard Behringer mapping, which allocates travel as 40 dB over the top
// quarter and progressively more per unit below that.
float faderDbToNormalised (float db) noexcept;
float normalisedToFaderDb (float norm) noexcept;

// Encode a fader position for the wire, given the encoding discovery resolved.
float encodeFader (float db, ValueEncoding encoding) noexcept;
float decodeFader (float wire, ValueEncoding encoding) noexcept;

// Whether a read-back agrees with what was commanded. Expressed in dB regardless
// of encoding, so the tolerance means the same thing on every console.
bool faderAgrees (float commandedDb, float readBackDb, float toleranceDb) noexcept;

// The wire value that means "this channel is silent", given the candidate's
// sense. Used by the abort path, which must be correct on the first attempt.
int mutedFlagValue (bool invertedFlag) noexcept;
int unmutedFlagValue (bool invertedFlag) noexcept;
} // namespace fbkt
