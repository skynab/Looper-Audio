# Unit tests. The real-time primitives have no JUCE dependency, so this target
# builds and runs fast — configure with -DLOOPER_BUILD_APP=OFF for a quick
# test-only loop that doesn't pull JUCE.
add_executable(looper_tests
  rt/SpscRingBufferTests.cpp
  engine/TempoMapTests.cpp
  engine/TransportTests.cpp
  engine/InterpolationTests.cpp
  engine/MidiNoteTests.cpp)

target_link_libraries(looper_tests
  PRIVATE
    looper::rt
    Catch2::Catch2WithMain)

target_compile_features(looper_tests PRIVATE cxx_std_20)

# Register each Catch2 TEST_CASE with CTest.
list(APPEND CMAKE_MODULE_PATH "${Catch2_SOURCE_DIR}/extras")
include(Catch)
catch_discover_tests(looper_tests)
