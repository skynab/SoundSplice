# Unit tests. The real-time primitives have no JUCE dependency, so this target
# builds and runs fast — configure with -DSOUNDSPLICE_BUILD_APP=OFF for a quick
# test-only loop that doesn't pull JUCE.
add_executable(soundsplice_tests
  rt/SpscRingBufferTests.cpp
  engine/TempoMapTests.cpp
  engine/TransportTests.cpp
  engine/InterpolationTests.cpp
  engine/ClipFadeTests.cpp
  engine/MidiNoteTests.cpp
  engine/SequencerMathTests.cpp
  engine/MetronomeMathTests.cpp
  engine/MidiCaptureTests.cpp
  engine/MidiRecorderTests.cpp
  engine/NoteOpsTests.cpp
  engine/OscillatorTests.cpp
  engine/AutomationCurveTests.cpp
  engine/SessionMathTests.cpp
  engine/WaveshaperTests.cpp
  engine/PedalDspTests.cpp
  engine/MasteringDspTests.cpp
  engine/NoiseReductionTests.cpp
  engine/AudioEditsTests.cpp
  engine/TimeStretchTests.cpp
  engine/SpectrumTests.cpp
  engine/CabinetIrTests.cpp
  engine/DelayLineTests.cpp
  engine/DitherTests.cpp
  engine/EqPedalTests.cpp
  engine/OversamplerTests.cpp
  engine/StateVariableFilterTests.cpp
  engine/ShelfPeakFilterTests.cpp
  engine/EqCurveTests.cpp
  engine/ReverbTests.cpp
  model/HistoryTests.cpp
  model/SongTests.cpp
  model/SerializationTests.cpp
  model/EffectParamsTests.cpp
  model/EffectPresetsTests.cpp
  model/TimebaseTests.cpp
  model/MarkersTests.cpp
  model/MasteringPresetsTests.cpp
  model/AutomationLaneTests.cpp
  app/TimelineGeometryTests.cpp
  app/PianoRollGeometryTests.cpp
  app/DockLayoutTreeTests.cpp
  app/WorkspaceLayoutsTests.cpp
  app/ClipPreviewTests.cpp
  app/AudioSelectionTests.cpp
  app/WaveformPeaksTests.cpp
  app/RenderProgressTests.cpp
  app/ScrollFollowTests.cpp
  app/TrackSelectionTests.cpp
  app/RecordSourceChoiceTests.cpp
  app/AutomationGeometryTests.cpp
  app/DragCommitTests.cpp
  app/ClipWindowTests.cpp
  app/AutosaveTests.cpp
  app/TimeFormatTests.cpp
  app/SnapTargetsTests.cpp)

target_link_libraries(soundsplice_tests
  PRIVATE
    soundsplice::rt
    Catch2::Catch2WithMain)

target_compile_features(soundsplice_tests PRIVATE cxx_std_20)

# Register each Catch2 TEST_CASE with CTest.
list(APPEND CMAKE_MODULE_PATH "${Catch2_SOURCE_DIR}/extras")
include(Catch)
catch_discover_tests(soundsplice_tests)

# GUI smoke tests. Separate from soundsplice_tests because these link JUCE's GUI
# module and construct real Components — the headless suite deliberately does
# not, so nothing in it can see a Component at all. That gap is how a pedal
# shipped whose controls were laid out and shown while never being parented,
# passing every check. These cover the things that go wrong on the way to the
# screen: parenting, bounds, visibility and hit-testing.
if (SOUNDSPLICE_BUILD_APP)
  add_executable(soundsplice_gui_tests
    gui/ArrangementViewTests.cpp
    gui/AudioExportTests.cpp
  gui/AudioEditorPaneTests.cpp
  gui/AudioFileTypesTests.cpp
    gui/IconsTests.cpp
    gui/PianoRollTests.cpp
    gui/AutomationPaneTests.cpp
    gui/ShortcutsTests.cpp
    gui/CommandTableTests.cpp
    gui/WaveformCacheTests.cpp
    gui/RowWrapLayoutTests.cpp
    gui/StemNamingTests.cpp
    gui/ProjectMediaTests.cpp
    gui/PaneLayoutTests.cpp
    gui/PaneWiringTests.cpp
    ${PROJECT_SOURCE_DIR}/src/engine/Mp3Encoder.cpp)

  target_include_directories(soundsplice_gui_tests PRIVATE ${PROJECT_SOURCE_DIR}/src)

  target_compile_definitions(soundsplice_gui_tests PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_STANDALONE_APPLICATION=1
    # triggerClick() is asynchronous; pumping the queue needs the dispatch
    # loop, which JUCE gates behind this. Fine in a test binary.
    JUCE_MODAL_LOOPS_PERMITTED=1)

  # See src/app/CMakeLists.txt for why soundsplice_no_float_equal_warning exists:
  # this codebase's exact-float-equality checks (undo/history dedup, a couple
  # of DSP fast paths) aren't the fuzzy-comparison bug -Wfloat-equal exists
  # to catch, and this target links the same model/engine code that trips it.
  target_link_libraries(soundsplice_gui_tests
    PRIVATE
      soundsplice::rt
      lame::mp3
      juce::juce_audio_utils
      Catch2::Catch2WithMain
      juce::juce_recommended_config_flags
      juce::juce_recommended_warning_flags
      soundsplice_no_float_equal_warning)

  target_compile_features(soundsplice_gui_tests PRIVATE cxx_std_20)
  catch_discover_tests(soundsplice_gui_tests)
endif()
