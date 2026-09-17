#pragma once

// Private to MainComponent's own .cpp files: the includes they share, and the
// constants and helpers that more than one of them uses. MainComponent is one
// class split across several files by area (see src/app/CMakeLists.txt); this
// is what those files have in common. Not for use anywhere else.

#include "MainComponent.h"

#include "ScrollFollow.h"
#include "Shortcuts.h"

#include "Icons.h"

#include "engine/ClipSlot.h"
#include "app/ExportAudioDialog.h"
#include "app/ImportRawDialog.h"
#include "app/RowWrapLayout.h"
#include "app/OfflineRenderJob.h"
#include "app/RenderProgress.h"
#include "app/StemNaming.h"
#include "app/TimelineZoom.h"
#include "engine/EffectSlotFactory.h"
#include "engine/NoteOps.h"
#include "engine/MidiFileIO.h"
#include "engine/OfflineRenderer.h"
#include "engine/SequenceAudioFormat.h"
#include "model/TrackChannels.h"
#include "model/MasteringPresets.h"
#include "model/Serialization.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <vector>

namespace soundsplice
{
/** View-menu ids for panels start well clear of the fixed commands, so adding
    a pane can never collide with one. */
inline constexpr int kFirstPanelMenuId = 100;

/** Layout entries in the View menu. Between the fixed commands (1..32) and
    the panel toggles (kFirstPanelMenuId upward), which is the only free
    range — the panel list is unbounded above, so this can't sit past it. */
inline constexpr int kFirstLayoutMenuId = 40;

/** Colour entries in the per-track gear menu, clear of that menu's own
    fixed items. */
inline constexpr int kFirstColourMenuId = 200;

/** How close to the edge the playhead gets before the keys grid pages. Small,
    so almost the whole width is travelled before each jump. */
inline constexpr int kKeysFollowMargin = 24;

/** Extra beats rendered past the last clip when bouncing, so reverb and delay
    tails decay into the file instead of being chopped off at the final beat.
    Two bars at 4/4 — comfortably longer than the master reverb's tail at its
    largest room size. */
inline constexpr double kBounceTailBeats = 8.0;

/** What Normalize aims the loudest sample at. Just under full scale rather
    than at it: a clip normalised to exactly 1.0 has no headroom left for the
    track fader, pan law, or any effect that can overshoot, so it would be
    the first thing to clip the master bus. */
inline constexpr float kNormaliseTargetPeak = 0.98f;

/** The time signatures offered. A fixed list because these are the ones
    people write in; a free numerator and denominator invites 4/7, which the
    rest of the app would have to have an opinion about. */
struct TimeSignatureOption { int numerator, denominator; };

inline constexpr TimeSignatureOption kTimeSignatures[] = {
    { 4, 4 }, { 3, 4 }, { 2, 4 }, { 5, 4 }, { 6, 8 }, { 7, 8 }, { 12, 8 },
};

inline constexpr int kNumTimeSignatures = (int) (sizeof(kTimeSignatures) / sizeof(kTimeSignatures[0]));

using Cmd = engine::EngineCommand::Type;



namespace mainui
{
    /** Adds a menu item that advertises its shortcut. PopupMenu's plain
        addItem overload has nowhere to put one, and an undiscoverable
        shortcut may as well not exist. */
    inline void addItem(juce::PopupMenu& menu, int id, const juce::String& text,
                 const juce::KeyPress& shortcut, bool enabled = true)
    {
        juce::PopupMenu::Item item(text);
        item.itemID                = id;
        item.isEnabled             = enabled;
        item.shortcutKeyDescription = shortcut.getTextDescriptionWithIcons();
        menu.addItem(std::move(item));
    }

    /** "Undo Delete track" rather than a bare "Undo". Every edit already
        records what it was; not showing it left the user to remember what
        they'd done, which is the one thing undo exists to spare them. */
    inline juce::String withAction(const char* verb, bool available, const std::string& action)
    {
        juce::String text(verb);
        if (available && ! action.empty())
            text += " " + juce::String(action);
        return text;
    }

