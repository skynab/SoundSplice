#include "engine/Transcriber.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <thread>

#include <whisper.h>

#include "engine/Resample.h"

namespace soundsplice::engine
{
namespace
{
    struct Callbacks
    {
        std::function<bool(double)> progress;
        std::atomic<bool>           stopped { false };
    };

    void onProgress(whisper_context*, whisper_state*, int percent, void* user)
    {
        auto* callbacks = static_cast<Callbacks*>(user);
        if (callbacks->progress && ! callbacks->progress(percent / 100.0))
            callbacks->stopped = true;
    }

    bool shouldAbort(void* user)
    {
        return static_cast<Callbacks*>(user)->stopped.load();
    }

    /** whisper.cpp logs to stderr unasked; the app has its own messages. */
    void quiet(ggml_log_level, const char*, void*) {}

    std::string trimmed(const std::string& text)
    {
        const auto first = text.find_first_not_of(" \t\r\n");
        const auto last  = text.find_last_not_of(" \t\r\n");
        return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
    }

    /** "[BLANK_AUDIO]", "(music)" and the like: whisper's notes, not words. */
    bool isAnnotation(const std::string& word)
    {
        return word.size() >= 2 && ((word.front() == '[' && word.back() == ']') || (word.front() == '(' && word.back() == ')'));
    }
}

Transcription transcribe(const juce::File& model, const std::vector<float>& mono, double sampleRate, const std::string& language,
                         std::function<bool(double)> progress)
{
    Transcription result;
    if (! model.existsAsFile())
    {
        result.error = "No transcription model at " + model.getFullPathName().toStdString()
                     + " - choose one in Preferences > Folders";
        return result;
    }
    if (mono.empty() || sampleRate <= 0.0)
    {
        result.error = "Nothing to transcribe";
        return result;
    }

    whisper_log_set(quiet, nullptr);

    auto contextParams    = whisper_context_default_params();
    contextParams.use_gpu = false;
    auto* context = whisper_init_from_file_with_params(model.getFullPathName().toRawUTF8(), contextParams);
    if (context == nullptr)
    {
        result.error = model.getFileName().toStdString() + " isn't a whisper.cpp model that this version can read";
        return result;
    }

    // Whisper hears at 16 kHz.
    const auto pcm = std::abs(sampleRate - WHISPER_SAMPLE_RATE) > 0.5 ? Resampler(sampleRate, WHISPER_SAMPLE_RATE).processAll(mono)
                                                                      : mono;

    Callbacks callbacks;
    callbacks.progress = std::move(progress);

    auto params                         = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    params.n_threads                    = (int) std::max(1u, std::thread::hardware_concurrency() - 1);
    params.language                     = language.empty() ? "auto" : language.c_str();
    params.detect_language              = false;
    params.print_progress               = false;
    params.print_realtime               = false;
    params.print_special                = false;
    params.print_timestamps             = false;
    // A word to a segment, each with its own times.
    params.token_timestamps             = true;
    params.max_len                      = 1;
    params.split_on_word                = true;
    params.progress_callback            = onProgress;
    params.progress_callback_user_data  = &callbacks;
    params.abort_callback               = shouldAbort;
    params.abort_callback_user_data     = &callbacks;

    const int status = whisper_full(context, params, pcm.data(), (int) pcm.size());
    if (status != 0 || callbacks.stopped)
    {
        result.error = callbacks.stopped ? "Stopped" : "The transcription failed";
        whisper_free(context);
        return result;
    }

    const auto eot = whisper_token_eot(context);
    const int  segments = whisper_full_n_segments(context);
    for (int i = 0; i < segments; ++i)
    {
        TranscribedWord word;
        word.text = trimmed(whisper_full_get_segment_text(context, i));
        if (word.text.empty() || isAnnotation(word.text))
            continue;
        // Centiseconds.
        word.start = whisper_full_get_segment_t0(context, i) / 100.0;
        word.end   = std::max(word.start, whisper_full_get_segment_t1(context, i) / 100.0);

        float sum = 0.0f;
        int   n   = 0;
        for (int t = 0; t < whisper_full_n_tokens(context, i); ++t)
            if (whisper_full_get_token_id(context, i, t) < eot) // text tokens only
            {
                sum += whisper_full_get_token_p(context, i, t);
                ++n;
            }
        word.confidence = n > 0 ? sum / (float) n : 1.0f;
        result.words.push_back(std::move(word));
    }

    const int languageId = whisper_full_lang_id(context);
    result.language      = languageId >= 0 ? whisper_lang_str(languageId) : language;
    result.ok            = true;
    whisper_free(context);
    return result;
}

} // namespace soundsplice::engine
