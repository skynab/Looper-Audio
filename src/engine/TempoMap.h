#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace looper::engine
{
/** A tempo in force from @p beat until the next change. */
struct TempoChange
{
    double beat = 0.0;   // quarter-note position where this tempo starts
    double bpm  = 120.0;

    bool operator==(const TempoChange&) const = default;
};

/**
    Converts between sample position and musical position (PPQ = quarter-note
    beats, and bars/beats), across a **piecewise-constant tempo map**.

    The map is a sorted list of tempo changes with an implicit first entry at
    beat 0, so a project with one tempo is the same object with one entry - and
    converts to exactly what constant-tempo arithmetic produced before this.
    That equivalence is the thing that keeps every existing project sounding
    identical, and it is asserted directly in the tests.

    **Why conversion, and not `samplesPerBeat`.** The previous version modelled
    one tempo and said its interface was "deliberately shaped so it can grow
    into a piecewise map without callers changing". It wasn't: callers asked for
    `samplesPerBeat` and multiplied, and `sampleRate * 60 / bpm` was
    hand-recomputed in eight places across the engine. A beat's length is no
    longer one number, so the only safe question is "which sample is this beat
    at?" - hence samplesPerBeatAt() takes a position, and asking without one is
    not possible.

    Segment boundaries are cached as cumulative sample offsets, so a conversion
    is a short walk over the changes rather than a rescan. Cumulative offsets
    are kept in double rather than int64 so rounding does not accumulate across
    a long song.

    Stepped changes only: tempo jumps at a change and holds. Ramps
    (accelerando, ritardando) are a strict extension of this structure - the
    same list with an interpolation flag - and deliberately are not here yet.

    No JUCE dependency, so it unit-tests fast and headless.
*/
class TempoMap
{
public:
    TempoMap() { rebuild(); }

    void setSampleRate(double sr) noexcept
    {
        if (sr > 0.0)
        {
            sampleRate_ = sr;
            rebuild();
        }
    }

    /** Sets the tempo at beat 0, leaving any later changes alone. This is what
        a single-tempo project means, and what the tempo slider drives today. */
    void setTempo(double bpm) noexcept
    {
        if (bpm > 0.0)
        {
            changes_.front().bpm = bpm;
            rebuild();
        }
    }

    /**
        Replaces the whole map. The list is sorted, changes at or before beat 0
        collapse into the first entry, and anything non-positive is dropped -
        so a caller cannot install a map that would divide by zero or run
        backwards in time.
    */
    void setTempoChanges(std::vector<TempoChange> changes)
    {
        // Sorted *first*. Seeding the start from changes.front() before
        // sorting reads whichever entry the caller happened to list first,
        // which for an unsorted map is an arbitrary tempo from the middle of
        // the song.
        std::sort(changes.begin(), changes.end(),
                  [](const TempoChange& a, const TempoChange& b) { return a.beat < b.beat; });

        changes.erase(std::remove_if(changes.begin(), changes.end(),
                                     [](const TempoChange& c) { return ! (c.bpm > 0.0); }),
                      changes.end());

        // A map whose earliest change is after beat 0 still has to say what the
        // song starts at. The earliest tempo extends backwards, which is the
        // least surprising reading and keeps the first segment from inheriting
        // an unrelated tempo.
        const double firstTempo = changes.empty() ? changes_.front().bpm : changes.front().bpm;

        std::vector<TempoChange> cleaned;
        cleaned.push_back({ 0.0, firstTempo > 0.0 ? firstTempo : 120.0 });

        for (const auto& change : changes)
        {
            if (change.beat <= 0.0)
            {
                cleaned.front().bpm = change.bpm; // an entry at or before the start *is* the start
                continue;
            }

            // Two changes at the same beat: the later one wins, since only one
            // of them can be in force.
            if (change.beat == cleaned.back().beat)
                cleaned.back().bpm = change.bpm;
            else
                cleaned.push_back(change);
        }

        changes_ = std::move(cleaned);
        rebuild();
    }

    void setTimeSignature(int num, int den) noexcept
    {
        if (num > 0) numerator_ = num;
        if (den > 0) denominator_ = den;
    }

    const std::vector<TempoChange>& tempoChanges() const noexcept { return changes_; }

    double sampleRate() const noexcept          { return sampleRate_; }
    int    timeSigNumerator() const noexcept    { return numerator_; }
    int    timeSigDenominator() const noexcept  { return denominator_; }

    /** The tempo at beat 0. Named `tempo()` because that is what it meant when
        there could only be one, and it is still what a single-tempo project's
        BPM is. */
    double tempo() const noexcept { return changes_.front().bpm; }

    double tempoAtBeat(double beat) const noexcept { return changes_[(size_t) segmentForBeat(beat)].bpm; }

    /** Samples per quarter note **at a given beat**. There is no position-free
        answer once the map can hold more than one tempo, which is why this
        cannot be asked without saying where. */
    double samplesPerBeatAt(double beat) const noexcept
    {
        return samplesPerBeatFor(tempoAtBeat(beat));
    }

    /** Quarter-note bar length (e.g. 4 for 4/4, 3 for 6/8). */
    double quartersPerBar() const noexcept { return numerator_ * 4.0 / denominator_; }

    double ppqFromSamples(int64_t samples) const noexcept
    {
        const double position = (double) samples;
        if (position <= 0.0)
            return position / samplesPerBeatFor(changes_.front().bpm);

        const int index = segmentForSamples(position);
        return changes_[(size_t) index].beat
             + (position - cumulativeSamples_[(size_t) index])
                   / samplesPerBeatFor(changes_[(size_t) index].bpm);
    }

    int64_t samplesFromPpq(double ppq) const noexcept
    {
        return (int64_t) std::llround(sampleOffsetForPpq(ppq));
    }

    /** As samplesFromPpq, without rounding to a whole sample - for arithmetic
        that then takes a difference, where rounding twice loses a sample. */
    double sampleOffsetForPpq(double ppq) const noexcept
    {
        if (ppq <= 0.0)
            return ppq * samplesPerBeatFor(changes_.front().bpm);

        const int index = segmentForBeat(ppq);
        return cumulativeSamples_[(size_t) index]
             + (ppq - changes_[(size_t) index].beat)
                   * samplesPerBeatFor(changes_[(size_t) index].bpm);
    }

    /** 1-based bar and beat (beat counted in the time signature's denominator unit) plus fractional tick. */
    struct BarsBeats
    {
        int    bar  = 1;
        int    beat = 1;
        double tick = 0.0; // 0..1 within the beat
    };

    BarsBeats barsBeatsFromPpq(double ppq) const noexcept
    {
        // Position measured in denominator-beats (e.g. eighth notes for x/8).
        const double posInBeats = ppq * denominator_ / 4.0;
        const int    bar        = (int) std::floor(posInBeats / numerator_);
        const double beatInBar  = posInBeats - (double) bar * numerator_;
        const int    beat       = (int) std::floor(beatInBar);
        return { bar + 1, beat + 1, beatInBar - beat };
    }

    BarsBeats barsBeatsFromSamples(int64_t samples) const noexcept
    {
        return barsBeatsFromPpq(ppqFromSamples(samples));
    }

private:
    double samplesPerBeatFor(double bpm) const noexcept { return sampleRate_ * 60.0 / bpm; }

    /** Index of the change in force at @p beat. */
    int segmentForBeat(double beat) const noexcept
    {
        // Linear from the end: a song has a handful of changes, and playback
        // walks forward, so the last few are the ones asked about.
        for (int i = (int) changes_.size() - 1; i > 0; --i)
            if (beat >= changes_[(size_t) i].beat)
                return i;

        return 0;
    }

    int segmentForSamples(double samples) const noexcept
    {
        for (int i = (int) cumulativeSamples_.size() - 1; i > 0; --i)
            if (samples >= cumulativeSamples_[(size_t) i])
                return i;

        return 0;
    }

    /** Cumulative sample offset at the start of each segment. */
    void rebuild()
    {
        cumulativeSamples_.assign(changes_.size(), 0.0);

        for (size_t i = 1; i < changes_.size(); ++i)
        {
            const double beats = changes_[i].beat - changes_[i - 1].beat;
            cumulativeSamples_[i] = cumulativeSamples_[i - 1]
                                  + beats * samplesPerBeatFor(changes_[i - 1].bpm);
        }
    }

    double sampleRate_  = 48000.0;
    int    numerator_   = 4;
    int    denominator_ = 4;

    // Always non-empty, and changes_[0].beat is always 0.
    std::vector<TempoChange> changes_ { { 0.0, 120.0 } };
    std::vector<double>      cumulativeSamples_ { 0.0 };
};

} // namespace looper::engine
