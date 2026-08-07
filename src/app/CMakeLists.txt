# The Looper-Audio application target. Phase 0 is a minimal JUCE GUI app that
# opens a window and plays a test tone through the audio device — proof that the
# whole path (device manager → audio callback → output) works on every platform.

juce_add_gui_app(LooperAudio
  PRODUCT_NAME "Looper-Audio"
  COMPANY_NAME "Anthony Lazzaro"
  BUNDLE_ID    "com.anthonylazzaro.looperaudio"
  VERSION      "${PROJECT_VERSION}")

target_sources(LooperAudio PRIVATE
  Main.cpp
  MainComponent.cpp
  ${PROJECT_SOURCE_DIR}/src/engine/AudioEngine.cpp)

# Trim JUCE features we don't need yet — this also removes system dependencies
# (WebKit, libcurl) that would otherwise be required to build on Linux.
target_compile_definitions(LooperAudio PRIVATE
  JUCE_PLUGINHOST_VST3=1
  JUCE_PLUGINHOST_AU=1
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:LooperAudio,JUCE_PRODUCT_NAME>"
  JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:LooperAudio,JUCE_VERSION>")

# -Wfloat-equal (part of juce_recommended_warning_flags below) flags every
# `==`/`!=` on a float, but this codebase uses exact float equality
# deliberately and throughout: settings structs compare bit-for-bit so
# undo/history can tell "unchanged" from "changed" (see DragCommit.h's
# commitStructDrag), and a few DSP hot paths skip a ramp when a start value
# exactly equals its end value (see InstrumentTrack.h). None of that is the
# fuzzy-comparison bug the warning exists to catch, so it's just noise here.
# A target's own compile options are always placed *before* flags inherited
# from linked interface libraries, so "-Wno-float-equal" can't just be added
# as a normal compile option — it would be overridden by
# juce_recommended_warning_flags' "-Wfloat-equal" later on the same command
# line. Linking one more tiny interface library, after that one, works
# because linked libraries' flags are concatenated in link order.
add_library(looper_no_float_equal_warning INTERFACE)
target_compile_options(looper_no_float_equal_warning INTERFACE -Wno-float-equal)

target_link_libraries(LooperAudio
  PRIVATE
    looper::rt
    juce::juce_audio_utils
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
    looper_no_float_equal_warning)
