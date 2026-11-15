#pragma once

#include "AudioSink.h"
#include "Dsp.h"
#include "Receiver.h"
#include "RtlSdrSource.h"
#include "Waterfall.h"

#include <imgui.h>

#include <string>
#include <vector>

// The whole user interface, plus starting and stopping the radio.
class App {
public:
    App();
    ~App();

    // Draws the UI; call once per frame between ImGui::NewFrame() and ImGui::Render()
    void draw();

    // How much the user has zoomed the whole UI in or out (1 = normal)
    float uiZoom() const { return zoom; }

private:
    // UI sections
    void drawToolbar();
    void drawSourceSection();
    void drawRadioSection();
    void drawDisplaySection();
    void handleZoomShortcuts();
    void setZoom(float newZoom);
    void drawSignalPanel();
    void drawSpectrum(ImDrawList* drawList, ImVec2 min, ImVec2 max);
    void drawVfo(ImDrawList* drawList, ImVec2 min, ImVec2 max) const;
    void handleTuning(ImVec2 min, ImVec2 max);

    // Actions
    void play();
    void stop();
    void refreshDevices();
    void applySourceSettings();
    void applyCenterFreq();
    void setOffset(double offsetHz);
    void setMode(DemodMode mode);

    // Helpers
    uint32_t sampleRate() const;
    double tuningStep() const;
    float offsetToX(double offsetHz, ImVec2 min, ImVec2 max) const;

    AudioSink audio;
    Receiver receiver{audio};
    RtlSdrSource source;
    Waterfall waterfall;
    bool playing = false;
    std::string status;

    // Source settings
    std::vector<RtlSdrDeviceInfo> devices;
    int deviceIndex = 0;
    double lastDeviceScan = -100.0;
    std::vector<int> gains; // tenths of a dB, as reported by the tuner
    int sampleRateIndex;
    int tunerBandwidthIndex = 0;
    bool tunerAgc = false; // the R820T's AGC runs the gain very high, so start with manual gain
    int gainIndex = 0;
    bool rtlAgc = false;
    int ppm = 0;
    bool biasTee = false;
    double centerFreqMHz = 100.0;

    // Radio settings
    DemodSettings demod;
    double bandwidthPerMode[3] = {200'000.0, 12'500.0, 10'000.0}; // remembered per mode

    // Display settings
    int fftSizeIndex = 1;
    float minDb = -80.0f;
    float maxDb = -20.0f;
    float smoothing = 0.6f;
    float zoom = 1.0f;
    std::vector<float> spectrumLine; // smoothed, for the spectrum plot
    std::vector<std::vector<float>> newLines;
    std::vector<float> columnMax;
};
