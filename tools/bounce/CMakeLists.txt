# Headless offline-render smoke tool: renders a demo pattern to a WAV with no
# audio device, so the synth + sequencer audio path can be verified in CI / by hand.
juce_add_console_app(looper_bounce PRODUCT_NAME "looper-bounce")

target_sources(looper_bounce PRIVATE main.cpp)

target_compile_definitions(looper_bounce PRIVATE
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0)

target_link_libraries(looper_bounce
  PRIVATE
    looper::rt
    juce::juce_audio_formats
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags)

# Register as a smoke test: it renders audio and exits non-zero if the output is
# silent, so CI verifies the synth + sequencer on every platform.
if(LOOPER_BUILD_TESTS)
  add_test(NAME bounce_smoke COMMAND looper_bounce)
endif()
