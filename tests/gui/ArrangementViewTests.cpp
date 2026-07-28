#include <catch2/catch_test_macros.hpp>

#include <juce_gui_basics/juce_gui_basics.h>

#include <app/ArrangementView.h>

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
