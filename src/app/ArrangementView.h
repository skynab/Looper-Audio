#pragma once

#include <algorithm>
#include <cmath>
#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "model/Song.h"

#include "ClipPreview.h"
#include "TimelineGeometry.h"

namespace looper
{
/**
    A bars/beats ruler, one lane per track with its clips drawn as blocks, and a
    playhead that sweeps during playback. Reads a snapshot of the Song; the owner
    refreshes it on document changes and feeds the playhead position from the
    transport on a timer.

    Sizes itself to the full content (song length x number of tracks, scaled by
    zoom) rather than the viewport — the owner wraps it in a juce::Viewport so
    long or heavily-zoomed timelines scroll instead of squeezing. Click empty
    ruler/lane space to seek the transport there; drag a clip to move where it
    starts (the engine now delays a track's pattern until its clip's start beat,
    so this is a real scheduling change, not just cosmetic — see Sequencer's
    clip-start gating).

    Also a juce::DragAndDropTarget for files dragged out of a FileBrowserPanel
    (identified by sourceComponent being a juce::FileTreeComponent, not by the
    drag's description string — DockRegion uses that string for its own
    panel-regrouping drags, so type is the unambiguous signal). Dropping a
    file fires onFileDropped with the beat under the drop point and the track
    lane it landed on (-1 if it landed outside every lane); the owner adds it
    to that track if it's an audio track, or otherwise creates a new one.
*/
class ArrangementView final : public juce::Component,
                              public juce::DragAndDropTarget
{
public:
    std::function<void(double)> onSeek; // beat position clicked
    std::function<void(int trackIndex, int clipIndex, double newStartBeats)> onClipMoved;
    std::function<void(int trackIndex, int clipIndex, double newLengthBeats)> onClipResized;
    std::function<void(int trackIndex, int clipIndex)> onClipSelected; // fired on press, before any drag
    std::function<void(const juce::File& file, double dropBeat, int trackIndex)> onFileDropped;

    void setSong(const model::Song& song)
    {
        song_ = song;
        updateContentSize();
        repaint();
    }

    void setPlayheadBeats(double beats)
    {
        if (std::abs(beats - playheadBeats_) > 1.0e-6)
        {
            playheadBeats_ = beats;
            repaint();
        }
    }

    void setZoom(float zoom)
    {
        geometry_.zoom = juce::jlimit(0.25f, 4.0f, zoom);
        updateContentSize();
        repaint();
    }

    float zoom() const noexcept { return geometry_.zoom; }

    /** Highlights the clip currently open in the piano roll. */
    void setSelectedClip(int trackIndex, int clipIndex)
    {
        if (selectedTrackForEdit_ != trackIndex || selectedClipForEdit_ != clipIndex)
        {
            selectedTrackForEdit_ = trackIndex;
            selectedClipForEdit_  = clipIndex;
            repaint();
        }
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff1e1e22));

        const float  ppb       = geometry_.pixelsPerBeat();
        const double qpb       = quartersPerBar();
        const auto   width     = (float) getWidth();
        const auto   height    = (float) getHeight();
        const float  timelineX = geometry_.gutterWidth;
        const int    numBars   = (int) std::ceil(totalBeats() / qpb);

        // Ruler + bar lines.
        g.setColour(juce::Colours::white.withAlpha(0.06f));
        g.fillRect(0.0f, 0.0f, width, geometry_.rulerHeight);
        g.setFont(juce::FontOptions(12.0f));
        for (int bar = 0; bar <= numBars; ++bar)
        {
            const float x = geometry_.xForBeat((double) bar * qpb);
            g.setColour(juce::Colours::white.withAlpha(0.16f));
            g.fillRect(x, 0.0f, 1.0f, height);
            g.setColour(juce::Colours::white.withAlpha(0.5f));
            g.drawText(juce::String(bar + 1), (int) x + 4, 2, 40, (int) geometry_.rulerHeight - 4,
                       juce::Justification::centredLeft);
        }

