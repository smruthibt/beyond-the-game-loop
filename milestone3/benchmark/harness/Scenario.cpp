#include "Scenario.hpp"

#include "Game.hpp"
#include "engine/Components.hpp"

#include <algorithm>

namespace rb {

using namespace engine;

std::vector<uint32_t> MakeTrace(int seed, int owner, int blocks) {
    // Segments of running right, left or standing (0.25-3 s), with jump
    // presses held 2-10 blocks and always released before the next press.
    Rng r(0x7EACE000ull ^ (static_cast<uint64_t>(seed) * 0x9E3779B97F4A7C15ull) ^
          (static_cast<uint64_t>(owner + 1) * 0xD1B54A32D192ED03ull));
    std::vector<uint32_t> trace(static_cast<size_t>(blocks) + 1, 0);
    uint32_t dir = 0;
    int segment = 0, hold = 0, rest = 0;
    for (int b = 1; b <= blocks; ++b) {
        if (segment == 0) {
            const uint32_t u = r.Below(100);
            dir = u < 45 ? InputState::Right : u < 80 ? InputState::Left : 0u;
            segment = 15 + static_cast<int>(r.Below(166));
        }
        --segment;
        uint32_t buttons = dir;
        if (hold == 0 && rest == 0 && r.Below(12) == 0) hold = 2 + static_cast<int>(r.Below(9));
        if (hold > 0) {
            buttons |= InputState::Jump;
            if (--hold == 0) rest = 1 + static_cast<int>(r.Below(3));
        } else if (rest > 0) {
            --rest;
        }
        trace[static_cast<size_t>(b)] = buttons;
    }
    return trace;
}

Scenario::Scenario(const Options &options) : opts(options) {
    for (int p = 0; p < opts.players; ++p) traces.push_back(MakeTrace(opts.seed, p, opts.FinalBlock()));
}

uint32_t Scenario::Buttons(int owner, long long block) const {
    const auto &t = traces.at(static_cast<size_t>(owner));
    return block >= 1 && block < static_cast<long long>(t.size()) ? t[static_cast<size_t>(block)] : 0u;
}

uint64_t Scenario::TraceHash() const {
    uint64_t h = 1469598103934665603ull;
    for (const auto &t : traces) {
        h = Fnv64(std::string(reinterpret_cast<const char *>(t.data()), t.size() * sizeof(uint32_t)), h);
    }
    return h;
}

std::string Scenario::Counts(const World &world) const {
    long long total = 0, networked = 0, players = 0, moving = 0, solids = 0, spawns = 0, deathZones = 0, other = 0;
    world.Each<Transform>([&](const GameObject &o, const Transform &) {
        ++total;
        if (o.Has<NetIdentity>()) ++networked;
        if (o.Has<InputState>()) ++players;
        else if (o.Has<PathMover>()) ++moving;
        else if (o.Has<SpawnPoint>()) ++spawns;
        else if (o.Has<DeathZone>()) ++deathZones;
        else if (const Collider *c = o.Get<Collider>(); c && c->type == Collider::Type::Solid) ++solids;
        else ++other;
    });
    Json j;
    j.Int("total", total)
        .Int("networked", networked)
        .Int("players", players)
        .Int("path_movers", moving)
        .Int("other_solids", solids)
        .Int("spawns", spawns)
        .Int("death_zones", deathZones)
        .Int("other", other);
    return j.Text();
}

uint64_t SchemaHash() {
    std::string s;
    for (const char *name : {"Transform", "RigidBody", "Collider", "Renderable", "InputState", "PlatformerController",
                             "PathMover", "SpawnPoint", "DeathZone", "Respawn", "NetIdentity", "Racer", "Checkpoint",
                             "Hazard", "Finish", "Impact", "PotionPlatform", "Boost", "PeerSession", "Knockback"}) {
        const ComponentInfo *info = ComponentRegistry::ByName(name);
        s += name;
        s += ':';
        s += info ? std::to_string(info->wireId) : "-";
        s += ';';
    }
    return Fnv64(s);
}

namespace {

// --- the engine benchmark's level ------------------------------------------------

class EngineScenario final : public Scenario {
public:
    explicit EngineScenario(const Options &o) : Scenario(o) {}

