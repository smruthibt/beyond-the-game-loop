#pragma once

namespace engine {

enum class ScaleMode {
    // Constant, pixel-based: one world unit is always one screen pixel.
    // Resizing the window reveals or hides world, it never resizes objects.
    Pixel,
    // Percentage-based: the design resolution is mapped onto the window, so
    // objects keep their share of the screen no matter how big it is.
    Proportional
};

// Task 6. Produces the single world-to-screen factor the renderer applies.
// The factor is applied to entity positions *and* sizes alike (see
// Entity::Draw), which makes it a uniform scale about the origin: relative
// geometry is preserved exactly, so gameplay - physics, collision - can stay
// in world space and never has to know which mode is active.
class Scaling {
public:
    static void Toggle();
    static ScaleMode GetMode();
    static const char *GetModeName();

    // Multiplier to apply to an entity's position and size given the current
    // window dimensions and the resolution the game was designed at.
    static float GetFactor(int windowWidth, int windowHeight, int baseWidth, int baseHeight);

private:
    static ScaleMode mode;
};

} // namespace engine
