#include <catch2/catch_test_macros.hpp>

#include <engine/GuitarString.h>

#include <cmath>
#include <vector>

using namespace looper::engine;

namespace
{
    constexpr double kSampleRate = 48000.0;

    /** Standard tuning, low to high. */
    constexpr double kOpenStrings[6] = { 82.4069, 110.0, 146.832, 195.998, 246.942, 329.628 };

    double fretted(double openHz, int fret) { return openHz * std::pow(2.0, fret / 12.0); }

    std::vector<float> render(GuitarString& string, double seconds)
    {
        const int n = (int) (seconds * kSampleRate);
        std::vector<float> out((size_t) n);
        for (int i = 0; i < n; ++i)
            out[(size_t) i] = string.process();
        return out;
    }

    /** Magnitude of one frequency, by direct correlation — a single DFT bin. */
    double magnitudeAt(const std::vector<float>& signal, double frequency, int from, int count)
    {
        double real = 0.0, imaginary = 0.0;
        for (int i = 0; i < count; ++i)
        {
            const double angle = 2.0 * M_PI * frequency * (double) i / kSampleRate;
            real      += signal[(size_t) (from + i)] * std::cos(angle);
            imaginary += signal[(size_t) (from + i)] * std::sin(angle);
        }
        return std::hypot(real, imaginary) / (double) count;
    }

    /** Finds the actual fundamental by searching for the strongest response
        near @p nominal, refining twice. Precise enough to resolve a couple of
        cents, which a plain autocorrelation peak is not at short periods. */
    double measurePitch(const std::vector<float>& signal, double nominal)
    {
        // A long window matters most for the low strings: at 82Hz a 0.4s
        // window is only ~33 cycles, and the peak search can't resolve a
        // fraction of a cent from that. Measured error on the open low E fell
        // from 1.2 cents to well under 0.5 simply by widening this, which is
        // how we know it was the measurement and not the string.
        const int from  = (int) (0.05 * kSampleRate); // past the pluck transient
        const int count = std::min((int) signal.size() - from, (int) (1.0 * kSampleRate));

        double best = nominal, span = 60.0, step = 3.0; // cents
        for (int pass = 0; pass < 3; ++pass)
        {
            double bestMag = -1.0, centre = best;
            for (double c = -span; c <= span; c += step)
            {
                const double candidate = centre * std::pow(2.0, c / 1200.0);
                const double mag       = magnitudeAt(signal, candidate, from, count);
                if (mag > bestMag) { bestMag = mag; best = candidate; }
            }
            span = step * 2.0;
            step /= 10.0;
        }
        return best;
    }

    double centsBetween(double measured, double target) { return 1200.0 * std::log2(measured / target); }

    /** Time for the signal to fall 60dB below its early peak. */
    double measureT60(const std::vector<float>& signal)
    {
        const int   window = (int) (0.02 * kSampleRate);
        auto rmsAt = [&](int from)
        {
            double sum = 0.0;
            for (int i = 0; i < window && from + i < (int) signal.size(); ++i)
                sum += (double) signal[(size_t) (from + i)] * signal[(size_t) (from + i)];
            return std::sqrt(sum / window);
        };

        const double reference = rmsAt(0);
        if (reference <= 0.0)
            return 0.0;

        for (int from = window; from + window < (int) signal.size(); from += window)
            if (rmsAt(from) < reference * 0.001)
                return (double) from / kSampleRate;

        return (double) signal.size() / kSampleRate; // never got there
    }
}

TEST_CASE("Every open string is in tune", "[engine][guitar]")
{
    for (double open : kOpenStrings)
    {
        GuitarString string;
        string.prepare(kSampleRate);
        string.setDecaySeconds(4.0);
        string.setFrequency(open);
        string.pluck(1.0f);

        const auto signal = render(string, 1.2);
        REQUIRE(std::abs(centsBetween(measurePitch(signal, open), open)) < 1.0);
    }
}

