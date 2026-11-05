#include "engine/Collision.hpp"

#include <cmath>

namespace engine {

bool Collision::CheckCollision(const Entity &a, const Entity &b) {
    if (!a.active || !b.active) {
        return false;
    }
    SDL_FRect rectA = a.GetBounds();
    SDL_FRect rectB = b.GetBounds();
    return SDL_HasRectIntersectionFloat(&rectA, &rectB);
}

Vec2 Collision::Resolve(const Entity &mover, const Entity &solid) {
    if (!mover.active || !solid.active) {
        return {0.0f, 0.0f};
    }
    const SDL_FRect a = mover.GetBounds();
    const SDL_FRect b = solid.GetBounds();

    // Signed distance between centres, and the extent the boxes would need to
    // be apart on each axis to not touch.
    const float dx = (a.x + a.w * 0.5f) - (b.x + b.w * 0.5f);
    const float dy = (a.y + a.h * 0.5f) - (b.y + b.h * 0.5f);
    const float overlapX = (a.w + b.w) * 0.5f - std::fabs(dx);
    const float overlapY = (a.h + b.h) * 0.5f - std::fabs(dy);

    if (overlapX <= 0.0f || overlapY <= 0.0f) {
        return {0.0f, 0.0f};
    }

    // Push out along the shallower axis, in the direction the mover already
    // leans. A box sliding along a wall penetrates it only slightly on one
    // axis and deeply on the other; correcting the shallow one is the slide.
    if (overlapX < overlapY) {
        return {dx < 0.0f ? -overlapX : overlapX, 0.0f};
    }
    return {0.0f, dy < 0.0f ? -overlapY : overlapY};
}

} // namespace engine
