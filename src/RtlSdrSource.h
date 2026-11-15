#pragma once

#include <rtl-sdr.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

struct RtlSdrDeviceInfo {
    int index = 0;
    std::string label; // e.g. "Nooelec NESDR SMART v5 (SN: 00000001)"
};

// Wraps one RTL-SDR dongle: opening it, changing its settings, and streaming samples.
class RtlSdrSource {
public:
    // Called from the streaming thread with raw interleaved 8-bit I/Q samples
    using SampleCallback = std::function<void(const uint8_t* data, int len)>;

    ~RtlSdrSource() { close(); }

    static std::vector<RtlSdrDeviceInfo> listDevices();
    // Supported tuner gains in tenths of a dB. Briefly opens the device if it isn't open.
    static std::vector<int> tunerGains(int index);

    bool open(int index);
    void close();
    bool isOpen() const { return dev != nullptr; }

    bool start(uint32_t sampleRate, SampleCallback callback);
    void stop();
    // False once streaming ends, including if the device was unplugged
    bool isStreaming() const { return streaming; }

    bool setSampleRate(uint32_t hz);
    bool setCenterFreq(uint32_t hz);
    bool setTunerBandwidth(uint32_t hz); // 0 = automatic
    bool setGain(bool automatic, int tenthsDb);
    bool setRtlAgc(bool on);
    bool setPpm(int ppm);
    bool setBiasTee(bool on);

private:
    static void asyncCallback(unsigned char* buf, uint32_t len, void* ctx);

    rtlsdr_dev_t* dev = nullptr;
    std::thread thread;
    std::atomic<bool> streaming = false;
    SampleCallback callback;
};
