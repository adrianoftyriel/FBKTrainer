#include "MainComponent.h"

#include <cmath>

namespace fbkt
{
namespace
{
constexpr int kRowHeight = 26;
constexpr int kLabelWidth = 190;

void styleLabel (juce::Label& l, const juce::String& text)
{
    l.setText (text, juce::dontSendNotification);
    l.setJustificationType (juce::Justification::centredLeft);
}

juce::String issueText (const std::vector<ConfigIssue>& issues)
{
    if (issues.empty())
        return "Everything checks out.";

    juce::StringArray lines;
    for (const auto& i : issues)
        lines.add (juce::String (i.severity == ConfigIssue::Severity::error ? "ERROR   " : "warning ")
                   + juce::String (i.message));
    return lines.joinIntoString ("\n");
}

// A read-only multi-line box, used everywhere a log or a verdict is shown.
void makeLogBox (juce::TextEditor& e)
{
    e.setMultiLine (true);
    e.setReadOnly (true);
    e.setScrollbarsShown (true);
    e.setCaretVisible (false);
    e.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 13.0f, juce::Font::plain));
}
} // namespace

void AppState::save()
{
    juce::String error;
    saveRig (rig, rigFile, error);
}

void AppState::applySpeechSettings()
{
    speech.setCacheDirectory (juce::File (rig.speechCacheFolder));
    speech.setPolicy (rig.speechPolicy);
    speech.setSources (rig.speechSources);
}

void AppState::refreshPlaylist()
{
    // Fetched material first, then anything in the local folder. The local
    // folder is additional rather than alternative: it is where the material no
    // public corpus contains goes - sung notes, sustained vowels, the difficult
    // voice this rig actually has to cope with.
    auto files = speech.playlist();

    if (rig.speechFolder.isNotEmpty())
    {
        const juce::File folder (rig.speechFolder);
        if (folder.isDirectory())
            for (const auto& entry : juce::RangedDirectoryIterator (
                     folder, true, "*.wav;*.flac;*.aiff;*.aif;*.mp3;*.ogg"))
                files.add (entry.getFile());
    }

    audio.setSpeechPlaylist (files);
}

void AppState::wiringChanged()
{
    // A self-test pass refers to one specific wiring. Anything that could change
    // what is plugged into what invalidates it, and it is cleared here rather
    // than checked at the point of use so that no path can consult a stale pass.
    rig.selfTestPassed = false;
    rig.selfTestSignature = {};
    save();
    if (onChanged)
        onChanged();
}

// ===========================================================================
class RigPanel final : public juce::Component, private juce::Timer
{
public:
    explicit RigPanel (AppState& state) : state_ (state)
    {
        addAndMakeVisible (deviceSelector_);

        styleLabel (nameLabel_, "Rig name");
        addAndMakeVisible (nameLabel_);
        addAndMakeVisible (nameEditor_);
        nameEditor_.setText (state_.rig.config.name, juce::dontSendNotification);
        nameEditor_.onTextChange = [this]
        {
            state_.rig.config.name = nameEditor_.getText().toStdString();
            state_.save();
        };

        auto setUpPort = [this] (juce::Label& label, juce::ComboBox& box, const juce::String& text)
        {
            styleLabel (label, text);
            addAndMakeVisible (label);
            addAndMakeVisible (box);
        };

        setUpPort (speechLabel_, speechBox_, "Speech Output");
        setUpPort (micLabel_, micBox_, "Listening Mic Input");
        setUpPort (returnLabel_, returnBox_, "Vocal Return Input");

        speechBox_.onChange = [this]
        {
            state_.rig.config.speechOutput.index = speechBox_.getSelectedId() - 1;
            state_.wiringChanged();
        };
        micBox_.onChange = [this]
        {
            state_.rig.config.listeningMicInput.index = micBox_.getSelectedId() - 1;
            state_.wiringChanged();
        };
        returnBox_.onChange = [this]
        {
            state_.rig.config.vocalReturnInput.index = returnBox_.getSelectedId() - 1;
            state_.wiringChanged();
        };

        addAndMakeVisible (applyButton_);
        applyButton_.setButtonText ("Open audio device");
        applyButton_.onClick = [this] { openDevice(); };

        makeLogBox (statusBox_);
        addAndMakeVisible (statusBox_);

        startTimerHz (4);
        refresh();
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12);

        auto row = [&area] (juce::Label& l, juce::Component& c)
        {
            auto r = area.removeFromTop (kRowHeight);
            l.setBounds (r.removeFromLeft (kLabelWidth));
            c.setBounds (r);
            area.removeFromTop (6);
        };

        row (nameLabel_, nameEditor_);
        row (speechLabel_, speechBox_);
        row (micLabel_, micBox_);
        row (returnLabel_, returnBox_);

