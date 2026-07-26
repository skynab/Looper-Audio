#pragma once

#include <algorithm>
#include <cmath>
#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "engine/MidiNote.h"
#include "engine/Pattern.h"

#include "PianoRollGeometry.h"

namespace looper
{
/**
    A minimal piano roll: a step grid of pitch rows x time steps, with a
    left-hand gutter naming each row's pitch (e.g. "C4"), the same way
    ArrangementView names each of its lanes. Clicking a cell toggles a
    one-step note. Edits fire onChange with the whole pattern, which the
    owner snapshots to the engine. Kept deliberately simple (fixed step
    length, no drag-resize, no scrolling/zoom yet) — enough to sequence the
    synth "on the grid" with the rows clearly labelled.
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
        if (! geometry_.cellAt(e.position.x, e.position.y, (float) getWidth(), (float) getHeight(), row, step))
            return;

        const int    noteNumber = geometry_.pitchForRow(row);
        const double start      = step * geometry_.stepBeats;

        auto it = std::find_if(pattern_.notes.begin(), pattern_.notes.end(),
                               [&](const engine::Note& n)
                               {
                                   return n.noteNumber == noteNumber
                                       && std::abs(n.startBeats - start) < 1.0e-6;
                               });

        if (it != pattern_.notes.end())
            pattern_.notes.erase(it);
        else
            pattern_.notes.push_back({ start, geometry_.stepBeats, noteNumber, 0.8f });

        repaint();
        if (onChange)
            onChange(pattern_);
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        int row = 0, step = 0;
        const int newHoverRow = geometry_.cellAt(e.position.x, e.position.y,
                                                  (float) getWidth(), (float) getHeight(), row, step)
                                     ? row : -1;
        if (newHoverRow != hoverRow_)
        {
            hoverRow_ = newHoverRow;
            repaint();
        }
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        if (hoverRow_ != -1)
        {
            hoverRow_ = -1;
            repaint();
        }
    }

    void paint(juce::Graphics& g) override
    {
        const float w  = (float) getWidth();
        const float h  = (float) getHeight();
        const float cw = geometry_.colWidth(w);
        const float ch = geometry_.rowHeight(h);
        const float gx = geometry_.gutterWidth;

        g.setColour(juce::Colours::black.withAlpha(0.3f));
        g.fillRect(getLocalBounds());

        g.setFont(juce::FontOptions(11.0f));
        for (int r = 0; r < geometry_.numRows; ++r)
        {
            const int  pitch  = geometry_.pitchForRow(r);
            const bool black  = engine::isBlackKey(pitch);
            const float y     = geometry_.yForRow(r, h);
            const bool hovered = (r == hoverRow_);

            // Gutter cell: the row's pitch name.
            g.setColour(black ? juce::Colour(0xff222226) : juce::Colour(0xff35353a));
            g.fillRect(juce::Rectangle<float>(0.0f, y, gx, ch));
            g.setColour(juce::Colours::white.withAlpha(black ? 0.55f : 0.85f));
            g.drawText(engine::midiNoteName(pitch), 4, (int) y, (int) gx - 6, (int) ch,
                       juce::Justification::centredLeft);

            // Grid lane for this row.
            g.setColour(hovered ? juce::Colours::white.withAlpha(0.10f)
                                : (black ? juce::Colours::black.withAlpha(0.28f)
                                         : juce::Colours::white.withAlpha(0.05f)));
            g.fillRect(juce::Rectangle<float>(gx, y, w - gx, ch));
        }

        const int stepsPerBeat = juce::jmax(1, (int) std::llround(1.0 / geometry_.stepBeats));
        for (int s = 0; s <= geometry_.numSteps; ++s)
        {
            const bool beat = (s % stepsPerBeat) == 0;
            g.setColour(juce::Colours::white.withAlpha(beat ? 0.25f : 0.08f));
            g.fillRect(geometry_.xForStep(s, w), 0.0f, beat ? 2.0f : 1.0f, h);
        }

        // Row separators, with a heavier line at each octave boundary (every
        // 12 rows) now that the gutter makes them visually meaningful.
        for (int r = 0; r <= geometry_.numRows; ++r)
        {
            const bool octaveBoundary = (geometry_.pitchForRow(std::min(r, geometry_.numRows - 1)) % 12) == 0;
            g.setColour(juce::Colours::white.withAlpha(octaveBoundary ? 0.18f : 0.06f));
            g.fillRect(0.0f, geometry_.yForRow(r, h), w, octaveBoundary ? 2.0f : 1.0f);
        }

        g.setColour(juce::Colours::white.withAlpha(0.16f));
        g.fillRect(gx - 1.0f, 0.0f, 1.0f, h);

        g.setColour(juce::Colours::limegreen);
        for (const auto& n : pattern_.notes)
        {
            const int step = (int) std::llround(n.startBeats / geometry_.stepBeats);
            const int row  = geometry_.rowForPitch(n.noteNumber);
            if (row < 0 || row >= geometry_.numRows || step < 0 || step >= geometry_.numSteps)
                continue;

            const float span = (float) std::max<int>(1, (int) std::llround(n.lengthBeats / geometry_.stepBeats));
            g.fillRect(juce::Rectangle<float>(geometry_.xForStep(step, w) + 1.0f, geometry_.yForRow(row, h) + 1.0f,
                                              span * cw - 2.0f, ch - 2.0f));
        }
    }

private:
    void seedDemo()
    {
        pattern_.lengthBeats = geometry_.numSteps * geometry_.stepBeats; // 4 beats = 1 bar
        const int root   = 60;                          // C4
        const int arp[]  = { 0, 4, 7, 12 };             // C E G C
        for (int i = 0; i < 4; ++i)
            pattern_.notes.push_back({ (double) i, 0.5, root + arp[i], 0.8f });
    }

    PianoRollGeometry geometry_;
    engine::Pattern   pattern_;
    int               hoverRow_ = -1;
};

} // namespace looper
