// net_bench - Milestone 3 Part 1C: the performance comparison framework.
//
// One headless program that plays any role in a networked scene built on the
// engine's object model, runs its game loop uncapped, and times it:
//
//   net_bench server [options]    the authority; times N loop iterations,
//                                 each = simulate + build the snapshot + send it
//   net_bench client [options]    a player: receives, decodes and applies the
//                                 world, plays scripted input, sends upward
//   net_bench peer   [options]    no server: peer 0 times N loop iterations,
//                                 each = simulate + send its own objects +
//                                 apply everyone else's
//
// Each process prints one line starting with RESULT, as key=value pairs, for
// engine/tools/net_experiments.py to collect. Every option the engine's NetOptions
// understands works here too, so any of the comparisons can be run:
//
//   --topology cs|p2p  --encoding text|binary  --delta on|off
//   --upstream state|input  --keyframe N
//
// and the experiment's conditions:
//
//   --clients C        players (server: waits for C before timing; peer: C peers)
//   --moving M         moving platforms in the level        (default 100)
//   --static S         static platforms in the level        (default 100)
//   --iterations N     timed loop iterations                (default 10000)
//   --warmup W         untimed iterations before timing     (default 500)
//   --publish-every K  the server sends the world every K iterations (default 1)
//   --send-hz H        players send upward H times a second of game time;
//                      0 = every iteration                  (default 0)
//   --port-base P      join P, world P+1, clients P+100.., peers P+200..
//   --id N             a peer's slot
#include "engine/Engine.hpp"
#include "engine/NetScene.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>

using namespace engine;
using Clock = std::chrono::steady_clock;

