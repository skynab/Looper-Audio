#pragma once

#include <array>
#include <functional>

#include <juce_gui_basics/juce_gui_basics.h>

#include "engine/MidiNote.h"
#include "model/GuitarSettings.h"

namespace looper
{
/**
    The guitar's own interface: six strings across, frets down the neck.

    A piano roll can express the notes a guitar plays but not *how* it plays
    them — which string, which fret, and the fact that only one note sounds per
    string at a time. The fretboard is the view where that is self-evident: the
    note currently ringing on each string is lit, so a second note on the same
    string visibly takes the first one's place rather than mysteriously
    silencing it.

    Laid out like tablature, high E at the top and low E at the bottom, because
    that is the orientation anyone reading guitar notation already has.

    Owns no document state: it draws from a settings snapshot plus the engine's
    ringing-note readout, and reports intent through the callbacks.
*/
class FretboardPane final : public juce::Component
{
public:
    std::function<void(int midiNote)>                    onFretPlayed;
    std::function<void(const model::GuitarSettings&)>    onSettingsChanged;

    FretboardPane()
    {
        placeholder_.setText("Select a Guitar track to play it", juce::dontSendNotification);
        placeholder_.setJustificationType(juce::Justification::centred);
        placeholder_.setColour(juce::Label::textColourId, juce::Colours::white.withAlpha(0.5f));
        addAndMakeVisible(placeholder_);

        // Tuning, one box per string. Explicit note names rather than a
        // custom click-to-nudge gesture: there's no hit-testing to get wrong,
        // and "drop D" is then visibly one box changed.
        for (int s = 0; s < model::kNumGuitarStrings; ++s)
        {
            auto& box = tuningBoxes_[(size_t) s];
            for (int note = kLowestTuning; note <= kHighestTuning; ++note)
                box.addItem(engine::midiNoteName(note), note); // ids are the note numbers
            box.onChange = [this] { pushSettings(); };
            addChildComponent(box);
        }

        setupSlider(decay_, 0.2, 12.0, 0.1, " s", [this] { pushSettings(); });
        setupSlider(brightness_, 0.0, 100.0, 1.0, " %", [this] { pushSettings(); });
        setupSlider(pickPosition_, 2.0, 50.0, 1.0, " %", [this] { pushSettings(); });
        setupSlider(pickHardness_, 0.0, 100.0, 1.0, " %", [this] { pushSettings(); });
        setupSlider(muteOnRelease_, 0.0, 100.0, 1.0, " %", [this] { pushSettings(); });

        setupLabel(decayLabel_, "Decay");
        setupLabel(brightnessLabel_, "Bright");
        setupLabel(pickPositionLabel_, "Pick pos");
        setupLabel(pickHardnessLabel_, "Pick");
        setupLabel(muteOnReleaseLabel_, "Damp off");

        setContentVisible(false);
    }

    void setSettings(const model::GuitarSettings& settings)
    {
        settings_ = settings;
        updating_ = true;

        for (int s = 0; s < model::kNumGuitarStrings; ++s)
            tuningBoxes_[(size_t) s].setSelectedId(settings.tuning[(size_t) s], juce::dontSendNotification);

        decay_.setValue(settings.decaySeconds, juce::dontSendNotification);
        brightness_.setValue(settings.brightness * 100.0, juce::dontSendNotification);
        pickPosition_.setValue(settings.pickPosition * 100.0, juce::dontSendNotification);
        pickHardness_.setValue(settings.pickHardness * 100.0, juce::dontSendNotification);
        muteOnRelease_.setValue(settings.muteOnNoteOff * 100.0, juce::dontSendNotification);

        updating_ = false;
        setContentVisible(true);
    }

    void setNoGuitarTrackSelected() { setContentVisible(false); }

