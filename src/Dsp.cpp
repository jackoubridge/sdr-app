#include "Dsp.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <numeric>

namespace {

constexpr int kMaxTapsPerPhase = 4096;
constexpr int kWfmIfRate = 240'000;
constexpr double kWfmDeviation = 75'000.0;
constexpr double kNfmDeviation = 5'000.0;
constexpr double kDeemphasisSeconds = 50e-6; // 50 us in UK/Europe, 75 us in the US

// Designs a windowed-sinc lowpass filter for a resampler that interpolates by `up`.
// The filter runs at inRate * up. Frequencies up to passbandHz are kept, and it is
// fully cutting by passbandHz + transitionHz; a narrower transition needs more taps.
std::vector<Ipp64f> designLowpass(int inRate, int up, double passbandHz, double transitionHz) {
    double edge = passbandHz + transitionHz / 2.0;

    // A Blackman window's transition width is about 5.5 * rate / taps
    int tapsPerPhase = std::clamp(static_cast<int>(std::ceil(5.5 * inRate / transitionHz)), 4, kMaxTapsPerPhase);
    int tapsLen = tapsPerPhase * up;
    double rFreq = std::min(edge / (static_cast<double>(inRate) * up), 0.49);

    std::vector<Ipp64f> taps(tapsLen);
    int bufSize = 0;
    ippsFIRGenGetBufferSize(tapsLen, &bufSize);
    IppBytes buf;
    buf.resize(bufSize);
    ippsFIRGenLowpass_64f(rFreq, taps.data(), tapsLen, ippWinBlackman, ippTrue, buf.data());

    // Interpolating inserts up-1 zeros between samples, so restore the lost gain
    ippsMulC_64f_I(up, taps.data(), tapsLen);
    return taps;
}

} // namespace

// ---- IqResampler ----

void IqResampler::init(int inRate, int outRate, double passbandHz, double transitionHz) {
    int divisor = std::gcd(inRate, outRate);
    up = outRate / divisor;
    down = inRate / divisor;

    std::vector<Ipp64f> taps64 = designLowpass(inRate, up, passbandHz, transitionHz);
    int tapsLen = static_cast<int>(taps64.size());
    std::vector<Ipp32f> tapsReal(tapsLen);
    std::vector<Ipp32fc> taps(tapsLen);
    ippsConvert_64f32f(taps64.data(), tapsReal.data(), tapsLen);
    ippsRealToCplx_32f(tapsReal.data(), nullptr, taps.data(), tapsLen);

    int specSize = 0;
    int workSize = 0;
    ippsFIRMRGetSize(tapsLen, up, down, ipp32fc, &specSize, &workSize);
    spec.resize(specSize);
    work.resize(workSize);
    ippsFIRMRInit_32fc(taps.data(), tapsLen, up, 0, down, 0, reinterpret_cast<IppsFIRSpec_32fc*>(spec.data()));

    int delayLen = (tapsLen + up - 1) / up;
    delayIn.assign(delayLen, {0, 0});
    delayOut.assign(delayLen, {0, 0});
    pending.clear();
}

void IqResampler::process(const Ipp32fc* in, int len, std::vector<Ipp32fc>& out) {
    // The filter consumes input in groups of `down` samples; keep any remainder for next time
    pending.insert(pending.end(), in, in + len);
    int groups = static_cast<int>(pending.size()) / down;
    if (groups == 0) {
        return;
    }

    size_t start = out.size();
    out.resize(start + groups * up);
    ippsFIRMR_32fc(pending.data(), out.data() + start, groups,
                   reinterpret_cast<IppsFIRSpec_32fc*>(spec.data()),
                   delayIn.data(), delayOut.data(), work.data());
    std::swap(delayIn, delayOut);
    pending.erase(pending.begin(), pending.begin() + groups * down);
}

// ---- AudioResampler ----

