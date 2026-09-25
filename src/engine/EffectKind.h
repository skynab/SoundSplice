#pragma once

namespace soundsplice::engine
{
/** What one slot of a track's effect chain is: a built-in or a hosted plugin.

    Defined here, below the model, so the engine's chain nodes and the
    document's slots share one list; model::EffectKind is this type. The
    numeric values go into the project file, so: append, never renumber. */
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
    Echo       = 24,
    Multiband  = 25,
    ParametricEq = 26, // see engine/ParametricEq.h
    Dynamics     = 27, // see engine/DynamicsProcessor.h
    GraphicEq31  = 28, // see engine/ThirdOctaveEq.h
    Convolution  = 29, // see engine/ConvolutionEffect.h
    Vocoder      = 30, // see engine/Vocoder.h
    ChannelMixer = 31  // see engine/ChannelMixer.h
};

} // namespace soundsplice::engine