        // Track lanes + clips.
        for (int i = 0; i < (int) song_.tracks.size(); ++i)
        {
            const auto& track = song_.tracks[(size_t) i];
            const float y     = geometry_.rulerHeight + (float) i * geometry_.laneHeight;

            if (i % 2 == 0)
            {
                g.setColour(juce::Colours::white.withAlpha(0.03f));
                g.fillRect(0.0f, y, width, geometry_.laneHeight);
            }

            g.setColour(juce::Colours::white.withAlpha(0.85f));
            g.setFont(juce::FontOptions(13.0f));
            g.drawText(track.name.empty() ? ("Track " + juce::String(i + 1)) : juce::String(track.name),
                       8, (int) y, (int) geometry_.gutterWidth - 12, (int) geometry_.laneHeight,
                       juce::Justification::centredLeft);

            for (int c = 0; c < (int) track.clips.size(); ++c)
            {
                const auto&  clip          = track.clips[(size_t) c];
                const bool   isBeingDragged = dragging_ && i == dragTrackIndex_ && c == dragClipIndex_;
                const bool   isEditSelected = i == selectedTrackForEdit_ && c == selectedClipForEdit_;
                const double startBeats     = isBeingDragged ? dragPreviewStart_ : clip.startBeats;
                const double lengthBeats    = isBeingDragged ? dragPreviewLength_ : clip.lengthBeats;

                const float cx = geometry_.xForBeat(startBeats);
                const float cw = juce::jmax(2.0f, (float) lengthBeats * ppb);
                const juce::Rectangle<float> r(cx, y + 3.0f, cw, geometry_.laneHeight - 6.0f);
                g.setColour(isBeingDragged ? juce::Colour(0xff5aad64) : juce::Colour(0xff3a7d44));
                g.fillRoundedRectangle(r, 3.0f);

                paintClipContents(g, clip, r);

                // A grip along the right edge, so the resize handle is
                // visible rather than only discoverable by hovering.
                if (r.getWidth() > 3.0f * kResizeEdgePixels)
                {
                    g.setColour(juce::Colours::white.withAlpha(0.18f));
                    g.fillRect(r.getRight() - kResizeEdgePixels, r.getY() + 2.0f,
                               kResizeEdgePixels - 1.0f, r.getHeight() - 4.0f);
                }

                g.setColour(isEditSelected ? juce::Colours::cyan.withAlpha(0.9f) : juce::Colours::black.withAlpha(0.3f));
                g.drawRoundedRectangle(r, 3.0f, isEditSelected ? 2.0f : 1.0f);
            }
        }

        // Gutter separator.
        g.setColour(juce::Colours::white.withAlpha(0.12f));
        g.fillRect(timelineX - 1.0f, 0.0f, 1.0f, height);

        // Playhead.
        const float px = geometry_.xForBeat(playheadBeats_);
        if (px >= timelineX && px <= width)
        {
            g.setColour(juce::Colours::orange.withAlpha(0.9f));
            g.fillRect(px, 0.0f, 2.0f, height);
        }