        applyButton_.setBounds (area.removeFromTop (30).removeFromLeft (200));
        area.removeFromTop (8);

        statusBox_.setBounds (area.removeFromBottom (juce::jmax (90, area.getHeight() / 3)));
        area.removeFromBottom (8);
        deviceSelector_.setBounds (area);
    }

private:
    void openDevice()
    {
        auto* device = state_.audio.deviceManager().getCurrentAudioDevice();
        if (device != nullptr)
            state_.rig.config.audioDeviceName = device->getName().toStdString();

        juce::String error;
        if (! state_.audio.start (state_.rig.config, error))
        {
            statusBox_.setText ("Could not open the audio device.\n\n" + error, false);
            return;
        }

        state_.rig.config.sampleRate = state_.audio.sampleRate();
        state_.wiringChanged();
        refresh();
    }

    void refresh()
    {
        populatePorts();

        juce::StringArray lines;
        lines.add ("Audio device: " + juce::String (state_.audio.isRunning() ? "running" : "not running"));
        if (state_.audio.isRunning())
            lines.add ("Sample rate: " + juce::String (state_.audio.sampleRate(), 0) + " Hz");

        lines.add ("Speech in the playlist: " + juce::String (state_.audio.speechFileCount()));
        lines.add ("");
        lines.add (issueText (validate (state_.rig.config)));

        statusBox_.setText (lines.joinIntoString ("\n"), false);
    }

    void populatePorts()
    {
        auto* device = state_.audio.deviceManager().getCurrentAudioDevice();

        auto fill = [] (juce::ComboBox& box, const juce::StringArray& names, int selectedIndex)
        {
            if (box.getNumItems() == names.size() && box.getSelectedId() == selectedIndex + 1)
                return;

            box.clear (juce::dontSendNotification);
            for (int i = 0; i < names.size(); ++i)
                box.addItem (juce::String (i + 1) + "  -  " + names[i], i + 1);

            if (selectedIndex >= 0 && selectedIndex < names.size())
                box.setSelectedId (selectedIndex + 1, juce::dontSendNotification);
        };

        const juce::StringArray outputs = device != nullptr ? device->getOutputChannelNames() : juce::StringArray {};
        const juce::StringArray inputs = device != nullptr ? device->getInputChannelNames() : juce::StringArray {};

        fill (speechBox_, outputs, state_.rig.config.speechOutput.index);
        fill (micBox_, inputs, state_.rig.config.listeningMicInput.index);
        fill (returnBox_, inputs, state_.rig.config.vocalReturnInput.index);
    }

    void timerCallback() override { refresh(); }

    AppState& state_;
    juce::AudioDeviceSelectorComponent deviceSelector_ { state_.audio.deviceManager(), 1, 64, 1, 64, false, false, false, false };

    juce::Label nameLabel_, speechLabel_, micLabel_, returnLabel_;
    juce::TextEditor nameEditor_;
    juce::ComboBox speechBox_, micBox_, returnBox_;
    juce::TextButton applyButton_;
    juce::TextEditor statusBox_;
};

// ===========================================================================
class ConsolePanel final : public juce::Component, private juce::Timer
{
public:
    explicit ConsolePanel (AppState& state) : state_ (state)
    {
        styleLabel (ipLabel_, "Mixer IP");
        addAndMakeVisible (ipLabel_);
        addAndMakeVisible (ipEditor_);
        ipEditor_.setText (state_.rig.config.consoleAddress, juce::dontSendNotification);
        ipEditor_.onTextChange = [this]
        {
            state_.rig.config.consoleAddress = ipEditor_.getText().trim().toStdString();
            state_.wiringChanged();
        };

        styleLabel (portLabel_, "OSC port");
        addAndMakeVisible (portLabel_);
        addAndMakeVisible (portEditor_);
        portEditor_.setText (juce::String (state_.rig.config.consolePort), juce::dontSendNotification);
        portEditor_.onTextChange = [this]
        {
            state_.rig.config.consolePort = portEditor_.getText().getIntValue();
            state_.wiringChanged();
        };

        styleLabel (channelLabel_, "Vocal Channel");
        addAndMakeVisible (channelLabel_);
        addAndMakeVisible (channelEditor_);
        channelEditor_.setText (juce::String (state_.rig.config.vocalChannel.number), juce::dontSendNotification);
        channelEditor_.onTextChange = [this]
        {
            state_.rig.config.vocalChannel.number = channelEditor_.getText().getIntValue();
            state_.wiringChanged();
        };

        addAndMakeVisible (connectButton_);
        connectButton_.setButtonText ("Connect");
        connectButton_.onClick = [this] { connect(); };

        addAndMakeVisible (discoverButton_);
        discoverButton_.setButtonText ("Discover addresses");
        discoverButton_.onClick = [this] { discover(); };

        makeLogBox (logBox_);
        addAndMakeVisible (logBox_);
        logBox_.setText ("Discovery queries the console read-only. Nothing is written, so no fader moves.\n\n"
                         "It works out which OSC addresses this firmware answers on. What it cannot tell you\n"
                         "is what units they are in - the routing self test settles that.", false);

        startTimerHz (2);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12);
        auto row = [&area] (juce::Label& l, juce::Component& c)
        {
            auto r = area.removeFromTop (kRowHeight);
            l.setBounds (r.removeFromLeft (kLabelWidth));
            c.setBounds (r.removeFromLeft (220));
            area.removeFromTop (6);
        };

