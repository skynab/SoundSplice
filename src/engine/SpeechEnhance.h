#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include <rnnoise.h>

#include "engine/Resample.h"

namespace soundsplice::engine::speechenhance
{
/**
    AI speech enhancement: RNNoise (Xiph's recurrent-network noise
    suppressor, BSD-licensed, its weights built in) takes away whatever
    isn't voice - fans, traffic, keyboards, room noise that changes - where
    the classic noise reduction needs the noise to be steady.

    RNNoise works at 48 kHz on 480-sample frames of one channel, two frames
    late (the analysis window overlaps the frame before, and the output is
    overlap-added after it: measured, not assumed - see the test); this
    resamples to it and back, runs each channel through its own model
    state, and takes the delay off so the result lines up sample for
    sample with what went in. @p amount blends it with the
    original, 0 to 1: a little of the original back is often more natural
    than all of the suppression.

    Offline: it's an edit, not a live effect.
*/
inline constexpr double kRate = 48000.0;

/** @p input (one channel at @p sampleRate), enhanced. */
inline std::vector<float> enhance(const std::vector<float>& input, double sampleRate, float amount = 1.0f)
{
    if (input.empty() || sampleRate <= 0.0)
        return input;
    amount = std::clamp(amount, 0.0f, 1.0f);

    const bool resampled = std::abs(sampleRate - kRate) > 0.5;
    auto       at48      = resampled ? Resampler(sampleRate, kRate).processAll(input) : input;

    const int frame = rnnoise_get_frame_size();
    const auto length = at48.size();

    // Padding at the end to bring out what the delay holds back, and to
    // fill the last frame.
    const size_t delay = 2 * (size_t) frame;
    std::vector<float> in(length + delay + (size_t) (frame - (int) (length % (size_t) frame)) % (size_t) frame, 0.0f);
    for (size_t i = 0; i < length; ++i)
        in[i] = at48[i] * 32768.0f; // RNNoise works in 16-bit scale
    std::vector<float> out(in.size(), 0.0f);

    DenoiseState* state = rnnoise_create(nullptr);
    for (size_t at = 0; at + (size_t) frame <= in.size(); at += (size_t) frame)
        rnnoise_process_frame(state, out.data() + at, in.data() + at);
    rnnoise_destroy(state);

    // Late by the delay: lined back up.
    std::vector<float> enhanced(length);
    for (size_t i = 0; i < length; ++i)
        enhanced[i] = out[i + delay] / 32768.0f;

    if (resampled)
    {
        enhanced = Resampler(kRate, sampleRate).processAll(enhanced);
        enhanced.resize(input.size(), 0.0f);
    }

    for (size_t i = 0; i < input.size(); ++i)
        enhanced[i] = amount * enhanced[i] + (1.0f - amount) * input[i];
    return enhanced;
}

} // namespace soundsplice::engine::speechenhance
