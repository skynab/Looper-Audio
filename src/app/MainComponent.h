#pragma once

#include <juce_audio_utils/juce_audio_utils.h>

#include <atomic>

namespace looper
{
/**
    Phase 0 "hello audio": an AudioAppComponent that generates a sine test tone.

    Its only job is to prove the audio path works end to end on every platform.
    Note the threading discipline that the rest of the engine will follow: the UI
    thread writes control values into atomics; the audio callback only ever reads
    them — it never touches JUCE component state directly.
*/
class MainComponent final : public juce::AudioAppComponent
{
public:
    MainComponent();
    ~MainComponent() override;

    void prepareToPlay(int samplesPerBlockExpected, double sampleRate) override;
    void getNextAudioBlock(const juce::AudioSourceChannelInfo& bufferToFill) override;
    void releaseResources() override;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    juce::ToggleButton toneButton { "Play test tone" };
    juce::Slider       frequencySlider;
    juce::Slider       gainSlider;
    juce::Label        frequencyLabel { {}, "Freq" };
    juce::Label        gainLabel      { {}, "Gain" };
    juce::AudioDeviceSelectorComponent deviceSelector;

    // Written by the UI thread, read by the audio thread.
    std::atomic<bool>  playing_         { false };
    std::atomic<float> targetGainDb_    { -12.0f };
    std::atomic<float> targetFrequency_ { 440.0f };

    // Audio-thread-only state.
    double sampleRate_ = 0.0;
    double phase_      = 0.0;
    juce::LinearSmoothedValue<float> gain_ { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

} // namespace looper
