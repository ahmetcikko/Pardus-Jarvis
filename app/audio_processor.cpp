#include "audio_processor.h"
#include <webrtc-audio-processing-1/modules/audio_processing/include/audio_processing.h>

void process_audio(float *samples, int n_samples) {
    static webrtc::AudioProcessing *apm = []() {
        webrtc::AudioProcessing *instance =
            webrtc::AudioProcessingBuilder().Create();
        webrtc::AudioProcessing::Config cfg;
        cfg.noise_suppression.enabled = true;
        cfg.noise_suppression.level =
            webrtc::AudioProcessing::Config::NoiseSuppression::kLow;
        cfg.gain_controller2.enabled = true;
        cfg.gain_controller2.adaptive_digital.enabled = true;
        cfg.gain_controller2.fixed_digital.gain_db = 6.0f;
        instance->ApplyConfig(cfg);
        return instance;
    }();
    webrtc::StreamConfig stream_cfg(16000, 1);

    // webrtc's APM only accepts 10ms frames regardless of sample rate
    const int frame_size = stream_cfg.sample_rate_hz() / 100;
    apm->Initialize();
    for (int offset = 0; offset + frame_size <= n_samples;
         offset += frame_size) {
        float *ptr = samples + offset;
        apm->ProcessStream(&ptr, stream_cfg, stream_cfg, &ptr);
    }
}
