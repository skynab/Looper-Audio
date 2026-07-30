#include <catch2/catch_test_macros.hpp>

#include <engine/PedalDsp.h>

#include <cmath>

using namespace looper::engine;

namespace
{
    constexpr double kSampleRate = 48000.0;

    Compressor makeCompressor(float thresholdDb, float ratio, float attackMs, float releaseMs)
    {
        Compressor c;
        c.prepare(kSampleRate);
        c.setThresholdDb(thresholdDb);
        c.setRatio(ratio);
        c.setAttackMs(attackMs);
        c.setReleaseMs(releaseMs);
        return c;
    }

    float dbToLinear(float db) { return std::pow(10.0f, db / 20.0f); }

    /** Runs a constant level in for @p ms and reports the gain at the end. */
    float settledGain(Compressor& c, float level, double ms)
    {
        const int samples = (int) (ms * 0.001 * kSampleRate);
        float gain = 1.0f;
        for (int n = 0; n < samples; ++n)
            gain = c.gainFor(level);
        return gain;
    }
}

TEST_CASE("A signal under the threshold passes at unity", "[engine][pedal]")
{
    // A compressor that quietly attenuates everything is a volume pedal.
    auto c = makeCompressor(-18.0f, 4.0f, 5.0f, 50.0f);
    const float gain = settledGain(c, dbToLinear(-30.0f), 500.0);
    REQUIRE(std::abs(gain - 1.0f) < 1.0e-4f);
}

TEST_CASE("A signal over the threshold is reduced by the ratio", "[engine][pedal]")
{
    // 12dB over at 4:1 should end up 3dB over, so 9dB of reduction.
    auto c = makeCompressor(-18.0f, 4.0f, 1.0f, 50.0f);
    settledGain(c, dbToLinear(-6.0f), 500.0);

    const float reduction = c.currentReductionDb();
    INFO("reduction " << reduction << " dB");
    REQUIRE(std::abs(reduction - (-9.0f)) < 0.2f);
}

TEST_CASE("A higher ratio squashes harder", "[engine][pedal]")
{
    auto gentle = makeCompressor(-18.0f, 2.0f, 1.0f, 50.0f);
    auto firm   = makeCompressor(-18.0f, 8.0f, 1.0f, 50.0f);

    settledGain(gentle, dbToLinear(-6.0f), 500.0);
    settledGain(firm, dbToLinear(-6.0f), 500.0);

    REQUIRE(firm.currentReductionDb() < gentle.currentReductionDb());
}

TEST_CASE("Limiting holds the output near the threshold", "[engine][pedal]")
{
    // A very high ratio is a limiter: whatever goes in, the output should sit
    // just above the threshold rather than climbing with the input.
    auto c = makeCompressor(-20.0f, 1000.0f, 1.0f, 50.0f);

    const float input = dbToLinear(0.0f);
    const float gain  = settledGain(c, input, 500.0);
    const float outDb = 20.0f * std::log10(input * gain);

    INFO("output " << outDb << " dB");
    REQUIRE(std::abs(outDb - (-20.0f)) < 0.5f);
}

TEST_CASE("Attack takes about as long as it says", "[engine][pedal]")
{
    // The reason smoothing is applied to the gain reduction and not to the
    // level detector: a stated attack time has to mean the same thing at
    // every signal level, or the number on the control is decoration.
    for (float attackMs : { 5.0f, 20.0f, 100.0f })
    {
        auto c = makeCompressor(-18.0f, 4.0f, attackMs, 200.0f);

        const float loud = dbToLinear(-6.0f);
        const int   ramp = (int) (attackMs * 0.001 * kSampleRate);

        for (int n = 0; n < ramp; ++n)
            c.gainFor(loud);

        // One time constant reaches ~63% of the eventual 9dB.
        const float afterOneTau = c.currentReductionDb();
        INFO("attack " << attackMs << "ms reached " << afterOneTau << " dB");
        REQUIRE(afterOneTau < -9.0f * 0.5f);
        REQUIRE(afterOneTau > -9.0f * 0.8f);
    }
}

TEST_CASE("A faster attack clamps a transient sooner", "[engine][pedal]")
{
    auto fast = makeCompressor(-18.0f, 8.0f, 1.0f, 200.0f);
    auto slow = makeCompressor(-18.0f, 8.0f, 50.0f, 200.0f);

    const float loud = dbToLinear(-6.0f);
    for (int n = 0; n < (int) (0.005 * kSampleRate); ++n) // 5ms in
    {
        fast.gainFor(loud);
        slow.gainFor(loud);
    }

    REQUIRE(fast.currentReductionDb() < slow.currentReductionDb());
}

