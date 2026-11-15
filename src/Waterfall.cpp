#include "Waterfall.h"

#include "Gl.h"

#include <ipp.h>

#include <algorithm>
#include <cmath>

Waterfall::Waterfall() {
    // Colour map from weak to strong: black -> blue -> cyan -> yellow -> red
    const float stops[][3] = {{0, 0, 0.05f}, {0, 0, 0.7f}, {0, 0.8f, 1}, {1, 1, 0}, {1, 0, 0}};
    const int segments = 4;
    for (int i = 0; i < 256; ++i) {
        float pos = i / 255.0f * segments;
        int s = std::min(static_cast<int>(pos), segments - 1);
        float t = pos - s;
        auto mix = [&](int c) { return static_cast<int>(255 * (stops[s][c] + t * (stops[s + 1][c] - stops[s][c]))); };
        palette[i] = IM_COL32(mix(0), mix(1), mix(2), 255);
    }
}

Waterfall::~Waterfall() {
    if (texture) {
        gl::DeleteTextures(1, &texture);
    }
}

void Waterfall::createTexture(int newWidth) {
    if (texture) {
        gl::DeleteTextures(1, &texture);
    }
    width = newWidth;
    newestRow = 0;

    std::vector<uint32_t> blank(static_cast<size_t>(width) * kHeight, palette[0]);
    gl::GenTextures(1, &texture);
    gl::BindTexture(gl::TEXTURE_2D, texture);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MIN_FILTER, gl::LINEAR);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_MAG_FILTER, gl::LINEAR);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_S, gl::CLAMP_TO_EDGE);
    gl::TexParameteri(gl::TEXTURE_2D, gl::TEXTURE_WRAP_T, gl::REPEAT); // lets the ring buffer wrap when drawn
    gl::TexImage2D(gl::TEXTURE_2D, 0, gl::RGBA, width, kHeight, 0, gl::RGBA, gl::UNSIGNED_BYTE, blank.data());
}

void Waterfall::addLine(const std::vector<float>& line, float minDb, float maxDb) {
    int n = static_cast<int>(line.size());
    if (n != width) {
        createTexture(n); // FFT size changed: start a fresh history
    }

    // dB -> palette index 0..255
    scaled.resize(n);
    levels.resize(n);
    pixels.resize(n);
    ippsNormalize_32f(line.data(), scaled.data(), n, minDb, (maxDb - minDb) / 255.0f);
    ippsThreshold_LTValGTVal_32f_I(scaled.data(), n, 0.0f, 0.0f, 255.0f, 255.0f);
    ippsConvert_32f8u_Sfs(scaled.data(), levels.data(), n, ippRndNear, 0);
    for (int i = 0; i < n; ++i) {
        pixels[i] = palette[levels[i]];
    }

    newestRow = (newestRow - 1 + kHeight) % kHeight;
    gl::BindTexture(gl::TEXTURE_2D, texture);
    gl::TexSubImage2D(gl::TEXTURE_2D, 0, 0, newestRow, width, 1, gl::RGBA, gl::UNSIGNED_BYTE, pixels.data());
}

void Waterfall::draw(ImDrawList* drawList, ImVec2 min, ImVec2 max) const {
    if (!texture) {
        drawList->AddRectFilled(min, max, palette[0]);
        return;
    }
    // Start at the newest row and run one full texture height; REPEAT wraps to the oldest rows
    float top = static_cast<float>(newestRow) / kHeight;
    drawList->AddImage(static_cast<ImTextureID>(texture), min, max, ImVec2(0, top), ImVec2(1, top + 1));
}
