#pragma once

/**
    Picks the right plugin-hosting module for whichever target is being built.

    JUCE 8 splits hosting in two: `juce_audio_processors` carries the plugin
    *editor* support, which on macOS drags in CoreAudioKit for AUGenericView,
    and `juce_audio_processors_headless` carries everything else. The app needs
    the first (it opens plugin windows); the headless bounce tool must not link
    it, or it fails to link against a GUI framework it has no use for.

    Both define the same format manager and instance types, so everything else
    in the engine is written once against whichever is present.
*/

#if defined(JUCE_MODULE_AVAILABLE_juce_audio_processors)
 #include <juce_audio_processors/juce_audio_processors.h>
 #define LOOPER_ADD_PLUGIN_FORMATS(manager) juce::addDefaultFormatsToManager(manager)
#else
 #include <juce_audio_processors_headless/juce_audio_processors_headless.h>
 #define LOOPER_ADD_PLUGIN_FORMATS(manager) juce::addHeadlessDefaultFormatsToManager(manager)
#endif
