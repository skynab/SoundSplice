# soundsplice-cli: convert, analyze, apply a macro, and render a project, from
# the command line - see main.cpp. It links the app's own readers, writers,
# effect renderer and meters; rendering a project runs the app itself with
# --render rather than carrying a second mixer.
juce_add_console_app(soundsplice_cli PRODUCT_NAME "soundsplice-cli")
# JUCE names a console app's binary after the target, not PRODUCT_NAME.
set_target_properties(soundsplice_cli PROPERTIES OUTPUT_NAME "soundsplice-cli")

target_sources(soundsplice_cli PRIVATE
  main.cpp
  ${PROJECT_SOURCE_DIR}/src/engine/AudioFormats.cpp
  ${PROJECT_SOURCE_DIR}/src/engine/Mp3Encoder.cpp)

target_include_directories(soundsplice_cli PRIVATE ${PROJECT_SOURCE_DIR}/src)

# Plugins in a macro's effects are hosted as the app hosts them: the headless
# hosting module, as the bounce tool uses - a console tool has no editors to show.
target_compile_definitions(soundsplice_cli PRIVATE
  JUCE_PLUGINHOST_VST3=1
  JUCE_PLUGINHOST_AU=1
  JUCE_WEB_BROWSER=0
  JUCE_USE_CURL=0)

target_link_libraries(soundsplice_cli
  PRIVATE
    soundsplice::rt
    soundsplice::model
    lame::mp3
    opusfile::opusfile
    WavPack::WavPack
    juce::juce_audio_formats
    juce::juce_audio_processors_headless
    juce::juce_data_structures
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_warning_flags
    soundsplice_no_float_equal_warning)

# See cmake/pch.cmake.
soundsplice_precompile(soundsplice_cli
  <juce_audio_formats/juce_audio_formats.h>
  <juce_audio_processors_headless/juce_audio_processors_headless.h>
  <juce_data_structures/juce_data_structures.h>)
