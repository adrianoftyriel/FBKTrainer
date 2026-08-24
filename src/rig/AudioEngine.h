// FBKTrainer - AudioEngine.h
//
// One audio device for everything: the speech that goes to the speaker, the
// measurement microphone that comes back, and the vocal channel's return.
//
// Why one device and not three
// ----------------------------
// It would be easier to play speech out of the computer's own output and take
// the microphones in through the console's card. It would also make every timing
// measurement in this program meaningless. Two audio devices are two sample
// clocks, and two sample clocks drift - typically tens of samples a minute, which
// is small enough to look like nothing and large enough to smear the arrival time
// of a howl across the window we are trying to measure it in. Since the entire
// proactive model is being trained on when things happen relative to each other,
// a drifting clock does not add noise to the data, it adds a slow lie.
//
// So the rig runs on one device - in practice the console's own card - and
// everything is sample-locked by construction.
//
// Measurement rather than metering
// --------------------------------
// The routing self test needs "the level over this defined interval", not "the
// level right now". Sampling an instantaneous meter at an arbitrary moment gives
// an answer that depends on whether the talker happened to be mid-syllable. So
// levels are accumulated into an explicitly started and stopped measurement
// window, and the peak meters exist separately for the safety path, which does
// want the instantaneous answer.
#pragma once

#include "RigConfig.h"

#include <JuceHeader.h>

#include <atomic>
#include <memory>

namespace fbkt
{
enum class TestSignal
{
    none,
    bandLimitedNoise    // 300 Hz - 4 kHz, for the routing self test
};

// What the safety path reads, continuously.
struct LiveLevels
{
    float micPeakDbFS { kSilenceDb };
    float micHfDbFS { kSilenceDb };
    float vocalPeakDbFS { kSilenceDb };
    bool  running { false };
};

// What a measurement window produces.
struct MeasuredLevels
{
    bool  valid { false };
    float micDbFS { kSilenceDb };
    float vocalDbFS { kSilenceDb };
    int   blocks { 0 };
};

class AudioEngine final : public juce::AudioIODeviceCallback
{
public:
    AudioEngine();
    ~AudioEngine() override;

    // Opens the configured device with exactly the channels the rig needs. The
    // error string is written for the operator, not for a log.
    bool start (const RigConfig& config, juce::String& errorOut);
    void stop();

    bool isRunning() const noexcept { return running_.load(); }
    double sampleRate() const noexcept { return sampleRate_.load(); }

    juce::AudioDeviceManager& deviceManager() noexcept { return deviceManager_; }

    // --- output ------------------------------------------------------------
    void setSpeechFolder (const juce::File& folder);
    int  speechFileCount() const;
    juce::String currentSpeechFile() const;

    void setSpeechPlaying (bool shouldPlay);
    bool isSpeechPlaying() const noexcept { return speechPlaying_.load(); }

    void setSpeechLevelDb (float db) noexcept { speechLevelDb_.store (db); }

    void setTestSignal (TestSignal signal, float levelDbFS) noexcept;
    TestSignal testSignal() const noexcept { return testSignal_.load(); }

    // --- measurement -------------------------------------------------------
    LiveLevels levels() const noexcept;

    void beginMeasurement() noexcept;
    MeasuredLevels endMeasurement() noexcept;

    // AudioIODeviceCallback
    void audioDeviceIOCallbackWithContext (const float* const* inputChannelData,
                                           int numInputChannels,
                                           float* const* outputChannelData,
                                           int numOutputChannels,
                                           int numSamples,
                                           const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;
    void audioDeviceError (const juce::String&) override;

    juce::String lastError() const;

private:
    void advanceSpeechFile();
    void prepareHighpass (double sampleRate);

    juce::AudioDeviceManager deviceManager_;
    juce::AudioFormatManager formatManager_;

    RigConfig config_;
    std::atomic<bool>   running_ { false };
    std::atomic<double> sampleRate_ { 48000.0 };

    // --- speech playback ---------------------------------------------------
    mutable juce::CriticalSection speechLock_;
    juce::Array<juce::File> speechFiles_;
    int speechIndex_ { 0 };
    std::unique_ptr<juce::AudioFormatReaderSource> speechSource_;
    std::unique_ptr<juce::ResamplingAudioSource>   speechResampler_;
    juce::AudioBuffer<float> speechScratch_;
    juce::String currentSpeechName_;

    std::atomic<bool>  speechPlaying_ { false };
    std::atomic<float> speechLevelDb_ { -24.0f };
    std::atomic<bool>  speechFileEnded_ { false };

    // --- test signal -------------------------------------------------------
    std::atomic<TestSignal> testSignal_ { TestSignal::none };
    std::atomic<float>      testLevelDb_ { -30.0f };
    juce::Random            random_;
    // Two cascaded biquads shape white noise into the speech band, so the self
    // test excites the part of the spectrum feedback actually lives in rather
    // than putting most of its energy where nothing can hear it.
    float noiseHpZ_[2][2] {};
    float noiseLpZ_[2][2] {};

    // --- metering ----------------------------------------------------------
    std::atomic<float> micPeakDbFS_ { kSilenceDb };
    std::atomic<float> micHfDbFS_ { kSilenceDb };
    std::atomic<float> vocalPeakDbFS_ { kSilenceDb };

    float micPeakHold_ { 0.0f };
    float vocalPeakHold_ { 0.0f };
    float hfState_[2] {};                       // highpass state for the HF meter
    float hfB0_ { 1.0f }, hfB1_ { -2.0f }, hfB2_ { 1.0f }, hfA1_ { 0.0f }, hfA2_ { 0.0f };

    std::atomic<bool>   measuring_ { false };
    std::atomic<double> measureMicSum_ { 0.0 };
    std::atomic<double> measureVocalSum_ { 0.0 };
    std::atomic<int>    measureBlocks_ { 0 };
    std::atomic<int>    measureSamples_ { 0 };

    mutable juce::CriticalSection errorLock_;
    juce::String lastError_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioEngine)
};
} // namespace fbkt
