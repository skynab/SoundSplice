#include "engine/AudioEngine.h"

#include "rt/RealtimeGuard.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

namespace looper::engine
{
namespace
{
}

AudioEngine::AudioEngine()
{
    formatManager_.registerBasicFormats();

    // Started once and left running for the engine's lifetime. It idles when
    // nothing is recording, and spinning a thread up at the instant the user
    // hits record is exactly the wrong moment to be doing it.
    recordWriterThread_.startThread(juce::Thread::Priority::normal);

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

    for (auto& source : sidechainSource_)
        source.store(-1, std::memory_order_relaxed);

    for (auto& bus : outputBus_)
        bus.store(-1, std::memory_order_relaxed);

    refreshMidiInputs();
}

AudioEngine::~AudioEngine()
{
    // Whatever we actually registered, not whatever happens to be plugged in
    // now — a device unplugged during the session is not in
    // getAvailableDevices() any more, and its callback would never be removed.
    for (const auto& identifier : registeredMidiInputs_)
        deviceManager_.removeMidiInputDeviceCallback(identifier, this);

    deviceManager_.removeAudioCallback(this);
    deviceManager_.closeAudioDevice();
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

bool AudioEngine::refreshMidiInputs()
{
    const auto available = juce::MidiInput::getAvailableDevices();

    juce::StringArray current;
    for (const auto& input : available)
        current.add(input.identifier);

    bool changed = false;

    // Newly arrived: enable and register. Registering an identifier twice
    // would deliver every message twice, so what has already been registered
    // is tracked here rather than re-derived from the device list.
    for (const auto& input : available)
    {
        if (registeredMidiInputs_.contains(input.identifier))
            continue;

        deviceManager_.setMidiInputDeviceEnabled(input.identifier, true);
        deviceManager_.addMidiInputDeviceCallback(input.identifier, this);
        registeredMidiInputs_.add(input.identifier);
        changed = true;
    }

    // Gone: unregister, so a controller can be unplugged and replaced without
    // accumulating dead callbacks for the engine's whole lifetime.
    for (int i = registeredMidiInputs_.size(); --i >= 0;)
    {
        const auto identifier = registeredMidiInputs_[i];
        if (current.contains(identifier))
            continue;

        deviceManager_.removeMidiInputDeviceCallback(identifier, this);
        registeredMidiInputs_.remove(i);
        changed = true;
    }

    return changed;
}

bool AudioEngine::hasMidiInput() const
{
    return ! registeredMidiInputs_.isEmpty();
}

bool AudioEngine::hasAudioInput() const
{
    auto* device = deviceManager_.getCurrentAudioDevice();
    return device != nullptr && device->getActiveInputChannels().countNumberOfSetBits() > 0;
}

void AudioEngine::handleIncomingMidiMessage(juce::MidiInput* /*source*/, const juce::MidiMessage& message)
{
    midiCollector_.addMessageToQueue(message);
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

std::shared_ptr<ClipData> AudioEngine::decodeOrGetCached(const juce::File& file)
{
    const auto path = file.getFullPathName();
    auto       it   = audioDecodeCache_.find(path);
    if (it != audioDecodeCache_.end())
        return it->second;

    auto decoded = decodeAudioFile(file);
    if (decoded == nullptr)
        return nullptr;

    std::shared_ptr<ClipData> shared(decoded.release());
    audioDecodeCache_[path] = shared;
    return shared;
}

TempoEstimate AudioEngine::detectFileTempo(const juce::File& file)
{
    auto decoded = decodeOrGetCached(file);
    if (decoded == nullptr || decoded->lengthSamples <= 0)
        return {};

    // The *unwarped* audio, deliberately: detection has to describe the file
    // as it is on disk, or re-detecting a warped clip would report the tempo
    // it was warped to and then warp it again from there.
    const int numChannels = juce::jmax(1, decoded->audio.getNumChannels());
    const int length      = decoded->lengthSamples;

    std::vector<std::vector<float>> channels((size_t) numChannels);
    for (int channel = 0; channel < numChannels; ++channel)
    {
        const float* read = decoded->audio.getReadPointer(channel);
        channels[(size_t) channel].assign(read, read + length);
    }

    return detectTempo(channels, decoded->sourceSampleRate);
}

std::shared_ptr<ClipData> AudioEngine::warpedOrGetCached(const juce::File& file, double stretchFactor)
{
    auto source = decodeOrGetCached(file);

    // Not warped, or warped by so little it would only cost quality: the
    // phase vocoder is not transparent, so running it for a 0.01% correction
    // makes the audio worse rather than better.
    if (source == nullptr || std::abs(stretchFactor - 1.0) < 1.0e-4)
        return source;

    const auto path = file.getFullPathName();

    auto cached = audioWarpCache_.find(path);
    if (cached != audioWarpCache_.end()
        && std::abs(cached->second.stretchFactor - stretchFactor) < 1.0e-9
        && cached->second.data != nullptr)
    {
        return cached->second.data;
    }

    const int numChannels = juce::jmax(1, source->numChannels);
    const int length      = source->lengthSamples;
    if (length <= 0)
        return source;

    auto warped = std::make_shared<ClipData>();
    warped->sourceSampleRate = source->sourceSampleRate;
    warped->numChannels      = numChannels;

    // Channel at a time, since timeStretch works on a plain vector. Each
    // channel is stretched by the same factor and so comes back the same
    // length; the shortest is taken as the truth rather than assumed, because
    // a length mismatch here would read as one channel of silence at the end.
    std::vector<std::vector<float>> stretched((size_t) numChannels);
    int stretchedLength = std::numeric_limits<int>::max();

    for (int channel = 0; channel < numChannels; ++channel)
    {
        const float* read = source->audio.getReadPointer(juce::jmin(channel, source->audio.getNumChannels() - 1));

        std::vector<float> samples((size_t) length);
        std::copy(read, read + length, samples.begin());

        stretched[(size_t) channel] = timestretch::timeStretch(samples, stretchFactor);
        stretchedLength = juce::jmin(stretchedLength, (int) stretched[(size_t) channel].size());
    }

    if (stretchedLength <= 0 || stretchedLength == std::numeric_limits<int>::max())
        return source; // the vocoder declined (too short to frame); play it unwarped

    warped->audio.setSize(numChannels, stretchedLength);
    warped->lengthSamples = stretchedLength;

    for (int channel = 0; channel < numChannels; ++channel)
        warped->audio.copyFrom(channel, 0, stretched[(size_t) channel].data(), stretchedLength);

    audioWarpCache_[path] = { stretchFactor, warped };
    return warped;
}

bool AudioEngine::setTrackAudioClips(int index, const std::vector<AudioClipSpec>& clips)
{
    if (index < 0 || index >= kMaxTracks)
        return false;

    auto* slots = new std::vector<AudioClipSlot>();
    slots->reserve(clips.size());
    bool allOk = true;

    for (const auto& spec : clips)
    {
        auto decoded = warpedOrGetCached(spec.file, spec.stretchFactor);
        if (decoded == nullptr)
        {
            allOk = false;
            continue; // skip this clip; the others still load
        }
        slots->push_back({ decoded, spec.startBeats, spec.lengthBeats,
                           juce::Decibels::decibelsToGain(spec.gainDb) });
    }

    auto& track = tracks_[(size_t) index];
    track.audioPlayer.collectRetiredClips();
    track.audioPlayer.submitClips(slots);
    return allOk;
}

void AudioEngine::setTrackSidechainSource(int index, int sourceTrackIndex)
{
    if (index < 0 || index >= kMaxTracks)
        return;

    // A track cannot duck itself: that is just an ordinary compressor with
    // extra routing, and treating it as a sidechain would mean reading the
    // very buffer being written.
    const int resolved = (sourceTrackIndex >= 0 && sourceTrackIndex < kMaxTracks
                          && sourceTrackIndex != index)
                             ? sourceTrackIndex : -1;

    sidechainSource_[(size_t) index].store(resolved, std::memory_order_relaxed);
}

void AudioEngine::setTrackIsBus(int index, bool isBus)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].isBus.store(isBus, std::memory_order_relaxed);
}

void AudioEngine::setTrackOutputBus(int index, int busTrackIndex)
{
    if (index < 0 || index >= kMaxTracks)
        return;

    // A track cannot feed itself, and a bus feeding a bus is not supported in
    // this pass — both resolve to the master rather than to a routing loop the
    // audio thread would have to detect every block.
    const bool valid = busTrackIndex >= 0 && busTrackIndex < kMaxTracks
                    && busTrackIndex != index
                    && ! tracks_[(size_t) index].isBus.load(std::memory_order_relaxed);

    outputBus_[(size_t) index].store(valid ? busTrackIndex : -1, std::memory_order_relaxed);
}

void AudioEngine::setTrackInstrument(int index, TrackInstrument instrument)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].instrument.store(instrument, std::memory_order_relaxed);
}

