#pragma once

#include <miniaudio.h>

#include <deque>
#include <mutex>

// Plays mono float audio at kAudioRate through the default output device.
// The receiver pushes audio in from the SDR thread; the sound card pulls it out
// from its own thread, so a small buffer sits in between.
class AudioSink {
public:
    ~AudioSink() { stop(); }

    bool start();
    void stop();
    void push(const float* samples, int count);

private:
    static void dataCallback(ma_device* device, void* output, const void* input, ma_uint32 frames);
    void fill(float* out, int frames);

    ma_device device{};
    bool running = false;

    std::mutex mutex;
    std::deque<float> buffer;
    bool primed = false; // true once enough audio is buffered to start playing smoothly
};
