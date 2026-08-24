#include "WingProtocol.h"

#include <cstdio>
#include <cstring>

namespace fbkt
{
const char* toString (ConsoleControl c) noexcept
{
    switch (c)
    {
        case ConsoleControl::channelFader:       return "fader";
        case ConsoleControl::channelMute:        return "mute";
        case ConsoleControl::channelHeadAmpGain: return "headAmpGain";
        case ConsoleControl::channelSendLevel:   return "sendLevel";
        case ConsoleControl::channelName:        return "name";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Candidate addresses, in preference order.
//
// These are candidates, not documentation. The WING's tree differs from the
// X32's and has changed across firmware; what this console actually answers on is
// settled by WingDiscovery at setup time and stored with the rig. The list is
// ordered by what is most likely on current WING firmware first, with the older
// and X32-shaped forms after, so discovery normally resolves on its first try
// and still succeeds if the console is older than this program.
std::vector<AddressCandidate> candidatesFor (ConsoleControl control)
{
    switch (control)
    {
        case ConsoleControl::channelFader:
            return {
                { "/ch/{ch}/fdr",        ValueEncoding::decibelsFloat },
                { "/ch/{ch}/fader",      ValueEncoding::decibelsFloat },
                { "/ch/{ch}/mix/fader",  ValueEncoding::normalisedFloat },
                { "/ch/{ch}/mix/fdr",    ValueEncoding::decibelsFloat },
            };

        case ConsoleControl::channelMute:
            return {
                // "mute = 1 means muted" and "on = 1 means audible" are both in
                // use; the sense travels with the candidate.
                { "/ch/{ch}/mute",    ValueEncoding::integerFlag, false },
                { "/ch/{ch}/mix/on",  ValueEncoding::integerFlag, true  },
                { "/ch/{ch}/on",      ValueEncoding::integerFlag, true  },
            };

        case ConsoleControl::channelHeadAmpGain:
            return {
                { "/ch/{ch}/in/set/g",   ValueEncoding::decibelsFloat },
                { "/ch/{ch}/in/g",       ValueEncoding::decibelsFloat },
                { "/ch/{ch}/preamp/gain",ValueEncoding::decibelsFloat },
            };

        case ConsoleControl::channelSendLevel:
            return {
                { "/ch/{ch}/send/{bus}/lvl",  ValueEncoding::decibelsFloat },
                { "/ch/{ch}/send/{bus}/fdr",  ValueEncoding::decibelsFloat },
                { "/ch/{ch}/mix/{bus}/level", ValueEncoding::normalisedFloat },
            };

        case ConsoleControl::channelName:
            return {
                { "/ch/{ch}/$name", ValueEncoding::integerFlag },
                { "/ch/{ch}/name",  ValueEncoding::integerFlag },
                { "/ch/{ch}/cfg/name", ValueEncoding::integerFlag },
            };
    }
    return {};
}

// ---------------------------------------------------------------------------
std::string formatAddress (const char* pattern, int channel, int bus)
{
    if (pattern == nullptr)
        return {};

    std::string out;
    out.reserve (std::strlen (pattern) + 8);

    for (const char* p = pattern; *p != '\0'; )
    {
        if (std::strncmp (p, "{ch}", 4) == 0)
        {
            if (channel < 1)
                return {};                    // caller did not supply a channel
            char buf[16];
            std::snprintf (buf, sizeof (buf), "%d", channel);
            out += buf;
            p += 4;
        }
        else if (std::strncmp (p, "{bus}", 5) == 0)
        {
            if (bus < 1)
                return {};                    // caller did not supply a bus
            char buf[16];
            std::snprintf (buf, sizeof (buf), "%d", bus);
            out += buf;
            p += 5;
        }
        else
        {
            out += *p++;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
const ResolvedAddresses::Entry& ResolvedAddresses::forControl (ConsoleControl c) const noexcept
{
    switch (c)
    {
        case ConsoleControl::channelFader:       return fader;
        case ConsoleControl::channelMute:        return mute;
        case ConsoleControl::channelHeadAmpGain: return headAmpGain;
        case ConsoleControl::channelSendLevel:   return sendLevel;
        default:                                 return fader;
    }
}

ResolvedAddresses::Entry& ResolvedAddresses::forControl (ConsoleControl c) noexcept
{
    switch (c)
    {
        case ConsoleControl::channelFader:       return fader;
        case ConsoleControl::channelMute:        return mute;
        case ConsoleControl::channelHeadAmpGain: return headAmpGain;
        case ConsoleControl::channelSendLevel:   return sendLevel;
        default:                                 return fader;
    }
}

// ---------------------------------------------------------------------------
float clampFaderDb (float db) noexcept
{
    if (! std::isfinite (db))
        return kWingFaderMinDb;
    return clampf (db, kWingFaderMinDb, kWingFaderMaxDb);
}

// The Behringer fader law: four linear segments in the normalised domain, which
// between them put 20 dB across the top half of the travel and the remaining
// 60 dB across the bottom half. Straight-line normalisation would make a 0.5 dB
// step near unity indistinguishable from no step at all.
float faderDbToNormalised (float db) noexcept
{
    if (! std::isfinite (db) || db <= -90.0f)
        return 0.0f;

    float n;
    if (db < -60.0f)      n = (db + 90.0f) / 480.0f;
    else if (db < -30.0f) n = (db + 70.0f) / 160.0f;
    else if (db < -10.0f) n = (db + 50.0f) / 80.0f;
    else                  n = (db + 30.0f) / 40.0f;

    return clampf (n, 0.0f, 1.0f);
}

float normalisedToFaderDb (float norm) noexcept
{
    if (! std::isfinite (norm) || norm <= 0.0f)
        return kWingFaderMinDb;

    const float n = clampf (norm, 0.0f, 1.0f);

    if (n >= 0.5f)      return n * 40.0f - 30.0f;
    if (n >= 0.25f)     return n * 80.0f - 50.0f;
    if (n >= 0.0625f)   return n * 160.0f - 70.0f;
    return n * 480.0f - 90.0f;
}

float encodeFader (float db, ValueEncoding encoding) noexcept
{
    const float clamped = clampFaderDb (db);
    switch (encoding)
    {
        case ValueEncoding::decibelsFloat:   return clamped;
        case ValueEncoding::normalisedFloat: return faderDbToNormalised (clamped);
        case ValueEncoding::integerFlag:     return clamped > kWingFaderMinDb ? 1.0f : 0.0f;
    }
    return clamped;
}

float decodeFader (float wire, ValueEncoding encoding) noexcept
{
    switch (encoding)
    {
        case ValueEncoding::decibelsFloat:   return clampFaderDb (wire);
        case ValueEncoding::normalisedFloat: return normalisedToFaderDb (wire);
        case ValueEncoding::integerFlag:     return wire > 0.5f ? 0.0f : kWingFaderMinDb;
    }
    return clampFaderDb (wire);
}

bool faderAgrees (float commandedDb, float readBackDb, float toleranceDb) noexcept
{
    if (! std::isfinite (commandedDb) || ! std::isfinite (readBackDb))
        return false;

    // Both at the floor agree regardless of tolerance: consoles disagree about
    // whether "off" is -144, -140 or -oo, and all of them mean silent.
    if (commandedDb <= kWingFaderMinDb + 0.5f && readBackDb <= kWingFaderMinDb + 0.5f)
        return true;

    return std::abs (commandedDb - readBackDb) <= toleranceDb;
}

int mutedFlagValue (bool invertedFlag) noexcept   { return invertedFlag ? 0 : 1; }
int unmutedFlagValue (bool invertedFlag) noexcept { return invertedFlag ? 1 : 0; }
} // namespace fbkt
