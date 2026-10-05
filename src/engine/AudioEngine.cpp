#include "engine/AudioEngine.h"
#include "engine/HqStretch.h"

#include "engine/SequenceAudioFormat.h"
#include "rt/RealtimeGuard.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace soundsplice::engine
{
namespace
{
}

AudioEngine::AudioEngine(bool openDevice)
{
    // Sequences too: an edited clip's audio is one (engine/SequenceAudioFormat.h).
    sequencefile::registerFormats(formatManager_);

    // Started once and left running for the engine's lifetime. It idles when
    // nothing is recording, and spinning a thread up at the instant the user
    // hits record is exactly the wrong moment to be doing it.
    recordWriterThread_.startThread(juce::Thread::Priority::normal);
    recorder_.keepLivePeaks(); // the main take is drawn as it's recorded
    streamThread_.startThread(juce::Thread::Priority::normal);

    // Each player reports where it's reading a stream in a slot of its own;
    // the last slot is the streamer's own guess for a stream nobody has
    // played yet.
    static_assert(kMaxTracks + 1 < ClipStream::kMaxReaders);
    for (int i = 0; i < kMaxTracks; ++i)
        tracks_[(size_t) i].audioPlayer.setReaderIndex(i);
    filePlayer_.setReaderIndex(kMaxTracks);

    // Input is asked for, but never at the cost of output.
    //
    // This used to be a bare initialiseWithDefaultDevices(2, 2) whose result
    // was ignored, on the assumption that JUCE would quietly fall back to
    // however many inputs the device actually had. It does not: if the input
    // cannot be opened the *whole* call fails and no device opens at all — no
    // output, so the transport never advances and the app looks like it has
    // stopped responding to the transport controls entirely.
    //
    // That became reachable the moment this app started asking macOS for real
    // microphone access. Before then the OS handed over silent input without
    // gating it; now a denied permission is a genuine failure to open the
    // input, and it must cost recording rather than all audio.
    if (! openDevice)
        return;

    inputOpenError_ = deviceManager_.initialiseWithDefaultDevices(2, 2);

    if (inputOpenError_.isNotEmpty())
    {
        const auto outputOnlyError = deviceManager_.initialiseWithDefaultDevices(0, 2);

        juce::Logger::writeToLog("Audio input unavailable (" + inputOpenError_
                                 + ") - opening output only");

        if (outputOnlyError.isNotEmpty())
            juce::Logger::writeToLog("Audio output also unavailable: " + outputOnlyError);
    }

    deviceManager_.addAudioCallback(this);
}

AudioEngine::~AudioEngine()
{
    deviceManager_.removeAudioCallback(this);
    deviceManager_.closeAudioDevice();

    // Tempo maps still on their way in or out.
    TempoMap* map = nullptr;
    while (tempoInbox_.pop(map))
        delete map;
    while (tempoReclaim_.pop(map))
        delete map;
}

bool AudioEngine::reopenAudioInput()
{
    // The same two-step the constructor does, and for the same reason: asking
    // for input is allowed to fail, but it must never cost the output device.
    // If this second attempt also fails we are no worse off than before it.
    inputOpenError_ = deviceManager_.initialiseWithDefaultDevices(2, 2);

    if (inputOpenError_.isNotEmpty())
    {
        const auto outputOnlyError = deviceManager_.initialiseWithDefaultDevices(0, 2);

        juce::Logger::writeToLog("Audio input still unavailable (" + inputOpenError_
                                 + ") - reopening output only");

        if (outputOnlyError.isNotEmpty())
            juce::Logger::writeToLog("Audio output also unavailable: " + outputOnlyError);
    }

    return hasAudioInput();
}

bool AudioEngine::hasAudioInput() const
{
    auto* device = deviceManager_.getCurrentAudioDevice();
    return device != nullptr && device->getActiveInputChannels().countNumberOfSetBits() > 0;
}

std::unique_ptr<ClipData> AudioEngine::decodeAudioFile(const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager_.createReaderFor(file));
    if (reader == nullptr)
        return nullptr;

    const int length = (int) juce::jmin<juce::int64>(reader->lengthInSamples,
                                                     (juce::int64) std::numeric_limits<int>::max());
    if (length <= 0)
        return nullptr;

    const int numChannels = juce::jmax(1, (int) reader->numChannels);

    auto clip = std::make_unique<ClipData>();
    clip->audio.setSize(numChannels, length);
    reader->read(&clip->audio, 0, length, 0, true, true);
    clip->sourceSampleRate = reader->sampleRate;
    clip->numChannels      = numChannels;
    clip->lengthSamples    = length;
    return clip;
}

double AudioEngine::probeDurationSeconds(const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager_.createReaderFor(file));
    if (reader == nullptr || reader->sampleRate <= 0.0)
        return 0.0;
    return (double) reader->lengthInSamples / reader->sampleRate;
}

bool AudioEngine::loadAudioFile(const juce::File& file)
{
    auto clip = decodeAudioFile(file);
    if (clip == nullptr)
        return false;

    loadedClipName_    = file.getFileName();
    loadedClipSeconds_ = clip->sourceSampleRate > 0.0 ? (double) clip->lengthSamples / clip->sourceSampleRate : 0.0;

    filePlayer_.collectRetiredClips();
    filePlayer_.submitSingleClip(clip.release());
    return true;
}

void AudioEngine::startAudition(const juce::AudioBuffer<float>& audio, double sampleRate)
{
    audition_.collectRetired();

    if (audio.getNumChannels() <= 0 || audio.getNumSamples() <= 0)
    {
        audition_.stop();
        return;
    }

    // Copied into a ClipData here, on the message thread, so the audio thread
    // receives a finished buffer it never has to allocate for.
    auto clip = std::make_unique<ClipData>();
    clip->audio.makeCopyOf(audio);
    clip->sourceSampleRate = sampleRate;
    clip->numChannels      = audio.getNumChannels();
    clip->lengthSamples    = audio.getNumSamples();

    audition_.play(std::move(clip));
}

void AudioEngine::stopAudition()
{
    audition_.stop();
    audition_.collectRetired();
}

std::shared_ptr<ClipData> AudioEngine::openStreamedClip(const juce::File& file)
{
    std::unique_ptr<juce::AudioFormatReader> reader(formatManager_.createReaderFor(file));
    if (reader == nullptr || reader->sampleRate <= 0.0 || reader->lengthInSamples <= 0
        || (double) reader->lengthInSamples / reader->sampleRate < kStreamClipsFromSeconds)
        return nullptr;

    auto clip              = std::make_shared<ClipData>();
    clip->sourceSampleRate = reader->sampleRate;
    clip->numChannels      = juce::jmax(1, (int) reader->numChannels);
    clip->lengthSamples    = (int) juce::jmin<juce::int64>(reader->lengthInSamples,
                                                           (juce::int64) std::numeric_limits<int>::max());
    clip->stream           = streamer_.open(std::move(reader));
    return clip;
}

std::shared_ptr<ClipData> AudioEngine::decodeOrGetCached(const juce::File& file)
{
    const auto path = file.getFullPathName();
    auto       it   = audioDecodeCache_.find(path);
    if (it != audioDecodeCache_.end())
        return it->second;

    auto shared = openStreamedClip(file);
    if (shared == nullptr)
    {
        auto decoded = decodeAudioFile(file);
        if (decoded == nullptr)
            return nullptr;

        shared.reset(decoded.release());
    }

    audioDecodeCache_[path] = shared;
    return shared;
}

