#include "engine/Collision.hpp"

namespace engine {

bool Collision::CheckCollision(const Entity &a, const Entity &b) {
    if (!a.active || !b.active) {
        return false;
    }
    SDL_FRect rectA = a.GetBounds();
    SDL_FRect rectB = b.GetBounds();
    return SDL_HasRectIntersectionFloat(&rectA, &rectB);
}

} // namespace engine