TEST_CASE("Release lets go after the signal does", "[engine][pedal]")
{
    auto c = makeCompressor(-18.0f, 8.0f, 1.0f, 50.0f);
    settledGain(c, dbToLinear(-6.0f), 500.0);
    REQUIRE(c.currentReductionDb() < -5.0f); // squashed

    // Silence for several release constants: it must come all the way back,
    // or every quiet passage after a loud one stays ducked.
    settledGain(c, 0.0f, 500.0);
    REQUIRE(std::abs(c.currentReductionDb()) < 0.1f);
}

TEST_CASE("Compression is stable and finite on silence", "[engine][pedal]")
{
    // log(0) is -inf: the detector floor is what stops that reaching the
    // gain, and without it the first silent sample poisons everything after.
    auto c = makeCompressor(-18.0f, 4.0f, 5.0f, 50.0f);
    for (int n = 0; n < 10000; ++n)
    {
        const float gain = c.gainFor(0.0f);
        REQUIRE(std::isfinite(gain));
        REQUIRE(gain <= 1.0001f);
    }
}

TEST_CASE("Tremolo at zero depth is not an effect", "[engine][pedal]")
{
    // The only reading of a depth control under which turning it down leaves
    // the signal alone.
    Tremolo t;
    t.prepare(kSampleRate);
    t.setDepth(0.0f);
    t.setRateHz(5.0f);

    for (int n = 0; n < 10000; ++n)
        REQUIRE(std::abs(t.nextGain() - 1.0f) < 1.0e-6f);
}

TEST_CASE("Tremolo starts at full volume", "[engine][pedal]")
{
    // Starting at the bottom of the LFO would swallow the front of any note
    // played on the beat.
    Tremolo t;
    t.prepare(kSampleRate);
    t.setDepth(1.0f);
    REQUIRE(std::abs(t.nextGain() - 1.0f) < 1.0e-5f);
}

TEST_CASE("Tremolo reaches silence at full depth and stays bounded", "[engine][pedal]")
{
    Tremolo t;
    t.prepare(kSampleRate);
    t.setDepth(1.0f);
    t.setRateHz(4.0f);

    float lowest = 2.0f, highest = -1.0f;
    for (int n = 0; n < (int) kSampleRate; ++n) // one second, four cycles
    {
        const float g = t.nextGain();
        lowest  = std::min(lowest, g);
        highest = std::max(highest, g);
    }

    REQUIRE(lowest < 0.01f);   // all the way down
    REQUIRE(highest > 0.99f);  // and all the way back
    REQUIRE(lowest >= 0.0f);   // never inverting the signal
}

TEST_CASE("Tremolo modulates at the rate it is given", "[engine][pedal]")
{
    // Counted by minima, since that is what a listener counts.
    Tremolo t;
    t.prepare(kSampleRate);
    t.setDepth(1.0f);
    t.setRateHz(7.0f);

    int   minima   = 0;
    float previous = 1.0f, beforeThat = 1.0f;
    for (int n = 0; n < (int) (kSampleRate * 2.0); ++n) // two seconds
    {
        const float g = t.nextGain();
        if (previous < beforeThat && previous < g)
            ++minima;
        beforeThat = previous;
        previous   = g;
    }

    INFO("counted " << minima << " minima in 2s at 7Hz");
    REQUIRE(minima >= 13);
    REQUIRE(minima <= 15);
}

namespace
{
    /** Magnitude at one frequency, by direct correlation — every claim here is
        about a single known bin, so a whole FFT would be more than it needs. */
    double magnitudeAtHz(const std::vector<float>& signal, double hz, double sampleRate)
    {
        double re = 0.0, im = 0.0;
        for (size_t n = 0; n < signal.size(); ++n)
        {
            const double phase = 2.0 * 3.14159265358979 * hz * (double) n / sampleRate;
            re += signal[n] * std::cos(phase);
            im -= signal[n] * std::sin(phase);
        }
        return std::sqrt(re * re + im * im) / (double) signal.size();
    }

    /** Runs a steady tone through a chorus and returns the output. */
    std::vector<float> throughChorus(Chorus& chorus, double toneHz, int samples)
    {
        std::vector<float> out;
        out.reserve((size_t) samples);
        for (int n = 0; n < samples; ++n)
            out.push_back(chorus.processSample(
                (float) std::sin(2.0 * 3.14159265358979 * toneHz * n / kSampleRate)));
        return out;
    }

}

