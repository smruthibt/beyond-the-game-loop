#include "engine/Scaling.hpp"

#include <algorithm>

namespace engine {

ScaleMode Scaling::mode = ScaleMode::Pixel;

void Scaling::Toggle() {
    mode = (mode == ScaleMode::Pixel) ? ScaleMode::Proportional : ScaleMode::Pixel;
}

ScaleMode Scaling::GetMode() {
    return mode;
}

const char *Scaling::GetModeName() {
    return mode == ScaleMode::Pixel ? "constant (pixel)" : "proportional (%)";
}

float Scaling::GetFactor(int windowWidth, int windowHeight, int baseWidth, int baseHeight) {
    // Constant size: world units are pixels, whatever the window does.
    if (mode == ScaleMode::Pixel) {
        return 1.0f;
    }
    if (baseWidth <= 0 || baseHeight <= 0) {
        return 1.0f;
    }
    // Proportional: take the smaller of the two ratios so the design space
    // always fits and a non-matching aspect ratio never stretches anything.
    float scaleX = (float)windowWidth / (float)baseWidth;
    float scaleY = (float)windowHeight / (float)baseHeight;
    return std::min(scaleX, scaleY);
}

} // namespace engine
