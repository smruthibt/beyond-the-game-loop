#include "Game.hpp"

#include <algorithm>
#include <cmath>
#include <random>
#include <vector>

namespace clockwork {
using namespace engine;

void Racer::Write(Writer &w) const {
    w.F32(elapsed); w.F32(countdown); w.F32(protection); w.I32(checkpoint); w.Bool(finished);
}
void Racer::Read(Reader &r) {
    elapsed = r.F32(); countdown = r.F32(); protection = r.F32(); checkpoint = r.I32(); finished = r.Bool();
}
void Hazard::Write(Writer &w) const {
    w.U8(static_cast<uint8_t>(type)); w.U8(static_cast<uint8_t>(phase)); w.F32(offset); w.F32(angle);
}
void Hazard::Read(Reader &r) {
    type = static_cast<Type>(r.U8()); phase = static_cast<Phase>(r.U8()); offset = r.F32(); angle = r.F32();
}

void RegisterComponents() {
    ComponentRegistry::RegisterBuiltins();
    ComponentRegistry::Register<Racer>();
    ComponentRegistry::Register<Checkpoint>();
    ComponentRegistry::Register<Hazard>();
    ComponentRegistry::Register<Finish>();
    ComponentRegistry::Register<Impact>();
    ComponentRegistry::Register<PotionPlatform>();
    ComponentRegistry::Register<Boost>();
    ComponentRegistry::Register<PeerSession>();
    ComponentRegistry::Register<Knockback>();
}

Transform PotionArea(const Transform &platform) {
    return {platform.Center().x - 14, platform.Top() - 36, 28, 34};
}

SDL_Color PlayerColor(int owner) {
    const SDL_Color colors[] = {{255, 110, 80, 255}, {60, 214, 255, 255}, {160, 240, 90, 255}, {255, 200, 60, 255}};
    return colors[static_cast<unsigned>(owner) % 4];
}

namespace {
constexpr float HEAD_BUMP = 260; // downward shove after rising into a ledge
constexpr float RUN_SPEED = 400;
constexpr float BOOST_SPEED = 1.5f;  // running on the ground only, so jumps keep their reach
constexpr float BOOST_SCALE = 1.25f;
constexpr float STOMP_BOUNCE = 520;
// A fall covers at most maxFallSpeed / 120 (about 12px) per step, and the runner
// underneath may be rising too; a stomp is a stomper's feet inside this band.
constexpr float STOMP_WINDOW = 20;

GameObject &Box(World &w, const char *kind, float x, float y, float width, float height, SDL_Color color) {
    GameObject &o = w.Create(kind);
    o.Add<Transform>(x, y, width, height);
    o.Add<Renderable>(color, 1);
    o.Add<NetIdentity>();
    return o;
}
void Spawn(World &w, int index, float x) {
    GameObject &o = w.Create("spawn");
    o.Add<Transform>(x, FLOOR_Y - 60, 44, 60);
    o.Add<SpawnPoint>(index);
    o.Add<NetIdentity>(); // deliberately no Renderable
}
void MovingPlatform(World &w, float x, float y, float dx, float dy, float period, float radius = 0) {
    auto &o = Box(w, "mover", x, y, 180, 26, {255, 190, 80, 255});
    o.Add<Collider>(Collider::Type::Solid, true);
    auto &p = o.Add<PathMover>();
    p.origin = {x, y}; p.offset = {dx, dy}; p.period = period;
    if (radius > 0) { p.pattern = PathMover::Pattern::Circular; p.radius = radius; }
}
void Saw(World &w, float x, float travel, float period) {
    auto &o = Box(w, "saw", x, FLOOR_Y - 68, 68, 68, {255, 64, 129, 255});
    o.Add<Hazard>();
    o.Add<Collider>(Collider::Type::Trigger);
    auto &p = o.Add<PathMover>();
    p.origin = {x, FLOOR_Y - 68}; p.offset = {travel, 0}; p.period = period;
}
void Barrier(World &w, float x, float offset) {
    auto &o = Box(w, "electric", x, FLOOR_Y - 190, 38, 190, {255, 64, 129, 255});
    auto &h = o.Add<Hazard>(); h.type = Hazard::Type::Electric; h.offset = offset;
    o.Add<Collider>(Collider::Type::Trigger);
}
bool Running(const GameObject &o) {
    const auto *r = o.Get<Racer>();
    return r && r->countdown <= 0 && !r->finished;
}
// Resizes a runner about its feet, so growing never sinks it into what it stands on.
void Resize(GameObject &p, float scale) {
    auto &t = *p.Get<Transform>();
    const float width = RUNNER_WIDTH * scale, height = RUNNER_HEIGHT * scale;
    t.position.x += (t.size.x - width) / 2;
    t.position.y += t.size.y - height;
    t.size = {width, height};
}
void EndBoost(GameObject &p) {
    if (auto *b = p.Get<Boost>()) b->remaining = 0;
    if (auto *session = p.Get<PeerSession>()) session->boostToken = 0;
    Resize(p, 1);
}
// Respawns, restarts and finishing end any push back and boost.
void ClearEffects(GameObject &p) {
    if (auto *k = p.Get<Knockback>()) k->remaining = k->cooldown = 0;
    if (auto *b = p.Get<Boost>()) b->stomped = 0;
    EndBoost(p);
}
// Static ledges are solid all round: land on them from above, slide up their sides,
// and bump your head on them from below. Getting on top takes a sideways jump.
GameObject &Ledge(World &w, float x, float y, float width, float height, SDL_Color color) {
    auto &o = Box(w, "platform", x, y, width, height, color);
    o.Add<Collider>(Collider::Type::Solid);
    o.Add<Impact>();
    return o;
}
// Physics stops a rising runner flush beneath a ledge; this turns that into push back,
// like Mario hitting a block: the runner is shoved straight back down and the ledge pops.
void ResolveHeadBumps(World &w, GameObject &p, const CourseEvents &events) {
    auto *k = p.Get<Knockback>();
    auto &body = *p.Get<RigidBody>();
    if (!k || k->remaining > 0 || k->cooldown > 0 || body.grounded) return;
    const auto &t = *p.Get<Transform>();
    GameObject *struck = nullptr;
    w.Each<Transform, Impact>([&](GameObject &o, Transform &ledge, Impact &) {
        if (!struck && std::abs(t.Top() - ledge.Bottom()) < 1 && t.Left() < ledge.Right() && ledge.Left() < t.Right())
            struck = &o;
    });
    if (!struck) return;
    k->remaining = 0.18f;
    k->cooldown = 0.3f;
    body.velocity = {0, HEAD_BUMP};
    body.ground = NO_OBJECT;
    struck->Get<Impact>()->amount = 1;
    if (auto *potion = struck->Get<PotionPlatform>()) potion->opened = true;
    if (events.bump) events.bump(*struck);
}
// The first runner to touch an opened platform's potion drinks it.
void ResolvePotions(World &w, GameObject &p, const CourseEvents &events) {
    const auto &t = *p.Get<Transform>();
    w.Each<Transform, PotionPlatform>([&](GameObject &ledge, Transform &platform, PotionPlatform &potion) {
        if (!potion.opened || potion.taken || !t.Overlaps(PotionArea(platform))) return;
        if (events.drink) { events.drink(ledge, p); return; }
        potion.taken = true;
        GiveBoost(p);
    });
}
// Runners pass through each other, except that one falling onto another's head is a
// stomp, as in Mario: the stomper bounces off, and the runner underneath loses any boost.
void ResolveStomps(World &w, const ObjectFilter &authority, const CourseEvents &events) {
    std::vector<GameObject *> runners;
    w.Each<Racer>([&](GameObject &o, Racer &) { if (Running(o)) runners.push_back(&o); });
    for (auto *stomper : runners) {
        if (authority && !authority(*stomper)) continue;
        auto &body = *stomper->Get<RigidBody>();
        if (body.velocity.y <= 0) continue;
        const auto &feet = *stomper->Get<Transform>();
        for (auto *under : runners) {
            const auto &head = *under->Get<Transform>();
            if (under == stomper || feet.Bottom() < head.Top() || feet.Bottom() > head.Top() + STOMP_WINDOW ||
                feet.Right() <= head.Left() || head.Right() <= feet.Left()) continue;
            body.velocity.y = -STOMP_BOUNCE;
            body.grounded = false;
            body.ground = NO_OBJECT;
            if (events.stomp) { events.stomp(*stomper, *under); break; }
            auto &boost = *under->Get<Boost>();
            if (boost.remaining > 0) { EndBoost(*under); boost.stomped = 0.6f; }
            break;
        }
    }
}
}

void GiveBoost(GameObject &p, float remaining) {
    if (p.Get<Boost>()->remaining <= 0) Resize(p, BOOST_SCALE);
    p.Get<Boost>()->remaining = remaining;
}

void RemoveBoost(GameObject &p) { EndBoost(p); }

void BuildCourse(World &w) {
    // Four distinct workshop decks separated by pits that require timing jumps.
    const float decks[][2] = {{0,1500}, {1920,1260}, {3740,1240}, {5480,920}};
    for (auto &d : decks) Box(w, "ground", d[0], FLOOR_Y, d[1], 160, {28, 30, 62, 255}).Add<Collider>();

    Ledge(w, 330, 600, 230, 28, {52, 230, 196, 255});
    Ledge(w, 780, 540, 160, 40, {96, 165, 250, 255});
    // One ledge per deck that a runner can jump onto straight from the floor hides a
    // diamond potion.
    Ledge(w, 1310, 600, 140, 22, {167, 139, 250, 255}).Add<PotionPlatform>();
    // A jump rises jumpSpeed^2 / (2 * gravity) = 850^2 / 3600, about 200px; steps
    // between ledges stay at 120-140px. No ledge overhangs another, so every step up
    // is a sideways jump.
    Ledge(w, 2200, 600, 220, 28, {52, 230, 196, 255}).Add<PotionPlatform>();
    Ledge(w, 2460, 480, 160, 28, {96, 165, 250, 255});
    Ledge(w, 2920, 600, 180, 22, {167, 139, 250, 255});
    Ledge(w, 4060, 605, 170, 28, {52, 230, 196, 255}).Add<PotionPlatform>();
    Ledge(w, 4260, 485, 140, 30, {96, 165, 250, 255});
    Ledge(w, 4820, 600, 140, 22, {167, 139, 250, 255});

    MovingPlatform(w, 1510, 700, 230, 0, 4);
    MovingPlatform(w, 3160, 690, 0, -180, 4.8f);
    MovingPlatform(w, 3450, 620, 0, 0, 5.5f, 85);
    MovingPlatform(w, 4990, 690, 170, 0, 4);
    MovingPlatform(w, 5310, 610, 0, 110, 3.6f);

    for (int i = 0; i < 4; ++i) Spawn(w, i, 90.0f + 56 * i);
    const float checks[] = {2020, 3860, 5540};
    for (int i = 0; i < CHECKPOINTS; ++i) {
        Spawn(w, 101 + i, checks[i] + 50);
        auto &o = w.Create("checkpoint");
        o.Add<Transform>(checks[i], FLOOR_Y - 210, 140, 210);
        o.Add<NetIdentity>(); // invisible progress trigger; no Renderable
        auto &c = o.Add<Checkpoint>(); c.order = i + 1; c.spawn = 101 + i;
        o.Add<Collider>(Collider::Type::Trigger);
    }

    Saw(w, 640, 160, 4);
    Saw(w, 2280, 200, 4.6f);
    Saw(w, 4100, 160, 3.8f);
    Saw(w, 5820, 180, 3.6f);
    Barrier(w, 1140, 0);
    Barrier(w, 2960, 1.6f);
    Barrier(w, 4700, 3.2f);

    auto &pit = w.Create("death-zone");
    pit.Add<Transform>(-600, LEVEL_HEIGHT + 20, LEVEL_WIDTH + 1200, 800);
    pit.Add<Collider>(Collider::Type::Trigger);
    pit.Add<DeathZone>(); pit.Add<NetIdentity>();

    auto &finish = Box(w, "finish", 6180, FLOOR_Y - 240, 170, 240, {255, 190, 80, 255});
    finish.Add<Finish>(); finish.Add<Collider>(Collider::Type::Trigger);
    systems::MovePaths(w, 0);
    UpdateHazards(w, 0);
}

void AddBenchObjects(World &w, int moving, int statics) {
    // A fixed seed, so both sides of a comparison simulate and send the same scene.
    // Heights stay above the spawns (y 680) even at the bottom of a vertical path.
    std::mt19937 rng(481);
    std::uniform_real_distribution<float> across(0, LEVEL_WIDTH - 260), high(140, 520), period(3, 6);
    const SDL_Color colors[] = {{52, 230, 196, 255}, {96, 165, 250, 255}, {167, 139, 250, 255}};
    for (int i = 0; i < statics; ++i) {
        const float x = across(rng), y = high(rng);
        Ledge(w, x, y, 140.0f + 20 * (i % 5), 22.0f + 2 * (i % 4), colors[i % 3]);
    }
    for (int i = 0; i < moving; ++i) {
        const float x = across(rng), y = high(rng), seconds = period(rng);
        switch (i % 3) { // the course's three patterns
        case 0: MovingPlatform(w, x, y, 200, 0, seconds); break;
        case 1: MovingPlatform(w, x, y, 0, 120, seconds); break;
        default: MovingPlatform(w, x, y, 0, 0, seconds, 80); break;
        }
    }
    systems::MovePaths(w, 0);
}

GameObject &AddPlayer(World &w, int owner) {
    auto &p = w.Create("player");
    p.Add<Transform>(0, 0, RUNNER_WIDTH, RUNNER_HEIGHT);
    p.Add<RigidBody>().gravity = true;
    p.Add<InputState>();
    auto &controller = p.Add<PlatformerController>(); controller.moveSpeed = RUN_SPEED; controller.jumpSpeed = 850;
    p.Add<Renderable>(PlayerColor(owner), 3);
    p.Add<NetIdentity>(owner);
    p.Add<Respawn>(); p.Add<Racer>();
    p.Add<Knockback>();
    p.Add<Boost>();
    Restart(w, p);
    return p;
}

GameObject *Player(World &w, int owner) {
    GameObject *result = nullptr;
    w.Each<Racer, NetIdentity>([&](GameObject &o, Racer &, NetIdentity &n) { if (n.owner == owner) result = &o; });
    return result;
}

void Restart(World &w, GameObject &p) {
    if (auto *session = p.Get<PeerSession>()) ++session->run;
    auto &r = *p.Get<Racer>();
    r.elapsed = 0; r.countdown = 3; r.protection = 0; r.checkpoint = 0; r.finished = false;
    auto &respawn = *p.Get<Respawn>(); respawn.spawnIndex = OwnerOf(p) % 4; respawn.deaths = 0;
    p.Get<PlatformerController>()->jumpLatched = false;
    ClearEffects(p);
    systems::PlaceAtSpawn(w, p, respawn.spawnIndex);
}

void UpdateHazards(World &w, double seconds) {
    w.Each<Hazard>([&](GameObject &, Hazard &h) {
        h.angle = static_cast<float>(std::fmod(seconds * 4.0, PathMover::TWO_PI));
        if (h.type == Hazard::Type::Saw) { h.phase = Hazard::Phase::Active; return; }
        const double t = std::fmod(seconds + h.offset, 5.0);
        h.phase = t < 2 ? Hazard::Phase::Off : t < 3 ? Hazard::Phase::Warning : Hazard::Phase::Active;
    });
}

bool HitsHazard(const Transform &player, const Transform &area, const Hazard &h) {
    if (h.phase != Hazard::Phase::Active) return false;
    if (h.type == Hazard::Type::Electric) return player.Overlaps(area);
    const Vec2 center = area.Center();
    const float dx = center.x - std::clamp(center.x, player.Left(), player.Right());
    const float dy = center.y - std::clamp(center.y, player.Top(), player.Bottom());
    const float radius = std::min(area.size.x, area.size.y) * 0.5f;
    return dx * dx + dy * dy < radius * radius;
}

void ResolveCourse(World &w, const ObjectFilter &authority, const CourseEvents &events) {
    // The built-in death-zone system handles pits and resets velocity/grounding.
    const auto pitDeaths = systems::ApplyDeathZones(w, [&](const GameObject &o) { return Running(o) && (!authority || authority(o)); });
    for (auto &event : pitDeaths) {
        auto &p = *w.Find(event.object);
        p.Get<Racer>()->protection = 0.75f;
        ClearEffects(p);
    }

    w.Each<Racer, Transform, Respawn>([&](GameObject &p, Racer &r, Transform &t, Respawn &respawn) {
        if (!Running(p) || (authority && !authority(p))) return;
        bool hit = false;
        if (r.protection <= 0) {
            w.Each<Transform, Hazard>([&](GameObject &, Transform &area, Hazard &h) {
                hit = hit || HitsHazard(t, area, h);
            });
        }
        if (hit) {
            if (systems::PlaceAtSpawn(w, p, respawn.spawnIndex)) {
                ++respawn.deaths; r.protection = 0.75f; ClearEffects(p);
            }
            return; // at most one death per player per step
        }
        w.Each<Transform, Checkpoint>([&](GameObject &, Transform &area, Checkpoint &c) {
            if (c.order == r.checkpoint + 1 && t.Overlaps(area)) { r.checkpoint = c.order; respawn.spawnIndex = c.spawn; }
        });
        if (r.checkpoint == CHECKPOINTS) {
            w.Each<Transform, Finish>([&](GameObject &, Transform &area, Finish &) {
                if (t.Overlaps(area)) { r.finished = true; p.Get<RigidBody>()->velocity = {0, 0}; }
            });
        }
        if (r.finished) { ClearEffects(p); return; }
        ResolveHeadBumps(w, p, events);
        ResolvePotions(w, p, events);
    });
    ResolveStomps(w, authority, events);
}

void Step(World &w, float dt, double &seconds, const ObjectFilter &authority, const CourseEvents &events) {
    // Small game steps also prevent fast players from crossing a hazard between
    // overlap checks. The engine physics retains its own substeps and collisions.
    const int steps = std::max(1, static_cast<int>(std::ceil(dt * 120)));
    const float h = dt / steps;
    for (int i = 0; i < steps; ++i) {
        seconds += h;
        systems::MovePaths(w, seconds);
        UpdateHazards(w, seconds);
        w.Each<Impact>([&](GameObject &, Impact &hit) { hit.amount = std::max(0.0f, hit.amount - h * 4); });
        w.Each<Knockback>([&](GameObject &p, Knockback &k) {
            if (authority && !authority(p)) return;
            k.remaining = std::max(0.0f, k.remaining - h);
            k.cooldown = std::max(0.0f, k.cooldown - h);
        });
        w.Each<Boost>([&](GameObject &p, Boost &b) {
            if (authority && !authority(p)) return;
            b.stomped = std::max(0.0f, b.stomped - h);
            if (b.remaining > 0 && (b.remaining -= h) <= 0) EndBoost(p);
        });
        // The boost speeds up running on the ground only; in the air a jump keeps its
        // normal reach, so pits still need the moving platforms.
        w.Each<Boost, PlatformerController, RigidBody>([&](GameObject &p, Boost &b, PlatformerController &pc, RigidBody &body) {
            if (authority && !authority(p)) return;
            pc.moveSpeed = b.remaining > 0 && body.grounded ? RUN_SPEED * BOOST_SPEED : RUN_SPEED;
        });
        w.Each<Racer, InputState>([&](GameObject &p, Racer &r, InputState &in) {
            if (authority && !authority(p)) return;
            const bool restart = (in.buttons & RESTART) != 0;
            if (restart && !r.restartHeld) Restart(w, p);
            r.restartHeld = restart;
            r.protection = std::max(0.0f, r.protection - h);
            if (r.countdown > 0) r.countdown = std::max(0.0f, r.countdown - h);
            else if (!r.finished) r.elapsed += h;
        });
        systems::ApplyControllers(w, [&](const GameObject &o) {
            const auto *k = o.Get<Knockback>();
            return (!authority || authority(o)) && Running(o) && (!k || k->remaining <= 0);
        });
        // The controller normally writes horizontal velocity every tick; a bumped
        // runner instead drops straight down under the engine's gravity and collisions.
        w.Each<Knockback, RigidBody>([&](GameObject &p, Knockback &k, RigidBody &body) {
            if ((!authority || authority(p)) && Running(p) && k.remaining > 0) body.velocity.x = 0;
        });
        systems::StepPhysics(w, h, PhysicsSettings{}, [&](const GameObject &o) { return Running(o) && (!authority || authority(o)); });
        w.Each<Racer, Transform>([&](GameObject &p, Racer &, Transform &t) {
            if (authority && !authority(p)) return;
            t.position.x = std::clamp(t.position.x, 0.0f, LEVEL_WIDTH - t.size.x);
        });
        ResolveCourse(w, authority, events);
    }
}

NetOptions NetworkOptions() {
    NetOptions options;
    options.upstream = Upstream::Input; // all game decisions belong to the server
    options.encoding = Encoding::Binary;
    return options;
}
Server::Config ServerConfig(int port) {
    Server::Config c;
    c.joinPort = port; c.worldPort = port + 1; c.clientPortBase = port + 100;
    c.maxClients = 0; c.tickHz = 120; c.publishEverySteps = 2; c.logTag = "clockwork";
    return c;
}
Client::Config ClientConfig(int port) {
    Client::Config c; c.joinPort = port; c.worldPort = port + 1; return c;
}
} // namespace clockwork
