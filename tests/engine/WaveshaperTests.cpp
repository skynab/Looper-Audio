#include <catch2/catch_test_macros.hpp>

#include <engine/Waveshaper.h>

#include <cmath>
#include <vector>

using namespace looper::engine;

namespace
{
    constexpr double kPi = 3.14159265358979323846;

    /** Magnitude at one frequency, by direct correlation. A whole FFT would
        be more than this needs: every claim here is about a single known bin. */
    double magnitudeAt(const std::vector<float>& signal, double hz, double sampleRate)
    {
        double re = 0.0, im = 0.0;
        for (size_t n = 0; n < signal.size(); ++n)
        {
            const double phase = 2.0 * kPi * hz * (double) n / sampleRate;
            re += signal[n] * std::cos(phase);
            im -= signal[n] * std::sin(phase);
        }
        return std::sqrt(re * re + im * im) / (double) signal.size();
    }

    std::vector<float> drive(Waveshaper& shaper, double toneHz, double sampleRate,
                             int samples, float amplitude, bool antiAliased)
    {
        std::vector<float> out;
        out.reserve((size_t) samples);
        for (int n = 0; n < samples; ++n)
        {
            const float x = amplitude * (float) std::sin(2.0 * kPi * toneHz * n / sampleRate);
            out.push_back(antiAliased ? shaper.processSample(x) : shaper.processSampleNaive(x));
        }
        return out;
    }
}

TEST_CASE("Anti-aliased shaping puts less energy in the aliases", "[engine][drive]")
{
    // Measuring aliasing needs a bin that nothing legitimate occupies. If the
    // sample rate is an exact multiple of the tone, every folded alias lands
    // straight back on a harmonic and the two are indistinguishable — so the
    // tones here are chosen so that fs/tone is not an integer, and the bin is
    // checked against the harmonic grid before it is used.
    constexpr double sampleRate = 48000.0;

    auto cleanAliasBin = [sampleRate](double tone)
    {
        auto onHarmonic = [sampleRate](double hz, double t)
        {
            for (int k = 1; k * t < sampleRate / 2; ++k)
                if (std::abs(k * t - hz) < 1.0)
                    return true;
            return false;
        };

        for (int k = 3; k < 60; k += 2)
        {
            const double harmonic = k * tone;
            if (harmonic <= sampleRate / 2)
                continue;

            double folded = std::fmod(harmonic, sampleRate);
            if (folded > sampleRate / 2)
                folded = sampleRate - folded;

            if (folded > 200.0 && folded < sampleRate / 2 - 200.0 && ! onHarmonic(folded, tone))
                return folded;
        }
        return -1.0;
    };

    for (double tone : { 1700.0, 3300.0, 5000.0 })
    {
        for (float driveAmount : { 4.0f, 12.0f })
        {
            const double aliasBin = cleanAliasBin(tone);
            REQUIRE(aliasBin > 0.0);

            Waveshaper naive, adaa;
            naive.setDrive(driveAmount);
            adaa.setDrive(driveAmount);
            naive.reset();
            adaa.reset();

            const double naiveAlias = magnitudeAt(drive(naive, tone, sampleRate, 16384, 0.9f, false),
                                                  aliasBin, sampleRate);
            const double adaaAlias  = magnitudeAt(drive(adaa, tone, sampleRate, 16384, 0.9f, true),
                                                  aliasBin, sampleRate);

            INFO("tone " << tone << " drive " << driveAmount << " alias bin " << aliasBin
                         << ": naive " << naiveAlias << " adaa " << adaaAlias);

            // Measured across these cases the ratio runs 0.44-0.60, i.e. about
            // 4.5-7dB. The bound is set above the worst of them with margin,
            // so this fails if ADAA is bypassed but doesn't chase the exact
            // figure.
            REQUIRE(adaaAlias < naiveAlias * 0.7);
        }
    }
}

TEST_CASE("Shaping still produces the harmonics it is supposed to", "[engine][drive]")
{
    // Suppressing aliases is worthless if it also suppresses the distortion.
    // A symmetric shaper generates odd harmonics: the third must be well up.
    constexpr double sampleRate = 48000.0;
    constexpr double tone       = 500.0;

    Waveshaper shaper;
    shaper.setDrive(10.0f);
    shaper.reset();

    const auto out = drive(shaper, tone, sampleRate, 8192, 0.9f, true);

    const double fundamental = magnitudeAt(out, tone, sampleRate);
    const double third       = magnitudeAt(out, tone * 3.0, sampleRate);

    REQUIRE(fundamental > 0.1);
    REQUIRE(third > fundamental * 0.05); // audibly present, not a rounding error
}