void AudioEngine::setTrackGuitarSettings(int index, const model::GuitarSettings& settings)
{
    if (index < 0 || index >= kMaxTracks)
        return;

    auto& guitar = tracks_[(size_t) index].guitar;
    guitar.setDecaySeconds(settings.decaySeconds);
    guitar.setBrightness(settings.brightness);
    guitar.setPickPosition(settings.pickPosition);
    guitar.setPickHardness(settings.pickHardness);
    guitar.setMuteOnNoteOff(settings.muteOnNoteOff);
    guitar.setVelocitySensitivity(settings.velocitySensitivity);
    guitar.setCoupling(settings.stringCoupling);
    guitar.setStiffness(settings.stiffness);
    guitar.setWidth(settings.stereoWidth);
    guitar.setPickupResonanceHz(settings.pickupResonanceHz);
    guitar.setPickupQ(settings.pickupQ);
    guitar.setPalmMuteDecaySeconds(settings.palmMuteDecaySeconds);
    guitar.setPalmMuteBrightness(settings.palmMuteBrightness);
}

void AudioEngine::setTrackGuitarTuning(int index, const std::array<int, kNumGuitarStrings>& tuning)
{
    if (index < 0 || index >= kMaxTracks)
        return;

    for (int s = 0; s < kNumGuitarStrings; ++s)
        tracks_[(size_t) index].guitar.setOpenNote(s, tuning[(size_t) s]);
}

