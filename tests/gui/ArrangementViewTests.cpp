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
