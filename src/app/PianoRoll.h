#pragma once

#include <algorithm>
#include <cmath>
#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "engine/Pattern.h"

namespace looper
{
/**
    A minimal piano roll: a step grid of pitch rows x time steps. Clicking a cell
    toggles a one-step note. Edits fire onChange with the whole pattern, which the
    owner snapshots to the engine. Kept deliberately simple (fixed step length, no
    drag-resize yet) — enough to sequence the synth "on the grid".
*/
class PianoRoll final : public juce::Component
{
public:
    PianoRoll() { seedDemo(); }

    std::function<void(const engine::Pattern&)> onChange;

    const engine::Pattern& pattern() const noexcept { return pattern_; }

    /** Replace the displayed pattern without firing onChange (used for undo/redo). */
    void setPattern(const engine::Pattern& p)
    {
        pattern_ = p;
        repaint();
    }

    void clear()
    {
        pattern_.notes.clear();
        repaint();
        if (onChange)
            onChange(pattern_);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        int row = 0, step = 0;
        if (! cellAt(e.getPosition(), row, step))
            return;

        const int    noteNumber = pitchForRow(row);
        const double start      = step * stepBeats_;

        auto it = std::find_if(pattern_.notes.begin(), pattern_.notes.end(),
                               [&](const engine::Note& n)
                               {
                                   return n.noteNumber == noteNumber
                                       && std::abs(n.startBeats - start) < 1.0e-6;
                               });

        if (it != pattern_.notes.end())
            pattern_.notes.erase(it);
        else
            pattern_.notes.push_back({ start, stepBeats_, noteNumber, 0.8f });

        repaint();
        if (onChange)
            onChange(pattern_);
    }

    void paint(juce::Graphics& g) override
    {
        const float w  = (float) getWidth();
        const float h  = (float) getHeight();
        const float cw = w / (float) numSteps_;
        const float ch = h / (float) numPitches_;

        g.setColour(juce::Colours::black.withAlpha(0.3f));
        g.fillRect(getLocalBounds());

        for (int r = 0; r < numPitches_; ++r)
        {
            const bool black = isBlackKey(pitchForRow(r));
            g.setColour(black ? juce::Colours::black.withAlpha(0.28f)
                              : juce::Colours::white.withAlpha(0.05f));
            g.fillRect(juce::Rectangle<float>(0.0f, (float) r * ch, w, ch));
        }

        for (int s = 0; s <= numSteps_; ++s)
        {
            const bool beat = (s % stepsPerBeat_) == 0;
            g.setColour(juce::Colours::white.withAlpha(beat ? 0.25f : 0.08f));
            g.fillRect((float) s * cw, 0.0f, beat ? 2.0f : 1.0f, h);
        }

        g.setColour(juce::Colours::white.withAlpha(0.06f));
        for (int r = 0; r <= numPitches_; ++r)
            g.fillRect(0.0f, (float) r * ch, w, 1.0f);

        g.setColour(juce::Colours::limegreen);
        for (const auto& n : pattern_.notes)
        {
            const int step = (int) std::llround(n.startBeats / stepBeats_);
            const int row  = rowForPitch(n.noteNumber);
            if (row < 0 || row >= numPitches_ || step < 0 || step >= numSteps_)
                continue;

            const float span = (float) std::max<int>(1, (int) std::llround(n.lengthBeats / stepBeats_));
            g.fillRect(juce::Rectangle<float>((float) step * cw + 1.0f, (float) row * ch + 1.0f,
                                              span * cw - 2.0f, ch - 2.0f));
        }
    }

private:
    bool cellAt(juce::Point<int> p, int& row, int& step) const
    {
        if (getWidth() <= 0 || getHeight() <= 0)
            return false;

        step = (int) ((float) p.x / ((float) getWidth() / (float) numSteps_));
        row  = (int) ((float) p.y / ((float) getHeight() / (float) numPitches_));
        return step >= 0 && step < numSteps_ && row >= 0 && row < numPitches_;
    }

    int pitchForRow(int row) const noexcept { return lowPitch_ + (numPitches_ - 1 - row); }
    int rowForPitch(int pitch) const noexcept { return (numPitches_ - 1) - (pitch - lowPitch_); }

    static bool isBlackKey(int midiNote)
    {
        switch (midiNote % 12)
        {
            case 1: case 3: case 6: case 8: case 10: return true;
            default:                                 return false;
        }
    }

    void seedDemo()
    {
        pattern_.lengthBeats = numSteps_ * stepBeats_; // 4 beats = 1 bar
        const int root   = 60;                          // C4
        const int arp[]  = { 0, 4, 7, 12 };             // C E G C
        for (int i = 0; i < 4; ++i)
            pattern_.notes.push_back({ (double) i, 0.5, root + arp[i], 0.8f });
    }

    static constexpr int    numPitches_   = 24;   // 2 octaves
    static constexpr int    lowPitch_     = 48;   // C3
    static constexpr int    numSteps_     = 16;   // 16 steps
    static constexpr int    stepsPerBeat_ = 4;    // 16th notes
    static constexpr double stepBeats_    = 0.25; // one step = a 16th

    engine::Pattern pattern_;
};

} // namespace looper
