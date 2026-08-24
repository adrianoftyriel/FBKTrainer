#include "AudioEngine.h"

#include <cmath>

namespace fbkt
{
namespace
{
// Peak meter release. Fast enough to follow a level coming down, slow enough
// that the ceiling check is not fooled by the gap between two syllables.
constexpr float kPeakReleasePerSecond = 12.0f;   // dB per second, as a ratio below

// Band edges for the self-test signal.
constexpr float kTestHighpassHz = 300.0f;
constexpr float kTestLowpassHz = 4000.0f;

// A one-pole-per-stage biquad, designed directly. Used for meters and the test
// signal only - nothing here is in the path of anything that ships.
struct Biquad
{
    float b0 { 1.0f }, b1 { 0.0f }, b2 { 0.0f }, a1 { 0.0f }, a2 { 0.0f };

    void setHighpass (double sampleRate, double freq, double q = 0.7071)
    {
        const double w = 2.0 * juce::MathConstants<double>::pi * freq / sampleRate;
        const double cw = std::cos (w), sw = std::sin (w);
        const double alpha = sw / (2.0 * q);
        const double a0 = 1.0 + alpha;
        b0 = static_cast<float> (((1.0 + cw) * 0.5) / a0);
        b1 = static_cast<float> ((-(1.0 + cw)) / a0);
        b2 = b0;
        a1 = static_cast<float> ((-2.0 * cw) / a0);
        a2 = static_cast<float> ((1.0 - alpha) / a0);
    }

    void setLowpass (double sampleRate, double freq, double q = 0.7071)
    {
        const double w = 2.0 * juce::MathConstants<double>::pi * freq / sampleRate;
        const double cw = std::cos (w), sw = std::sin (w);
        const double alpha = sw / (2.0 * q);
        const double a0 = 1.0 + alpha;
        b0 = static_cast<float> (((1.0 - cw) * 0.5) / a0);
        b1 = static_cast<float> ((1.0 - cw) / a0);
        b2 = b0;
        a1 = static_cast<float> ((-2.0 * cw) / a0);
        a2 = static_cast<float> ((1.0 - alpha) / a0);
    }