void AudioResampler::init(int inRate, int outRate, double passbandHz, double transitionHz) {
    int divisor = std::gcd(inRate, outRate);
    up = outRate / divisor;
    down = inRate / divisor;

    std::vector<Ipp64f> taps64 = designLowpass(inRate, up, passbandHz, transitionHz);
    int tapsLen = static_cast<int>(taps64.size());
    std::vector<Ipp32f> taps(tapsLen);
    ippsConvert_64f32f(taps64.data(), taps.data(), tapsLen);

    int specSize = 0;
    int workSize = 0;
    ippsFIRMRGetSize(tapsLen, up, down, ipp32f, &specSize, &workSize);
    spec.resize(specSize);
    work.resize(workSize);
    ippsFIRMRInit_32f(taps.data(), tapsLen, up, 0, down, 0, reinterpret_cast<IppsFIRSpec_32f*>(spec.data()));

    int delayLen = (tapsLen + up - 1) / up;
    delayIn.assign(delayLen, 0.0f);
    delayOut.assign(delayLen, 0.0f);
    pending.clear();
}

void AudioResampler::process(const Ipp32f* in, int len, std::vector<Ipp32f>& out) {
    pending.insert(pending.end(), in, in + len);
    int groups = static_cast<int>(pending.size()) / down;
    if (groups == 0) {
        return;
    }

    size_t start = out.size();
    out.resize(start + groups * up);
    ippsFIRMR_32f(pending.data(), out.data() + start, groups,
                  reinterpret_cast<IppsFIRSpec_32f*>(spec.data()),
                  delayIn.data(), delayOut.data(), work.data());
    std::swap(delayIn, delayOut);
    pending.erase(pending.begin(), pending.begin() + groups * down);
}

// ---- SpectrumAnalyzer ----

void SpectrumAnalyzer::init(int size, int sampleRate, int linesPerSecond) {
    fftSize = size;
    interval = std::max(size, sampleRate / linesPerSecond);
    skipRemaining = 0;

    int order = static_cast<int>(std::log2(size));
    int specSize = 0;
    int initSize = 0;
    int workSize = 0;
    ippsFFTGetSize_C_32fc(order, IPP_FFT_NODIV_BY_ANY, ippAlgHintFast, &specSize, &initSize, &workSize);
    fftSpec.resize(specSize);
    fftWork.resize(workSize);
    IppBytes initBuf;
    initBuf.resize(initSize);
    ippsFFTInit_C_32fc(&fft, order, IPP_FFT_NODIV_BY_ANY, ippAlgHintFast, fftSpec.data(), initBuf.data());

    collected.clear();
    collected.reserve(size);
    windowed.resize(size);
    power.resize(size);

    // The window reduces the signal's amplitude; measure by how much so that
    // a full-scale tone still reads 0 dBFS
    std::vector<Ipp32fc> ones(size, {1.0f, 0.0f});
    ippsWinBlackmanOpt_32fc_I(ones.data(), size);
    Ipp32fc sum;
    ippsSum_32fc(ones.data(), size, &sum, ippAlgHintNone);
    scale = 1.0f / (sum.re * sum.re);
}

void SpectrumAnalyzer::process(const Ipp32fc* in, int len, std::vector<std::vector<float>>& linesOut) {
    int pos = 0;
    while (pos < len) {
        // Skip samples between lines, so the line rate doesn't depend on the sample rate
        if (skipRemaining > 0) {
            int skip = std::min(skipRemaining, len - pos);
            pos += skip;
            skipRemaining -= skip;
            continue;
        }

        int take = std::min(fftSize - static_cast<int>(collected.size()), len - pos);
        collected.insert(collected.end(), in + pos, in + pos + take);
        pos += take;

        if (static_cast<int>(collected.size()) == fftSize) {
            linesOut.emplace_back();
            computeLine(linesOut.back());
            collected.clear();
            skipRemaining = interval - fftSize;
        }
    }
}

void SpectrumAnalyzer::computeLine(std::vector<float>& line) {
    ippsCopy_32fc(collected.data(), windowed.data(), fftSize);
    ippsWinBlackmanOpt_32fc_I(windowed.data(), fftSize);
    ippsFFTFwd_CToC_32fc_I(windowed.data(), fft, fftWork.data());

    // Power in dBFS: 10 * log10(power) = 10 / ln(10) * ln(power)
    ippsPowerSpectr_32fc(windowed.data(), power.data(), fftSize);
    ippsMulC_32f_I(scale, power.data(), fftSize);
    ippsThreshold_LT_32f_I(power.data(), fftSize, 1e-20f); // avoid log(0)
    ippsLn_32f_I(power.data(), fftSize);
    ippsMulC_32f_I(10.0f / std::numbers::ln10_v<float>, power.data(), fftSize);

    // The FFT puts 0 Hz first; swap the halves so the lowest frequency is first
    int half = fftSize / 2;
    line.resize(fftSize);
    ippsCopy_32f(power.data() + half, line.data(), half);
    ippsCopy_32f(power.data(), line.data() + half, half);
}

