#pragma once

#include <cmath>
#include <string>
#include <vector>

namespace soundsplice::model
{
/** Master delay settings, stored in the document (saved + undoable state). */
struct DelaySettings
{
    bool  enabled  = false;
    float timeMs   = 300.0f;
    float feedback = 0.35f; // 0..0.95
    float mix      = 0.30f; // 0..1

    bool operator==(const DelaySettings&) const = default;
};

/** Master filter settings. mode: 0 = low-pass, 1 = high-pass, 2 = band-pass. */
struct FilterSettings
{
    bool  enabled   = false;
    int   mode      = 0;
    float cutoff    = 1000.0f; // Hz
    float resonance = 0.707f;

    bool operator==(const FilterSettings&) const = default;
};

/** Master reverb settings. */
struct ReverbSettings
{
    bool  enabled  = false;
    float roomSize = 0.5f; // 0..1
    float damping  = 0.5f; // 0..1
    float mix      = 0.3f; // 0..1

    bool operator==(const ReverbSettings&) const = default;
};

/** Master 3-band EQ: fixed-frequency treble/mid/bass shelving+peak, the
    "regular mastering controls" a whole-song bus gets rather than the
    sweepable single-band FilterSettings above. Crossovers are fixed
    (bassHz/trebleHz below) rather than user-adjustable — three knobs, not a
    parametric EQ. */
struct EqSettings
{
    bool  enabled = false;
    float bassDb   = 0.0f; // -18..+18, low shelf below bassHz
    float midDb    = 0.0f; // -18..+18, peaking band between bassHz and trebleHz
    float trebleDb = 0.0f; // -18..+18, high shelf above trebleHz

    static constexpr float bassHz   = 250.0f;
    static constexpr float trebleHz = 4000.0f;

    /** The mid band's centre: geometric (not arithmetic) mean of the two
        crossovers, since frequency perception — and the octave-wide EQ bands
        either side of it — is logarithmic. Shared by EqEffect and EqCurve so
        a drawn curve can't drift from what the audio actually does. */
    static float midHz() { return std::sqrt(bassHz * trebleHz); }

    bool operator==(const EqSettings&) const = default;
};



/** Which plugin format an entry came from. The numeric values go into the
    project file, so: append, never renumber. */
enum class PluginFormat
{
    Unknown   = 0,
    VST3      = 1,
    AudioUnit = 2
};

/**
    A reference to a hosted plugin, as the *document* sees it.

    Deliberately JUCE-free rather than a juce::PluginDescription. Three
    reasons, in order of weight: the test target links Catch2 only, so anything
    JUCE-typed can't be covered headlessly and this is exactly the
    serialization-shaped problem that needs tests; a project must survive a
    plugin being missing, which means storing enough to *name* the one it
    wanted rather than silently dropping it; and PluginDescription is an
    engine/UI concern, converted at the boundary the same way AutomationCurve
    and AutomationLane already are.

    `state` is the plugin's own opaque blob, base64'd. It is not this format's
    business to understand it.
*/
struct PluginRef
{
    PluginFormat format = PluginFormat::Unknown;
    std::string  identifier; // what JUCE needs to find it again
    std::string  name;       // for display, and to say which plugin is missing
    std::string  state;      // base64 of the plugin's own state

    bool operator==(const PluginRef&) const = default;
};

/** What one slot of a track's effect chain is. */
/** The mastering rack on the master bus: the last thing the mix passes
    through before it leaves the app.

    A fixed set of stages in a fixed order (see engine::MasteringProcessor),
    not a reorderable chain — a mastering chain has a canonical order for real
    reasons (shape the tone, then add harmonics to what you shaped, then set
    the width, then the space, then catch the peaks last), and every stage
    here is a no-op at its default so an untouched rack is bit-identical to
    no rack at all. */
struct MasteringSettings
{
    bool enabled = false;

    // Three-band EQ, the shape of Audition's mastering EQ: a shelf at each
    // end and one peaking band between them. Frequencies are adjustable here
    // (unlike the fixed master EqSettings above), because deciding *where*
    // the mud or the harshness is is most of the job.
    float lowShelfHz    = 120.0f;
    float lowShelfDb    = 0.0f;
    float peakHz        = 1000.0f;
    float peakDb        = 0.0f;
    float peakQ         = 0.9f;
    float highShelfHz   = 8000.0f;
    float highShelfDb   = 0.0f;