    void BuildLevel(World &w, bool networked) override {
        auto net = [networked](GameObject &o) { if (networked) o.Add<NetIdentity>(); };
        // Separate streams, so the first k statics (or movers) are the same
        // objects whatever the other count is.
        Rng statics(static_cast<uint64_t>(opts.seed) * 1000003ull + 11ull);
        Rng movers(static_cast<uint64_t>(opts.seed) * 1000003ull + 29ull);

        GameObject &floor = w.Create("floor");
        floor.Add<Transform>(0.0f, 1900.0f, 20000.0f, 100.0f);
        floor.Add<Collider>();
        floor.Add<Renderable>(SDL_Color{90, 70, 50, 255});
        net(floor);

        for (int i = 0; i < opts.statics; ++i) {
            const float x = statics.Uniform(0.0f, 19800.0f), y = statics.Uniform(300.0f, 1800.0f);
            const float width = statics.Uniform(80.0f, 400.0f), height = statics.Uniform(16.0f, 40.0f);
            GameObject &o = w.Create("platform");
            o.Add<Transform>(x, y, width, height);
            o.Add<Collider>(Collider::Type::Solid, i % 4 == 0);
            o.Add<Renderable>(SDL_Color{static_cast<Uint8>(60 + i % 150), 120, 90, 255}, 1);
            net(o);
        }
        for (int i = 0; i < opts.moving; ++i) {
            const float x = movers.Uniform(0.0f, 19800.0f), y = movers.Uniform(300.0f, 1800.0f);
            const float period = movers.Uniform(2.0f, 6.0f), phase = movers.Uniform(0.0f, 1.0f);
            GameObject &o = w.Create("mover");
            o.Add<Transform>(0.0f, 0.0f, 120.0f, 20.0f);
            PathMover &p = o.Add<PathMover>();
            p.origin = {x, y};
            switch (i % 3) {
            case 0: p.offset = {300.0f, 0.0f}; break;                                  // horizontal
            case 1: p.offset = {0.0f, 200.0f}; break;                                  // vertical
            default: p.pattern = PathMover::Pattern::Circular; p.radius = 120.0f;      // circular
            }
            p.period = period;
            p.phase = phase;
            o.Add<Collider>();
            o.Add<Renderable>(SDL_Color{220, 200, 60, 255}, 1);
            net(o);
        }
        for (int i = 0; i < 4; ++i) {
            GameObject &s = w.Create("spawn");
            s.Add<Transform>(200.0f + 400.0f * static_cast<float>(i), 1800.0f, 40.0f, 100.0f);
            s.Add<SpawnPoint>(i);
            net(s);
        }
        GameObject &pit = w.Create("deathzone");
        pit.Add<Transform>(-2000.0f, 2100.0f, 24000.0f, 400.0f);
        pit.Add<Collider>(Collider::Type::Trigger);
        pit.Add<DeathZone>();
        net(pit);

        // Change-density objects: decorative, never collide, always server-owned.
        if (opts.density >= 0) {
            for (int i = 0; i < DENSITY_OBJECTS; ++i) {
                GameObject &d = w.CreateWithId(DENSITY_BASE + static_cast<ObjectId>(i), "marker");
                d.Add<Transform>(100.0f * static_cast<float>(i), 100.0f, 24.0f, 24.0f);
                d.Add<Renderable>(SDL_Color{static_cast<Uint8>(i), 80, 200, 255}, 0);
                d.Add<NetIdentity>();
            }
        }
        systems::MovePaths(w, 0.0);
    }