        row (ipLabel_, ipEditor_);
        row (portLabel_, portEditor_);
        row (channelLabel_, channelEditor_);

        auto buttons = area.removeFromTop (32);
        connectButton_.setBounds (buttons.removeFromLeft (150));
        buttons.removeFromLeft (10);
        discoverButton_.setBounds (buttons.removeFromLeft (200));
        area.removeFromTop (10);

        logBox_.setBounds (area);
    }

private:
    void connect()
    {
        state_.console.disconnect();
        const auto host = juce::String (state_.rig.config.consoleAddress);
        if (! state_.console.connect (host, state_.rig.config.consolePort))
        {
            logBox_.setText ("Could not open a socket to " + host + ":"
                             + juce::String (state_.rig.config.consolePort), false);
            return;
        }
        logBox_.setText ("Socket open to " + host + ":" + juce::String (state_.rig.config.consolePort)
                         + "\nThis does not mean the console is there - UDP has nothing to connect to.\n"
                           "Run discovery to find out whether it answers.", false);
    }

    void discover()
    {
        if (! state_.console.isConnected())
        {
            logBox_.setText ("Connect first.", false);
            return;
        }

        discoverButton_.setEnabled (false);
        logBox_.setText ("Querying...", false);

        const int channel = state_.rig.config.vocalChannel.number;
        const int bus = state_.rig.config.sendBus;

        // Discovery blocks on replies that may never arrive, so it runs off the
        // message thread. A frozen UI during setup would be merely annoying; the
        // habit of blocking the message thread is what later turns into a
        // watchdog abort mid-run.
        juce::Thread::launch ([this, channel, bus]
        {
            auto report = state_.console.discover (channel, bus);
            juce::MessageManager::callAsync ([this, report]
            {
                discoverButton_.setEnabled (true);
                logBox_.setText (report.summary, false);
                if (report.succeeded)
                {
                    state_.rig.addresses = report.addresses;
                    state_.wiringChanged();
                }
            });
        });
    }

    void timerCallback() override
    {
        const bool connected = state_.console.isConnected();
        connectButton_.setButtonText (connected ? "Reconnect" : "Connect");
    }

    AppState& state_;
    juce::Label ipLabel_, portLabel_, channelLabel_;
    juce::TextEditor ipEditor_, portEditor_, channelEditor_;
    juce::TextButton connectButton_, discoverButton_;
    juce::TextEditor logBox_;
};


