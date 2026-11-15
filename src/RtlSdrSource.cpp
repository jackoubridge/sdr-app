#include "RtlSdrSource.h"

#include <algorithm>

std::vector<RtlSdrDeviceInfo> RtlSdrSource::listDevices() {
    std::vector<RtlSdrDeviceInfo> devices;
    uint32_t count = rtlsdr_get_device_count();
    for (uint32_t i = 0; i < count; ++i) {
        char vendor[256]{};
        char product[256]{};
        char serial[256]{};
        RtlSdrDeviceInfo info;
        info.index = static_cast<int>(i);
        if (rtlsdr_get_device_usb_strings(i, vendor, product, serial) == 0) {
            info.label = std::string(vendor) + " " + product + " (SN: " + serial + ")";
        } else {
            info.label = rtlsdr_get_device_name(i); // busy or no permission to read USB strings
        }
        devices.push_back(info);
    }
    return devices;
}

std::vector<int> RtlSdrSource::tunerGains(int index) {
    rtlsdr_dev_t* d = nullptr;
    if (rtlsdr_open(&d, index) < 0) {
        return {};
    }
    std::vector<int> gains(std::max(0, rtlsdr_get_tuner_gains(d, nullptr)));
    if (!gains.empty()) {
        rtlsdr_get_tuner_gains(d, gains.data());
    }
    rtlsdr_close(d);
    return gains;
}

bool RtlSdrSource::open(int index) {
    close();
    return rtlsdr_open(&dev, index) == 0;
}

void RtlSdrSource::close() {
    stop();
    if (dev) {
        rtlsdr_close(dev);
        dev = nullptr;
    }
}

bool RtlSdrSource::start(uint32_t sampleRate, SampleCallback cb) {
    if (!dev || thread.joinable()) {
        return false;
    }
    callback = std::move(cb);

    // Aim for about 50 callbacks a second so the display and audio update smoothly.
    // librtlsdr needs buffers that are a multiple of 16 KB.
    uint32_t bufLen = std::max<uint32_t>(16384, (2 * sampleRate / 50 + 16383) / 16384 * 16384);

    rtlsdr_reset_buffer(dev); // required before reading
    streaming = true;
    thread = std::thread([this, bufLen] {
        // Blocks until rtlsdr_cancel_async() is called or the device fails
        rtlsdr_read_async(dev, asyncCallback, this, 0, bufLen);
        streaming = false;
    });
    return true;
}

void RtlSdrSource::stop() {
    if (thread.joinable()) {
        rtlsdr_cancel_async(dev);
        thread.join();
    }
    streaming = false;
}

void RtlSdrSource::asyncCallback(unsigned char* buf, uint32_t len, void* ctx) {
    auto* self = static_cast<RtlSdrSource*>(ctx);
    self->callback(buf, static_cast<int>(len));
}

bool RtlSdrSource::setSampleRate(uint32_t hz) {
    return dev && rtlsdr_set_sample_rate(dev, hz) == 0;
}

bool RtlSdrSource::setCenterFreq(uint32_t hz) {
    return dev && rtlsdr_set_center_freq(dev, hz) == 0;
}

bool RtlSdrSource::setTunerBandwidth(uint32_t hz) {
    return dev && rtlsdr_set_tuner_bandwidth(dev, hz) == 0;
}

bool RtlSdrSource::setGain(bool automatic, int tenthsDb) {
    if (!dev || rtlsdr_set_tuner_gain_mode(dev, automatic ? 0 : 1) != 0) {
        return false;
    }
    return automatic || rtlsdr_set_tuner_gain(dev, tenthsDb) == 0;
}

bool RtlSdrSource::setRtlAgc(bool on) {
    return dev && rtlsdr_set_agc_mode(dev, on ? 1 : 0) == 0;
}

bool RtlSdrSource::setPpm(int ppm) {
    if (!dev) {
        return false;
    }
    int result = rtlsdr_set_freq_correction(dev, ppm);
    return result == 0 || result == -2; // -2 means it was already set to this value
}

bool RtlSdrSource::setBiasTee(bool on) {
    return dev && rtlsdr_set_bias_tee(dev, on ? 1 : 0) == 0;
}