    /** "Play / pause  (space)" — a control with no menu entry has nowhere
        else to say what its shortcut is. */
    inline juce::String withShortcut(const juce::String& text, const juce::KeyPress& key)
    {
        return text + "  (" + key.getTextDescriptionWithIcons() + ")";
    }

    inline juce::PropertiesFile::Options makeSettingsOptions()
    {
        juce::PropertiesFile::Options opts;
        opts.applicationName     = "SoundSplice";
        opts.filenameSuffix      = ".settings";
        opts.folderName          = "SoundSplice";
        opts.osxLibrarySubFolder = "Application Support";
        return opts;
    }
}

/** Live tweak from the Track FX pane — updates the document in place (not a
    separate undo step per knob notch) and mirrors it into the engine, the
    same pattern the mixer faders and the Synth pane use. */
/** One chain slot's parameters in the engine's terms. `enabled` is the
    slot's own bypass, not the per-settings flag — bypass has to mean the same
    thing for a hosted plugin as for a built-in. */
/** How long the blend back to the unprocessed audio takes at each edge of a
    rendered selection. Long enough to remove the step an effect's level
    change leaves, far too short to hear as a fade. */
inline constexpr double kEffectEdgeFadeSeconds = 0.005;

/** model::PluginFormat -> the name JUCE's format manager uses. The document
    stores an enum so the file format doesn't depend on JUCE's spelling; this
    is the one place the two meet. */
inline std::string pluginFormatName(model::PluginFormat format)
{
    switch (format)
    {
        case model::PluginFormat::VST3:      return "VST3";
        case model::PluginFormat::AudioUnit: return "AudioUnit";
        case model::PluginFormat::Unknown:   break;
    }
    return {};
}

/** Converts a track's model automation lanes into the engine's curve form.
    The engine can't use model::AutomationLane directly — `model` already
    depends on `engine`, so the dependency can't run both ways — and this is
    the single place the two representations meet, used by both live playback
    and the offline exporter. */
inline engine::TrackAutomation toTrackAutomation(const model::Track& track)
{
    engine::TrackAutomation curves;

    auto copyLane = [&track](model::TrackParam param, engine::AutomationCurve& into)
    {
        if (const auto* lane = track.lane(param))
        {
            for (const auto& point : lane->points())
                into.addPoint(point.beat, point.value);
            into.sortPoints();
        }
    };

    copyLane(model::TrackParam::Gain, curves.gain);
    copyLane(model::TrackParam::Pan, curves.pan);
    return curves;
}

/** How many of @p chain's slots are enabled, built-ins and plugins alike:
    the ones rendering it would run. */
inline int enabledEffectCount(const std::vector<model::EffectSlot>& chain)
{
    return (int) std::count_if(chain.begin(), chain.end(), [](const auto& slot) { return slot.enabled; });
}

/** A plugin instance's own state, base64'd the way model::PluginRef stores it. */
inline std::string pluginStateOf(juce::AudioPluginInstance& instance)
{
    juce::MemoryBlock block;
    instance.getStateInformation(block);
    return block.toBase64Encoding().toStdString();
}

/** What rendering a chain came to. On failure nothing was rendered, and
    @c error says which plugin wouldn't load. */
struct EffectRenderResult
{
    bool         ok = true;
    juce::String error;
};

/** Runs @p chain's enabled slots, built-ins and hosted plugins, over
    @p block in place. Shared by Apply and Preview, so a preview is exactly
    what Apply will write.

    Each plugin slot gets its own instance, with the slot's saved state
    restored, separate from any copy playing live on a track. The chain runs
    in fixed-size blocks, because a plugin prepared for a block size can't be
    handed a whole selection in one call. The chain's reported latency is
    rendered past the end and read back into place, so a plugin that delays
    its output doesn't shift the audio late.

    All-or-nothing: if any plugin can't be loaded, nothing is rendered. Half a
    chain applied to the audio would be worse than an honest refusal. */
inline EffectRenderResult renderEffectChain(const std::vector<model::EffectSlot>& chain,
                                            juce::AudioBuffer<float>& block, double sampleRate, double bpm,
                                            engine::PluginHost& plugins)
{
    constexpr int kRenderBlockSize = 512;

    engine::EffectChain built;
    for (const auto& slot : chain)
    {
        if (! slot.enabled)
            continue;

        if (slot.kind == model::EffectKind::Plugin)
        {
            std::string error;
            auto instance = plugins.createInstance(pluginFormatName(slot.plugin.format), slot.plugin.identifier,
                                                   sampleRate, kRenderBlockSize, &error);
            if (instance == nullptr)
            {
                const juce::String name = slot.plugin.name.empty() ? juce::String("A plugin")
                                                                   : "\"" + juce::String(slot.plugin.name) + "\"";
                return { false, name + " couldn't be loaded"
                                    + (error.empty() ? juce::String() : ": " + juce::String(error)) };
            }

            auto node = std::make_unique<engine::PluginNode>(std::move(instance));
            node->restoreState(slot.plugin.state);
            node->setEnabled(true);
            built.add(std::move(node));
            continue;
        }

        if (auto node = engine::makeConfiguredNode(slot))
            built.add(std::move(node));
    }

    built.prepare(sampleRate, kRenderBlockSize);
    built.setBpm(bpm); // the wobble pedal is tempo-locked

    // Only known once prepared: a plugin may report a different latency for
    // a different rate or block size.
    int latency = 0;
    for (size_t i = 0; i < built.size(); ++i)
        if (auto* pluginNode = dynamic_cast<engine::PluginNode*>(built.nodeAt(i)))
            if (auto* instance = pluginNode->instance())
                latency += juce::jmax(0, instance->getLatencySamples());

    const int numChannels = block.getNumChannels();
    const int length      = block.getNumSamples();

    juce::AudioBuffer<float> work(numChannels, length + latency);
    work.clear();
    for (int ch = 0; ch < numChannels; ++ch)
        work.copyFrom(ch, 0, block, ch, 0, length);

    for (int start = 0; start < work.getNumSamples(); start += kRenderBlockSize)
    {
        const int count = juce::jmin(kRenderBlockSize, work.getNumSamples() - start);
        juce::AudioBuffer<float> view(work.getArrayOfWritePointers(), numChannels, start, count);
        built.process(view);
    }

    for (int ch = 0; ch < numChannels; ++ch)
        block.copyFrom(ch, 0, work, ch, latency, length);

    return {};
}

/** How much of a selection Preview plays: enough to judge an effect by,
    short enough that rendering it doesn't stall the UI on a long selection. */
inline constexpr double kEffectPreviewSeconds = 10.0;

/** Reads the value a fader controls, straight from the document. */
inline float readFader(const model::Song& song, int index, MixerStrip::Fader fader)
{
    if (index < 0 || index >= (int) song.tracks.size())
        return 0.0f;

    const auto& track = song.tracks[(size_t) index];
    switch (fader)
    {
        case MixerStrip::Fader::Gain: return track.gainDb;
        case MixerStrip::Fader::Pan:  return track.pan;
    }
    return 0.0f;
}

inline void writeFader(model::Song& song, int index, MixerStrip::Fader fader, float value)
{
    if (index < 0 || index >= (int) song.tracks.size())
        return;

    auto& track = song.tracks[(size_t) index];
    switch (fader)
    {
        case MixerStrip::Fader::Gain: track.gainDb    = value; break;
        case MixerStrip::Fader::Pan:  track.pan       = value; break;
    }
}

inline const char* faderName(MixerStrip::Fader fader)
{
    switch (fader)
    {
        case MixerStrip::Fader::Gain: return "Set track gain";
        case MixerStrip::Fader::Pan:  return "Set track pan";
    }
    return "Set track level";
}

/** How long unsaved changes can go unprotected. Long enough that writing the
    project costs nothing noticeable, short enough that a crash loses little. */
inline constexpr double kAutosaveIntervalSeconds = 30.0;

using namespace mainui;

} // namespace soundsplice