std::shared_ptr<ClipData> AudioEngine::warpedOrGetCached(const juce::File& file, double stretch)
{
    // Not warped, or by so little that stretching would only cost quality.
    if (std::abs(stretch - 1.0) < 1.0e-4)
        return decodeOrGetCached(file);

    auto& versions = warpCache_[file.getFullPathName()];
    for (const auto& version : versions)
        if (std::abs(version.stretch - stretch) < 1.0e-9 && version.data != nullptr)
            return version.data;

    // The whole file, in memory: a stretch needs all of it, so even a clip
    // long enough to stream from disk is decoded here.
    auto source = decodeAudioFile(file);
    if (source == nullptr || source->lengthSamples <= 0)
        return decodeOrGetCached(file);

    const int channelCount = juce::jmax(1, source->audio.getNumChannels());
    std::vector<std::vector<float>> channels((size_t) channelCount);
    for (int c = 0; c < channelCount; ++c)
    {
        const float* read = source->audio.getReadPointer(c);
        channels[(size_t) c].assign(read, read + source->lengthSamples);
    }

    hqstretch::Settings settings;
    settings.lengthFactor = stretch;
    auto stretched = hqstretch::process(channels, source->sourceSampleRate, settings);
    if (stretched.empty())
        return decodeOrGetCached(file); // too short for the stretcher: play it as it is

    auto warped              = std::make_shared<ClipData>();
    warped->sourceSampleRate = source->sourceSampleRate;
    warped->numChannels      = channelCount;
    warped->lengthSamples    = (int) stretched[0].size();
    warped->audio.setSize(channelCount, warped->lengthSamples);
    for (int c = 0; c < channelCount; ++c)
        warped->audio.copyFrom(c, 0, stretched[(size_t) c].data(), warped->lengthSamples);

    // A few stretches per file are kept: dragging the tempo makes many.
    if (versions.size() >= 4)
        versions.erase(versions.begin());
    versions.push_back({ stretch, warped });
    return warped;
}

TempoEstimate AudioEngine::detectFileTempo(const juce::File& file)
{
    // The file as it is on disk, not as it plays warped: detecting a warped
    // clip's playback would report the tempo it was warped to.
    auto decoded = decodeAudioFile(file);
    if (decoded == nullptr || decoded->lengthSamples <= 0)
        return {};

    std::vector<std::vector<float>> channels((size_t) juce::jmax(1, decoded->audio.getNumChannels()));
    for (int c = 0; c < (int) channels.size(); ++c)
    {
        const float* read = decoded->audio.getReadPointer(c);
        channels[(size_t) c].assign(read, read + decoded->lengthSamples);
    }
    return detectTempo(channels, decoded->sourceSampleRate);
}

bool AudioEngine::setTrackAudioClips(int index, const std::vector<AudioClipSpec>& clips)
{
    if (index < 0 || index >= kMaxTracks)
        return false;

    auto* slots = new std::vector<AudioClipSlot>();
    slots->reserve(clips.size());
    bool allOk = true;

    // Chains of clips this track no longer has go; the rest are reused.
    for (auto it = clipChains_.begin(); it != clipChains_.end();)
    {
        const bool kept = std::any_of(clips.begin(), clips.end(),
                                      [&](const AudioClipSpec& spec) { return spec.clipId == it->first && ! spec.effects.empty(); });
        if (it->second.track == index && ! kept)
        {
            clipPluginChainsChanged_ = clipPluginChainsChanged_ || it->second.hasPlugin();
            it = clipChains_.erase(it);
        }
        else
            ++it;
    }

    for (const auto& spec : clips)
    {
        auto decoded = warpedOrGetCached(spec.file, spec.stretch);
        if (decoded == nullptr)
        {
            allOk = false;
            continue; // skip this clip; the others still load
        }
        slots->push_back({ decoded, spec.startBeats, spec.lengthBeats,
                           juce::Decibels::decibelsToGain(spec.gainDb), spec.sourceOffsetSeconds,
                           spec.fades, spec.channels, spec.envelope, clipChainFor(index, spec) });
    }

    auto& track = tracks_[(size_t) index];
    track.audioPlayer.collectRetiredClips();
    track.audioPlayer.submitClips(slots);
    return allOk;
}

/** The chain for @p spec's effects: the one it already had if the kinds in
    it are the same, otherwise a new one, prepared here where allocating is
    allowed. Either way, set to the settings @p spec carries. Null for a clip
    with none. */
std::shared_ptr<EffectChain> AudioEngine::clipChainFor(int trackIndex, const AudioClipSpec& spec)
{
    if (spec.effects.empty())
        return nullptr;

    // The chain's shape: the slots the spec names, or each effect's kind.
    std::vector<EffectSlotSpec> shape;
    for (size_t i = 0; i < spec.effects.size(); ++i)
    {
        auto slot = i < spec.slots.size() ? spec.slots[i] : EffectSlotSpec {};
        slot.kind = spec.effects[i].kind;
        shape.push_back(std::move(slot));
    }

    auto& entry   = clipChains_[spec.clipId];
    bool  rebuild = entry.chain == nullptr || entry.shape.size() != shape.size();
    for (size_t i = 0; ! rebuild && i < shape.size(); ++i)
        rebuild = ! entry.shape[i].sameShapeAs(shape[i]);

    if (rebuild)
    {
        clipPluginChainsChanged_ = clipPluginChainsChanged_ || entry.hasPlugin();

        auto chain = std::make_shared<EffectChain>();
        for (const auto& slot : shape)
        {
            // A kind this build can't make keeps its place as a pass-through.
            auto node = makeSlotNode(slot);
            chain->add(node != nullptr ? std::move(node) : std::make_unique<PluginNode>(nullptr));
        }

        const double rate = sampleRate_.load(std::memory_order_relaxed);
        if (rate > 0.0)
            chain->prepare(rate, currentBlockSize_);

        entry.chain = std::move(chain);
    }
    else
    {
        // As for a track's chain: a changed saved state reaches the plugin.
        for (size_t i = 0; i < shape.size(); ++i)
            if (shape[i].kind == EffectKind::Plugin && shape[i].pluginState != entry.shape[i].pluginState)
                if (auto* node = dynamic_cast<PluginNode*>(entry.chain->nodeAt(i)))
                    node->restoreState(shape[i].pluginState);
    }
    entry.shape = std::move(shape);
    entry.track = trackIndex;

    for (size_t i = 0; i < spec.effects.size(); ++i)
        entry.chain->applyParams(i, spec.effects[i]);
    return entry.chain;
}

void AudioEngine::notePluginState(int trackIndex, int slotIndex, const std::string& state)
{
    if (trackIndex >= 0 && trackIndex < kMaxTracks && slotIndex >= 0
        && slotIndex < (int) chainStructure_[(size_t) trackIndex].size())
        chainStructure_[(size_t) trackIndex][(size_t) slotIndex].pluginState = state;
}

void AudioEngine::noteClipPluginState(int clipId, int slotIndex, const std::string& state)
{
    const auto it = clipChains_.find(clipId);
    if (it != clipChains_.end() && slotIndex >= 0 && slotIndex < (int) it->second.shape.size())
        it->second.shape[(size_t) slotIndex].pluginState = state;
}