TEST_CASE("Tuning holds all the way up the neck", "[engine][guitar]")
{
    // The check that matters: an integer delay line is roughly fine down low
    // and drifts badly as the period shortens — about 19 cents sharp at the
    // 24th fret of the high E. Sweeping the whole range is what catches it.
    for (double open : kOpenStrings)
    {
        for (int fret : { 5, 12, 19, 24 })
        {
            const double target = fretted(open, fret);

            GuitarString string;
            string.prepare(kSampleRate);
            string.setDecaySeconds(4.0);
            string.setFrequency(target);
            string.pluck(1.0f);

            const auto   signal = render(string, 1.2);
            const double error  = centsBetween(measurePitch(signal, target), target);
            INFO("open " << open << "Hz, fret " << fret << ", target " << target
                         << "Hz, error " << error << " cents");
            // Measured under 0.35 cents everywhere; asserted at 1.0 to leave
            // room for the stiffness/dispersion model still to come, which
            // legitimately shifts the fundamental a little. An integer delay
            // line fails this at fret 5 (-3.1 cents) and gets worse upward,
            // which is the regression this exists to catch.
            REQUIRE(std::abs(error) < 1.0);
        }
    }
}

TEST_CASE("Decay time follows the requested T60", "[engine][guitar]")
{
    GuitarString string;
    string.prepare(kSampleRate);
    string.setFrequency(110.0);
    string.setDecaySeconds(1.0);
    string.pluck(1.0f);

    const double measured = measureT60(render(string, 3.0));
    INFO("measured T60 " << measured << "s");
    REQUIRE(measured > 0.6);
    REQUIRE(measured < 1.6);
}

TEST_CASE("Decay time holds across the pitch range", "[engine][guitar]")
{
    // Without pitch compensation a fixed loop coefficient makes high notes die
    // far too fast, because they go round the loop more often per second. Two
    // octaves apart with the same requested T60 must decay at similar rates.
    double measured[2] = { 0.0, 0.0 };
    const double pitches[2] = { 82.41, 329.63 };

    for (int i = 0; i < 2; ++i)
    {
        GuitarString string;
        string.prepare(kSampleRate);
        string.setFrequency(pitches[i]);
        string.setDecaySeconds(2.0);
        string.pluck(1.0f);
        measured[i] = measureT60(render(string, 5.0));
    }

    INFO("low " << measured[0] << "s, high " << measured[1] << "s");
    REQUIRE(measured[0] > 1.0);
    REQUIRE(measured[1] > 1.0);
    REQUIRE(std::abs(measured[0] - measured[1]) < 1.2);
}

TEST_CASE("A string never grows in amplitude", "[engine][guitar]")
{
    // Loop gain is < 1 by construction, but a filter or interpolator with gain
    // above unity anywhere would still let it run away — and a self-oscillating
    // string is a stuck note that only a restart clears.
    for (double frequency : { 82.41, 220.0, 880.0, 1318.51 })
    {
        for (double decay : { 0.5, 5.0, 30.0 })
        {
            GuitarString string;
            string.prepare(kSampleRate);
            string.setFrequency(frequency);
            string.setDecaySeconds(decay);
            string.setBrightness(1.0f); // least damping: the worst case
            string.pluck(1.0f);

            const auto signal = render(string, 10.0);

            float early = 0.0f, late = 0.0f;
            for (int i = 0; i < (int) (0.1 * kSampleRate); ++i)
                early = std::max(early, std::abs(signal[(size_t) i]));
            for (int i = (int) (9.0 * kSampleRate); i < (int) signal.size(); ++i)
                late = std::max(late, std::abs(signal[(size_t) i]));

            INFO("f " << frequency << "Hz, decay " << decay << "s");
            REQUIRE(late <= early);
            REQUIRE(std::isfinite(late));
        }
    }
}

TEST_CASE("A pluck produces sound and eventually retires", "[engine][guitar]")
{
    GuitarString string;
    string.prepare(kSampleRate);
    string.setFrequency(146.83);
    string.setDecaySeconds(0.4);

    REQUIRE_FALSE(string.isRinging()); // silent until plucked

    string.pluck(1.0f);
    const auto signal = render(string, 0.2);

    float peak = 0.0f;
    for (float sample : signal)
        peak = std::max(peak, std::abs(sample));
    REQUIRE(peak > 0.01f);

    render(string, 4.0);
    REQUIRE_FALSE(string.isRinging()); // and the voice can be reclaimed
}

