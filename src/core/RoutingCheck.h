// FBKTrainer - RoutingCheck.h
//
// Decides whether the rig is actually wired the way the configuration claims,
// from measured levels. Pure arithmetic, no audio and no console, so every
// verdict below can be tested against numbers rather than against a room.
//
// Why this exists at all
// ----------------------
// The configuration says "the vocal microphone is console channel 12". Nothing
// checks that. If it is really on channel 14, then every gain command the trainer
// issues goes to a channel that is not in the feedback loop - and the symptom is
// not an error, it is a rig that raises gain steadily, sees no feedback, and
// keeps going. The trainer would conclude the room has enormous gain margin and
// push until something else in the system gave way.
//
// So the wiring is proved before any gain is permitted, and it is proved by
// observation rather than by assertion: move the control we believe is the vocal
// channel, and check that the level at the measurement microphone moves with it.
// A channel that does not respond is not the channel we think it is.
//
// The four checks, and what each one rules out:
//
//   speechReachesRoom   - the speech output actually drives a speaker.
//                         Rules out: wrong output port, muted output, dead amp.
//   roomReachesMic      - the measurement microphone hears that speaker.
//                         Rules out: wrong input port, dead capsule, phantom off.
//   loopExists          - the vocal channel hears it too, so there is a loop to
//                         work with at all.
//                         Rules out: vocal mic pointing away, wrong return port.
//   faderControlsLoop   - moving the assigned console fader moves the level.
//                         Rules out: the wrong channel number - the failure that
//                         otherwise presents as "enormous gain margin".
#pragma once

#include "Units.h"

#include <string>
#include <vector>

namespace fbkt
{
// Levels observed during the self test, all in dBFS at the relevant input.
struct RoutingMeasurements
{
    // With the speech output silent: the room's own noise floor.
    float silentMicDbFS { kSilenceDb };
    float silentVocalDbFS { kSilenceDb };

    // With the test signal playing, vocal fader at the low reference position.
    float testMicDbFS { kSilenceDb };
    float testVocalDbFS { kSilenceDb };

    // With the test signal playing and the vocal fader raised by faderDeltaDb.
    float raisedMicDbFS { kSilenceDb };
    float faderDeltaDb { 6.0f };

    // What the console reported the fader moved by, decoded through whatever
    // encoding discovery settled on. Discovery can find which addresses a console
    // answers on but cannot tell what units they are in - a fader at -10 dB reads
    // as "-10.0" in one encoding and "0.5" in the other, and both are plausible
    // numbers. Commanding a known move and checking the read-back agrees is what
    // settles it, and it has to be settled before any gain is raised: an encoding
    // that is wrong by a factor makes every subsequent step the wrong size.
    float readBackFaderDeltaDb { 0.0f };

    bool  consoleAcknowledged { false };
};

struct RoutingVerdict
{
    bool passed { false };

    bool speechReachesRoom { false };
    bool roomReachesMic { false };
    bool loopExists { false };
    bool faderControlsLoop { false };
    bool faderEncodingConfirmed { false };

    // How much the measurement microphone actually moved when the fader moved.
    // Reported whether or not the check passed, because the number is the useful
    // part: a channel that moves by 0.2 dB is a different problem from one that
    // moves by 5.8 dB when 6 was asked for.
    float observedFaderResponseDb { 0.0f };

    float micSignalToNoiseDb { 0.0f };
    float vocalSignalToNoiseDb { 0.0f };

    std::vector<std::string> failures;
};

struct RoutingThresholds
{
    // How far above the room's own noise the test signal must be before any
    // measurement made against it means anything.
    float minSignalToNoiseDb { 12.0f };

    // How much of the commanded fader move must show up at the microphone. Not
    // all of it: the measurement microphone hears the speaker directly as well as
    // through the vocal channel, and that direct path does not move with the
    // fader. Half is a comfortable margin over a coincidence and well under what
    // a correctly assigned channel produces.
    float minFaderResponseFraction { 0.5f };

    // A response larger than the commanded move means something else changed
    // during the measurement - someone spoke, a door closed, the room was not
    // quiet. The test is void rather than passed.
    float maxFaderResponseFraction { 1.8f };

    // How closely the console's own read-back must match the commanded move.
    // Tight, because this is arithmetic rather than acoustics: the console is
    // reporting its own fader, and if the encoding is right the two numbers agree
    // to within the fader's own resolution.
    float faderReadBackToleranceDb { 1.0f };
};

RoutingVerdict evaluateRouting (const RoutingMeasurements& m,
                                const RoutingThresholds& t = {});
} // namespace fbkt