PluginNode* AudioEngine::clipPluginNode(int clipId, int slotIndex)
{
    const auto it = clipChains_.find(clipId);
    if (it == clipChains_.end() || slotIndex < 0)
        return nullptr;
    return dynamic_cast<PluginNode*>(it->second.chain->nodeAt((size_t) slotIndex));
}

std::unique_ptr<EffectProcessor> AudioEngine::makeSlotNode(const EffectSlotSpec& spec)
{
    if (spec.kind != EffectKind::Plugin)
        return makeBuiltInNode(spec.kind);

    // Instantiated here, on the message thread: loading a binary and
    // running third-party initialisation must never happen under the
    // audio thread. A plugin this machine doesn't have becomes a node
    // that passes audio through rather than failing the load — the
    // document still remembers which one it wanted. A node rather than a
    // gap, so every later slot keeps the position the document gives it,
    // which is how parameters and automation find it.
    const double rate = sampleRate_.load(std::memory_order_relaxed);
    std::string  error;
    auto instance = pluginHost_.createInstance(spec.pluginFormat, spec.pluginIdentifier, rate > 0.0 ? rate : 48000.0,
                                               currentBlockSize_, &error);
    if (instance == nullptr)
        DBG("plugin unavailable: " << spec.pluginIdentifier.c_str() << " (" << error.c_str() << ")");

    auto node = std::make_unique<PluginNode>(std::move(instance));
    node->restoreState(spec.pluginState);
    return node;
}

void AudioEngine::setClipEffectParams(int clipId, int slotIndex, const EffectParamValues& values)
{
    const auto it = clipChains_.find(clipId);
    if (it != clipChains_.end() && slotIndex >= 0)
        it->second.chain->applyParams((size_t) slotIndex, values);
}

/** Raises the input meter's peaks, and flags a channel that reached full
    scale: a converter clips there, and a sample at 0.999 is already one. A
    mono input shows on both sides. Audio thread. */
void AudioEngine::meterInput(const float* const* inputChannelData, int numInputChannels, int numSamples) noexcept
{
    if (inputChannelData == nullptr || numInputChannels <= 0 || numSamples <= 0)
        return;

    // Every input, for the armed tracks' meters, each of which may take any.
    for (int ch = 0; ch < juce::jmin(numInputChannels, kMeteredInputs); ++ch)
    {
        const float* samples = inputChannelData[ch];
        if (samples == nullptr)
            continue;

        float peak = 0.0f;
        for (int n = 0; n < numSamples; ++n)
            peak = juce::jmax(peak, std::abs(samples[n]));

        auto& stored = channelPeak_[(size_t) ch];
        float before = stored.load(std::memory_order_relaxed);
        while (peak > before && ! stored.compare_exchange_weak(before, peak, std::memory_order_relaxed))
        {
        }
        if (peak >= 0.999f)
            channelClipped_[(size_t) ch].store(true, std::memory_order_relaxed);
    }

    // The inputs a take would record: a mono one shows on both sides.
    const int first    = meterFirstInput_.load(std::memory_order_relaxed);
    const int channels = meterChannels_.load(std::memory_order_relaxed);
    for (int ch = 0; ch < 2; ++ch)
    {
        const float* samples = inputChannelData[juce::jmin(first + juce::jmin(ch, channels - 1), numInputChannels - 1)];
        if (samples == nullptr)
            continue;

        float peak = 0.0f;
        for (int n = 0; n < numSamples; ++n)
            peak = juce::jmax(peak, std::abs(samples[n]));

        auto& stored = inputPeak_[(size_t) ch];
        float before = stored.load(std::memory_order_relaxed);
        while (peak > before && ! stored.compare_exchange_weak(before, peak, std::memory_order_relaxed))
        {
        }
        if (peak >= 0.999f)
            inputClipped_[(size_t) ch].store(true, std::memory_order_relaxed);
    }
}

void AudioEngine::setRetroactiveSeconds(double seconds)
{
    retroSeconds_ = juce::jmax(0.0, seconds);

    // The ring is reallocated, so the audio thread must not be in it: the
    // device is taken off for the moment it takes, as an export does.
    deviceManager_.removeAudioCallback(this);
    retro_.prepare(sampleRate_.load(std::memory_order_relaxed), retroSeconds_);
    deviceManager_.addAudioCallback(this);
}

bool AudioEngine::copyRecentInput(juce::AudioBuffer<float>& out, int64_t& startPlayhead) const
{
    // While playing, a second at the oldest end may be written over during
    // the copy, so it's left out.
    const int margin = transport_.isPlaying() ? (int) retro_.sampleRate() : 0;
    return retro_.copyLatestRun(out, startPlayhead, margin);
}

juce::StringArray AudioEngine::inputChannelNames()
{
    auto* device = deviceManager_.getCurrentAudioDevice();
    if (device == nullptr)
        return {};

    // Only the ones that are switched on in Audio Settings deliver anything.
    const auto names  = device->getInputChannelNames();
    const auto active = device->getActiveInputChannels();
    juce::StringArray result;
    for (int i = 0; i < names.size(); ++i)
        result.add(active[i] ? names[i] : names[i] + " (off)");
    return result;
}

int AudioEngine::reportedRoundTripSamples()
{
    auto* device = deviceManager_.getCurrentAudioDevice();
    return device != nullptr
             ? juce::jmax(0, device->getInputLatencyInSamples()) + juce::jmax(0, device->getOutputLatencyInSamples())
             : 0;
}

int64_t AudioEngine::countInLeadInSamples() const
{
    // The count-in is expressed in samples here, on the message thread, from
    // the tempo in force when recording starts — the audio thread only ever
    // counts it down (see AudioRecorder::process).
    // Measured from where the take will actually start rather than from a
    // single samples-per-bar figure: with a tempo map a bar's length depends on
    // where it is, so a count-in at bar 40 is not necessarily a count-in at
    // bar 1.
    const auto&   tempoMap  = transport_.tempoMap();
    const int64_t startFrom = transport_.playheadForUI();

    const double startBeat  = tempoMap.ppqFromSamples(startFrom);
    const double countBeats = tempoMap.quartersPerBar() * (double) countInBars_;
    return tempoMap.samplesFromPpq(startBeat + countBeats) - startFrom;
}

bool AudioEngine::beginRecording(const juce::File& destination)
{
    auto* device = deviceManager_.getCurrentAudioDevice();
    if (device == nullptr || device->getActiveInputChannels().countNumberOfSetBits() == 0)
        return false;

    // Opening the file is part of arming: a take that was never going to be
    // written should fail before the user plays it, not after.
    return recorder_.arm(destination, recordWriterThread_, countInLeadInSamples());
}

bool AudioEngine::beginExtraRecording(int slot, const juce::File& destination, const AudioRecorder::Format& format,
                                      bool joinNow)
{
    if (slot < 0 || slot >= kExtraTakes)
        return false;

    auto& extra = extraRecorders_[(size_t) slot];
    extra.prepare(sampleRate_.load(std::memory_order_relaxed), format.channels);
    extra.setFormat(format);
    extra.setSoundTrigger(recorder_.soundTriggerGain(), recorder_.soundTriggerStopSamples());
    // Joining a take that's running starts at once: its count-in, if it had
    // one, is long over.
    return extra.arm(destination, recordWriterThread_, joinNow ? 0 : countInLeadInSamples());
}

