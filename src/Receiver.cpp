#include "Receiver.h"

namespace {

constexpr int kSpectrumLinesPerSecond = 30;
// If the GUI stalls, keep only this many lines rather than growing forever
constexpr size_t kMaxQueuedLines = 64;

} // namespace

void Receiver::reset(int rate) {
    std::lock_guard lock(settingsMutex);
    sampleRate = rate;
    spectrum.init(wantedFftSize, sampleRate, kSpectrumLinesPerSecond);
    demod.configure(sampleRate, wantedDemod);
    settingsChanged = false;
}

void Receiver::setDemod(const DemodSettings& settings) {
    std::lock_guard lock(settingsMutex);
    wantedDemod = settings;
    settingsChanged = true;
}

void Receiver::setFftSize(int size) {
    std::lock_guard lock(settingsMutex);
    wantedFftSize = size;
    settingsChanged = true;
}

void Receiver::takeSpectrumLines(std::vector<std::vector<float>>& out) {
    std::lock_guard lock(linesMutex);
    out = std::move(lines);
    lines.clear();
}

void Receiver::applyPendingSettings() {
    DemodSettings settings;
    int fftSize = 0;
    {
        std::lock_guard lock(settingsMutex);
        if (!settingsChanged) {
            return;
        }
        settings = wantedDemod;
        fftSize = wantedFftSize;
        settingsChanged = false;
    }

    if (fftSize != spectrum.size()) {
        spectrum.init(fftSize, sampleRate, kSpectrumLinesPerSecond);
    }

    // Rebuilding the filters causes a tiny audio glitch, so only do it when needed
    const DemodSettings& current = demod.current();
    if (settings.mode != current.mode || settings.bandwidthHz != current.bandwidthHz) {
        demod.configure(sampleRate, settings);
    } else {
        demod.setOffset(settings.offsetHz);
        demod.setVolume(settings.volume);
    }
}

void Receiver::processRaw(const uint8_t* data, int len) {
    applyPendingSettings();

    // Unsigned bytes centred on 127.5 -> floats in [-1, 1]. Interleaved I, Q floats
    // have the same memory layout as IPP's complex type.
    samples.resize(len);
    ippsConvert_8u32f(data, samples.data(), len);
    ippsSubC_32f_I(127.5f, samples.data(), len);
    ippsMulC_32f_I(1.0f / 127.5f, samples.data(), len);
    const auto* iq = reinterpret_cast<const Ipp32fc*>(samples.data());
    int count = len / 2;

    newLines.clear();
    spectrum.process(iq, count, newLines);
    if (!newLines.empty()) {
        std::lock_guard lock(linesMutex);
        for (auto& line : newLines) {
            lines.push_back(std::move(line));
        }
        if (lines.size() > kMaxQueuedLines) {
            lines.erase(lines.begin(), lines.end() - kMaxQueuedLines);
        }
    }

    audioBlock.clear();
    demod.process(iq, count, audioBlock);
    audio.push(audioBlock.data(), static_cast<int>(audioBlock.size()));
}
