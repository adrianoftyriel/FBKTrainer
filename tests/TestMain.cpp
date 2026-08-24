// FBKTrainer core tests.
//
// The tests that matter most here are the ones on SafetySupervisor, and the
// reason is worth stating plainly: this program's job is to raise the gain on a
// live PA towards instability and then leave it doing that, unattended, for days.
// Every other component can be debugged by watching it misbehave. The safety
// envelope cannot, because the whole point of it is the situations nobody is
// present for - the packet that never arrives, the microphone unplugged at three
// in the morning, the machine that decides to install an update mid-run.
//
// So those situations are arranged here instead, where they can be arranged
// exactly and repeatedly.
#include "TestFramework.h"

#include "GainRamp.h"
#include "RigConfig.h"
#include "RoutingCheck.h"
#include "SpeechCatalogue.h"
#include "SafetySupervisor.h"
#include "Units.h"
#include "WingProtocol.h"

#include <algorithm>
#include <cstdio>
#include <vector>

using namespace fbkt;

namespace
{
// A configuration that passes validation, so that each test can break exactly
// one thing and see only that failure.
RigConfig goodConfig()
{
    RigConfig c;
    c.name = "test rig";
    c.console = ConsoleModel::wing;
    c.speechOutput.index = 0;
    c.consoleAddress = "192.168.1.40";
    c.consolePort = kWingDefaultOscPort;
    c.vocalChannel.number = 1;
    c.listeningMicInput.index = 2;
    c.vocalReturnInput.index = 3;
    c.audioDeviceName = "WING";
    c.sampleRate = 48000.0;
    c.micCalibration = MicCalibration::fromCalibrator (94.0f, -26.0f);
    return c;
}

bool hasError (const std::vector<ConfigIssue>& issues, const std::string& field)
{
    return std::any_of (issues.begin(), issues.end(), [&] (const ConfigIssue& i)
                        { return i.severity == ConfigIssue::Severity::error && i.field == field; });
}

bool hasWarning (const std::vector<ConfigIssue>& issues, const std::string& field)
{
    return std::any_of (issues.begin(), issues.end(), [&] (const ConfigIssue& i)
                        { return i.severity == ConfigIssue::Severity::warning && i.field == field; });
}

// A healthy observation: everything nominal, console answering, room quiet.
SafetyObservation nominal (long long timeMs, const RigConfig& c)
{
    SafetyObservation o;
    o.timeMs = timeMs;
    o.audioRunning = true;
    o.consoleConnected = true;
    o.lastConsoleReplyMs = timeMs;
    o.commandedGainDb = 0.0f;
    o.confirmedGainDb = 0.0f;
    o.listeningMicDbFS = c.micCalibration.toDbFS (75.0f);
    o.listeningHfDbFS = c.micCalibration.toDbFS (65.0f);
    o.predictedInstabilityGainDb = 8.0f;
    return o;
}

// Runs the supervisor forward at 50 Hz, letting the caller adjust each
// observation. Returns the last decision.
template <typename Fn>
SafetyDecision run (SafetySupervisor& s, const RigConfig& c,
                    long long& t, int ticks, Fn&& adjust)
{
    SafetyDecision d;
    for (int i = 0; i < ticks; ++i)
    {
        t += 20;
        SafetyObservation o = nominal (t, c);
        adjust (o, i);
        d = s.tick (o);
    }
    return d;
}
} // namespace

// ---------------------------------------------------------------------------
void testUnits()
{
    test::beginTest ("Units - level arithmetic");

    CHECK_CLOSE (linearToDb (1.0f), 0.0, 1e-4);
    CHECK_CLOSE (linearToDb (0.5f), -6.0206, 1e-3);
    CHECK_CLOSE (dbToLinear (-6.0206f), 0.5, 1e-4);
    CHECK (linearToDb (0.0f) <= kSilenceDb);

    // Power and amplitude must not be interchangeable: mixing them up is a
    // factor of two in dB, which would make every acoustic limit wrong by an
    // amount large enough to matter and small enough to look plausible.
    CHECK_CLOSE (powerToDb (0.25f), -6.0206, 1e-3);
    CHECK_CLOSE (linearToDb (0.25f), -12.041, 1e-3);

    // Two equal uncorrelated levels sum to +3 dB.
    CHECK_CLOSE (sumDb (70.0f, 70.0f), 73.0103, 1e-3);
    CHECK_CLOSE (sumDb (70.0f, kSilenceDb), 70.0, 1e-4);

    test::beginTest ("Units - microphone calibration");

    const auto cal = MicCalibration::fromCalibrator (94.0f, -26.0f);
    CHECK (cal.valid);
    CHECK_CLOSE (cal.splAtFullScaleDb, 120.0, 1e-4);
    CHECK_CLOSE (cal.toSplDb (-40.0f), 80.0, 1e-4);
    CHECK_CLOSE (cal.toDbFS (100.0f), -20.0, 1e-4);

    // A calibration that implies an impossible chain is rejected rather than
    // accepted quietly. Accepting it would scale every acoustic limit in the
    // program by the size of the mistake.
    CHECK (! MicCalibration::fromCalibrator (94.0f, 60.0f).valid);   // implies 34
    CHECK (! MicCalibration::fromCalibrator (94.0f, -120.0f).valid); // implies 214

    const auto bad = MicCalibration::fromCalibrator (94.0f, 60.0f);
    CHECK (bad.toSplDb (-40.0f) <= kSilenceDb);   // invalid calibration reports nothing
}