void AudioEngine::setTrackDrumKit(int index, const std::vector<DrumPadSpec>& pads)
{
    if (index < 0 || index >= kMaxTracks)
        return;

    auto* map = new DrumPadMap();
    map->reserve(pads.size());

    for (const auto& pad : pads)
    {
        std::shared_ptr<ClipData> clip;
        if (pad.file != juce::File{})
            clip = decodeOrGetCached(pad.file); // nullptr on failure — the pad just stays silent

        DrumPadAssignment assignment;
        assignment.noteNumber = pad.noteNumber;
        assignment.clipData   = std::move(clip);
        assignment.gain       = juce::Decibels::decibelsToGain(pad.gainDb);
        assignment.pan        = juce::jlimit(-1.0f, 1.0f, pad.pan);
        assignment.pitchRatio = std::pow(2.0f, pad.pitchSemitones / 12.0f);
        assignment.muted      = pad.muted;
        map->push_back(std::move(assignment));
    }

    auto& track = tracks_[(size_t) index];
    track.drumKit.collectRetired();
    track.drumKit.setPadMap(map);
}

int64_t AudioEngine::countInLeadInSamples() const
{
    // The count-in is expressed in samples here, on the message thread, from
    // the tempo in force when recording starts — the audio thread only ever
    // counts it down (see AudioRecorder::process, MidiRecorder::process).
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

void AudioEngine::beginMidiRecording()
{
    // Grown on the message thread, before the audio thread can need it: the
    // capture translation in the callback must never allocate.
    midiCaptureScratch_.reserve(kMaxCapturedEventsPerBlock);
    midiRecorder_.arm(countInLeadInSamples());
}

void AudioEngine::setActiveTrackCount(int count)
{
    const int clamped = juce::jlimit(0, kMaxTracks, count);
    for (int i = 0; i < kMaxTracks; ++i)
        tracks_[(size_t) i].active.store(i < clamped, std::memory_order_relaxed);
}

void AudioEngine::setTrackClips(int index, const std::vector<ClipSlot>& clips)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].sequencer.submitClips(new std::vector<ClipSlot>(clips));
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

    bool anySolo = false;
    for (const auto& other : tracks_)
        anySolo |= other.solo.load(std::memory_order_relaxed);

    return ! anySolo || track.solo.load(std::memory_order_relaxed);
}

void AudioEngine::setTempoChanges(const std::vector<TempoChange>& changes)
{
    auto* copy = new TempoChangeList(changes);
    if (! tempoInbox_.push(copy))
        delete copy; // the queue is full; the next edit will carry the same map
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

void AudioEngine::setTrackSendLevel(int index, float level)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].sendLevel.store(level, std::memory_order_relaxed);
}

void AudioEngine::setTrackSessionSlots(int index, const std::vector<SessionSlotData>& slots)
{
    if (index < 0 || index >= kMaxTracks)
        return;

    auto& track = tracks_[(size_t) index];
    track.session.collectRetired();
    track.session.submitSlots(new SessionPlayer::SlotList(slots));
}

void AudioEngine::launchSessionSlot(int index, int sceneIndex)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].session.requestLaunch(sceneIndex);
}

void AudioEngine::stopSessionSlot(int index)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].session.requestStop();
}

void AudioEngine::launchScene(int sceneIndex)
{
    // Every track is told something, including the ones with nothing in this
    // scene: a scene says what the whole grid should be playing, so a track
    // with an empty slot falls silent rather than keeping its previous clip.
    for (auto& track : tracks_)
        track.session.requestLaunch(sceneIndex);
}

void AudioEngine::stopAllSessionSlots()
{
    for (auto& track : tracks_)
        track.session.requestStop();
}

void AudioEngine::setTrackAutomation(int index, const TrackAutomation& curves)
{
    if (index < 0 || index >= kMaxTracks)
        return;

    auto& track = tracks_[(size_t) index];
    track.collectRetiredAutomation();
    track.setAutomation(new TrackAutomation(curves));
}

void AudioEngine::setTrackSynthWaveform(int index, int waveform)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].synth.setWaveform(waveform);
}

void AudioEngine::setTrackSynthAttackMs(int index, float ms)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].synth.setAttackMs(ms);
}

void AudioEngine::setTrackSynthDecayMs(int index, float ms)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].synth.setDecayMs(ms);
}

void AudioEngine::setTrackSynthSustain(int index, float level)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].synth.setSustain(level);
}

