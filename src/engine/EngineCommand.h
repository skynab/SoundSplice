#pragma once

namespace looper::engine
{
/**
    A message from the UI thread to the audio thread. Trivially copyable so it can
    travel through a lock-free SpscRingBuffer. Two generic payload fields cover
    every command; each Type documents how they're interpreted.
*/
struct EngineCommand
{
    enum class Type
    {
        SetPlaying,     // a: 0/1
        SetLooping,     // a: 0/1
        Seek,           // a: sample position
        SetTempo,       // a: bpm
        SetLoopRegion,  // a: start sample, b: end sample
        SetMasterGainDb, // a: dB
        SetTimeSignature // a: numerator, b: denominator
    };

    Type   type {};
    double a = 0.0;
    double b = 0.0;
};

} // namespace looper::engine
