#pragma once

#include "engine/NetScene.hpp"

namespace clockwork {

constexpr float LEVEL_WIDTH = 6400.0f;
constexpr float LEVEL_HEIGHT = 900.0f;
constexpr float FLOOR_Y = 740.0f;
constexpr int CHECKPOINTS = 3;
constexpr int DEFAULT_PORT = 8650;
constexpr uint32_t RESTART = 1u << 8;
constexpr float RUNNER_WIDTH = 44.0f;
constexpr float RUNNER_HEIGHT = 60.0f;
constexpr float BOOST_SECONDS = 7.0f;

// Game-owned components extend the engine registry; engine code is unchanged.
struct Racer : engine::Component {
    ENGINE_COMPONENT(Racer)
    float elapsed = 0;
    float countdown = 3;
    float protection = 0;
    int checkpoint = 0;
    bool finished = false;
    bool restartHeld = false; // server-only edge latch
    void Write(engine::Writer &w) const override;
    void Read(engine::Reader &r) override;
};

struct Checkpoint : engine::Component {
    ENGINE_COMPONENT(Checkpoint)
    int order = 1;
    int spawn = 101;
    void Write(engine::Writer &w) const override { w.I32(order); w.I32(spawn); }
    void Read(engine::Reader &r) override { order = r.I32(); spawn = r.I32(); }
};

struct Hazard : engine::Component {
    ENGINE_COMPONENT(Hazard)
    enum class Type : uint8_t { Saw, Electric };
    enum class Phase : uint8_t { Off, Warning, Active };
    Type type = Type::Saw;
    Phase phase = Phase::Active;
    float offset = 0;
    float angle = 0;
    void Write(engine::Writer &w) const override;
    void Read(engine::Reader &r) override;
};

struct Finish : engine::Component { ENGINE_COMPONENT(Finish) };

// A recent head bump on a ledge, replicated so every client shows the pop.
struct Impact : engine::Component {
    ENGINE_COMPONENT(Impact)
    float amount = 0;
    void Write(engine::Writer &w) const override { w.F32(amount); }
    void Read(engine::Reader &r) override { amount = r.F32(); }
};

// A ledge drawn as bricks until a runner first bumps it from below. That opens it for
// everyone and puts one diamond potion on top, for whichever runner touches it first.
struct PotionPlatform : engine::Component {
    ENGINE_COMPONENT(PotionPlatform)
    bool opened = false;
    bool taken = false;
    void Write(engine::Writer &w) const override { w.Bool(opened); w.Bool(taken); }
    void Read(engine::Reader &r) override { opened = r.Bool(); taken = r.Bool(); }
};

// The diamond potion's effect: faster running on the ground and a bigger body, for
// BOOST_SECONDS, ended early by dying, restarting or being stomped.
struct Boost : engine::Component {
    ENGINE_COMPONENT(Boost)
    float remaining = 0; // seconds left; 0 = a normal runner
    float stomped = 0;   // brief marker after losing the boost to a stomp
    void Write(engine::Writer &w) const override { w.F32(remaining); w.F32(stomped); }
    void Read(engine::Reader &r) override { remaining = r.F32(); stomped = r.F32(); }
};

// Identifies a peer process and personal run, so delayed shared events cannot
// affect a replacement player or a new run in the same slot.
struct PeerSession : engine::Component {
    ENGINE_COMPONENT(PeerSession)
    int64_t session = 0;
    int run = 0;
    int64_t boostToken = 0;
    void Write(engine::Writer &w) const override { w.I64(session); w.I32(run); w.I64(boostToken); }
    void Read(engine::Reader &r) override { session = r.I64(); run = r.I32(); boostToken = r.I64(); }
};

struct CourseEvents {
    std::function<void(engine::GameObject &)> bump;
    std::function<void(engine::GameObject &, engine::GameObject &)> drink;
    std::function<void(engine::GameObject &, engine::GameObject &)> stomp;
};

// Push back after a head bump: input is ignored briefly and the runner drops straight down.
struct Knockback : engine::Component {
    ENGINE_COMPONENT(Knockback)
    float remaining = 0;
    float cooldown = 0;
    void Write(engine::Writer &w) const override { w.F32(remaining); w.F32(cooldown); }
    void Read(engine::Reader &r) override { remaining = r.F32(); cooldown = r.F32(); }
};

void RegisterComponents();
void BuildCourse(engine::World &world);
// Benchmark scaling: adds `moving` moving platforms and `statics` ledges, built exactly
// like the course's own, at the same generated positions every run.
void AddBenchObjects(engine::World &world, int moving, int statics);
engine::GameObject &AddPlayer(engine::World &world, int owner);
engine::GameObject *Player(engine::World &world, int owner);
void Restart(engine::World &world, engine::GameObject &player);
void UpdateHazards(engine::World &world, double seconds);
bool HitsHazard(const engine::Transform &player, const engine::Transform &area, const Hazard &hazard);
void GiveBoost(engine::GameObject &player, float remaining = BOOST_SECONDS);
void RemoveBoost(engine::GameObject &player);
void ResolveCourse(engine::World &world, const engine::ObjectFilter &authority = {}, const CourseEvents &events = {});
void Step(engine::World &world, float dt, double &seconds,
          const engine::ObjectFilter &authority = {}, const CourseEvents &events = {});
engine::NetOptions NetworkOptions();
engine::Server::Config ServerConfig(int port);
engine::Client::Config ClientConfig(int port);
SDL_Color PlayerColor(int owner);
// Where an opened potion platform's potion sits: centred on top of it.
engine::Transform PotionArea(const engine::Transform &platform);

// Renders game art from the same engine components used by physics/networking.
void Draw(engine::World &world, SDL_Renderer *renderer, const engine::Camera &camera,
          int owner, bool connected, bool practice, double seconds, float respawnFlash, bool peer = false);

} // namespace clockwork