    void AddPlayers(World &w) override {
        for (int owner = 0; owner < opts.players; ++owner) {
            GameObject &p = w.CreateWithId(PLAYER_BASE + static_cast<ObjectId>(owner), "player");
            p.Add<Transform>(0.0f, 0.0f, 32.0f, 48.0f);
            p.Add<RigidBody>().gravity = true;
            p.Add<InputState>();
            p.Add<PlatformerController>();
            p.Add<Respawn>().spawnIndex = owner % 4;
            p.Add<Renderable>(SDL_Color{255, 200, 70, 255}, 2);
            p.Add<NetIdentity>(owner);
            systems::PlaceAtSpawn(w, p, owner % 4);
        }
    }

    void Tick(World &w, long long tick, const ObjectFilter &authority) override {
        systems::MovePaths(w, static_cast<double>(tick) * TICK_SECONDS);
        systems::ApplyControllers(w, authority);
        systems::StepPhysics(w, TICK_DT, PhysicsSettings{}, authority);
        systems::ApplyDeathZones(w, authority);
    }

    // Created, destroyed, a component removed and re-added, a one-time change
    // followed by inactivity. Markers never collide, so they cannot change
    // anyone's trajectory.
    void Fixtures(World &w, long long tick, int owner) override {
        const ObjectId base = FIXTURE_BASE + static_cast<ObjectId>(owner + 1) * 16u;
        auto marker = [&](int k) -> GameObject & {
            GameObject &m = w.CreateWithId(base + static_cast<ObjectId>(k), "marker");
            m.Add<Transform>(100.0f + 50.0f * static_cast<float>(k), 200.0f + 10.0f * static_cast<float>(owner + 1), 10.0f, 10.0f);
            m.Add<Renderable>(SDL_Color{static_cast<Uint8>(40 * k), 200, 120, 255}, 0);
            m.Add<NetIdentity>(owner);
            return m;
        };
        switch (tick) {
        case 1000:
            for (int k = 0; k < 5; ++k) marker(k);
            break;
        case 3000:
            w.Destroy(base + 1);
            break;
        case 5000:
            if (GameObject *m = w.Find(base + 2)) m->Remove<Renderable>();
            break;
        case 7000:
            if (GameObject *m = w.Find(base + 2)) m->Add<Renderable>(SDL_Color{9, 9, 9, 255}, 3);
            break;
        case 9000:
            if (GameObject *m = w.Find(base + 3)) m->Get<Transform>()->position.x += 77.0f;
            break;
        case 11000:
            w.Destroy(base + 4);
            marker(5);
            break;
        default:
            break;
        }
    }

    // Exactly `density` of the 100 eligible objects change each publication.
    void Density(World &w, long long block) override {
        if (opts.density <= 0) return;
        for (int i = 0; i < DENSITY_OBJECTS; ++i) {
            if ((i * 37 + block * 11) % 100 >= opts.density) continue;
            if (GameObject *d = w.Find(DENSITY_BASE + static_cast<ObjectId>(i))) {
                d->Get<Transform>()->position.y = 100.0f + static_cast<float>(block % 1000) * 0.5f;
            }
        }
    }
};

// --- Clockwork Dash ---------------------------------------------------------------

class GameScenario final : public Scenario {
public:
    explicit GameScenario(const Options &o) : Scenario(o) {}

    void BuildLevel(World &w, bool) override {
        clockwork::BuildCourse(w);
        clockwork::AddBenchObjects(w, opts.moving, opts.statics);
    }

    void AddPlayers(World &w) override {
        for (int owner = 0; owner < opts.players; ++owner) clockwork::AddPlayer(w, owner);
    }

    void Tick(World &w, long long, const ObjectFilter &authority) override {
        clockwork::Step(w, TICK_DT, seconds, authority);
    }

private:
    double seconds = 0.0;   // the game's own race clock, advanced by Step
};

} // namespace

std::unique_ptr<Scenario> Scenario::Make(const Options &o) {
    ComponentRegistry::RegisterBuiltins();
    clockwork::RegisterComponents();   // registered everywhere, so the schema is identical
    if (o.scenario == "game") return std::make_unique<GameScenario>(o);
    return std::make_unique<EngineScenario>(o);
}

} // namespace rb
