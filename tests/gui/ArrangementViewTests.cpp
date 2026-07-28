#include <catch2/catch_test_macros.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <app/ArrangementView.h>
#include <app/TrackColours.h>

#include <algorithm>
#include <string>

using namespace looper;

namespace
{
    struct JuceFixture
    {
        juce::ScopedJuceInitialiser_GUI juce;
    };
}

TEST_CASE("Timeline zoom stays inside its range", "[gui][arrangement]")
{
    JuceFixture fixture;
    ArrangementView view;

    view.setZoom(1000.0f);
    REQUIRE(view.zoom() == ArrangementView::kMaxZoom);

    view.setZoom(0.0f);
    REQUIRE(view.zoom() == ArrangementView::kMinZoom);
}

TEST_CASE("The zoom buttons know when they would do nothing", "[gui][arrangement]")
{
    // This is what greys them out. A button that silently does nothing at the
    // limit is indistinguishable from one that's broken — which is roughly
    // what "I don't understand what these are doing" describes.
    JuceFixture fixture;
    ArrangementView view;

    view.setZoom(ArrangementView::kMaxZoom);
    REQUIRE_FALSE(view.canZoomIn());
    REQUIRE(view.canZoomOut());

    view.setZoom(ArrangementView::kMinZoom);
    REQUIRE(view.canZoomIn());
    REQUIRE_FALSE(view.canZoomOut());

    view.setZoom(1.0f);
    REQUIRE(view.canZoomIn());
    REQUIRE(view.canZoomOut());
}

TEST_CASE("Zooming changes how much timeline a pixel covers", "[gui][arrangement]")
{
    // The whole point of the control: if this didn't change, the buttons
    // really would be doing nothing.
    JuceFixture fixture;
    ArrangementView view;
    view.setSize(800, 400);

    view.setZoom(1.0f);
    const double beatAtMiddleNormal = view.beatForXForTesting(400.0f);

    view.setZoom(2.0f);
    const double beatAtMiddleZoomed = view.beatForXForTesting(400.0f);

    REQUIRE(beatAtMiddleZoomed < beatAtMiddleNormal);
}

namespace
{
    /** A song with @p count tracks, each holding one clip. */
    model::Song songWithTracks(int count)
    {
        model::Song song;
        for (int i = 0; i < count; ++i)
        {
            const int id = model::addTrack(song, model::TrackType::Instrument,
                                           "Track " + std::to_string(i + 1)).id;
            model::Clip clip;
            clip.type                = model::ClipType::Instrument;
            clip.lengthBeats         = 4.0;
            clip.pattern.lengthBeats = 4.0;
            model::addClip(song, id, clip);
        }
        return song;
    }

    std::unique_ptr<ArrangementView> viewWith(int trackCount)
    {
        auto view = std::make_unique<ArrangementView>();
        view->setVisible(true);
        view->setSize(900, 500);
        view->setSong(songWithTracks(trackCount));
        return view;
    }
}

TEST_CASE("Every track gets a mute button, inside the gutter", "[gui][arrangement]")
{
    JuceFixture fixture;
    auto view = viewWith(4);

    for (int i = 0; i < 4; ++i)
    {
        const auto bounds = view->muteButtonBoundsForTesting(i);
        INFO("track " << i << " mute at " << bounds.toString());

        REQUIRE(bounds.getWidth() > 0.0f);
        REQUIRE(bounds.getHeight() > 0.0f);
        REQUIRE(bounds.getRight() <= view->gutterWidthForTesting()); // clear of the timeline
        REQUIRE(bounds.getX() >= 0.0f);
    }
}

TEST_CASE("Mute buttons don't overlap each other", "[gui][arrangement]")
{
    // One per lane: overlapping ones would mute the wrong track at the edges.
    JuceFixture fixture;
    auto view = viewWith(5);

    for (int i = 1; i < 5; ++i)
    {
        const auto above = view->muteButtonBoundsForTesting(i - 1);
        const auto here  = view->muteButtonBoundsForTesting(i);
        INFO("tracks " << (i - 1) << " and " << i);
        REQUIRE(here.getY() >= above.getBottom());
    }
}