// ===========================================================================
class SpeechPanel final : public juce::Component, private juce::Timer
{
public:
    explicit SpeechPanel (AppState& state) : state_ (state)
    {
        styleLabel (heading_, "Speech material");
        heading_.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        addAndMakeVisible (heading_);

        styleLabel (explain_,
                    "The rig fetches its own speech from LibriVox, which is public domain.");
        addAndMakeVisible (explain_);

        addAndMakeVisible (fetchButton_);
        fetchButton_.setButtonText ("Start fetching");
        fetchButton_.onClick = [this] { toggleFetching(); };

        addAndMakeVisible (openLicenceButton_);
        openLicenceButton_.setButtonText ("Open-licensed material only");
        openLicenceButton_.setToggleState (state_.rig.speechPolicy.requireOpenLicence,
                                           juce::dontSendNotification);
        openLicenceButton_.onClick = [this]
        {
            state_.rig.speechPolicy.requireOpenLicence = openLicenceButton_.getToggleState();
            state_.applySpeechSettings();
            state_.refreshPlaylist();
            state_.save();
        };

        styleLabel (cacheLabel_, "Cache limit (GB)");
        addAndMakeVisible (cacheLabel_);
        addAndMakeVisible (cacheEditor_);
        cacheEditor_.setText (juce::String (state_.rig.speechPolicy.maxBytes
                                            / (1024.0 * 1024.0 * 1024.0), 1),
                              juce::dontSendNotification);
        cacheEditor_.onTextChange = [this]
        {
            const double gb = juce::jlimit (0.5, 500.0, cacheEditor_.getText().getDoubleValue());
            state_.rig.speechPolicy.maxBytes = static_cast<long long> (gb * 1024.0 * 1024.0 * 1024.0);
            state_.applySpeechSettings();
            state_.save();
        };

        styleLabel (bankedLabel_, "Hours needed before an unattended run");
        addAndMakeVisible (bankedLabel_);
        addAndMakeVisible (bankedEditor_);
        bankedEditor_.setText (juce::String (state_.rig.speechPolicy.minBufferedSecondsForUnattended / 3600.0, 1),
                               juce::dontSendNotification);
        bankedEditor_.onTextChange = [this]
        {
            const double hours = juce::jlimit (0.1, 200.0, bankedEditor_.getText().getDoubleValue());
            state_.rig.speechPolicy.minBufferedSecondsForUnattended = hours * 3600.0;
            state_.applySpeechSettings();
            state_.save();
        };

        addAndMakeVisible (progress_);

        styleLabel (extraHeading_, "Additional material");
        extraHeading_.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        addAndMakeVisible (extraHeading_);

        addAndMakeVisible (folderButton_);
        folderButton_.setButtonText (state_.rig.speechFolder.isEmpty()
                                         ? "Add a local folder..."
                                         : state_.rig.speechFolder);
        folderButton_.onClick = [this] { chooseFolder(); };

        addAndMakeVisible (clearFolderButton_);
        clearFolderButton_.setButtonText ("Clear");
        clearFolderButton_.onClick = [this]
        {
            state_.rig.speechFolder = {};
            folderButton_.setButtonText ("Add a local folder...");
            state_.refreshPlaylist();
            state_.save();
        };

        styleLabel (feedLabel_, "Podcast / RSS feed URL");
        addAndMakeVisible (feedLabel_);
        addAndMakeVisible (feedEditor_);
        addAndMakeVisible (addFeedButton_);
        addFeedButton_.setButtonText ("Add feed");
        addFeedButton_.onClick = [this] { addFeed(); };

        makeLogBox (statusBox_);
        addAndMakeVisible (statusBox_);

        startTimerHz (2);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12);

        heading_.setBounds (area.removeFromTop (24));
        explain_.setBounds (area.removeFromTop (22));
        area.removeFromTop (6);

        auto top = area.removeFromTop (32);
        fetchButton_.setBounds (top.removeFromLeft (170));
        top.removeFromLeft (12);
        progress_.setBounds (top.removeFromLeft (280).reduced (0, 6));
        area.removeFromTop (8);

        openLicenceButton_.setBounds (area.removeFromTop (kRowHeight));
        area.removeFromTop (6);

        auto row = [&area] (juce::Label& l, juce::Component& c)
        {
            auto r = area.removeFromTop (kRowHeight);
            l.setBounds (r.removeFromLeft (280));
            c.setBounds (r.removeFromLeft (110));
            area.removeFromTop (6);
        };
        row (cacheLabel_, cacheEditor_);
        row (bankedLabel_, bankedEditor_);

        area.removeFromTop (12);
        extraHeading_.setBounds (area.removeFromTop (24));
        area.removeFromTop (4);

        auto folderRow = area.removeFromTop (kRowHeight);
        clearFolderButton_.setBounds (folderRow.removeFromRight (80));
        folderRow.removeFromRight (8);
        folderButton_.setBounds (folderRow);
        area.removeFromTop (6);

        auto feedRow = area.removeFromTop (kRowHeight);
        feedLabel_.setBounds (feedRow.removeFromLeft (200));
        addFeedButton_.setBounds (feedRow.removeFromRight (100));
        feedRow.removeFromRight (8);
        feedEditor_.setBounds (feedRow);
        area.removeFromTop (10);

        statusBox_.setBounds (area);
    }