void AudioEngine::setActiveTrackCount(int count)
{
    const int clamped = juce::jlimit(0, kMaxTracks, count);
    for (int i = 0; i < kMaxTracks; ++i)
        tracks_[(size_t) i].active.store(i < clamped, std::memory_order_relaxed);
}

void AudioEngine::setTrackMuted(int index, bool muted)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].muted.store(muted, std::memory_order_relaxed);
}

void AudioEngine::setTrackSolo(int index, bool solo)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].solo.store(solo, std::memory_order_relaxed);
}

bool AudioEngine::trackContributesToMix(int index) const noexcept
{
    if (index < 0 || index >= kMaxTracks)
        return false;

    const auto& track = tracks_[(size_t) index];

    if (! track.active.load(std::memory_order_relaxed))
        return false;

    // Mute always wins, whatever solo says.
    if (track.muted.load(std::memory_order_relaxed))
        return false;

    // Solo through the routing: what feeds a soloed bus, and what a soloed
    // track feeds, are heard too (see mixrouting::soloAudible).
    std::array<mixrouting::Node, kMaxTracks> nodes;
    for (int i = 0; i < kMaxTracks; ++i)
    {
        const auto& t   = tracks_[(size_t) i];
        auto&       n   = nodes[(size_t) i];
        n.active    = t.active.load(std::memory_order_relaxed);
        n.isBus     = t.isBus.load(std::memory_order_relaxed);
        n.solo      = t.solo.load(std::memory_order_relaxed);
        n.output    = t.outputBus.load(std::memory_order_relaxed);
        n.sendCount = juce::jlimit(0, mixrouting::kMaxSends, t.sendCount.load(std::memory_order_relaxed));
        for (int s = 0; s < n.sendCount; ++s)
            n.sends[(size_t) s].bus = t.sendBus[(size_t) s].load(std::memory_order_relaxed);
    }

    bool audible[kMaxTracks] {};
    mixrouting::soloAudible(nodes.data(), kMaxTracks, audible);
    return audible[index];
}

void AudioEngine::setTrackRouting(int index, bool isBus, int outputBus, const std::vector<SendSpec>& sends)
{
    if (index < 0 || index >= kMaxTracks)
        return;

    auto&     track = tracks_[(size_t) index];
    const int count = juce::jmin((int) sends.size(), mixrouting::kMaxSends);

    // Fewer sends first, then the entries, then more: the audio thread
    // never reads an entry that isn't filled in yet.
    track.sendCount.store(juce::jmin(count, track.sendCount.load(std::memory_order_relaxed)), std::memory_order_release);
    for (int s = 0; s < count; ++s)
    {
        track.sendBus[(size_t) s].store(sends[(size_t) s].bus, std::memory_order_relaxed);
        track.sendGain[(size_t) s].store(juce::Decibels::decibelsToGain(sends[(size_t) s].gainDb), std::memory_order_relaxed);
        track.sendPreFader[(size_t) s].store(sends[(size_t) s].preFader, std::memory_order_relaxed);
    }
    track.sendCount.store(count, std::memory_order_release);

    track.isBus.store(isBus, std::memory_order_relaxed);
    track.outputBus.store(outputBus, std::memory_order_relaxed);
}

void AudioEngine::setTempoChanges(const std::vector<TempoChange>& changes)
{
    auto* map = new TempoMap();
    map->setSampleRate(transport_.tempoMap().sampleRate());
    map->setTempoChanges(changes);
    if (! tempoInbox_.push(map))
        delete map; // the queue is full: the next edit carries the same map
}

void AudioEngine::installIncomingTempoMap() noexcept
{
    TempoMap* incoming = nullptr;
    while (tempoInbox_.pop(incoming))
    {
        transport_.tempoMap().adopt(*incoming);
        (void) tempoReclaim_.push(incoming); // a full queue leaks it until the destructor
    }
}

void AudioEngine::setTrackSidechains(int index, const std::vector<std::pair<int, int>>& slotAndSource)
{
    if (index < 0 || index >= kMaxTracks)
        return;

    auto&     track = tracks_[(size_t) index];
    const int count = juce::jmin((int) slotAndSource.size(), mixrouting::kMaxKeys);
    track.keyCount.store(juce::jmin(count, track.keyCount.load(std::memory_order_relaxed)), std::memory_order_release);
    for (int k = 0; k < count; ++k)
    {
        track.keySlot[(size_t) k].store(slotAndSource[(size_t) k].first, std::memory_order_relaxed);
        track.keySource[(size_t) k].store(slotAndSource[(size_t) k].second, std::memory_order_relaxed);
    }
    track.keyCount.store(count, std::memory_order_release);
}

void AudioEngine::snapshotRouting(std::array<mixrouting::Node, kMaxTracks>& nodes) noexcept
{
    for (int i = 0; i < kMaxTracks; ++i)
    {
        auto& track = tracks_[(size_t) i];
        auto& node  = nodes[(size_t) i];
        node.active       = track.active.load(std::memory_order_relaxed);
        node.isBus        = track.isBus.load(std::memory_order_relaxed);
        node.solo         = track.solo.load(std::memory_order_relaxed);
        node.output       = track.outputBus.load(std::memory_order_relaxed);
        node.sendCount    = juce::jlimit(0, mixrouting::kMaxSends, track.sendCount.load(std::memory_order_acquire));
        for (int s = 0; s < node.sendCount; ++s)
            node.sends[(size_t) s] = { track.sendBus[(size_t) s].load(std::memory_order_relaxed),
                                       track.sendGain[(size_t) s].load(std::memory_order_relaxed),
                                       track.sendPreFader[(size_t) s].load(std::memory_order_relaxed) };
        node.chainLatency = node.active ? track.chainLatency() : 0;
        node.keyCount     = juce::jlimit(0, mixrouting::kMaxKeys, track.keyCount.load(std::memory_order_acquire));
        for (int k = 0; k < node.keyCount; ++k)
            node.keySources[(size_t) k] = track.keySource[(size_t) k].load(std::memory_order_relaxed);
    }
}

void AudioEngine::setTrackGainDb(int index, float gainDb)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].gainDb.store(gainDb, std::memory_order_relaxed);
}

void AudioEngine::setTrackPan(int index, float pan)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].pan.store(juce::jlimit(-1.0f, 1.0f, pan), std::memory_order_relaxed);
}

void AudioEngine::setTrackAutomation(int index, const TrackAutomation& curves)
{
    if (index < 0 || index >= kMaxTracks)
        return;

    auto& track = tracks_[(size_t) index];
    track.collectRetiredAutomation();
    track.setAutomation(new TrackAutomation(curves));
}

void AudioEngine::rebuildTrackEffectChain(int index, double rate)
{
    if (index < 0 || index >= kMaxTracks)
        return;

    auto chain = std::make_unique<EffectChain>();
    for (const auto& spec : chainStructure_[(size_t) index])
        if (auto node = makeSlotNode(spec))
            chain->add(std::move(node));

    // Its settings, as last set (see chainParams_). Before it's submitted,
    // while this thread is still the only one that can see it.
    const auto& params = chainParams_[(size_t) index];
    for (size_t i = 0; i < params.size(); ++i)
        chain->applyParams(i, params[i]); // a slot of another kind is left alone

    // Prepared here, on the message thread, where allocating a delay line is
    // allowed. A rate of zero means the device hasn't started yet; the rebuild
    // in audioDeviceAboutToStart covers that case.
    if (rate <= 0.0)
        rate = sampleRate_.load(std::memory_order_relaxed);
    if (rate > 0.0)
        chain->prepare(rate, currentBlockSize_);

    auto& track = tracks_[(size_t) index];
    track.collectRetiredEffectChain();

    submittedChain_[(size_t) index] = chain.get();
    track.setEffectChain(chain.release());
}

