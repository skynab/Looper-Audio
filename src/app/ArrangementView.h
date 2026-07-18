#pragma once

#include <cmath>

#include <juce_gui_basics/juce_gui_basics.h>

#include "model/Song.h"

namespace looper
{
/**
    A read-only timeline: a bars/beats ruler, one lane per track with its clips
    drawn as blocks, and a playhead that sweeps during playback. Reads a snapshot
    of the Song; the owner refreshes it on document changes and feeds the playhead
    position from the transport on a timer.
*/
class ArrangementView final : public juce::Component
{
public:
    ArrangementView() { setInterceptsMouseClicks(false, false); }

    void setSong(const model::Song& song)
    {
        song_ = song;
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

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff1e1e22));

        const float  ppb         = pixelsPerBeat();
        const double qpb         = quartersPerBar();
        const auto   width       = (float) getWidth();
        const auto   height      = (float) getHeight();
        const float  timelineX   = (float) gutterWidth_;

        // Ruler + bar lines.
        g.setColour(juce::Colours::white.withAlpha(0.06f));
        g.fillRect(0.0f, 0.0f, width, (float) rulerHeight_);
        g.setFont(juce::FontOptions(12.0f));
        for (int bar = 0; bar <= barsShown_; ++bar)
        {
            const float x = timelineX + (float) ((double) bar * qpb) * ppb;
            g.setColour(juce::Colours::white.withAlpha(0.16f));
            g.fillRect(x, 0.0f, 1.0f, height);
            g.setColour(juce::Colours::white.withAlpha(0.5f));
            g.drawText(juce::String(bar + 1), (int) x + 4, 2, 40, rulerHeight_ - 4,
                       juce::Justification::centredLeft);
        }

        // Track lanes + clips.
        for (int i = 0; i < (int) song_.tracks.size(); ++i)
        {
            const auto& track = song_.tracks[(size_t) i];
            const float y     = (float) (rulerHeight_ + i * laneHeight_);

            if (i % 2 == 0)
            {
                g.setColour(juce::Colours::white.withAlpha(0.03f));
                g.fillRect(0.0f, y, width, (float) laneHeight_);
            }

            g.setColour(juce::Colours::white.withAlpha(0.85f));
            g.setFont(juce::FontOptions(13.0f));
            g.drawText(track.name.empty() ? ("Track " + juce::String(i + 1)) : juce::String(track.name),
                       8, (int) y, gutterWidth_ - 12, laneHeight_, juce::Justification::centredLeft);

            for (const auto& clip : track.clips)
            {
                const float cx = timelineX + (float) clip.startBeats * ppb;
                const float cw = juce::jmax(2.0f, (float) clip.lengthBeats * ppb);
                const juce::Rectangle<float> r(cx, y + 3.0f, cw, (float) laneHeight_ - 6.0f);
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
        const float px = timelineX + (float) playheadBeats_ * ppb;
        if (px >= timelineX && px <= width)
        {
            g.setColour(juce::Colours::orange.withAlpha(0.9f));
            g.fillRect(px, 0.0f, 2.0f, height);
        }
    }

private:
    double quartersPerBar() const
    {
        return juce::jmax(1, song_.timeSigNumerator) * 4.0 / (double) juce::jmax(1, song_.timeSigDenominator);
    }

    float pixelsPerBeat() const
    {
        const double totalBeats = (double) barsShown_ * quartersPerBar();
        const int    timelineW  = juce::jmax(1, getWidth() - gutterWidth_);
        return totalBeats > 0.0 ? (float) ((double) timelineW / totalBeats) : 1.0f;
    }

    static constexpr int gutterWidth_ = 110;
    static constexpr int rulerHeight_ = 22;
    static constexpr int laneHeight_  = 40;
    static constexpr int barsShown_   = 8;

    model::Song song_;
    double      playheadBeats_ = 0.0;
};

} // namespace looper