private:
    void toggleFetching()
    {
        if (state_.speech.status().running)
        {
            state_.speech.stopFetching();
            return;
        }

        state_.applySpeechSettings();
        state_.speech.startFetching();
    }

    void chooseFolder()
    {
        chooser_ = std::make_unique<juce::FileChooser> ("Folder of speech recordings");
        chooser_->launchAsync (juce::FileBrowserComponent::openMode
                                   | juce::FileBrowserComponent::canSelectDirectories,
                               [this] (const juce::FileChooser& fc)
                               {
                                   const auto folder = fc.getResult();
                                   if (! folder.isDirectory())
                                       return;

                                   state_.rig.speechFolder = folder.getFullPathName();
                                   folderButton_.setButtonText (state_.rig.speechFolder);
                                   state_.refreshPlaylist();
                                   state_.save();
                               });
    }

    void addFeed()
    {
        const auto url = feedEditor_.getText().trim();
        if (url.isEmpty())
            return;

        if (! isHttps (url.toStdString()))
        {
            statusBox_.setText ("Feeds must be https. A plaintext feed is refused because its "
                                "contents get downloaded and decoded on this machine.", false);
            return;
        }

        SpeechSource src;
        src.enabled = true;
        src.kind = SpeechSourceKind::podcastFeed;
        src.name = hostOf (url.toStdString());
        src.feedUrl = url;
        // Anything from a feed is unknown-licence by definition. With the open
        // licence requirement on - which is the default - it will be fetched and
        // catalogued but never played, and the panel says so rather than leaving
        // it looking as though the feed did nothing.
        src.licence = LicenceClass::unknown;

        state_.rig.speechSources.add (src);
        state_.applySpeechSettings();
        state_.save();
        feedEditor_.clear();
    }

    void timerCallback() override
    {
        const auto s = state_.speech.status();

        fetchButton_.setButtonText (s.running ? "Stop fetching" : "Start fetching");
        progressValue_ = s.targetSeconds > 0.0
                             ? juce::jlimit (0.0, 1.0, s.playableSeconds / s.targetSeconds)
                             : 0.0;
        progress_.setTextToDisplay (juce::String (s.playableSeconds / 3600.0, 1) + " h banked");

        // The playlist only changes when the fetcher lands something, so rebuild
        // it on a change in the item count rather than every tick.
        if (s.itemCount != lastItemCount_)
        {
            lastItemCount_ = s.itemCount;
            state_.refreshPlaylist();
        }

        // Record what is actually being played, which is what the eviction order
        // is built from.
        const auto playing = state_.audio.currentSpeechPath();
        if (playing != lastPlayed_ && playing.existsAsFile())
        {
            lastPlayed_ = playing;
            state_.speech.notePlayed (playing);
        }

        const auto manifest = state_.speech.manifest();
        int pd = 0, attribution = 0, unknown = 0;
        for (const auto& i : manifest.items())
        {
            switch (i.licence)
            {
                case LicenceClass::publicDomain:          ++pd; break;
                case LicenceClass::permissiveAttribution: ++attribution; break;
                case LicenceClass::unknown:               ++unknown; break;
            }
        }

        juce::StringArray lines;
        lines.add (s.step.isNotEmpty() ? s.step : juce::String ("Idle."));
        lines.add ("");
        lines.add ("Banked and playable: " + juce::String (s.playableSeconds / 3600.0, 2)
                   + " h of " + juce::String (s.targetSeconds / 3600.0, 1) + " h target");
        lines.add ("Cache:               " + juce::String (s.cacheBytes / (1024.0 * 1024.0 * 1024.0), 2)
                   + " GB of " + juce::String (state_.rig.speechPolicy.maxBytes / (1024.0 * 1024.0 * 1024.0), 1)
                   + " GB, " + juce::String (s.itemCount) + " files");
        lines.add ("Fetched this session: " + juce::String (s.downloadedThisSession)
                   + " kept, " + juce::String (s.rejectedThisSession) + " rejected");
        lines.add ("");
        lines.add ("Licences:  public domain " + juce::String (pd)
                   + "   attribution " + juce::String (attribution)
                   + "   unknown " + juce::String (unknown));

        if (unknown > 0 && state_.rig.speechPolicy.requireOpenLicence)
            lines.add ("           " + juce::String (unknown)
                       + " unknown-licence files are cached but will not be played,");
        if (unknown > 0 && state_.rig.speechPolicy.requireOpenLicence)
            lines.add ("           because \"open-licensed material only\" is on.");

        lines.add ("");
        lines.add (s.readyForUnattended
                       ? "Enough banked for an unattended run."
                       : "Not enough banked for an unattended run yet.");

        if (s.lastError.isNotEmpty())
        {
            lines.add ("");
            lines.add ("Last problem: " + s.lastError);
        }

        lines.add ("");
        lines.add ("Note: LibriVox is read speech. It does not contain the material that most");
        lines.add ("often causes a false detection - sustained vowels and sung notes with vibrato.");
        lines.add ("Record those yourself and add them as a local folder.");

        statusBox_.setText (lines.joinIntoString ("\n"), false);
    }

    AppState& state_;
    juce::Label heading_, explain_, cacheLabel_, bankedLabel_, extraHeading_, feedLabel_;
    juce::TextEditor cacheEditor_, bankedEditor_, feedEditor_;
    juce::TextButton fetchButton_, folderButton_, clearFolderButton_, addFeedButton_;
    juce::ToggleButton openLicenceButton_;
    double progressValue_ { 0.0 };
    juce::ProgressBar progress_ { progressValue_ };
    juce::TextEditor statusBox_;
    std::unique_ptr<juce::FileChooser> chooser_;
    int lastItemCount_ { -1 };
    juce::File lastPlayed_;
};

