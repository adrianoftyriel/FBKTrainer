// FBKTrainer - SpeechFetcher.h
//
// Keeps a local corpus of speech topped up from the Internet, so the rig has
// something to say for as long as a run lasts without anyone feeding it files.
//
// The rule that shapes everything here
// ------------------------------------
// The audio path never waits for the network. The fetcher downloads into a cache
// on its own thread; the player only ever sees files that are already on disk and
// have already been proved to decode. A network that disappears halfway through a
// three-day run is therefore a fetcher that stops making progress, not a run that
// stops - and the run keeps playing whatever is already banked.
//
// That is also why an unattended run is gated on hours-of-material-banked rather
// than on the network being up when the button is pressed.
//
// Proved by decoding, not by trusting
// -----------------------------------
// Nothing enters the manifest because a server said it was audio. Every file is
// opened with an AudioFormatReader first, and its duration is taken from the
// decoded stream rather than from the metadata that described it. A truncated
// download, an error page served with an audio content type, or a format this
// platform cannot decode all fail the same way: the file is discarded and never
// reaches a playlist. It also means the banked-hours figure is measured rather
// than claimed, which matters because that figure is what decides whether a run
// nobody will be watching may start.
#pragma once

#include "SpeechCatalogue.h"

#include <JuceHeader.h>

#include <atomic>
#include <functional>

namespace fbkt
{
struct SpeechSource
{
    bool enabled { true };
    SpeechSourceKind kind { SpeechSourceKind::librivox };
    juce::String name;
    juce::String feedUrl;        // podcastFeed only
    LicenceClass licence { LicenceClass::publicDomain };
};

struct FetcherStatus
{
    bool running { false };
    juce::String step;
    juce::String lastError;

    int    itemCount { 0 };
    long long cacheBytes { 0 };
    double playableSeconds { 0.0 };
    double targetSeconds { 0.0 };
    bool   readyForUnattended { false };

    int    downloadedThisSession { 0 };
    int    rejectedThisSession { 0 };
};

class SpeechFetcher final : public juce::Thread
{
public:
    SpeechFetcher();
    ~SpeechFetcher() override;

    void setCacheDirectory (const juce::File&);
    juce::File cacheDirectory() const;

    void setPolicy (const CachePolicy&);
    CachePolicy policy() const;

    void setSources (const juce::Array<SpeechSource>&);
    juce::Array<SpeechSource> sources() const;

    // Loads the manifest and reconciles it with what is actually on disk. Cheap,
    // and safe to call before starting.
    void loadLibrary();
    void saveLibrary();

    void startFetching();
    void stopFetching();

    FetcherStatus status() const;
    SpeechManifest manifest() const;

    // Files the policy permits to be played, in a shuffled order so a run does
    // not work through one reader's entire back catalogue before hearing anyone
    // else. Only ever returns files that exist and have been decoded once.
    juce::Array<juce::File> playlist() const;

    // Records that a file was played, which feeds the eviction ordering.
    void notePlayed (const juce::File&);

    std::function<void()> onLibraryChanged;

    void run() override;

private:
    struct Candidate
    {
        juce::String identifier;   // Archive item, or a direct URL for a feed
        juce::String directUrl;
        juce::String title;
        juce::String pageUrl;
        SpeechSourceKind kind { SpeechSourceKind::librivox };
        LicenceClass licence { LicenceClass::publicDomain };
        juce::String sourceName;
    };

    void setStep (const juce::String&);
    void setError (const juce::String&);

    bool topUpCandidates();
    bool discoverArchiveItems (const SpeechSource&);
    bool discoverFeedItems (const SpeechSource&);

    // Resolves one Archive item into the single best derivative worth fetching.
    bool resolveArchiveItem (const Candidate&, SpeechItem& itemOut, juce::String& urlOut);

    bool fetchOne (const Candidate&);
    bool download (const juce::String& url, const juce::File& destination, long long maxBytes);

    // Opens the file and returns its real duration, or 0 if it will not decode.
    double validateAndMeasure (const juce::File&);

    void enforceCacheBudget();
    void reconcileWithDisk();
    juce::File manifestFile() const;

    juce::AudioFormatManager formats_;

    mutable juce::CriticalSection lock_;
    juce::File cacheDir_;
    CachePolicy policy_;
    juce::Array<SpeechSource> sources_;
    SpeechManifest manifest_;
    juce::Array<Candidate> candidates_;
    juce::StringArray seenIdentifiers_;

    mutable juce::CriticalSection statusLock_;
    FetcherStatus status_;

    juce::Random random_;
    std::atomic<bool> dirty_ { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (SpeechFetcher)
};
} // namespace fbkt