// ---------------------------------------------------------------------------
void testConfigValidation()
{
    test::beginTest ("RigConfig - a complete rig validates");

    auto c = goodConfig();
    auto issues = validate (c);
    CHECK (canRun (issues));
    CHECK (canRaiseGain (c));

    test::beginTest ("RigConfig - each missing assignment is caught");

    for (int which = 0; which < 4; ++which)
    {
        auto broken = goodConfig();
        const char* field = "";
        switch (which)
        {
            case 0: broken.speechOutput.index = -1;      field = "speechOutput"; break;
            case 1: broken.consoleAddress.clear();       field = "consoleAddress"; break;
            case 2: broken.vocalChannel.number = 0;      field = "vocalChannel"; break;
            case 3: broken.listeningMicInput.index = -1; field = "listeningMicInput"; break;
        }
        const auto is = validate (broken);
        CHECK (hasError (is, field));
        CHECK (! canRun (is));
    }

    test::beginTest ("RigConfig - wiring mistakes and bad addresses");

    auto sameInput = goodConfig();
    sameInput.vocalReturnInput.index = sameInput.listeningMicInput.index;
    CHECK (! canRun (validate (sameInput)));

    auto badIp = goodConfig();
    badIp.consoleAddress = "192.168.1";
    CHECK (hasError (validate (badIp), "consoleAddress"));
    badIp.consoleAddress = "192.168.1.999";
    CHECK (hasError (validate (badIp), "consoleAddress"));
    badIp.consoleAddress = "192.168.1.40";
    CHECK (canRun (validate (badIp)));

    test::beginTest ("RigConfig - uncalibrated rig may run but may not raise gain");

    auto uncal = goodConfig();
    uncal.micCalibration = MicCalibration {};
    const auto uncalIssues = validate (uncal);
    CHECK (canRun (uncalIssues));                 // measuring and playing is fine
    CHECK (hasWarning (uncalIssues, "micCalibration"));
    CHECK (! canRaiseGain (uncal));               // raising gain is not

    test::beginTest ("RigConfig - self-contradictory limits are rejected");

    auto noHeadroom = goodConfig();
    noHeadroom.limits.warnSplDb = 105.0f;         // above the ceiling
    CHECK (! canRun (validate (noHeadroom)));

    auto shortCooldown = goodConfig();
    shortCooldown.limits.maxBurstMs = 400;
    shortCooldown.limits.minCooldownMs = 200;     // shorter than the burst
    CHECK (! canRun (validate (shortCooldown)));

    auto bigStep = goodConfig();
    bigStep.limits.maxStepDb = 6.0f;
    CHECK (! canRun (validate (bigStep)));

    // X32 is configurable but not implemented, and says so rather than silently
    // behaving like a WING.
    auto x32 = goodConfig();
    x32.console = ConsoleModel::x32;
    CHECK (! canRun (validate (x32)));
}

// ---------------------------------------------------------------------------
void testWingProtocol()
{
    test::beginTest ("WingProtocol - address formatting");

    CHECK (formatAddress ("/ch/{ch}/fdr", 7) == "/ch/7/fdr");
    CHECK (formatAddress ("/ch/{ch}/send/{bus}/lvl", 3, 5) == "/ch/3/send/5/lvl");
    CHECK (formatAddress ("/no/placeholders", 0) == "/no/placeholders");

    // A pattern whose placeholder cannot be filled returns nothing rather than an
    // address containing a literal "{ch}". The console would ignore such an
    // address silently, which is the one outcome this program cannot tolerate:
    // commands that appear to be sent and do nothing.
    CHECK (formatAddress ("/ch/{ch}/fdr", 0).empty());
    CHECK (formatAddress ("/ch/{ch}/send/{bus}/lvl", 3, 0).empty());
    CHECK (formatAddress (nullptr, 1).empty());

    test::beginTest ("WingProtocol - fader law round trip");

    // Round trip over the usable range. The law's bottom segment approaches
    // -90 dB as the normalised value approaches zero, so -90 is the bottom of
    // what the law can express rather than a point on it.
    for (float db = -89.5f; db <= 10.0f; db += 0.5f)
    {
        const float n = faderDbToNormalised (db);
        CHECK (n >= 0.0f && n <= 1.0f);
        CHECK_CLOSE (normalisedToFaderDb (n), db, 0.01);
    }

    // At and below -90 dB the law collapses to zero, and zero decodes to the
    // console's floor rather than back to -90. That asymmetry is deliberate: a
    // fader at the bottom of its travel is off, and reporting it as "-90 dB"
    // would claim the channel is passing audio when it is not.
    CHECK_CLOSE (faderDbToNormalised (-90.0f), 0.0, 1e-6);
    CHECK_CLOSE (faderDbToNormalised (-200.0f), 0.0, 1e-6);
    CHECK_CLOSE (normalisedToFaderDb (0.0f), kWingFaderMinDb, 1e-4);
    CHECK_CLOSE (encodeFader (kWingFaderMinDb, ValueEncoding::normalisedFloat), 0.0, 1e-6);

    // The law must be monotonic, or a step upwards could decode as a step down.
    float previous = -1.0f;
    for (float db = -90.0f; db <= 10.0f; db += 0.25f)
    {
        const float n = faderDbToNormalised (db);
        CHECK (n >= previous);
        previous = n;
    }

    // And it must allocate enough travel near unity for the step sizes we use:
    // half a decibel at the working point has to be a distinguishable move.
    CHECK_GT (faderDbToNormalised (0.0f) - faderDbToNormalised (-0.5f), 0.005f);

    CHECK_CLOSE (clampFaderDb (100.0f), kWingFaderMaxDb, 1e-4);
    CHECK_CLOSE (clampFaderDb (-500.0f), kWingFaderMinDb, 1e-4);
    CHECK_CLOSE (clampFaderDb (std::nanf ("")), kWingFaderMinDb, 1e-4);

    test::beginTest ("WingProtocol - encodings and read-back agreement");

    CHECK_CLOSE (encodeFader (-6.0f, ValueEncoding::decibelsFloat), -6.0, 1e-4);
    CHECK_CLOSE (decodeFader (encodeFader (-6.0f, ValueEncoding::normalisedFloat),
                              ValueEncoding::normalisedFloat), -6.0, 0.01);

    CHECK (faderAgrees (-6.0f, -6.05f, 0.25f));
    CHECK (! faderAgrees (-6.0f, -7.0f, 0.25f));
    // Consoles disagree about what "off" is; all the answers mean silent.
    CHECK (faderAgrees (kWingFaderMinDb, -144.0f, 0.01f));
    CHECK (! faderAgrees (-6.0f, std::nanf (""), 10.0f));

    test::beginTest ("WingProtocol - mute sense travels with the candidate");

    // Getting this backwards would mean the abort path unmutes the channel it is
    // trying to silence.
    CHECK (mutedFlagValue (false) == 1);      // "/mute": 1 means muted
    CHECK (unmutedFlagValue (false) == 0);
    CHECK (mutedFlagValue (true) == 0);       // "/mix/on": 0 means muted
    CHECK (unmutedFlagValue (true) == 1);

    for (auto control : { ConsoleControl::channelFader, ConsoleControl::channelMute,
                          ConsoleControl::channelHeadAmpGain, ConsoleControl::channelSendLevel })
    {
        const auto list = candidatesFor (control);
        CHECK (! list.empty());
        for (const auto& cand : list)
            CHECK (cand.pattern != nullptr && cand.pattern[0] == '/');
    }
}