TEST_CASE("A click on a mute button reports that track and no other", "[gui][arrangement]")
{
    // The hit-test and the painting must agree about where the button is.
    // Worked out separately they drift, and the control responds somewhere
    // other than where it's drawn.
    JuceFixture fixture;
    auto view = viewWith(4);

    for (int i = 0; i < 4; ++i)
    {
        const auto centre = view->muteButtonBoundsForTesting(i).getCentre();
        INFO("track " << i << " centre " << centre.toString());
        REQUIRE(view->muteButtonAtForTesting(centre) == i);
    }
}

TEST_CASE("A click on the timeline is not a mute", "[gui][arrangement]")
{
    // The gutter check runs before the clip hit-test, so it must not claim
    // anything outside the gutter — otherwise clicking a clip would mute.
    JuceFixture fixture;
    auto view = viewWith(3);

    const float laneY = view->muteButtonBoundsForTesting(1).getCentreY();
    REQUIRE(view->muteButtonAtForTesting({ view->gutterWidthForTesting() + 50.0f, laneY }) == -1);
    REQUIRE(view->muteButtonAtForTesting({ 400.0f, laneY }) == -1);
}

TEST_CASE("A click in the gutter but off a button is not a mute", "[gui][arrangement]")
{
    JuceFixture fixture;
    auto view = viewWith(3);

    // Over the track name, well left of the button.
    const float laneY = view->muteButtonBoundsForTesting(0).getCentreY();
    REQUIRE(view->muteButtonAtForTesting({ 10.0f, laneY }) == -1);
}

TEST_CASE("The gear sits beside the mute without overlapping it", "[gui][arrangement]")
{
    // mouseDown checks mute first, so any overlap makes the gear unreachable
    // in exactly that region — a button that works everywhere except where
    // it's drawn.
    JuceFixture fixture;
    auto view = viewWith(4);

    for (int i = 0; i < 4; ++i)
    {
        const auto mute = view->muteButtonBoundsForTesting(i);
        const auto gear = view->gearButtonBoundsForTesting(i);

        INFO("track " << i << " mute " << mute.toString() << " gear " << gear.toString());
        REQUIRE_FALSE(mute.intersects(gear));
        REQUIRE(gear.getRight() <= view->gutterWidthForTesting());
        REQUIRE(gear.getWidth() > 0.0f);
    }
}

TEST_CASE("A click on the gear reports that track, and mute doesn't claim it", "[gui][arrangement]")
{
    JuceFixture fixture;
    auto view = viewWith(4);

    for (int i = 0; i < 4; ++i)
    {
        const auto centre = view->gearButtonBoundsForTesting(i).getCentre();
        INFO("track " << i);
        REQUIRE(view->gearButtonAtForTesting(centre) == i);
        REQUIRE(view->muteButtonAtForTesting(centre) == -1); // checked first, must not win here
    }
}

TEST_CASE("The gear claims nothing outside itself", "[gui][arrangement]")
{
    JuceFixture fixture;
    auto view = viewWith(3);

    const float laneY = view->gearButtonBoundsForTesting(1).getCentreY();
    REQUIRE(view->gearButtonAtForTesting({ 10.0f, laneY }) == -1);                                  // the name
    REQUIRE(view->gearButtonAtForTesting({ view->gutterWidthForTesting() + 40.0f, laneY }) == -1);   // the timeline
    REQUIRE(view->gearButtonAtForTesting(view->muteButtonBoundsForTesting(1).getCentre()) == -1);
}

TEST_CASE("Every track type has a tag, and they are distinct", "[gui][arrangement]")
{
    // Renaming a track is only free if something else still says what kind it
    // is. Two types sharing a tag would defeat that for one of them.
    const model::TrackType types[] = { model::TrackType::Instrument, model::TrackType::Audio,
                                       model::TrackType::Drum, model::TrackType::Guitar };

    std::vector<std::string> tags;
    for (auto type : types)
    {
        const juce::String tag = trackTypeTag(type);
        INFO("type " << (int) type << " tag " << tag);
        REQUIRE(tag.isNotEmpty());
        REQUIRE(tag.length() <= 4); // it has to fit the badge
        tags.push_back(tag.toStdString());
    }

    std::sort(tags.begin(), tags.end());
    REQUIRE(std::adjacent_find(tags.begin(), tags.end()) == tags.end());
}

TEST_CASE("The default colour leaves a track looking as it always did", "[gui][arrangement]")
{
    // Colour 0 means "untouched", and an existing project must not change
    // appearance because the feature was added.
    REQUIRE(trackColour(0) == juce::Colour(kDefaultTrackColour));
    REQUIRE(trackColour(0xff36618e) == juce::Colour(0xff36618e));
}