void AudioEngine::setTrackSynthReleaseMs(int index, float ms)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].synth.setReleaseMs(ms);
}

void AudioEngine::setTrackSynthFilterEnabled(int index, bool enabled)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].synth.setFilterEnabled(enabled);
}

void AudioEngine::setTrackSynthFilterMode(int index, int mode)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].synth.setFilterMode(mode);
}

void AudioEngine::setTrackSynthFilterCutoff(int index, float hz)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].synth.setFilterCutoff(hz);
}

void AudioEngine::setTrackSynthFilterResonance(int index, float q)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].synth.setFilterResonance(q);
}

void AudioEngine::setTrackSynthGainDb(int index, float db)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].synth.setGainDb(db);
}

void AudioEngine::setTrackSynthFilterEnvAmount(int index, float hz)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].synth.setFilterEnvAmount(hz);
}

void AudioEngine::setTrackSynthFilterEnvAttackMs(int index, float ms)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].synth.setFilterEnvAttackMs(ms);
}

void AudioEngine::setTrackSynthFilterEnvDecayMs(int index, float ms)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].synth.setFilterEnvDecayMs(ms);
}

void AudioEngine::setTrackSynthFilterEnvSustain(int index, float level)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].synth.setFilterEnvSustain(level);
}

void AudioEngine::setTrackSynthFilterEnvReleaseMs(int index, float ms)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].synth.setFilterEnvReleaseMs(ms);
}

void AudioEngine::setTrackSynthSubOscEnabled(int index, bool enabled)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].synth.setSubOscEnabled(enabled);
}

void AudioEngine::setTrackSynthSubOscLevel(int index, float level)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].synth.setSubOscLevel(level);
}

void AudioEngine::setTrackSynthUnisonVoices(int index, int voices)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].synth.setUnisonVoices(voices);
}

void AudioEngine::setTrackSynthUnisonDetuneCents(int index, float cents)
{
    if (index >= 0 && index < kMaxTracks)
        tracks_[(size_t) index].synth.setUnisonDetuneCents(cents);
}

void AudioEngine::rebuildTrackEffectChain(int index)
{
    if (index < 0 || index >= kMaxTracks)
        return;

    const double rateForPlugins = sampleRate_.load(std::memory_order_relaxed);

    auto chain = std::make_unique<EffectChain>();
    for (const auto& spec : chainStructure_[(size_t) index])
    {
        switch (spec.kind)
        {
            case EffectNodeKind::Filter: chain->add(std::make_unique<FilterNode>()); break;
            case EffectNodeKind::Delay:  chain->add(std::make_unique<DelayNode>());  break;
            case EffectNodeKind::Reverb: chain->add(std::make_unique<ReverbNode>()); break;
            case EffectNodeKind::Drive:  chain->add(std::make_unique<DriveNode>());  break;
            case EffectNodeKind::Compressor: chain->add(std::make_unique<CompressorNode>()); break;
            case EffectNodeKind::Tremolo:    chain->add(std::make_unique<TremoloNode>());    break;
            case EffectNodeKind::Chorus:     chain->add(std::make_unique<ChorusNode>());     break;
            case EffectNodeKind::Wobble:     chain->add(std::make_unique<WobbleNode>());     break;
            case EffectNodeKind::Gate:       chain->add(std::make_unique<GateNode>());       break;
            case EffectNodeKind::Eq:         chain->add(std::make_unique<EqNode>());         break;

            case EffectNodeKind::Plugin:
            {
                // Instantiated here, on the message thread: loading a binary
                // and running third-party initialisation must never happen
                // under the audio thread. A plugin this machine doesn't have
                // simply leaves a gap in the chain rather than failing the
                // load — the document still remembers which one it wanted.
                std::string error;
                auto instance = pluginHost_.createInstance(spec.pluginFormat, spec.pluginIdentifier,
                                                           rateForPlugins > 0.0 ? rateForPlugins : 48000.0,
                                                           currentBlockSize_, &error);
                if (instance == nullptr)
                {
                    DBG("plugin unavailable: " << spec.pluginIdentifier.c_str() << " (" << error.c_str() << ")");
                    break;
                }

                auto node = std::make_unique<PluginNode>(std::move(instance));
                node->restoreState(spec.pluginState);
                chain->add(std::move(node));
                break;
            }
        }
    }

    // Prepared here, on the message thread, where allocating a delay line is
    // allowed. A rate of zero means the device hasn't started yet; the rebuild
    // in audioDeviceAboutToStart covers that case.
    const double rate = sampleRate_.load(std::memory_order_relaxed);
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
    const auto& existing = chainStructure_[(size_t) index];
    bool sameShape = existing.size() == slots.size() && submittedChain_[(size_t) index] != nullptr;
    for (size_t i = 0; sameShape && i < slots.size(); ++i)
        sameShape = existing[i].sameShapeAs(slots[i]);

    if (sameShape)
        return false;

    chainStructure_[(size_t) index] = slots;
    rebuildTrackEffectChain(index);
    return true;
}