namespace {

struct Bench {
    std::string role;
    NetOptions net;
    int clients = 2;
    int moving = 100;
    int statics = 100;
    long long iterations = 10000;
    long long warmup = 500;
    int publishEvery = 1;
    double sendHz = 0.0;
    int portBase = 8500;
    int id = -1;
};

int IntArg(int argc, char **argv, const char *flag, int fallback) {
    for (int i = 1; i + 1 < argc; ++i) if (std::strcmp(argv[i], flag) == 0) return std::atoi(argv[i + 1]);
    return fallback;
}
double DoubleArg(int argc, char **argv, const char *flag, double fallback) {
    for (int i = 1; i + 1 < argc; ++i) if (std::strcmp(argv[i], flag) == 0) return std::atof(argv[i + 1]);
    return fallback;
}

double Seconds(Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double>(b - a).count();
}

// The level every endpoint of a run shares. Deterministic: the same flags
// build the same objects with the same ids, which is what lets peers build it
// themselves. All of it travels when `networked`.
void BuildLevel(World &w, int moving, int statics, bool networked) {
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> x(0.0f, 19800.0f), y(300.0f, 1800.0f), width(80.0f, 400.0f),
        height(16.0f, 40.0f), period(2.0f, 6.0f), phase(0.0f, 1.0f);
    auto finish = [networked](GameObject &o) { if (networked) o.Add<NetIdentity>(); };

    GameObject &floor = w.Create("floor");
    floor.Add<Transform>(0.0f, 1900.0f, 20000.0f, 100.0f);
    floor.Add<Collider>();
    floor.Add<Renderable>(SDL_Color{90, 70, 50, 255});
    finish(floor);

    for (int i = 0; i < statics; ++i) {
        GameObject &o = w.Create("platform");
        o.Add<Transform>(x(rng), y(rng), width(rng), height(rng));
        o.Add<Collider>(Collider::Type::Solid, i % 4 == 0);
        o.Add<Renderable>(SDL_Color{static_cast<Uint8>(60 + i % 150), 120, 90, 255}, 1);
        finish(o);
    }
    for (int i = 0; i < moving; ++i) {
        GameObject &o = w.Create("mover");
        o.Add<Transform>(0.0f, 0.0f, 120.0f, 20.0f);
        PathMover &p = o.Add<PathMover>();
        p.origin = {x(rng), y(rng)};
        switch (i % 3) {
        case 0: p.offset = {300.0f, 0.0f}; break;                              // horizontal
        case 1: p.offset = {0.0f, 200.0f}; break;                              // vertical
        default: p.pattern = PathMover::Pattern::Circular; p.radius = 120.0f;  // circular
        }
        p.period = period(rng);
        p.phase = phase(rng);
        o.Add<Collider>();
        o.Add<Renderable>(SDL_Color{220, 200, 60, 255}, 1);
        finish(o);
    }
    for (int i = 0; i < 4; ++i) {
        GameObject &s = w.Create("spawn");
        s.Add<Transform>(200.0f + 400.0f * static_cast<float>(i), 1800.0f, 40.0f, 100.0f);
        s.Add<SpawnPoint>(i);
        finish(s);
    }
    GameObject &pit = w.Create("deathzone");
    pit.Add<Transform>(-2000.0f, 2100.0f, 24000.0f, 400.0f);
    pit.Add<Collider>(Collider::Type::Trigger);
    pit.Add<DeathZone>();
    finish(pit);
}

GameObject &AddPlayer(World &w, int owner) {
    GameObject &p = w.Create("player");
    p.Add<Transform>(0.0f, 0.0f, 32.0f, 48.0f);
    p.Add<RigidBody>().gravity = true;
    p.Add<InputState>();
    p.Add<PlatformerController>();
    p.Add<Respawn>().spawnIndex = owner % 4;
    p.Add<Renderable>(SDL_Color{255, 200, 70, 255}, 2);
    p.Add<NetIdentity>(owner);
    systems::PlaceAtSpawn(w, p, owner % 4);
    return p;
}

// What a scripted player presses at `t` seconds: run one way then the other,
// and jump now and then. The same for every encoding, so gameplay is fixed.
uint32_t ScriptedButtons(int id, double t) {
    uint32_t b = std::fmod(t + 0.4 * id, 3.0) < 1.5 ? InputState::Right : InputState::Left;
    if (std::fmod(t + 0.2 * id, 0.7) < 0.05) b |= InputState::Jump;
    return b;
}

void Simulate(World &w, float dt, const ObjectFilter &authority) {
    systems::ApplyControllers(w, authority);
    systems::StepPhysics(w, dt, PhysicsSettings{}, authority);
    systems::ApplyDeathZones(w, authority);
}

void PrintOptions(const Bench &b) {
    printf(" topology=%s encoding=%s delta=%s upstream=%s clients=%d moving=%d static=%d",
           b.net.topology == Topology::PeerToPeer ? "p2p" : "cs", EncodingName(b.net.encoding), b.net.delta ? "on" : "off",
           b.net.upstream == Upstream::Input ? "input" : "state", b.clients, b.moving, b.statics);
}

double Avg(uint64_t total, uint64_t count) {
    return count ? static_cast<double>(total) / static_cast<double>(count) : 0.0;
}

// --- server --------------------------------------------------------------------

int RunServer(const Bench &b) {
    Engine engine;
    if (!engine.InitHeadless()) return 1;
    Server::Config cfg;
    cfg.joinPort = b.portBase;
    cfg.worldPort = b.portBase + 1;
    cfg.clientPortBase = b.portBase + 100;
    cfg.maxClients = 0;
    cfg.tickHz = 0;   // uncapped: we are timing how fast the loop can go
    cfg.publishEverySteps = b.publishEvery;
    cfg.logTag = "bench-server";
    SceneServer scene(engine, cfg, b.net);
    BuildLevel(scene.GetWorld(), b.moving, b.statics, true);

    enum class Phase { Waiting, Warmup, Timed, Done } phase = Phase::Waiting;
    long long count = 0;
    double seconds = 0.0;
    Clock::time_point start, end;
    uint64_t msgs0 = 0, bytes0 = 0, enc0 = 0, upMsgs0 = 0, dec0 = 0, decoded0 = 0;
    size_t objects = 0;

    SceneServer::Callbacks cb;
    cb.onJoin = [](World &w, int id) { AddPlayer(w, id); };
    cb.step = [&](World &w, float dt) {
        seconds += dt;
        systems::MovePaths(w, seconds);
        Simulate(w, dt, scene.Authority());
        switch (phase) {
        case Phase::Waiting:
            if (scene.GetServer().ConnectedCount() >= b.clients) phase = Phase::Warmup;
            break;
        case Phase::Warmup:
            if (++count >= b.warmup) {
                phase = Phase::Timed;
                count = 0;
                const NetStats &s = scene.Stats();
                msgs0 = s.messagesOut; bytes0 = s.bytesOut; enc0 = s.encodeMicros; upMsgs0 = s.messagesIn; dec0 = s.decodeMicros;
                decoded0 = s.decoded;
                start = Clock::now();
            }
            break;
        case Phase::Timed:
            if (++count >= b.iterations) {
                end = Clock::now();
                phase = Phase::Done;
                objects = w.Count();
                scene.Stop();
            }
            break;
        case Phase::Done:
            break;
        }
    };
    if (!scene.Run(cb) || phase != Phase::Done) {
        fprintf(stderr, "net_bench server: did not finish (ports in use, or stopped early)\n");
        return 1;
    }

    const NetStats &s = scene.Stats();
    const uint64_t msgs = s.messagesOut - msgs0, bytes = s.bytesOut - bytes0;
    const uint64_t upMsgs = s.messagesIn - upMsgs0;
    const double secs = Seconds(start, end);
    printf("RESULT role=server");
    PrintOptions(b);
    printf(" objects=%zu iterations=%lld seconds=%.6f us_per_iter=%.3f snapshots=%llu snapshot_bytes=%.1f mb_out=%.3f"
           " encode_us=%.3f upstream_msgs=%llu decode_us=%.3f\n",
           objects, b.iterations, secs, secs * 1e6 / static_cast<double>(b.iterations),
           static_cast<unsigned long long>(msgs), Avg(bytes, msgs), static_cast<double>(bytes) / 1e6,
           Avg(s.encodeMicros - enc0, msgs), static_cast<unsigned long long>(upMsgs), Avg(s.decodeMicros - dec0, s.decoded - decoded0));
    fflush(stdout);
    return 0;
}

// --- client --------------------------------------------------------------------

int RunClient(const Bench &b) {
    Client::Config cfg;
    cfg.joinPort = b.portBase;
    cfg.worldPort = b.portBase + 1;
    cfg.sendIntervalSeconds = b.sendHz > 0.0 ? static_cast<float>(1.0 / b.sendHz) : 0.0f;
    SceneClient scene(cfg, b.net);
    if (!scene.Join()) {
        fprintf(stderr, "net_bench client: %s\n", scene.LastError().c_str());
        return 1;
    }
    std::mutex mutex;
    const auto t0 = Clock::now();
    auto last = t0, lastWorld = t0;
    uint64_t heard = 0;
    long long loops = 0;
    double t = 0.0;
    while (true) {
        const auto now = Clock::now();
        const float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        t += dt;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (GameObject *me = scene.MyPlayer()) {
                me->Get<InputState>()->buttons = ScriptedButtons(scene.Id(), t);
                Simulate(scene.GetWorld(), dt, scene.Authority());
                scene.StampLocal();
            }
        }
        scene.Tick(dt, mutex);
        ++loops;
        if (scene.Stats().messagesIn != heard) {
            heard = scene.Stats().messagesIn;
            lastWorld = now;
        }
        // The server finished (or went away): nothing heard for 1 s.
        if (scene.GetClient().Dropped() || Seconds(lastWorld, now) > 1.0) break;
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
    scene.Leave();
    const NetStats &s = scene.Stats();
    const LatencySummary lat = s.Latency();
    printf("RESULT role=client id=%d", scene.Id());
    PrintOptions(b);
    printf(" loops=%lld snapshots_in=%llu decoded=%llu skipped=%llu snapshot_bytes=%.1f decode_us=%.3f upstream_msgs=%llu upstream_bytes=%.1f"
           " encode_us=%.3f latency_ms=%.3f latency_p95_ms=%.3f\n",
           loops, static_cast<unsigned long long>(s.messagesIn.load()), static_cast<unsigned long long>(s.decoded.load()),
           static_cast<unsigned long long>(s.skipped.load()), Avg(s.bytesIn, s.messagesIn), Avg(s.decodeMicros, s.decoded),
           static_cast<unsigned long long>(s.messagesOut.load()), Avg(s.bytesOut, s.messagesOut), Avg(s.encodeMicros, s.messagesOut),
           lat.meanMs, lat.p95Ms);
    fflush(stdout);
    return 0;
}

// --- peer ----------------------------------------------------------------------

int RunPeer(const Bench &b) {
    ScenePeer::Config cfg;
    cfg.peer.portBase = b.portBase + 200;
    cfg.peer.maxPeers = b.clients > 4 ? b.clients : 4;
    cfg.sendIntervalSeconds = b.sendHz > 0.0 ? static_cast<float>(1.0 / b.sendHz) : 0.0f;
    ScenePeer scene(cfg, b.net);
    BuildLevel(scene.GetWorld(), b.moving, b.statics, false);   // every peer builds the level itself
    const int id = scene.Claim(b.id);
    if (id < 0) {
        fprintf(stderr, "net_bench peer: %s\n", scene.LastError().c_str());
        return 1;
    }
    AddPlayer(scene.GetWorld(), id);
    std::mutex mutex;

    // Peer 0 times the run; the others play until peer 0 has gone.
    const bool timer = id == 0;
    enum class Phase { Waiting, Warmup, Timed, Done } phase = Phase::Waiting;
    long long count = 0, loops = 0;
    double t = 0.0;
    auto last = Clock::now();
    Clock::time_point start, end, lastFull = last;
    uint64_t msgs0 = 0, bytes0 = 0, enc0 = 0, in0 = 0, dec0 = 0;
    bool everFull = false;
    while (phase != Phase::Done) {
        const auto now = Clock::now();
        const float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        t += dt;
        {
            std::lock_guard<std::mutex> lock(mutex);
            World &w = scene.GetWorld();
            systems::MovePaths(w, t);
            w.Each<InputState, NetIdentity>([&](GameObject &, InputState &in, NetIdentity &n) {
                if (n.owner == id) in.buttons = ScriptedButtons(id, t);
            });
            Simulate(w, dt, scene.Authority());
            scene.StampLocal();
        }
        scene.Tick(dt, mutex);
        ++loops;

        const bool full = scene.PeersHeard() >= b.clients - 1;
        if (full) { everFull = true; lastFull = now; }
        if (!timer) {
            if (everFull && Seconds(lastFull, now) > 1.0) phase = Phase::Done;
            else std::this_thread::sleep_for(std::chrono::microseconds(500));
            continue;
        }
        switch (phase) {
        case Phase::Waiting:
            if (full) phase = Phase::Warmup;
            break;
        case Phase::Warmup:
            if (++count >= b.warmup) {
                phase = Phase::Timed;
                count = 0;
                const NetStats &s = scene.Stats();
                msgs0 = s.messagesOut; bytes0 = s.bytesOut; enc0 = s.encodeMicros; in0 = s.messagesIn; dec0 = s.decodeMicros;
                start = Clock::now();
            }
            break;
        case Phase::Timed:
            if (++count >= b.iterations) {
                end = Clock::now();
                phase = Phase::Done;
            }
            break;
        case Phase::Done:
            break;
        }
    }

    const NetStats &s = scene.Stats();
    const LatencySummary lat = s.Latency();
    printf("RESULT role=peer id=%d", id);
    PrintOptions(b);
    if (timer) {
        const uint64_t msgs = s.messagesOut - msgs0, in = s.messagesIn - in0;
        const double secs = Seconds(start, end);
        printf(" iterations=%lld seconds=%.6f us_per_iter=%.3f msgs_out=%llu msg_bytes=%.1f encode_us=%.3f msgs_in=%llu decode_us=%.3f",
               b.iterations, secs, secs * 1e6 / static_cast<double>(b.iterations), static_cast<unsigned long long>(msgs),
               Avg(s.bytesOut - bytes0, msgs), Avg(s.encodeMicros - enc0, msgs), static_cast<unsigned long long>(in),
               Avg(s.decodeMicros - dec0, in));
    } else {
        printf(" loops=%lld msgs_in=%llu decode_us=%.3f", loops, static_cast<unsigned long long>(s.messagesIn.load()),
               Avg(s.decodeMicros, s.messagesIn));
    }
    printf(" latency_ms=%.3f latency_p95_ms=%.3f\n", lat.meanMs, lat.p95Ms);
    fflush(stdout);
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc < 2) {
        fprintf(stderr, "usage: net_bench server|client|peer [options] - see the top of bench/net_bench.cpp\n");
        return 2;
    }
    Bench b;
    b.role = argv[1];
    std::string error;
    if (!NetOptions::Parse(argc, argv, b.net, &error)) {
        fprintf(stderr, "net_bench: %s\n", error.c_str());
        return 2;
    }
    b.clients = IntArg(argc, argv, "--clients", b.clients);
    b.moving = IntArg(argc, argv, "--moving", b.moving);
    b.statics = IntArg(argc, argv, "--static", b.statics);
    b.iterations = IntArg(argc, argv, "--iterations", static_cast<int>(b.iterations));
    b.warmup = IntArg(argc, argv, "--warmup", static_cast<int>(b.warmup));
    b.publishEvery = IntArg(argc, argv, "--publish-every", b.publishEvery);
    b.sendHz = DoubleArg(argc, argv, "--send-hz", b.sendHz);
    b.portBase = IntArg(argc, argv, "--port-base", b.portBase);
    b.id = IntArg(argc, argv, "--id", b.id);
    ComponentRegistry::RegisterBuiltins();

    if (b.role == "server") return RunServer(b);
    if (b.role == "client") return RunClient(b);
    if (b.role == "peer") return RunPeer(b);
    fprintf(stderr, "net_bench: the role is server, client or peer, not %s\n", b.role.c_str());
    return 2;
}
