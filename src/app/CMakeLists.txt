# The Looper-Audio application target. Phase 0 is a minimal JUCE GUI app that
# opens a window and plays a test tone through the audio device — proof that the
# whole path (device manager → audio callback → output) works on every platform.

# MICROPHONE_PERMISSION_ENABLED is what puts NSMicrophoneUsageDescription into
# the bundle's Info.plist, and on macOS 10.14+ that key is the whole of whether
# an app can record. Without it the OS never shows the permission prompt and
# never grants access - but CoreAudio still opens the input device and still
# delivers buffers, full of zeros. So recording appeared to work: the transport
# rolled, a take was written, a track appeared, and it was silent. JUCE does not
# request the permission itself on macOS (RuntimePermissions::request is an
# Android-only implementation and returns true everywhere else), so there is no
# runtime fallback for a missing key.
juce_add_gui_app(LooperAudio
  PRODUCT_NAME "Looper-Audio"
  COMPANY_NAME "Anthony Lazzaro"
  BUNDLE_ID    "com.anthonylazzaro.looperaudio"
  VERSION      "${PROJECT_VERSION}"
  MICROPHONE_PERMISSION_ENABLED TRUE
  MICROPHONE_PERMISSION_TEXT    "Looper-Audio needs the microphone to record audio tracks.")

target_sources(LooperAudio PRIVATE
  Main.cpp
  MainComponent.cpp
  ${PROJECT_SOURCE_DIR}/src/engine/AudioEngine.cpp
  ${PROJECT_SOURCE_DIR}/src/engine/Mp3Encoder.cpp)

# Microphone permission is a per-platform question with only one real answer
# today (see app/MicrophonePermission.h): macOS gates it and can prompt for it,
# nowhere else does. The macOS implementation needs AVFoundation, and so has to
# be Objective-C++ — hence a separate translation unit rather than an #ifdef
# inside MainComponent.cpp.
if (APPLE)
  target_sources(LooperAudio PRIVATE MicrophonePermission_mac.mm)
  target_link_libraries(LooperAudio PRIVATE
    "-framework AVFoundation"
    "-framework AppKit")
else()
  target_sources(LooperAudio PRIVATE MicrophonePermission_generic.cpp)
endif()

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
    lame::mp3
    juce::juce_audio_utils
  PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags
    looper_no_float_equal_warning)
