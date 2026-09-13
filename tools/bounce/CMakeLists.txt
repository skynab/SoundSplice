# Headless offline-render smoke tool: renders a demo pattern to a WAV with no
# audio device, so the synth + sequencer audio path can be verified in CI / by hand.
juce_add_console_app(looper_bounce PRODUCT_NAME "looper-bounce")

target_sources(looper_bounce PRIVATE
  main.cpp
  ${PROJECT_SOURCE_DIR}/src/engine/Mp3Encoder.cpp)

# Plugin hosting is enabled here too so the smoke tool can verify it against a
# real plugin rather than a mock — see the pluginHost* checks in main.cpp. It
# links the *headless* hosting module: the GUI one carries plugin-editor
# support, which on macOS needs CoreAudioKit, and a console tool has no use for
# a window.
target_compile_definitions(looper_bounce PRIVATE
  JUCE_PLUGINHOST_VST3=1
  JUCE_PLUGINHOST_AU=1
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0)

# See src/app/CMakeLists.txt for why looper_no_float_equal_warning exists:
# this codebase's exact-float-equality checks (undo/history dedup, a couple
# of DSP fast paths) aren't the fuzzy-comparison bug -Wfloat-equal exists to
# catch, and this tool links the same engine code that trips it.
target_link_libraries(looper_bounce
  PRIVATE
    looper::rt
    lame::mp3
    juce::juce_audio_formats
    juce::juce_audio_processors_headless
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
    looper_no_float_equal_warning)

# Register as a smoke test: it renders audio and exits non-zero if the output is
# silent, so CI verifies the synth + sequencer on every platform.
if(LOOPER_BUILD_TESTS)
  add_test(NAME bounce_smoke COMMAND looper_bounce)
endif()
