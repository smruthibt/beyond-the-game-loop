#pragma once

#include "engine/Entity.hpp"   // Vec2
#include "engine/GameObject.hpp"
#include "engine/Serialize.hpp"

#include <SDL3/SDL.h>

#include <cmath>
#include <string>

namespace engine {

// The components the engine itself understands, and the systems in
// Systems.hpp act on. Games add their own the same way: derive from
// Component, ENGINE_COMPONENT(Name), Write/Read the fields that travel, and
// ComponentRegistry::Register<T>() at startup on every endpoint.
//
// Rules every component here follows:
//   - plain data, no behaviour beyond small helpers;
//   - Write and Read list the same fields in the same order;
//   - runtime-only fields (a frame's carry delta, an edge-detect latch) are
//     marked as such and never written - the far end recomputes them.

// Where an object is and how big its box is. World units, never scaled.
struct Transform : Component {
    ENGINE_COMPONENT(Transform)
    Vec2 position;
    Vec2 size = {32.0f, 32.0f};

    Transform() = default;
    Transform(float x, float y, float w, float h) : position{x, y}, size{w, h} {}

    float Left() const { return position.x; }
    float Right() const { return position.x + size.x; }
    float Top() const { return position.y; }
    float Bottom() const { return position.y + size.y; }
    Vec2 Center() const { return {position.x + size.x * 0.5f, position.y + size.y * 0.5f}; }

    // Strict overlap: boxes that only share an edge do not overlap, so a
    // player standing on a floor is not "inside" it.
    bool Overlaps(const Transform &o) const {
        return Left() < o.Right() && o.Left() < Right() && Top() < o.Bottom() && o.Top() < Bottom();
    }

    void Write(Writer &w) const override { w.F32(position.x); w.F32(position.y); w.F32(size.x); w.F32(size.y); }
    void Read(Reader &r) override { position.x = r.F32(); position.y = r.F32(); size.x = r.F32(); size.y = r.F32(); }
};

// Something the physics system moves.
struct RigidBody : Component {
    ENGINE_COMPONENT(RigidBody)
    Vec2 velocity;
    bool gravity = false;

    // Set by the physics system each step.
    bool grounded = false;
    ObjectId ground = NO_OBJECT;   // runtime only: what it stands on
    Vec2 groundLast;               // runtime only: where that was, so a moving platform carries its riders

    void Write(Writer &w) const override { w.F32(velocity.x); w.F32(velocity.y); w.Bool(gravity); w.Bool(grounded); }
    void Read(Reader &r) override { velocity.x = r.F32(); velocity.y = r.F32(); gravity = r.Bool(); grounded = r.Bool(); }
};

// How the box takes part in collision.
//   Solid    movers cannot enter it; with oneWay, only from above (a ledge you
//            can jump up through and land on).
//   Trigger  movers pass through; systems ask who is inside (death zones,
//            scroll boundaries).
struct Collider : Component {
    ENGINE_COMPONENT(Collider)
    enum class Type : uint8_t { Solid, Trigger };
    Type type = Type::Solid;
    bool oneWay = false;

    Collider() = default;
    explicit Collider(Type t, bool oneWayLedge = false) : type(t), oneWay(oneWayLedge) {}

    void Write(Writer &w) const override { w.U8(static_cast<uint8_t>(type)); w.Bool(oneWay); }
    void Read(Reader &r) override { type = r.U8() == 1 ? Type::Trigger : Type::Solid; oneWay = r.Bool(); }
};

// How to draw it. An object without one, or with visible == false, is
// hidden - it still collides, triggers and travels like any other object.
// `sprite` names a file under media/ (TextureCache loads it on first use);
// empty means a flat rectangle in `color`, which also tints a sprite.
struct Renderable : Component {
    ENGINE_COMPONENT(Renderable)
    SDL_Color color = {255, 255, 255, 255};
    bool visible = true;
    bool flipX = false;   // mirror the sprite left-right: art drawn facing right, now facing left
    int layer = 0;   // lower layers are drawn first
    std::string sprite;
    int frames = 1;
    int frameWidth = 0;
    int frameHeight = 0;
    int frameMs = 100;

