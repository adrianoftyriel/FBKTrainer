#include "SpeechFetcher.h"

namespace fbkt
{
namespace
{
constexpr int kConnectTimeoutMs = 15000;
constexpr int kRedirectsToFollow = 5;

// How many Archive items to pull per search page, and how many candidates to
// keep queued. Small, because each one costs a metadata request and there is no
// hurry: the fetcher is topping up a cache measured in hours.
constexpr int kSearchRows = 50;
constexpr int kMaxQueuedCandidates = 200;

// The Archive's LibriVox collection. Everything in it is a recording of a
// public-domain text, released by its reader to the public domain.
constexpr const char* kLibriVoxCollection = "librivoxaudio";

// Pause between fetches, so an unattended run does not hammer a free service for
// days. The cache only needs to gain material faster than the run consumes it,
// and the run consumes it in real time.
constexpr int kBetweenFetchesMs = 2000;
constexpr int kIdlePollMs = 30000;

juce::String archiveSearchUrl (int page)
{
    return juce::String ("https://archive.org/advancedsearch.php?q=")
           + juce::URL::addEscapeChars ("collection:\"" + juce::String (kLibriVoxCollection) + "\"", false)
           + "&fl%5B%5D=identifier&fl%5B%5D=title&rows=" + juce::String (kSearchRows)
           + "&page=" + juce::String (page)
           + "&output=json";
}

std::unique_ptr<juce::InputStream> openUrl (const juce::String& url)
{
    // Refuse anything that is not HTTPS before opening it, rather than after.
    if (! isHttps (url.toStdString()))
        return nullptr;

    return juce::URL (url).createInputStream (
        juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
            .withConnectionTimeoutMs (kConnectTimeoutMs)
            .withNumRedirectsToFollow (kRedirectsToFollow));
}

juce::String readWholeUrl (const juce::String& url, int maxBytes = 4 * 1024 * 1024)
{
    auto stream = openUrl (url);
    if (stream == nullptr)
        return {};

    juce::MemoryOutputStream out;
    out.writeFromInputStream (*stream, maxBytes);
    return out.toString();
}

// Everything the Archive stores that is not the audio we want.
bool looksLikeAudioName (const juce::String& name)
{
    const auto lower = name.toLowerCase();
    return lower.endsWith (".mp3") || lower.endsWith (".ogg") || lower.endsWith (".flac")
           || lower.endsWith (".wav") || lower.endsWith (".aiff") || lower.endsWith (".aif")
           || lower.endsWith (".m4a") || lower.endsWith (".opus");
}

} // namespace

// ---------------------------------------------------------------------------
SpeechFetcher::SpeechFetcher() : juce::Thread ("FBKTrainer speech fetcher")
{
    formats_.registerBasicFormats();
}

SpeechFetcher::~SpeechFetcher()
{
    stopFetching();
}

void SpeechFetcher::setCacheDirectory (const juce::File& dir)
{
    const juce::ScopedLock sl (lock_);
    cacheDir_ = dir;
}

juce::File SpeechFetcher::cacheDirectory() const
{
    const juce::ScopedLock sl (lock_);
    return cacheDir_;
}

void SpeechFetcher::setPolicy (const CachePolicy& p)
{
    const juce::ScopedLock sl (lock_);
    policy_ = p;
}

CachePolicy SpeechFetcher::policy() const
{
    const juce::ScopedLock sl (lock_);
    return policy_;
}

void SpeechFetcher::setSources (const juce::Array<SpeechSource>& s)
{
    const juce::ScopedLock sl (lock_);
    sources_ = s;
}

juce::Array<SpeechSource> SpeechFetcher::sources() const
{
    const juce::ScopedLock sl (lock_);
    return sources_;
}

SpeechManifest SpeechFetcher::manifest() const
{
    const juce::ScopedLock sl (lock_);
    return manifest_;
}

juce::File SpeechFetcher::manifestFile() const
{
    return cacheDirectory().getChildFile ("library.xml");
}

void SpeechFetcher::setStep (const juce::String& s)
{
    const juce::ScopedLock sl (statusLock_);
    status_.step = s;
}

void SpeechFetcher::setError (const juce::String& s)
{
    const juce::ScopedLock sl (statusLock_);
    status_.lastError = s;
}

FetcherStatus SpeechFetcher::status() const
{
    FetcherStatus s;
    {
        const juce::ScopedLock sl (statusLock_);
        s = status_;
    }

    const juce::ScopedLock sl (lock_);
    s.running = isThreadRunning();
    s.itemCount = static_cast<int> (manifest_.items().size());
    s.cacheBytes = manifest_.totalBytes();
    s.playableSeconds = manifest_.playableSeconds (policy_);
    s.targetSeconds = policy_.targetBufferedSeconds;
    s.readyForUnattended = manifest_.readyForUnattendedRun (policy_);
    return s;
}

// ---------------------------------------------------------------------------
void SpeechFetcher::loadLibrary()
{
    const auto file = manifestFile();
    SpeechManifest loaded;

    if (auto xml = juce::XmlDocument::parse (file))
    {
        if (xml->hasTagName ("FBKTrainerSpeechLibrary"))
        {
            for (auto* e : xml->getChildWithTagNameIterator ("item"))
            {
                SpeechItem i;
                i.id = e->getStringAttribute ("id").toStdString();
                i.sourceName = e->getStringAttribute ("source").toStdString();
                i.kind = static_cast<SpeechSourceKind> (e->getIntAttribute ("kind", 0));
                i.licence = static_cast<LicenceClass> (e->getIntAttribute ("licence", 2));
                i.title = e->getStringAttribute ("title").toStdString();
                i.pageUrl = e->getStringAttribute ("pageUrl").toStdString();
                i.mediaUrl = e->getStringAttribute ("mediaUrl").toStdString();
                i.fileName = e->getStringAttribute ("file").toStdString();
                i.bytes = e->getStringAttribute ("bytes").getLargeIntValue();
                i.seconds = e->getDoubleAttribute ("seconds", 0.0);
                i.fetchedAtUnix = e->getStringAttribute ("fetchedAt").getLargeIntValue();
                i.lastPlayedUnix = e->getStringAttribute ("lastPlayed").getLargeIntValue();
                i.timesPlayed = e->getIntAttribute ("timesPlayed", 0);

                if (! i.id.empty())
                    loaded.add (i);
            }
        }
    }

    {
        const juce::ScopedLock sl (lock_);
        manifest_ = loaded;
    }

    reconcileWithDisk();
}

void SpeechFetcher::saveLibrary()
{
    juce::XmlElement root ("FBKTrainerSpeechLibrary");
    root.setAttribute ("version", 1);

    {
        const juce::ScopedLock sl (lock_);
        for (const auto& i : manifest_.items())
        {
            auto* e = root.createNewChildElement ("item");
            e->setAttribute ("id", juce::String (i.id));
            e->setAttribute ("source", juce::String (i.sourceName));
            e->setAttribute ("kind", static_cast<int> (i.kind));
            e->setAttribute ("licence", static_cast<int> (i.licence));
            e->setAttribute ("licenceName", juce::String (toString (i.licence)));
            e->setAttribute ("title", juce::String (i.title));
            e->setAttribute ("pageUrl", juce::String (i.pageUrl));
            e->setAttribute ("mediaUrl", juce::String (i.mediaUrl));
            e->setAttribute ("file", juce::String (i.fileName));
            e->setAttribute ("bytes", juce::String (i.bytes));
            e->setAttribute ("seconds", i.seconds);
            e->setAttribute ("fetchedAt", juce::String (i.fetchedAtUnix));
            e->setAttribute ("lastPlayed", juce::String (i.lastPlayedUnix));
            e->setAttribute ("timesPlayed", i.timesPlayed);
        }
    }

    const auto file = manifestFile();
    file.getParentDirectory().createDirectory();
    root.writeTo (file);
}

void SpeechFetcher::reconcileWithDisk()
{
    const auto dir = cacheDirectory();
    if (! dir.isDirectory())
        return;

    // Anything left in the staging directory is the remains of a download that
    // was interrupted - a crash, a kill, a machine that lost power mid-run. None
    // of it was ever validated, so none of it is worth keeping.
    const auto staging = dir.getChildFile ("incoming");
    if (staging.isDirectory())
        staging.deleteRecursively();

    // A manifest entry whose file has gone - a cleared cache, a half-copied
    // directory - is worse than no entry, because it inflates the banked-hours
    // figure that gates an unattended run.
    juce::StringArray missing;
    juce::StringArray known;

    {
        const juce::ScopedLock sl (lock_);
        for (const auto& i : manifest_.items())
        {
            const auto name = juce::String (i.fileName);
            known.add (name);
            if (! dir.getChildFile (name).existsAsFile())
                missing.add (juce::String (i.id));
        }
    }

    // And a file on disk that no entry claims is the mirror problem: it occupies
    // the disk without being counted against the budget, so the cache quietly
    // grows past its limit and nothing ever evicts it.
    int orphansRemoved = 0;
    for (const auto& entry : juce::RangedDirectoryIterator (dir, false, "*", juce::File::findFiles))
    {
        const auto file = entry.getFile();
        const auto name = file.getFileName();

        if (name == "library.xml" || known.contains (name))
            continue;

        if (file.deleteFile())
            ++orphansRemoved;
    }

    if (missing.isEmpty() && orphansRemoved == 0)
        return;

    if (! missing.isEmpty())
    {
        const juce::ScopedLock sl (lock_);
        for (const auto& id : missing)
            manifest_.remove (id.toStdString());
    }

    saveLibrary();
    dirty_ = true;
}

// ---------------------------------------------------------------------------
void SpeechFetcher::startFetching()
{
    if (isThreadRunning())
        return;

    {
        const juce::ScopedLock sl (statusLock_);
        status_.downloadedThisSession = 0;
        status_.rejectedThisSession = 0;
        status_.lastError = {};
    }

    startThread();
}

void SpeechFetcher::stopFetching()
{
    signalThreadShouldExit();
    stopThread (8000);
}

// ---------------------------------------------------------------------------
void SpeechFetcher::run()
{
    cacheDirectory().createDirectory();
    loadLibrary();

    while (! threadShouldExit())
    {
        CachePolicy policy;
        {
            const juce::ScopedLock sl (lock_);
            policy = policy_;
        }

        const double banked = manifest().playableSeconds (policy);

        if (banked >= policy.targetBufferedSeconds)
        {
            setStep ("Cache is full enough - "
                     + juce::String (banked / 3600.0, 1) + " hours banked");
            wait (kIdlePollMs);
            continue;
        }

        if (! topUpCandidates())
        {
            setStep ("No candidates available - waiting");
            wait (kIdlePollMs);
            continue;
        }

        Candidate next;
        {
            const juce::ScopedLock sl (lock_);
            if (candidates_.isEmpty())
                continue;
            next = candidates_.removeAndReturn (0);
        }

        if (threadShouldExit())
            break;

        enforceCacheBudget();

        if (fetchOne (next))
        {
            saveLibrary();
            dirty_ = true;

            if (onLibraryChanged)
            {
                auto callback = onLibraryChanged;
                juce::MessageManager::callAsync ([callback] { callback(); });
            }
        }

        wait (kBetweenFetchesMs);
    }

    setStep ("Stopped");
}

// ---------------------------------------------------------------------------
bool SpeechFetcher::topUpCandidates()
{
    {
        const juce::ScopedLock sl (lock_);
        if (! candidates_.isEmpty())
            return true;
    }

    const auto sourceList = sources();
    bool any = false;

    for (const auto& source : sourceList)
    {
        if (! source.enabled || threadShouldExit())
            continue;

        if (source.kind == SpeechSourceKind::librivox)
            any |= discoverArchiveItems (source);
        else if (source.kind == SpeechSourceKind::podcastFeed)
            any |= discoverFeedItems (source);
    }

    const juce::ScopedLock sl (lock_);
    return any && ! candidates_.isEmpty();
}

bool SpeechFetcher::discoverArchiveItems (const SpeechSource& source)
{
    // The collection holds tens of thousands of items. Starting at a random page
    // each time is what stops every run of the program working through the same
    // few readers in the same order - which would make a corpus that is large
    // and narrow at the same time.
    const int page = 1 + random_.nextInt (400);

    setStep ("Looking for recordings (page " + juce::String (page) + ")");

    const auto json = readWholeUrl (archiveSearchUrl (page));
    if (json.isEmpty())
    {
        setError ("Could not reach archive.org. The rig will keep playing what is already cached.");
        return false;
    }

    const auto parsed = juce::JSON::parse (json);
    const auto* response = parsed.getProperty ("response", {}).getDynamicObject();
    if (response == nullptr)
    {
        setError ("archive.org returned something unexpected.");
        return false;
    }

    const auto docs = response->getProperty ("docs");
    if (! docs.isArray())
        return false;

    int added = 0;
    for (const auto& doc : *docs.getArray())
    {
        const auto identifier = doc.getProperty ("identifier", {}).toString();
        if (identifier.isEmpty())
            continue;

        const juce::ScopedLock sl (lock_);
        if (seenIdentifiers_.contains (identifier) || candidates_.size() >= kMaxQueuedCandidates)
            continue;

        seenIdentifiers_.add (identifier);

        Candidate c;
        c.identifier = identifier;
        c.title = doc.getProperty ("title", {}).toString();
        c.pageUrl = "https://archive.org/details/" + identifier;
        c.kind = SpeechSourceKind::librivox;
        c.licence = source.licence;
        c.sourceName = source.name;
        candidates_.add (c);
        ++added;
    }

    // Keep the identifier list from growing without bound over a multi-day run.
    {
        const juce::ScopedLock sl (lock_);
        while (seenIdentifiers_.size() > 20000)
            seenIdentifiers_.remove (0);
    }

    return added > 0;
}

bool SpeechFetcher::discoverFeedItems (const SpeechSource& source)
{
    setStep ("Reading feed " + source.name);

    const auto xmlText = readWholeUrl (source.feedUrl);
    if (xmlText.isEmpty())
    {
        setError ("Could not read the feed " + source.name);
        return false;
    }

    const auto xml = juce::XmlDocument::parse (xmlText);
    if (xml == nullptr)
    {
        setError ("The feed " + source.name + " is not valid XML.");
        return false;
    }

    int added = 0;

    // RSS: rss > channel > item > enclosure[url,type]
    for (auto* channel : xml->getChildWithTagNameIterator ("channel"))
    {
        for (auto* item : channel->getChildWithTagNameIterator ("item"))
        {
            auto* enclosure = item->getChildByName ("enclosure");
            if (enclosure == nullptr)
                continue;

            const auto url = enclosure->getStringAttribute ("url");
            const auto type = enclosure->getStringAttribute ("type");

            if (url.isEmpty() || ! type.startsWithIgnoreCase ("audio"))
                continue;

            // A feed cannot be host-locked, because podcast enclosures routinely
            // redirect through content networks. HTTPS, a size cap and proving
            // the file decodes are what stand in for that.
            if (! isHttps (url.toStdString()))
                continue;

            const juce::ScopedLock sl (lock_);
            if (candidates_.size() >= kMaxQueuedCandidates)
                break;

            const auto id = itemIdForUrl (url.toStdString());
            if (manifest_.contains (id))
                continue;

            Candidate c;
            c.directUrl = url;
            c.title = item->getChildElementAllSubText ("title", {});
            c.pageUrl = item->getChildElementAllSubText ("link", source.feedUrl);
            c.kind = SpeechSourceKind::podcastFeed;
            c.licence = LicenceClass::unknown;
            c.sourceName = source.name;
            candidates_.add (c);
            ++added;
        }
    }

    return added > 0;
}

// ---------------------------------------------------------------------------
bool SpeechFetcher::resolveArchiveItem (const Candidate& candidate,
                                        SpeechItem& itemOut, juce::String& urlOut)
{
    const auto metadataUrl = "https://archive.org/metadata/" + candidate.identifier;
    const auto json = readWholeUrl (metadataUrl);
    if (json.isEmpty())
        return false;

    const auto parsed = juce::JSON::parse (json);
    const auto files = parsed.getProperty ("files", {});
    if (! files.isArray())
        return false;

    CachePolicy policy;
    {
        const juce::ScopedLock sl (lock_);
        policy = policy_;
    }

    // Pick the single best derivative this platform can actually decode.
    int bestRank = std::numeric_limits<int>::max();
    juce::String bestName;
    long long bestBytes = 0;
    double bestSeconds = 0.0;

    for (const auto& f : *files.getArray())
    {
        const auto name = f.getProperty ("name", {}).toString();
        const auto format = f.getProperty ("format", {}).toString();

        if (name.isEmpty() || ! looksLikeAudioName (name))
            continue;

        if (! formatDecodableHere (format.toStdString()))
            continue;

        const int rank = archiveFormatPreference (format.toStdString());
        if (rank < 0 || rank >= bestRank)
            continue;

        const long long bytes = f.getProperty ("size", {}).toString().getLargeIntValue();
        if (bytes < policy.minItemBytes || bytes > policy.maxItemBytes)
            continue;

        bestRank = rank;
        bestName = name;
        bestBytes = bytes;
        bestSeconds = parseArchiveLength (f.getProperty ("length", {}).toString().toStdString());
    }

    if (bestName.isEmpty())
        return false;

    urlOut = juce::String (archiveDownloadUrl (candidate.identifier.toStdString(),
                                               bestName.toStdString()));
    if (urlOut.isEmpty())
        return false;

    itemOut = {};
    itemOut.mediaUrl = urlOut.toStdString();
    itemOut.id = itemIdForUrl (itemOut.mediaUrl);
    itemOut.sourceName = candidate.sourceName.toStdString();
    itemOut.kind = candidate.kind;
    itemOut.licence = candidate.licence;
    itemOut.title = (candidate.title + " - " + bestName).toStdString();
    itemOut.pageUrl = candidate.pageUrl.toStdString();
    itemOut.bytes = bestBytes;
    itemOut.seconds = bestSeconds;      // replaced by the decoded duration below
    itemOut.fileName = itemOut.id + safeExtensionFor (bestName.toStdString());
    return true;
}

bool SpeechFetcher::fetchOne (const Candidate& candidate)
{
    SpeechItem item;
    juce::String url;

    if (candidate.kind == SpeechSourceKind::librivox)
    {
        setStep ("Resolving " + candidate.identifier);
        if (! resolveArchiveItem (candidate, item, url))
        {
            const juce::ScopedLock sl (statusLock_);
            ++status_.rejectedThisSession;
            return false;
        }

        // Belt and braces: the URL we built, and whatever it redirects to, must
        // stay on the Archive.
        if (! isArchiveHost (url.toStdString()))
        {
            setError ("Refused a download URL that was not on archive.org: " + url);
            return false;
        }
    }
    else
    {
        url = candidate.directUrl;
        item.mediaUrl = url.toStdString();
        item.id = itemIdForUrl (item.mediaUrl);
        item.sourceName = candidate.sourceName.toStdString();
        item.kind = candidate.kind;
        item.licence = candidate.licence;
        item.title = candidate.title.toStdString();
        item.pageUrl = candidate.pageUrl.toStdString();
        item.fileName = item.id + extensionFromUrlPath (item.mediaUrl);
    }

    {
        const juce::ScopedLock sl (lock_);
        if (manifest_.contains (item.id))
            return false;
    }

    CachePolicy policy;
    {
        const juce::ScopedLock sl (lock_);
        policy = policy_;
    }

    const auto dir = cacheDirectory();
    dir.createDirectory();

    const auto destination = dir.getChildFile (juce::String (item.fileName));

    // Download into a staging directory rather than alongside the cache, and -
    // this is the part that matters - under the file's real extension.
    //
    // An audio reader is chosen by extension before the file is ever opened, so
    // a download staged as "<id>.ogg.part" matches no format and is rejected as
    // unplayable however perfect the bytes are. Staging keeps partial files out
    // of the cache directory without costing the extension.
    const auto staging = dir.getChildFile ("incoming");
    staging.createDirectory();
    auto temporary = staging.getChildFile (destination.getFileName());

    setStep ("Downloading " + juce::String (item.title).substring (0, 60));

    long long downloadedBytes = 0;
    if (! download (url, temporary, policy.maxItemBytes, downloadedBytes))
    {
        temporary.deleteFile();
        setError ("Download failed after " + juce::File::descriptionOfSizeInBytes (downloadedBytes)
                  + ": " + juce::String (item.title));
        const juce::ScopedLock sl (statusLock_);
        ++status_.rejectedThisSession;
        return false;
    }

    // Prove it decodes before it is allowed anywhere near a playlist, and take
    // the duration from the decoded stream rather than from what the metadata
    // claimed.
    double seconds = validateAndMeasure (temporary);

    // A name that does not match the contents is a real case for feed
    // enclosures, whose URLs often carry no extension at all. Since the reader
    // is picked by name, the only way to tell a mislabelled file from a broken
    // one is to try the other names.
    if (seconds <= 1.0)
    {
        for (const auto& ext : decodableExtensionCandidates())
        {
            if (threadShouldExit())
                break;

            const auto renamed = temporary.getParentDirectory()
                                     .getChildFile (temporary.getFileNameWithoutExtension()
                                                    + juce::String (ext));
            if (renamed == temporary || ! temporary.moveFileTo (renamed))
                continue;

            temporary = renamed;
            seconds = validateAndMeasure (temporary);
            if (seconds > 1.0)
            {
                item.fileName = renamed.getFileName().toStdString();
                break;
            }
        }
    }

    if (seconds <= 1.0)
    {
        setError ("Downloaded " + juce::File::descriptionOfSizeInBytes (downloadedBytes)
                  + " but it would not decode as audio: " + juce::String (item.title));
        temporary.deleteFile();
        const juce::ScopedLock sl (statusLock_);
        ++status_.rejectedThisSession;
        return false;
    }

    const auto finalDestination = dir.getChildFile (juce::String (item.fileName));
    finalDestination.deleteFile();
    if (! temporary.moveFileTo (finalDestination))
    {
        temporary.deleteFile();
        setError ("Could not move a downloaded file into the cache.");
        return false;
    }

    item.seconds = seconds;
    item.bytes = finalDestination.getSize();
    item.fetchedAtUnix = juce::Time::getCurrentTime().toMilliseconds() / 1000;

    {
        const juce::ScopedLock sl (lock_);
        manifest_.add (item);
    }

    {
        const juce::ScopedLock sl (statusLock_);
        ++status_.downloadedThisSession;
    }

    return true;
}

bool SpeechFetcher::download (const juce::String& url, const juce::File& destination,
                              long long maxBytes, long long& bytesOut)
{
    bytesOut = 0;

    auto stream = openUrl (url);
    if (stream == nullptr)
        return false;

    destination.deleteFile();
    juce::FileOutputStream out (destination);
    if (out.failedToOpen())
        return false;

    juce::HeapBlock<char> buffer (65536);
    long long total = 0;

    while (! stream->isExhausted())
    {
        if (threadShouldExit())
            return false;

        const int read = stream->read (buffer.get(), 65536);
        if (read <= 0)
            break;

        total += read;
        bytesOut = total;

        // A server that keeps sending is not permitted to fill the disk.
        if (total > maxBytes)
            return false;

        if (! out.write (buffer.get(), static_cast<size_t> (read)))
            return false;
    }

    out.flush();
    return total > 0;
}

double SpeechFetcher::validateAndMeasure (const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader (formats_.createReaderFor (file));
    if (reader == nullptr)
        return 0.0;

    if (reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0)
        return 0.0;

    return static_cast<double> (reader->lengthInSamples) / reader->sampleRate;
}

// ---------------------------------------------------------------------------
void SpeechFetcher::enforceCacheBudget()
{
    CachePolicy policy;
    long long total = 0;
    {
        const juce::ScopedLock sl (lock_);
        policy = policy_;
        total = manifest_.totalBytes();
    }

    // Keep one item's worth of headroom, so the next download does not have to
    // evict mid-flight.
    const long long wanted = policy.maxBytes - policy.maxItemBytes;
    if (total <= wanted)
        return;

    const auto toEvict = manifest().selectForEviction (total - wanted);
    if (toEvict.empty())
        return;

    setStep ("Making room in the cache (" + juce::String (static_cast<int> (toEvict.size())) + " files)");

    const auto dir = cacheDirectory();
    for (const auto& id : toEvict)
    {
        juce::String fileName;
        {
            const juce::ScopedLock sl (lock_);
            if (const auto* item = manifest_.find (id))
                fileName = juce::String (item->fileName);
        }

        if (fileName.isNotEmpty())
            dir.getChildFile (fileName).deleteFile();

        const juce::ScopedLock sl (lock_);
        manifest_.remove (id);
    }

    saveLibrary();
    dirty_ = true;
}

// ---------------------------------------------------------------------------
juce::Array<juce::File> SpeechFetcher::playlist() const
{
    juce::Array<juce::File> files;
    const auto dir = cacheDirectory();

    std::vector<SpeechItem> allowed;
    {
        const juce::ScopedLock sl (lock_);
        allowed = manifest_.playable (policy_);
    }

    for (const auto& i : allowed)
    {
        const auto f = dir.getChildFile (juce::String (i.fileName));
        if (f.existsAsFile())
            files.add (f);
    }

    // Shuffled, so a run does not work through one reader's entire back
    // catalogue before hearing a different voice. The corpus is only as diverse
    // as the order it is played in.
    juce::Random shuffler (juce::Time::getMillisecondCounter());
    for (int i = files.size(); --i > 0;)
        files.swap (i, shuffler.nextInt (i + 1));

    return files;
}

void SpeechFetcher::notePlayed (const juce::File& file)
{
    const auto name = file.getFileName().toStdString();

    {
        const juce::ScopedLock sl (lock_);
        for (const auto& i : manifest_.items())
        {
            if (i.fileName == name)
            {
                manifest_.notePlayed (i.id, juce::Time::getCurrentTime().toMilliseconds() / 1000);
                break;
            }
        }
    }
}
} // namespace fbkt
