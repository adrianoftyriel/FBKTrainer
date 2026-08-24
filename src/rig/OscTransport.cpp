#include "OscTransport.h"

namespace fbkt
{
OscTransport::OscTransport() = default;

OscTransport::~OscTransport()
{
    disconnect();
}

bool OscTransport::connect (const juce::String& host, int port, int localPort)
{
    disconnect();

    if (! socket_.bindToPort (localPort))
        return false;

    // Receiver first: a console that answers immediately must not answer into a
    // socket that has no listener attached yet.
    receiver_.addListener (this);
    if (! receiver_.connectToSocket (socket_))
    {
        receiver_.removeListener (this);
        return false;
    }

    if (! sender_.connectToSocket (socket_, host, port))
    {
        receiver_.disconnect();
        receiver_.removeListener (this);
        return false;
    }

    connected_ = true;
    return true;
}

void OscTransport::disconnect()
{
    if (! connected_)
        return;

    sender_.disconnect();
    receiver_.disconnect();
    receiver_.removeListener (this);
    connected_ = false;
}

bool OscTransport::send (const juce::String& address)
{
    return connected_ && sender_.send (juce::OSCMessage (juce::OSCAddressPattern (address)));
}

bool OscTransport::send (const juce::String& address, float value)
{
    return connected_ && sender_.send (juce::OSCMessage (juce::OSCAddressPattern (address), value));
}

bool OscTransport::send (const juce::String& address, int value)
{
    return connected_ && sender_.send (juce::OSCMessage (juce::OSCAddressPattern (address), value));
}

bool OscTransport::sendText (const juce::String& address, const juce::String& value)
{
    return connected_ && sender_.send (juce::OSCMessage (juce::OSCAddressPattern (address), value));
}

void OscTransport::oscMessageReceived (const juce::OSCMessage& message)
{
    const auto now = static_cast<juce::int64> (juce::Time::getMillisecondCounter());

    OscValue v;
    v.receivedAtMs = now;

    if (message.size() > 0)
    {
        const auto& arg = message[0];
        if (arg.isFloat32())     { v.type = OscValue::Type::floating; v.floatValue = arg.getFloat32(); }
        else if (arg.isInt32())  { v.type = OscValue::Type::integer;  v.intValue = arg.getInt32(); }
        else if (arg.isString()) { v.type = OscValue::Type::text;     v.textValue = arg.getString(); }
    }
    // An address with no argument is still an answer: it proves the console
    // recognised the address, which is exactly what discovery needs to know. The
    // default-constructed none type carries that.

    {
        const std::lock_guard<std::mutex> lock (mutex_);
        values_[message.getAddressPattern().toString()] = v;
        lastReplyMs_ = now;
    }

    if (onMessage)
        onMessage (message);
}

void OscTransport::oscBundleReceived (const juce::OSCBundle& bundle)
{
    for (const auto& element : bundle)
    {
        if (element.isMessage())
            oscMessageReceived (element.getMessage());
        else if (element.isBundle())
            oscBundleReceived (element.getBundle());
    }
}

OscValue OscTransport::lastValue (const juce::String& address) const
{
    const std::lock_guard<std::mutex> lock (mutex_);
    const auto it = values_.find (address);
    return it != values_.end() ? it->second : OscValue {};
}

juce::int64 OscTransport::lastReplyMs() const
{
    const std::lock_guard<std::mutex> lock (mutex_);
    return lastReplyMs_;
}

juce::StringArray OscTransport::answeredAddresses() const
{
    const std::lock_guard<std::mutex> lock (mutex_);
    juce::StringArray out;
    for (const auto& pair : values_)
        out.add (pair.first);
    return out;
}

void OscTransport::clearHistory()
{
    const std::lock_guard<std::mutex> lock (mutex_);
    values_.clear();
    lastReplyMs_ = 0;
}
} // namespace fbkt