    Renderable() = default;
    explicit Renderable(SDL_Color c, int drawLayer = 0) : color(c), layer(drawLayer) {}

    void Write(Writer &w) const override {
        w.U8(color.r); w.U8(color.g); w.U8(color.b); w.U8(color.a);
        w.Bool(visible); w.Bool(flipX); w.I32(layer); w.Str(sprite);
        w.U16(static_cast<uint16_t>(frames)); w.U16(static_cast<uint16_t>(frameWidth));
        w.U16(static_cast<uint16_t>(frameHeight)); w.U16(static_cast<uint16_t>(frameMs));
    }
    void Read(Reader &r) override {
        color.r = r.U8(); color.g = r.U8(); color.b = r.U8(); color.a = r.U8();
        visible = r.Bool(); flipX = r.Bool(); layer = r.I32(); sprite = r.Str();
        frames = r.U16(); frameWidth = r.U16(); frameHeight = r.U16(); frameMs = r.U16();
    }
};

// The buttons a player is holding, as a bit set. The local client fills it
// from the keyboard; everything downstream reads only this. That is what lets
// an experiment send inputs instead of positions: this one small component is
// the whole of a player's intent.
struct InputState : Component {
    ENGINE_COMPONENT(InputState)
    enum Button : uint32_t { Left = 1u, Right = 2u, Up = 4u, Down = 8u, Jump = 16u, Action = 32u };
    uint32_t buttons = 0;

    bool Held(Button b) const { return (buttons & b) != 0; }
    void Set(Button b, bool down) { buttons = down ? (buttons | b) : (buttons & ~static_cast<uint32_t>(b)); }

    void Write(Writer &w) const override { w.U32(buttons); }
    void Read(Reader &r) override { buttons = r.U32(); }
};

// Turns InputState into platformer motion: run left and right, jump when on
// the ground. A jump needs a fresh press, so holding the key does not bounce.
// `facing` follows the last direction run (1 right, -1 left); with
// `flipSprite`, the object's Renderable is mirrored to match, so sprites drawn
// facing right turn round on their own.
struct PlatformerController : Component {
    ENGINE_COMPONENT(PlatformerController)
    float moveSpeed = 320.0f;
    float jumpSpeed = 760.0f;
    int facing = 1;
    bool flipSprite = true;

    bool jumpLatched = false;   // runtime only: Jump was held last step

    void Write(Writer &w) const override { w.F32(moveSpeed); w.F32(jumpSpeed); w.I32(facing); w.Bool(flipSprite); }
    void Read(Reader &r) override { moveSpeed = r.F32(); jumpSpeed = r.F32(); facing = r.I32(); flipSprite = r.Bool(); }
};

// Moves its object along a fixed pattern as a pure function of a clock, the
// way Maze Run's patrols did: the position at time t is computed, never
// accumulated, so it cannot drift and any endpoint holding the same clock
// computes the same place.
//   Linear    back and forth between origin and origin + offset, eased at the
//             ends. offset (d, 0) is horizontal, (0, d) vertical.
//   Circular  around origin at `radius`.
// `period` is seconds per full cycle; `phase` (0..1) offsets objects that
// share a pattern.
struct PathMover : Component {
    ENGINE_COMPONENT(PathMover)
    enum class Pattern : uint8_t { Linear, Circular };
    Pattern pattern = Pattern::Linear;
    Vec2 origin;
    Vec2 offset;
    float radius = 0.0f;
    float period = 4.0f;
    float phase = 0.0f;

    static constexpr float TWO_PI = 6.28318530718f;