// ---- Demodulator ----

void Demodulator::configure(int rate, const DemodSettings& newSettings) {
    sampleRate = rate;
    settings = newSettings;
    ifRate = settings.mode == DemodMode::WFM ? kWfmIfRate : kAudioRate;

    // Stage 1: drop to the IF rate. This keeps the widest channel the mode allows (90% of
    // the IF rate) and removes everything that would otherwise alias back into it.
    int lowRate = std::min(ifRate, sampleRate);
    double maxPassband = 0.45 * lowRate;
    decimator.init(sampleRate, ifRate, maxPassband, lowRate - 2.0 * maxPassband);

    // Stage 2: the channel filter set by the bandwidth control. It runs at the low IF
    // rate, so it can afford a sharp edge, placed at exactly half the bandwidth.
    double bandwidth = std::clamp(settings.bandwidthHz, 1'000.0, 2.0 * maxPassband);
    double transition = std::max(0.1 * bandwidth, 500.0);
    channelFilter.init(ifRate, ifRate, (bandwidth - transition) / 2.0, transition);

    if (settings.mode == DemodMode::WFM) {
        // Mono audio is below 15 kHz; anything above 48 kHz - 15 kHz = 33 kHz would alias into it
        audioFilter.init(ifRate, kAudioRate, 15'000.0, 18'000.0);

        // First-order lowpass: y[n] = a * x[n] + (1 - a) * y[n-1]
        // IPP's tap layout is b0, b1, a0, a1
        float a = 1.0f - static_cast<float>(std::exp(-1.0 / (ifRate * kDeemphasisSeconds)));
        const Ipp32f taps[4] = {a, 0.0f, 1.0f, a - 1.0f};
        int stateSize = 0;
        ippsIIRGetStateSize_32f(1, &stateSize);
        deemphState.resize(stateSize);
        ippsIIRInit_32f(&deemph, taps, 1, nullptr, deemphState.data());
    }

    setOffset(settings.offsetHz);
    ncoPhase = 0.0f;
    lastSample = {0, 0};
    amCarrier = 0.0f;
    levelDb = -150.0f;
    squelchGain = settings.squelchEnabled ? 0.0f : 1.0f;
}

void Demodulator::setSquelch(bool enabled, float levelDbThreshold) {
    settings.squelchEnabled = enabled;
    settings.squelchLevelDb = levelDbThreshold;
}

void Demodulator::setOffset(double offsetHz) {
    settings.offsetHz = offsetHz;
    // Shifting by -offset brings the wanted signal to 0 Hz. IPP wants a frequency in [0, 1)
    double freq = -offsetHz / sampleRate;
    ncoFreq = static_cast<float>(freq - std::floor(freq));
    // A tiny positive offset gives a value just below 1 that rounds up to exactly 1.0f
    // as a float. IPP rejects 1.0, and the tone would then never be generated, so wrap it
    // to 0 (the same frequency).
    if (ncoFreq >= 1.0f) {
        ncoFreq = 0.0f;
    }
}

void Demodulator::process(const Ipp32fc* in, int len, std::vector<float>& audioOut) {
    // 1. Shift the wanted signal down to 0 Hz
    tone.resize(len);
    mixed.resize(len);
    ippsTone_32fc(tone.data(), len, 1.0f, ncoFreq, &ncoPhase, ippAlgHintAccurate);
    ippsMul_32fc(in, tone.data(), mixed.data(), len);

    // 2. Reduce the sample rate, then keep only this channel
    decimated.clear();
    decimator.process(mixed.data(), len, decimated);
    channel.clear();
    channelFilter.process(decimated.data(), static_cast<int>(decimated.size()), channel);
    if (channel.empty()) {
        return;
    }

    // Measure the channel's average power: sum(|x|^2) / n, in dBFS.
    // A full-scale carrier reads 0 dB, the same scale as the spectrum display.
    int n = static_cast<int>(channel.size());
    Ipp64f norm = 0.0;
    ippsNorm_L2_32fc64f(channel.data(), n, &norm);
    double power = norm * norm / n;
    levelDb = static_cast<float>(10.0 * std::log10(std::max(power, 1e-15)));

    // 3. Demodulate to audio
    size_t start = audioOut.size();
    switch (settings.mode) {
    case DemodMode::WFM:
        demodulateFm(kWfmDeviation);
        ippsIIR_32f_I(demod.data(), static_cast<int>(demod.size()), deemph);
        audioFilter.process(demod.data(), static_cast<int>(demod.size()), audioOut);
        break;
    case DemodMode::NFM:
        demodulateFm(kNfmDeviation);
        audioOut.insert(audioOut.end(), demod.begin(), demod.end());
        break;
    case DemodMode::AM:
        demodulateAm();
        audioOut.insert(audioOut.end(), demod.begin(), demod.end());
        break;
    }

    // 4. Volume, and clip to the valid audio range
    float* audio = audioOut.data() + start;
    int audioLen = static_cast<int>(audioOut.size() - start);
    ippsMulC_32f_I(settings.volume, audio, audioLen);
    ippsThreshold_LTValGTVal_32f_I(audio, audioLen, -1.0f, -1.0f, 1.0f, 1.0f);

    // 5. Squelch
    applySquelch(audio, audioLen);
}

void Demodulator::applySquelch(float* audio, int len) {
    if (len == 0) {
        return;
    }

    // Open when the channel is loud enough (or the squelch is off), closed otherwise.
    // The decision is made once per block, like SDR++.
    bool open = !settings.squelchEnabled || levelDb >= settings.squelchLevelDb;
    float target = open ? 1.0f : 0.0f;

    if (squelchGain == target) {
        // No change: leave the audio alone, or silence the whole block
        if (target == 0.0f) {
            ippsZero_32f(audio, len);
        }
        return;
    }

    // The squelch just opened or closed. Fade over this block rather than switching
    // instantly, which would make an audible click.
    for (int i = 0; i < len; ++i) {
        float gain = squelchGain + (target - squelchGain) * (i + 1) / len;
        audio[i] *= gain;
    }
    squelchGain = target;
}

void Demodulator::demodulateFm(double deviationHz) {
    // FM carries the audio as the rate of change of phase:
    // angle(x[n] * conj(x[n-1])) is the phase step between samples
    int n = static_cast<int>(channel.size());
    delayed.resize(n);
    demod.resize(n);
    delayed[0] = lastSample;
    ippsCopy_32fc(channel.data(), delayed.data() + 1, n - 1);
    lastSample = channel[n - 1];

    ippsConj_32fc_I(delayed.data(), n);
    ippsMul_32fc_I(channel.data(), delayed.data(), n);
    ippsPhase_32fc(delayed.data(), demod.data(), n);

    // Scale so that full deviation gives an amplitude of 1
    float gain = static_cast<float>(ifRate / (2.0 * std::numbers::pi * deviationHz));
    ippsMulC_32f_I(gain, demod.data(), n);
}

void Demodulator::demodulateAm() {
    // AM carries the audio in the signal's envelope (magnitude). The carrier
    // shows up as a constant level, which is removed and also used to normalise
    // the volume so weak and strong stations sound alike.
    int n = static_cast<int>(channel.size());
    demod.resize(n);
    ippsMagnitude_32fc(channel.data(), demod.data(), n);

    Ipp32f mean = 0.0f;
    ippsMean_32f(demod.data(), n, &mean, ippAlgHintFast);
    amCarrier = amCarrier == 0.0f ? mean : 0.9f * amCarrier + 0.1f * mean;
    if (amCarrier > 1e-6f) {
        ippsSubC_32f_I(amCarrier, demod.data(), n);
        ippsDivC_32f_I(amCarrier, demod.data(), n);
    }
}
