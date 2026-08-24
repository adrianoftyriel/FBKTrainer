#include "ConfigStore.h"

namespace fbkt
{
namespace
{
constexpr const char* kFormat = "FBKTrainerRig";
constexpr int kVersion = 1;

void writeLimits (juce::XmlElement& parent, const SafetyLimits& l)
{
    auto* e = parent.createNewChildElement ("limits");
    e->setAttribute ("ceilingSplDb", l.ceilingSplDb);
    e->setAttribute ("warnSplDb", l.warnSplDb);
    e->setAttribute ("maxGainAboveStartDb", l.maxGainAboveStartDb);
    e->setAttribute ("maxStepDb", l.maxStepDb);
    e->setAttribute ("minStepIntervalMs", l.minStepIntervalMs);
    e->setAttribute ("maxBurstMs", l.maxBurstMs);
    e->setAttribute ("minCooldownMs", l.minCooldownMs);
    e->setAttribute ("normalMarginDb", l.normalMarginDb);
    e->setAttribute ("thermalTimeConstantS", l.thermalTimeConstantS);
    e->setAttribute ("thermalBudgetS", l.thermalBudgetS);
    e->setAttribute ("thermalBandLowHz", l.thermalBandLowHz);
    e->setAttribute ("watchdogTimeoutMs", l.watchdogTimeoutMs);
    e->setAttribute ("consoleAckTimeoutMs", l.consoleAckTimeoutMs);
}

void readLimits (const juce::XmlElement& parent, SafetyLimits& l)
{
    const auto* e = parent.getChildByName ("limits");
    if (e == nullptr)
        return;                       // keep the defaults; see the header

    const SafetyLimits d;
    l.ceilingSplDb = static_cast<float> (e->getDoubleAttribute ("ceilingSplDb", d.ceilingSplDb));
    l.warnSplDb = static_cast<float> (e->getDoubleAttribute ("warnSplDb", d.warnSplDb));
    l.maxGainAboveStartDb = static_cast<float> (e->getDoubleAttribute ("maxGainAboveStartDb", d.maxGainAboveStartDb));
    l.maxStepDb = static_cast<float> (e->getDoubleAttribute ("maxStepDb", d.maxStepDb));
    l.minStepIntervalMs = e->getIntAttribute ("minStepIntervalMs", d.minStepIntervalMs);
    l.maxBurstMs = e->getIntAttribute ("maxBurstMs", d.maxBurstMs);
    l.minCooldownMs = e->getIntAttribute ("minCooldownMs", d.minCooldownMs);
    l.normalMarginDb = static_cast<float> (e->getDoubleAttribute ("normalMarginDb", d.normalMarginDb));
    l.thermalTimeConstantS = static_cast<float> (e->getDoubleAttribute ("thermalTimeConstantS", d.thermalTimeConstantS));
    l.thermalBudgetS = static_cast<float> (e->getDoubleAttribute ("thermalBudgetS", d.thermalBudgetS));
    l.thermalBandLowHz = static_cast<float> (e->getDoubleAttribute ("thermalBandLowHz", d.thermalBandLowHz));
    l.watchdogTimeoutMs = e->getIntAttribute ("watchdogTimeoutMs", d.watchdogTimeoutMs);
    l.consoleAckTimeoutMs = e->getIntAttribute ("consoleAckTimeoutMs", d.consoleAckTimeoutMs);
}

void writeEntry (juce::XmlElement& parent, const char* name, const ResolvedAddresses::Entry& e)
{
    auto* x = parent.createNewChildElement (name);
    x->setAttribute ("resolved", e.resolved);
    x->setAttribute ("pattern", juce::String (e.pattern));
    x->setAttribute ("encoding", static_cast<int> (e.encoding));
    x->setAttribute ("inverted", e.invertedFlag);
}

void readEntry (const juce::XmlElement& parent, const char* name, ResolvedAddresses::Entry& e)
{
    if (const auto* x = parent.getChildByName (name))
    {
        e.resolved = x->getBoolAttribute ("resolved", false);
        e.pattern = x->getStringAttribute ("pattern").toStdString();
        e.encoding = static_cast<ValueEncoding> (x->getIntAttribute ("encoding", 0));
        e.invertedFlag = x->getBoolAttribute ("inverted", false);
    }
}
} // namespace

juce::String wiringSignature (const RigConfig& config)
{
    // Everything the self test's verdict depends on. Deliberately includes the
    // audio device and sample rate: the same port index on a different device is
    // a different physical socket.
    juce::StringArray parts;
    parts.add (config.audioDeviceName);
    parts.add (juce::String (config.sampleRate, 0));
    parts.add (juce::String (config.speechOutput.index));
    parts.add (juce::String (config.listeningMicInput.index));
    parts.add (juce::String (config.vocalReturnInput.index));
    parts.add (juce::String (config.vocalChannel.number));
    parts.add (config.consoleAddress);
    parts.add (juce::String (config.consolePort));
    parts.add (juce::String (static_cast<int> (config.gainControl)));
    parts.add (juce::String (config.sendBus));
    return parts.joinIntoString ("|");
}

bool saveRig (const StoredRig& rig, const juce::File& file, juce::String& errorOut)
{
    juce::XmlElement root (kFormat);
    root.setAttribute ("version", kVersion);

    const auto& c = rig.config;
    root.setAttribute ("name", juce::String (c.name));
    root.setAttribute ("console", static_cast<int> (c.console));
    root.setAttribute ("speechOutput", c.speechOutput.index);
    root.setAttribute ("consoleAddress", juce::String (c.consoleAddress));
    root.setAttribute ("consolePort", c.consolePort);
    root.setAttribute ("vocalChannel", c.vocalChannel.number);
    root.setAttribute ("listeningMicInput", c.listeningMicInput.index);
    root.setAttribute ("vocalReturnInput", c.vocalReturnInput.index);
    root.setAttribute ("gainControl", static_cast<int> (c.gainControl));
    root.setAttribute ("sendBus", c.sendBus);
    root.setAttribute ("sampleRate", c.sampleRate);
    root.setAttribute ("audioDeviceName", juce::String (c.audioDeviceName));
    root.setAttribute ("speechFolder", rig.speechFolder);
    root.setAttribute ("speechCacheFolder", rig.speechCacheFolder);
    root.setAttribute ("startFaderDb", rig.startFaderDb);

    auto* speech = root.createNewChildElement ("speech");
    speech->setAttribute ("maxBytes", juce::String (rig.speechPolicy.maxBytes));
    speech->setAttribute ("minBufferedSecondsForUnattended", rig.speechPolicy.minBufferedSecondsForUnattended);
    speech->setAttribute ("targetBufferedSeconds", rig.speechPolicy.targetBufferedSeconds);
    speech->setAttribute ("requireOpenLicence", rig.speechPolicy.requireOpenLicence);
    speech->setAttribute ("minItemBytes", juce::String (rig.speechPolicy.minItemBytes));
    speech->setAttribute ("maxItemBytes", juce::String (rig.speechPolicy.maxItemBytes));

    for (const auto& src : rig.speechSources)
    {
        auto* e = speech->createNewChildElement ("source");
        e->setAttribute ("enabled", src.enabled);
        e->setAttribute ("kind", static_cast<int> (src.kind));
        e->setAttribute ("name", src.name);
        e->setAttribute ("feedUrl", src.feedUrl);
        e->setAttribute ("licence", static_cast<int> (src.licence));
    }

    auto* cal = root.createNewChildElement ("micCalibration");
    cal->setAttribute ("valid", c.micCalibration.valid);
    cal->setAttribute ("splAtFullScaleDb", c.micCalibration.splAtFullScaleDb);
    cal->setAttribute ("referenceSplDb", c.micCalibration.referenceSplDb);
    cal->setAttribute ("measuredDbFS", c.micCalibration.measuredDbFS);

    writeLimits (root, c.limits);

    auto* addr = root.createNewChildElement ("addresses");
    addr->setAttribute ("valid", rig.addresses.valid);
    addr->setAttribute ("firmwareNote", juce::String (rig.addresses.firmwareNote));
    writeEntry (*addr, "fader", rig.addresses.fader);
    writeEntry (*addr, "mute", rig.addresses.mute);
    writeEntry (*addr, "headAmpGain", rig.addresses.headAmpGain);
    writeEntry (*addr, "sendLevel", rig.addresses.sendLevel);

    auto* selfTest = root.createNewChildElement ("selfTest");
    selfTest->setAttribute ("passed", rig.selfTestPassed);
    selfTest->setAttribute ("signature", rig.selfTestSignature);

    if (! file.getParentDirectory().createDirectory())
    {
        errorOut = "Could not create " + file.getParentDirectory().getFullPathName();
        return false;
    }

    if (! root.writeTo (file))
    {
        errorOut = "Could not write " + file.getFullPathName();
        return false;
    }
    return true;
}

bool loadRig (StoredRig& rigOut, const juce::File& file, juce::String& errorOut)
{
    if (! file.existsAsFile())
    {
        errorOut = "No rig file at " + file.getFullPathName();
        return false;
    }

    const auto root = juce::XmlDocument::parse (file);
    if (root == nullptr || ! root->hasTagName (kFormat))
    {
        errorOut = file.getFileName() + " is not an FBKTrainer rig file.";
        return false;
    }

    StoredRig rig;
    auto& c = rig.config;

    c.name = root->getStringAttribute ("name", "Untitled rig").toStdString();
    c.console = static_cast<ConsoleModel> (root->getIntAttribute ("console", 0));
    c.speechOutput.index = root->getIntAttribute ("speechOutput", -1);
    c.consoleAddress = root->getStringAttribute ("consoleAddress").toStdString();
    c.consolePort = root->getIntAttribute ("consolePort", kWingDefaultOscPort);
    c.vocalChannel.number = root->getIntAttribute ("vocalChannel", 0);
    c.listeningMicInput.index = root->getIntAttribute ("listeningMicInput", -1);
    c.vocalReturnInput.index = root->getIntAttribute ("vocalReturnInput", -1);
    c.gainControl = static_cast<RigConfig::GainControl> (root->getIntAttribute ("gainControl", 0));
    c.sendBus = root->getIntAttribute ("sendBus", 0);
    c.sampleRate = root->getDoubleAttribute ("sampleRate", 48000.0);
    c.audioDeviceName = root->getStringAttribute ("audioDeviceName").toStdString();
    rig.speechFolder = root->getStringAttribute ("speechFolder");
    rig.speechCacheFolder = root->getStringAttribute ("speechCacheFolder");
    rig.startFaderDb = static_cast<float> (root->getDoubleAttribute ("startFaderDb", -20.0));

    if (const auto* speech = root->getChildByName ("speech"))
    {
        const CachePolicy d;
        rig.speechPolicy.maxBytes = speech->getStringAttribute ("maxBytes", juce::String (d.maxBytes)).getLargeIntValue();
        rig.speechPolicy.minBufferedSecondsForUnattended =
            speech->getDoubleAttribute ("minBufferedSecondsForUnattended", d.minBufferedSecondsForUnattended);
        rig.speechPolicy.targetBufferedSeconds =
            speech->getDoubleAttribute ("targetBufferedSeconds", d.targetBufferedSeconds);
        // Defaults to true when the attribute is absent, so a file written by an
        // older build does not quietly let unknown-licence material into the
        // corpus behind a model somebody intends to distribute.
        rig.speechPolicy.requireOpenLicence = speech->getBoolAttribute ("requireOpenLicence", true);
        rig.speechPolicy.minItemBytes = speech->getStringAttribute ("minItemBytes", juce::String (d.minItemBytes)).getLargeIntValue();
        rig.speechPolicy.maxItemBytes = speech->getStringAttribute ("maxItemBytes", juce::String (d.maxItemBytes)).getLargeIntValue();

        for (auto* e : speech->getChildWithTagNameIterator ("source"))
        {
            SpeechSource src;
            src.enabled = e->getBoolAttribute ("enabled", true);
            src.kind = static_cast<SpeechSourceKind> (e->getIntAttribute ("kind", 0));
            src.name = e->getStringAttribute ("name");
            src.feedUrl = e->getStringAttribute ("feedUrl");
            src.licence = static_cast<LicenceClass> (e->getIntAttribute ("licence", 2));
            rig.speechSources.add (src);
        }
    }

    if (rig.speechSources.isEmpty())
        rig.speechSources = defaultSpeechSources();

    if (rig.speechCacheFolder.isEmpty())
        rig.speechCacheFolder = defaultSpeechCacheFolder().getFullPathName();

    if (const auto* cal = root->getChildByName ("micCalibration"))
    {
        c.micCalibration.valid = cal->getBoolAttribute ("valid", false);
        c.micCalibration.splAtFullScaleDb = static_cast<float> (cal->getDoubleAttribute ("splAtFullScaleDb", 0.0));
        c.micCalibration.referenceSplDb = static_cast<float> (cal->getDoubleAttribute ("referenceSplDb", 94.0));
        c.micCalibration.measuredDbFS = static_cast<float> (cal->getDoubleAttribute ("measuredDbFS", 0.0));
    }

    readLimits (*root, c.limits);

    if (const auto* addr = root->getChildByName ("addresses"))
    {
        rig.addresses.valid = addr->getBoolAttribute ("valid", false);
        rig.addresses.firmwareNote = addr->getStringAttribute ("firmwareNote").toStdString();
        readEntry (*addr, "fader", rig.addresses.fader);
        readEntry (*addr, "mute", rig.addresses.mute);
        readEntry (*addr, "headAmpGain", rig.addresses.headAmpGain);
        readEntry (*addr, "sendLevel", rig.addresses.sendLevel);
    }

    if (const auto* selfTest = root->getChildByName ("selfTest"))
    {
        rig.selfTestPassed = selfTest->getBoolAttribute ("passed", false);
        rig.selfTestSignature = selfTest->getStringAttribute ("signature");
    }

    // A stored pass that refers to a different wiring is not a pass. Dropping it
    // here rather than at the point of use means there is no path that can
    // consult it without this check having run.
    if (rig.selfTestPassed && rig.selfTestSignature != wiringSignature (c))
    {
        rig.selfTestPassed = false;
        rig.selfTestSignature = {};
    }

    rigOut = rig;
    return true;
}

juce::File defaultRigFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("FBKTrainer")
        .getChildFile ("rig.xml");
}

juce::File defaultSpeechCacheFolder()
{
    // Not next to the rig file: this grows to gigabytes, and the roaming
    // application data directory is the wrong place for that on Windows.
    return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
        .getChildFile ("FBKTrainer")
        .getChildFile ("speech-cache");
}

juce::Array<SpeechSource> defaultSpeechSources()
{
    SpeechSource librivox;
    librivox.enabled = true;
    librivox.kind = SpeechSourceKind::librivox;
    librivox.name = "LibriVox (public domain)";
    librivox.licence = LicenceClass::publicDomain;

    juce::Array<SpeechSource> sources;
    sources.add (librivox);
    return sources;
}
} // namespace fbkt
