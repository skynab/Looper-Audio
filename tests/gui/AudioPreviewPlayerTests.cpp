#include <catch2/catch_test_macros.hpp>

#include <juce_audio_basics/juce_audio_basics.h>

#include <engine/AudioPreviewPlayer.h>

#include <cmath>
#include <memory>

using namespace looper::engine;

namespace
{
    constexpr double kSampleRate = 48000.0;

    /** A clip of @p seconds at the device rate, every sample 1.0 — so "did
        this play" is just "is the output non-zero", with no windowing or
        interpolation to reason about. */
    ClipData* makeClip(double seconds, double sourceRate = kSampleRate, int channels = 1)
    {
        auto* clip = new ClipData();
        const int length = (int) std::lround(seconds * sourceRate);

        clip->audio.setSize(channels, length);
        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < length; ++i)
                clip->audio.setSample(ch, i, 1.0f);

        clip->sourceSampleRate = sourceRate;
        clip->numChannels      = channels;
        clip->lengthSamples    = length;
        return clip;
    }

    /** Runs @p blocks of @p blockSize through the player and reports the peak
        it produced. */
    float runBlocks(AudioPreviewPlayer& player, int blocks, int blockSize = 512)
    {
        juce::AudioBuffer<float> buffer(2, blockSize);
        float peak = 0.0f;

        for (int b = 0; b < blocks; ++b)
        {
            buffer.clear();
            player.process(buffer);
            peak = std::max(peak, buffer.getMagnitude(0, 0, blockSize));
        }
        return peak;
    }
}

TEST_CASE("A preview plays nothing until asked", "[engine][preview]")
{
    // It is not slaved to the transport, so it must not start just because
    // audio is flowing — the whole reason this isn't AudioFilePlayerNode.
    AudioPreviewPlayer player;
    player.prepare(kSampleRate);
    player.submitClip(makeClip(1.0));

    REQUIRE_FALSE(player.isPlaying());
    REQUIRE(runBlocks(player, 4) == 0.0f);
}

TEST_CASE("A preview plays once started", "[engine][preview]")
{
    AudioPreviewPlayer player;
    player.prepare(kSampleRate);
    player.submitClip(makeClip(1.0));

    player.play(0.0, 0.0); // whole file
    REQUIRE(player.isPlaying());
    REQUIRE(runBlocks(player, 4) > 0.5f);
}

TEST_CASE("A preview stops at the end of the file", "[engine][preview]")
{
    // Running on past the end would read silence forever and leave the
    // playhead stuck at the end, which reads as a hung transport.
    AudioPreviewPlayer player;
    player.prepare(kSampleRate);
    player.submitClip(makeClip(0.05)); // 2400 samples

    player.play(0.0, 0.0);
    runBlocks(player, 20); // well past 2400 samples

    REQUIRE_FALSE(player.isPlaying());
}

TEST_CASE("A preview stops at the end of the selection", "[engine][preview]")
{
    // "Play selection" has to mean the selection — running on into the rest
    // of the clip is the difference between auditioning an edit and
    // auditioning the whole recording.
    AudioPreviewPlayer player;
    player.prepare(kSampleRate);
    player.submitClip(makeClip(2.0));

    player.play(0.1, 0.15); // 50ms
    REQUIRE(player.isPlaying());

    // ~50ms is about five 512-sample blocks; ten is comfortably past it.
    runBlocks(player, 10);
    REQUIRE_FALSE(player.isPlaying());
    REQUIRE(player.positionSeconds() <= 0.16);
}

TEST_CASE("A preview starts where it was told to", "[engine][preview]")
{
    AudioPreviewPlayer player;
    player.prepare(kSampleRate);
    player.submitClip(makeClip(2.0));

    player.play(1.0, 0.0);
    REQUIRE(std::abs(player.positionSeconds() - 1.0) < 1.0e-9);

    runBlocks(player, 2);
    REQUIRE(player.positionSeconds() > 1.0);
    REQUIRE(player.positionSeconds() < 1.1);
}

TEST_CASE("Restarting resets the position rather than resuming", "[engine][preview]")
{
    AudioPreviewPlayer player;
    player.prepare(kSampleRate);
    player.submitClip(makeClip(2.0));

    player.play(0.0, 0.0);
    runBlocks(player, 10);
    const double afterFirst = player.positionSeconds();
    REQUIRE(afterFirst > 0.0);

    player.play(0.5, 0.0);
    runBlocks(player, 1);
    REQUIRE(player.positionSeconds() >= 0.5);
    REQUIRE(player.positionSeconds() < 0.5 + 0.05);
}

TEST_CASE("Stopping really stops", "[engine][preview]")
{
    AudioPreviewPlayer player;
    player.prepare(kSampleRate);
    player.submitClip(makeClip(2.0));

    player.play(0.0, 0.0);
    runBlocks(player, 2);
    player.stop();

    REQUIRE_FALSE(player.isPlaying());
    REQUIRE(runBlocks(player, 4) == 0.0f);
}

TEST_CASE("Loading another clip stops the audition", "[engine][preview]")
{
    // The position was a position in the previous file; carrying it over
    // would play an unrelated part of the new one.
    AudioPreviewPlayer player;
    player.prepare(kSampleRate);
    player.submitClip(makeClip(2.0));
    player.play(0.0, 0.0);
    REQUIRE(player.isPlaying());

    player.submitClip(makeClip(2.0));
    REQUIRE_FALSE(player.isPlaying());
}

TEST_CASE("A preview with no clip is silent rather than crashing", "[engine][preview]")
{
    // The state the pane is in before a file has been chosen.
    AudioPreviewPlayer player;
    player.prepare(kSampleRate);
    player.play(0.0, 0.0);

    REQUIRE(runBlocks(player, 4) == 0.0f);
}

TEST_CASE("A file at another sample rate still plays for the right length",
          "[engine][preview]")
{
    // Resampling is by ratio, so a rate mismatch changes how many output
    // samples one second of file takes — getting it backwards would make a
    // 44.1k file play too fast and stop early.
    AudioPreviewPlayer player;
    player.prepare(kSampleRate);
    player.submitClip(makeClip(0.5, 44100.0));

    player.play(0.0, 0.0);
    REQUIRE(runBlocks(player, 2) > 0.5f);

    // 0.5s of file at 44.1k, played at 48k, is ~0.5s of output — about 47
    // blocks of 512. Still going at 20, done by 80.
    runBlocks(player, 18);
    REQUIRE(player.isPlaying());
    runBlocks(player, 80);
    REQUIRE_FALSE(player.isPlaying());
}

TEST_CASE("A mono file feeds both output channels", "[engine][preview]")
{
    // Otherwise auditioning a mono recording comes out of the left speaker
    // only, which reads as a broken file.
    AudioPreviewPlayer player;
    player.prepare(kSampleRate);
    player.submitClip(makeClip(1.0, kSampleRate, 1));
    player.play(0.0, 0.0);

    juce::AudioBuffer<float> buffer(2, 512);
    buffer.clear();
    player.process(buffer);

    REQUIRE(buffer.getMagnitude(0, 0, 512) > 0.5f);
    REQUIRE(buffer.getMagnitude(1, 0, 512) > 0.5f);
}