// ---------------------------------------------------------------------------
void testGainRamp()
{
    test::beginTest ("GainRamp - increases are stepped, rate-limited and confirmed");

    SafetyLimits limits;
    limits.maxStepDb = 0.5f;
    limits.minStepIntervalMs = 100;

    GainRamp ramp;
    ramp.configure (limits, -10.0f);
    ramp.setTarget (6.0f);

    long long t = 0;
    auto cmd = ramp.update (t, 12.0f);
    CHECK (cmd.issue);
    CHECK_CLOSE (cmd.gainDb, 0.5, 1e-4);              // one step, not the whole way
    CHECK_CLOSE (cmd.faderDb, -9.5, 1e-4);            // relative to the start point
    CHECK (ramp.awaitingConfirmation());

    // No further increase until the console confirms, however long we wait.
    t += 5000;
    CHECK (! ramp.update (t, 12.0f).issue);
    CHECK_CLOSE (ramp.commandedGainDb(), 0.5, 1e-4);

    ramp.notifyConfirmed (0.5f, t);
    CHECK (! ramp.awaitingConfirmation());

    // Confirmed, but the rate limiter still applies.
    CHECK (! ramp.update (t + 50, 12.0f).issue);
    cmd = ramp.update (t + 150, 12.0f);
    CHECK (cmd.issue);
    CHECK_CLOSE (cmd.gainDb, 1.0, 1e-4);

    test::beginTest ("GainRamp - a wrong confirmation does not clear the wait");

    ramp.notifyConfirmed (0.2f, t + 150);             // not what we asked for
    CHECK (ramp.awaitingConfirmation());
    CHECK (! ramp.update (t + 400, 12.0f).issue);

    test::beginTest ("GainRamp - the permission ceiling is obeyed");

    GainRamp r2;
    r2.configure (limits, -10.0f);
    r2.setTarget (10.0f);
    long long t2 = 0;
    for (int i = 0; i < 40; ++i)
    {
        t2 += 150;
        const auto c = r2.update (t2, 1.0f);          // permitted only +1 dB
        if (c.issue)
            r2.notifyConfirmed (c.gainDb, t2);
    }
    CHECK_CLOSE (r2.commandedGainDb(), 1.0, 1e-4);

    test::beginTest ("GainRamp - reductions are immediate and unbounded");

    // The asymmetry that matters: a large reduction happens in one command, is
    // not rate-limited, and does not wait for any confirmation - including while
    // an increase is still in flight.
    GainRamp r3;
    r3.configure (limits, -10.0f);
    r3.setTarget (14.0f);          // still climbing after the loop below
    long long t3 = 0;
    for (int i = 0; i < 20; ++i)
    {
        t3 += 150;
        const auto c = r3.update (t3, 12.0f);
        if (c.issue)
            r3.notifyConfirmed (c.gainDb, t3);
    }
    CHECK_GT (r3.commandedGainDb(), 4.0f);

    const auto issued = r3.update (t3 + 200, 12.0f);   // an increase in flight
    CHECK (issued.issue);
    CHECK (r3.awaitingConfirmation());

    const auto drop = r3.update (t3 + 210, -20.0f);   // permission collapses
    CHECK (drop.issue);
    CHECK (drop.isReduction);
    CHECK_CLOSE (drop.gainDb, -20.0, 1e-4);           // all the way, in one step
    CHECK (! r3.awaitingConfirmation());

    test::beginTest ("GainRamp - panic goes to the floor and stays there");

    GainRamp r4;
    r4.configure (limits, -10.0f);
    r4.setTarget (6.0f);
    long long t4 = 0;
    for (int i = 0; i < 20; ++i)
    {
        t4 += 150;
        const auto c = r4.update (t4, 12.0f);
        if (c.issue)
            r4.notifyConfirmed (c.gainDb, t4);
    }

    r4.panic();
    const auto p = r4.update (t4 + 10, 12.0f);
    CHECK (p.issue);
    CHECK (p.isReduction);
    CHECK_CLOSE (p.faderDb, kWingFaderMinDb, 1e-4);

    // A panicked ramp does not climb again just because permission returns.
    r4.setTarget (6.0f);
    for (int i = 0; i < 20; ++i)
    {
        t4 += 150;
        CHECK (! r4.update (t4, 12.0f).issue);
    }
}

// ---------------------------------------------------------------------------
void testSupervisorBasics()
{
    test::beginTest ("Supervisor - idle grants nothing");

    auto c = goodConfig();
    SafetySupervisor s;
    s.configure (c);

    long long t = 1000;
    auto d = s.tick (nominal (t, c));
    CHECK (d.state == SafetyState::idle);
    CHECK_CLOSE (d.permittedGainDb, kGainOffDb, 1e-4);
    CHECK (! d.mayIncrease);

    test::beginTest ("Supervisor - an unmeasured loop may not be pushed at all");

    // Without a sweep the instability point could be anywhere, including below
    // where we already are, so no increase above the start point is permitted.
    s.arm (t);
    d = run (s, c, t, 5, [] (SafetyObservation& o, int)
             { o.predictedInstabilityGainDb = std::numeric_limits<float>::quiet_NaN(); });
    CHECK (d.state == SafetyState::running);
    CHECK_LT (d.permittedGainDb, 0.01f);
    CHECK (! d.mayIncrease);

    test::beginTest ("Supervisor - a measured loop permits gain up to the margin");

    d = run (s, c, t, 5, [] (SafetyObservation& o, int) { o.predictedInstabilityGainDb = 8.0f; });
    CHECK (d.state == SafetyState::running);
    // 8 dB to instability, 3 dB margin, so 5 dB is the ceiling.
    CHECK_CLOSE (d.permittedGainDb, 5.0, 1e-3);
    CHECK (d.mayIncrease);

    test::beginTest ("Supervisor - the absolute cap outranks the measurement");

    // A measurement claiming the loop is stable 40 dB up does not unlock 40 dB.
    // The cap exists because the search is the part of the program that is
    // supposed to be changing itself, and a bug in it must not be expressible as
    // a fader at +40.
    d = run (s, c, t, 5, [] (SafetyObservation& o, int) { o.predictedInstabilityGainDb = 40.0f; });
    CHECK_CLOSE (d.permittedGainDb, c.limits.maxGainAboveStartDb, 1e-3);
}

