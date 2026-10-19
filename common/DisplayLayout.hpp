#pragma once
#include <algorithm>
#include <cmath>

namespace td {
inline int logicalDisplaySize(int pixels, double scale) {
    // Protocol dimensions are even. At 2x the logical dimension may be odd;
    // rounding that logical size to an even value would add two backing pixels.
    if (scale == 1.0)
        return pixels;
    if (scale == 2.0)
        return pixels / 2;
    return std::max(128, int(std::round(pixels / scale / 2)) * 2);
}
}