TEST_CASE("The palette offers distinguishable colours", "[gui][arrangement]")
{
    // The point of a fixed palette rather than a picker is telling parts
    // apart; two entries that look alike would waste a slot.
    for (int i = 1; i < kNumTrackColours; ++i)
    {
        for (int j = i + 1; j < kNumTrackColours; ++j)
        {
            const auto a = juce::Colour(kTrackColours[i].argb);
            const auto b = juce::Colour(kTrackColours[j].argb);
            INFO(kTrackColours[i].name << " vs " << kTrackColours[j].name);
            REQUIRE(std::abs(a.getHue() - b.getHue()) > 0.03f);
        }
    }
}

TEST_CASE("The gear is drawn smaller than the area it responds to", "[gui][arrangement]")
{
    // The gear's artwork is square and fills its box, while the speaker
    // beside it is wider than tall and fills only part of one — at equal box
    // sizes the gear reads as the heaviest thing in the gutter. It is drawn
    // smaller deliberately, and the hit target is deliberately not, so
    // shrinking the glyph doesn't make the button harder to hit.
    JuceFixture fixture;
    auto view = viewWith(2);

    const float glyph = ArrangementView::gearGlyphSizeForTesting();
    const float hit   = ArrangementView::muteSizeForTesting();

    REQUIRE(glyph < hit);
    REQUIRE(glyph >= 8.0f); // still legible as a gear rather than a dot

    for (int i = 0; i < 2; ++i)
    {
        const auto bounds = view->gearButtonBoundsForTesting(i);
        INFO("track " << i << " hit area " << bounds.toString());
        REQUIRE(bounds.getWidth() == hit);   // unchanged by the glyph shrinking
        REQUIRE(bounds.getHeight() == hit);
    }
}

TEST_CASE("The ruler is a scrub target, the gutter above it isn't", "[gui][arrangement]")
{
    // The strip over the gutter is above the track names, not above any part
    // of the timeline, so there is no position for it to scrub to.
    JuceFixture fixture;
    auto view = viewWith(2);

    const float rulerY = view->rulerHeightForTesting() * 0.5f;
    const float gutter = view->gutterWidthForTesting();

    REQUIRE(view->isOnRulerForTesting({ gutter + 10.0f, rulerY }));
    REQUIRE(view->isOnRulerForTesting({ gutter + 400.0f, rulerY }));
    REQUIRE_FALSE(view->isOnRulerForTesting({ gutter - 10.0f, rulerY })); // over the names
    REQUIRE_FALSE(view->isOnRulerForTesting({ 4.0f, rulerY }));
}

TEST_CASE("The ruler ends where the lanes begin", "[gui][arrangement]")
{
    // Below the ruler a press belongs to the clips, not to scrubbing.
    JuceFixture fixture;
    auto view = viewWith(2);

    const float x      = view->gutterWidthForTesting() + 50.0f;
    const float height = view->rulerHeightForTesting();

    REQUIRE(view->isOnRulerForTesting({ x, 0.0f }));
    REQUIRE(view->isOnRulerForTesting({ x, height - 0.5f }));
    REQUIRE_FALSE(view->isOnRulerForTesting({ x, height }));
    REQUIRE_FALSE(view->isOnRulerForTesting({ x, height + 20.0f }));
}

TEST_CASE("Scrubbing across the ruler maps to increasing beats", "[gui][arrangement]")
{
    // What the drag actually reports. If this didn't rise with x, dragging
    // would move the playhead somewhere unrelated to the mouse.
    JuceFixture fixture;
    auto view = viewWith(2);
    view->setZoom(1.0f);

    const float gutter = view->gutterWidthForTesting();

    const double atStart  = view->beatForXForTesting(gutter);
    const double atMiddle = view->beatForXForTesting(gutter + 100.0f);
    const double atEnd    = view->beatForXForTesting(gutter + 400.0f);

    REQUIRE(atStart == 0.0);
    REQUIRE(atMiddle > atStart);
    REQUIRE(atEnd > atMiddle);

    // Dragging left of the timeline pins to the start rather than going
    // negative — a playhead before bar 1 isn't a position.
    REQUIRE(view->beatForXForTesting(gutter - 50.0f) == 0.0);
}