TEST_CASE("A chorus at zero mix is the dry signal", "[engine][pedal]")
{
    // The only reading of a mix control under which turning it down leaves the
    // signal alone.
    Chorus chorus;
    chorus.prepare(kSampleRate);
    chorus.setMix(0.0f);
    chorus.setDepth(1.0f);

    for (int n = 0; n < 4000; ++n)
    {
        const float in  = (float) std::sin(0.01 * n);
        const float out = chorus.processSample(in);
        REQUIRE(std::abs(out - in) < 1.0e-6f);
    }
}

TEST_CASE("Modulation is what makes it a chorus", "[engine][pedal]")
{
    // A fixed short delay mixed with the dry signal is a comb filter — a tone
    // colour, not a chorus. The sweep is the effect, so depth must change the
    // output and not merely be stored.
    Chorus still, moving;
    still.prepare(kSampleRate);
    moving.prepare(kSampleRate);

    for (auto* c : { &still, &moving })
    {
        c->setMix(1.0f);
        c->setRateHz(2.0f);
    }
    still.setDepth(0.0f);
    moving.setDepth(1.0f);

    const auto flat  = throughChorus(still, 440.0, 20000);
    const auto swept = throughChorus(moving, 440.0, 20000);

    float worst = 0.0f;
    for (size_t n = 5000; n < flat.size(); ++n)
        worst = std::max(worst, std::abs(flat[n] - swept[n]));

    INFO("largest difference between static and swept: " << worst);
    REQUIRE(worst > 0.05f);
}

TEST_CASE("A swept chorus doesn't generate harmonics", "[engine][pedal]")
{
    // The reason the delay is read at a fractional position, measured rather
    // than asserted. A stepped delay is a nonlinearity: it puts energy at
    // harmonics of the tone, which neither a delay nor a slow modulation of one
    // can produce. Measured at 2x and 3x the tone, interpolation leaves about
    // 0.3% of the fundamental there and whole-sample reads leave about 11%.
    //
    // This replaced a worst-sample-to-sample-step test, which did not
    // discriminate: a one-sample delay jump moves the output by roughly the
    // same amount as one sample of the tone's own slope, so no threshold on
    // step size separates them.
    constexpr double tone = 3000.0;

    Chorus chorus;
    chorus.prepare(kSampleRate);
    chorus.setMix(1.0f);
    chorus.setDepth(1.0f);
    chorus.setRateHz(4.0f);

    const auto out = throughChorus(chorus, tone, 32768);
    const std::vector<float> steady(out.begin() + 8192, out.end());

    const double fundamental = magnitudeAtHz(steady, tone, kSampleRate);
    REQUIRE(fundamental > 1.0e-3); // the tone got through at all

    for (double harmonic : { tone * 2.0, tone * 3.0 })
    {
        const double level = magnitudeAtHz(steady, harmonic, kSampleRate);
        INFO("harmonic " << harmonic << " at " << (100.0 * level / fundamental)
             << "% of the fundamental");
        REQUIRE(level < fundamental * 0.02);
    }
}

TEST_CASE("A chorus stays bounded and finite", "[engine][pedal]")
{
    // No feedback path, so it cannot run away — but the taps are summed, and
    // a summing error would show up here.
    Chorus chorus;
    chorus.prepare(kSampleRate);
    chorus.setMix(1.0f);
    chorus.setDepth(1.0f);
    chorus.setRateHz(8.0f);

    for (int n = 0; n < 200000; ++n)
    {
        const float out = chorus.processSample(n % 2 == 0 ? 1.0f : -1.0f);
        REQUIRE(std::isfinite(out));
        REQUIRE(std::abs(out) <= 2.0f);
    }
}

TEST_CASE("Chorus settings are clamped to usable ranges", "[engine][pedal]")
{
    // Depth past its range would ask the delay line for a position it hasn't
    // allocated, and rate at zero would leave the sweep stationary.
    Chorus chorus;
    chorus.prepare(kSampleRate);
    chorus.setMix(2.0f);
    chorus.setDepth(50.0f);
    chorus.setRateHz(1000.0f);

    for (int n = 0; n < 8000; ++n)
    {
        const float out = chorus.processSample((float) std::sin(0.02 * n));
        REQUIRE(std::isfinite(out));
        REQUIRE(std::abs(out) <= 2.0f);
    }
}