// ===========================================================================
class CheckPanel final : public juce::Component, private juce::Timer
{
public:
    explicit CheckPanel (AppState& state) : state_ (state)
    {
        styleLabel (calHeading_, "Measurement microphone calibration");
        calHeading_.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        addAndMakeVisible (calHeading_);

        styleLabel (calExplain_,
                    "Present an acoustic calibrator to the capsule, then press Measure.");
        addAndMakeVisible (calExplain_);

        styleLabel (refLabel_, "Calibrator level (dB SPL)");
        addAndMakeVisible (refLabel_);
        addAndMakeVisible (refEditor_);
        refEditor_.setText (juce::String (state_.rig.config.micCalibration.referenceSplDb, 1),
                            juce::dontSendNotification);

        addAndMakeVisible (measureButton_);
        measureButton_.setButtonText ("Measure calibrator");
        measureButton_.onClick = [this] { measureCalibrator(); };

        styleLabel (calResult_, "");
        addAndMakeVisible (calResult_);

        styleLabel (testHeading_, "Routing self test");
        testHeading_.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        addAndMakeVisible (testHeading_);

        addAndMakeVisible (selfTestButton_);
        selfTestButton_.setButtonText ("Run routing self test");
        selfTestButton_.onClick = [this] { runSelfTest(); };

        addAndMakeVisible (progress_);

        makeLogBox (logBox_);
        addAndMakeVisible (logBox_);
        logBox_.setText (
            "The self test plays a quiet band-limited signal, then raises the assigned vocal\n"
            "fader by 6 dB from a low reference position and checks the room level follows.\n\n"
            "That last step is what proves the configured Vocal Channel really is the channel\n"
            "the microphone is on. If it is not, the rig would raise gain, see no feedback,\n"
            "and conclude the room has enormous margin.", false);

        startTimerHz (10);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12);

        calHeading_.setBounds (area.removeFromTop (24));
        calExplain_.setBounds (area.removeFromTop (22));
        area.removeFromTop (4);

        auto r = area.removeFromTop (kRowHeight);
        refLabel_.setBounds (r.removeFromLeft (kLabelWidth));
        refEditor_.setBounds (r.removeFromLeft (100));
        r.removeFromLeft (10);
        measureButton_.setBounds (r.removeFromLeft (200));
        area.removeFromTop (6);

        calResult_.setBounds (area.removeFromTop (24));
        area.removeFromTop (14);

        testHeading_.setBounds (area.removeFromTop (24));
        area.removeFromTop (4);

        auto t = area.removeFromTop (32);
        selfTestButton_.setBounds (t.removeFromLeft (220));
        t.removeFromLeft (10);
        progress_.setBounds (t.removeFromLeft (260).reduced (0, 6));
        area.removeFromTop (10);

        logBox_.setBounds (area);
    }

private:
    void measureCalibrator()
    {
        if (! state_.audio.isRunning())
        {
            calResult_.setText ("The audio device is not running.", juce::dontSendNotification);
            return;
        }

        measureButton_.setEnabled (false);
        state_.audio.beginMeasurement();

        // Two seconds, on a background thread, so the message thread stays
        // responsive and the measurement is not sliced by UI work.
        juce::Thread::launch ([this]
        {
            juce::Thread::sleep (2000);
            const auto measured = state_.audio.endMeasurement();
            juce::MessageManager::callAsync ([this, measured]
            {
                measureButton_.setEnabled (true);
                if (! measured.valid)
                {
                    calResult_.setText ("No audio was captured.", juce::dontSendNotification);
                    return;
                }

                const float reference = refEditor_.getText().getFloatValue();
                const auto cal = MicCalibration::fromCalibrator (reference, measured.micDbFS);

                if (! cal.valid)
                {
                    calResult_.setText ("Measured " + juce::String (measured.micDbFS, 1)
                                            + " dBFS, which implies an impossible chain. Check the input and the calibrator.",
                                        juce::dontSendNotification);
                    return;
                }

                state_.rig.config.micCalibration = cal;
                state_.save();
                calResult_.setText ("Measured " + juce::String (measured.micDbFS, 1)
                                        + " dBFS. Full scale is " + juce::String (cal.splAtFullScaleDb, 1)
                                        + " dB SPL.",
                                    juce::dontSendNotification);
                if (state_.onChanged)
                    state_.onChanged();
            });
        });
    }

    void runSelfTest()
    {
        if (state_.selfTest == nullptr)
            state_.selfTest = std::make_unique<RoutingSelfTest> (state_.audio, state_.console);

        state_.selfTest->onFinished = [this] (const RoutingVerdict& verdict, const juce::String& log)
        {
            selfTestButton_.setEnabled (true);
            logBox_.setText (log, false);

            state_.rig.selfTestPassed = verdict.passed;
            state_.rig.selfTestSignature = verdict.passed ? wiringSignature (state_.rig.config)
                                                          : juce::String {};
            state_.save();
            if (state_.onChanged)
                state_.onChanged();
        };

        selfTestButton_.setEnabled (false);
        state_.selfTest->startTest (state_.rig.config, state_.rig.startFaderDb);
    }

    void timerCallback() override
    {
        if (state_.selfTest != nullptr && state_.selfTest->isThreadRunning())
        {
            // ProgressBar reads the double it was constructed with, so the value
            // is set on the variable rather than on the component.
            progressValue_ = static_cast<double> (state_.selfTest->progress());
            progress_.setTextToDisplay (state_.selfTest->currentStep());
        }

        const auto& cal = state_.rig.config.micCalibration;
        if (cal.valid && ! calResult_.getText().isNotEmpty())
            calResult_.setText ("Calibrated: full scale is " + juce::String (cal.splAtFullScaleDb, 1) + " dB SPL.",
                                juce::dontSendNotification);
    }

    AppState& state_;
    juce::Label calHeading_, calExplain_, refLabel_, calResult_, testHeading_;
    juce::TextEditor refEditor_;
    juce::TextButton measureButton_, selfTestButton_;
    double progressValue_ { 0.0 };
    juce::ProgressBar progress_ { progressValue_ };
    juce::TextEditor logBox_;
};