// ---------------------------------------------------------------------------
void testSupervisorAborts()
{
    test::beginTest ("Supervisor - the SPL ceiling aborts, and the abort latches");

    auto c = goodConfig();
    SafetySupervisor s;
    s.configure (c);
    long long t = 1000;
    s.arm (t);
    run (s, c, t, 5, [] (SafetyObservation&, int) {});

    auto d = run (s, c, t, 1, [&] (SafetyObservation& o, int)
                  { o.listeningMicDbFS = c.micCalibration.toDbFS (c.limits.ceilingSplDb + 0.5f); });
    CHECK (d.state == SafetyState::aborted);
    CHECK (d.reason == AbortReason::splCeiling);
    CHECK (d.abortRaised);
    CHECK_CLOSE (d.permittedGainDb, kGainOffDb, 1e-4);

    // Latched: a room that has gone quiet again does not un-abort the run. An
    // automatic recovery would let the rig re-enter the condition indefinitely,
    // unattended, which is the scenario the abort exists to prevent.
    d = run (s, c, t, 50, [] (SafetyObservation&, int) {});
    CHECK (d.state == SafetyState::aborted);
    CHECK (! d.abortRaised);                          // raised once, not repeatedly
    CHECK_CLOSE (d.permittedGainDb, kGainOffDb, 1e-4);

    // arm() does not clear it either; only an explicit reset does.
    s.arm (t);
    CHECK (s.isAborted());
    s.reset();
    CHECK (s.state() == SafetyState::idle);
    CHECK (s.abortReason() == AbortReason::none);

    test::beginTest ("Supervisor - the first reason wins");

    SafetySupervisor s2;
    s2.configure (c);
    long long t2 = 1000;
    s2.arm (t2);
    run (s2, c, t2, 5, [] (SafetyObservation&, int) {});
    // Everything fails at once. The report should name what happened, not the
    // last consequence to be noticed.
    auto d2 = run (s2, c, t2, 1, [&] (SafetyObservation& o, int)
                   {
                       o.audioRunning = false;
                       o.consoleConnected = false;
                       o.listeningMicDbFS = c.micCalibration.toDbFS (120.0f);
                   });
    CHECK (d2.state == SafetyState::aborted);
    CHECK (d2.reason == AbortReason::audioStopped);
}

// ---------------------------------------------------------------------------
void testSupervisorWatchdog()
{
    test::beginTest ("Supervisor - a gap in ticks aborts");

    auto c = goodConfig();
    SafetySupervisor s;
    s.configure (c);
    long long t = 1000;
    s.arm (t);
    run (s, c, t, 5, [] (SafetyObservation&, int) {});

    // The machine stalls for two seconds - a driver, an update, a swap storm.
    t += 2000;
    auto d = s.tick (nominal (t, c));
    CHECK (d.state == SafetyState::aborted);
    CHECK (d.reason == AbortReason::watchdogTimeout);

    test::beginTest ("Supervisor - isOverdue reports the stall before tick() can");

    // tick() only ever notices a gap once it resumes, which is after the gap.
    // isOverdue() is what an independent watchdog thread calls, and it is the
    // actual dead-man's switch.
    SafetySupervisor s2;
    s2.configure (c);
    long long t2 = 1000;
    s2.arm (t2);
    run (s2, c, t2, 5, [] (SafetyObservation&, int) {});

    CHECK (! s2.isOverdue (t2 + 100));
    CHECK (! s2.isOverdue (t2 + c.limits.watchdogTimeoutMs - 10));
    CHECK (s2.isOverdue (t2 + c.limits.watchdogTimeoutMs + 10));

    // An idle or already-aborted supervisor is not "overdue" - there is nothing
    // running for the watchdog to rescue, and reporting otherwise would mean the
    // watchdog fires forever after a normal stop.
    s2.disarm();
    CHECK (! s2.isOverdue (t2 + 100000));
}

// ---------------------------------------------------------------------------
void testSupervisorConsole()
{
    auto c = goodConfig();

    test::beginTest ("Supervisor - a silent console aborts");

    SafetySupervisor s;
    s.configure (c);
    long long t = 1000;
    s.arm (t);
    run (s, c, t, 5, [] (SafetyObservation&, int) {});

    auto d = run (s, c, t, 3, [&] (SafetyObservation& o, int)
                  { o.lastConsoleReplyMs = o.timeMs - c.limits.consoleAckTimeoutMs - 100; });
    CHECK (d.state == SafetyState::aborted);
    CHECK (d.reason == AbortReason::consoleUnreachable);

    test::beginTest ("Supervisor - a step in flight is not a mismatch");

    SafetySupervisor s2;
    s2.configure (c);
    long long t2 = 1000;
    s2.arm (t2);
    // Commanded half a step ahead of confirmed, for a long time. This is what a
    // healthy ramp looks like and must never abort.
    auto d2 = run (s2, c, t2, 200, [] (SafetyObservation& o, int)
                   {
                       o.commandedGainDb = 3.0f;
                       o.confirmedGainDb = 2.75f;
                   });
    CHECK (d2.state != SafetyState::aborted);

    test::beginTest ("Supervisor - a sustained mismatch aborts");

    SafetySupervisor s3;
    s3.configure (c);
    long long t3 = 1000;
    s3.arm (t3);
    // The console is somewhere we never put it. More dangerous than silence,
    // because it means commands are landing unpredictably rather than not at all.
    auto d3 = run (s3, c, t3, 200, [] (SafetyObservation& o, int)
                   {
                       o.commandedGainDb = 3.0f;
                       o.confirmedGainDb = 0.0f;
                   });
    CHECK (d3.state == SafetyState::aborted);
    CHECK (d3.reason == AbortReason::consoleMismatch);

    test::beginTest ("Supervisor - a transient mismatch that resolves does not abort");

    SafetySupervisor s4;
    s4.configure (c);
    long long t4 = 1000;
    s4.arm (t4);
    auto d4 = run (s4, c, t4, 200, [&] (SafetyObservation& o, int i)
                   {
                       o.commandedGainDb = 3.0f;
                       // Disagrees for 10 ticks (200 ms), then catches up.
                       o.confirmedGainDb = (i >= 20 && i < 30) ? 0.0f : 3.0f;
                   });
    CHECK (d4.state != SafetyState::aborted);
}

// ---------------------------------------------------------------------------
void testSupervisorMicrophone()
{
    auto c = goodConfig();

    test::beginTest ("Supervisor - an uncalibrated rig cannot run at all");

    // Every acoustic limit is in dB SPL. Without the calibration constant those
    // limits do not correspond to anything, so running would mean running with
    // no enforceable ceiling.
    auto uncal = goodConfig();
    uncal.micCalibration = MicCalibration {};
    SafetySupervisor su;
    su.configure (uncal);
    long long tu = 1000;
    su.arm (tu);
    auto du = run (su, uncal, tu, 2, [] (SafetyObservation&, int) {});
    CHECK (du.state == SafetyState::aborted);
    CHECK (du.reason == AbortReason::micUncalibrated);

    test::beginTest ("Supervisor - a dead measurement microphone aborts");

    SafetySupervisor s;
    s.configure (c);
    long long t = 1000;
    s.arm (t);
    run (s, c, t, 5, [] (SafetyObservation&, int) {});

    // Silence for well over the hold interval while the rig is making noise. The
    // microphone is our only view of the room; without it every limit is
    // unenforceable and the rig is blind.
    auto d = run (s, c, t, 150, [] (SafetyObservation& o, int)
                  { o.listeningMicDbFS = -120.0f; });
    CHECK (d.state == SafetyState::aborted);
    CHECK (d.reason == AbortReason::micDead);

    test::beginTest ("Supervisor - a pause in speech is not a dead microphone");

    SafetySupervisor s2;
    s2.configure (c);
    long long t2 = 1000;
    s2.arm (t2);
    // Half-second gaps, repeatedly. Normal speech; must not abort.
    auto d2 = run (s2, c, t2, 400, [&] (SafetyObservation& o, int i)
                   {
                       if ((i % 50) < 25)
                           o.listeningMicDbFS = -120.0f;
                   });
    CHECK (d2.state != SafetyState::aborted);
}

