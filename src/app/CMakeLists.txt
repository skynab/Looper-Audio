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
  MainComponent.cpp)

# Trim JUCE features we don't need yet — this also removes system dependencies
# (WebKit, libcurl) that would otherwise be required to build on Linux.
target_compile_definitions(LooperAudio PRIVATE
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0
  JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:LooperAudio,JUCE_PRODUCT_NAME>"
  JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:LooperAudio,JUCE_VERSION>")

target_link_libraries(LooperAudio
  PRIVATE
    looper::rt
    juce::juce_audio_utils
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)