TEST_CASE("Muting shortens the decay", "[engine][guitar]")
{
    auto decayWithMute = [](float mute)
    {
        GuitarString string;
        string.prepare(kSampleRate);
        string.setFrequency(110.0);
        string.setDecaySeconds(3.0);
        string.mute(mute);
        string.pluck(1.0f);
        return measureT60(render(string, 4.0));
    };

    REQUIRE(decayWithMute(0.8f) < decayWithMute(0.0f));
}

TEST_CASE("Pick position changes the tone without changing the pitch", "[engine][guitar]")
{
    // The comb is meant to colour the excitation, not detune the string — an
    // easy thing to get wrong by folding it into the loop instead.
    auto pluckAt = [](float position)
    {
        GuitarString string;
        string.prepare(kSampleRate);
        string.setFrequency(196.0);
        string.setDecaySeconds(3.0);
        string.setPickPosition(position);
        string.pluck(1.0f);
        return render(string, 1.2);
    };

    const auto nearBridge = pluckAt(0.05f);
    const auto atMiddle   = pluckAt(0.5f);

    REQUIRE(std::abs(centsBetween(measurePitch(nearBridge, 196.0), 196.0)) < 2.0);
    REQUIRE(std::abs(centsBetween(measurePitch(atMiddle, 196.0), 196.0)) < 2.0);

    // Plucking at the middle suppresses the even harmonics; near the bridge
    // keeps them. The second harmonic is where that shows most clearly.
    const int from = (int) (0.02 * kSampleRate), count = (int) (0.3 * kSampleRate);
    REQUIRE(magnitudeAt(atMiddle, 392.0, from, count) < magnitudeAt(nearBridge, 392.0, from, count));
}

// --- Phase 1: dynamics and coupling (see docs/PLAN.md §33) ----------------

namespace
{
    /** Energy above @p frequency relative to the fundamental — a crude but
        sufficient brightness figure. Normalised by the fundamental so a louder
        note isn't mistaken for a brighter one, which is the entire distinction
        being measured here. */
    double brightnessRatio(const std::vector<float>& signal, double /*fundamental*/)
    {
        // Measured across the *attack*, not the tail. The excitation's
        // spectrum is what velocity shapes, and the loop's damping filter
        // washes that difference out within a few hundred milliseconds — so a
        // window starting at 10ms and running for 250ms measures the loop
        // filter, not the pick, and reports almost no difference at all.
        const int count = (int) (0.03 * kSampleRate);

        // High-frequency content as the energy of the first difference,
        // normalised by the signal's own energy: a first difference is a
        // 6dB/oct highpass, so this rises with brightness and is blind to how
        // loud the note is — which is the whole distinction under test.
        double high  = 0.0;
        double total = 0.0;

        for (int i = 1; i < count; ++i)
        {
            const double difference = (double) signal[(size_t) i] - (double) signal[(size_t) (i - 1)];
            high  += difference * difference;
            total += (double) signal[(size_t) i] * (double) signal[(size_t) i];
        }

        return total > 0.0 ? high / total : 0.0;
    }

    std::vector<float> pluckAtVelocity(float velocity, float sensitivity)
    {
        GuitarString string;
        string.prepare(kSampleRate);
        string.setFrequency(146.832);
        string.setDecaySeconds(3.0);
        string.setVelocitySensitivity(sensitivity);
        string.pluck(velocity);
        return render(string, 0.4);
    }
}

TEST_CASE("Picking harder is brighter, not just louder", "[engine][guitar]")
{
    // Velocity used to scale amplitude and nothing else, so every note in a
    // part had an identical spectrum — most of why a programmed guitar sounds
    // machine-gunned.
    const double soft = brightnessRatio(pluckAtVelocity(0.35f, 0.5f), 146.832);
    const double hard = brightnessRatio(pluckAtVelocity(1.0f,  0.5f), 146.832);

    INFO("soft " << soft << " hard " << hard);
    REQUIRE(hard > soft * 1.1);
}

TEST_CASE("Velocity sensitivity of zero restores the old behaviour",
          "[engine][guitar]")
{
    // The escape hatch, and the reason this could be added without retuning
    // every part already written.
    const double soft = brightnessRatio(pluckAtVelocity(0.35f, 0.0f), 146.832);
    const double hard = brightnessRatio(pluckAtVelocity(1.0f,  0.0f), 146.832);

    REQUIRE(std::abs(hard - soft) < soft * 0.02);
}

