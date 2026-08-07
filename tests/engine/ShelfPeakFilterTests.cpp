#include <catch2/catch_test_macros.hpp>

#include <engine/ShelfPeakFilter.h>

#include <cmath>

using looper::engine::ShelfPeakFilter;

namespace
{
// RMS gain of the filter at a given frequency, measured over the settled tail.
float measureGainDb(ShelfPeakFilter& filter, float freq, float sampleRate, int numSamples = 8000)
{
    constexpr double twoPi = 6.283185307179586;
    double phase = 0.0;
    const double inc = twoPi * freq / sampleRate;

    double sumIn = 0.0, sumOut = 0.0;
    for (int i = 0; i < numSamples; ++i)
    {
        const float x = (float) std::sin(phase);
        phase += inc;
        const float y = filter.processSample(x);

        if (i >= numSamples / 2) // ignore the transient
        {
            sumIn  += (double) x * x;
            sumOut += (double) y * y;
        }
    }
    const float ratio = sumIn > 0.0 ? (float) std::sqrt(sumOut / sumIn) : 0.0f;
    return 20.0f * std::log10(std::max(ratio, 1.0e-6f));
}
}

TEST_CASE("Low shelf boosts below its frequency and leaves highs alone", "[engine][eq]")
{
    const float sr = 48000.0f;
    ShelfPeakFilter f;
    f.prepare(sr);
    f.setShape(ShelfPeakFilter::Shape::LowShelf);
    f.setFrequency(250.0f);
    f.setGainDb(12.0f);

    f.reset();
    const float low  = measureGainDb(f, 60.0f, sr);
    f.reset();
    const float high = measureGainDb(f, 8000.0f, sr);

    REQUIRE(low > 8.0f);          // close to the full +12dB boost well below the shelf
    REQUIRE(std::abs(high) < 1.0f); // untouched well above it
}

TEST_CASE("Low shelf cuts below its frequency when gain is negative", "[engine][eq]")
{
    const float sr = 48000.0f;
    ShelfPeakFilter f;
    f.prepare(sr);
    f.setShape(ShelfPeakFilter::Shape::LowShelf);
    f.setFrequency(250.0f);
    f.setGainDb(-12.0f);

    f.reset();
    const float low = measureGainDb(f, 60.0f, sr);

    REQUIRE(low < -8.0f);
}

TEST_CASE("High shelf boosts above its frequency and leaves lows alone", "[engine][eq]")
{
    const float sr = 48000.0f;
    ShelfPeakFilter f;
    f.prepare(sr);
    f.setShape(ShelfPeakFilter::Shape::HighShelf);
    f.setFrequency(4000.0f);
    f.setGainDb(12.0f);

    f.reset();
    const float high = measureGainDb(f, 12000.0f, sr);
    f.reset();
    const float low  = measureGainDb(f, 100.0f, sr);

    REQUIRE(high > 8.0f);
    REQUIRE(std::abs(low) < 1.0f);
}

TEST_CASE("Peaking band boosts near its centre and leaves the extremes alone", "[engine][eq]")
{
    const float sr = 48000.0f;
    ShelfPeakFilter f;
    f.prepare(sr);
    f.setShape(ShelfPeakFilter::Shape::Peaking);
    f.setFrequency(1000.0f);
    f.setQ(0.7f);
    f.setGainDb(12.0f);

    f.reset();
    const float centre = measureGainDb(f, 1000.0f, sr);
    f.reset();
    const float low  = measureGainDb(f, 60.0f, sr);
    f.reset();
    const float high = measureGainDb(f, 16000.0f, sr);

    REQUIRE(centre > 10.0f);
    REQUIRE(std::abs(low) < 1.5f);
    REQUIRE(std::abs(high) < 1.5f);
}

TEST_CASE("Zero gain leaves the signal unchanged, in every shape", "[engine][eq]")
{
    const float sr = 48000.0f;
    for (auto shape : { ShelfPeakFilter::Shape::LowShelf, ShelfPeakFilter::Shape::HighShelf, ShelfPeakFilter::Shape::Peaking })
    {
        ShelfPeakFilter f;
        f.prepare(sr);
        f.setShape(shape);
        f.setFrequency(1000.0f);
        f.setGainDb(0.0f);

        const float gainDb = measureGainDb(f, 1000.0f, sr);
        REQUIRE(std::abs(gainDb) < 0.1f);
    }
}
