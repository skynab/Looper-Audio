#pragma once

#include <algorithm>
#include <cmath>
#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "engine/MidiNote.h"
#include "engine/Pattern.h"
#include "model/DrumKit.h"

#include "PianoRollGeometry.h"

namespace looper
{
/**
    A step grid of rows x time steps, with a left-hand gutter naming each row —
    a pitch (e.g. "C4") in the usual melodic mode, or a pad name ("Kick",
    "Snare", ...) in drum mode (see setDrumPads) — the same way ArrangementView
    names each of its lanes.

    Clicking an empty cell adds a note and clicking an existing one removes it:
    the original one-click-per-step behaviour, kept because it's the fastest way
    to block out a part. Everything else is additive on top of it:

      - drag a note's right edge to set its length (which then becomes the
        length new notes are added with, so a part in eighths is entered by
        resizing once);
      - drag in the velocity lane along the bottom to set a note's velocity,
        which the grid also shows as note brightness;
      - scroll the wheel to move the visible pitch range, ⌘/ctrl-scroll to zoom
        it — the grid is no longer stuck on a fixed two octaves.

    Edits fire onChange with the whole pattern, which the owner snapshots into
    the engine. Drags report only on mouse-up so that dragging a note's length
    is one undo step rather than one per pixel.
*/
class PianoRoll final : public juce::Component
{
public:
    PianoRoll() { seedDemo(); }

    std::function<void(const engine::Pattern&)> onChange;
    std::function<void(int noteNumber)>         onNotePreview; // fired when a note is *added* by clicking

    const engine::Pattern& pattern() const noexcept { return pattern_; }

    /** Replace the displayed pattern without firing onChange (used for undo/redo). */
    void setPattern(const engine::Pattern& p)
    {
        pattern_ = p;
        repaint();
    }

    /** Switches into drum mode: one row per pad, labelled and pitched by
        @p pads instead of the usual contiguous pitch range — no
        black/white shading or octave lines (neither means anything for
        pads), and no pitch scrolling since the rows *are* the kit. */
    void setDrumPads(const std::vector<model::DrumPad>& pads)
    {
        drumPads_             = pads;
        drumMode_             = true;
        geometry_.numRows     = juce::jmax(1, (int) pads.size());
        hoverRow_             = -1;
        repaint();
    }