void AudioEngine::setTrackEffectSlotParams(int index, int slotIndex, const EffectSlotParams& params)
{
    if (index < 0 || index >= kMaxTracks || slotIndex < 0)
        return;

    if (auto* chain = submittedChain_[(size_t) index])
        chain->applyParams((size_t) slotIndex, params);
}

PluginNode* AudioEngine::trackPluginNode(int index, int slotIndex)
{
    if (index < 0 || index >= kMaxTracks || slotIndex < 0)
        return nullptr;

    auto* chain = submittedChain_[(size_t) index];
    return chain != nullptr ? dynamic_cast<PluginNode*>(chain->nodeAt((size_t) slotIndex)) : nullptr;
}

void AudioEngine::setArmedTrack(int index)
{
    armedTrack_.store(juce::jlimit(0, kMaxTracks - 1, index), std::memory_order_relaxed);
}

void AudioEngine::pump() noexcept
{
    for (auto& track : tracks_)
    {
        track.sequencer.collectRetired();
        track.audioPlayer.collectRetiredClips();
        track.drumKit.collectRetired();
        track.collectRetiredAutomation();
        track.session.collectRetired();
        track.collectRetiredEffectChain();
    }

    filePlayer_.collectRetiredClips();

    TempoChangeList* retiredTempo = nullptr;
    while (tempoReclaim_.pop(retiredTempo))
        delete retiredTempo;
}

