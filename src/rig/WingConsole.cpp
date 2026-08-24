#include "WingConsole.h"

#include <cmath>
#include <limits>

namespace fbkt
{
namespace
{
// How long to wait for an answer to a single query before moving on to the next
// candidate. Generous, because a console on a busy wired network still answers in
// single-digit milliseconds and the cost of being wrong here is a discovery pass
// that silently rejects the right address.
constexpr int kQueryTimeoutMs = 400;

// The abort path sends this many times. UDP loses packets; a mute that has to
// arrive gets several chances.
constexpr int kPanicRepeats = 5;
constexpr int kPanicGapMs = 12;

// Keep-alive interval. Sent more often than any console in this family requires,
// which costs one small packet and removes a class of "it stopped updating after
// ten seconds" problem.
constexpr int kKeepAliveIntervalMs = 4000;
} // namespace

WingConsole::WingConsole() = default;

WingConsole::~WingConsole()
{
    disconnect();
}

bool WingConsole::connect (const juce::String& host, int port)
{
    return transport_.connect (host, port);
}

void WingConsole::disconnect()
{
    transport_.disconnect();
}

juce::String WingConsole::addressFor (ConsoleControl control, int channel, int bus) const
{
    const auto& entry = resolved_.forControl (control);
    if (! entry.resolved)
        return {};
    return juce::String (formatAddress (entry.pattern.c_str(), channel, bus));
}

// ---------------------------------------------------------------------------
DiscoveryReport WingConsole::discover (int channel, int sendBus)
{
    DiscoveryReport report;
    sendBus_ = sendBus;

    if (! transport_.isConnected())
    {
        report.summary = "Not connected to the console.";
        report.consoleSilent = true;
        return report;
    }

    transport_.clearHistory();

    const ConsoleControl controls[] = { ConsoleControl::channelFader,
                                        ConsoleControl::channelMute,
                                        ConsoleControl::channelHeadAmpGain,
                                        ConsoleControl::channelSendLevel };

    for (auto control : controls)
    {
        auto& entry = report.addresses.forControl (control);

        for (const auto& candidate : candidatesFor (control))
        {
            const int bus = (control == ConsoleControl::channelSendLevel) ? sendBus : 0;
            const auto address = juce::String (formatAddress (candidate.pattern, channel, bus));

            if (address.isEmpty())
                continue;      // this candidate needs a bus we were not given

            report.tried.add (address);

            // A bare address with no arguments is the query form. Read-only: no
            // value is written, so nothing on the console moves.
            transport_.send (address);

            const auto deadline = juce::Time::getMillisecondCounter() + kQueryTimeoutMs;
            bool answered = false;
            while (juce::Time::getMillisecondCounter() < deadline)
            {
                if (! transport_.lastValue (address).isNone()
                    || transport_.answeredAddresses().contains (address))
                {
                    answered = true;
                    break;
                }
                juce::Thread::sleep (5);
            }

            if (! answered)
                continue;

            report.answered.add (address);
            entry.resolved = true;
            entry.pattern = candidate.pattern;
            entry.encoding = candidate.encoding;
            entry.invertedFlag = candidate.invertedFlag;
            break;             // first candidate that answers wins
        }
    }

    report.consoleSilent = report.answered.isEmpty();
    report.addresses.valid = report.addresses.fader.resolved && report.addresses.mute.resolved;
    report.succeeded = report.addresses.valid;

    juce::StringArray lines;
    if (report.consoleSilent)
    {
        lines.add ("The console did not answer any query.");
        lines.add ("Check the mixer IP, that the computer is on the same wired network, and that OSC is enabled on the console.");
    }
    else
    {
        lines.add ("Answered on " + juce::String (report.answered.size())
                   + " of " + juce::String (report.tried.size()) + " addresses tried.");

        lines.add (report.addresses.fader.resolved
                       ? "  fader: " + juce::String (report.addresses.fader.pattern)
                       : "  fader: NOT FOUND - the rig cannot run without it.");

        lines.add (report.addresses.mute.resolved
                       ? "  mute:  " + juce::String (report.addresses.mute.pattern)
                       : "  mute:  NOT FOUND - the abort path needs it.");

        if (report.addresses.headAmpGain.resolved)
            lines.add ("  head amp: " + juce::String (report.addresses.headAmpGain.pattern));
        if (report.addresses.sendLevel.resolved)
            lines.add ("  send:     " + juce::String (report.addresses.sendLevel.pattern));

        // Say plainly what discovery cannot establish, rather than letting the
        // resolved encoding read as a measured fact.
        lines.add ("");
        lines.add ("Units are a hypothesis at this stage. The routing self test moves the fader");
        lines.add ("by a known amount and checks the read-back agrees, which is what confirms it.");
    }

    report.summary = lines.joinIntoString ("\n");

    if (report.succeeded)
        resolved_ = report.addresses;

    return report;
}

// ---------------------------------------------------------------------------
bool WingConsole::setFaderDb (int channel, float db)
{
    const auto address = addressFor (ConsoleControl::channelFader, channel);
    if (address.isEmpty())
        return false;

    const auto& entry = resolved_.forControl (ConsoleControl::channelFader);
    const float wire = encodeFader (db, entry.encoding);

    if (! transport_.send (address, wire))
        return false;

    // Ask straight back. Every setter is followed by a query because the send
    // above only means the packet left this machine.
    transport_.send (address);
    return true;
}

void WingConsole::queryFader (int channel)
{
    const auto address = addressFor (ConsoleControl::channelFader, channel);
    if (address.isNotEmpty())
        transport_.send (address);
}

float WingConsole::confirmedFaderDb (int channel) const
{
    const auto address = addressFor (ConsoleControl::channelFader, channel);
    if (address.isEmpty())
        return std::numeric_limits<float>::quiet_NaN();

    const auto value = transport_.lastValue (address);
    if (value.isNone())
        return std::numeric_limits<float>::quiet_NaN();

    const auto& entry = resolved_.forControl (ConsoleControl::channelFader);
    return decodeFader (value.asFloat(), entry.encoding);
}

bool WingConsole::setMuted (int channel, bool muted)
{
    const auto address = addressFor (ConsoleControl::channelMute, channel);
    if (address.isEmpty())
        return false;

    const auto& entry = resolved_.forControl (ConsoleControl::channelMute);
    const int flag = muted ? mutedFlagValue (entry.invertedFlag)
                           : unmutedFlagValue (entry.invertedFlag);

    if (! transport_.send (address, flag))
        return false;

    transport_.send (address);
    return true;
}

void WingConsole::queryMute (int channel)
{
    const auto address = addressFor (ConsoleControl::channelMute, channel);
    if (address.isNotEmpty())
        transport_.send (address);
}

bool WingConsole::panic (int channel)
{
    const auto faderAddress = addressFor (ConsoleControl::channelFader, channel);
    const auto muteAddress = addressFor (ConsoleControl::channelMute, channel);

    if (faderAddress.isEmpty() && muteAddress.isEmpty())
        return false;

    const auto& faderEntry = resolved_.forControl (ConsoleControl::channelFader);
    const auto& muteEntry = resolved_.forControl (ConsoleControl::channelMute);

    const float floorWire = encodeFader (kWingFaderMinDb, faderEntry.encoding);
    const int mutedFlag = mutedFlagValue (muteEntry.invertedFlag);

    bool sentAnything = false;
    for (int i = 0; i < kPanicRepeats; ++i)
    {
        // Mute first. It is the faster-acting of the two and does not depend on
        // the fader law having been resolved correctly.
        if (muteAddress.isNotEmpty())
            sentAnything |= transport_.send (muteAddress, mutedFlag);

        if (faderAddress.isNotEmpty())
            sentAnything |= transport_.send (faderAddress, floorWire);

        if (i + 1 < kPanicRepeats)
            juce::Thread::sleep (kPanicGapMs);
    }

    if (faderAddress.isNotEmpty())
        transport_.send (faderAddress);
    if (muteAddress.isNotEmpty())
        transport_.send (muteAddress);

    return sentAnything;
}

void WingConsole::keepAlive()
{
    static thread_local juce::uint32 lastSent = 0;
    const auto now = juce::Time::getMillisecondCounter();
    if (now - lastSent < static_cast<juce::uint32> (kKeepAliveIntervalMs))
        return;

    lastSent = now;
    // Harmless on a console that does not need it, and the reply doubles as the
    // liveness evidence the supervisor watches for.
    transport_.send ("/?");
}
} // namespace fbkt
