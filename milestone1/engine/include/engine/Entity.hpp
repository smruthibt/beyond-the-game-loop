#pragma once

#include <SDL3/SDL.h>

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

    // Rectangle fallback, used when no texture is set.
    SDL_Color color = {255, 255, 255, 255};

    // Optional sprite sheet animation.
    SDL_Texture *texture = nullptr;
    int frameCount = 1;
    int currentFrame = 0;
    int frameWidth = 0;
    int frameHeight = 0;
    Uint32 frameDelayMs = 100;
    Uint32 lastFrameTime = 0;

    // Bounds in world space, i.e. never scaled. The render scale is uniform
    // about the origin, so world-space overlap and on-screen overlap are the
    // same question - collision and physics stay independent of the view.
    SDL_FRect GetBounds() const;

    void SetTexture(SDL_Texture *tex, int frameCount, int frameWidth, int frameHeight, Uint32 frameDelayMs = 100);
    void UpdateAnimation(Uint32 nowMs);

    // `scale` comes from Engine::GetScale() / the Scaling system, and is
    // applied to position and size together.
    void Draw(SDL_Renderer *renderer, float scale = 1.0f) const;
};

} // namespace engine
