#include <iostream>

#include <juce_audio_formats/juce_audio_formats.h>

#include "engine/OfflineRenderer.h"

// Headless bounce: renders a demo arpeggio to a WAV so the synth + sequencer
// audio path can be verified without an audio device. Also usable as a smoke test.
int main(int argc, char** argv)
{
    using namespace looper::engine;

    const double bpm        = 120.0;
    const double sampleRate = 44100.0;
    const double seconds    = 4.0;

    // Track 1: C-E-G-C arpeggio, one note per beat (the piano-roll demo).
    Pattern arp;
    arp.lengthBeats = 4.0;
    const int root     = 60;
    const int arpNotes[] = { 0, 4, 7, 12 };
    for (int i = 0; i < 4; ++i)
        arp.notes.push_back({ (double) i, 0.5, root + arpNotes[i], 0.8f });

    // Track 2: a simple root-note bass on beats 1 and 3, to exercise track summing.
    Pattern bass;
    bass.lengthBeats = 4.0;
    bass.notes.push_back({ 0.0, 1.0, 36, 0.9f });
    bass.notes.push_back({ 2.0, 1.0, 43, 0.9f });

    const auto buffer = OfflineRenderer::render({ arp, bass }, bpm, sampleRate, seconds);

    const juce::File out = juce::File::getCurrentWorkingDirectory()
                               .getChildFile(argc > 1 ? argv[1] : "bounce.wav");

    if (! OfflineRenderer::writeWav(out, buffer, sampleRate))
    {
        std::cerr << "Failed to write " << out.getFullPathName() << "\n";
        return 1;
    }

    const float rms  = buffer.getRMSLevel(0, 0, buffer.getNumSamples());
    const float peak = buffer.getMagnitude(0, buffer.getNumSamples());

    std::cout << "wrote " << out.getFullPathName()
              << "  frames=" << buffer.getNumSamples()
              << "  rms=" << rms
              << "  peak=" << peak << "\n";

    // Non-silent output confirms notes actually sounded.
    return (rms > 0.0f && std::isfinite(rms)) ? 0 : 2;
}