void AudioEngine::drainCommandQueue() noexcept
{
    // The tempo map, if a new one has arrived. Ahead of the commands below so
    // a tempo change and a SetTempo submitted together land in that order —
    // SetTempo edits beat 0 of whatever map is current.
    TempoChangeList* incomingTempo = nullptr;
    while (tempoInbox_.pop(incomingTempo))
    {
        if (incomingTempo != nullptr)
        {
            transport_.tempoMap().setTempoChanges(*incomingTempo);
            tempoReclaim_.push(incomingTempo);
        }
    }

    EngineCommand command;
    while (commandQueue_.pop(command))
    {
        switch (command.type)
        {
            case EngineCommand::Type::SetPlaying:      transport_.setPlaying(command.a != 0.0); break;
            case EngineCommand::Type::SetLooping:      transport_.setLooping(command.a != 0.0); break;
            case EngineCommand::Type::Seek:            transport_.seek((int64_t) command.a); break;
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

    incomingMidi_.clear();
    midiCollector_.removeNextBlockOfMessages(incomingMidi_, numSamples);
    keyboardState_.processNextMidiBuffer(incomingMidi_, 0, numSamples, true);

    ProcessContext context;
    context.sampleRate = sampleRate_.load(std::memory_order_relaxed);
    context.numSamples = numSamples;
    context.transport  = transport_.snapshot(context.numSamples);

    recorder_.process(inputChannelData, numInputChannels, numSamples,
                      context.transport.playing, context.transport.playheadSamples);

    // Before processBlock, so what is captured is exactly what the armed track
    // is about to play — the take and the monitoring can't disagree. Capturing
    // here rather than in handleIncomingMidiMessage is what makes the timing
    // sample-accurate: the collector has already placed each message at its
    // offset within this block, and on-screen keyboard input comes through the
    // same buffer, so it records too.
    captureMidi(incomingMidi_, context);

    processBlock(output, incomingMidi_, context);

    // Dry input monitoring, mixed in *after* the master bus for the same
    // reasons the metronome is: it stays out of the meter, out of the master
    // effects, and — because renderOffline only ever calls processBlock — it
    // can never end up in an exported file.
    mixInputMonitoring(output, inputChannelData, numInputChannels, numSamples);

    // After the master bus deliberately: the click bypasses the master
    // effects and gain, stays off the meter, and can never be exported (it
    // sits outside processBlock, which is what renderOffline renders).
    // `force` sounds it through a count-in even when it's otherwise off.
    metronome_.process(output, context, isCountingIn());
}

void AudioEngine::captureMidi(const juce::MidiBuffer& midi, const ProcessContext& context) noexcept
{
    const bool armed = midiRecorder_.isArmed();

    // No take armed and none still closing is the overwhelmingly common case,
    // and it must cost nothing. The second half of that condition is load-
    // bearing: process() is what publishes finished_ once a take is disarmed,
    // so returning on `! armed` alone would leave every take permanently
    // unfinished and the UI stuck mid-record.
    if (! armed && midiRecorder_.isFinished())
        return;

    midiCaptureScratch_.clear();

    // A disarmed-but-unfinished take still needs its process() call below, but
    // has nothing left to capture — so the buffer walk is what's skipped, not
    // the call.
    if (armed)
    {
        for (const auto metadata : midi)
        {
            const auto message = metadata.getMessage();

            // Notes only. Pitch bend, CC and aftertouch are real performance
            // data and worth recording one day, but a Pattern has nowhere to
            // put them — capturing them now would mean silently discarding
            // them later.
            if (! message.isNoteOnOrOff())
                continue;

            if (midiCaptureScratch_.size() >= kMaxCapturedEventsPerBlock)
                break; // never grown on this thread

            RecordedMidiEvent event;
            event.timeSamples = (int64_t) metadata.samplePosition;
            event.noteNumber  = message.getNoteNumber();
            event.velocity    = message.getFloatVelocity();
            // A note-on with velocity 0 is left as-is rather than normalised
            // here: MidiCapture treats it as a note-off, and translating it at
            // this layer would hide from the take what the controller
            // actually sent.
            event.noteOn      = message.isNoteOn();

            midiCaptureScratch_.push_back(event);
        }
    }

    midiRecorder_.process(midiCaptureScratch_.data(), (int) midiCaptureScratch_.size(),
                          context.numSamples, context.transport.playing,
                          context.transport.playheadSamples);
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

void AudioEngine::processBlock(juce::AudioBuffer<float>& output, juce::MidiBuffer& midi,
                               const ProcessContext& context,
                               int soloTrack, bool applyMasterBus) noexcept
{
    const int numSamples = context.numSamples;

    // A bus's solo is not counted: buses are never silenced by solo (see
    // InstrumentTrack::render), so letting one *arm* solo would mute every
    // real track while the bus that carries them stayed open — silence with
    // no obvious cause.
    bool anySolo = false;
    for (auto& track : tracks_)
        if (! track.isBus.load(std::memory_order_relaxed))
            anySolo |= track.solo.load(std::memory_order_relaxed);

    sendBus_.setSize(2, numSamples, false, false, true);

    // Kept at the block length for the same reason sendBus_ is: a detector
    // shorter than the block is ignored by the compressor, which would
    // silently turn ducking off on the first short block.
    if (silentDetector_.getNumSamples() < numSamples)
    {
        silentDetector_.setSize(2, numSamples, false, false, true);
        silentDetector_.clear();
    }
    sendBus_.clear();

    // Launch quantization is expressed in beats and converted here, once per
    // block, from the tempo actually in force.
    // From the block's own musical span rather than from a global tempo:
    // launch quantisation waits for the next N-beat boundary, and where that
    // falls depends on the tempo in force there.
    const double blockBeats           = context.transport.blockLengthBeats();
    const double samplesPerBeat       = blockBeats > 0.0
                                          ? (double) numSamples / blockBeats : 0.0;
    const double launchQuantumSamples = samplesPerBeat * launchQuantumBeats_.load(std::memory_order_relaxed);

    const int armed = armedTrack_.load(std::memory_order_relaxed);

    // Sidechain sources have to be rendered before the tracks that listen to
    // them, or the detector reads a buffer that hasn't been filled yet.
    //
    // The order is only rearranged when a sidechain actually exists. That is
    // not an optimisation: float addition isn't associative, so summing the
    // same tracks in a different order changes the mix in the last bits, and
    // a project with no sidechain must render bit-identically to how it always
    // has (the bounce tool's rmsDry sentinel would catch it, which is the
    // point).
    std::array<int, kMaxTracks> order {};
    int orderCount = 0;
    bool anySidechain = false;
    bool anyBus       = false;

    for (int i = 0; i < kMaxTracks; ++i)
    {
        if (sidechainSource_[(size_t) i].load(std::memory_order_relaxed) >= 0)
            anySidechain = true;
        if (tracks_[(size_t) i].isBus.load(std::memory_order_relaxed))
            anyBus = true;
    }

    // A bus receives what its members write, so its buffer has to be empty
    // before any of them render — the bus itself renders last and cannot clear
    // it without discarding the whole group.
    if (anyBus)
    {
        for (int i = 0; i < kMaxTracks; ++i)
            if (tracks_[(size_t) i].isBus.load(std::memory_order_relaxed)
                && tracks_[(size_t) i].active.load(std::memory_order_relaxed))
            {
                tracks_[(size_t) i].prepareBusInput(numSamples);
            }
    }

    if (! anySidechain && ! anyBus)
    {
        for (int i = 0; i < kMaxTracks; ++i)
            order[(size_t) orderCount++] = i;
    }
    else if (anyBus && ! anySidechain)
    {
        // Members first, buses last: a bus mixes what it was given, so
        // everything routed into it must already have run.
        for (int i = 0; i < kMaxTracks; ++i)
            if (! tracks_[(size_t) i].isBus.load(std::memory_order_relaxed))
                order[(size_t) orderCount++] = i;
        for (int i = 0; i < kMaxTracks; ++i)
            if (tracks_[(size_t) i].isBus.load(std::memory_order_relaxed))
                order[(size_t) orderCount++] = i;
    }
    else
    {
        // Sources first, then everyone else, each in index order. A full
        // topological sort would be the general answer, but with one detector
        // per track the only case it buys is a chain of sidechains (A ducks B
        // ducks C), and against that it would also have to define what a cycle
        // means. Two passes handle the case people actually build — a kick
        // ducking several tracks — and anything deeper simply reads the
        // silence-safe fallback below rather than misbehaving.
        std::array<bool, kMaxTracks> isSource {};
        for (int i = 0; i < kMaxTracks; ++i)
        {
            const int source = sidechainSource_[(size_t) i].load(std::memory_order_relaxed);
            if (source >= 0 && source < kMaxTracks)
                isSource[(size_t) source] = true;
        }

        auto isBusTrack = [this](int i)
        {
            return tracks_[(size_t) i].isBus.load(std::memory_order_relaxed);
        };

        // Sidechain sources, then ordinary tracks, then buses. Buses stay last
        // whatever else is true: a bus that rendered before its members would
        // mix an empty buffer, which is a silent group rather than a subtly
        // wrong one.
        for (int i = 0; i < kMaxTracks; ++i)
            if (isSource[(size_t) i] && ! isBusTrack(i))
                order[(size_t) orderCount++] = i;
        for (int i = 0; i < kMaxTracks; ++i)
            if (! isSource[(size_t) i] && ! isBusTrack(i))
                order[(size_t) orderCount++] = i;
        for (int i = 0; i < kMaxTracks; ++i)
            if (isBusTrack(i))
                order[(size_t) orderCount++] = i;
    }

    trackOutputs_.fill(nullptr);

    for (int slot = 0; slot < orderCount; ++slot)
    {
        const int i = order[(size_t) slot];

        // A stem renders one track. Note that anySolo is still whatever the
        // whole pool says, and the track still applies mute/solo itself — this
        // only decides who gets *asked*, so a stem is that track exactly as it
        // sounds in the mix rather than a special case of it.
        if (soloTrack >= 0 && i != soloTrack)
            continue;

        if (! tracks_[(size_t) i].active.load(std::memory_order_relaxed))
            continue;

        const juce::AudioBuffer<float>* detector = nullptr;
        const int source = sidechainSource_[(size_t) i].load(std::memory_order_relaxed);

        if (source >= 0 && source < kMaxTracks)
        {
            // Silence when the source produced nothing (muted, soloed out, or
            // not yet rendered): the honest reading, and the one that stops
            // ducking rather than falling back to self-compression, which
            // would change the sound of the track for a reason the user never
            // asked for.
            detector = trackOutputs_[(size_t) source] != nullptr
                         ? trackOutputs_[(size_t) source]
                         : &silentDetector_;
        }

        // Where this track's audio goes: its group bus if it has one and that
        // bus is really a bus and really active, otherwise straight to the
        // master. Checked here rather than trusted, because a stale routing
        // (to a track that has since stopped being a bus) must degrade to the
        // master rather than write into another instrument's scratch.
        juce::AudioBuffer<float>* destination = &output;

        if (! tracks_[(size_t) i].isBus.load(std::memory_order_relaxed))
        {
            const int busIndex = outputBus_[(size_t) i].load(std::memory_order_relaxed);

            if (busIndex >= 0 && busIndex < kMaxTracks
                && tracks_[(size_t) busIndex].isBus.load(std::memory_order_relaxed)
                && tracks_[(size_t) busIndex].active.load(std::memory_order_relaxed)
                && soloTrack < 0) // a stem renders one track in isolation, straight out
            {
                destination = &tracks_[(size_t) busIndex].busInput();
            }
        }

        tracks_[(size_t) i].render(*destination, sendBus_, midi, context, i == armed, anySolo,
                                   launchQuantumSamples, detector);

        trackOutputs_[(size_t) i] = tracks_[(size_t) i].blockOutput();
    }

    if (sendBusEnabled_.load(std::memory_order_relaxed))
    {
        if (sendBusEffectType_.load(std::memory_order_relaxed) == 1) // 1 = delay, 0 = reverb
            sendBusDelay_.process(sendBus_);
        else
            sendBusReverb_.process(sendBus_);

        const float returnGain = sendReturnGain_.load(std::memory_order_relaxed);
        const int   channels   = juce::jmin(output.getNumChannels(), sendBus_.getNumChannels());
        for (int ch = 0; ch < channels; ++ch)
            output.addFrom(ch, 0, sendBus_, ch, 0, numSamples, returnGain);
    }

    if (applyMasterBus)
    {
        // The file player and master ignore the MIDI buffer.
        filePlayer_.process(output, midi, context);
        masterFilter_.process(output);
        masterDelay_.process(output);
        masterReverb_.process(output);
        masterEq_.process(output);
        // The mastering rack sits between the master EQ and the output node, so
        // its limiter is the last thing to touch level before the meter reads it
        // — a ceiling that something after it could exceed wouldn't be one.
        mastering_.process(output);
        master_.process(output, midi, context);
    }

    transport_.advance(numSamples);
}

juce::AudioBuffer<float> AudioEngine::renderOffline(const OfflineRenderOptions& options)
{
    const double startBeats  = options.startBeats;
    const double lengthBeats = options.lengthBeats;

    const double deviceRate = sampleRate_.load(std::memory_order_relaxed);
    const double sampleRate = options.sampleRate > 0.0 ? options.sampleRate : deviceRate;
    const double bpm        = transport_.tempoMap().tempo();

    juce::AudioBuffer<float> output(2, 0);
    if (deviceRate <= 0.0 || sampleRate <= 0.0 || bpm <= 0.0 || lengthBeats <= 0.0)
        return output;

    const int blockSize = juce::jmax(1, options.blockSize);

    // Through the map: a render's length in samples is the distance between
    // two musical positions, not a beat count times one tempo.
    const auto&   tempoMap     = transport_.tempoMap();
    const int64_t startSample  = tempoMap.samplesFromPpq(startBeats);
    const int     totalSamples = (int) (tempoMap.samplesFromPpq(startBeats + lengthBeats) - startSample);
    if (totalSamples <= 0)
        return output;

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

    // Looping off for the duration: a loop region set for auditioning would
    // otherwise wrap the playhead mid-export and repeat a section.
    transport_.setLooping(false);
    transport_.seek(startSample);
    transport_.setPlaying(true);

    output.setSize(2, totalSamples);
    output.clear();

    juce::MidiBuffer noLiveMidi;

    // Every 16th block rather than every block: at 512 samples that is roughly
    // every 190ms, which is smooth enough for a progress bar while keeping a
    // std::function call out of the inner loop.
    constexpr int kBlocksPerProgressReport = 16;

    bool cancelled = false;
    int  blockIndex = 0;

    for (int pos = 0; pos < totalSamples; pos += blockSize, ++blockIndex)
    {
        const int n = juce::jmin(blockSize, totalSamples - pos);

        ProcessContext context;
        context.sampleRate = sampleRate;
        context.numSamples = n;
        context.transport  = transport_.snapshot(context.numSamples);

        juce::AudioBuffer<float> view(output.getArrayOfWritePointers(), 2, pos, n);
        noLiveMidi.clear();
        processBlock(view, noLiveMidi, context, options.soloTrack, options.applyMasterBus);

        if (options.onProgress != nullptr && blockIndex % kBlocksPerProgressReport == 0)
        {
            if (! options.onProgress((double) (pos + n) / (double) totalSamples))
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
    prepareAll(deviceRate, blockSize);

    // After that prepare, because wasPlayhead is a sample count at the device
    // rate and prepareAll is what puts the transport's tempo map back on that
    // rate. Restoring it first would place the playhead using the render's
    // clock.
    transport_.setPlaying(wasPlaying);
    transport_.setLooping(wasLooping);
    transport_.seek(wasPlayhead);

    deviceManager_.addAudioCallback(this);

    if (cancelled)
        output.setSize(2, 0); // see OfflineRenderOptions::onProgress

    return output;
}

void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    const double sampleRate = device->getCurrentSampleRate();
    const int    blockSize  = device->getCurrentBufferSizeSamples();

    sampleRate_.store(sampleRate, std::memory_order_relaxed);
    midiCollector_.reset(sampleRate);
    incomingMidi_.ensureSize(2048);

    prepareAll(sampleRate, blockSize);
}

void AudioEngine::prepareAll(double sampleRate, int blockSize)
{
    transport_.prepare(sampleRate);

    currentBlockSize_ = blockSize;

    for (auto& track : tracks_)
        track.prepare(sampleRate, blockSize);

    // A chain must be prepared for the rate it will actually run at, and one
    // submitted before the device started (or before a rate change) was
    // prepared for the wrong rate, or not at all. Rebuilding here guarantees
    // it; device starts are rare enough that the cost doesn't matter.
    for (int i = 0; i < kMaxTracks; ++i)
        rebuildTrackEffectChain(i);

    filePlayer_.prepare(sampleRate, blockSize);
    masterFilter_.prepare(sampleRate, blockSize);
    masterDelay_.prepare(sampleRate, blockSize);
    masterReverb_.prepare(sampleRate, blockSize);
    masterEq_.prepare(sampleRate, blockSize);
    mastering_.prepare(sampleRate, blockSize);
    master_.prepare(sampleRate, blockSize);

    sendBus_.setSize(2, blockSize);

    // Prepared here, never on the audio thread, and cleared once: nothing ever
    // writes to it, so it stays silent for the engine's lifetime.
    silentDetector_.setSize(2, juce::jmax(1, blockSize));
    silentDetector_.clear();
    sendBusReverb_.prepare(sampleRate, blockSize);
    sendBusReverb_.setEnabled(true); // always on internally; sendBusEnabled_ gates the mix-back
    sendBusReverb_.setMix(1.0f);     // a return bus is always fully wet

    sendBusDelay_.prepare(sampleRate, blockSize);
    sendBusDelay_.setEnabled(true); // same convention as sendBusReverb_ above
    sendBusDelay_.setMix(1.0f);     // a return bus is always fully wet

    recorder_.prepare(sampleRate, 2);
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

} // namespace looper::engine
