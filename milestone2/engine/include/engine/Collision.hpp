#pragma once

#include "engine/Entity.hpp"

namespace engine {

// Generic bounding-box collision detection between any two entities. The
// engine only compares rectangles - it has no idea what a "player" or an
// "enemy" is, and leaves collision *response* entirely to the game.
class Collision {
public:
    static bool CheckCollision(const Entity &a, const Entity &b);
};

} // namespace engine
