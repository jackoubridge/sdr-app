#pragma once

#include "AudioSink.h"
#include "Dsp.h"

#include <cstdint>
#include <mutex>
#include <vector>

// The processing chain between the SDR and the outputs:
// raw samples -> I/Q -> spectrum lines (for the display) and demodulated audio (to the speakers).
//
// processRaw() runs on the SDR's streaming thread. Settings changed from the GUI are
// stored and picked up at the start of the next block, so the two threads never
// touch the DSP state at the same time.
class Receiver {
public:
    explicit Receiver(AudioSink& audio) : audio(audio) {}

    // Call while not streaming, before the SDR starts
    void reset(int sampleRate);

    void processRaw(const uint8_t* data, int len);

    void setDemod(const DemodSettings& settings);
    void setFftSize(int size);

    // Moves the spectrum lines produced since the last call into `out` (oldest first)
    void takeSpectrumLines(std::vector<std::vector<float>>& out);

private:
    void applyPendingSettings();

    AudioSink& audio;
    int sampleRate = 0;

    std::mutex settingsMutex;
    DemodSettings wantedDemod;
    int wantedFftSize = 2048;
    bool settingsChanged = false;

    std::mutex linesMutex;
    std::vector<std::vector<float>> lines;

    // Only used on the streaming thread
    SpectrumAnalyzer spectrum;
    Demodulator demod;
    std::vector<Ipp32f> samples;
    std::vector<std::vector<float>> newLines;
    std::vector<float> audioBlock;
};