    /** What each string is currently sounding, straight from the engine — the
        pane can't infer it, since a string keeps ringing after its note-off. */
    void setRingingNotes(const std::array<int, model::kNumGuitarStrings>& notes)
    {
        if (ringing_ != notes)
        {
            ringing_ = notes;
            repaint();
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (! contentVisible_)
            return;

        int stringIndex = 0, fret = 0;
        if (! fretAt(e.position, stringIndex, fret))
            return;

        const int note = settings_.tuning[(size_t) stringIndex] + fret;
        if (note >= 0 && note <= 127 && onFretPlayed)
            onFretPlayed(note);
    }

    void paint(juce::Graphics& g) override
    {
        g.fillAll(juce::Colour(0xff1a1a1e));
        if (! contentVisible_)
            return;

        const auto board = boardArea();
        if (board.getHeight() <= 0 || board.getWidth() <= 0)
            return;

        const float rowHeight = (float) board.getHeight() / (float) model::kNumGuitarStrings;
        const float fretWidth = (float) board.getWidth() / (float) (kNumFrets + 1);

        // Position dots, where they sit on a real neck — the only thing that
        // makes a grid of identical cells navigable at a glance.
        g.setColour(juce::Colours::white.withAlpha(0.07f));
        for (int fret : { 3, 5, 7, 9, 12, 15, 17, 19, 21, 24 })
        {
            const auto x = (float) board.getX() + (float) fret * fretWidth;
            g.fillRect(juce::Rectangle<float>(x, (float) board.getY(), fretWidth, (float) board.getHeight()));
        }

        for (int row = 0; row < model::kNumGuitarStrings; ++row)
        {
            // Row 0 is the top of the display, which in tab is the *highest*
            // string — so the display order is the reverse of the model's.
            const int   stringIndex = model::kNumGuitarStrings - 1 - row;
            const float y           = (float) board.getY() + (float) row * rowHeight;
            const int   openNote    = settings_.tuning[(size_t) stringIndex];
            const int   sounding    = ringing_[(size_t) stringIndex];

            // Thicker line for the lower strings, as on the instrument.
            g.setColour(juce::Colours::white.withAlpha(0.22f));
            g.fillRect((float) board.getX(), y + rowHeight * 0.5f,
                       (float) board.getWidth(), 1.0f + 0.4f * (float) stringIndexToThickness(stringIndex));

            for (int fret = 0; fret <= kNumFrets; ++fret)
            {
                const auto cell = juce::Rectangle<float>((float) board.getX() + (float) fret * fretWidth,
                                                         y, fretWidth, rowHeight).reduced(1.0f);

                if (sounding == openNote + fret)
                {
                    // The note this string is actually ringing right now.
                    g.setColour(juce::Colours::limegreen);
                    g.fillRoundedRectangle(cell, 3.0f);
                    g.setColour(juce::Colours::black.withAlpha(0.7f));
                }
                else if (fret == hoverFret_ && stringIndex == hoverString_)
                {
                    g.setColour(juce::Colours::white.withAlpha(0.14f));
                    g.fillRoundedRectangle(cell, 3.0f);
                    g.setColour(juce::Colours::white.withAlpha(0.8f));
                }
                else
                {
                    g.setColour(juce::Colours::white.withAlpha(fret == 0 ? 0.55f : 0.28f));
                }

                g.setFont(juce::FontOptions(9.5f));
                g.drawText(engine::midiNoteName(openNote + fret), cell, juce::Justification::centred);
            }
        }

        // Nut: the open-string column is the instrument, not a fret.
        g.setColour(juce::Colours::white.withAlpha(0.45f));
        g.fillRect((float) board.getX() + fretWidth - 1.0f, (float) board.getY(),
                   2.0f, (float) board.getHeight());
    }

    void mouseMove(const juce::MouseEvent& e) override
    {
        int stringIndex = -1, fret = -1;
        if (! fretAt(e.position, stringIndex, fret))
            stringIndex = fret = -1;

        if (stringIndex != hoverString_ || fret != hoverFret_)
        {
            hoverString_ = stringIndex;
            hoverFret_   = fret;
            repaint();
        }
    }

    void mouseExit(const juce::MouseEvent&) override
    {
        hoverString_ = hoverFret_ = -1;
        repaint();
    }

    void resized() override
    {
        placeholder_.setBounds(getLocalBounds());
        if (! contentVisible_)
            return;

        auto area = getLocalBounds().reduced(6);

        auto tuningRow = area.removeFromTop(kTuningHeight);
        const int boxWidth = juce::jmax(40, tuningRow.getWidth() / model::kNumGuitarStrings);
        for (int s = 0; s < model::kNumGuitarStrings; ++s)
        {
            // Left to right as low-to-high, matching how a player names them
            // ("E A D G B e"), even though the board draws high at the top.
            tuningBoxes_[(size_t) s].setBounds(tuningRow.removeFromLeft(boxWidth).reduced(2));
        }

        auto tone = area.removeFromBottom(kToneHeight);
        auto row  = [&tone](juce::Label& label, juce::Slider& slider)
        {
            auto r = tone.removeFromTop(22);
            label.setBounds(r.removeFromLeft(64));
            slider.setBounds(r.reduced(2, 1));
        };
        row(decayLabel_, decay_);
        row(brightnessLabel_, brightness_);
        row(pickPositionLabel_, pickPosition_);
        row(pickHardnessLabel_, pickHardness_);
        row(muteOnReleaseLabel_, muteOnRelease_);
    }

private:
    static constexpr int kNumFrets      = 22; // 0 (open) through 22
    static constexpr int kTuningHeight  = 26;
    static constexpr int kToneHeight    = 5 * 22;
    static constexpr int kLowestTuning  = 28; // E1, low enough for any drop tuning
    static constexpr int kHighestTuning = 67;

    /** Lower strings are drawn thicker, as on the instrument. */
    static int stringIndexToThickness(int stringIndex) { return model::kNumGuitarStrings - 1 - stringIndex; }

    juce::Rectangle<int> boardArea() const
    {
        auto area = getLocalBounds().reduced(6);
        area.removeFromTop(kTuningHeight);
        area.removeFromBottom(kToneHeight);
        return area;
    }

    bool fretAt(juce::Point<float> point, int& stringOut, int& fretOut) const
    {
        const auto board = boardArea();
        if (! board.toFloat().contains(point))
            return false;

        const float rowHeight = (float) board.getHeight() / (float) model::kNumGuitarStrings;
        const float fretWidth = (float) board.getWidth() / (float) (kNumFrets + 1);
        if (rowHeight <= 0.0f || fretWidth <= 0.0f)
            return false;

        const int row  = juce::jlimit(0, model::kNumGuitarStrings - 1,
                                      (int) ((point.y - (float) board.getY()) / rowHeight));
        const int fret = juce::jlimit(0, kNumFrets,
                                      (int) ((point.x - (float) board.getX()) / fretWidth));

        stringOut = model::kNumGuitarStrings - 1 - row; // display is high-to-low
        fretOut   = fret;
        return true;
    }

    void setupSlider(juce::Slider& slider, double lo, double hi, double step,
                     const juce::String& suffix, std::function<void()> onChange)
    {
        slider.setSliderStyle(juce::Slider::LinearHorizontal);
        slider.setRange(lo, hi, step);
        slider.setTextBoxStyle(juce::Slider::TextBoxRight, false, 64, 18);
        slider.setTextValueSuffix(suffix);
        slider.onValueChange = std::move(onChange);
        addChildComponent(slider);
    }

    void setupLabel(juce::Label& label, const juce::String& text)
    {
        label.setText(text, juce::dontSendNotification);
        label.setFont(juce::Font(juce::FontOptions(11.0f)));
        label.setInterceptsMouseClicks(false, false);
        addChildComponent(label);
    }

    /** Reads the controls back into the settings and reports them. Guarded
        against setSettings's own setValue calls, which would otherwise echo
        straight back as a user edit. */
    void pushSettings()
    {
        if (updating_ || ! onSettingsChanged)
            return;

        for (int s = 0; s < model::kNumGuitarStrings; ++s)
        {
            const int id = tuningBoxes_[(size_t) s].getSelectedId();
            if (id > 0)
                settings_.tuning[(size_t) s] = id;
        }

        settings_.decaySeconds  = (float) decay_.getValue();
        settings_.brightness    = (float) (brightness_.getValue() / 100.0);
        settings_.pickPosition  = (float) (pickPosition_.getValue() / 100.0);
        settings_.pickHardness  = (float) (pickHardness_.getValue() / 100.0);
        settings_.muteOnNoteOff = (float) (muteOnRelease_.getValue() / 100.0);

        onSettingsChanged(settings_);
        repaint(); // a tuning change relabels the whole board
    }

    void setContentVisible(bool visible)
    {
        contentVisible_ = visible;
        placeholder_.setVisible(! visible);

        for (auto& box : tuningBoxes_)
            box.setVisible(visible);

        juce::Component* tone[] = { &decayLabel_, &decay_, &brightnessLabel_, &brightness_,
                                    &pickPositionLabel_, &pickPosition_, &pickHardnessLabel_,
                                    &pickHardness_, &muteOnReleaseLabel_, &muteOnRelease_ };
        for (auto* c : tone)
            c->setVisible(visible);

        resized();
        repaint();
    }

    model::GuitarSettings                      settings_;
    std::array<int, model::kNumGuitarStrings>  ringing_ { -1, -1, -1, -1, -1, -1 };
    bool                                       contentVisible_ = false;
    bool                                       updating_       = false;
    int                                        hoverString_    = -1;
    int                                        hoverFret_      = -1;

    juce::Label     placeholder_;
    std::array<juce::ComboBox, model::kNumGuitarStrings> tuningBoxes_;
    juce::Label     decayLabel_, brightnessLabel_, pickPositionLabel_, pickHardnessLabel_, muteOnReleaseLabel_;
    juce::Slider    decay_, brightness_, pickPosition_, pickHardness_, muteOnRelease_;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(FretboardPane)
};

} // namespace looper
