#pragma once

#include "engine/Entity.hpp"

namespace engine {

// Generic bounding-box collision between any two entities. The engine only
// compares rectangles - it has no idea what a "player" or an "enemy" is.
class Collision {
public:
    static bool CheckCollision(const Entity &a, const Entity &b);

    // The smallest move that takes `mover` out of `solid`, or (0,0) if they do
    // not overlap. Pushes along whichever axis has the shallower penetration,
    // which is what makes a moving box stop against a wall and slide along it
    // rather than snag on it.
    //
    // This is the one piece of collision *response* the engine offers, and it
    // is deliberately the dumbest one: no velocity, no restitution, no mass.
    // Every game that has solid geometry needs exactly this step and would
    // otherwise re-derive it - Milestone 1's one-way landing was a special
    // case of it written by hand. What to do after the push-out (zero the
    // velocity? bounce? die?) is still the game's decision.
    static Vec2 Resolve(const Entity &mover, const Entity &solid);
};

} // namespace engine