// ===========================================================================
class RunPanel final : public juce::Component, private juce::Timer
{
public:
    explicit RunPanel (AppState& state) : state_ (state)
    {
        addAndMakeVisible (startButton_);
        startButton_.setButtonText ("Start run");
        startButton_.onClick = [this] { toggleRun(); };

        addAndMakeVisible (panicButton_);
        panicButton_.setButtonText ("STOP");
        panicButton_.setColour (juce::TextButton::buttonColourId, juce::Colours::darkred);
        panicButton_.onClick = [this] { stopRun(); };

        styleLabel (startFaderLabel_, "Start fader position (dB)");
        addAndMakeVisible (startFaderLabel_);
        addAndMakeVisible (startFaderEditor_);
        startFaderEditor_.setText (juce::String (state_.rig.startFaderDb, 1), juce::dontSendNotification);
        startFaderEditor_.onTextChange = [this]
        {
            state_.rig.startFaderDb = startFaderEditor_.getText().getFloatValue();
            state_.save();
        };

        makeLogBox (statusBox_);
        addAndMakeVisible (statusBox_);

        makeLogBox (blockersBox_);
        addAndMakeVisible (blockersBox_);

        startTimerHz (10);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced (12);

        auto r = area.removeFromTop (kRowHeight);
        startFaderLabel_.setBounds (r.removeFromLeft (kLabelWidth));
        startFaderEditor_.setBounds (r.removeFromLeft (100));
        area.removeFromTop (8);

        auto buttons = area.removeFromTop (40);
        startButton_.setBounds (buttons.removeFromLeft (180));
        buttons.removeFromLeft (12);
        panicButton_.setBounds (buttons.removeFromLeft (120));
        area.removeFromTop (10);

        blockersBox_.setBounds (area.removeFromBottom (juce::jmax (110, area.getHeight() / 2)));
        area.removeFromBottom (8);
        statusBox_.setBounds (area);
    }

private:
    juce::StringArray blockers() const
    {
        juce::StringArray out;

        for (const auto& issue : validate (state_.rig.config))
            if (issue.severity == ConfigIssue::Severity::error)
                out.add ("- " + juce::String (issue.message));

        if (! state_.audio.isRunning())
            out.add ("- The audio device is not running. Open it on the Rig panel.");

        if (! state_.console.isConnected() || ! state_.console.resolved().valid)
            out.add ("- The console's addresses have not been resolved. Run discovery on the Console panel.");

        if (! state_.rig.config.micCalibration.valid)
            out.add ("- The measurement microphone is not calibrated, so no acoustic limit can be enforced.");

        if (! state_.rig.selfTestPassed)
            out.add ("- The routing self test has not passed for this wiring. Run it on the Check panel.");

        // A run with nothing to say measures nothing. Gated on hours banked
        // rather than on the network being up, because the network going away
        // mid-run is survivable and starting with an empty cache is not.
        const auto speech = state_.speech.status();
        if (! speech.readyForUnattended)
            out.add ("- Only " + juce::String (speech.playableSeconds / 3600.0, 1)
                     + " h of speech is banked, and this rig wants "
                     + juce::String (state_.rig.speechPolicy.minBufferedSecondsForUnattended / 3600.0, 1)
                     + " h before an unattended run. Fetch more on the Speech panel.");

        return out;
    }

    void toggleRun()
    {
        if (state_.run != nullptr && state_.run->isRunning())
        {
            stopRun();
            return;
        }

        if (state_.run == nullptr)
            state_.run = std::make_unique<RunController> (state_.audio, state_.console);

        juce::String error;
        if (! state_.run->start (state_.rig.config, state_.rig.startFaderDb, error))
        {
            statusBox_.setText ("Could not start.\n\n" + error, false);
            return;
        }

        state_.audio.setSpeechPlaying (true);
    }