    Vec2 PositionAt(double seconds) const {
        const double cycles = (period > 0.0f ? seconds / period : 0.0) + phase;
        const double angle = TWO_PI * (cycles - std::floor(cycles));
        if (pattern == Pattern::Circular) {
            return {origin.x + radius * static_cast<float>(std::cos(angle)),
                    origin.y + radius * static_cast<float>(std::sin(angle))};
        }
        const float s = 0.5f - 0.5f * static_cast<float>(std::cos(angle));   // 0 -> 1 -> 0, eased
        return {origin.x + offset.x * s, origin.y + offset.y * s};
    }

    void Write(Writer &w) const override {
        w.U8(static_cast<uint8_t>(pattern));
        w.F32(origin.x); w.F32(origin.y); w.F32(offset.x); w.F32(offset.y);
        w.F32(radius); w.F32(period); w.F32(phase);
    }
    void Read(Reader &r) override {
        pattern = r.U8() == 1 ? Pattern::Circular : Pattern::Linear;
        origin.x = r.F32(); origin.y = r.F32(); offset.x = r.F32(); offset.y = r.F32();
        radius = r.F32(); period = r.F32(); phase = r.F32();
    }
};

// A hidden marker: a place a player can (re)appear. Several may exist;
// `index` lets a game send a player to a particular one.
struct SpawnPoint : Component {
    ENGINE_COMPONENT(SpawnPoint)
    int index = 0;

    SpawnPoint() = default;
    explicit SpawnPoint(int i) : index(i) {}

    void Write(Writer &w) const override { w.I32(index); }
    void Read(Reader &r) override { index = r.I32(); }
};

// A hidden boundary: anything with a Respawn component that overlaps it is
// sent back to a spawn point. Needs a Transform (the area).
struct DeathZone : Component {
    ENGINE_COMPONENT(DeathZone)
};

// Marks objects death zones act on, and where they go back to: the spawn
// point with this index, or the lowest-indexed one if there is no match.
struct Respawn : Component {
    ENGINE_COMPONENT(Respawn)
    int spawnIndex = 0;
    int deaths = 0;

    void Write(Writer &w) const override { w.I32(spawnIndex); w.I32(deaths); }
    void Read(Reader &r) override { spawnIndex = r.I32(); deaths = r.I32(); }
};

// What a view looks at. `position` is the world point at the top-left of the
// view; `view` is how much world it shows. Usually local to one client - each
// player has their own - so it does not travel.
struct Camera : Component {
    Vec2 position;
    Vec2 view = {1920.0f, 1080.0f};
    ObjectId target = NO_OBJECT;
    // The camera never shows outside these, when max > min.
    Vec2 boundsMin;
    Vec2 boundsMax;
};

// A hidden box that rides along with a camera, `inset` world units in from
// one edge of its view. When the camera's target touches it, the camera - and
// so the whole scene on screen - shifts sideways by exactly as much as the
// target pushed in: the side-scrolling boundary. Needs a Transform.
struct ScrollBoundary : Component {
    enum class Side : uint8_t { Left, Right, Top, Bottom };
    Side side = Side::Right;
    float inset = 400.0f;
    float thickness = 40.0f;
    ObjectId camera = NO_OBJECT;

    ScrollBoundary() = default;
    ScrollBoundary(ObjectId cam, Side s, float in) : side(s), inset(in), camera(cam) {}
};

// Marks an object as part of the networked scene and says who is
// authoritative for it: -1 the server (or, between peers, nobody - the
// object is computed identically everywhere), otherwise the client or peer id
// that owns it. `stampMicros` is when its owner last sampled it, on the
// machine's steady clock - every endpoint on one host reads the same clock,
// so now minus stamp is a true one-way latency.
struct NetIdentity : Component {
    ENGINE_COMPONENT(NetIdentity)
    int owner = -1;
    int64_t stampMicros = 0;

    NetIdentity() = default;
    explicit NetIdentity(int ownerId) : owner(ownerId) {}

    void Write(Writer &w) const override { w.I32(owner); w.I64(stampMicros); }
    void Read(Reader &r) override { owner = r.I32(); stampMicros = r.I64(); }
};

} // namespace engine
