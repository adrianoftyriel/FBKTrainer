#include "RoutingCheck.h"

#include <cmath>
#include <cstdio>

namespace fbkt
{
namespace
{
std::string dbText (const char* prefix, float db, const char* suffix)
{
    char buf[192];
    std::snprintf (buf, sizeof (buf), "%s%.1f dB%s", prefix, static_cast<double> (db), suffix);
    return buf;
}
} // namespace

RoutingVerdict evaluateRouting (const RoutingMeasurements& m, const RoutingThresholds& t)
{
    RoutingVerdict v;

    v.micSignalToNoiseDb = m.testMicDbFS - m.silentMicDbFS;
    v.vocalSignalToNoiseDb = m.testVocalDbFS - m.silentVocalDbFS;
    v.observedFaderResponseDb = m.raisedMicDbFS - m.testMicDbFS;

    if (! m.consoleAcknowledged)
        v.failures.push_back ("The console did not acknowledge the test fader moves, so nothing measured here can be trusted.");

    // The measurement microphone must be hearing something well clear of the
    // room's own noise, or every later number is a measurement of the noise.
    v.roomReachesMic = v.micSignalToNoiseDb >= t.minSignalToNoiseDb;
    if (! v.roomReachesMic)
        v.failures.push_back (dbText ("The listening mic barely hears the test signal (", v.micSignalToNoiseDb,
                                      " above the room noise). Check the listening mic input assignment, the cable and phantom power."));

    // Distinguishing "no speaker" from "no microphone" is not possible from the
    // microphone alone, so the vocal return is what separates them: if neither
    // input hears anything, nothing is coming out of the speaker.
    v.speechReachesRoom = (v.micSignalToNoiseDb >= t.minSignalToNoiseDb)
                          || (v.vocalSignalToNoiseDb >= t.minSignalToNoiseDb);
    if (! v.speechReachesRoom)
        v.failures.push_back ("Neither the listening mic nor the vocal channel hears the test signal, so nothing is reaching the room. Check the speech output assignment and that the speaker is powered and turned up.");

    v.loopExists = v.vocalSignalToNoiseDb >= t.minSignalToNoiseDb;
    if (! v.loopExists)
        v.failures.push_back (dbText ("The vocal channel barely hears the test signal (", v.vocalSignalToNoiseDb,
                                      " above the room noise). There is no feedback loop to work with. Check the vocal mic position and the vocal return assignment."));

    // The check that matters most: proving the console channel we are about to
    // raise is the one in the loop.
    const float expected = m.faderDeltaDb;
    const float lower = expected * t.minFaderResponseFraction;
    const float upper = expected * t.maxFaderResponseFraction;

    v.faderControlsLoop = v.observedFaderResponseDb >= lower && v.observedFaderResponseDb <= upper;

    if (v.observedFaderResponseDb < lower)
    {
        char buf[320];
        std::snprintf (buf, sizeof (buf),
                       "Raising the vocal fader by %.1f dB moved the room level by only %.1f dB. "
                       "The assigned vocal channel is probably not the channel the microphone is on. "
                       "This is the failure that otherwise looks like enormous gain margin, so the rig will not run until it is fixed.",
                       static_cast<double> (expected), static_cast<double> (v.observedFaderResponseDb));
        v.failures.push_back (buf);
    }
    else if (v.observedFaderResponseDb > upper)
    {
        char buf[320];
        std::snprintf (buf, sizeof (buf),
                       "Raising the vocal fader by %.1f dB moved the room level by %.1f dB, which is more than the move can account for. "
                       "Something else changed during the test - keep the room quiet and run it again.",
                       static_cast<double> (expected), static_cast<double> (v.observedFaderResponseDb));
        v.failures.push_back (buf);
    }

    // Encoding check. Unlike everything else here this is not acoustic: the
    // console is reporting its own fader back to us, so if we are decoding it
    // correctly the two numbers agree closely. A mismatch means the units are
    // wrong, and wrong units mean every step the ramp takes is the wrong size -
    // which would be discovered later as either a rig that never reaches
    // instability or one that arrives there several steps sooner than planned.
    v.faderEncodingConfirmed =
        std::abs (m.readBackFaderDeltaDb - m.faderDeltaDb) <= t.faderReadBackToleranceDb;

    if (! v.faderEncodingConfirmed)
    {
        char buf[320];
        std::snprintf (buf, sizeof (buf),
                       "The fader was commanded up by %.1f dB and the console reported it moved by %.1f dB. "
                       "The address is answering but the units are being read wrongly, so every gain step would be the wrong size. "
                       "Re-run discovery, or set the fader encoding by hand.",
                       static_cast<double> (m.faderDeltaDb),
                       static_cast<double> (m.readBackFaderDeltaDb));
        v.failures.push_back (buf);
    }

    v.passed = m.consoleAcknowledged && v.speechReachesRoom && v.roomReachesMic
               && v.loopExists && v.faderControlsLoop && v.faderEncodingConfirmed;
    return v;
}
} // namespace fbkt
