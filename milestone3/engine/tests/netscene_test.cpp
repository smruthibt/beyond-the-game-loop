// Milestone 3 Part 1B: the object model on the network, end to end, over
// real sockets. A SceneServer with a small level, SceneClients joining it,
// and ScenePeers with no server - in every combination of the experiment
// settings - checking that every endpoint ends up with the same scene,
// movement arrives in time, and disconnects (polite or not) are cleaned up.
#include "engine/Engine.hpp"
#include "engine/NetScene.hpp"
#include "engine/Network.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <thread>

using namespace engine;
using Clock = std::chrono::steady_clock;

static int failures = 0;
static void Check(bool ok, const char *name, double a = 0, double b = 0) {
    if (ok) printf("  PASS  %s\n", name);
    else    printf("  FAIL  %s   (%g, %g)\n", name, a, b), ++failures;
}

// Ports nothing else on the machine uses: the games sit at 65xx-67xx.
constexpr int JOIN = 7855, WORLD = 7856, CLIENTS = 7900, PEERS = 7980;
constexpr int TIMEOUT_MS = 600;

static void Sleep(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// Polls `cond` until it holds or `ms` pass.
static bool WaitFor(const std::function<bool()> &cond, int ms) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < deadline) {
        if (cond()) return true;
        Sleep(5);
    }
    return cond();
}

// The shared level: three static platforms of different sizes and colours,
// and two moving ones with different patterns. Every endpoint that builds it
// (the server, or each peer) gets the same ids.
static void BuildLevel(World &w, bool networked) {
    const float plats[3][4] = {{0, 900, 2000, 60}, {300, 700, 200, 20}, {800, 600, 120, 30}};
    for (auto &p : plats) {
        GameObject &o = w.Create("platform");
        o.Add<Transform>(p[0], p[1], p[2], p[3]);
        o.Add<Collider>();
        o.Add<Renderable>(SDL_Color{static_cast<Uint8>(p[2] / 10), 120, 80, 255});
        if (networked) o.Add<NetIdentity>();
    }
    for (int i = 0; i < 2; ++i) {
        GameObject &o = w.Create("mover");
        o.Add<Transform>(0, 0, 100, 20);
        PathMover &pm = o.Add<PathMover>();
        pm.pattern = i ? PathMover::Pattern::Circular : PathMover::Pattern::Linear;
        pm.origin = {1000.0f + 300.0f * static_cast<float>(i), 500.0f};
        pm.offset = {300.0f, 0.0f};
        pm.radius = 80.0f;
        pm.period = 3.0f;
        o.Add<Collider>();
        o.Add<Renderable>(SDL_Color{200, 200, 60, 255});
        if (networked) o.Add<NetIdentity>();
    }
}

static GameObject &AddPlayer(World &w, int owner, float x) {
    GameObject &p = w.Create("player");
    p.Add<Transform>(x, 100.0f, 32.0f, 48.0f);
    p.Add<RigidBody>();   // no gravity: the tests steer it directly
    p.Add<InputState>();
    p.Add<PlatformerController>();
    p.Add<NetIdentity>(owner);
    return p;
}

static int CountKind(World &w, const char *kind) {
    int n = 0;
    w.Each<Transform>([&](GameObject &o, Transform &) { if (o.Kind() == kind) ++n; });
    return n;
}

// A server running in its own thread.
struct TestServer {
    Engine engine;
    std::unique_ptr<SceneServer> scene;
    std::thread thread;
    double seconds = 0.0;

    TestServer(const NetOptions &opts, int maxClients = 4) {
        engine.InitHeadless();
        Server::Config cfg;
        cfg.joinPort = JOIN; cfg.worldPort = WORLD; cfg.clientPortBase = CLIENTS;
        cfg.maxClients = maxClients; cfg.clientTimeoutMs = TIMEOUT_MS; cfg.logTag = "test-scene";
        cfg.publishEverySteps = 1;   // the world goes out right after each tick: lowest latency
        scene = std::make_unique<SceneServer>(engine, cfg, opts);
        BuildLevel(scene->GetWorld(), true);
        SceneServer::Callbacks cb;
        cb.onJoin = [](World &w, int id) { AddPlayer(w, id, 100.0f + 60.0f * static_cast<float>(id)); };
        cb.step = [this](World &w, float dt) {
            seconds += dt;
            systems::MovePaths(w, seconds);
            systems::ApplyControllers(w, scene->Authority());
            systems::StepPhysics(w, dt, PhysicsSettings{}, scene->Authority());
        };
        thread = std::thread([this, cb]() { scene->Run(cb); });
        Sleep(250);
    }
    ~TestServer() {
        scene->Stop();
        thread.join();
    }
    World &W() { return scene->GetWorld(); }
    std::mutex &M() { return engine.GetWorldMutex(); }
};