TEST_CASE("A note at the reference velocity is unaffected by sensitivity",
          "[engine][guitar]")
{
    // The mapping is a deviation from kReferenceVelocity, so a note there is
    // excited exactly as it was before velocity affected timbre at all —
    // which is what keeps existing parts sounding as written.
    const auto without = pluckAtVelocity(GuitarString::kReferenceVelocity, 0.0f);
    const auto with    = pluckAtVelocity(GuitarString::kReferenceVelocity, 1.0f);

    REQUIRE(without.size() == with.size());
    for (size_t i = 0; i < without.size(); ++i)
        REQUIRE(without[i] == with[i]);
}

TEST_CASE("Bridge coupling makes an untouched string ring", "[engine][guitar]")
{
    // Sympathetic resonance: the point of coupling, and what makes a chord
    // bloom rather than stack up as six independent notes.
    GuitarString struck;
    struck.prepare(kSampleRate);
    struck.setFrequency(146.832);
    struck.setDecaySeconds(3.0);

    GuitarString neighbour;
    neighbour.prepare(kSampleRate);
    neighbour.setFrequency(195.998);
    neighbour.setDecaySeconds(3.0);

    REQUIRE_FALSE(neighbour.isRinging());

    struck.pluck(1.0f);

    double neighbourEnergy = 0.0;
    for (int i = 0; i < (int) (0.5 * kSampleRate); ++i)
    {
        const float bridge = struck.process();
        const float voice  = neighbour.process();

        neighbour.couple(bridge * 0.03f);
        neighbourEnergy += (double) voice * (double) voice;
    }

    REQUIRE(neighbourEnergy > 0.0);
    REQUIRE(neighbour.isRinging());
}

TEST_CASE("With no coupling the neighbour stays silent", "[engine][guitar]")
{
    // The control for the test above: without the injection there is no path
    // between two strings at all, so any energy would be a bug elsewhere.
    GuitarString struck;
    struck.prepare(kSampleRate);
    struck.setFrequency(146.832);
    struck.pluck(1.0f);

    GuitarString neighbour;
    neighbour.prepare(kSampleRate);
    neighbour.setFrequency(195.998);

    double neighbourEnergy = 0.0;
    for (int i = 0; i < (int) (0.5 * kSampleRate); ++i)
    {
        struck.process();
        const float voice = neighbour.process();
        neighbourEnergy += (double) voice * (double) voice;
    }

    REQUIRE(neighbourEnergy == 0.0);
}

TEST_CASE("A coupled pair cannot grow without bound", "[engine][guitar]")
{
    // Coupling is a feedback path between strings, so the existing "no string
    // may grow" guarantee has to hold at the coupled setting too — this is the
    // property that would fail catastrophically rather than subtly.
    GuitarString a, b;
    for (auto* string : { &a, &b })
    {
        string->prepare(kSampleRate);
        string->setDecaySeconds(30.0); // the least damped setting available
        string->setBrightness(1.0f);   // and the least lossy loop filter
    }
    a.setFrequency(82.4069);
    b.setFrequency(110.0);

    a.pluck(1.0f);
    b.pluck(1.0f);

    float peakEarly = 0.0f;
    float peakLate  = 0.0f;
    const int total = (int) (20.0 * kSampleRate);

    for (int i = 0; i < total; ++i)
    {
        const float va = a.process();
        const float vb = b.process();

        // Well above what GuitarNode ever applies (kMaxCoupling = 0.03), so
        // this is a margin check rather than a check of the exact setting.
        const float bridge = (va + vb) * 0.05f;
        a.couple(bridge);
        b.couple(bridge);

        const float magnitude = std::max(std::abs(va), std::abs(vb));
        if (i < (int) (0.5 * kSampleRate))
            peakEarly = std::max(peakEarly, magnitude);
        if (i > total - (int) (0.5 * kSampleRate))
            peakLate = std::max(peakLate, magnitude);
    }

    INFO("early " << peakEarly << " late " << peakLate);
    REQUIRE(std::isfinite(peakLate));
    REQUIRE(peakLate < peakEarly);
}
