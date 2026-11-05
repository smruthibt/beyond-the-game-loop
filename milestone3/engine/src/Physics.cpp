#include "engine/Physics.hpp"

namespace engine {

float Physics::gravity = 0.0f;

void Physics::SetGravity(float g) {
    gravity = g;
}

float Physics::GetGravity() {
    return gravity;
}

void Physics::Update(Entity &entity, float dt) {
    if (entity.affectedByGravity) {
        entity.velocity.y += gravity * dt;
    }
    entity.position.x += entity.velocity.x * dt;
    entity.position.y += entity.velocity.y * dt;
}

} // namespace engine