    void stopRun()
    {
        if (state_.run != nullptr)
            state_.run->stop();
        state_.audio.setSpeechPlaying (false);
    }

    void timerCallback() override
    {
        const auto list = blockers();
        const bool running = state_.run != nullptr && state_.run->isRunning();

        startButton_.setEnabled (running || list.isEmpty());
        startButton_.setButtonText (running ? "Stop run" : "Start run");

        blockersBox_.setText (list.isEmpty()
                                  ? "Ready to run."
                                  : "Not ready:\n" + list.joinIntoString ("\n"),
                              false);

        if (! running)
        {
            const auto levels = state_.audio.levels();
            const auto& cal = state_.rig.config.micCalibration;
            juce::StringArray lines;
            lines.add ("Not running.");
            lines.add ("");
            lines.add ("Listening mic: " + juce::String (levels.micPeakDbFS, 1) + " dBFS"
                       + (cal.valid ? "   (" + juce::String (cal.toSplDb (levels.micPeakDbFS), 1) + " dB SPL)"
                                    : "   (uncalibrated)"));
            lines.add ("Vocal return:  " + juce::String (levels.vocalPeakDbFS, 1) + " dBFS");
            statusBox_.setText (lines.joinIntoString ("\n"), false);
            return;
        }

        const auto s = state_.run->status();
        juce::StringArray lines;
        lines.add ("State:       " + juce::String (toString (s.state)));
        lines.add ("Elapsed:     " + juce::RelativeTime::milliseconds (s.elapsedMs).getDescription());
        lines.add ("");
        lines.add ("Gain:        commanded " + juce::String (s.commandedGainDb, 2)
                   + " dB, confirmed " + juce::String (s.confirmedGainDb, 2)
                   + " dB, permitted " + juce::String (s.permittedGainDb, 2) + " dB");
        lines.add ("Margin:      " + juce::String (s.marginDb, 2) + " dB below the predicted instability point");
        lines.add ("Room level:  " + juce::String (s.measuredSplDb, 1) + " dB SPL"
                   + "  (warn " + juce::String (state_.rig.config.limits.warnSplDb, 0)
                   + ", ceiling " + juce::String (state_.rig.config.limits.ceilingSplDb, 0) + ")");
        lines.add ("Thermal:     " + juce::String (s.thermalFraction * 100.0f, 0) + "% of budget");
        lines.add ("");
        lines.add ("Console:     " + juce::String (s.consoleConnected ? "answering" : "SILENT"));
        lines.add ("Audio:       " + juce::String (s.audioRunning ? "running" : "STOPPED"));
        if (s.note.isNotEmpty())
            lines.add ("Note:        " + s.note);

        statusBox_.setText (lines.joinIntoString ("\n"), false);
    }

    AppState& state_;
    juce::Label startFaderLabel_;
    juce::TextEditor startFaderEditor_;
    juce::TextButton startButton_, panicButton_;
    juce::TextEditor statusBox_, blockersBox_;
};

// ===========================================================================
MainComponent::MainComponent()
{
    juce::String error;
    loadRig (state_.rig, state_.rigFile, error);

    if (state_.rig.addresses.valid)
        state_.console.setResolved (state_.rig.addresses);

    if (state_.rig.speechSources.isEmpty())
        state_.rig.speechSources = defaultSpeechSources();
    if (state_.rig.speechCacheFolder.isEmpty())
        state_.rig.speechCacheFolder = defaultSpeechCacheFolder().getFullPathName();

    state_.applySpeechSettings();
    state_.speech.loadLibrary();
    state_.refreshPlaylist();

    // A landed download makes the playlist stale, so rebuild it when the fetcher
    // says so rather than polling the disk.
    state_.speech.onLibraryChanged = [this] { state_.refreshPlaylist(); };

    const auto background = juce::Colours::darkgrey.darker (0.6f);
    addAndMakeVisible (tabs_);
    tabs_.addTab ("Rig", background, new RigPanel (state_), true);
    tabs_.addTab ("Console", background, new ConsolePanel (state_), true);
    tabs_.addTab ("Speech", background, new SpeechPanel (state_), true);
    tabs_.addTab ("Check", background, new CheckPanel (state_), true);
    tabs_.addTab ("Run", background, new RunPanel (state_), true);

    setSize (960, 700);
}

MainComponent::~MainComponent()
{
    if (state_.run != nullptr)
        state_.run->stop();
    state_.speech.onLibraryChanged = nullptr;
    state_.speech.stopFetching();
    state_.speech.saveLibrary();
    state_.audio.stop();
    state_.save();
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (getLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId));
}

void MainComponent::resized()
{
    tabs_.setBounds (getLocalBounds());
}
} // namespace fbkt