TEST_CASE("A held signal doesn't turn into noise", "[engine][drive]")
{
    // The degenerate case: consecutive samples nearly equal makes the ADAA
    // quotient 0/0. A sustained note is exactly that, so getting it wrong
    // would make held notes — the thing a guitar does most — break up.
    Waveshaper shaper;
    shaper.setDrive(4.0f);
    shaper.reset();

    for (int n = 0; n < 64; ++n) // settle
        shaper.processSample(0.5f);

    const float expected = std::tanh(0.5f * 4.0f);
    for (int n = 0; n < 256; ++n)
    {
        const float y = shaper.processSample(0.5f);
        REQUIRE(std::isfinite(y));
        REQUIRE(std::abs(y - expected) < 1.0e-4f);
    }
}

TEST_CASE("A very slow ramp stays finite and monotonic", "[engine][drive]")
{
    // Every sample of this sits in the fallback region.
    Waveshaper shaper;
    shaper.setDrive(2.0f);
    shaper.reset();

    // ADAA carries the previous input as state, and reset() clears it to
    // zero. The very first sample is therefore averaged over the jump from
    // silence to wherever the signal starts — one sample of startup
    // transient, inherent to the method rather than a defect. Prime it.
    shaper.processSample(-0.5f);

    float previous = -2.0f;
    for (int n = 0; n < 4000; ++n)
    {
        const float y = shaper.processSample(-0.5f + (float) n * 1.0e-6f);
        REQUIRE(std::isfinite(y));
        REQUIRE(y >= previous - 1.0e-5f);
        previous = y;
    }
}

TEST_CASE("Large inputs don't overflow the antiderivative", "[engine][drive]")
{
    // log(cosh(x)) via cosh() overflows to infinity around |x| = 710 and the
    // quotient becomes NaN. A drive pedal is where large inputs turn up.
    Waveshaper shaper;
    shaper.setDrive(1000.0f);
    shaper.reset();

    for (float x : { -50.0f, -1.0f, 0.0f, 1.0f, 50.0f, 900.0f, -900.0f })
    {
        const float y = shaper.processSample(x);
        INFO("input " << x);
        REQUIRE(std::isfinite(y));
        REQUIRE(std::abs(y) <= 1.001f); // tanh is bounded, and so is its average
    }
}

TEST_CASE("Hard clipping is bounded and flat above the threshold", "[engine][drive]")
{
    Waveshaper shaper;
    shaper.setKind(Waveshaper::Kind::Hard);
    shaper.setDrive(1.0f);
    shaper.reset();

    for (int n = 0; n < 64; ++n)
        shaper.processSample(5.0f);

    REQUIRE(std::abs(shaper.processSample(5.0f) - 1.0f) < 1.0e-5f);
    for (int n = 0; n < 64; ++n)
        shaper.processSample(-5.0f);
    REQUIRE(std::abs(shaper.processSample(-5.0f) + 1.0f) < 1.0e-5f);
}

TEST_CASE("The cabinet takes the fizz off the top", "[engine][drive]")
{
    // The whole reason it ships with the drive: distortion is full of energy
    // above a speaker's top corner, and that energy is what reads as fizz.
    constexpr double sampleRate = 48000.0;

    auto responseAt = [sampleRate](double hz)
    {
        CabinetSim cab;
        cab.prepare(sampleRate);

        std::vector<float> out;
        const int samples = 16384;
        out.reserve((size_t) samples);
        for (int n = 0; n < samples; ++n)
            out.push_back(cab.processSample((float) std::sin(2.0 * kPi * hz * n / sampleRate)));

        // Skip the settling transient before measuring.
        const std::vector<float> steady(out.begin() + 4096, out.end());
        return magnitudeAt(steady, hz, sampleRate);
    };

    const double atOneK  = responseAt(1000.0);
    const double atTenK  = responseAt(10000.0);
    const double atThirty = responseAt(30.0);

    INFO("1k " << atOneK << "  10k " << atTenK << "  30Hz " << atThirty);
    REQUIRE(atTenK < atOneK * 0.25);   // the fizz band is well down
    REQUIRE(atThirty < atOneK * 0.5);  // and so is the mud below the cab
    REQUIRE(atOneK > 0.1);             // while the guitar's own range survives
}

TEST_CASE("The cabinet settles rather than running away", "[engine][drive]")
{
    CabinetSim cab;
    cab.prepare(48000.0);

    for (int n = 0; n < 100000; ++n)
    {
        const float y = cab.processSample(n % 2 == 0 ? 1.0f : -1.0f);
        REQUIRE(std::isfinite(y));
        REQUIRE(std::abs(y) < 10.0f);
    }
}