    /** Switches back to the usual contiguous-pitch melodic mode. */
    void setMelodicMode()
    {
        if (! drumMode_)
            return;
        drumMode_ = false;
        geometry_.setPitchRange(kDefaultLowPitch, kDefaultNumRows);
        hoverRow_ = -1;
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
        dragMode_      = DragMode::None;
        dragNoteIndex_ = -1;

        if (isInVelocityLane(e.position.y))
        {
            beginVelocityDrag(e);
            return;
        }

        int row = 0, step = 0;
        if (! geometry_.cellAt(e.position.x, e.position.y, (float) getWidth(), gridHeight(), row, step))
            return;

        const int noteNumber = pitchForRow(row);
        if (noteNumber < 0)
            return; // a drum-mode row past the end of the pad list (shouldn't happen; defensive)

        const int existing = noteIndexAtCell(row, step);
        if (existing >= 0)
        {
            // Grabbing the right-hand edge resizes rather than deletes — the
            // one place a click on a note doesn't remove it.
            if (isOnResizeEdge(existing, e.position.x))
            {
                dragMode_      = DragMode::ResizeNote;
                dragNoteIndex_ = existing;
                return;
            }

            pattern_.notes.erase(pattern_.notes.begin() + existing);
            repaint();
            if (onChange)
                onChange(pattern_);
            return;
        }

        pattern_.notes.push_back({ step * geometry_.stepBeats, defaultLengthBeats_, noteNumber, defaultVelocity_ });
        if (onNotePreview)
            onNotePreview(noteNumber); // only on add, not on removing an existing note

        repaint();
        if (onChange)
            onChange(pattern_);
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (dragNoteIndex_ < 0 || dragNoteIndex_ >= (int) pattern_.notes.size())
            return;

        auto& note = pattern_.notes[(size_t) dragNoteIndex_];

        if (dragMode_ == DragMode::ResizeNote)
        {
            const float colWidth = geometry_.colWidth((float) getWidth());
            if (colWidth <= 0.0f)
                return;

            // Length snaps to whole steps, never goes below one (so a note
            // can't be dragged out of existence), and can't run past the end
            // of the pattern it lives in.
            const int   start    = stepOf(note);
            const int   maxSteps = juce::jmax(1, geometry_.numSteps - start);
            const float startX   = geometry_.xForStep(start, (float) getWidth());
            const int   steps    = juce::jlimit(1, maxSteps,
                                                (int) std::lround((e.position.x - startX) / colWidth));
            note.lengthBeats     = steps * geometry_.stepBeats;
            defaultLengthBeats_ = note.lengthBeats; // new notes inherit the length you just chose
            repaint();
        }
        else if (dragMode_ == DragMode::Velocity)
        {
            note.velocity    = velocityForY(e.position.y);
            defaultVelocity_ = note.velocity;
            repaint();
        }
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        // Drags report once, on release: resizing a note across ten pixels
        // should be one undo step, not ten.
        if (dragMode_ != DragMode::None && onChange)
            onChange(pattern_);

        dragMode_      = DragMode::None;
        dragNoteIndex_ = -1;
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        int row = 0, step = 0;
        const int newHoverRow = geometry_.cellAt(e.position.x, e.position.y,
                                                  (float) getWidth(), gridHeight(), row, step)
                                     ? row : -1;
        if (newHoverRow != hoverRow_)
        {
            hoverRow_ = newHoverRow;
            repaint();
        }

        // A resize cursor is the only hint that the edge is grabbable.
        const int overNote = newHoverRow >= 0 ? noteIndexAtCell(row, step) : -1;
        setMouseCursor(overNote >= 0 && isOnResizeEdge(overNote, e.position.x)
                           ? juce::MouseCursor::LeftRightResizeCursor
                           : juce::MouseCursor::NormalCursor);
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        if (hoverRow_ != -1)
        {
            hoverRow_ = -1;
            repaint();
        }
        setMouseCursor(juce::MouseCursor::NormalCursor);
    }

