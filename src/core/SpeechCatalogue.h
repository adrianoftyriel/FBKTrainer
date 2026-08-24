// FBKTrainer - SpeechCatalogue.h
//
// What speech the rig plays, where it came from, and what may be kept.
//
// Provenance is the point
// -----------------------
// The eventual deliverable is a model that ships. A model that ships is a model
// whose training material somebody will eventually ask about - which corpus,
// under what licence, fetched when. That question is cheap to answer if every
// file was recorded with its origin at the moment it arrived, and impossible to
// answer afterwards from a directory of audio. So nothing enters the cache
// without a manifest entry naming its source, its licence and the page it came
// from, and the run can be restricted to open-licensed material only.
//
// That restriction is on by default. Fetching a podcast for a local experiment is
// reasonable; letting it into the corpus behind a model you intend to distribute
// is not, and the difference should not depend on anybody remembering.
//
// Nothing here does I/O
// ---------------------
// This is the policy: which hosts may be fetched from, which formats are worth
// having, what a duration string means, what to evict when the cache is full, and
// whether enough material is banked to start a run nobody will be watching. All
// of it is arithmetic and string handling, so all of it is testable without a
// network.
#pragma once

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace fbkt
{
// ---------------------------------------------------------------------------
enum class LicenceClass
{
    publicDomain,           // LibriVox: recordings of public-domain texts, released to the public domain
    permissiveAttribution,  // CC-BY and similar: usable, with attribution recorded
    unknown                 // anything the operator pointed us at. Local use only.
};

const char* toString (LicenceClass) noexcept;
const char* describe (LicenceClass) noexcept;

enum class SpeechSourceKind
{
    librivox,       // built-in, via the Internet Archive
    podcastFeed,    // an RSS/Atom feed the operator supplied
    localFolder     // files on this machine
};

const char* toString (SpeechSourceKind) noexcept;

// ---------------------------------------------------------------------------
struct SpeechItem
{
    std::string id;              // stable; derived from the media URL
    std::string sourceName;
    SpeechSourceKind kind { SpeechSourceKind::librivox };
    LicenceClass licence { LicenceClass::unknown };

    std::string title;
    std::string pageUrl;         // where a human can go to check the provenance
    std::string mediaUrl;
    std::string fileName;        // within the cache directory

    long long bytes { 0 };
    double    seconds { 0.0 };

    long long fetchedAtUnix { 0 };
    long long lastPlayedUnix { 0 };   // 0 = never played
    int       timesPlayed { 0 };
};

// ---------------------------------------------------------------------------
struct CachePolicy
{
    // How much disk the fetched corpus may occupy. A multi-day run wants a lot of
    // material and the machine running it usually has one disk, so this is a
    // limit rather than a target.
    long long maxBytes { 8LL * 1024 * 1024 * 1024 };

    // How much playable material must be banked before an unattended run may
    // start. Running out of speech mid-run does not damage anything - it just
    // means the room goes quiet and the remaining hours measure nothing.
    double minBufferedSecondsForUnattended { 6.0 * 3600.0 };

    // Below this, the fetcher works to top the cache up.
    double targetBufferedSeconds { 24.0 * 3600.0 };

    // Whether unknown-licence material may be played at all. On by default: see
    // the header comment.
    bool requireOpenLicence { true };

    // Nothing enormous, and nothing suspiciously small. A 4 kB "file" is an error
    // page; a 2 GB one is not a chapter of a book.
    long long minItemBytes { 64 * 1024 };
    long long maxItemBytes { 512LL * 1024 * 1024 };
};

bool licenceAcceptable (LicenceClass licence, const CachePolicy& policy) noexcept;

// ---------------------------------------------------------------------------
// URL handling.
//
// The built-in source is locked to the Internet Archive's hosts. This is not
// about trusting the Archive with our data - we send it nothing - but about
// bounding where a redirect or a malformed metadata response can send us. A
// feed the operator supplies cannot be host-locked, because podcast enclosures
// routinely redirect through CDNs; those get HTTPS and a size cap instead, and
// every file is proved by decoding it before it is ever queued for playback.

std::string hostOf (const std::string& url);
bool isHttps (const std::string& url);

// archive.org, and its numbered file servers.
bool isArchiveHost (const std::string& url);

// ---------------------------------------------------------------------------
// Which of an Archive item's derivatives is worth downloading.
//
// Preference order is FLAC, then Ogg Vorbis, then the highest-rate MP3. The
// ordering is partly about quality - LibriVox material is bounded by whatever
// the reader uploaded, but taking the 64 kbps derivative when a 128 kbps one
// exists throws away bandwidth we are trying to excite a room with - and partly
// practical: the Linux build has no MP3 decoder, since JUCE's software decoder
// is off by default and carries a patent disclaimer, while Windows and macOS
// decode MP3 through the operating system.
//
// Returns a rank, lower being better, or -1 for a format we cannot use.
int archiveFormatPreference (const std::string& archiveFormatName) noexcept;

// True if this format can be decoded on the platform the program was built for.
bool formatDecodableHere (const std::string& archiveFormatName) noexcept;

// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// One file as the Archive's metadata describes it.
struct ArchiveFile
{
    std::string name;
    std::string format;
    long long   bytes { 0 };
    double      seconds { 0.0 };
};

// Which of an item's files to fetch, or -1 if none of them will do.
//
// This is separated from the code that talks to the network on purpose. The
// choice is where the judgement lives - reject the cover art and the metadata
// files, prefer the derivative worth the bandwidth, refuse an MP3 on a platform
// with no MP3 decoder, skip anything absurdly small or large - and it is exactly
// the part that should be tested against awkward inputs rather than against
// whatever a live service happened to return the day someone tried it.
int chooseBestDerivative (const std::vector<ArchiveFile>& files, const CachePolicy& policy) noexcept;

// Percent-encodes one path segment for a download URL. Archive item filenames
// routinely contain spaces, apostrophes and non-ASCII characters, and a URL
// built by concatenation would simply 404 for those - silently reducing the
// corpus to the files with tidy names.
std::string percentEncodePathSegment (const std::string& segment);

// The download URL for one file of one Archive item.
std::string archiveDownloadUrl (const std::string& identifier, const std::string& fileName);

// The extension to give the local copy, derived from a remote filename that we
// do not control.
//
// Cached files are named after their id hash, so the only part of a remote name
// that reaches the filesystem is this extension - and that is enough to matter.
// Taking everything after the last dot of "a.o/b" yields ".o/b", which turns a
// filename into a path, and a path is something File::getChildFile will happily
// resolve. So the extension is accepted only if it is short and alphanumeric,
// and anything else becomes a harmless default.
std::string safeExtensionFor (const std::string& remoteFileName);

// The extension implied by a URL's path, ignoring the query and fragment.
//
// Podcast enclosures routinely carry tracking parameters - ".../ep12.mp3?t=abc"
// - and taking everything after the last dot of the whole URL yields ".mp3?t=abc"
// or, worse, the extension of a query value. Since a file's extension is what
// decides whether it can be decoded at all, getting this wrong means every
// fetched file is discarded as unplayable.
std::string extensionFromUrlPath (const std::string& url);

// Extensions worth trying when a file's real format is not known from its name -
// a feed enclosure with no extension, or one whose extension is a lie. Ordered
// by how likely they are for spoken-word audio on the open web.
//
// This exists because an audio file is identified by its extension before it is
// opened: the reader is chosen by name, not by content, so a correct file with
// the wrong name is indistinguishable from a corrupt one.
const std::vector<std::string>& decodableExtensionCandidates();

// The Archive reports a file's duration as either plain seconds ("1234.56") or
// a clock string ("20:34", "1:20:34"). Returns 0 for anything unparseable
// rather than guessing - a wrong duration would corrupt the buffered-hours
// figure that decides whether an unattended run may start.
double parseArchiveLength (const std::string& text) noexcept;

// A stable id for an item, derived from its media URL, so the same file fetched
// twice is recognised as the same file.
std::string itemIdForUrl (const std::string& mediaUrl);

// ---------------------------------------------------------------------------
class SpeechManifest
{
public:
    void clear() noexcept;

    bool contains (const std::string& id) const noexcept;
    bool add (const SpeechItem& item);          // false if already present
    bool remove (const std::string& id);

    const std::vector<SpeechItem>& items() const noexcept { return items_; }
    std::vector<SpeechItem>& mutableItems() noexcept { return items_; }

    const SpeechItem* find (const std::string& id) const noexcept;

    void notePlayed (const std::string& id, long long atUnix) noexcept;

    long long totalBytes() const noexcept;

    // Material the policy permits to be played, and how much of it there is.
    std::vector<SpeechItem> playable (const CachePolicy&) const;
    double playableSeconds (const CachePolicy&) const noexcept;

    bool readyForUnattendedRun (const CachePolicy& policy) const noexcept
    {
        return playableSeconds (policy) >= policy.minBufferedSecondsForUnattended;
    }

    // How many more bytes may be fetched before the cache is full.
    long long headroomBytes (const CachePolicy& policy) const noexcept;

    // Ids to delete in order to free at least bytesToFree, least-recently-played
    // first. Material that has never been played is evicted last: it is the only
    // part of the cache that still has something to contribute, and throwing it
    // away to keep something already used would make the corpus narrower every
    // time the disk filled.
    //
    // Anything in protectedIds is never selected - the file currently playing,
    // and whatever is queued behind it.
    std::vector<std::string> selectForEviction (long long bytesToFree,
                                                const std::set<std::string>& protectedIds = {}) const;

private:
    std::vector<SpeechItem> items_;
};
} // namespace fbkt