// A client and the worker loop a game would run for it.
struct TestClient {
    std::unique_ptr<SceneClient> scene;
    std::mutex mutex;
    std::thread thread;
    std::atomic<bool> running{true};
    std::atomic<bool> paused{false};
    std::atomic<bool> right{false};

    explicit TestClient(const NetOptions &opts, int heartbeatMs = 1000) {
        Client::Config cfg;
        cfg.joinPort = JOIN; cfg.worldPort = WORLD; cfg.heartbeatMs = heartbeatMs;
        scene = std::make_unique<SceneClient>(cfg, opts);
    }
    bool Start() {
        if (!scene->Join()) return false;
        thread = std::thread([this]() {
            auto last = Clock::now();
            while (running) {
                const auto now = Clock::now();
                float dt = std::chrono::duration<float>(now - last).count();
                last = now;
                if (paused) dt = 0.0f;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (GameObject *me = scene->MyPlayer()) {
                        me->Get<InputState>()->Set(InputState::Right, right);
                        // Sending state: this client moves its own player.
                        systems::ApplyControllers(scene->GetWorld(), scene->Authority());
                        systems::StepPhysics(scene->GetWorld(), dt, PhysicsSettings{}, scene->Authority());
                        scene->StampLocal();
                    }
                }
                scene->Tick(dt, mutex);
                Sleep(4);
            }
        });
        return true;
    }
    void Stop(bool sayBye = true) {
        running = false;
        if (thread.joinable()) thread.join();
        if (sayBye) scene->Leave();
    }
    ~TestClient() { Stop(); }

    float XOf(int owner) {
        std::lock_guard<std::mutex> lock(mutex);
        float x = -1.0f;
        scene->GetWorld().Each<Transform, NetIdentity>([&](GameObject &, Transform &t, NetIdentity &n) {
            if (n.owner == owner) x = t.position.x;
        });
        return x;
    }
    int Players() {
        std::lock_guard<std::mutex> lock(mutex);
        return CountKind(scene->GetWorld(), "player");
    }
    int Objects() {
        std::lock_guard<std::mutex> lock(mutex);
        return static_cast<int>(scene->GetWorld().Count());
    }
};

