// FBKTrainer - format tests.
//
// These exist because of a bug that shipped, and they encode the thing that made
// it possible to ship.
//
// An audio file is identified by its *name*. JUCE picks a reader by asking each
// registered format whether it can handle the file, and every format answers by
// looking at the extension - not at the bytes. So a perfectly good Ogg file
// staged as "abc.ogg.part" matches no format at all, and the only symptom is
// that it "would not decode".
//
// That is exactly what happened: every fetched file was downloaded correctly and
// then rejected, because the download was staged under a ".part" suffix and
// validated there. Nothing about the bytes was ever wrong.
//
// The core tests cannot cover this, because they are deliberately free of JUCE
// and this is a fact about JUCE's format registry. So it gets its own small
// target, which also means it runs on all three platforms in CI - where it
// doubles as a check on which formats each platform can actually decode.
#include <JuceHeader.h>

#include "SpeechCatalogue.h"

namespace
{
int gFailures = 0;
int gChecks = 0;

void check (bool ok, const char* expr, int line)
{
    ++gChecks;
    if (! ok)
    {
        ++gFailures;
        std::printf ("  FAIL  %s\n        at FormatTests.cpp:%d\n", expr, line);
    }
}

#define CHECK(x) check ((x), #x, __LINE__)

void beginTest (const char* name)
{
    std::printf ("\n=== %s ===\n", name);
}

// A second of quiet noise, written as a real WAV so the reader has something
// genuine to open.
bool writeTestWav (const juce::File& file)
{
    file.deleteFile();

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::FileOutputStream> stream (file.createOutputStream());
    if (stream == nullptr)
        return false;

    std::unique_ptr<juce::AudioFormatWriter> writer (
        wav.createWriterFor (stream.get(), 44100.0, 1, 16, {}, 0));
    if (writer == nullptr)
        return false;

    stream.release();   // the writer owns it now

    juce::AudioBuffer<float> buffer (1, 44100);
    juce::Random random (1234);
    for (int i = 0; i < buffer.getNumSamples(); ++i)
        buffer.setSample (0, i, (random.nextFloat() * 2.0f - 1.0f) * 0.1f);

    return writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
}
} // namespace

int main()
{
    // No JUCE initialiser: reading audio formats needs neither a message loop
    // nor a GUI, and this target links only juce_audio_formats.
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                         .getChildFile ("fbkt-format-tests");
    dir.deleteRecursively();
    dir.createDirectory();

    beginTest ("A real audio file decodes under its own extension");

    const auto good = dir.getChildFile ("sample.wav");
    CHECK (writeTestWav (good));
    CHECK (good.existsAsFile());
    CHECK (good.getSize() > 1000);

    {
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (good));
        CHECK (reader != nullptr);
        if (reader != nullptr)
        {
            CHECK (reader->sampleRate == 44100.0);
            CHECK (reader->lengthInSamples == 44100);
        }
    }

    beginTest ("The same bytes under a staging suffix decode as nothing at all");

    // This is the shipped bug, reproduced. The file is byte-identical to the one
    // above; only the name differs. A validation step that runs here rejects
    // every file it is ever given, and reports it as a decode failure - which
    // sends anyone debugging it looking at the download, which is fine.
    const auto staged = dir.getChildFile ("sample.wav.part");
    CHECK (good.copyFileTo (staged));
    CHECK (staged.getSize() == good.getSize());
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (staged));
        CHECK (reader == nullptr);
    }

    // And the feed path's placeholder extension has the same problem
    // independently: ".audio" is not a format anyone registers.
    const auto placeholder = dir.getChildFile ("sample.audio");
    CHECK (good.copyFileTo (placeholder));
    {
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (placeholder));
        CHECK (reader == nullptr);
    }

    beginTest ("Renaming to a candidate extension recovers a mislabelled file");

    // What the fetcher now does when a name does not match the contents, which is
    // the normal case for a feed enclosure with no extension in its URL.
    bool recovered = false;
    auto trying = placeholder;
    for (const auto& ext : fbkt::decodableExtensionCandidates())
    {
        const auto renamed = trying.getParentDirectory()
                                 .getChildFile (trying.getFileNameWithoutExtension()
                                                + juce::String (ext));
        if (renamed == trying || ! trying.moveFileTo (renamed))
            continue;

        trying = renamed;
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (trying));
        if (reader != nullptr && reader->lengthInSamples > 0)
        {
            recovered = true;
            break;
        }
    }
    CHECK (recovered);

    beginTest ("What this platform can decode");

    // Reported rather than asserted, because it differs by platform on purpose:
    // Windows decodes MP3 through Media Foundation and macOS through CoreAudio,
    // while the Linux build has neither and skips MP3 items instead of
    // downloading them and finding them unplayable.
    juce::StringArray extensions;
    for (auto* format : formats)
        extensions.addArray (format->getFileExtensions());
    extensions.removeDuplicates (true);

    std::printf ("  info  formats: %s\n", formats.getWildcardForAllFormats().toRawUTF8());
    std::printf ("  info  extensions: %s\n", extensions.joinIntoString (" ").toRawUTF8());

    const bool mp3Here = extensions.contains (".mp3", true);
    std::printf ("  info  mp3 decodable here: %s\n", mp3Here ? "yes" : "no");

    // The catalogue's compile-time view of MP3 support has to agree with the
    // format registry's runtime view, or the fetcher either downloads files it
    // cannot play or skips ones it could.
    CHECK (fbkt::formatDecodableHere ("128Kbps MP3") == mp3Here);
    CHECK (fbkt::formatDecodableHere ("Ogg Vorbis"));
    CHECK (fbkt::formatDecodableHere ("Flac"));
    CHECK (! fbkt::formatDecodableHere ("JPEG"));

    dir.deleteRecursively();

    std::printf ("\n----------------------------------------\n");
    std::printf ("%d checks, %d failures\n", gChecks, gFailures);
    return gFailures == 0 ? 0 : 1;
}
