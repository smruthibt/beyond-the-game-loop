#pragma once

#include "engine/Entity.hpp"

namespace engine {

// Generic physics system. Gravity is a single, configurable value shared by
// every entity that opts in via Entity::affectedByGravity - it is never
// hard-coded per-entity.
class Physics {
public:
    static void SetGravity(float gravityPixelsPerSecondSquared);
    static float GetGravity();

    // Integrates gravity into velocity (if the entity is affected by it),
    // then integrates velocity into position.
    static void Update(Entity &entity, float deltaSeconds);

private:
    static float gravity;
};

} // namespace engine