    void mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel) override
    {
        // Drum mode's rows are the kit's pads, not a pitch range, so there's
        // nothing to scroll or zoom.
        if (drumMode_ || std::abs(wheel.deltaY) < 1.0e-4f)
            return;

        const bool up = wheel.deltaY > 0.0f;
        if (e.mods.isCommandDown() || e.mods.isCtrlDown())
            geometry_.zoomBy(up ? -2 : 2); // fewer rows = taller rows = zoomed in
        else
            geometry_.scrollPitchBy(up ? 1 : -1);

        hoverRow_ = -1;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        const float w  = (float) getWidth();
        const float h  = gridHeight();
        const float cw = geometry_.colWidth(w);
        const float ch = geometry_.rowHeight(h);
        const float gx = geometry_.gutterWidth;

        g.setColour(juce::Colours::black.withAlpha(0.3f));
        g.fillRect(getLocalBounds());

        g.setFont(juce::FontOptions(11.0f));
        for (int r = 0; r < geometry_.numRows; ++r)
        {
            const bool  black    = ! drumMode_ && engine::isBlackKey(pitchForRow(r));
            const float y        = geometry_.yForRow(r, h);
            const bool  hovered  = (r == hoverRow_);

            // Gutter cell: the row's pitch (melodic) or pad (drum) name.
            g.setColour(black ? juce::Colour(0xff222226) : juce::Colour(0xff35353a));
            g.fillRect(juce::Rectangle<float>(0.0f, y, gx, ch));
            g.setColour(juce::Colours::white.withAlpha(black ? 0.55f : 0.85f));
            g.drawText(labelForRow(r), 4, (int) y, (int) gx - 6, (int) ch,
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

        // Row separators — melodic mode gets a heavier line at each octave
        // boundary (every 12 rows); drum mode has no such concept, just
        // plain separators between the handful of pads.
        for (int r = 0; r <= geometry_.numRows; ++r)
        {
            const bool heavy = ! drumMode_ && (pitchForRow(std::min(r, geometry_.numRows - 1)) % 12) == 0;
            g.setColour(juce::Colours::white.withAlpha(heavy ? 0.18f : 0.06f));
            g.fillRect(0.0f, geometry_.yForRow(r, h), w, heavy ? 2.0f : 1.0f);
        }

        g.setColour(juce::Colours::white.withAlpha(0.16f));
        g.fillRect(gx - 1.0f, 0.0f, 1.0f, h);

        for (const auto& n : pattern_.notes)
        {
            const int step = stepOf(n);
            const int row  = rowForPitch(n.noteNumber);
            if (row < 0 || row >= geometry_.numRows || step < 0 || step >= geometry_.numSteps)
                continue; // outside the visible pitch window or past the pattern's end

            // Brightness carries velocity, so the grid alone tells you which
            // hits are accented without reading the lane below.
            g.setColour(juce::Colours::limegreen.withAlpha(0.4f + 0.6f * juce::jlimit(0.0f, 1.0f, n.velocity)));
            const float span = (float) spanOf(n);
            g.fillRect(juce::Rectangle<float>(geometry_.xForStep(step, w) + 1.0f,
                                              geometry_.yForRow(row, h) + 1.0f,
                                              span * cw - 2.0f, ch - 2.0f));
        }

        paintVelocityLane(g, w, cw, gx);
    }

private:
    enum class DragMode { None, ResizeNote, Velocity };

    static constexpr int   kDefaultLowPitch    = 48; // C3
    static constexpr int   kDefaultNumRows     = 24; // two octaves
    static constexpr float kVelocityLaneHeight = 46.0f;
    static constexpr float kResizeEdgePixels   = 6.0f;

    float gridHeight() const { return juce::jmax(1.0f, (float) getHeight() - kVelocityLaneHeight); }
    float velocityLaneTop() const { return gridHeight(); }
    bool  isInVelocityLane(float y) const { return y >= velocityLaneTop(); }

    int stepOf(const engine::Note& n) const { return (int) std::llround(n.startBeats / geometry_.stepBeats); }
    int spanOf(const engine::Note& n) const
    {
        return juce::jmax(1, (int) std::llround(n.lengthBeats / geometry_.stepBeats));
    }

    /** The note occupying (row, step), or -1 — a note covers every step from
        its start for as long as it lasts, so clicking anywhere along a long
        note finds it. */
    int noteIndexAtCell(int row, int step) const
    {
        const int pitch = pitchForRow(row);
        if (pitch < 0)
            return -1;

        for (size_t i = 0; i < pattern_.notes.size(); ++i)
        {
            const auto& n = pattern_.notes[i];
            if (n.noteNumber != pitch)
                continue;
            const int start = stepOf(n);
            if (step >= start && step < start + spanOf(n))
                return (int) i;
        }
        return -1;
    }

    bool isOnResizeEdge(int noteIndex, float x) const
    {
        if (noteIndex < 0 || noteIndex >= (int) pattern_.notes.size())
            return false;
        const auto& n     = pattern_.notes[(size_t) noteIndex];
        const float right = geometry_.xForStep(stepOf(n) + spanOf(n), (float) getWidth());
        return x >= right - kResizeEdgePixels && x <= right;
    }

    /** The note whose velocity bar sits under @p x in the lane: the one
        *starting* at that step (the bar is drawn at a note's start), highest
        pitch first so a chord's topmost bar is the one you grab. */
    int noteIndexForVelocityAt(float x) const
    {
        const float colWidth = geometry_.colWidth((float) getWidth());
        if (colWidth <= 0.0f || x < geometry_.gutterWidth)
            return -1;

        const int step  = (int) ((x - geometry_.gutterWidth) / colWidth);
        int       best  = -1;
        int       bestPitch = -1;

        for (size_t i = 0; i < pattern_.notes.size(); ++i)
        {
            const auto& n = pattern_.notes[i];
            if (stepOf(n) != step || rowForPitch(n.noteNumber) < 0)
                continue;
            if (n.noteNumber > bestPitch)
            {
                bestPitch = n.noteNumber;
                best      = (int) i;
            }
        }
        return best;
    }

    void beginVelocityDrag(const juce::MouseEvent& e)
    {
        const int index = noteIndexForVelocityAt(e.position.x);
        if (index < 0)
            return;

        dragMode_      = DragMode::Velocity;
        dragNoteIndex_ = index;
        pattern_.notes[(size_t) index].velocity = velocityForY(e.position.y);
        defaultVelocity_ = pattern_.notes[(size_t) index].velocity;
        repaint();
    }

    /** Lane position -> velocity, floored just above zero: a note at velocity
        0 is silent but still drawn, which looks like a bug. */
    float velocityForY(float y) const
    {
        const float top  = velocityLaneTop();
        const float span = juce::jmax(1.0f, (float) getHeight() - top);
        return juce::jlimit(0.05f, 1.0f, 1.0f - (y - top) / span);
    }

    void paintVelocityLane(juce::Graphics& g, float w, float colWidth, float gutter)
    {
        const float top    = velocityLaneTop();
        const float height = (float) getHeight() - top;
        if (height <= 0.0f)
            return;

        g.setColour(juce::Colour(0xff1a1a1e));
        g.fillRect(juce::Rectangle<float>(0.0f, top, w, height));
        g.setColour(juce::Colours::white.withAlpha(0.16f));
        g.fillRect(0.0f, top, w, 1.0f);

        g.setColour(juce::Colours::white.withAlpha(0.5f));
        g.setFont(juce::FontOptions(10.0f));
        g.drawText("Vel", 4, (int) top, (int) gutter - 6, (int) height, juce::Justification::centredLeft);

        for (const auto& n : pattern_.notes)
        {
            const int step = stepOf(n);
            if (step < 0 || step >= geometry_.numSteps || rowForPitch(n.noteNumber) < 0)
                continue; // hidden by the current pitch window, so no bar either

            const float velocity = juce::jlimit(0.0f, 1.0f, n.velocity);
            const float barH     = juce::jmax(1.0f, velocity * (height - 4.0f));
            const float x        = geometry_.xForStep(step, w);

            g.setColour(juce::Colours::limegreen.withAlpha(0.35f + 0.65f * velocity));
            g.fillRect(juce::Rectangle<float>(x + 2.0f, top + height - 2.0f - barH,
                                              juce::jmax(2.0f, colWidth - 4.0f), barH));
        }
    }

    int pitchForRow(int row) const
    {
        if (! drumMode_)
            return geometry_.pitchForRow(row);
        return (row >= 0 && row < (int) drumPads_.size()) ? drumPads_[(size_t) row].noteNumber : -1;
    }

    int rowForPitch(int pitch) const
    {
        if (! drumMode_)
        {
            const int row = geometry_.rowForPitch(pitch);
            return (row >= 0 && row < geometry_.numRows) ? row : -1; // outside the visible window
        }
        for (size_t i = 0; i < drumPads_.size(); ++i)
            if (drumPads_[i].noteNumber == pitch)
                return (int) i;
        return -1;
    }

    juce::String labelForRow(int row) const
    {
        if (! drumMode_)
            return engine::midiNoteName(pitchForRow(row));
        return (row >= 0 && row < (int) drumPads_.size()) ? juce::String(drumPads_[(size_t) row].label)
                                                          : juce::String();
    }

    void seedDemo()
    {
        pattern_.lengthBeats = geometry_.numSteps * geometry_.stepBeats; // 4 beats = 1 bar
        const int root   = 60;                          // C4
        const int arp[]  = { 0, 4, 7, 12 };             // C E G C
        for (int i = 0; i < 4; ++i)
            pattern_.notes.push_back({ (double) i, 0.5, root + arp[i], 0.8f });
    }

    PianoRollGeometry           geometry_;
    engine::Pattern             pattern_;
    int                         hoverRow_ = -1;
    bool                        drumMode_ = false;
    std::vector<model::DrumPad> drumPads_;

    DragMode dragMode_      = DragMode::None;
    int      dragNoteIndex_ = -1;
    double   defaultLengthBeats_ = 0.25; // one step, until a resize changes it
    float    defaultVelocity_    = 0.8f;
};

} // namespace looper
