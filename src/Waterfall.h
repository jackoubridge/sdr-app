#pragma once

#include <imgui.h>

#include <cstdint>
#include <vector>

// A scrolling spectrogram: each spectrum line becomes one row of colour, newest at the top.
// Rows live in an OpenGL texture used as a ring buffer, so adding a line uploads one row
// instead of shifting the whole image.
class Waterfall {
public:
    Waterfall();
    ~Waterfall();

    void addLine(const std::vector<float>& line, float minDb, float maxDb);
    void draw(ImDrawList* drawList, ImVec2 min, ImVec2 max) const;

private:
    void createTexture(int newWidth);

    static constexpr int kHeight = 512; // rows of history

    unsigned texture = 0;
    int width = 0;
    int newestRow = 0;
    uint32_t palette[256];
    std::vector<float> scaled;
    std::vector<uint8_t> levels;
    std::vector<uint32_t> pixels;
};