static void ClientServerRound(const NetOptions &opts) {
    printf("\nclient-server: %s\n", opts.Describe().c_str());
    TestServer server(opts);
    TestClient a(opts), b(opts);
    Check(a.Start() && b.Start(), "two clients join");
    Check(WaitFor([&] { return a.Objects() == 7 && b.Objects() == 7; }, 3000),
          "both hold the whole scene: 5 level objects and 2 players", a.Objects(), b.Objects());

    const int aid = a.scene->Id();
    const float before = b.XOf(aid);
    a.right = true;
    Check(WaitFor([&] { return b.XOf(aid) > before + 40.0f; }, 2000),
          "A holds Right: B sees A's player move", before, b.XOf(aid));
    Sleep(500);   // keep moving a while, for latency samples
    a.right = false;
    Sleep(300);
    float serverX = -1;
    {
        std::lock_guard<std::mutex> lock(server.M());
        server.W().Each<Transform, NetIdentity>([&](GameObject &, Transform &t, NetIdentity &n) { if (n.owner == aid) serverX = t.position.x; });
    }
    Check(std::fabs(serverX - b.XOf(aid)) < 1.0f && std::fabs(serverX - a.XOf(aid)) < 1.0f,
          "once A stops, A, B and the server agree on where A is", serverX, b.XOf(aid));

    float moverA = -1, moverB = -2;
    Check(WaitFor([&] {
              {
                  std::lock_guard<std::mutex> lock(a.mutex);
                  const GameObject *m = a.scene->GetWorld().Find(4);
                  moverA = m ? m->Get<Transform>()->position.x : -1.0f;
              }
              {
                  std::lock_guard<std::mutex> lock(b.mutex);
                  const GameObject *m = b.scene->GetWorld().Find(4);
                  moverB = m ? m->Get<Transform>()->position.x : -2.0f;
              }
              return moverA != 1000.0f && std::fabs(moverA - moverB) < 25.0f;
          }, 2000),
          "the moving platforms move, and both clients see them in the same place", moverA, moverB);

    const LatencySummary lat = b.scene->Stats().Latency();
    printf("        B measured A's movement: %zu samples, mean %.2f ms, p95 %.2f ms, max %.2f ms\n",
           lat.samples, lat.meanMs, lat.p95Ms, lat.maxMs);
    // A delta stream sends a player only while it changes, and an inputs
    // stream only when a button changes, so the sample count varies by mode.
    Check(lat.samples >= 2 && lat.p95Ms < 100.0, "propagation latency under 100 ms (95th percentile)", lat.p95Ms, 100.0);

    a.Stop(true);
    Check(WaitFor([&] { return b.Players() == 1; }, 1500), "A says BYE: its player leaves B's scene", b.Players(), 1);
    int serverPlayers = -1;
    {
        std::lock_guard<std::mutex> lock(server.M());
        serverPlayers = CountKind(server.W(), "player");
    }
    Check(serverPlayers == 1, "and the server's", serverPlayers, 1);
    const float bBefore = b.XOf(b.scene->Id());
    b.right = true;
    Check(WaitFor([&] { return b.XOf(b.scene->Id()) > bBefore + 20.0f; }, 1500), "B carries on normally");
    b.right = false;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("Networked scene verification\n");

    NetOptions o;
    o.encoding = Encoding::Text;
    ClientServerRound(o);                                  // text, full, state
    o.encoding = Encoding::Binary; o.delta = true;
    ClientServerRound(o);                                  // binary, delta, state
    o.encoding = Encoding::Text; o.delta = false; o.upstream = Upstream::Input;
    ClientServerRound(o);                                  // text, full, inputs
    o.encoding = Encoding::Binary; o.delta = true;
    ClientServerRound(o);                                  // binary, delta, inputs

    printf("\nDisconnects without a goodbye\n");
    {
        NetOptions opts;
        TestServer server(opts);
        TestClient b(opts);
        b.Start();
        // A "crashed" client: it joins with a bare socket and then never says
        // anything again - no state, no heartbeat, no BYE.
        net::Socket crashed(net::SocketType::Request);
        crashed.SetReceiveTimeoutMs(1000);
        crashed.Connect("tcp://localhost:" + std::to_string(JOIN));
        crashed.Send("JOIN");
        std::string welcome;
        Check(crashed.Receive(welcome) && welcome.rfind("WELCOME", 0) == 0, "a client joins and then goes silent");
        Check(WaitFor([&] { return b.Players() == 2; }, 2000), "its player appears for the others", b.Players(), 2);
        const auto t0 = Clock::now();
        Check(WaitFor([&] { return b.Players() == 1; }, TIMEOUT_MS * 4), "after the timeout its player is removed everywhere", b.Players(), 1);
        const double took = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        Check(took > TIMEOUT_MS * 0.5, "not before the timeout", took, TIMEOUT_MS);
        Check(server.scene->GetServer().TimedOutCount() == 1 && server.scene->GetServer().ConnectedCount() == 1,
              "the server counts one timeout and still serves the rest");

        TestClient paused(opts, 150);   // heartbeats every 150 ms
        paused.Start();
        paused.paused = true;   // game time stopped: no state is sent at all
        Sleep(TIMEOUT_MS * 3);
        Check(paused.scene->GetClient().Heartbeats() > 3 && !paused.scene->GetClient().Dropped() &&
                  server.scene->GetServer().ConnectedCount() == 2,
              "a paused client sends only heartbeats and is never timed out");

        TestClient silent(opts, 0);     // no heartbeats: a client with its network stalled
        silent.Start();
        silent.paused = true;
        Check(WaitFor([&] { return silent.scene->GetClient().Dropped() || server.scene->GetServer().TimedOutCount() == 2; }, TIMEOUT_MS * 4),
              "a client that is silent too long is dropped");
        silent.paused = false;
        Check(WaitFor([&] { return silent.scene->GetClient().Dropped(); }, 1500),
              "and when it speaks again it is told GONE, not given someone else's slot");
        silent.Stop(false);
        paused.Stop();
    }

    printf("\nAny number of connections\n");
    {
        NetOptions opts;
        opts.encoding = Encoding::Binary;
        TestServer server(opts, 0);   // no limit
        std::vector<std::unique_ptr<TestClient>> clients;
        for (int i = 0; i < 10; ++i) {
            clients.push_back(std::make_unique<TestClient>(opts));
            clients.back()->Start();
        }
        Check(server.scene->GetServer().ConnectedCount() == 10, "ten clients are accepted (maxClients 0: no limit)",
              server.scene->GetServer().ConnectedCount(), 10);
        Check(WaitFor([&] {
                  for (auto &c : clients) if (c->Players() != 10) return false;
                  return true;
              }, 4000),
              "and every one of them sees all ten players");
        for (auto &c : clients) c->Stop();
    }

    printf("\nPeer-to-peer\n");
    for (int round = 0; round < 2; ++round) {
        NetOptions opts;
        opts.topology = Topology::PeerToPeer;
        if (round == 1) { opts.encoding = Encoding::Binary; opts.delta = true; opts.upstream = Upstream::Input; opts.keyframeEvery = 30; }
        printf("  (%s)\n", opts.Describe().c_str());

        struct TestPeer {
            std::unique_ptr<ScenePeer> scene;
            std::mutex mutex;
            std::thread thread;
            std::atomic<bool> running{true}, right{false};
            double seconds = 0.0;
        };
        auto start = [&](TestPeer &p) {
            ScenePeer::Config cfg;
            cfg.peer.portBase = PEERS;
            cfg.peer.maxPeers = 4;
            cfg.peerTimeoutMs = TIMEOUT_MS;
            p.scene = std::make_unique<ScenePeer>(cfg, opts);
            BuildLevel(p.scene->GetWorld(), false);   // the shared level, before Claim
            const int id = p.scene->Claim();
            AddPlayer(p.scene->GetWorld(), id, 100.0f + 60.0f * static_cast<float>(id));
            p.thread = std::thread([&p]() {
                auto last = Clock::now();
                while (p.running) {
                    const auto now = Clock::now();
                    const float dt = std::chrono::duration<float>(now - last).count();
                    last = now;
                    p.seconds += dt;
                    {
                        std::lock_guard<std::mutex> lock(p.mutex);
                        World &w = p.scene->GetWorld();
                        w.Each<InputState, NetIdentity>([&](GameObject &, InputState &in, NetIdentity &n) {
                            if (n.owner == p.scene->Id()) in.Set(InputState::Right, p.right);
                        });
                        systems::MovePaths(w, p.seconds);
                        systems::ApplyControllers(w, p.scene->Authority());
                        systems::StepPhysics(w, dt, PhysicsSettings{}, p.scene->Authority());
                        p.scene->StampLocal();
                    }
                    p.scene->Tick(dt, p.mutex);
                    Sleep(4);
                }
            });
            return id;
        };
        auto players = [](TestPeer &p) {
            std::lock_guard<std::mutex> lock(p.mutex);
            return CountKind(p.scene->GetWorld(), "player");
        };
        auto xOf = [](TestPeer &p, int owner) {
            std::lock_guard<std::mutex> lock(p.mutex);
            float x = -1.0f;
            p.scene->GetWorld().Each<Transform, NetIdentity>([&](GameObject &, Transform &t, NetIdentity &n) {
                if (n.owner == owner) x = t.position.x;
            });
            return x;
        };
        auto stop = [](TestPeer &p) {
            p.running = false;
            if (p.thread.joinable()) p.thread.join();
        };

        TestPeer p0, p1, p2;
        const int id0 = start(p0), id1 = start(p1), id2 = start(p2);
        Check(id0 == 0 && id1 == 1 && id2 == 2, "three peers claim slots 0, 1, 2 with no server");
        Check(WaitFor([&] { return players(p0) == 3 && players(p1) == 3 && players(p2) == 3; }, 3000),
              "each sees all three players", players(p0), players(p1));
        const float before = xOf(p2, id0);
        p0.right = true;
        Check(WaitFor([&] { return xOf(p2, id0) > before + 40.0f; }, 2000), "peer 0 moves: peer 2 sees it", before, xOf(p2, id0));
        p0.right = false;
        Check(WaitFor([&] { return std::fabs(xOf(p2, id0) - xOf(p0, id0)) < 2.0f && std::fabs(xOf(p1, id0) - xOf(p0, id0)) < 2.0f; }, 2500),
              "and when it stops, every peer agrees where it is", xOf(p0, id0), xOf(p2, id0));
        const LatencySummary lat = p2.scene->Stats().Latency();
        printf("        peer 2 measured the others: %zu samples, mean %.2f ms, p95 %.2f ms\n", lat.samples, lat.meanMs, lat.p95Ms);
        Check(lat.samples >= 2 && lat.p95Ms < 100.0, "peer-to-peer latency under 100 ms (95th percentile)", lat.p95Ms, 100.0);

        stop(p1);
        p1.scene.reset();   // destroying a ScenePeer says BYE
        Check(WaitFor([&] { return players(p0) == 2 && players(p2) == 2; }, 1000), "a peer that leaves is removed from the others at once");

        stop(p2);           // stops ticking, never says BYE: a crash
        const auto t0 = Clock::now();
        Check(WaitFor([&] { return players(p0) == 1; }, TIMEOUT_MS * 4), "a peer that goes silent is removed after the timeout");
        Check(std::chrono::duration<double, std::milli>(Clock::now() - t0).count() > TIMEOUT_MS * 0.5, "not before");
        stop(p0);
        p0.scene.reset();
        p2.scene.reset();
        Sleep(200);
    }

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASS", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