bool AudioEngine::setTrackEffectChain(int index, const std::vector<EffectSlotSpec>& slots)
{
    if (index < 0 || index >= kMaxTracks)
        return false;

    // Rebuilding resets every tail in the chain — and reinstantiates every
    // plugin — so only do it when the shape actually changed. A changed
    // preset or parameter is not a shape change.
    auto& existing = chainStructure_[(size_t) index];
    bool sameShape = existing.size() == slots.size() && submittedChain_[(size_t) index] != nullptr;
    for (size_t i = 0; sameShape && i < slots.size(); ++i)
        sameShape = existing[i].sameShapeAs(slots[i]);

    if (sameShape)
    {
        // A plugin whose saved state changed without the plugin changing it
        // (an undo, say) is given it.
        for (size_t i = 0; i < slots.size(); ++i)
            if (slots[i].kind == EffectKind::Plugin && slots[i].pluginState != existing[i].pluginState)
            {
                if (auto* node = trackPluginNode(index, (int) i))
                    node->restoreState(slots[i].pluginState);
                existing[i].pluginState = slots[i].pluginState;
            }
        return false;
    }

    // A new shape: the old slots' settings no longer line up with it. The
    // caller sends the new ones next (MainComponent::syncEngineTracks).
    chainStructure_[(size_t) index] = slots;
    chainParams_[(size_t) index].clear();
    rebuildTrackEffectChain(index);
    return true;
}

void AudioEngine::setTrackEffectSlotParams(int index, int slotIndex, const EffectParamValues& values)
{
    if (index < 0 || index >= kMaxTracks || slotIndex < 0)
        return;

    auto& kept = chainParams_[(size_t) index];
    if ((size_t) slotIndex >= kept.size())
        kept.resize((size_t) slotIndex + 1);
    kept[(size_t) slotIndex] = values;

    if (auto* chain = submittedChain_[(size_t) index])
        chain->applyParams((size_t) slotIndex, values);
}

void AudioEngine::setTrackEffectParam(int index, int slotIndex, EffectKind kind, const std::string& paramId,
                                      float value)
{
    if (index < 0 || index >= kMaxTracks || slotIndex < 0)
        return;

    if (auto& kept = chainParams_[(size_t) index]; (size_t) slotIndex < kept.size() && kept[(size_t) slotIndex].kind == kind)
        kept[(size_t) slotIndex].set(paramId, value);

    if (auto* chain = submittedChain_[(size_t) index])
        if (auto* node = chain->nodeAt((size_t) slotIndex); node != nullptr && node->kind() == kind)
            node->setParam(paramId, value);
}

PluginNode* AudioEngine::trackPluginNode(int index, int slotIndex)
{
    if (index < 0 || index >= kMaxTracks || slotIndex < 0)
        return nullptr;

    auto* chain = submittedChain_[(size_t) index];
    return chain != nullptr ? dynamic_cast<PluginNode*>(chain->nodeAt((size_t) slotIndex)) : nullptr;
}

void AudioEngine::rebuildMasterEffectChain(double rate)
{
    auto chain = std::make_unique<EffectChain>();
    for (const auto& spec : masterChainStructure_)
        if (auto node = makeSlotNode(spec))
            chain->add(std::move(node));
    for (size_t i = 0; i < masterChainParams_.size(); ++i)
        chain->applyParams(i, masterChainParams_[i]);

    if (rate <= 0.0)
        rate = sampleRate_.load(std::memory_order_relaxed);
    if (rate > 0.0)
        chain->prepare(rate, currentBlockSize_);

    masterChain_.collectRetired();
    submittedMasterChain_ = chain.get();
    masterChain_.submit(chain.release());
}

bool AudioEngine::setMasterEffectChain(const std::vector<EffectSlotSpec>& slots)
{
    // Only a new shape rebuilds, as for a track (see setTrackEffectChain).
    bool sameShape = masterChainStructure_.size() == slots.size() && submittedMasterChain_ != nullptr;
    for (size_t i = 0; sameShape && i < slots.size(); ++i)
        sameShape = masterChainStructure_[i].sameShapeAs(slots[i]);

    if (sameShape)
    {
        for (size_t i = 0; i < slots.size(); ++i)
            if (slots[i].kind == EffectKind::Plugin && slots[i].pluginState != masterChainStructure_[i].pluginState)
            {
                if (auto* node = masterPluginNode((int) i))
                    node->restoreState(slots[i].pluginState);
                masterChainStructure_[i].pluginState = slots[i].pluginState;
            }
        return false;
    }

    masterChainStructure_ = slots;
    masterChainParams_.clear();
    rebuildMasterEffectChain();
    return true;
}

void AudioEngine::setMasterEffectSlotParams(int slotIndex, const EffectParamValues& values)
{
    if (slotIndex < 0)
        return;
    if ((size_t) slotIndex >= masterChainParams_.size())
        masterChainParams_.resize((size_t) slotIndex + 1);
    masterChainParams_[(size_t) slotIndex] = values;

    if (submittedMasterChain_ != nullptr)
        submittedMasterChain_->applyParams((size_t) slotIndex, values);
}

void AudioEngine::noteMasterPluginState(int slotIndex, const std::string& state)
{
    if (slotIndex >= 0 && slotIndex < (int) masterChainStructure_.size())
        masterChainStructure_[(size_t) slotIndex].pluginState = state;
}

PluginNode* AudioEngine::masterPluginNode(int slotIndex)
{
    if (submittedMasterChain_ == nullptr || slotIndex < 0)
        return nullptr;
    return dynamic_cast<PluginNode*>(submittedMasterChain_->nodeAt((size_t) slotIndex));
}

void AudioEngine::pump() noexcept
{
    for (auto& track : tracks_)
    {
        track.audioPlayer.collectRetiredClips();
        track.collectRetiredAutomation();
        track.collectRetiredEffectChain();
    }

    filePlayer_.collectRetiredClips();
    audition_.collectRetired();
    reference_.collectRetired();
    masterChain_.collectRetired();

    TempoMap* retiredTempo = nullptr;
    while (tempoReclaim_.pop(retiredTempo))
        delete retiredTempo;
}

void AudioEngine::drainCommandQueue() noexcept
{
    // The tempo map first, so a SetTempo queued after it edits the new one.
    installIncomingTempoMap();

    EngineCommand command;
    while (commandQueue_.pop(command))
    {
        switch (command.type)
        {
            case EngineCommand::Type::SetPlaying:      transport_.setPlaying(command.a != 0.0); break;
            case EngineCommand::Type::SetLooping:      transport_.setLooping(command.a != 0.0); break;
            case EngineCommand::Type::Seek:            transport_.seek((int64_t) command.a); varispeed_.reset(); break;
            case EngineCommand::Type::SetTempo:        transport_.setTempo(command.a); break;
            case EngineCommand::Type::SetLoopRegion:   transport_.setLoopRegion((int64_t) command.a, (int64_t) command.b); break;
            case EngineCommand::Type::SetMasterGainDb: master_.setGainDb((float) command.a); break;
            case EngineCommand::Type::SetTimeSignature:
                transport_.tempoMap().setTimeSignature((int) command.a, (int) command.b);
                break;
        }
    }
}

