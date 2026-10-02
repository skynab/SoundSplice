#pragma once

#include <functional>
#include <string>
#include <vector>

#include <juce_core/juce_core.h>

namespace soundsplice::engine
{
/**
    Local speech transcription with whisper.cpp: audio in, words out, each
    with when it starts and ends and how sure the model was. Nothing leaves
    the machine.

    The model is a ggml file the user downloaded (Preferences > Folders);
    larger ones are slower and more accurate, ".en" ones English-only and
    better at it. Implemented in Transcriber.cpp, the only file that sees
    whisper.h.
*/
struct TranscribedWord
{
    std::string text;
    double      start      = 0.0; // seconds into the audio given
    double      end        = 0.0;
    float       confidence = 1.0f; // 0-1, the model's own
};

struct Transcription
{
    bool                         ok = false;
    std::string                  error;
    std::vector<TranscribedWord> words;
    std::string                  language; // what it heard, or was told
};

/** Transcribes @p mono (one channel at @p sampleRate) with the model at
    @p model. @p language is a code ("en") or "auto". @p progress is told
    0-1 as it goes and returns false to stop, which leaves it not ok. */
Transcription transcribe(const juce::File& model, const std::vector<float>& mono, double sampleRate,
                         const std::string& language = "auto",
                         std::function<bool(double)> progress = {});

/** Where whisper.cpp's models come from, to open in a browser. */
inline constexpr const char* kModelsUrl = "https://huggingface.co/ggerganov/whisper.cpp/tree/main";

} // namespace soundsplice::engine