// ---------------------------------------------------------------------------
void testSupervisorGovernor()
{
    test::beginTest ("Supervisor - above the warning level, gain is given back");

    auto c = goodConfig();
    SafetySupervisor s;
    s.configure (c);
    long long t = 1000;
    s.arm (t);
    run (s, c, t, 5, [] (SafetyObservation&, int) {});

    const float over = 2.0f;
    auto d = run (s, c, t, 2, [&] (SafetyObservation& o, int)
                  {
                      o.commandedGainDb = 4.0f;
                      o.confirmedGainDb = 4.0f;
                      o.listeningMicDbFS = c.micCalibration.toDbFS (c.limits.warnSplDb + over);
                  });
    CHECK (d.state != SafetyState::aborted);
    CHECK (! d.mayIncrease);
    // Pulled back in proportion to the overshoot, so the response is gentle near
    // the warning level and aggressive near the ceiling.
    CHECK_CLOSE (d.permittedGainDb, 4.0 - over, 1e-3);

    test::beginTest ("Supervisor - below the warning level, gain is permitted again");

    d = run (s, c, t, 2, [&] (SafetyObservation& o, int)
             {
                 o.commandedGainDb = 4.0f;
                 o.confirmedGainDb = 4.0f;
                 o.listeningMicDbFS = c.micCalibration.toDbFS (c.limits.warnSplDb - 6.0f);
             });
    CHECK (d.mayIncrease);
    CHECK_CLOSE (d.permittedGainDb, 5.0, 1e-3);
}

// ---------------------------------------------------------------------------
void testSupervisorBurstAndThermal()
{
    test::beginTest ("Supervisor - a burst crosses the margin, then expires into cooldown");

    auto c = goodConfig();
    SafetySupervisor s;
    s.configure (c);
    long long t = 1000;
    s.arm (t);
    run (s, c, t, 5, [] (SafetyObservation&, int) {});

    // Ask to cross. Permission should now reach past the predicted instability
    // point rather than stopping short of it - that crossing is what produces
    // oscillation at all.
    auto d = run (s, c, t, 2, [] (SafetyObservation& o, int)
                  {
                      o.burstRequested = true;
                      o.commandedGainDb = 5.0f;
                      o.confirmedGainDb = 5.0f;
                  });
    CHECK (d.state == SafetyState::burst);
    CHECK_GT (d.permittedGainDb, 8.0f);

    // Held past the burst limit, it drops to cooldown on its own, whether or not
    // the caller stops asking.
    d = run (s, c, t, 40, [] (SafetyObservation& o, int)
             {
                 o.burstRequested = true;
                 o.commandedGainDb = 5.0f;
                 o.confirmedGainDb = 5.0f;
             });
    CHECK (d.state == SafetyState::cooldown);
    CHECK (! d.mayIncrease);
    CHECK_LT (d.permittedGainDb, 5.01f);
    CHECK_GT (s.burstMillisTotal(), 300);

    // And it stays in cooldown for the full rest interval, still refusing.
    d = run (s, c, t, 100, [] (SafetyObservation& o, int)
             {
                 o.burstRequested = true;
                 o.commandedGainDb = 5.0f;
                 o.confirmedGainDb = 5.0f;
             });
    CHECK (d.state == SafetyState::cooldown);

    // After the cooldown elapses, normal running resumes.
    d = run (s, c, t, 150, [] (SafetyObservation& o, int)
             {
                 o.commandedGainDb = 5.0f;
                 o.confirmedGainDb = 5.0f;
             });
    CHECK (d.state == SafetyState::running);

    test::beginTest ("Supervisor - the thermal budget accumulates and blocks");

    auto hot = goodConfig();
    hot.limits.thermalBudgetS = 2.0f;             // a small budget, to reach quickly
    hot.limits.thermalTimeConstantS = 600.0f;     // and a slow recovery

    SafetySupervisor s2;
    s2.configure (hot);
    long long t2 = 1000;
    s2.arm (t2);
    run (s2, hot, t2, 5, [] (SafetyObservation&, int) {});

    // Sustained high-frequency energy at the ceiling: one second of budget per
    // second, which is exactly what a howl in the 2-8 kHz region costs a
    // compression driver.
    auto d2 = run (s2, hot, t2, 300, [&] (SafetyObservation& o, int)
                   {
                       o.listeningHfDbFS = hot.micCalibration.toDbFS (hot.limits.ceilingSplDb);
                       o.listeningMicDbFS = hot.micCalibration.toDbFS (hot.limits.ceilingSplDb - 8.0f);
                       o.commandedGainDb = 4.0f;
                       o.confirmedGainDb = 4.0f;
                   });
    CHECK_GT (d2.thermalFraction, 1.0f);
    CHECK (! d2.mayIncrease);
    CHECK_LT (d2.permittedGainDb, 4.01f);

    test::beginTest ("Supervisor - a quiet room does not consume the budget");

    SafetySupervisor s3;
    s3.configure (c);
    long long t3 = 1000;
    s3.arm (t3);
    auto d3 = run (s3, c, t3, 500, [&] (SafetyObservation& o, int)
                   { o.listeningHfDbFS = c.micCalibration.toDbFS (55.0f); });
    CHECK_LT (d3.thermalFraction, 0.05f);
    CHECK (d3.state != SafetyState::aborted);
}

