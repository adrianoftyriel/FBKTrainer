// FBKTrainer - ConfigStore.h
//
// Reads and writes a rig to disk.
//
// Safety limits are written out with everything else and read back with
// everything else, but a file that is missing them - because it was written by an
// older build, or edited by hand - gets the defaults rather than zeros. A limit
// that silently becomes zero is a limit that is either impossible to satisfy or
// trivially satisfied, and which of those it is depends on which way round the
// comparison happens to be written.
//
// The resolved console addresses are stored too. Discovery is a setup-time act
// and it queries a live console; making it a startup act would mean every launch
// probing a PA that may not be powered.
#pragma once

#include "RigConfig.h"
#include "SpeechFetcher.h"
#include "WingProtocol.h"

#include <JuceHeader.h>

namespace fbkt
{
struct StoredRig
{
    RigConfig config;
    ResolvedAddresses addresses;

    // Speech material. The folder is optional and additional: the rig fetches its
    // own corpus, and a local folder is for the material no public corpus
    // contains - sung notes, sustained vowels, your own difficult voice.
    juce::String speechFolder;
    juce::String speechCacheFolder;
    CachePolicy  speechPolicy;
    juce::Array<SpeechSource> speechSources;

    float startFaderDb { -20.0f };

    // Whether the routing self test has passed for this rig, and against which
    // configuration. Cleared whenever anything that could change the wiring
    // changes, because a pass that refers to a different configuration is worse
    // than no pass at all.
    bool selfTestPassed { false };
    juce::String selfTestSignature;
};

// A short string summarising every field the routing self test depends on. If it
// changes, a previous pass no longer applies.
juce::String wiringSignature (const RigConfig& config);

bool saveRig (const StoredRig& rig, const juce::File& file, juce::String& errorOut);
bool loadRig (StoredRig& rigOut, const juce::File& file, juce::String& errorOut);

juce::File defaultRigFile();
juce::File defaultSpeechCacheFolder();

// The sources a rig starts with: LibriVox, and nothing else. Public domain, tens
// of thousands of hours, thousands of readers, and no account needed.
juce::Array<SpeechSource> defaultSpeechSources();
} // namespace fbkt