    float exciterAmount     = 0.0f;    // 0..1
    float exciterCrossoverHz = 3000.0f;

    float width = 1.0f; // 1 = unchanged, 0 = mono, >1 = wider

    float reverbAmount   = 0.0f; // 0..1 wet mix
    float reverbRoomSize = 0.6f;

    float maximizerInputDb  = 0.0f;   // drive into the limiter: this is "louder"
    float maximizerCeilingDb = -0.3f; // nothing gets past this
    float maximizerReleaseMs = 100.0f;

    float outputGainDb = 0.0f;

    bool operator==(const MasteringSettings&) const = default;
};

enum class EffectKind
{
    Filter = 0,
    Delay  = 1,
    Reverb = 2,
    Plugin = 3,
    Drive      = 4, // see engine::DriveEffect and PedalEffects.h
    Compressor = 5,
    Tremolo    = 6,
    Chorus     = 7,
    Wobble     = 8,
    Gate       = 9,
    Eq         = 10,
    Amplify    = 11, // see engine/UtilityEffects.h
    Invert     = 12,
    DcOffset   = 13,
    Limiter    = 14,
    Phaser     = 15, // see engine/ToneEffects.h
    Flanger    = 16,
    BassTreble = 17,
    StereoTool = 18,
    GraphicEq  = 19, // see engine/DynamicsEffects.h
    DeEsser    = 20,
    Expander   = 21,
    RingMod    = 22,
    Wah        = 23,
    Echo       = 24
};

/**
    One effect in a track's chain: a built-in or a hosted plugin.

    Every built-in's settings are stored regardless of which kind the slot
    currently is, so switching kind doesn't lose the others — a slightly fat
    struct in exchange for no lost state.

    `enabled` belongs to the slot rather than to the settings structs, so
    bypass means the same thing for a plugin as for a built-in. The
    per-settings `enabled` flags stay for the master bus, whose effects are
    single fixed effects rather than chain slots.
*/
/** An overdrive/distortion pedal. `hardClip` picks a fuzz's flat ceiling over
    an overdrive's gradual compression; `cabinet` is on by default because
    drive without a speaker sim is heard as fizz rather than distortion. */
struct DriveSettings
{
    bool  enabled  = false;
    float drive    = 4.0f;  // how hard the signal is pushed into the shaper
    float tone     = 0.5f;  // 0..1, dark to bright, after the clipping
    float level    = 0.7f;  // make-up gain
    bool  hardClip = false;
    bool  cabinet  = true;

    // A DC bias into the shaper, so the two halves of the waveform clip
    // differently and even harmonics appear - see engine::Waveshaper. 0 is the
    // symmetric curve this had before; a real tube stage is never symmetric.
    float asymmetry = 0.0f;

    /** How many gain stages the signal passes through, 1..3 — see
        engine::DriveEffect::setStages. 1 is a pedal (one clipper) and is what
        this did before the field existed; 2-3 is an amp, and the difference is
        a different kind of distortion rather than more of it. */
    int stages = 1;

    /** Convolve a synthesised cabinet impulse response instead of running the
        cabinet's filter chain — see engine/CabinetIr.h. Voiced identically by
        construction; what it adds is the time-domain structure a filter cannot
        express. Off by default, so nothing that has not asked for it changes. */
    bool cabinetIr = false;

    // Runs the shaper at 4x - see engine::Oversampler4x. Off by default,
    // because it costs real CPU and the ADAA shaper alone is enough for a
    // single moderate stage. The high-gain presets turn it on, since they
    // cascade one clipper into another and fold the first one's output twice.
    bool  oversample = false;

    bool operator==(const DriveSettings&) const = default;
};

/** A three-band EQ pedal: low shelf, a **sweepable** mid bell, high shelf.

    The mid is sweepable because "the mids" is 400Hz on one guitar and 1.2kHz
    on another. Before this there was no way to boost or cut a band's gain on
    a track at all — EqSettings is master-bus only, and a filter slot picks a
    cutoff, which is a different thing — so the mid scoop/push that decides a
    rock or metal tone was unreachable by construction.

    Every default is flat, so adding the pedal changes nothing until it's
    dialled. See engine::EqPedalEffect. */
struct EqPedalSettings
{
    bool  enabled = false;