    float process (float x, float* z) const noexcept
    {
        const float y = b0 * x + z[0];
        z[0] = b1 * x - a1 * y + z[1];
        z[1] = b2 * x - a2 * y;
        return y;
    }
};

Biquad gTestHp, gTestLp;
} // namespace

AudioEngine::AudioEngine()
{
    formatManager_.registerBasicFormats();
}

AudioEngine::~AudioEngine()
{
    stop();
}

// ---------------------------------------------------------------------------
bool AudioEngine::start (const RigConfig& config, juce::String& errorOut)
{
    stop();
    config_ = config;

    // Ask for exactly the channels the rig uses and no more. Opening every
    // channel a 48-in card offers would work, but it makes the index the operator
    // chose in the UI and the index the callback receives two different things
    // the moment a driver reorders anything.
    const int maxInput = std::max (config.listeningMicInput.index,
                                   config.vocalReturnInput.isValid() ? config.vocalReturnInput.index : 0);
    const int maxOutput = config.speechOutput.index;

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.sampleRate = config.sampleRate;
    setup.bufferSize = 256;
    setup.inputDeviceName = config.audioDeviceName;
    setup.outputDeviceName = config.audioDeviceName;
    setup.useDefaultInputChannels = false;
    setup.useDefaultOutputChannels = false;
    setup.inputChannels.clear();
    setup.inputChannels.setRange (0, maxInput + 1, true);
    setup.outputChannels.clear();
    setup.outputChannels.setRange (0, maxOutput + 1, true);

    errorOut = deviceManager_.initialise (maxInput + 1, maxOutput + 1, nullptr, false, {}, &setup);
    if (errorOut.isNotEmpty())
        return false;

    auto* device = deviceManager_.getCurrentAudioDevice();
    if (device == nullptr)
    {
        errorOut = "The audio device did not open.";
        return false;
    }

    if (device->getActiveInputChannels().getHighestBit() < maxInput)
    {
        errorOut = "The device opened with fewer input channels than the rig needs. "
                   "Listening mic input " + juce::String (config.listeningMicInput.index + 1)
                   + " is not available.";
        return false;
    }

    if (device->getActiveOutputChannels().getHighestBit() < maxOutput)
    {
        errorOut = "The device opened with fewer output channels than the rig needs. "
                   "Speech output " + juce::String (config.speechOutput.index + 1)
                   + " is not available.";
        return false;
    }

    sampleRate_.store (device->getCurrentSampleRate());
    prepareHighpass (device->getCurrentSampleRate());
    gTestHp.setHighpass (device->getCurrentSampleRate(), kTestHighpassHz);
    gTestLp.setLowpass (device->getCurrentSampleRate(), kTestLowpassHz);

    deviceManager_.addAudioCallback (this);
    running_.store (true);
    return true;
}

void AudioEngine::stop()
{
    if (! running_.exchange (false))
        return;

    deviceManager_.removeAudioCallback (this);
    deviceManager_.closeAudioDevice();

    const juce::ScopedLock lock (speechLock_);
    speechResampler_.reset();
    speechSource_.reset();
}

void AudioEngine::prepareHighpass (double sr)
{
    Biquad hp;
    hp.setHighpass (sr, static_cast<double> (config_.limits.thermalBandLowHz));
    hfB0_ = hp.b0; hfB1_ = hp.b1; hfB2_ = hp.b2; hfA1_ = hp.a1; hfA2_ = hp.a2;
    hfState_[0] = hfState_[1] = 0.0f;
}

// ---------------------------------------------------------------------------
void AudioEngine::setSpeechPlaylist (const juce::Array<juce::File>& files)
{
    const juce::ScopedLock lock (speechLock_);
    speechFiles_ = files;
    speechIndex_ = 0;
    // Do not tear down whatever is currently playing: a playlist refresh happens
    // every time the fetcher lands a new file, and dropping the current source
    // for that would put a gap in the speech every few minutes.
    if (speechResampler_ == nullptr)
        speechFileEnded_.store (true);
}

void AudioEngine::setSpeechFolder (const juce::File& folder)
{
    juce::Array<juce::File> found;
    if (folder.isDirectory())
    {
        for (const auto& entry : juce::RangedDirectoryIterator (folder, true, "*.wav;*.flac;*.aiff;*.aif;*.mp3;*.ogg"))
            found.add (entry.getFile());
    }
    found.sort();
    setSpeechPlaylist (found);
}

int AudioEngine::speechFileCount() const
{
    const juce::ScopedLock lock (speechLock_);
    return speechFiles_.size();
}

juce::String AudioEngine::currentSpeechFile() const
{
    const juce::ScopedLock lock (speechLock_);
    return currentSpeechName_;
}

juce::File AudioEngine::currentSpeechPath() const
{
    const juce::ScopedLock lock (speechLock_);
    return currentSpeechPath_;
}

void AudioEngine::setSpeechPlaying (bool shouldPlay)
{
    speechPlaying_.store (shouldPlay);
}

void AudioEngine::advanceSpeechFile()
{
    // Called from the audio thread only when the current file has run out, and it
    // does allocate - opening a reader is not a real-time operation. That is
    // acceptable here and nowhere else in the program: a glitch at a file
    // boundary costs one block of training audio, whereas the alternatives
    // (preloading hours of speech, or a second thread with a handover) buy
    // nothing that matters for material that is deliberately arbitrary.
    const juce::ScopedLock lock (speechLock_);

    if (speechFiles_.isEmpty())
    {
        speechResampler_.reset();
        speechSource_.reset();
        currentSpeechName_ = {};
        currentSpeechPath_ = juce::File();
        return;
    }

    speechIndex_ = (speechIndex_ + 1) % speechFiles_.size();
    const auto file = speechFiles_[speechIndex_];

    speechResampler_.reset();
    speechSource_.reset();

    if (auto* reader = formatManager_.createReaderFor (file))
    {
        speechSource_ = std::make_unique<juce::AudioFormatReaderSource> (reader, true);
        speechSource_->setLooping (false);

        const double ratio = reader->sampleRate / sampleRate_.load();
        speechResampler_ = std::make_unique<juce::ResamplingAudioSource> (speechSource_.get(), false, 1);
        speechResampler_->setResamplingRatio (ratio);
        speechResampler_->prepareToPlay (512, sampleRate_.load());
        currentSpeechName_ = file.getFileName();
        currentSpeechPath_ = file;
    }
    else
    {
        // Every fetched file was decoded once before it entered the library, so
        // this means it has been removed or damaged since. Skipping is correct;
        // the library reconciles itself with the disk separately.
        currentSpeechName_ = "(could not read " + file.getFileName() + ")";
        currentSpeechPath_ = juce::File();
    }
}

void AudioEngine::setTestSignal (TestSignal signal, float levelDbFS) noexcept
{
    testLevelDb_.store (levelDbFS);
    testSignal_.store (signal);
}

// ---------------------------------------------------------------------------
LiveLevels AudioEngine::levels() const noexcept
{
    LiveLevels l;
    l.micPeakDbFS = micPeakDbFS_.load();
    l.micHfDbFS = micHfDbFS_.load();
    l.vocalPeakDbFS = vocalPeakDbFS_.load();
    l.running = running_.load();
    return l;
}

void AudioEngine::beginMeasurement() noexcept
{
    measureMicSum_.store (0.0);
    measureVocalSum_.store (0.0);
    measureBlocks_.store (0);
    measureSamples_.store (0);
    measuring_.store (true);
}

MeasuredLevels AudioEngine::endMeasurement() noexcept
{
    measuring_.store (false);

    MeasuredLevels m;
    m.blocks = measureBlocks_.load();
    const int samples = measureSamples_.load();
    if (samples <= 0)
        return m;

    m.valid = true;
    m.micDbFS = powerToDb (static_cast<float> (measureMicSum_.load() / samples));
    m.vocalDbFS = powerToDb (static_cast<float> (measureVocalSum_.load() / samples));
    return m;
}

// ---------------------------------------------------------------------------
void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                                    int numInputChannels,
                                                    float* const* outputChannelData,
                                                    int numOutputChannels,
                                                    int numSamples,
                                                    const juce::AudioIODeviceCallbackContext&)
{
    for (int ch = 0; ch < numOutputChannels; ++ch)
        if (outputChannelData[ch] != nullptr)
            juce::FloatVectorOperations::clear (outputChannelData[ch], numSamples);

    const int outIndex = config_.speechOutput.index;
    float* out = (outIndex >= 0 && outIndex < numOutputChannels) ? outputChannelData[outIndex] : nullptr;

    // --- speech ------------------------------------------------------------
    if (out != nullptr && speechPlaying_.load())
    {
        const juce::ScopedTryLock lock (speechLock_);
        if (lock.isLocked())
        {
            if (speechResampler_ == nullptr)
            {
                speechFileEnded_.store (true);
            }
            else
            {
                if (speechScratch_.getNumSamples() < numSamples)
                    speechScratch_.setSize (1, numSamples, false, false, true);

                juce::AudioSourceChannelInfo info (&speechScratch_, 0, numSamples);
                speechResampler_->getNextAudioBlock (info);

                const float gain = dbToLinear (speechLevelDb_.load());
                juce::FloatVectorOperations::addWithMultiply (out, speechScratch_.getReadPointer (0), gain, numSamples);

                if (speechSource_ != nullptr
                    && speechSource_->getNextReadPosition() >= speechSource_->getTotalLength())
                    speechFileEnded_.store (true);
            }
        }
    }

    // --- test signal -------------------------------------------------------
    if (out != nullptr && testSignal_.load() == TestSignal::bandLimitedNoise)
    {
        const float gain = dbToLinear (testLevelDb_.load());
        for (int i = 0; i < numSamples; ++i)
        {
            float n = random_.nextFloat() * 2.0f - 1.0f;
            n = gTestHp.process (n, noiseHpZ_[0]);
            n = gTestHp.process (n, noiseHpZ_[1]);
            n = gTestLp.process (n, noiseLpZ_[0]);
            n = gTestLp.process (n, noiseLpZ_[1]);
            out[i] += n * gain;
        }
    }

    // --- input metering ----------------------------------------------------
    const int micIndex = config_.listeningMicInput.index;
    const int vocalIndex = config_.vocalReturnInput.index;

    const float* mic = (micIndex >= 0 && micIndex < numInputChannels) ? inputChannelData[micIndex] : nullptr;
    const float* vocal = (vocalIndex >= 0 && vocalIndex < numInputChannels) ? inputChannelData[vocalIndex] : nullptr;

    const float release = std::pow (10.0f,
                                    -(kPeakReleasePerSecond * static_cast<float> (numSamples)
                                      / static_cast<float> (sampleRate_.load())) / 20.0f);

    double micSum = 0.0, vocalSum = 0.0;

    if (mic != nullptr)
    {
        float peak = 0.0f;
        double hfSum = 0.0;
        for (int i = 0; i < numSamples; ++i)
        {
            const float x = mic[i];
            peak = std::max (peak, std::abs (x));
            micSum += static_cast<double> (x) * x;

            // Highpass for the thermal model. What heats a compression driver is
            // the energy above the crossover, not the level of the programme.
            const float y = hfB0_ * x + hfState_[0];
            hfState_[0] = hfB1_ * x - hfA1_ * y + hfState_[1];
            hfState_[1] = hfB2_ * x - hfA2_ * y;
            hfSum += static_cast<double> (y) * y;
        }

        micPeakHold_ = std::max (peak, micPeakHold_ * release);
        micPeakDbFS_.store (linearToDb (micPeakHold_));
        micHfDbFS_.store (powerToDb (static_cast<float> (hfSum / std::max (1, numSamples))));
    }
    else
    {
        // No microphone channel in this callback is indistinguishable, from here,
        // from a microphone reading silence - and the supervisor must treat it
        // the same way, so report silence rather than holding the last value.
        micPeakHold_ = 0.0f;
        micPeakDbFS_.store (kSilenceDb);
        micHfDbFS_.store (kSilenceDb);
    }

    if (vocal != nullptr)
    {
        float peak = 0.0f;
        for (int i = 0; i < numSamples; ++i)
        {
            const float x = vocal[i];
            peak = std::max (peak, std::abs (x));
            vocalSum += static_cast<double> (x) * x;
        }
        vocalPeakHold_ = std::max (peak, vocalPeakHold_ * release);
        vocalPeakDbFS_.store (linearToDb (vocalPeakHold_));
    }
    else
    {
        vocalPeakHold_ = 0.0f;
        vocalPeakDbFS_.store (kSilenceDb);
    }

    if (measuring_.load())
    {
        // Plain atomic accumulation. Contention is impossible in practice - one
        // audio thread writes, and the reader only looks after measuring_ is
        // cleared - and the alternative would be a lock on the audio thread.
        measureMicSum_.store (measureMicSum_.load() + micSum);
        measureVocalSum_.store (measureVocalSum_.load() + vocalSum);
        measureBlocks_.store (measureBlocks_.load() + 1);
        measureSamples_.store (measureSamples_.load() + numSamples);
    }

    if (speechFileEnded_.exchange (false))
        advanceSpeechFile();
}

void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    if (device == nullptr)
        return;

    sampleRate_.store (device->getCurrentSampleRate());
    prepareHighpass (device->getCurrentSampleRate());
    gTestHp.setHighpass (device->getCurrentSampleRate(), kTestHighpassHz);
    gTestLp.setLowpass (device->getCurrentSampleRate(), kTestLowpassHz);

    micPeakHold_ = vocalPeakHold_ = 0.0f;
    hfState_[0] = hfState_[1] = 0.0f;
    speechFileEnded_.store (true);      // opens the first file on the next block
}

void AudioEngine::audioDeviceStopped()
{
    running_.store (false);
    micPeakDbFS_.store (kSilenceDb);
    micHfDbFS_.store (kSilenceDb);
    vocalPeakDbFS_.store (kSilenceDb);
}

void AudioEngine::audioDeviceError (const juce::String& message)
{
    const juce::ScopedLock lock (errorLock_);
    lastError_ = message;
}

juce::String AudioEngine::lastError() const
{
    const juce::ScopedLock lock (errorLock_);
    return lastError_;
}
} // namespace fbkt