void AudioEngine::audioDeviceIOCallbackWithContext(const float* const* inputChannelData,
                                                   int numInputChannels,
                                                   float* const* outputChannelData,
                                                   int numOutputChannels,
                                                   int numSamples,
                                                   const juce::AudioIODeviceCallbackContext& /*context*/)
{
    rt::markCurrentThreadAsAudioThread();
    drainCommandQueue();

    juce::AudioBuffer<float> output(outputChannelData, numOutputChannels, numSamples);
    output.clear();

    ProcessContext context;
    context.sampleRate = sampleRate_.load(std::memory_order_relaxed);
    context.numSamples = numSamples;
    context.transport  = transport_.snapshot(context.numSamples);

    // Before anything reads a streamed clip's pages: see ClipStream.
    context.streamEpoch = streamer_.beginBlock();

    recorder_.process(inputChannelData, numInputChannels, numSamples,
                      context.transport.playing, context.transport.playheadSamples);
    for (auto& extra : extraRecorders_)
        extra.process(inputChannelData, numInputChannels, numSamples,
                      context.transport.playing, context.transport.playheadSamples);
    meterInput(inputChannelData, numInputChannels, numSamples);
    retro_.process(inputChannelData, numInputChannels, numSamples, context.transport.playing,
                   context.transport.playheadSamples);

    // Play-at-speed renders the song in blocks of its own; everything below
    // that isn't the song (monitoring, preview, click) stays at the device's.
    const double speed    = playSpeed_.load(std::memory_order_relaxed);
    const bool   atSpeed  = context.transport.playing && std::abs(speed - 1.0) > 1.0e-6
                         && ! recorder_.isArmed() && ! isCountingIn()
                         && varispeed_.isPrepared() && numSamples <= varispeed_.maxOutputFrames();

    if (atSpeed)
    {
        renderAtSpeed(output, speed, numSamples);
    }
    else
    {
        if (varispeedActive_)
        {
            varispeed_.reset();
            varispeedActive_ = false;
        }
        processBlock(output, context);
    }

    // A/B against a reference: on what the song made, before monitoring and
    // the click, which stay as they are whichever is heard.
    reference_.process(outputChannelData, numOutputChannels, numSamples, context.transport.playheadSamples,
                       context.sampleRate, context.transport.playing);

    // Dry input monitoring, mixed in *after* the master bus for the same
    // reasons the metronome is: it stays out of the meter, out of the master
    // effects, and — because renderOffline only ever calls processBlock — it
    // can never end up in an exported file.
    // Not while measuring: with a cable from an output to an input, the
    // monitored input would come straight back round as a second click.
    if (! latencyProbe_.isRunning())
        mixInputMonitoring(output, inputChannelData, numInputChannels, numSamples);

    // An effect preview, on its own clock. Mixed in here, outside
    // processBlock, for the same reason as the monitoring: it must never
    // reach an exported file.
    audition_.process(output, numSamples);

    // After the master bus deliberately: the click bypasses the master
    // effects and gain, stays off the meter, and can never be exported (it
    // sits outside processBlock, which is what renderOffline renders).
    // `force` sounds it through a count-in even when it's otherwise off.
    // The click follows the song's beats, which at another speed are not the
    // device's: rather than a click in the wrong place, none.
    if (! atSpeed)
        metronome_.process(output, context, isCountingIn());

    // Last, so the click goes out as it is: see LatencyProbe.
    latencyProbe_.process(inputChannelData, numInputChannels, outputChannelData, numOutputChannels, numSamples);
}

void AudioEngine::renderAtSpeed(juce::AudioBuffer<float>& output, double speed, int numSamples) noexcept
{
    varispeedActive_ = true;

    const int blockSize = varispeedScratch_.getNumSamples();
    const double rate   = sampleRate_.load(std::memory_order_relaxed);

    for (int need = varispeed_.framesNeeded(numSamples, speed); need > 0;)
    {
        const int n = juce::jmin(need, blockSize);

        juce::AudioBuffer<float> block(varispeedScratch_.getArrayOfWritePointers(),
                                       varispeedScratch_.getNumChannels(), n);
        block.clear();

        ProcessContext context;
        context.sampleRate  = rate;
        context.numSamples  = n;
        context.transport   = transport_.snapshot(n);
        context.streamEpoch = streamer_.beginBlock();

        processBlock(block, context);

        varispeed_.push(block.getArrayOfReadPointers(), block.getNumChannels(), n);
        need -= n;
    }

    varispeed_.pull(output.getArrayOfWritePointers(), output.getNumChannels(), numSamples, speed);
}

void AudioEngine::mixInputMonitoring(juce::AudioBuffer<float>& output,
                                     const float* const* inputChannelData,
                                     int numInputChannels, int numSamples) noexcept
{
    const float target = inputMonitoring_.load(std::memory_order_relaxed)
                             ? juce::jlimit(0.0f, 2.0f, inputMonitorGain_.load(std::memory_order_relaxed))
                             : 0.0f;

    // Nothing to do, and nothing to ramp down from.
    if (target <= 0.0f && monitorGainRamp_ <= 0.0f)
        return;

    if (inputChannelData == nullptr || numInputChannels <= 0 || numSamples <= 0)
    {
        monitorGainRamp_ = target;
        return;
    }

    // Ramped across the block rather than switched: toggling monitoring
    // mid-take would otherwise put a step in the output, which through
    // headphones at tracking level is unpleasant.
    const float startGain = monitorGainRamp_;
    const float step      = (target - startGain) / (float) numSamples;

    const int channels = output.getNumChannels();
    for (int ch = 0; ch < channels; ++ch)
    {
        // A mono source is heard on both sides rather than only the left.
        const int   source = juce::jmin(ch, numInputChannels - 1);
        const auto* input  = inputChannelData[source];
        if (input == nullptr)
            continue;

        auto* out = output.getWritePointer(ch);
        float gain = startGain;

        for (int n = 0; n < numSamples; ++n)
        {
            out[n] += input[n] * gain;
            gain += step;
        }
    }

    monitorGainRamp_ = target;
}

int AudioEngine::latestTrackLatency() noexcept
{
    // Along each track's way out, through the buses it feeds.
    std::array<mixrouting::Node, kMaxTracks> nodes;
    snapshotRouting(nodes);
    return juce::jmin(mixrouting::latestPath(nodes.data(), kMaxTracks), MixerTrack::kMaxCompensation - 1);
}

