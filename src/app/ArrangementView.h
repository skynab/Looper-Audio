#pragma once

#include <algorithm>
#include <cmath>
#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "model/Song.h"

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
    long or heavily-zoomed timelines scroll instead of squeezing. Click anywhere
    in the ruler/lane area to seek the transport there.

    Note: dragging clips to new positions is not implemented yet — the engine
    currently always loops each track's pattern from the timeline origin, so a
    clip's on-screen position is not yet a real scheduling input. Making that
    true is a separate, deliberately-deferred engine change (see the "clip
    position gates playback" note in the roadmap).
*/
class ArrangementView final : public juce::Component
{
public:
    std::function<void(double)> onSeek; // beat position clicked

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

            for (const auto& clip : track.clips)
            {
                const float cx = geometry_.xForBeat(clip.startBeats);
                const float cw = juce::jmax(2.0f, (float) clip.lengthBeats * ppb);
                const juce::Rectangle<float> r(cx, y + 3.0f, cw, geometry_.laneHeight - 6.0f);
                g.setColour(juce::Colour(0xff3a7d44));
                g.fillRoundedRectangle(r, 3.0f);
                g.setColour(juce::Colours::black.withAlpha(0.3f));
                g.drawRoundedRectangle(r, 3.0f, 1.0f);
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
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if ((float) e.x < geometry_.gutterWidth)
            return; // clicked the track-name gutter, not the timeline

        if (onSeek)
            onSeek(geometry_.beatForX((float) e.x));
    }

private:
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
};

} // namespace looper
