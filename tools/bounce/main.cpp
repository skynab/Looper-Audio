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

    // The written file is the full two-track mix at unity gain.
    const auto buffer = OfflineRenderer::render({ arp, bass }, { 0.0f, 0.0f }, bpm, sampleRate, seconds);

    // Per-track gain check: -6 dB should roughly halve the amplitude (10^(-6/20) ~= 0.501).
    const std::vector<Pattern> one { arp };
    const auto  full      = OfflineRenderer::render(one, std::vector<float> { 0.0f },  bpm, sampleRate, seconds);
    const auto  quiet     = OfflineRenderer::render(one, std::vector<float> { -6.0f }, bpm, sampleRate, seconds);
    const float rmsFull   = full.getRMSLevel(0, 0, full.getNumSamples());
    const float rmsQuiet  = quiet.getRMSLevel(0, 0, quiet.getNumSamples());
    const float gainRatio = rmsFull > 0.0f ? rmsQuiet / rmsFull : 0.0f;

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
              << "  peak=" << peak
              << "  gainRatio(-6dB)=" << gainRatio << "\n";

    // Non-silent output plus a correct -6 dB ratio confirm the render + gain paths.
    const bool ok = rms > 0.0f && std::isfinite(rms) && gainRatio > 0.47f && gainRatio < 0.53f;
    return ok ? 0 : 2;
}