void AudioEngine::processBlock(juce::AudioBuffer<float>& output, const ProcessContext& context,
                               int soloTrack, bool applyMasterBus) noexcept
{
    const int numSamples = context.numSamples;

    // The routing for this block (engine/MixRouting.h): who renders before
    // whom, who solo leaves audible, and how late each track's path is.
    std::array<mixrouting::Node, kMaxTracks> nodes;
    snapshotRouting(nodes);

    std::array<int, kMaxTracks> order {};
    const int orderCount = mixrouting::renderOrder(nodes.data(), kMaxTracks, order.data());

    bool audible[kMaxTracks] {};
    mixrouting::soloAudible(nodes.data(), kMaxTracks, audible);

    // Delay compensation: every track is played as late as the latest path's
    // effects make it - its own and every bus's on its way out - so a plugin
    // with latency doesn't leave its track behind the others. Taken over
    // every active track even for a stem, so a stem lines up with the mix it
    // came from.
    const int latest = juce::jmin(mixrouting::latestPath(nodes.data(), kMaxTracks),
                                  MixerTrack::kMaxCompensation - 1);

    // A stem renders one track: straight to the output, as it leaves its
    // fader. A bus's stem is the bus with what feeds it.
    std::array<bool, kMaxTracks> needed {};
    if (soloTrack >= 0 && soloTrack < kMaxTracks)
    {
        needed[(size_t) soloTrack] = true;
        for (bool grew = true; grew;)
        {
            grew = false;
            for (int i = 0; i < kMaxTracks; ++i)
            {
                if (needed[(size_t) i] || ! nodes[(size_t) i].active)
                    continue;
                bool feeds = false;
                if (const int out = mixrouting::outputOf(nodes.data(), kMaxTracks, i); out >= 0 && needed[(size_t) out])
                    feeds = true;
                for (int s = 0; s < nodes[(size_t) i].sendCount && ! feeds; ++s)
                    if (const int bus = mixrouting::validBus(nodes.data(), kMaxTracks, i, nodes[(size_t) i].sends[(size_t) s].bus);
                        bus >= 0 && needed[(size_t) bus])
                        feeds = true;

                // A key a needed track listens to: rendered for that, though
                // nothing of it reaches the stem.
                for (int j = 0; j < kMaxTracks && ! feeds; ++j)
                    if (needed[(size_t) j])
                        for (int k = 0; k < nodes[(size_t) j].keyCount; ++k)
                            if (mixrouting::validKey(nodes.data(), kMaxTracks, j, nodes[(size_t) j].keySources[(size_t) k]) == i)
                                feeds = true;
                if (feeds)
                    needed[(size_t) i] = grew = true;
            }
        }
    }

    // Who's listened to, so they keep their output for their listeners.
    std::array<bool, kMaxTracks> isKey {};
    for (int i = 0; i < kMaxTracks; ++i)
        for (int k = 0; k < nodes[(size_t) i].keyCount; ++k)
            if (const int source = mixrouting::validKey(nodes.data(), kMaxTracks, i, nodes[(size_t) i].keySources[(size_t) k]); source >= 0)
                isKey[(size_t) source] = true;

    if (silentKey_.getNumSamples() < numSamples)
        silentKey_.setSize(2, numSamples, false, false, true);
    silentKey_.clear();

    // Each bus starts the block empty; its sources add in before it renders.
    for (int i = 0; i < kMaxTracks; ++i)
        if (nodes[(size_t) i].active && nodes[(size_t) i].isBus)
            tracks_[(size_t) i].busInput.clear();

    std::array<bool, kMaxTracks> rendered {};
    for (int k = 0; k < orderCount; ++k)
    {
        const int i = order[(size_t) k];
        if (soloTrack >= 0 && ! needed[(size_t) i])
            continue;

        const auto& node = nodes[(size_t) i];
        MixerTrack::Destinations to;
        to.audible = audible[i];
        to.delay   = mixrouting::compensationFor(nodes.data(), kMaxTracks, i, latest);

        // A bus already rendered this block (only inside a loop) can't take
        // more; what would have gone there is dropped rather than mixed late.
        const auto busInputOf = [&](int bus) -> juce::AudioBuffer<float>*
        {
            if (bus < 0 || rendered[(size_t) bus] || (soloTrack >= 0 && ! needed[(size_t) bus]))
                return nullptr;
            return &tracks_[(size_t) bus].busInput;
        };

        const int out = mixrouting::outputOf(nodes.data(), kMaxTracks, i);
        if (soloTrack >= 0)
            to.output = i == soloTrack ? &output : busInputOf(out);
        else
            to.output = out >= 0 ? busInputOf(out) : &output;

        for (int s = 0; s < node.sendCount && soloTrack != i; ++s)
        {
            const auto& send = node.sends[(size_t) s];
            if (auto* target = busInputOf(mixrouting::validBus(nodes.data(), kMaxTracks, i, send.bus)))
            {
                to.sends[(size_t) to.sendCount]        = target;
                to.sendGains[(size_t) to.sendCount]    = send.gain;
                to.sendPreFader[(size_t) to.sendCount] = send.preFader;
                ++to.sendCount;
            }
        }

        // Keys: the source's output if it has rendered this block, else
        // silence - never the track's own input, which would be a different
        // effect from the one asked for.
        to.keepKey = isKey[(size_t) i];
        for (int k = 0; k < node.keyCount && to.keyCount < mixrouting::kMaxKeys; ++k)
        {
            const int source = mixrouting::validKey(nodes.data(), kMaxTracks, i, node.keySources[(size_t) k]);
            to.keySlots[(size_t) to.keyCount] = tracks_[(size_t) i].keySlot[(size_t) k].load(std::memory_order_relaxed);
            to.keys[(size_t) to.keyCount]     = source >= 0 && rendered[(size_t) source] ? &tracks_[(size_t) source].keyOutput
                                                                                         : &silentKey_;
            ++to.keyCount;
        }

        tracks_[(size_t) i].renderRouted(context, to);
        rendered[(size_t) i] = true;
    }

    if (applyMasterBus)
    {
        filePlayer_.process(output, context);
        if (auto* chain = masterChain_.adopt(); chain != nullptr && ! chain->empty())
        {
            chain->setBpm(context.transport.bpm);
            chain->process(output);
        }
        // The mastering rack sits between the master chain and the output
        // node, so its limiter is the last thing to touch level before the
        // meter reads it — a ceiling that something after it could exceed
        // wouldn't be one.
        mastering_.process(output);
        master_.process(output, context);
    }

    transport_.advance(numSamples);
}

