#include "engine/Entity.hpp"

namespace engine {

SDL_FRect Entity::GetBounds() const {
    return SDL_FRect{position.x, position.y, size.x, size.y};
}

void Entity::SetTexture(SDL_Texture *tex, int frames, int fw, int fh, Uint32 delayMs) {
    texture = tex;
    frameCount = frames;
    frameWidth = fw;
    frameHeight = fh;
    frameDelayMs = delayMs;
    currentFrame = 0;
    lastFrameTime = 0;
}

void Entity::UpdateAnimation(Uint32 nowMs) {
    if (!texture || frameCount <= 1) {
        return;
    }
    if (nowMs - lastFrameTime >= frameDelayMs) {
        currentFrame = (currentFrame + 1) % frameCount;
        lastFrameTime = nowMs;
    }
}

void Entity::Draw(SDL_Renderer *renderer, float scale) const {
    if (!active) {
        return;
    }

    // The scale is applied to the position as well as the size, not just
    // the size. Scaling only the size would shrink sprites in place and
    // leave them anchored to unscaled world coordinates - a player snapped
    // flush onto a platform would render floating above it. Scaling both
    // makes this a uniform transform about the origin, so anything that
    // overlaps in world space (what Collision tests) overlaps on screen.
    SDL_FRect dst = {position.x * scale, position.y * scale, size.x * scale, size.y * scale};

    if (texture) {
        SDL_FRect src = {
            (float)(currentFrame * frameWidth),
            0.0f,
            (float)frameWidth,
            (float)frameHeight
        };
        SDL_RenderTexture(renderer, texture, &src, &dst);
    } else {
        SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
        SDL_RenderFillRect(renderer, &dst);
    }
}

} // namespace engine
