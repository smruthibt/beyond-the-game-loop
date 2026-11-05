#pragma once

#include <SDL3/SDL.h>

#include "engine/Timeline.hpp"

namespace engine {

struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

// A generic game object. The engine only knows that an Entity has a
// position, a size, and something to draw (a sprite sheet texture, or a
// flat-colored rectangle if no texture is set) - it never knows what the
// entity represents in a specific game.
class Entity {
public:
    Vec2 position;
    Vec2 velocity;
    Vec2 size = {32.0f, 32.0f};

    bool affectedByGravity = false;
    bool active = true;

    // The fill when no texture is set, and a tint multiplied over the sprite
    // when one is: white leaves a sprite as drawn, so one greyscale sheet can
    // be drawn in any number of colors.
    SDL_Color color = {255, 255, 255, 255};

    // Optional sprite sheet animation.
    SDL_Texture *texture = nullptr;
    int frameCount = 1;
    int currentFrame = 0;
    int frameWidth = 0;
    int frameHeight = 0;
    Uint32 frameDelayMs = 100;

    // The instant this animation started, in seconds on whatever timeline
    // drives it. The current frame is a pure function of that clock and this
    // zero - which is exactly what a "local timeline" is: an asset whose
    // zero-time is marked by something in the game. Negative means unseeded.
    double animationStartSeconds = -1.0;

    // Bounds in world space, i.e. never scaled. The render scale is uniform
    // about the origin, so world-space overlap and on-screen overlap are the
    // same question - collision and physics stay independent of the view.
    SDL_FRect GetBounds() const;

    void SetTexture(SDL_Texture *tex, int frameCount, int frameWidth, int frameHeight, Uint32 frameDelayMs = 100);
    // Advances the frame if `frameDelayMs` of the given timeline's time has
    // passed. Pass the engine's game time for an animation that pauses and
    // scales with the world, or a local timeline anchored to it for one that
    // has its own zero - a death animation, a power-up flash - which is what
    // "local timelines" means.
    void UpdateAnimation(const Timeline &clock);

    // `scale` comes from Engine::GetScale() / the Scaling system, and is
    // applied to position and size together. `camera` is the world point that
    // lands on the window's top-left corner.
    void Draw(SDL_Renderer *renderer, float scale = 1.0f, Vec2 camera = {}) const;
};

} // namespace engine