juce::AudioBuffer<float> AudioEngine::renderOffline(const OfflineRenderOptions& options)
{
    const double startBeats  = options.startBeats;
    const double lengthBeats = options.lengthBeats;

    // With no device (a headless render) there is no device rate to return
    // to afterwards, so the render's own stands in for it.
    const double deviceRate  = sampleRate_.load(std::memory_order_relaxed);
    const double sampleRate  = options.sampleRate > 0.0 ? options.sampleRate : deviceRate;
    const double restoreRate = deviceRate > 0.0 ? deviceRate : sampleRate;
    const double bpm         = transport_.tempoMap().tempo();

    juce::AudioBuffer<float> output(2, 0);
    if (sampleRate <= 0.0 || bpm <= 0.0 || lengthBeats <= 0.0)
        return output;

    const int blockSize = juce::jmax(1, options.blockSize);

    // Suspending the device is what makes this safe rather than a race: the
    // mixer's state has exactly one writer by design, and the device thread
    // is otherwise inside processBlock at the same time as this loop.
    deviceManager_.removeAudioCallback(this);

    // Only the device callback drains this, so with the callback removed
    // anything still queued would sit unapplied for the whole render — a
    // tempo change made just before hitting Bounce would silently not be in
    // the file. Drained here, while this thread is the only one running.
    drainCommandQueue();

    const bool    wasPlaying  = transport_.isPlaying();
    const bool    wasLooping  = transport_.isLooping();
    const int64_t wasPlayhead = transport_.playhead();

    // Fresh state, so a render is reproducible instead of inheriting whatever
    // tails happened to be ringing when the user hit Export - and prepared for
    // the *render* rate, which is what makes exporting at 44.1 from a 48k
    // device a native render rather than a resample.
    prepareAll(sampleRate, blockSize);

    // Through the map: a render's length in samples is the distance between
    // two musical positions, not a beat count times one tempo. Worked out
    // only now that the map is at the *render* rate: before the prepare above
    // it counted at the device's, which made an export at another rate the
    // wrong length - at 48 kHz from a 44.1 kHz device, about 8% short, cut
    // off before the end of the song.
    const auto&   tempoMap     = transport_.tempoMap();
    const int64_t startSample  = tempoMap.samplesFromPpq(startBeats);
    const int     totalSamples = juce::jmax(0, (int) (tempoMap.samplesFromPpq(startBeats + lengthBeats) - startSample));

    // Looping off for the duration: a loop region set for auditioning would
    // otherwise wrap the playhead mid-export and repeat a section.
    transport_.setLooping(false);
    transport_.seek(startSample);
    transport_.setPlaying(true);

    // The tracks are played as late as their latest effects make them, and
    // the mastering rack's lookahead adds to that: rendered that much longer
    // and trimmed from the start below, so the file lines up with the song
    // rather than starting late.
    const int latency = latestTrackLatency() + (options.applyMasterBus ? mastering_.latencySamples() : 0);
    const int rendered = totalSamples > 0 ? totalSamples + latency : 0;

    output.setSize(2, rendered);
    output.clear();

    // Every 16th block rather than every block: at 512 samples that is roughly
    // every 190ms, which is smooth enough for a progress bar while keeping a
    // std::function call out of the inner loop.
    constexpr int kBlocksPerProgressReport = 16;

    bool cancelled = false;
    int  blockIndex = 0;

    for (int pos = 0; pos < rendered; pos += blockSize, ++blockIndex)
    {
        const int n = juce::jmin(blockSize, rendered - pos);

        ProcessContext context;
        context.sampleRate = sampleRate;
        context.numSamples = n;
        context.transport  = transport_.snapshot(context.numSamples);

        // This thread stands in for the audio thread, so it advances the
        // block epoch the same way, and being offline it loads streamed audio
        // it needs rather than skipping it.
        context.streamEpoch = streamer_.beginBlock();
        context.offline     = true;

        juce::AudioBuffer<float> view(output.getArrayOfWritePointers(), 2, pos, n);
        processBlock(view, context, options.soloTrack, options.applyMasterBus);

        if (options.onProgress != nullptr && blockIndex % kBlocksPerProgressReport == 0)
        {
            if (! options.onProgress((double) (pos + n) / (double) rendered))
            {
                cancelled = true;
                break; // the restore below still runs — that is the point of breaking rather than returning
            }
        }
    }

    // Back to the *device* rate, not the render rate: everything above was
    // prepared for the export, and leaving it that way would have live
    // playback running at the wrong rate the moment the callback returns.
    // Also again on the way out at all, so playback doesn't start up holding
    // the render's delay and reverb tails.
    prepareAll(restoreRate, blockSize);

    // After that prepare, because wasPlayhead is a sample count at the device
    // rate and prepareAll is what puts the transport's tempo map back on that
    // rate. Restoring it first would place the playhead using the render's
    // clock.
    transport_.setPlaying(wasPlaying);
    transport_.setLooping(wasLooping);
    transport_.seek(wasPlayhead);

    deviceManager_.addAudioCallback(this);

    if (cancelled || totalSamples <= 0)
        output.setSize(2, 0); // see OfflineRenderOptions::onProgress
    else if (latency > 0)
    {
        juce::AudioBuffer<float> aligned(2, totalSamples);
        for (int ch = 0; ch < 2; ++ch)
            aligned.copyFrom(ch, 0, output, ch, latency, totalSamples);
        return aligned;
    }

    return output;
}

void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    const double sampleRate = device->getCurrentSampleRate();
    const int    blockSize  = device->getCurrentBufferSizeSamples();

    sampleRate_.store(sampleRate, std::memory_order_relaxed);

    // Before the callback starts: the room is for this rate, and what was
    // kept at another one can't be placed any more.
    retro_.prepare(sampleRate, retroSeconds_);

    prepareAll(sampleRate, blockSize);
}

void AudioEngine::prepareAll(double sampleRate, int blockSize)
{
    transport_.prepare(sampleRate);

    currentBlockSize_ = blockSize;

    // Room for a device that hands over up to twice the block it promised.
    varispeedScratch_.setSize(2, juce::jmax(1, blockSize));
    varispeed_.prepare(2, juce::jmax(1, blockSize) * 2, juce::jmax(1, blockSize));

    for (auto& track : tracks_)
        track.prepare(sampleRate, blockSize);

    // A chain must be prepared for the rate it will actually run at, and one
    // submitted before the device started (or before a rate change) was
    // prepared for the wrong rate, or not at all. Rebuilding here guarantees
    // it; device starts are rare enough that the cost doesn't matter.
    for (int i = 0; i < kMaxTracks; ++i)
        rebuildTrackEffectChain(i, sampleRate);

    // Clips' chains are prepared again rather than rebuilt: nothing is playing
    // them while this runs, and rebuilding would lose their settings.
    for (auto& [clipId, entry] : clipChains_)
        entry.chain->prepare(sampleRate, blockSize);

    filePlayer_.prepare(sampleRate, blockSize);
    silentKey_.setSize(2, juce::jmax(1, blockSize));
    silentKey_.clear();
    audition_.prepare(sampleRate);
    rebuildMasterEffectChain(sampleRate);
    mastering_.prepare(sampleRate, blockSize);
    master_.prepare(sampleRate, blockSize);

    recorder_.prepare(sampleRate, recorder_.format().channels);
    metronome_.prepare(sampleRate);
}

juce::String AudioEngine::systemDefaultOutputName()
{
    auto* type = deviceManager_.getCurrentDeviceTypeObject();
    if (type == nullptr)
        return {};

    // Without a rescan the list is whatever it was when the type was created,
    // so a device that has just been plugged in isn't in it yet — which is
    // precisely the moment this gets asked.
    type->scanForDevices();

    const auto names = type->getDeviceNames(false); // false = outputs
    const int  index = type->getDefaultDeviceIndex(false);

    return juce::isPositiveAndBelow(index, names.size()) ? names[index] : juce::String {};
}

juce::String AudioEngine::followSystemDefaultOutput()
{
    const auto defaultName = systemDefaultOutputName();
    if (defaultName.isEmpty())
        return {};

    if (auto* current = deviceManager_.getCurrentAudioDevice())
        if (current->getName() == defaultName)
            return {}; // already on it

    auto setup = deviceManager_.getAudioDeviceSetup();
    setup.outputDeviceName         = defaultName;
    setup.useDefaultOutputChannels = true;

    // treatAsChosenDevice = false: this is the app following the system, not
    // the user picking something, so it shouldn't be written back as a
    // remembered preference.
    const auto error = deviceManager_.setAudioDeviceSetup(setup, false);
    if (error.isNotEmpty())
        return {};

    return defaultName;
}

void AudioEngine::audioDeviceStopped()
{
    filePlayer_.release();
}

} // namespace soundsplice::engine