// ---------------------------------------------------------------------------
void testRoutingCheck()
{
    // A rig wired exactly as configured: strong signal at both inputs, and the
    // room level following the vocal fader.
    auto healthy = [] {
        RoutingMeasurements m;
        m.silentMicDbFS = -70.0f;
        m.silentVocalDbFS = -68.0f;
        m.testMicDbFS = -30.0f;
        m.testVocalDbFS = -28.0f;
        m.faderDeltaDb = 6.0f;
        m.raisedMicDbFS = -25.0f;      // 5 of the 6 dB shows up
        m.readBackFaderDeltaDb = 6.0f; // and the console agrees it moved by 6
        m.consoleAcknowledged = true;
        return m;
    };

    test::beginTest ("RoutingCheck - a correctly wired rig passes");

    auto v = evaluateRouting (healthy());
    CHECK (v.passed);
    CHECK (v.speechReachesRoom && v.roomReachesMic && v.loopExists && v.faderControlsLoop);
    CHECK (v.failures.empty());
    CHECK_CLOSE (v.observedFaderResponseDb, 5.0, 1e-3);

    test::beginTest ("RoutingCheck - the wrong console channel is caught");

    // The failure this whole check exists for. The fader moves, the console
    // acknowledges, both microphones hear the speech - and the room level does
    // not follow, because the channel being raised is not the one the microphone
    // is on. Left undetected, the trainer raises gain, sees no feedback, and
    // concludes the room has enormous margin.
    auto wrongChannel = healthy();
    wrongChannel.raisedMicDbFS = wrongChannel.testMicDbFS + 0.3f;
    v = evaluateRouting (wrongChannel);
    CHECK (! v.passed);
    CHECK (! v.faderControlsLoop);
    CHECK (v.roomReachesMic);          // everything else looked fine
    CHECK (v.loopExists);
    CHECK (! v.failures.empty());

    test::beginTest ("RoutingCheck - a partial response is still a failure");

    // Half the commanded move is the threshold, because the measurement mic also
    // hears the speaker directly and that path does not move with the fader.
    auto partial = healthy();
    partial.raisedMicDbFS = partial.testMicDbFS + 2.0f;   // 2 of 6
    CHECK (! evaluateRouting (partial).faderControlsLoop);

    partial.raisedMicDbFS = partial.testMicDbFS + 3.5f;   // comfortably over half
    CHECK (evaluateRouting (partial).faderControlsLoop);

    test::beginTest ("RoutingCheck - an impossibly large response voids the test");

    // More movement than the fader can account for means something else changed
    // during the measurement. Void, not passed.
    auto disturbed = healthy();
    disturbed.raisedMicDbFS = disturbed.testMicDbFS + 14.0f;
    v = evaluateRouting (disturbed);
    CHECK (! v.passed);
    CHECK (! v.faderControlsLoop);

    test::beginTest ("RoutingCheck - dead inputs are told apart");

    // Listening mic dead, vocal channel fine: the speaker is working, so the
    // fault is on the measurement side.
    auto deadMic = healthy();
    deadMic.testMicDbFS = deadMic.silentMicDbFS + 1.0f;
    deadMic.raisedMicDbFS = deadMic.testMicDbFS;
    v = evaluateRouting (deadMic);
    CHECK (! v.passed);
    CHECK (! v.roomReachesMic);
    CHECK (v.speechReachesRoom);       // the vocal channel proves the speaker works
    CHECK (v.loopExists);

    // Nothing reaching the room at all: both inputs quiet.
    auto deadSpeaker = healthy();
    deadSpeaker.testMicDbFS = deadSpeaker.silentMicDbFS + 1.0f;
    deadSpeaker.raisedMicDbFS = deadSpeaker.testMicDbFS;
    deadSpeaker.testVocalDbFS = deadSpeaker.silentVocalDbFS + 1.0f;
    v = evaluateRouting (deadSpeaker);
    CHECK (! v.passed);
    CHECK (! v.speechReachesRoom);
    CHECK (! v.loopExists);

    test::beginTest ("RoutingCheck - a wrong fader encoding is caught");

    // The console answers, the room responds, everything acoustic looks right -
    // but we are reading its fader in the wrong units, so a 6 dB command reads
    // back as a fraction of a dB. Left undetected, every gain step the ramp takes
    // would be the wrong size.
    auto wrongUnits = healthy();
    wrongUnits.readBackFaderDeltaDb = 0.075f;    // normalised travel read as dB
    v = evaluateRouting (wrongUnits);
    CHECK (! v.passed);
    CHECK (! v.faderEncodingConfirmed);
    CHECK (v.faderControlsLoop);                 // the acoustics were fine
    CHECK (! v.failures.empty());

    // And a small disagreement, within the fader's own resolution, is fine.
    auto slightlyOff = healthy();
    slightlyOff.readBackFaderDeltaDb = 5.6f;
    CHECK (evaluateRouting (slightlyOff).passed);

    test::beginTest ("RoutingCheck - an unacknowledged console voids everything");

    auto noAck = healthy();
    noAck.consoleAcknowledged = false;
    v = evaluateRouting (noAck);
    CHECK (! v.passed);
    CHECK (! v.failures.empty());
}

