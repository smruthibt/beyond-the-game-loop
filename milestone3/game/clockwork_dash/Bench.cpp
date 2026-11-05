// Clockwork Dash's Part 1C benchmark: the real race, timed.
//
//   clockwork_dash bench-server [options]   the race server; times N loop iterations
//   clockwork_dash bench-client [options]   a scripted runner with no window
//
// One server iteration is one tick of a real race: Step advances 1/120 s of course,
// hazards, physics, checkpoints, head bumps, potions and stomps, and every second
// tick (ServerConfig's publishEverySteps) the snapshot is encoded and published.
// The only difference from `clockwork_dash server` is that the loop does not sleep
// to hold 120 Hz, so the time measured is the work done. Each process prints one
// RESULT line of key=value pairs.
#include "Bench.hpp"
#include "engine/Engine.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <thread>

namespace clockwork {
using namespace engine;
using Clock = std::chrono::steady_clock;

namespace {
constexpr float TICK = 1.0f / 120; // the step a real race server takes at its 120 Hz

double Seconds(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double>(b - a).count(); }
double Avg(uint64_t total, uint64_t count) { return count ? static_cast<double>(total) / static_cast<double>(count) : 0.0; }

// What a scripted runner holds at `t` seconds of its race: mostly right, back left
// now and then, jumping often, so runners meet ledges, saws, barriers and pits.
uint32_t ScriptedButtons(int id, double t) {
    uint32_t b = std::fmod(t + 0.4 * id, 3.0) < 2.0 ? InputState::Right : InputState::Left;
    if (std::fmod(t + 0.2 * id, 0.6) < 0.3) b |= InputState::Jump;
    return b;
}

int TotalRespawns(World &w) {
    int total = 0;
    w.Each<Racer, Respawn>([&](GameObject &, Racer &, Respawn &r) { total += r.deaths; });
    return total;
}

void PrintSettings(const BenchSettings &s, const NetOptions &options) {
    std::printf(" encoding=%s delta=%s clients=%d moving=%d static=%d", EncodingName(options.encoding),
                options.delta ? "on" : "off", s.clients, s.moving, s.statics);
}
} // namespace

int BenchServer(const BenchSettings &s, const NetOptions &options) {
    Engine app;
    if (!app.InitHeadless()) return 1;
    Server::Config config = ServerConfig(s.port);
    config.tickHz = 0; // uncapped: the one change from a real race
    config.logTag = "clockwork-bench";
    SceneServer scene(app, config, options);
    BuildCourse(scene.GetWorld());
    AddBenchObjects(scene.GetWorld(), s.moving, s.statics);
    int movingObjects = 0, staticObjects = 0;
    scene.GetWorld().Each<PathMover>([&](GameObject &, PathMover &) { ++movingObjects; });
    scene.GetWorld().Each<Collider>([&](GameObject &o, Collider &c) {
        if (c.type == Collider::Type::Solid && !o.Get<PathMover>()) ++staticObjects;
    });

    enum class Phase { Waiting, Warmup, Timed, Done } phase = Phase::Waiting;
    double seconds = 0;
    long long count = 0;
    Clock::time_point start, end;
    uint64_t out0 = 0, bytes0 = 0, enc0 = 0, in0 = 0, dec0 = 0, decoded0 = 0;
    int respawns = 0;
    size_t objects = 0;

    SceneServer::Callbacks callbacks;
    callbacks.onJoin = [](World &w, int id) { AddPlayer(w, id); };
    callbacks.step = [&](World &w, float) {
        Step(w, TICK, seconds); // a fixed tick, so both sides of a comparison play the same race
        switch (phase) {
        case Phase::Waiting:
            if (scene.GetServer().ConnectedCount() >= s.clients) phase = Phase::Warmup;
            break;
        case Phase::Warmup:
            if (++count >= s.warmup) {
                phase = Phase::Timed;
                count = 0;
                const NetStats &n = scene.Stats();
                out0 = n.messagesOut; bytes0 = n.bytesOut; enc0 = n.encodeMicros;
                in0 = n.messagesIn; dec0 = n.decodeMicros; decoded0 = n.decoded;
                respawns = -TotalRespawns(w);
                start = Clock::now();
            }
            break;
        case Phase::Timed:
            if (++count >= s.iterations) {
                end = Clock::now();
                phase = Phase::Done;
                objects = w.Count();
                respawns += TotalRespawns(w);
                scene.Stop();
            }
            break;
        case Phase::Done:
            break;
        }
    };
    if (!scene.Run(callbacks) || phase != Phase::Done) {
        std::fprintf(stderr, "clockwork bench server: did not finish (ports in use, or stopped early)\n");
        return 1;
    }

    const NetStats &n = scene.Stats();
    const uint64_t out = n.messagesOut - out0, bytes = n.bytesOut - bytes0;
    const double secs = Seconds(start, end);
    std::printf("RESULT role=server");
    PrintSettings(s, options);
    std::printf(" objects=%zu moving_objects=%d static_objects=%d iterations=%lld seconds=%.6f us_per_iter=%.3f"
                " snapshots=%llu snapshot_bytes=%.1f mb_out=%.3f encode_us=%.3f upstream_msgs=%llu decode_us=%.3f"
                " respawns=%d game_seconds=%.1f\n",
                objects, movingObjects, staticObjects, s.iterations, secs, secs * 1e6 / static_cast<double>(s.iterations),
                static_cast<unsigned long long>(out), Avg(bytes, out), static_cast<double>(bytes) / 1e6,
                Avg(n.encodeMicros - enc0, out), static_cast<unsigned long long>(n.messagesIn - in0),
                Avg(n.decodeMicros - dec0, n.decoded - decoded0), respawns, seconds);
    std::fflush(stdout);
    app.Shutdown();
    return 0;
}

int BenchClient(const BenchSettings &s, const NetOptions &options) {
    SceneClient scene(ClientConfig(s.port), options);
    if (!scene.Join()) {
        std::fprintf(stderr, "clockwork bench client: %s\n", scene.LastError().c_str());
        return 1;
    }
    std::mutex mutex;
    auto last = Clock::now(), lastWorld = last;
    uint64_t heard = 0;
    long long loops = 0;
    while (true) {
        const auto now = Clock::now();
        const float dt = std::chrono::duration<float>(now - last).count();
        last = now;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (GameObject *me = scene.MyPlayer()) {
                // Scripted on the race clock the server replicates, not the wall clock,
                // because the uncapped server runs the race faster than real time.
                const Racer *racer = me->Get<Racer>();
                me->Get<InputState>()->buttons = ScriptedButtons(scene.Id(), racer ? racer->elapsed : 0.0);
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
    const NetStats &n = scene.Stats();
    const LatencySummary latency = n.Latency();
    std::printf("RESULT role=client id=%d", scene.Id());
    PrintSettings(s, options);
    std::printf(" loops=%lld snapshots_in=%llu decoded=%llu skipped=%llu snapshot_bytes=%.1f decode_us=%.3f"
                " upstream_msgs=%llu upstream_bytes=%.1f encode_us=%.3f latency_ms=%.3f latency_p95_ms=%.3f\n",
                loops, static_cast<unsigned long long>(n.messagesIn.load()), static_cast<unsigned long long>(n.decoded.load()),
                static_cast<unsigned long long>(n.skipped.load()), Avg(n.bytesIn, n.messagesIn), Avg(n.decodeMicros, n.decoded),
                static_cast<unsigned long long>(n.messagesOut.load()), Avg(n.bytesOut, n.messagesOut),
                Avg(n.encodeMicros, n.messagesOut), latency.meanMs, latency.p95Ms);
    std::fflush(stdout);
    return 0;
}

} // namespace clockwork
