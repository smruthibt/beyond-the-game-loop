#include "engine/Components.hpp"

namespace engine {

void ComponentRegistry::RegisterBuiltins() {
    Register<Transform>();
    Register<RigidBody>();
    Register<Collider>();
    Register<Renderable>();
    Register<InputState>();
    Register<PlatformerController>();
    Register<PathMover>();
    Register<SpawnPoint>();
    Register<DeathZone>();
    Register<Respawn>();
    Register<NetIdentity>();
}

} // namespace engine
