// FBKTrainer - OscTransport.h
//
// OSC over UDP to the console, with one detail that matters more than the rest:
// the sender and the receiver share a single socket.
//
// Consoles in this family reply to the source port of the packet that asked. A
// JUCE OSCSender opens its own ephemeral socket, so if the receiver is bound to
// some other port, queries go out and answers arrive at a port nobody is
// listening on. The symptom is a console that appears to be unreachable while
// happily answering every query - and since this program treats an unanswered
// console as a reason to abort, that would make the rig unrunnable for a reason
// that never appears in any log.
//
// So both ends are connected to one DatagramSocket, and read-back works.
//
// Everything here is thread-safe. Replies arrive on the OSC receive thread and
// are stored under a lock; the run loop reads them from its own thread. The
// alternative - dispatching to the message thread - would make console
// acknowledgement depend on UI responsiveness, and a stalled UI thread is one of
// the exact conditions the watchdog exists to survive.
#pragma once

#include <JuceHeader.h>

#include <functional>
#include <map>
#include <mutex>

namespace fbkt
{
// One value read back from the console. Kept as a variant of the two things a
// console actually sends for the controls we use.
struct OscValue
{
    enum class Type { none, floating, integer, text };
    Type type { Type::none };

    float        floatValue { 0.0f };
    int          intValue { 0 };
    juce::String textValue;

    juce::int64 receivedAtMs { 0 };

    bool isNone() const noexcept { return type == Type::none; }

    // Consoles are inconsistent about whether a level arrives as a float or an
    // int, so asking for "the number" is the useful operation.
    float asFloat() const noexcept
    {
        switch (type)
        {
            case Type::floating: return floatValue;
            case Type::integer:  return static_cast<float> (intValue);
            case Type::text:     return 0.0f;
            case Type::none:     return 0.0f;
        }
        return 0.0f;
    }
};

class OscTransport : private juce::OSCReceiver::Listener<juce::OSCReceiver::RealtimeCallback>
{
public:
    OscTransport();
    ~OscTransport() override;

    // localPort of 0 lets the OS choose, which is normally right: the console
    // replies to wherever the query came from, so the port does not need to be
    // predictable.
    bool connect (const juce::String& host, int port, int localPort = 0);
    void disconnect();
    bool isConnected() const noexcept { return connected_; }

    // Fire and forget. Returns false only if the packet could not be handed to
    // the socket at all - which is not the same as the console receiving it, and
    // is why every setter in WingConsole is followed by a query.
    bool send (const juce::String& address);
    bool send (const juce::String& address, float value);
    bool send (const juce::String& address, int value);
    bool sendText (const juce::String& address, const juce::String& value);

    // The most recent value seen for an address, or a none-typed value if the
    // console has never answered for it.
    OscValue lastValue (const juce::String& address) const;

    // When we last heard anything at all from the console. The supervisor treats
    // a long silence as a reason to stop, so this is a safety-relevant number
    // rather than a diagnostic.
    juce::int64 lastReplyMs() const;

    // Every address the console has ever answered on, for the discovery report.
    juce::StringArray answeredAddresses() const;

    void clearHistory();

    // Called for every inbound message, on the receive thread. Used by discovery
    // to watch for answers it has not asked for by address.
    std::function<void (const juce::OSCMessage&)> onMessage;

private:
    void oscMessageReceived (const juce::OSCMessage&) override;
    void oscBundleReceived (const juce::OSCBundle&) override;

    juce::DatagramSocket socket_ { true };
    juce::OSCSender      sender_;
    juce::OSCReceiver    receiver_;

    mutable std::mutex mutex_;
    std::map<juce::String, OscValue> values_;
    juce::int64 lastReplyMs_ { 0 };

    bool connected_ { false };
};
} // namespace fbkt
