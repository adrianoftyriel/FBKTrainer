#include "SpeechCatalogue.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace fbkt
{
namespace
{
std::string lowercase (std::string s)
{
    for (auto& c : s)
        c = static_cast<char> (std::tolower (static_cast<unsigned char> (c)));
    return s;
}

bool endsWith (const std::string& s, const std::string& suffix)
{
    return s.size() >= suffix.size()
           && s.compare (s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool contains (const std::string& s, const std::string& needle)
{
    return s.find (needle) != std::string::npos;
}
} // namespace

// ---------------------------------------------------------------------------
const char* toString (LicenceClass l) noexcept
{
    switch (l)
    {
        case LicenceClass::publicDomain:          return "public-domain";
        case LicenceClass::permissiveAttribution: return "permissive-attribution";
        case LicenceClass::unknown:               return "unknown";
    }
    return "?";
}

const char* describe (LicenceClass l) noexcept
{
    switch (l)
    {
        case LicenceClass::publicDomain:
            return "Public domain. Usable in a model you intend to distribute.";
        case LicenceClass::permissiveAttribution:
            return "Openly licensed with attribution. Usable, and the attribution is recorded with the file.";
        case LicenceClass::unknown:
            return "Licence unknown. Local experimentation only - not permitted into the corpus behind a distributed model.";
    }
    return "";
}

const char* toString (SpeechSourceKind k) noexcept
{
    switch (k)
    {
        case SpeechSourceKind::librivox:    return "librivox";
        case SpeechSourceKind::podcastFeed: return "podcast-feed";
        case SpeechSourceKind::localFolder: return "local-folder";
    }
    return "?";
}

bool licenceAcceptable (LicenceClass licence, const CachePolicy& policy) noexcept
{
    if (! policy.requireOpenLicence)
        return true;
    return licence == LicenceClass::publicDomain || licence == LicenceClass::permissiveAttribution;
}

// ---------------------------------------------------------------------------
std::string hostOf (const std::string& url)
{
    const auto schemeEnd = url.find ("://");
    if (schemeEnd == std::string::npos)
        return {};

    const auto start = schemeEnd + 3;
    auto end = url.find_first_of ("/?#", start);
    if (end == std::string::npos)
        end = url.size();

    auto host = lowercase (url.substr (start, end - start));

    // Strip any userinfo, which is the classic way to make a URL look like it
    // points somewhere it does not: https://archive.org@evil.example/x
    const auto at = host.find ('@');
    if (at != std::string::npos)
        host = host.substr (at + 1);

    // And any port.
    const auto colon = host.find (':');
    if (colon != std::string::npos)
        host = host.substr (0, colon);

    return host;
}

bool isHttps (const std::string& url)
{
    return lowercase (url).rfind ("https://", 0) == 0;
}

bool isArchiveHost (const std::string& url)
{
    if (! isHttps (url))
        return false;

    const auto host = hostOf (url);
    if (host.empty())
        return false;

    // The Archive serves files from numbered per-datacentre hosts as well as from
    // archive.org itself, and a download URL redirects to one of them.
    return host == "archive.org"
           || host == "librivox.org"
           || endsWith (host, ".archive.org");
}

// ---------------------------------------------------------------------------
int archiveFormatPreference (const std::string& archiveFormatName) noexcept
{
    const auto f = lowercase (archiveFormatName);

    if (contains (f, "flac"))          return 0;
    if (contains (f, "ogg vorbis"))    return 1;
    if (contains (f, "ogg"))           return 2;

    // MP3 derivatives, best rate first. The Archive names these "VBR MP3",
    // "128Kbps MP3", "64Kbps MP3" and so on.
    if (contains (f, "vbr mp3"))       return 10;
    if (contains (f, "256kbps mp3"))   return 11;
    if (contains (f, "192kbps mp3"))   return 12;
    if (contains (f, "128kbps mp3"))   return 13;
    if (contains (f, "96kbps mp3"))    return 14;
    if (contains (f, "64kbps mp3"))    return 15;
    if (contains (f, "32kbps mp3"))    return 16;
    if (contains (f, "mp3"))           return 17;

    if (contains (f, "wave") || contains (f, "wav")) return 3;
    if (contains (f, "aiff"))          return 4;

    // Everything else - text, images, the Archive's own metadata files, and
    // formats no audio reader here will open.
    return -1;
}

bool formatDecodableHere (const std::string& archiveFormatName) noexcept
{
    const int rank = archiveFormatPreference (archiveFormatName);
    if (rank < 0)
        return false;

   #if defined(__linux__) && ! defined(FBKT_HAVE_MP3)
    // No MP3 decoder on Linux: JUCE's software decoder is off by default and
    // carries a patent disclaimer, and there is no OS codec to fall back on.
    // Windows decodes MP3 through Media Foundation and macOS through CoreAudio,
    // so this restriction applies to the portability build only.
    if (rank >= 10)
        return false;
   #endif

    return true;
}

// ---------------------------------------------------------------------------
namespace
{
bool looksLikeAudioFileName (const std::string& name)
{
    static const char* kExtensions[] = { ".mp3", ".ogg", ".flac", ".wav",
                                         ".aiff", ".aif", ".opus", ".m4a" };
    const auto lower = lowercase (name);
    for (const auto* ext : kExtensions)
        if (endsWith (lower, ext))
            return true;
    return false;
}
} // namespace

int chooseBestDerivative (const std::vector<ArchiveFile>& files, const CachePolicy& policy) noexcept
{
    int bestIndex = -1;
    int bestRank = std::numeric_limits<int>::max();

    for (size_t i = 0; i < files.size(); ++i)
    {
        const auto& f = files[i];

        // Two independent checks, because the Archive's format field and the
        // file extension disagree often enough to matter: an item can carry a
        // "VBR MP3" entry describing a .png thumbnail derivative's sibling, and
        // a name-only check would accept a .mp3 entry whose format is something
        // no reader here opens.
        if (! looksLikeAudioFileName (f.name))
            continue;

        if (! formatDecodableHere (f.format))
            continue;

        const int rank = archiveFormatPreference (f.format);
        if (rank < 0 || rank >= bestRank)
            continue;

        // An error page saved as audio is a few kilobytes; a whole unsplit
        // audiobook is a gigabyte. Neither is a chapter of speech.
        if (f.bytes < policy.minItemBytes || f.bytes > policy.maxItemBytes)
            continue;

        bestRank = rank;
        bestIndex = static_cast<int> (i);
    }

    return bestIndex;
}

std::string percentEncodePathSegment (const std::string& segment)
{
    static const char* kHex = "0123456789ABCDEF";
    std::string out;
    out.reserve (segment.size() + 8);

    for (unsigned char c : segment)
    {
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
                                || (c >= '0' && c <= '9')
                                || c == '-' || c == '_' || c == '.' || c == '~';
        if (unreserved)
        {
            out += static_cast<char> (c);
        }
        else
        {
            out += '%';
            out += kHex[(c >> 4) & 0x0f];
            out += kHex[c & 0x0f];
        }
    }
    return out;
}

std::string safeExtensionFor (const std::string& remoteFileName)
{
    const auto dot = remoteFileName.find_last_of ('.');
    if (dot == std::string::npos || dot + 1 >= remoteFileName.size())
        return ".audio";

    const auto ext = lowercase (remoteFileName.substr (dot + 1));
    if (ext.size() > 5)
        return ".audio";

    for (unsigned char c : ext)
        if (! ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
            return ".audio";

    return "." + ext;
}

std::string extensionFromUrlPath (const std::string& url)
{
    // Trim the query and the fragment before looking for an extension: the dot
    // in "?v=1.2" is not a file extension, and treating it as one produces a
    // name no audio reader will ever match.
    auto path = url;
    const auto cut = path.find_first_of ("?#");
    if (cut != std::string::npos)
        path = path.substr (0, cut);

    // And only look at the last path segment.
    const auto slash = path.find_last_of ('/');
    if (slash != std::string::npos)
        path = path.substr (slash + 1);

    return safeExtensionFor (path);
}

const std::vector<std::string>& decodableExtensionCandidates()
{
    static const std::vector<std::string> candidates { ".mp3", ".ogg", ".m4a", ".flac", ".wav", ".aiff" };
    return candidates;
}

std::string archiveDownloadUrl (const std::string& identifier, const std::string& fileName)
{
    if (identifier.empty() || fileName.empty())
        return {};
    return "https://archive.org/download/" + percentEncodePathSegment (identifier)
           + "/" + percentEncodePathSegment (fileName);
}

// ---------------------------------------------------------------------------
double parseArchiveLength (const std::string& text) noexcept
{
    if (text.empty())
        return 0.0;

    // Clock form: [hh:]mm:ss(.fff)
    if (text.find (':') != std::string::npos)
    {
        double parts[3] = { 0.0, 0.0, 0.0 };
        int count = 0;
        size_t start = 0;

        while (count < 3)
        {
            const auto colon = text.find (':', start);
            const auto piece = text.substr (start, colon == std::string::npos
                                                       ? std::string::npos
                                                       : colon - start);
            if (piece.empty())
                return 0.0;

            char* end = nullptr;
            const double v = std::strtod (piece.c_str(), &end);
            if (end == piece.c_str() || v < 0.0)
                return 0.0;

            parts[count++] = v;
            if (colon == std::string::npos)
                break;
            start = colon + 1;
        }

        if (count == 2)
            return parts[0] * 60.0 + parts[1];
        if (count == 3)
            return parts[0] * 3600.0 + parts[1] * 60.0 + parts[2];
        return 0.0;
    }

    char* end = nullptr;
    const double v = std::strtod (text.c_str(), &end);
    if (end == text.c_str() || v < 0.0 || v != v)
        return 0.0;
    return v;
}

std::string itemIdForUrl (const std::string& mediaUrl)
{
    // A short, stable, filesystem-safe digest. FNV-1a is more than enough here:
    // the only thing being defended against is two different files colliding by
    // accident, and there is nothing to gain by making them collide on purpose.
    std::uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : mediaUrl)
    {
        hash ^= c;
        hash *= 1099511628211ull;
    }

    char buf[24];
    std::snprintf (buf, sizeof (buf), "%016llx", static_cast<unsigned long long> (hash));
    return buf;
}

// ---------------------------------------------------------------------------
void SpeechManifest::clear() noexcept
{
    items_.clear();
}

bool SpeechManifest::contains (const std::string& id) const noexcept
{
    return find (id) != nullptr;
}

const SpeechItem* SpeechManifest::find (const std::string& id) const noexcept
{
    for (const auto& i : items_)
        if (i.id == id)
            return &i;
    return nullptr;
}

bool SpeechManifest::add (const SpeechItem& item)
{
    if (item.id.empty() || contains (item.id))
        return false;
    items_.push_back (item);
    return true;
}

bool SpeechManifest::remove (const std::string& id)
{
    const auto it = std::find_if (items_.begin(), items_.end(),
                                  [&] (const SpeechItem& i) { return i.id == id; });
    if (it == items_.end())
        return false;
    items_.erase (it);
    return true;
}

void SpeechManifest::notePlayed (const std::string& id, long long atUnix) noexcept
{
    for (auto& i : items_)
    {
        if (i.id == id)
        {
            i.lastPlayedUnix = atUnix;
            ++i.timesPlayed;
            return;
        }
    }
}

long long SpeechManifest::totalBytes() const noexcept
{
    long long total = 0;
    for (const auto& i : items_)
        total += i.bytes;
    return total;
}

std::vector<SpeechItem> SpeechManifest::playable (const CachePolicy& policy) const
{
    std::vector<SpeechItem> out;
    for (const auto& i : items_)
        if (licenceAcceptable (i.licence, policy))
            out.push_back (i);
    return out;
}

double SpeechManifest::playableSeconds (const CachePolicy& policy) const noexcept
{
    double total = 0.0;
    for (const auto& i : items_)
        if (licenceAcceptable (i.licence, policy))
            total += i.seconds;
    return total;
}

long long SpeechManifest::headroomBytes (const CachePolicy& policy) const noexcept
{
    return std::max (0LL, policy.maxBytes - totalBytes());
}

std::vector<std::string> SpeechManifest::selectForEviction (
    long long bytesToFree, const std::set<std::string>& protectedIds) const
{
    std::vector<std::string> chosen;
    if (bytesToFree <= 0)
        return chosen;

    std::vector<const SpeechItem*> candidates;
    candidates.reserve (items_.size());
    for (const auto& i : items_)
        if (protectedIds.find (i.id) == protectedIds.end())
            candidates.push_back (&i);

    // Least-recently-played first. Never-played sorts last, so fresh material
    // survives a full disk and the corpus does not narrow every time it fills.
    std::sort (candidates.begin(), candidates.end(),
               [] (const SpeechItem* a, const SpeechItem* b)
               {
                   const long long ka = a->lastPlayedUnix == 0
                                            ? std::numeric_limits<long long>::max()
                                            : a->lastPlayedUnix;
                   const long long kb = b->lastPlayedUnix == 0
                                            ? std::numeric_limits<long long>::max()
                                            : b->lastPlayedUnix;
                   if (ka != kb)
                       return ka < kb;
                   return a->id < b->id;      // stable, for a predictable test
               });

    long long freed = 0;
    for (const auto* item : candidates)
    {
        chosen.push_back (item->id);
        freed += item->bytes;
        if (freed >= bytesToFree)
            break;
    }
    return chosen;
}
} // namespace fbkt
