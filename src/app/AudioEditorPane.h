#pragma once

#include <functional>

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include "model/Track.h"

#include "AudioSelection.h"
#include "TrackColours.h"
#include "WaveformPeaks.h"

namespace looper
{
/**
    The waveform editor: one audio clip drawn large, with a click-drag range
    selection over it.

    The arrangement's 40px lanes are for arranging — you can see that a clip
    has audio in it, but you can't pick out the gap between two words. Every
    mastering operation starts by pointing at a piece of the recording ("this
    bit is just room noise"), and that gesture needs a view where a tenth of a
    second is more than a pixel wide. Hence a pane of its own rather than
    another gesture layered onto the timeline.

    Owns no document state: it draws from a file path plus a length, and
    reports the selection through the callbacks. The actions themselves live
    in MainComponent, the same separation every other pane here keeps.
*/
class AudioEditorPane final : public juce::Component
{
public:
    /** The selected range, in seconds into the file. An empty range means the
        user has cleared the selection — actions then apply to the whole clip,
        so the two are deliberately distinguishable (see AudioRange). */
    std::function<void(AudioRange)> onSelectionChanged;

    /** Apply @p gainDb to the clip. Fired live while the slider moves; the
        drag-start/end pair around it is what makes the whole drag one undo
        step, exactly as SynthEditor and FretboardPane do. */
    std::function<void(float gainDb)> onGainChanged;
    std::function<void()>             onGainDragStart;
    std::function<void()>             onGainDragEnd;

    /** Set the clip's gain so its loudest point reaches the target peak.
        MainComponent measures the file — this pane has no sample data. */
    std::function<void()> onNormaliseRequested;

    /** Measure the noise in the current selection, to subtract later. Only
        offered when something is selected: a print taken from the whole clip
        would describe the material as much as the noise, and denoising with
        it would gut the recording. */
    std::function<void()> onCaptureNoisePrintRequested;

    /** Subtract the captured print from the whole clip. Amount is how much
        more than the measured noise to remove; floor bounds how far any one
        frequency may be attenuated (the musical-noise guard). */
    std::function<void(float amountDb, float floorDb)> onReduceNoiseRequested;

    AudioEditorPane()
    {
        placeholderLabel_.setText("Select an Audio clip to edit it", juce::dontSendNotification);
        placeholderLabel_.setJustificationType(juce::Justification::centred);
        placeholderLabel_.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.5f));
        addAndMakeVisible(placeholderLabel_);

        selectionLabel_.setFont(juce::Font(juce::FontOptions(11.0f)));
        selectionLabel_.setInterceptsMouseClicks(false, false);

        gainSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
        gainSlider_.setRange(-24.0, 24.0, 0.1);
        gainSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 60, 18);
        gainSlider_.setTextValueSuffix(" dB");
        gainSlider_.setTooltip("Level for this clip alone, on top of the track fader");
        gainSlider_.onValueChange = [this]
        {
            gainDb_ = (float) gainSlider_.getValue();
            repaint(); // the waveform follows the gain live
            if (! updating_ && onGainChanged)
                onGainChanged(gainDb_);
        };
        gainSlider_.onDragStart = [this] { if (onGainDragStart) onGainDragStart(); };
        gainSlider_.onDragEnd   = [this] { if (onGainDragEnd)   onGainDragEnd(); };

        gainLabel_.setText("Gain", juce::dontSendNotification);
        gainLabel_.setFont(juce::Font(juce::FontOptions(11.0f)));
        gainLabel_.setInterceptsMouseClicks(false, false);

        normaliseButton_.setButtonText("Normalize");
        normaliseButton_.setTooltip("Set this clip's gain so its loudest point just reaches full scale");
        normaliseButton_.onClick = [this] { if (onNormaliseRequested) onNormaliseRequested(); };

        captureNoiseButton_.setButtonText("Capture Noise Print");
        captureNoiseButton_.setTooltip("Select a passage with only background noise, then capture it");
        captureNoiseButton_.onClick = [this] { if (onCaptureNoisePrintRequested) onCaptureNoisePrintRequested(); };

        reduceNoiseButton_.setButtonText("Reduce Noise");
        reduceNoiseButton_.setTooltip("Subtract the captured noise print from the whole clip");
        reduceNoiseButton_.onClick = [this]
        {
            if (onReduceNoiseRequested)
                onReduceNoiseRequested((float) noiseAmountSlider_.getValue(),
                                       (float) noiseFloorSlider_.getValue());
        };

        noiseAmountSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
        noiseAmountSlider_.setRange(0.0, 24.0, 0.5);
        noiseAmountSlider_.setValue(12.0, juce::dontSendNotification);
        noiseAmountSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 52, 18);
        noiseAmountSlider_.setTextValueSuffix(" dB");
        noiseAmountSlider_.setTooltip("How much more than the measured noise to subtract");
        noiseAmountSlider_.onValueChange = [] {}; // read when Reduce Noise is pressed

        noiseFloorSlider_.setSliderStyle(juce::Slider::LinearHorizontal);
        noiseFloorSlider_.setRange(-48.0, -3.0, 1.0);
        noiseFloorSlider_.setValue(-24.0, juce::dontSendNotification);
        noiseFloorSlider_.setTextBoxStyle(juce::Slider::TextBoxRight, false, 52, 18);
        noiseFloorSlider_.setTextValueSuffix(" dB");
        noiseFloorSlider_.setTooltip("How far any one frequency may be attenuated - "
                                     "deeper removes more noise but risks warbling artefacts");
        noiseFloorSlider_.onValueChange = [] {}; // read when Reduce Noise is pressed

        noiseAmountLabel_.setText("Amount", juce::dontSendNotification);
        noiseAmountLabel_.setFont(juce::Font(juce::FontOptions(11.0f)));
        noiseAmountLabel_.setInterceptsMouseClicks(false, false);
        noiseFloorLabel_.setText("Floor", juce::dontSendNotification);
        noiseFloorLabel_.setFont(juce::Font(juce::FontOptions(11.0f)));
        noiseFloorLabel_.setInterceptsMouseClicks(false, false);

        zoomInButton_.setButtonText("+");
        zoomInButton_.setTooltip("Zoom in");
        zoomInButton_.onClick = [this] { zoomBy(0.5); };

        zoomOutButton_.setButtonText("-");
        zoomOutButton_.setTooltip("Zoom out");
        zoomOutButton_.onClick = [this] { zoomBy(2.0); };

        zoomFitButton_.setButtonText("Fit");
        zoomFitButton_.setTooltip("Show the whole clip");
        zoomFitButton_.onClick = [this] { zoomToFit(); };

        selectAllButton_.setButtonText("Select All");
        selectAllButton_.onClick = [this]
        {
            setSelection({ 0.0, geometry_.fileLengthSeconds });
            notifySelection();
        };

        clearSelectionButton_.setButtonText("Clear");
        clearSelectionButton_.setTooltip("Clear the selection, so actions apply to the whole clip");
        clearSelectionButton_.onClick = [this]
        {
            setSelection({});
            notifySelection();
        };

        // Same guarantee as FretboardPane and SynthEditor: a control that was
        // never parented lays out and hides perfectly while drawing nothing.
        for (auto* control : managedControls())
            addChildComponent(control);

        setContentVisible(false);
    }

    /** Shows @p file for editing. @p gainDb is the clip's current trim. */
    void setClip(const juce::File& file, double fileLengthSeconds, float gainDb,
                 const juce::String& trackName, juce::uint32 trackColour)
    {
        const bool differentFile = file != file_;

        file_        = file;
        trackName_   = trackName;
        trackColour_ = trackColour;

        geometry_.fileLengthSeconds = juce::jmax(0.0, fileLengthSeconds);

        updating_ = true;
        gainSlider_.setValue(gainDb, juce::dontSendNotification);
        updating_ = false;
        gainDb_   = gainDb;

        // A selection is a position in *this* file; carrying it across to a
        // different one would point at unrelated audio while looking
        // deliberate. Zoom is reset with it for the same reason.
        if (differentFile)
        {
            // A noise print describes one particular recording's noise floor.
            // Carrying it to another file would subtract the wrong spectrum
            // while looking deliberate — which for a destructive action is
            // the worst kind of wrong.
            selection_          = {};
            noisePrintCaptured_ = false;
            zoomToFit();
            notifySelection();
        }

        setContentVisible(true);
        repaint();
    }

    void setNoAudioClipSelected()
    {
        file_      = juce::File{};
        selection_ = {};
        setContentVisible(false);
    }

    /** The current selection, or an empty range. Read by the owner when an
        action fires rather than tracked separately. */
    AudioRange selection() const { return selection_; }

    /** The decoded peaks for the clip on show. Built by the owner (which is
        what reads files) and pushed in only when the file actually changes —
        rebuilding on every refresh would re-read the file dozens of times
        per edit. */
    void setWaveform(WaveformPeaks peaks, double sampleRate)
    {
        peaks_            = std::move(peaks);
        peaksSampleRate_  = sampleRate > 0.0 ? sampleRate : 0.0;
        repaint();
    }

    /** Whether a noise print has been captured for the clip on show. The
        owner owns the print itself; this only drives what's clickable. */
    void setNoisePrintCaptured(bool captured)
    {
        noisePrintCaptured_ = captured;
        updateNoiseControls();
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff1a1a1e));
        if (! contentVisible_)
            return;

        paintTrackHeader(g, getLocalBounds().removeFromTop(kTrackHeaderHeight),
                         trackName_, trackColour_, model::TrackType::Audio);

        const auto area = waveformArea();
        if (area.isEmpty())
            return;

        g.setColour(juce::Colour(0xff121216));
        g.fillRect(area);

        // The selection is painted under the waveform, so the waveform stays
        // fully legible inside it — a selection drawn on top dims exactly the
        // part the user is trying to look at.
        if (! selection_.isEmpty())
        {
            const float x1 = juce::jlimit((float) area.getX(), (float) area.getRight(),
                                          geometry_.xForSeconds(selection_.startSeconds));
            const float x2 = juce::jlimit((float) area.getX(), (float) area.getRight(),
                                          geometry_.xForSeconds(selection_.endSeconds));
            g.setColour(juce::Colours::cyan.withAlpha(0.22f));
            g.fillRect(juce::Rectangle<float>(x1, (float) area.getY(), x2 - x1, (float) area.getHeight()));
        }

        paintWaveform(g, area);

        if (! selection_.isEmpty())
        {
            g.setColour(juce::Colours::cyan.withAlpha(0.8f));
            for (double edge : { selection_.startSeconds, selection_.endSeconds })
            {
                const float x = geometry_.xForSeconds(edge);
                if (x >= (float) area.getX() && x <= (float) area.getRight())
                    g.drawVerticalLine((int) x, (float) area.getY(), (float) area.getBottom());
            }
        }
    }

    /** Draws the waveform as it will actually sound: scaled by the clip's
        gain, with anything that would clip marked, and the un-gained
        original ghosted behind so the edit is legible as a change. */
    void paintWaveform(juce::Graphics& g, juce::Rectangle<int> area)
    {
        if (peaks_.isEmpty() || peaksSampleRate_ <= 0.0)
        {
            g.setColour(juce::Colours::white.withAlpha(0.35f));
            g.drawText("Reading waveform...", area, juce::Justification::centred);
            return;
        }

        const float gain     = juce::Decibels::decibelsToGain(gainDb_);
        const int   channels = juce::jmax(1, peaks_.numChannels());
        const int   laneH    = area.getHeight() / channels;

        for (int ch = 0; ch < channels; ++ch)
        {
            auto lane = area.withY(area.getY() + ch * laneH).withHeight(laneH);
            paintChannel(g, lane, ch, gain);
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (! contentVisible_ || ! waveformArea().contains(e.getPosition()))
            return;

        dragAnchorSeconds_ = geometry_.secondsForX((float) e.position.x);
        dragging_          = true;

        // A plain click clears rather than selecting a zero-length range:
        // "click somewhere to deselect" is the expected gesture, and an empty
        // range is exactly how the rest of the pane spells "no selection".
        setSelection({});
        notifySelection();
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (! dragging_)
            return;

        setSelection(AudioRange::fromDrag(dragAnchorSeconds_,
                                          geometry_.secondsForX((float) e.position.x))
                         .clampedTo(geometry_.fileLengthSeconds));
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (! dragging_)
            return;

        dragging_ = false;
        notifySelection();
    }

    void resized() override
    {
        placeholderLabel_.setBounds(getLocalBounds());
        if (! contentVisible_)
            return;

        auto area = getLocalBounds();
        area.removeFromTop(kTrackHeaderHeight);
        area = area.reduced(6);

        auto toolRow = area.removeFromTop(kToolRowHeight);

        // The readout only gets a share once there's enough width for the
        // buttons to still be usable; below that it's dropped to zero and the
        // buttons take the row. A Label at zero width is invisible but
        // harmless, where a *button* at zero width is present, hit-tests
        // against nothing, and is indistinguishable from one that doesn't
        // work — the failure FretboardPane's proportional row exists to
        // prevent, and which fixed widths here reproduced exactly.
        const int labelWidth = toolRow.getWidth() > kLabelNeedsWidth ? kSelectionLabelWidth : 0;
        selectionLabel_.setBounds(toolRow.removeFromRight(labelWidth).reduced(4, 0));

        // Divide what's actually left rather than imposing minimums, so the
        // buttons shrink together instead of the last ones falling off.
        juce::Component* toolButtons[] { &zoomOutButton_, &zoomInButton_, &zoomFitButton_,
                                         &selectAllButton_, &clearSelectionButton_ };
        const int buttonCount = (int) std::size(toolButtons);
        for (int i = 0; i < buttonCount; ++i)
        {
            const int remaining = buttonCount - i;
            const int width     = juce::jmax(1, toolRow.getWidth() / remaining);
            toolButtons[i]->setBounds(toolRow.removeFromLeft(width).reduced(1));
        }

        // Denoise sits above the gain row, in workflow order: capture a
        // print, then reduce, then set the level.
        auto noiseRow = area.removeFromBottom(kToolRowHeight);
        {
            juce::Component* noiseControls[] { &captureNoiseButton_, &reduceNoiseButton_ };
            const int count = (int) std::size(noiseControls);
            const int forButtons = juce::jmax(2, noiseRow.getWidth() / 2);
            auto buttons = noiseRow.removeFromLeft(forButtons);
            for (int i = 0; i < count; ++i)
            {
                const int remaining = count - i;
                const int width     = juce::jmax(1, buttons.getWidth() / remaining);
                noiseControls[i]->setBounds(buttons.removeFromLeft(width).reduced(1));
            }

            // A label capped at a *third* of its section, never a fixed
            // width: at a narrow pane a fixed-width label eats the whole
            // section and leaves the slider at zero — present, draggable
            // against nothing. Same failure the tool row above already had.
            auto layoutLabelled = [](juce::Rectangle<int> section, int maxLabel,
                                     juce::Label& label, juce::Slider& slider)
            {
                label.setBounds(section.removeFromLeft(juce::jmin(maxLabel, section.getWidth() / 3)));
                slider.setBounds(section.reduced(2, 1));
            };

            layoutLabelled(noiseRow.removeFromLeft(juce::jmax(2, noiseRow.getWidth() / 2)),
                           48, noiseAmountLabel_, noiseAmountSlider_);
            layoutLabelled(noiseRow, 36, noiseFloorLabel_, noiseFloorSlider_);
        }

        auto gainRow = area.removeFromBottom(kToolRowHeight);
        normaliseButton_.setBounds(gainRow.removeFromRight(juce::jmax(1, gainRow.getWidth() / 3))
                                       .reduced(1));
        gainLabel_.setBounds(gainRow.removeFromLeft(juce::jmin(36, gainRow.getWidth())));
        gainSlider_.setBounds(gainRow.reduced(2, 1));

        // Whatever is left is the waveform, and the geometry is told where it
        // starts so a pixel means the same thing in paint() and in a click.
        geometry_.contentLeft = (float) area.getX();
        geometry_.visibleStartSeconds =
            geometry_.clampedStart(geometry_.visibleStartSeconds, (float) area.getWidth());
    }

private:
    static constexpr int kToolRowHeight = 24;

    /** The selection readout's width, and the row width below which it is
        dropped entirely so the buttons keep usable sizes. */
    static constexpr int kSelectionLabelWidth = 240;
    static constexpr int kLabelNeedsWidth     = 420;

    juce::Rectangle<int> waveformArea() const
    {
        auto area = getLocalBounds();
        area.removeFromTop(kTrackHeaderHeight);
        area = area.reduced(6);
        area.removeFromTop(kToolRowHeight);
        area.removeFromBottom(kToolRowHeight);
        return area;
    }

    /** One channel's lane: gridlines, the original ghosted, the gained
        waveform, and clipping marked. */
    void paintChannel(juce::Graphics& g, juce::Rectangle<int> lane, int channel, float gain)
    {
        const float centreY = (float) lane.getCentreY();
        const float halfH   = (float) lane.getHeight() * 0.5f;

        // dBFS gridlines. Levels are judged in decibels, and a linear
        // waveform with no reference makes -6 and -12 look nearly identical.
        for (float db : { -6.0f, -12.0f, -18.0f })
        {
            const float fraction = juce::Decibels::decibelsToGain(db);
            g.setColour(juce::Colours::white.withAlpha(0.07f));
            for (float sign : { -1.0f, 1.0f })
                g.drawHorizontalLine((int) (centreY + sign * fraction * halfH),
                                     (float) lane.getX(), (float) lane.getRight());
        }

        // Full scale, drawn brighter — the line the gained waveform must not
        // cross.
        g.setColour(juce::Colours::white.withAlpha(0.16f));
        g.drawHorizontalLine(lane.getY(), (float) lane.getX(), (float) lane.getRight());
        g.drawHorizontalLine(lane.getBottom() - 1, (float) lane.getX(), (float) lane.getRight());

        // Centre line, so a silent passage is visibly silent rather than
        // merely thin — the whole judgement being made when picking a noise
        // print.
        g.setColour(juce::Colours::white.withAlpha(0.14f));
        g.drawHorizontalLine((int) centreY, (float) lane.getX(), (float) lane.getRight());

        const double secondsPerPixel = geometry_.secondsPerPixel > 0.0 ? geometry_.secondsPerPixel : 1.0e-9;
        const bool   showGhost       = std::abs(gainDb_) > 0.05f;

        for (int x = lane.getX(); x < lane.getRight(); ++x)
        {
            const double startSeconds = geometry_.visibleStartSeconds
                                      + (double) (x - (int) geometry_.contentLeft) * secondsPerPixel;
            const double endSeconds   = startSeconds + secondsPerPixel;

            const int from = (int) (startSeconds * peaksSampleRate_);
            const int to   = (int) (endSeconds * peaksSampleRate_);
            if (to <= 0 || from >= peaks_.totalSamples())
                continue;

            const auto bin = peaks_.range(channel, juce::jmax(0, from), juce::jmax(1, to));
            if (bin.isEmpty())
                continue;

            // The original, behind: without something to compare against, a
            // quieter waveform just looks like a quieter recording.
            if (showGhost)
            {
                g.setColour(juce::Colours::white.withAlpha(0.16f));
                g.drawVerticalLine(x, centreY - bin.maximum * halfH, centreY - bin.minimum * halfH);
            }

            const float top    = juce::jlimit(-1.0f, 1.0f, bin.maximum * gain);
            const float bottom = juce::jlimit(-1.0f, 1.0f, bin.minimum * gain);
            const bool  clips  = bin.magnitude() * gain > 1.0f;

            // Clipping in red rather than silently flattened at the lane
            // edge: a waveform that just touches the top looks the same
            // whether it is at full scale or 6dB past it, and those are very
            // different problems.
            g.setColour(clips ? juce::Colours::red
                              : juce::Colours::aquamarine.withAlpha(0.85f));
            g.drawVerticalLine(x, centreY - top * halfH, centreY - bottom * halfH);
        }
    }

    void setSelection(AudioRange range)
    {
        selection_ = range;
        updateSelectionLabel();
        updateNoiseControls();
        repaint();
    }

    /** Capture needs something selected; reduce needs something captured.
        Spelling the workflow out in the controls is what stops "Reduce
        Noise" being a button that silently does nothing. */
    void updateNoiseControls()
    {
        captureNoiseButton_.setEnabled(! selection_.isEmpty());
        reduceNoiseButton_.setEnabled(noisePrintCaptured_);
        noiseAmountSlider_.setEnabled(noisePrintCaptured_);
        noiseFloorSlider_.setEnabled(noisePrintCaptured_);
    }

    void notifySelection()
    {
        if (onSelectionChanged)
            onSelectionChanged(selection_);
    }

    void updateSelectionLabel()
    {
        if (selection_.isEmpty())
        {
            selectionLabel_.setText("No selection (actions apply to the whole clip)",
                                    juce::dontSendNotification);
            return;
        }

        selectionLabel_.setText(juce::String(selection_.startSeconds, 3) + "s - "
                                    + juce::String(selection_.endSeconds, 3) + "s  ("
                                    + juce::String(selection_.lengthSeconds(), 3) + "s)",
                                juce::dontSendNotification);
    }

    void zoomBy(double factor)
    {
        const auto area = waveformArea();
        if (area.isEmpty())
            return;

        // Anchored on the centre of the view, so zooming doesn't wander off
        // whatever the user was looking at.
        const double centre  = geometry_.visibleStartSeconds
                             + geometry_.visibleSeconds((float) area.getWidth()) * 0.5;
        const double fitPerPixel = geometry_.secondsPerPixelToFit((float) area.getWidth());

        geometry_.secondsPerPixel =
            juce::jlimit(1.0e-6, juce::jmax(1.0e-6, fitPerPixel), geometry_.secondsPerPixel * factor);

        geometry_.visibleStartSeconds =
            geometry_.clampedStart(centre - geometry_.visibleSeconds((float) area.getWidth()) * 0.5,
                                   (float) area.getWidth());
        repaint();
    }

    void zoomToFit()
    {
        const auto area = waveformArea();
        if (area.isEmpty())
            return;

        geometry_.secondsPerPixel     = geometry_.secondsPerPixelToFit((float) area.getWidth());
        geometry_.visibleStartSeconds = 0.0;
        repaint();
    }

    /** Every control this pane shows and hides, in one place — see the same
        list in FretboardPane and SynthEditor, and the bug that made it worth
        having. */
    std::vector<juce::Component*> managedControls()
    {
        return { &selectionLabel_, &gainLabel_, &gainSlider_, &normaliseButton_,
                 &zoomInButton_, &zoomOutButton_, &zoomFitButton_,
                 &selectAllButton_, &clearSelectionButton_,
                 &captureNoiseButton_, &reduceNoiseButton_,
                 &noiseAmountLabel_, &noiseAmountSlider_,
                 &noiseFloorLabel_, &noiseFloorSlider_ };
    }

    void setContentVisible(bool visible)
    {
        contentVisible_ = visible;
        placeholderLabel_.setVisible(! visible);

        for (auto* c : managedControls())
            c->setVisible(visible);

        updateSelectionLabel();
        updateNoiseControls();
        resized();
        repaint();
    }

    AudioSelection geometry_;
    AudioRange     selection_;

    juce::File   file_;
    juce::String trackName_;
    juce::uint32 trackColour_    = 0;
    bool         contentVisible_ = false;
    bool         updating_       = false;
    bool         dragging_       = false;
    double       dragAnchorSeconds_ = 0.0;

    juce::Label      placeholderLabel_;
    juce::Label      selectionLabel_, gainLabel_;
    juce::Slider     gainSlider_;
    juce::TextButton normaliseButton_, zoomInButton_, zoomOutButton_, zoomFitButton_;
    juce::TextButton selectAllButton_, clearSelectionButton_;
    juce::TextButton captureNoiseButton_, reduceNoiseButton_;
    juce::Label      noiseAmountLabel_, noiseFloorLabel_;
    juce::Slider     noiseAmountSlider_, noiseFloorSlider_;
    bool             noisePrintCaptured_ = false;
    WaveformPeaks    peaks_;
    double           peaksSampleRate_ = 0.0;
    float            gainDb_          = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEditorPane)
};

} // namespace looper