    float lowShelfHz  = 100.0f;
    float lowShelfDb  = 0.0f;
    float midHz       = 800.0f;
    float midDb       = 0.0f;
    float midQ        = 1.0f;
    float highShelfHz = 4000.0f;
    float highShelfDb = 0.0f;

    bool operator==(const EqPedalSettings&) const = default;
};

/** A compressor pedal. Threshold and ratio are the shape; attack and release
    are the feel, and they mean what they say — see engine::Compressor. */
struct CompressorSettings
{
    bool  enabled     = false;
    float thresholdDb = -18.0f;
    float ratio       = 4.0f;
    float attackMs    = 10.0f;
    float releaseMs   = 120.0f;
    float makeUpDb    = 0.0f;

    bool operator==(const CompressorSettings&) const = default;
};

/** A tremolo pedal. `depth` is how far the quiet part drops, so zero is off. */
struct TremoloSettings
{
    bool  enabled = false;
    float rateHz  = 5.0f;
    float depth   = 0.5f;

    bool operator==(const TremoloSettings&) const = default;
};

/** A chorus pedal. `depth` is how far the delay sweeps, `mix` how much of the
    swept copies is heard — at zero depth it's a fixed comb rather than a
    chorus, which is a usable tone and not a broken one. */
struct ChorusSettings
{
    bool  enabled = false;
    float rateHz  = 0.6f;
    float depth   = 0.5f;
    float mix     = 0.5f;

    bool operator==(const ChorusSettings&) const = default;
};

/** A wobble pedal: a resonant low-pass swept by an LFO locked to the song's
    tempo, in beats rather than Hz — dubstep's "wub wub." `rateBeats` is how
    many beats one full sweep takes (0.25/0.5/1.0/2.0 for a sixteenth, an
    eighth, a quarter, a half note); `depth` is how far the sweep opens above
    `baseCutoffHz`, so depth zero leaves a static low-pass rather than muting
    anything. See engine::Wobble and PedalDsp.h. */
struct WobbleSettings
{
    bool  enabled      = false;
    float rateBeats    = 0.25f;
    float depth        = 0.7f;
    float baseCutoffHz = 200.0f;
    float resonance    = 0.9f;
    float mix          = 1.0f;

    bool operator==(const WobbleSettings&) const = default;
};

/** A noise gate. Attenuates *below* thresholdDb rather than above it (the
    mirror of CompressorSettings), down to rangeDb rather than by a ratio —
    closed means "silent," not "quieter." holdMs is what stops a decaying
    note from chattering the gate open and closed instead of closing once,
    cleanly, when the note is actually done. See engine::Gate. */
struct GateSettings
{
    bool  enabled     = false;
    float thresholdDb = -40.0f;
    float rangeDb     = 60.0f;
    float attackMs    = 2.0f;
    float holdMs      = 20.0f;
    float releaseMs   = 150.0f;

    bool operator==(const GateSettings&) const = default;
};

/** Gain, as Audacity's Amplify. */
struct AmplifySettings
{
    bool  enabled = false;
    float gainDb  = 0.0f;

    bool operator==(const AmplifySettings&) const = default;
};

/** Polarity inversion, per channel. */
struct InvertSettings
{
    bool enabled = false;
    bool left    = true;
    bool right   = true;

    bool operator==(const InvertSettings&) const = default;
};

/** DC offset removal: a high-pass a few hertz up. */
struct DcOffsetSettings
{
    bool  enabled  = false;
    float cutoffHz = 5.0f;

    bool operator==(const DcOffsetSettings&) const = default;
};

/** A lookahead brickwall limiter, the mastering Maximizer as a chain effect. */
struct LimiterSettings
{
    bool  enabled     = false;
    float inputGainDb = 0.0f;
    float ceilingDb   = -1.0f;
    float releaseMs   = 100.0f;

    bool operator==(const LimiterSettings&) const = default;
};

/** A phaser: allpass stages swept by an LFO. `stagePairs` is half the
    number of stages, 1..6, so every setting makes whole notches. */
struct PhaserSettings
{
    bool  enabled    = false;
    float rateHz     = 0.5f;
    float depth      = 0.7f;
    float feedback   = 0.5f;
    int   stagePairs = 3;
    float mix        = 0.5f;