        // Drop preview: a file is being dragged over the timeline.
        if (fileDragActive_)
        {
            const float dx = geometry_.xForBeat(dropPreviewBeat_);
            g.setColour(juce::Colours::cyan.withAlpha(0.5f));
            g.fillRect(dx, 0.0f, 2.0f, height);
        }
    }

    /** Draws what's inside a clip as small blocks, so two clips holding
        different music don't look identical. Audio clips are left plain:
        there is no waveform cached here, and reading the file at paint time
        is not something a paint routine should do. */
    void paintClipContents(juce::Graphics& g, const model::Clip& clip,
                           juce::Rectangle<float> bounds)
    {
        if (clip.type != model::ClipType::Instrument)
            return;

        // Below this the blocks are smaller than the corner rounding and read
        // as noise rather than as content.
        if (bounds.getWidth() < 16.0f || bounds.getHeight() < 10.0f)
            return;

        const auto area = bounds.reduced(2.0f, 3.0f);
        if (area.getWidth() <= 0.0f || area.getHeight() <= 0.0f)
            return;

        const auto blocks = clipPreviewBlocks(clip.pattern.notes, clip.pattern.lengthBeats,
                                              clip.lengthBeats);
        if (blocks.empty())
            return;

        g.setColour(juce::Colours::white.withAlpha(0.55f));

        for (const auto& block : blocks)
        {
            // At least a pixel each way: a sixteenth in a long clip rounds to
            // nothing otherwise, and a clip that looks empty is worse than
            // one that looks approximate.
            const float w = juce::jmax(1.0f, (float) block.width * area.getWidth());
            const float h = juce::jmax(1.0f, (float) block.height * area.getHeight());

            g.fillRect(area.getX() + (float) block.x * area.getWidth(),
                       area.getY() + (float) block.y * area.getHeight(),
                       w, h);
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        int trackIndex = -1, clipIndex = -1;
        if (findClipAt(e.position, trackIndex, clipIndex))
        {
            const auto& clip = song_.tracks[(size_t) trackIndex].clips[(size_t) clipIndex];

            dragging_           = true;
            resizing_           = isOnClipRightEdge(clip, e.position.x);
            dragTrackIndex_     = trackIndex;
            dragClipIndex_      = clipIndex;
            dragGrabBeat_       = geometry_.beatForX(e.position.x);
            dragOriginalStart_  = clip.startBeats;
            dragOriginalLength_ = clip.lengthBeats;
            dragPreviewStart_   = dragOriginalStart_;
            dragPreviewLength_  = dragOriginalLength_;

            if (onClipSelected)
                onClipSelected(trackIndex, clipIndex);
            return;
        }

        if (e.position.x < geometry_.gutterWidth)
            return; // clicked the track-name gutter, not the timeline

        if (onSeek)
            onSeek(geometry_.beatForX(e.position.x));
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (! dragging_)
            return;

        const double currentBeat = geometry_.beatForX(e.position.x);

        // Snap to whole beats unless alt is held — the usual DAW convention,
        // and without it a clip can't be given an exact length at all.
        const bool snap = ! e.mods.isAltDown();

        if (resizing_)
            dragPreviewLength_ = std::max(kMinClipBeats,
                                          maybeSnap(currentBeat - dragPreviewStart_, snap, kMinClipBeats));
        else
            dragPreviewStart_ = std::max(0.0, maybeSnap(dragOriginalStart_ + (currentBeat - dragGrabBeat_),
                                                        snap, 0.0));
        repaint();
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (! dragging_)
            return;

        const bool wasResizing = resizing_;
        dragging_ = false;
        resizing_ = false;

        // Only fire for an actual change — a plain click-to-select (no drag)
        // would otherwise create a harmless but noisy no-op undo step.
        if (wasResizing)
        {
            if (onClipResized && std::abs(dragPreviewLength_ - dragOriginalLength_) > 1.0e-9)
                onClipResized(dragTrackIndex_, dragClipIndex_, dragPreviewLength_);
        }
        else if (onClipMoved && std::abs(dragPreviewStart_ - dragOriginalStart_) > 1.0e-9)
        {
            onClipMoved(dragTrackIndex_, dragClipIndex_, dragPreviewStart_);
        }
        repaint();
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        // The resize cursor is the only hint the clip's edge is grabbable.
        int trackIndex = -1, clipIndex = -1;
        const bool onEdge = findClipAt(e.position, trackIndex, clipIndex)
                         && isOnClipRightEdge(song_.tracks[(size_t) trackIndex].clips[(size_t) clipIndex],
                                              e.position.x);
        setMouseCursor(onEdge ? juce::MouseCursor::LeftRightResizeCursor : juce::MouseCursor::NormalCursor);
    }

    // juce::DragAndDropTarget
    bool isInterestedInDragSource(const SourceDetails& details) override
    {
        return dynamic_cast<juce::FileTreeComponent*>(details.sourceComponent.get()) != nullptr;
    }

    void itemDragEnter(const SourceDetails& details) override
    {
        fileDragActive_  = true;
        dropPreviewBeat_ = geometry_.beatForX((float) details.localPosition.x);
        repaint();
    }

    void itemDragMove(const SourceDetails& details) override
    {
        dropPreviewBeat_ = geometry_.beatForX((float) details.localPosition.x);
        repaint();
    }

    void itemDragExit(const SourceDetails&) override
    {
        fileDragActive_ = false;
        repaint();
    }

    void itemDropped(const SourceDetails& details) override
    {
        fileDragActive_ = false;
        repaint();

        auto* fileTree = dynamic_cast<juce::FileTreeComponent*>(details.sourceComponent.get());
        if (fileTree == nullptr || fileTree->getNumSelectedFiles() == 0)
            return;

        const auto file = fileTree->getSelectedFile(0);
        if (file != juce::File{} && onFileDropped)
            onFileDropped(file, geometry_.beatForX((float) details.localPosition.x),
                         trackIndexForY((float) details.localPosition.y));
    }

private:
    /** The track lane @p y falls in, or -1 if it's above the first lane
        (the ruler) or below the last one. */
    int trackIndexForY(float y) const
    {
        if (y < geometry_.rulerHeight)
            return -1;
        const int idx = (int) ((y - geometry_.rulerHeight) / geometry_.laneHeight);
        return (idx >= 0 && idx < (int) song_.tracks.size()) ? idx : -1;
    }

    /** Finds the clip under @p pos, if any (searching by lane, then by clip rect). */
    bool findClipAt(juce::Point<float> pos, int& trackIndexOut, int& clipIndexOut) const
    {
        for (int i = 0; i < (int) song_.tracks.size(); ++i)
        {
            const float y = geometry_.rulerHeight + (float) i * geometry_.laneHeight;
            if (pos.y < y || pos.y >= y + geometry_.laneHeight)
                continue;

            const auto& track = song_.tracks[(size_t) i];
            for (int c = 0; c < (int) track.clips.size(); ++c)
            {
                const auto& clip = track.clips[(size_t) c];
                const float cx   = geometry_.xForBeat(clip.startBeats);
                const float cw   = juce::jmax(2.0f, (float) clip.lengthBeats * geometry_.pixelsPerBeat());

                if (pos.x >= cx && pos.x < cx + cw)
                {
                    trackIndexOut = i;
                    clipIndexOut  = c;
                    return true;
                }
            }
        }
        return false;
    }

    double quartersPerBar() const
    {
        return juce::jmax(1, song_.timeSigNumerator) * 4.0 / (double) juce::jmax(1, song_.timeSigDenominator);
    }

    /** The song's musical span: at least a minimum number of bars, or further if
        any clip extends past that (plus a little trailing room to work in). */
    double totalBeats() const
    {
        const double qpb    = quartersPerBar();
        double       endBeat = kMinimumBars * qpb;

        for (const auto& track : song_.tracks)
            for (const auto& clip : track.clips)
                endBeat = std::max(endBeat, clip.startBeats + clip.lengthBeats);

        return endBeat + 4.0 * qpb;
    }

    void updateContentSize()
    {
        const int numTracks = (int) song_.tracks.size();
        setSize((int) std::ceil(geometry_.contentWidth(totalBeats())),
                (int) std::ceil(geometry_.contentHeight(numTracks)));
    }

    static constexpr int kMinimumBars = 16;

    TimelineGeometry geometry_;
    model::Song      song_;
    double           playheadBeats_ = 0.0;

    static constexpr double kMinClipBeats     = 1.0;  // a clip shorter than a beat isn't useful
    static constexpr float  kResizeEdgePixels = 6.0f;

    /** Rounds to whole beats when snapping is on, with a floor so a snapped
        value can't collapse below its minimum. */
    static double maybeSnap(double beats, bool snap, double minimum)
    {
        const double snapped = snap ? std::round(beats) : beats;
        return std::max(minimum, snapped);
    }

    bool isOnClipRightEdge(const model::Clip& clip, float x) const
    {
        const float right = geometry_.xForBeat(clip.startBeats + clip.lengthBeats);
        return x >= right - kResizeEdgePixels && x <= right;
    }

    bool   dragging_          = false;
    bool   resizing_          = false;
    int    dragTrackIndex_    = -1;
    int    dragClipIndex_     = -1;
    double dragGrabBeat_       = 0.0; // beat under the mouse at grab
    double dragOriginalStart_  = 0.0; // the clip's startBeats at grab
    double dragPreviewStart_   = 0.0; // live preview while dragging
    double dragOriginalLength_ = 0.0; // the clip's lengthBeats at grab
    double dragPreviewLength_  = 0.0; // live preview while resizing

    int selectedTrackForEdit_ = -1;
    int selectedClipForEdit_  = -1;

    bool   fileDragActive_  = false;
    double dropPreviewBeat_ = 0.0;
};

} // namespace looper
