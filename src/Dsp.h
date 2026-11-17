#pragma once

// Signal processing building blocks. All number crunching is done with
// Intel oneAPI IPP (Integrated Performance Primitives).

#include <ipp.h>

#include <vector>

// Memory from ippsMalloc, which is aligned for IPP's vectorised code.
class IppBytes {
public:
    IppBytes() = default;
    ~IppBytes() { ippsFree(data_); }
    IppBytes(const IppBytes&) = delete;
    IppBytes& operator=(const IppBytes&) = delete;

    void resize(int size) {
        ippsFree(data_);
        data_ = size > 0 ? ippsMalloc_8u(size) : nullptr;
    }
    Ipp8u* data() { return data_; }

private:
    Ipp8u* data_ = nullptr;
};

// Changes the sample rate of a complex (I/Q) stream by an exact ratio, with a lowpass
// filter that keeps frequencies below passbandHz and removes those above passbandHz + transitionHz.
// With inRate == outRate it is just a filter.
// Can be fed blocks of any length: leftover input and filter history are kept between calls.
class IqResampler {
public:
    void init(int inRate, int outRate, double passbandHz, double transitionHz);
    void process(const Ipp32fc* in, int len, std::vector<Ipp32fc>& out);

private:
    int up = 1;
    int down = 1;
    IppBytes spec;
    IppBytes work;
    std::vector<Ipp32fc> delayIn;
    std::vector<Ipp32fc> delayOut;
    std::vector<Ipp32fc> pending;
};

// Same as IqResampler but for a real (audio) stream.
class AudioResampler {
public:
    void init(int inRate, int outRate, double passbandHz, double transitionHz);
    void process(const Ipp32f* in, int len, std::vector<Ipp32f>& out);

private:
    int up = 1;
    int down = 1;
    IppBytes spec;
    IppBytes work;
    std::vector<Ipp32f> delayIn;
    std::vector<Ipp32f> delayOut;
    std::vector<Ipp32f> pending;
};

// Turns I/Q samples into spectrum lines (power in dBFS per FFT bin, lowest frequency first).
class SpectrumAnalyzer {
public:
    // A new line is produced roughly every sampleRate / linesPerSecond samples.
    void init(int fftSize, int sampleRate, int linesPerSecond);
    void process(const Ipp32fc* in, int len, std::vector<std::vector<float>>& linesOut);
    int size() const { return fftSize; }

private:
    void computeLine(std::vector<float>& line);

    int fftSize = 0;
    int interval = 0;      // samples between the start of one line and the next
    int skipRemaining = 0; // samples still to skip before collecting the next line
    float scale = 1.0f;    // makes a full-scale tone read 0 dBFS
    IppsFFTSpec_C_32fc* fft = nullptr;
    IppBytes fftSpec;
    IppBytes fftWork;
    std::vector<Ipp32fc> collected;
    std::vector<Ipp32fc> windowed;
    std::vector<Ipp32f> power;
};

enum class DemodMode { WFM, NFM, AM };

struct DemodSettings {
    DemodMode mode = DemodMode::WFM;
    double offsetHz = 0.0;           // VFO position relative to the centre frequency
    double bandwidthHz = 200'000.0;  // channel filter width
    float volume = 0.5f;

    // Squelch: mute the audio while the channel's power is below squelchLevelDb (dBFS)
    bool squelchEnabled = false;
    float squelchLevelDb = -50.0f;
};

constexpr int kAudioRate = 48'000;

// Tunes to one signal within the I/Q stream and demodulates it to audio at kAudioRate.
class Demodulator {
public:
    void configure(int sampleRate, const DemodSettings& settings);
    void setOffset(double offsetHz);
    void setVolume(float volume) { settings.volume = volume; }
    void setSquelch(bool enabled, float levelDb);
    const DemodSettings& current() const { return settings; }

    void process(const Ipp32fc* in, int len, std::vector<float>& audioOut);

    // Power in the channel during the last block, in dBFS (for the squelch and the meter)
    float channelLevelDb() const { return levelDb; }

private:
    void demodulateFm(double deviationHz);
    void demodulateAm();
    void applySquelch(float* audio, int len);

    int sampleRate = 0;
    int ifRate = 0; // rate of the channel after filtering
    DemodSettings settings;

    // Frequency shift (numerically controlled oscillator)
    float ncoFreq = 0.0f; // cycles per sample, in [0, 1)
    float ncoPhase = 0.0f;

    IqResampler decimator;      // sample rate -> IF rate
    IqResampler channelFilter;  // IF rate, cuts at the chosen bandwidth
    AudioResampler audioFilter; // WFM only: IF rate -> audio rate

    // WFM de-emphasis filter
    IppsIIRState_32f* deemph = nullptr;
    IppBytes deemphState;

    Ipp32fc lastSample{0, 0}; // for FM phase difference across blocks
    float amCarrier = 0.0f;   // smoothed AM carrier level

    float levelDb = -150.0f;  // channel power of the last block
    float squelchGain = 1.0f; // 1 = open (audio passes), 0 = closed (muted)

    std::vector<Ipp32fc> mixed;
    std::vector<Ipp32fc> tone;
    std::vector<Ipp32fc> decimated;
    std::vector<Ipp32fc> channel;
    std::vector<Ipp32fc> delayed;
    std::vector<Ipp32f> demod;
};