// ---------------------------------------------------------------------------
void testSpeechCatalogue()
{
    test::beginTest ("SpeechCatalogue - host extraction resists disguised URLs");

    CHECK (hostOf ("https://archive.org/download/x/y.mp3") == "archive.org");
    CHECK (hostOf ("https://ia800207.us.archive.org/12/items/x/y.ogg") == "ia800207.us.archive.org");
    CHECK (hostOf ("https://ARCHIVE.ORG/x") == "archive.org");
    CHECK (hostOf ("https://archive.org:443/x") == "archive.org");
    CHECK (hostOf ("not a url").empty());

    // The classic disguise: userinfo before the real host. Naive parsing reads
    // this as archive.org, which is exactly how an allowlist gets walked past.
    CHECK (hostOf ("https://archive.org@evil.example/x") == "evil.example");
    CHECK (! isArchiveHost ("https://archive.org@evil.example/x"));
    CHECK (! isArchiveHost ("https://archive.org.evil.example/x"));
    CHECK (! isArchiveHost ("https://notarchive.org/x"));

    test::beginTest ("SpeechCatalogue - the built-in source is locked to HTTPS on Archive hosts");

    CHECK (isArchiveHost ("https://archive.org/download/x/y.ogg"));
    CHECK (isArchiveHost ("https://ia903103.us.archive.org/9/items/x/y.flac"));
    CHECK (isArchiveHost ("https://librivox.org/api/feed/audiobooks"));
    // Plaintext is refused even on the right host: a redirect to http is a
    // downgrade, and we are downloading files we will decode.
    CHECK (! isArchiveHost ("http://archive.org/download/x/y.ogg"));
    CHECK (! isHttps ("http://archive.org/x"));
    CHECK (isHttps ("https://archive.org/x"));

    test::beginTest ("SpeechCatalogue - derivative preference");

    // FLAC and Ogg beat any MP3; among MP3s, the higher rate wins.
    CHECK_LT (archiveFormatPreference ("Flac"), archiveFormatPreference ("Ogg Vorbis"));
    CHECK_LT (archiveFormatPreference ("Ogg Vorbis"), archiveFormatPreference ("128Kbps MP3"));
    CHECK_LT (archiveFormatPreference ("128Kbps MP3"), archiveFormatPreference ("64Kbps MP3"));
    CHECK_LT (archiveFormatPreference ("VBR MP3"), archiveFormatPreference ("64Kbps MP3"));

    // Everything the Archive also stores alongside the audio must be rejected,
    // or the fetcher downloads cover art and calls it speech.
    CHECK (archiveFormatPreference ("JPEG") < 0);
    CHECK (archiveFormatPreference ("Metadata") < 0);
    CHECK (archiveFormatPreference ("Text") < 0);
    CHECK (archiveFormatPreference ("") < 0);

    test::beginTest ("SpeechCatalogue - choosing a derivative from a real item layout");

    CachePolicy sel;

    // What an Archive LibriVox item actually looks like: several audio
    // derivatives plus cover art, metadata and the Archive's own bookkeeping.
    const std::vector<ArchiveFile> item = {
        { "chapter01.mp3",            "VBR MP3",       9'400'000, 1234.0 },
        { "chapter01_64kb.mp3",       "64Kbps MP3",    4'900'000, 1234.0 },
        { "chapter01_128kb.mp3",      "128Kbps MP3",   9'800'000, 1234.0 },
        { "chapter01.ogg",            "Ogg Vorbis",    7'100'000, 1234.0 },
        { "cover.jpg",                "JPEG",             84'000, 0.0 },
        { "book_meta.xml",            "Metadata",          3'400, 0.0 },
        { "book_files.xml",           "Metadata",          8'100, 0.0 },
        { "__ia_thumb.jpg",           "Item Tile",        12'000, 0.0 },
    };

    const int pick = chooseBestDerivative (item, sel);
    CHECK (pick >= 0);
    CHECK (item[static_cast<size_t> (pick)].name == "chapter01.ogg");   // Ogg beats every MP3

    // With no Ogg present, the best MP3 wins rather than the first one listed.
    std::vector<ArchiveFile> mp3Only;
    for (const auto& f : item)
        if (f.format.find ("Ogg") == std::string::npos)
            mp3Only.push_back (f);

    const int mp3Pick = chooseBestDerivative (mp3Only, sel);
   #if defined(__linux__) && ! defined(FBKT_HAVE_MP3)
    // No MP3 decoder on this platform, so an item with only MP3 derivatives is
    // correctly skipped rather than downloaded and then found unplayable.
    CHECK (mp3Pick < 0);
   #else
    CHECK (mp3Pick >= 0);
    CHECK (mp3Only[static_cast<size_t> (mp3Pick)].format == "VBR MP3");
   #endif

    test::beginTest ("SpeechCatalogue - an item with nothing usable is refused");

    CHECK (chooseBestDerivative ({}, sel) < 0);
    CHECK (chooseBestDerivative ({ { "cover.jpg", "JPEG", 84'000, 0.0 },
                                   { "meta.xml", "Metadata", 3'400, 0.0 } }, sel) < 0);

    // The format field and the extension have to agree. An entry naming an audio
    // format on a file that is plainly not audio is a trap that a format-only
    // check walks straight into.
    CHECK (chooseBestDerivative ({ { "cover.jpg", "Ogg Vorbis", 900'000, 10.0 } }, sel) < 0);
    CHECK (chooseBestDerivative ({ { "chapter.ogg", "JPEG", 900'000, 10.0 } }, sel) < 0);

    test::beginTest ("SpeechCatalogue - absurd sizes are refused");

    // An error page saved as audio.
    CHECK (chooseBestDerivative ({ { "chapter.ogg", "Ogg Vorbis", 2'000, 10.0 } }, sel) < 0);
    // A whole unsplit audiobook.
    CHECK (chooseBestDerivative ({ { "whole.ogg", "Ogg Vorbis", 3'000'000'000LL, 40000.0 } }, sel) < 0);
    // And the one in between is fine.
    CHECK (chooseBestDerivative ({ { "chapter.ogg", "Ogg Vorbis", 7'000'000, 1200.0 } }, sel) >= 0);

    test::beginTest ("SpeechCatalogue - download URLs survive awkward filenames");

    // Archive filenames routinely carry spaces and apostrophes. A URL built by
    // concatenation 404s for exactly those, which would quietly narrow the
    // corpus to the files with tidy names.
    CHECK (archiveDownloadUrl ("mybook_1234", "chapter01.ogg")
           == "https://archive.org/download/mybook_1234/chapter01.ogg");
    CHECK (archiveDownloadUrl ("b", "a file.ogg")
           == "https://archive.org/download/b/a%20file.ogg");
    CHECK (archiveDownloadUrl ("b", "o'brien.mp3")
           == "https://archive.org/download/b/o%27brien.mp3");
    CHECK (archiveDownloadUrl ("b", "caf\xc3\xa9.ogg")
           == "https://archive.org/download/b/caf%C3%A9.ogg");

    // A path separator in a name must not escape the item directory.
    CHECK (archiveDownloadUrl ("b", "../../etc/passwd")
           == "https://archive.org/download/b/..%2F..%2Fetc%2Fpasswd");

    test::beginTest ("SpeechCatalogue - a remote name cannot become a local path");

    // Only the extension of a remote name reaches the filesystem, because cached
    // files are named after their id hash. That is still enough to matter: an
    // extension containing a separator turns a filename into a path.
    CHECK (safeExtensionFor ("chapter01.ogg") == ".ogg");
    CHECK (safeExtensionFor ("Chapter01.OGG") == ".ogg");
    CHECK (safeExtensionFor ("a.flac") == ".flac");
    CHECK (safeExtensionFor ("a.o/b") == ".audio");          // would have been ".o/b"
    CHECK (safeExtensionFor ("a.ogg/../../evil") == ".audio");
    CHECK (safeExtensionFor ("noextension") == ".audio");
    CHECK (safeExtensionFor ("trailingdot.") == ".audio");
    CHECK (safeExtensionFor (".ogg") == ".ogg");             // a bare extension is still one
    CHECK (safeExtensionFor (".hidden") == ".audio");        // six characters: past the limit
    CHECK (safeExtensionFor ("a.verylongextension") == ".audio");
    CHECK (safeExtensionFor ("a.o g") == ".audio");
    CHECK (safeExtensionFor ("") == ".audio");

    CHECK (archiveDownloadUrl ("", "x.ogg").empty());
    CHECK (archiveDownloadUrl ("b", "").empty());

    // And whatever it builds must still satisfy the host lock.
    CHECK (isArchiveHost (archiveDownloadUrl ("mybook", "ch01.ogg")));

    test::beginTest ("SpeechCatalogue - duration parsing, or zero rather than a guess");

    CHECK_CLOSE (parseArchiveLength ("1234.56"), 1234.56, 1e-6);
    CHECK_CLOSE (parseArchiveLength ("20:34"), 1234.0, 1e-6);
    CHECK_CLOSE (parseArchiveLength ("1:20:34"), 4834.0, 1e-6);
    CHECK_CLOSE (parseArchiveLength ("0:07"), 7.0, 1e-6);

    // A wrong duration would corrupt the buffered-hours figure that decides
    // whether an unattended run may start, so anything unparseable is zero.
    CHECK_CLOSE (parseArchiveLength (""), 0.0, 1e-9);
    CHECK_CLOSE (parseArchiveLength ("unknown"), 0.0, 1e-9);
    CHECK_CLOSE (parseArchiveLength ("12:"), 0.0, 1e-9);
    CHECK_CLOSE (parseArchiveLength ("-5"), 0.0, 1e-9);

    test::beginTest ("SpeechCatalogue - ids are stable and distinct");

    const auto a = itemIdForUrl ("https://archive.org/download/book/ch01.ogg");
    const auto b = itemIdForUrl ("https://archive.org/download/book/ch02.ogg");
    CHECK (a == itemIdForUrl ("https://archive.org/download/book/ch01.ogg"));
    CHECK (a != b);
    CHECK (a.size() == 16);

    test::beginTest ("SpeechCatalogue - unknown-licence material is excluded by default");

    auto makeItem = [] (const char* id, LicenceClass licence, long long bytes,
                        double seconds, long long lastPlayed)
    {
        SpeechItem i;
        i.id = id;
        i.licence = licence;
        i.bytes = bytes;
        i.seconds = seconds;
        i.lastPlayedUnix = lastPlayed;
        return i;
    };

    SpeechManifest m;
    CHECK (m.add (makeItem ("aa", LicenceClass::publicDomain, 1000, 600.0, 0)));
    CHECK (m.add (makeItem ("bb", LicenceClass::permissiveAttribution, 2000, 600.0, 0)));
    CHECK (m.add (makeItem ("cc", LicenceClass::unknown, 4000, 600.0, 0)));
    CHECK (! m.add (makeItem ("aa", LicenceClass::publicDomain, 1, 1.0, 0)));   // duplicate

    CachePolicy strict;
    strict.requireOpenLicence = true;
    CHECK (m.playable (strict).size() == 2);
    CHECK_CLOSE (m.playableSeconds (strict), 1200.0, 1e-6);

    CachePolicy loose;
    loose.requireOpenLicence = false;
    CHECK (m.playable (loose).size() == 3);
    CHECK_CLOSE (m.playableSeconds (loose), 1800.0, 1e-6);

    // Total bytes counts everything on disk regardless of licence - the file is
    // occupying the disk whether or not we are allowed to play it.
    CHECK (m.totalBytes() == 7000);

    test::beginTest ("SpeechCatalogue - unattended readiness");

    CachePolicy policy;
    policy.minBufferedSecondsForUnattended = 3600.0;
    CHECK (! m.readyForUnattendedRun (policy));

    SpeechManifest big;
    for (int i = 0; i < 10; ++i)
    {
        char id[8];
        std::snprintf (id, sizeof (id), "i%02d", i);
        big.add (makeItem (id, LicenceClass::publicDomain, 1000, 400.0, 0));
    }
    CHECK (big.readyForUnattendedRun (policy));          // 4000 s > 3600 s

    // Unknown-licence material does not count towards readiness while the open
    // licence requirement is on, so a cache full of podcasts cannot let a
    // distribution run start.
    SpeechManifest podcasts;
    for (int i = 0; i < 10; ++i)
    {
        char id[8];
        std::snprintf (id, sizeof (id), "p%02d", i);
        podcasts.add (makeItem (id, LicenceClass::unknown, 1000, 400.0, 0));
    }
    CHECK (! podcasts.readyForUnattendedRun (policy));
    CachePolicy permissive = policy;
    permissive.requireOpenLicence = false;
    CHECK (podcasts.readyForUnattendedRun (permissive));

    test::beginTest ("SpeechCatalogue - eviction takes the least recently played first");

    SpeechManifest cache;
    cache.add (makeItem ("old",    LicenceClass::publicDomain, 1000, 100.0, 1000));
    cache.add (makeItem ("newer",  LicenceClass::publicDomain, 1000, 100.0, 5000));
    cache.add (makeItem ("newest", LicenceClass::publicDomain, 1000, 100.0, 9000));
    cache.add (makeItem ("unused", LicenceClass::publicDomain, 1000, 100.0, 0));

    auto evict = cache.selectForEviction (1500);
    CHECK (evict.size() == 2);
    CHECK (evict[0] == "old");
    CHECK (evict[1] == "newer");

    // Never-played material is evicted last. It is the only part of the cache
    // that still has something to contribute; discarding it to keep something
    // already used would narrow the corpus every time the disk filled.
    auto evictAll = cache.selectForEviction (100000);
    CHECK (evictAll.size() == 4);
    CHECK (evictAll.back() == "unused");

    test::beginTest ("SpeechCatalogue - what is playing is never evicted");

    auto protectedEvict = cache.selectForEviction (1500, { "old", "newer" });
    CHECK (! protectedEvict.empty());
    for (const auto& id : protectedEvict)
    {
        CHECK (id != "old");
        CHECK (id != "newer");
    }

    CHECK (cache.selectForEviction (0).empty());
    CHECK (cache.selectForEviction (-5).empty());

    test::beginTest ("SpeechCatalogue - headroom and play accounting");

    CachePolicy small;
    small.maxBytes = 4500;
    CHECK (cache.headroomBytes (small) == 500);
    small.maxBytes = 1000;
    CHECK (cache.headroomBytes (small) == 0);          // never negative

    // Playing it stamps it, which moves it out of the never-played group and
    // into the ordinary least-recently-played ordering at that timestamp.
    cache.notePlayed ("unused", 3000);
    const auto* played = cache.find ("unused");
    CHECK (played != nullptr && played->lastPlayedUnix == 3000);
    CHECK (played != nullptr && played->timesPlayed == 1);

    const auto afterPlaying = cache.selectForEviction (100000);
    CHECK (afterPlaying.size() == 4);
    CHECK (afterPlaying[0] == "old");        // 1000
    CHECK (afterPlaying[1] == "unused");     // 3000, no longer protected by being fresh
    CHECK (afterPlaying[2] == "newer");      // 5000
    CHECK (afterPlaying[3] == "newest");     // 9000

    CHECK (cache.remove ("old"));
    CHECK (! cache.remove ("old"));
    CHECK (cache.find ("old") == nullptr);
}

// ---------------------------------------------------------------------------
int main()
{
    testUnits();
    testConfigValidation();
    testWingProtocol();
    testGainRamp();
    testSupervisorBasics();
    testSupervisorAborts();
    testSupervisorWatchdog();
    testSupervisorConsole();
    testSupervisorMicrophone();
    testSupervisorGovernor();
    testSupervisorBurstAndThermal();
    testRoutingCheck();
    testSpeechCatalogue();
    return test::summary();
}
