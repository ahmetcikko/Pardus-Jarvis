#include "whisper_wrapper.h"
#include <whisper.h>

static whisper_context *ctx = nullptr;

void whisper_init() {
    if (!ctx)
        ctx = whisper_init_from_file_with_params(
            "ggml-small-q5_1.bin", whisper_context_default_params());
}

std::string transcribe(const float *samples, int n_samples) {
    if (!ctx)
        return "";

    // below half a second at 16kHz there's rarely anything worth decoding
    if (n_samples < 8000)
        return "";
    whisper_full_params params =
        whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
    params.single_segment = true;
    params.no_context = true;
    params.no_timestamps = true;
    params.language = "tr";
    if (whisper_full(ctx, params, samples, n_samples) != 0)
        return "";
    std::string result;
    int n = whisper_full_n_segments(ctx);
    for (int i = 0; i < n; i++)
        result += whisper_full_get_segment_text(ctx, i);
    return result;
}