    bool operator==(const PhaserSettings&) const = default;
};

/** A flanger: a short swept delay mixed back in. */
struct FlangerSettings
{
    bool  enabled  = false;
    float rateHz   = 0.25f;
    float depth    = 0.7f;
    float delayMs  = 1.0f;
    float feedback = 0.5f;
    float mix      = 0.5f;

    bool operator==(const FlangerSettings&) const = default;
};

/** Bass and treble shelves and an output volume, as Audacity's. */
struct BassTrebleSettings
{
    bool  enabled  = false;
    float bassDb   = 0.0f;
    float trebleDb = 0.0f;
    float volumeDb = 0.0f;

    bool operator==(const BassTrebleSettings&) const = default;
};

/** Width, balance, mono and swap for a stereo pair. */
struct StereoToolSettings
{
    bool  enabled = false;
    float width   = 1.0f;
    float balance = 0.0f;
    bool  mono    = false;
    bool  swap    = false;

    bool operator==(const StereoToolSettings&) const = default;
};

/** Ten octave bands, 31 Hz to 16 kHz, each -12..+12 dB. */
struct GraphicEqSettings
{
    bool  enabled = false;
    float band31 = 0.0f;
    float band62 = 0.0f;
    float band125 = 0.0f;
    float band250 = 0.0f;
    float band500 = 0.0f;
    float band1k = 0.0f;
    float band2k = 0.0f;
    float band4k = 0.0f;
    float band8k = 0.0f;
    float band16k = 0.0f;

    bool operator==(const GraphicEqSettings&) const = default;
};

/** Turns the audio above a frequency down while it's over a threshold. */
struct DeEsserSettings
{
    bool  enabled        = false;
    float frequencyHz    = 5500.0f;
    float thresholdDb    = -30.0f;
    float maxReductionDb = 12.0f;

    bool operator==(const DeEsserSettings&) const = default;
};

/** A downward expander. */
struct ExpanderSettings
{
    bool  enabled     = false;
    float thresholdDb = -40.0f;
    float ratio       = 2.0f;
    float rangeDb     = 40.0f;
    float attackMs    = 5.0f;
    float releaseMs   = 100.0f;

    bool operator==(const ExpanderSettings&) const = default;
};

struct RingModSettings
{
    bool  enabled     = false;
    float frequencyHz = 440.0f;
    float mix         = 1.0f;

    bool operator==(const RingModSettings&) const = default;
};

/** An auto-wah. */
struct WahSettings
{
    bool  enabled   = false;
    float rateHz    = 1.5f;
    float depth     = 0.8f;
    float resonance = 4.0f;
    float mix       = 1.0f;

    bool operator==(const WahSettings&) const = default;
};

/** A multitap echo: several repeats at a fixed spacing, each quieter. */
struct EchoSettings
{
    bool  enabled  = false;
    float timeMs   = 250.0f;
    int   taps     = 3;
    float decay    = 0.5f;
    float mix      = 0.35f;
    bool  pingPong = false;

    bool operator==(const EchoSettings&) const = default;
};

struct EffectSlot
{
    EffectKind kind    = EffectKind::Filter;
    bool       enabled = false;

    FilterSettings filter;
    DelaySettings  delay;
    ReverbSettings reverb;
    DriveSettings      drive;
    CompressorSettings compressor;
    TremoloSettings    tremolo;
    ChorusSettings     chorus;
    WobbleSettings     wobble;
    GateSettings       gate;
    EqPedalSettings    eqPedal;
    AmplifySettings    amplify;
    InvertSettings     invert;
    DcOffsetSettings   dcOffset;
    LimiterSettings    limiter;
    PhaserSettings     phaser;
    FlangerSettings    flanger;
    BassTrebleSettings bassTreble;
    StereoToolSettings stereoTool;
    GraphicEqSettings  graphicEq;
    DeEsserSettings    deEsser;
    ExpanderSettings   expander;
    RingModSettings    ringMod;
    WahSettings        wah;
    EchoSettings       echo;
    PluginRef          plugin; // meaningful when kind == Plugin

    bool operator==(const EffectSlot&) const = default;
};

} // namespace soundsplice::model
