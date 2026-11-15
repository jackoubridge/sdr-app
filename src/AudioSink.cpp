#include "AudioSink.h"

#include "Dsp.h"

#include <algorithm>

namespace {

// Wait for this much audio before playing, so small timing jitter doesn't cause gaps
constexpr size_t kStartLevel = kAudioRate / 10; // 100 ms
// The SDR and sound card clocks drift slightly; if audio builds up beyond this, drop the excess
constexpr size_t kMaxLevel = kAudioRate / 2;    // 500 ms

} // namespace

bool AudioSink::start() {
    if (running) {
        return true;
    }

    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = 1;
    config.sampleRate = kAudioRate;
    config.dataCallback = dataCallback;
    config.pUserData = this;

    if (ma_device_init(nullptr, &config, &device) != MA_SUCCESS) {
        return false;
    }
    if (ma_device_start(&device) != MA_SUCCESS) {
        ma_device_uninit(&device);
        return false;
    }
    running = true;
    return true;
}

void AudioSink::stop() {
    if (!running) {
        return;
    }
    ma_device_uninit(&device);
    running = false;

    std::lock_guard lock(mutex);
    buffer.clear();
    primed = false;
}

void AudioSink::push(const float* samples, int count) {
    std::lock_guard lock(mutex);
    buffer.insert(buffer.end(), samples, samples + count);
    if (buffer.size() > kMaxLevel) {
        buffer.erase(buffer.begin(), buffer.end() - kStartLevel);
    }
}

void AudioSink::dataCallback(ma_device* device, void* output, const void*, ma_uint32 frames) {
    static_cast<AudioSink*>(device->pUserData)->fill(static_cast<float*>(output), static_cast<int>(frames));
}

void AudioSink::fill(float* out, int frames) {
    std::lock_guard lock(mutex);
    if (!primed && buffer.size() >= kStartLevel) {
        primed = true;
    }

    int available = primed ? static_cast<int>(std::min<size_t>(frames, buffer.size())) : 0;
    std::copy_n(buffer.begin(), available, out);
    buffer.erase(buffer.begin(), buffer.begin() + available);
    std::fill(out + available, out + frames, 0.0f);

    // Ran dry: wait to build the buffer back up before playing again
    if (available < frames) {
        primed = false;
    }
}
