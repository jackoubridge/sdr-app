#include "App.h"

#include <ipp.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {

struct Option {
    uint32_t value;
    const char* label;
};

constexpr Option kSampleRates[] = {
    {250'000, "0.25 MHz"},     {1'024'000, "1.024 MHz"}, {1'536'000, "1.536 MHz"},
    {1'792'000, "1.792 MHz"},  {1'920'000, "1.92 MHz"},  {2'048'000, "2.048 MHz"},
    {2'160'000, "2.16 MHz"},   {2'400'000, "2.4 MHz"},
    {2'560'000, "2.56 MHz (may drop samples)"},
    {2'880'000, "2.88 MHz (may drop samples)"},
    {3'200'000, "3.2 MHz (may drop samples)"},
};
constexpr int kDefaultSampleRate = 7; // 2.4 MHz

// The tuner's own analogue filter, ahead of the sampling
constexpr Option kTunerBandwidths[] = {
    {0, "Auto"},           {300'000, "300 kHz"},   {600'000, "600 kHz"},
    {1'000'000, "1 MHz"},  {1'500'000, "1.5 MHz"}, {2'000'000, "2 MHz"},
    {2'500'000, "2.5 MHz"}, {3'000'000, "3 MHz"},
};

constexpr int kFftSizes[] = {1024, 2048, 4096, 8192};
constexpr const char* kFftSizeLabels[] = {"1024", "2048", "4096", "8192"};

constexpr const char* kModeNames[] = {"WFM", "NFM", "AM"};
constexpr float kBandwidthMinKHz[] = {50.0f, 5.0f, 3.0f};
constexpr float kBandwidthMaxKHz[] = {216.0f, 40.0f, 40.0f};
constexpr double kTuningSteps[] = {100'000.0, 12'500.0, 1'000.0};

// Tuning range of the R820T2 tuner in the NESDR SMART
constexpr double kMinFreqMHz = 24.0;
constexpr double kMaxFreqMHz = 1766.0;

constexpr double kDeviceScanSeconds = 2.0;

constexpr float kMinZoom = 0.5f;
constexpr float kMaxZoom = 3.0f;
constexpr float kZoomStep = 0.1f;

// Combo box over a list of Options; returns true when the selection changes
template <size_t N>
bool optionCombo(const char* id, int& index, const Option (&options)[N]) {
    bool changed = false;
    if (ImGui::BeginCombo(id, options[index].label)) {
        for (int i = 0; i < static_cast<int>(N); ++i) {
            if (ImGui::Selectable(options[i].label, i == index)) {
                index = i;
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

// A round grid spacing (1, 2 or 5 times a power of ten) close to `rough`
double niceStep(double rough) {
    double power = std::pow(10.0, std::floor(std::log10(rough)));
    for (double m : {1.0, 2.0, 5.0}) {
        if (m * power >= rough) {
            return m * power;
        }
    }
    return 10.0 * power;
}

} // namespace

App::App() : sampleRateIndex(kDefaultSampleRate) {
    receiver.setFftSize(kFftSizes[fftSizeIndex]);
}

App::~App() {
    stop();
}

uint32_t App::sampleRate() const {
    return kSampleRates[sampleRateIndex].value;
}

double App::tuningStep() const {
    return kTuningSteps[static_cast<int>(demod.mode)];
}

// ---- Actions ----

void App::play() {
    if (devices.empty()) {
        return;
    }
    if (!source.open(devices[deviceIndex].index)) {
        status = "Could not open the device. It may be in use by another program, "
                 "or claimed by the kernel's DVB driver (sudo rmmod dvb_usb_rtl28xxu).";
        return;
    }

    uint32_t rate = sampleRate();
    source.setSampleRate(rate);
    applySourceSettings();
    applyCenterFreq();

    setOffset(demod.offsetHz); // re-clamps the VFO in case the sample rate shrank
    receiver.setFftSize(kFftSizes[fftSizeIndex]);
    receiver.reset(rate);
    spectrumLine.clear();

    status.clear();
    if (!audio.start()) {
        status = "No audio output available; continuing without sound.";
    }

    if (!source.start(rate, [this](const uint8_t* data, int len) { receiver.processRaw(data, len); })) {
        source.close();
        audio.stop();
        status = "Failed to start streaming from the device.";
        return;
    }
    playing = true;
}

void App::stop() {
    source.close();
    audio.stop();
    playing = false;
}

void App::refreshDevices() {
    lastDeviceScan = ImGui::GetTime();
    devices = RtlSdrSource::listDevices();
    if (devices.empty()) {
        deviceIndex = 0;
        gains.clear();
        return;
    }
    deviceIndex = std::min(deviceIndex, static_cast<int>(devices.size()) - 1);
    if (gains.empty()) {
        gains = RtlSdrSource::tunerGains(devices[deviceIndex].index);
        gainIndex = static_cast<int>(gains.size()) / 2;
    }
}

void App::applySourceSettings() {
    source.setTunerBandwidth(kTunerBandwidths[tunerBandwidthIndex].value);
    source.setGain(tunerAgc, gains.empty() ? 0 : gains[gainIndex]);
    source.setRtlAgc(rtlAgc);
    source.setPpm(ppm);
    source.setBiasTee(biasTee);
}

void App::applyCenterFreq() {
    centerFreqMHz = std::clamp(centerFreqMHz, kMinFreqMHz, kMaxFreqMHz);
    if (source.isOpen()) {
        source.setCenterFreq(static_cast<uint32_t>(std::llround(centerFreqMHz * 1e6)));
    }
}

void App::setOffset(double offsetHz) {
    // Keep the whole channel inside the captured band
    double limit = std::max(0.0, sampleRate() / 2.0 - demod.bandwidthHz / 2.0);
    demod.offsetHz = std::clamp(offsetHz, -limit, limit);
    receiver.setDemod(demod);
}

void App::setZoom(float newZoom) {
    // Round to whole steps so repeated zooming doesn't drift (e.g. to 1.0000001)
    zoom = std::clamp(std::round(newZoom / kZoomStep) * kZoomStep, kMinZoom, kMaxZoom);
}

void App::setMode(DemodMode mode) {
    demod.mode = mode;
    demod.bandwidthHz = bandwidthPerMode[static_cast<int>(mode)];
    setOffset(demod.offsetHz);
}

// ---- UI ----

void App::draw() {
    // Rescan while stopped, so plugging in a dongle is noticed
    if (!playing && ImGui::GetTime() - lastDeviceScan > kDeviceScanSeconds) {
        refreshDevices();
    }
    // Streaming ended without being asked to
    if (playing && !source.isStreaming()) {
        stop();
        status = "The device stopped streaming. Was it unplugged?";
    }
    handleZoomShortcuts();

    // Feed new spectrum lines to the waterfall and the (smoothed) spectrum plot
    receiver.takeSpectrumLines(newLines);
    for (const auto& line : newLines) {
        waterfall.addLine(line, minDb, maxDb);
        int n = static_cast<int>(line.size());
        if (spectrumLine.size() != line.size()) {
            spectrumLine = line;
        } else {
            // spectrumLine = smoothing * spectrumLine + (1 - smoothing) * line
            ippsMulC_32f_I(smoothing, spectrumLine.data(), n);
            ippsAddProductC_32f(line.data(), 1.0f - smoothing, spectrumLine.data(), n);
        }
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::Begin("SDR", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                 ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);

    drawToolbar();
    ImGui::Separator();

    ImGui::BeginChild("Sidebar", ImVec2(ImGui::GetFontSize() * 22, 0), ImGuiChildFlags_Borders);
    if (ImGui::CollapsingHeader("Source", ImGuiTreeNodeFlags_DefaultOpen)) {
        drawSourceSection();
    }
    if (ImGui::CollapsingHeader("Radio", ImGuiTreeNodeFlags_DefaultOpen)) {
        drawRadioSection();
    }
    if (ImGui::CollapsingHeader("Display", ImGuiTreeNodeFlags_DefaultOpen)) {
        drawDisplaySection();
    }
    if (!status.empty()) {
        ImGui::Separator();
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 180, 80, 255));
        ImGui::TextWrapped("%s", status.c_str());
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("Signal");
    drawSignalPanel();
    ImGui::EndChild();

    ImGui::End();
}

void App::drawToolbar() {
    ImGui::BeginDisabled(devices.empty() && !playing);
    if (ImGui::Button(playing ? "Stop" : "Play", ImVec2(ImGui::GetFontSize() * 5, 0))) {
        playing ? stop() : play();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14);
    bool edited = ImGui::InputDouble("Centre (MHz)", &centerFreqMHz, 0.1, 1.0, "%.4f");
    // Apply after typing (Enter or clicking away) or straight away for the +/- buttons
    if ((edited && !ImGui::IsItemActive()) || ImGui::IsItemDeactivatedAfterEdit()) {
        applyCenterFreq();
    }

    ImGui::SameLine();
    ImGui::Text("   Tuned: %.4f MHz %s", centerFreqMHz + demod.offsetHz / 1e6, kModeNames[static_cast<int>(demod.mode)]);
}

void App::drawSourceSection() {
    ImGui::BeginDisabled(playing); // these need a restart, as in SDR++

    ImGui::TextUnformatted("Device");
    ImGui::SetNextItemWidth(-ImGui::GetFontSize() * 5);
    const char* preview = devices.empty() ? "No device found" : devices[deviceIndex].label.c_str();
    if (ImGui::BeginCombo("##device", preview)) {
        for (int i = 0; i < static_cast<int>(devices.size()); ++i) {
            if (ImGui::Selectable(devices[i].label.c_str(), i == deviceIndex) && i != deviceIndex) {
                deviceIndex = i;
                gains = RtlSdrSource::tunerGains(devices[i].index);
                gainIndex = static_cast<int>(gains.size()) / 2;
            }
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Refresh", ImVec2(-FLT_MIN, 0))) {
        gains.clear();
        refreshDevices();
    }

    ImGui::TextUnformatted("Sample rate");
    ImGui::SetNextItemWidth(-FLT_MIN);
    optionCombo("##samplerate", sampleRateIndex, kSampleRates);

    ImGui::EndDisabled();

    // Everything below can change while playing
    ImGui::TextUnformatted("Tuner bandwidth");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (optionCombo("##tunerbw", tunerBandwidthIndex, kTunerBandwidths)) {
        source.setTunerBandwidth(kTunerBandwidths[tunerBandwidthIndex].value);
    }

    // The tuner only supports specific gain steps, so the slider moves through their indices
    ImGui::TextUnformatted("Gain");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (gains.empty()) {
        ImGui::BeginDisabled();
        int none = 0;
        ImGui::SliderInt("##gain", &none, 0, 0, "Connect a device");
        ImGui::EndDisabled();
    } else {
        char label[32];
        std::snprintf(label, sizeof label, tunerAgc ? "Auto" : "%.1f dB", gains[gainIndex] / 10.0);
        ImGui::BeginDisabled(tunerAgc);
        if (ImGui::SliderInt("##gain", &gainIndex, 0, static_cast<int>(gains.size()) - 1, label)) {
            source.setGain(false, gains[gainIndex]);
        }
        ImGui::EndDisabled();
    }

    if (ImGui::Checkbox("Tuner AGC", &tunerAgc)) {
        source.setGain(tunerAgc, gains.empty() ? 0 : gains[gainIndex]);
    }
    ImGui::SetItemTooltip("Let the tuner set its own gain; turn off to use the slider");
    if (ImGui::Checkbox("RTL AGC", &rtlAgc)) {
        source.setRtlAgc(rtlAgc);
    }
    ImGui::SetItemTooltip("Automatic gain in the RTL2832U's digital stage");

    if (ImGui::Checkbox("Bias tee", &biasTee)) {
        source.setBiasTee(biasTee);
    }
    ImGui::SetItemTooltip("Sends 4.5 V up the antenna cable to power an LNA.\nOnly enable with equipment that expects it.");

    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
    if (ImGui::InputInt("PPM correction", &ppm)) {
        ppm = std::clamp(ppm, -200, 200);
        source.setPpm(ppm);
    }
}

void App::drawRadioSection() {
    for (int i = 0; i < 3; ++i) {
        if (i > 0) {
            ImGui::SameLine();
        }
        if (ImGui::RadioButton(kModeNames[i], static_cast<int>(demod.mode) == i)) {
            setMode(static_cast<DemodMode>(i));
        }
    }

    int mode = static_cast<int>(demod.mode);
    float bandwidthKHz = static_cast<float>(demod.bandwidthHz / 1000.0);
    ImGui::TextUnformatted("Bandwidth");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::SliderFloat("##bandwidth", &bandwidthKHz, kBandwidthMinKHz[mode], kBandwidthMaxKHz[mode], "%.1f kHz")) {
        demod.bandwidthHz = bandwidthKHz * 1000.0;
        bandwidthPerMode[mode] = demod.bandwidthHz;
        setOffset(demod.offsetHz);
    }

    ImGui::TextUnformatted("Volume");
    ImGui::SetNextItemWidth(-FLT_MIN);
    float volumePercent = demod.volume * 100.0f;
    if (ImGui::SliderFloat("##volume", &volumePercent, 0.0f, 100.0f, "%.0f%%")) {
        demod.volume = volumePercent / 100.0f;
        receiver.setDemod(demod);
    }

    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("Click the spectrum to tune; scroll to step %.1f kHz", tuningStep() / 1000.0);
    ImGui::PopStyleColor();
}

void App::drawDisplaySection() {
    ImGui::TextUnformatted("FFT size");
    ImGui::SetNextItemWidth(-FLT_MIN);
    if (ImGui::Combo("##fftsize", &fftSizeIndex, kFftSizeLabels, IM_ARRAYSIZE(kFftSizeLabels))) {
        receiver.setFftSize(kFftSizes[fftSizeIndex]);
    }

    ImGui::TextUnformatted("Range");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::DragFloatRange2("##range", &minDb, &maxDb, 0.5f, -150.0f, 0.0f, "Min %.0f dB", "Max %.0f dB",
                           ImGuiSliderFlags_AlwaysClamp);
    maxDb = std::max(maxDb, minDb + 10.0f);

    ImGui::TextUnformatted("Smoothing");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderFloat("##smoothing", &smoothing, 0.0f, 0.95f, "%.2f");

    // Buttons rather than a slider, as the slider would resize under the mouse while dragging
    ImGui::TextUnformatted("UI zoom");
    if (ImGui::Button("-")) {
        setZoom(zoom - kZoomStep);
    }
    ImGui::SameLine();
    if (ImGui::Button("+")) {
        setZoom(zoom + kZoomStep);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reset")) {
        setZoom(1.0f);
    }
    ImGui::SameLine();
    ImGui::Text("%.0f%%", zoom * 100.0f);
    ImGui::SetItemTooltip("Ctrl + / Ctrl - / Ctrl 0, or Ctrl + mouse wheel");
}

void App::handleZoomShortcuts() {
    // ImGuiInputFlags_RouteGlobal: works whichever widget has focus
    auto pressed = [](ImGuiKeyChord chord) { return ImGui::Shortcut(chord, ImGuiInputFlags_RouteGlobal); };
    if (pressed(ImGuiMod_Ctrl | ImGuiKey_Equal) || pressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Equal) ||
        pressed(ImGuiMod_Ctrl | ImGuiKey_KeypadAdd)) {
        setZoom(zoom + kZoomStep);
    }
    if (pressed(ImGuiMod_Ctrl | ImGuiKey_Minus) || pressed(ImGuiMod_Ctrl | ImGuiKey_KeypadSubtract)) {
        setZoom(zoom - kZoomStep);
    }
    if (pressed(ImGuiMod_Ctrl | ImGuiKey_0) || pressed(ImGuiMod_Ctrl | ImGuiKey_Keypad0)) {
        setZoom(1.0f);
    }

    const ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl && io.MouseWheel != 0.0f) {
        setZoom(zoom + (io.MouseWheel > 0 ? kZoomStep : -kZoomStep));
    }
}

float App::offsetToX(double offsetHz, ImVec2 min, ImVec2 max) const {
    return min.x + static_cast<float>((offsetHz / sampleRate() + 0.5) * (max.x - min.x));
}

void App::drawSignalPanel() {
    ImVec2 min = ImGui::GetCursorScreenPos();
    ImVec2 size = ImGui::GetContentRegionAvail();
    if (size.x < 10 || size.y < 10) {
        return;
    }
    ImVec2 max(min.x + size.x, min.y + size.y);
    float split = min.y + std::floor(size.y * 0.4f);
    ImVec2 spectrumMax(max.x, split);
    ImVec2 waterfallMin(min.x, split + 2);

    ImDrawList* drawList = ImGui::GetWindowDrawList();
    drawSpectrum(drawList, min, spectrumMax);
    waterfall.draw(drawList, waterfallMin, max);
    drawVfo(drawList, min, spectrumMax);
    drawVfo(drawList, waterfallMin, max);

    // One invisible button over both views captures the mouse for tuning
    ImGui::InvisibleButton("##tune", size);
    handleTuning(min, max);
}

void App::drawSpectrum(ImDrawList* drawList, ImVec2 min, ImVec2 max) {
    drawList->PushClipRect(min, max, true);
    drawList->AddRectFilled(min, max, IM_COL32(12, 14, 20, 255));

    float width = max.x - min.x;
    float height = max.y - min.y;
    const ImU32 gridColour = IM_COL32(255, 255, 255, 30);
    const ImU32 labelColour = IM_COL32(200, 200, 200, 200);
    auto dbToY = [&](float db) { return max.y - (db - minDb) / (maxDb - minDb) * height; };

    // Horizontal grid every 10 dB
    for (float db = std::ceil(minDb / 10.0f) * 10.0f; db <= maxDb; db += 10.0f) {
        float y = dbToY(db);
        drawList->AddLine(ImVec2(min.x, y), ImVec2(max.x, y), gridColour);
        char label[16];
        std::snprintf(label, sizeof label, "%.0f dB", db);
        drawList->AddText(ImVec2(min.x + 4, y - ImGui::GetFontSize()), labelColour, label);
    }

    // Vertical grid at round frequencies
    double rate = sampleRate();
    double lowHz = centerFreqMHz * 1e6 - rate / 2.0;
    double step = niceStep(rate / 8.0);
    int decimals = std::max(0, static_cast<int>(std::ceil(-std::log10(step / 1e6))));
    for (double f = std::ceil(lowHz / step) * step; f <= lowHz + rate; f += step) {
        float x = min.x + static_cast<float>((f - lowHz) / rate * width);
        drawList->AddLine(ImVec2(x, min.y), ImVec2(x, max.y), gridColour);
        char label[32];
        std::snprintf(label, sizeof label, "%.*f", decimals, f / 1e6);
        drawList->AddText(ImVec2(x + 3, max.y - ImGui::GetFontSize() - 2), labelColour, label);
    }

    if (spectrumLine.empty()) {
        const char* hint = playing ? "Waiting for samples..." : "Press Play to start receiving";
        ImVec2 textSize = ImGui::CalcTextSize(hint);
        drawList->AddText(ImVec2(min.x + (width - textSize.x) / 2, min.y + (height - textSize.y) / 2), labelColour, hint);
        drawList->PopClipRect();
        return;
    }

    // One value per pixel column: the strongest bin it covers, so narrow peaks aren't lost
    int columns = std::max(1, static_cast<int>(width));
    int bins = static_cast<int>(spectrumLine.size());
    columnMax.resize(columns);
    for (int x = 0; x < columns; ++x) {
        int start = std::min(x * bins / columns, bins - 1);
        int end = std::max(start + 1, (x + 1) * bins / columns);
        ippsMax_32f(spectrumLine.data() + start, end - start, &columnMax[x]);
    }

    const ImU32 fillColour = IM_COL32(60, 140, 255, 50);
    const ImU32 lineColour = IM_COL32(120, 190, 255, 255);
    ImVec2 previous;
    for (int x = 0; x < columns; ++x) {
        float y = std::clamp(dbToY(columnMax[x]), min.y, max.y);
        ImVec2 point(min.x + x, y);
        drawList->AddLine(ImVec2(point.x, y), ImVec2(point.x, max.y), fillColour);
        if (x > 0) {
            drawList->AddLine(previous, point, lineColour);
        }
        previous = point;
    }

    drawList->PopClipRect();
}

void App::drawVfo(ImDrawList* drawList, ImVec2 min, ImVec2 max) const {
    float x = offsetToX(demod.offsetHz, min, max);
    float halfWidth = static_cast<float>(demod.bandwidthHz / sampleRate() * (max.x - min.x) / 2.0);
    drawList->PushClipRect(min, max, true);
    drawList->AddRectFilled(ImVec2(x - halfWidth, min.y), ImVec2(x + halfWidth, max.y), IM_COL32(255, 255, 255, 35));
    drawList->AddLine(ImVec2(x, min.y), ImVec2(x, max.y), IM_COL32(255, 70, 70, 255), 1.5f);
    drawList->PopClipRect();
}

void App::handleTuning(ImVec2 min, ImVec2 max) {
    double centerHz = centerFreqMHz * 1e6;
    double step = tuningStep();
    auto snap = [&](double offsetHz) { return std::round((centerHz + offsetHz) / step) * step - centerHz; };
    auto mouseOffset = [&] {
        return (static_cast<double>(ImGui::GetMousePos().x - min.x) / (max.x - min.x) - 0.5) * sampleRate();
    };

    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        setOffset(snap(mouseOffset()));
    }

    if (ImGui::IsItemHovered()) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f && !ImGui::GetIO().KeyCtrl) { // Ctrl + wheel zooms the UI instead
            setOffset(snap(demod.offsetHz + (wheel > 0 ? step : -step)));
        }
        ImGui::SetTooltip("%.4f MHz", (centerHz + mouseOffset()) / 1e6);
    }
}
