# Unit tests. The real-time primitives have no JUCE dependency, so this target
# builds and runs fast — configure with -DLOOPER_BUILD_APP=OFF for a quick
# test-only loop that doesn't pull JUCE.
add_executable(looper_tests
  rt/SpscRingBufferTests.cpp
  engine/TempoMapTests.cpp
  engine/TransportTests.cpp
  engine/InterpolationTests.cpp
  engine/MidiNoteTests.cpp
  engine/SequencerMathTests.cpp
  engine/MetronomeMathTests.cpp
  engine/NoteOpsTests.cpp
  engine/OscillatorTests.cpp
  engine/AutomationCurveTests.cpp
  engine/SessionMathTests.cpp
  engine/GuitarStringTests.cpp
  engine/GuitarChordsTests.cpp
  engine/WaveshaperTests.cpp
  engine/PedalDspTests.cpp
  engine/DelayLineTests.cpp
  engine/StateVariableFilterTests.cpp
  engine/ReverbTests.cpp
  model/HistoryTests.cpp
  model/SongTests.cpp
  model/SerializationTests.cpp
  model/AutomationLaneTests.cpp
  app/TimelineGeometryTests.cpp
  app/PianoRollGeometryTests.cpp
  app/DockLayoutTreeTests.cpp
  app/ClipPreviewTests.cpp)

target_link_libraries(looper_tests
  PRIVATE
    looper::rt
    Catch2::Catch2WithMain)

target_compile_features(looper_tests PRIVATE cxx_std_20)

# Register each Catch2 TEST_CASE with CTest.
list(APPEND CMAKE_MODULE_PATH "${Catch2_SOURCE_DIR}/extras")
include(Catch)
catch_discover_tests(looper_tests)

# GUI smoke tests. Separate from looper_tests because these link JUCE's GUI
# module and construct real Components — the headless suite deliberately does
# not, so nothing in it can see a Component at all. That gap is how a pedal
# shipped whose controls were laid out and shown while never being parented,
# passing every check. These cover the things that go wrong on the way to the
# screen: parenting, bounds, visibility and hit-testing.
if (LOOPER_BUILD_APP)
  add_executable(looper_gui_tests
    gui/FretboardPaneTests.cpp
    gui/ArrangementViewTests.cpp)

  target_include_directories(looper_gui_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)

  target_compile_definitions(looper_gui_tests PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_STANDALONE_APPLICATION=1
    # triggerClick() is asynchronous; pumping the queue needs the dispatch
    # loop, which JUCE gates behind this. Fine in a test binary.
    JUCE_MODAL_LOOPS_PERMITTED=1)

  target_link_libraries(looper_gui_tests
    PRIVATE
      looper::rt
      juce::juce_audio_utils
      Catch2::Catch2WithMain
      juce::juce_recommended_config_flags
      juce::juce_recommended_warning_flags)

  target_compile_features(looper_gui_tests PRIVATE cxx_std_20)
  catch_discover_tests(looper_gui_tests)
endif()
