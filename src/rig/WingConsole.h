// FBKTrainer - WingConsole.h
//
// The console as this program needs it: a fader that can be set and read back, a
// mute that can be trusted, and an honest answer to "are you still there".
//
// Discovery is read-only
// ----------------------
// Working out which OSC addresses this console answers on is done entirely with
// queries. Nothing is written. That is not a stylistic preference: discovery runs
// against a live PA with a microphone in front of it, often before the operator
// has set a working level, and a discovery routine that probed by writing values
// would be a routine that could put a fader somewhere unexpected while nobody was
// ready for it.
//
// The consequence is that discovery can identify which addresses exist but cannot
// by itself prove what units they are in - a fader at -10 dB reads as "-10.0" in
// one encoding and "0.5" in the other, and both are plausible numbers. That
// ambiguity is resolved later, in the routing self test, which moves the fader by
// a known amount and checks that the read-back moves by the same amount. A wrong
// encoding shows up there as a delta that is nothing like the one commanded.
//
// Panic is repeated
// -----------------
// OSC is UDP and a dropped packet is normal. Everywhere else that is handled by
// confirming before proceeding, but the abort path has nothing to proceed to and
// cannot afford to wait: it sends the mute and the fader floor several times
// over. Sending a mute five times costs nothing; sending it once and losing it
// costs a driver.
#pragma once

#include "OscTransport.h"
#include "RigConfig.h"
#include "WingProtocol.h"

#include <JuceHeader.h>

namespace fbkt
{
struct DiscoveryReport
{
    bool succeeded { false };
    ResolvedAddresses addresses;

    juce::StringArray tried;        // every address queried, in order
    juce::StringArray answered;     // those the console replied to
    juce::String summary;

    // Set when the console answered nothing at all, which is a different problem
    // from answering on unexpected addresses: the first is a network or address
    // fault, the second is a firmware difference.
    bool consoleSilent { false };
};

class WingConsole
{
public:
    WingConsole();
    ~WingConsole();

    bool connect (const juce::String& host, int port);
    void disconnect();
    bool isConnected() const noexcept { return transport_.isConnected(); }

    // Resolves which addresses this console answers on, for the given channel.
    // Blocking, and intended to be called from a background thread: it waits for
    // replies that may never come. Read-only throughout.
    DiscoveryReport discover (int channel, int sendBus = 0);

    void setResolved (const ResolvedAddresses& r) { resolved_ = r; }
    const ResolvedAddresses& resolved() const noexcept { return resolved_; }

    // --- control -----------------------------------------------------------
    bool setFaderDb (int channel, float db);
    void queryFader (int channel);

    // The last fader position the console reported, in dB, or NaN if it has
    // never reported one. NaN rather than a plausible default on purpose: a
    // caller that treats "unknown" as "0 dB" would let the ramp believe it was
    // somewhere it has never been told it is.
    float confirmedFaderDb (int channel) const;

    bool setMuted (int channel, bool muted);
    void queryMute (int channel);

    // Fader to the floor and the channel muted, sent repeatedly. Returns false
    // only if nothing could be sent at all.
    bool panic (int channel);

    // Consoles in this family stop sending updates unless periodically asked to
    // keep going. Called from the run loop; harmless if the console does not
    // need it.
    void keepAlive();

    juce::int64 lastReplyMs() const { return transport_.lastReplyMs(); }
    bool sawAnyReply() const { return transport_.lastReplyMs() > 0; }

private:
    juce::String addressFor (ConsoleControl control, int channel, int bus = 0) const;

    OscTransport      transport_;
    ResolvedAddresses resolved_;
    int               sendBus_ { 0 };
};
} // namespace fbkt
