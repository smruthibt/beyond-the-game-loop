#pragma once

#include "engine/Components.hpp"
#include "engine/World.hpp"

#include <SDL3/SDL.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace engine {

// The behaviour half of the object model: each system is a plain function
// that walks the World and acts on every object holding the components it
// reads. A system never asks what KIND of object it is looking at - the
// physics step moves anything with Transform + RigidBody, whether the game
// calls it a player, a crate or a bullet.
//
// The optional filter limits a system to some objects. A networked endpoint
// uses it to simulate only what it is authoritative for: a client steps its
// own player and leaves everyone else to the snapshots.
using ObjectFilter = std::function<bool(const GameObject &)>;

// -1 for objects with no NetIdentity, which are the authority's.
int OwnerOf(const GameObject &object);
ObjectFilter OwnedBy(int owner);

struct PhysicsSettings {
    float gravity = 1800.0f;        // world units per second squared, for bodies with gravity
    float maxFallSpeed = 1400.0f;
    float maxStep = 1.0f / 120.0f;  // dt is split into steps no longer than this
};

struct RespawnEvent {
    ObjectId object;
    ObjectId zone;
    ObjectId spawn;
};

// A mover inside a Trigger collider: a player on a pressure plate, touching a
// gem, standing in a doorway.
struct TriggerContact {
    ObjectId trigger;
    ObjectId other;
};

// What changed since the last Update: Enter once when something arrives,
// Stay every update while it is there, Exit once when it leaves (or is
// destroyed). Pickups want Enter; plates and doorways want Stay.
struct TriggerEvent {
    enum class Type : uint8_t { Enter, Stay, Exit };
    Type type;
    ObjectId trigger;
    ObjectId other;
};

class TriggerTracker {
public:
    // Movers (Transform + RigidBody) that `filter` accepts, against every
    // Trigger collider.
    std::vector<TriggerEvent> Update(World &world, const ObjectFilter &filter = {});
    void Clear() { inside.clear(); }

private:
    std::vector<std::pair<ObjectId, ObjectId>> inside;   // sorted (trigger, other)
};

// Which key presses which button. A key may press several (W is Up and Jump).
struct KeyBinding {
    SDL_Scancode key;
    uint32_t button;   // an InputState::Button
};
// A/D or the arrows run, W/Up/Space jump, S/Down is Down, E/Enter is Action.
std::vector<KeyBinding> DefaultKeyBindings();

namespace systems {

// Places every PathMover at its pattern's position for `seconds` on the
// clock the caller chooses - the server's, or a shared clock between peers.
void MovePaths(World &world, double seconds);

// InputState + PlatformerController + RigidBody -> velocity: run, and jump
// from the ground on a fresh press.
void ApplyControllers(World &world, const ObjectFilter &filter = {});

// Moves every Transform + RigidBody: gravity, then the X axis and the Y axis
// separately against every Solid collider, so movers stop at walls, land on
// floors, bump heads on ceilings and pass up through one-way ledges. A body
// standing on something that moved since the last step - a moving platform,
// whether this endpoint moved it or a snapshot did - is carried with it.
void StepPhysics(World &world, float dt, const PhysicsSettings &settings = {}, const ObjectFilter &filter = {});

// Sends every Respawn object that overlaps a DeathZone to its spawn point.
std::vector<RespawnEvent> ApplyDeathZones(World &world, const ObjectFilter &filter = {});

// Every mover (Transform + RigidBody, passing `filter`) inside every Trigger
// collider, right now. TriggerTracker turns this into enter/stay/exit.
std::vector<TriggerContact> FindTriggerContacts(World &world, const ObjectFilter &filter = {});

// Sets `input` from the keyboard: each binding whose key is down presses its
// button. Main thread (it reads Input). `isDown` replaces the keyboard - for
// tests, replays, or a second player on the same keyboard.
void ReadKeyboard(InputState &input, const std::vector<KeyBinding> &bindings = DefaultKeyBindings(),
                  const std::function<bool(SDL_Scancode)> &isDown = {});

// The SpawnPoint with this index, or the lowest-indexed one; nullptr if none.
GameObject *FindSpawn(World &world, int index);
// Stands `object` on the spawn point (bottom-centre to bottom-centre) and
// stops it. False if there is no spawn point at all.
bool PlaceAtSpawn(World &world, GameObject &object, int index);

// Keeps each camera's scroll boundaries in place around its view, and when
// the camera's target pushes into one, translates the camera by exactly that
// much. Then clamps to the camera's bounds.
void UpdateCameras(World &world);

// Makes the two hidden side boundaries (and, if insetY > 0, top and bottom)
// for a camera. Returns how many it made.
int AddScrollBoundaries(World &world, ObjectId camera, float insetX, float insetY = 0.0f);

} // namespace systems

// Loads sprites named by Renderable::sprite from the media/ folder beside the
// build folder, once each. The renderer owns the textures.
class TextureCache {
public:
    SDL_Texture *Get(SDL_Renderer *renderer, const std::string &name);
    void SetRoot(const std::string &directory) { root = directory; }
    // A texture the game made itself, under a name Renderable::sprite can use.
    void Put(const std::string &name, SDL_Texture *texture) { cache[name] = texture; }

private:
    std::map<std::string, SDL_Texture *> cache;   // a failed load is remembered as nullptr
    std::string root;
};

namespace systems {

// Draws every visible Renderable, lowest layer first, through `camera`.
// `seconds` drives sprite animation - pass the game time for animation that
// pauses with the game.
void Render(const World &world, SDL_Renderer *renderer, const Camera &camera, float scale,
            double seconds, TextureCache *textures = nullptr);

} // namespace systems

} // namespace engine
