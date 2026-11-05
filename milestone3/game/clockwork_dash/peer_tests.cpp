#include "PeerRace.hpp"
#include "engine/Network.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>

using namespace engine;
using namespace clockwork;
namespace {
int failures = 0;
void Check(bool ok, const char *name) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", name);
    if (!ok) ++failures;
}
int Players(World &w) {
    int count = 0; w.Each<Racer>([&](GameObject &, Racer &) { ++count; }); return count;
}
GameObject &Potion(World &w) {
    GameObject *result = nullptr;
    w.Each<PotionPlatform>([&](GameObject &p, PotionPlatform &) { if (!result) result = &p; });
    return *result;
}
void Move(GameObject &p, float x, float y = FLOOR_Y - RUNNER_HEIGHT) {
    p.Get<Transform>()->position = {x, y};
    p.Get<RigidBody>()->velocity = {0, 0};
    p.Get<RigidBody>()->ground = NO_OBJECT;
    p.Get<RigidBody>()->grounded = false;
}
// Four independent worlds using the exact owner-filtered simulation, event
// reconciliation and snapshot encoding used by PeerRace, without requiring TCP.
struct Endpoint {
    World world;
    int owner;
    int64_t sequence;
    std::vector<Peer::Event> &log;
    PeerCourse course;
    CourseEvents hooks;
    SnapshotEncoder encoder;
    SnapshotDecoder decoder;
    double seconds = 0;
    Endpoint(int id, Encoding encoding, bool delta, std::vector<Peer::Event> &events, int64_t session = 0)
        : owner(id), sequence(session ? session : 1000 + id * 100), log(events),
          course(world, id, [this](const std::string &s) { log.push_back({owner, ++sequence, s}); }),
          hooks(course.Hooks()), encoder(SnapshotOptions(encoding, delta, 3)) {
        BuildCourse(world); world.SetNextId((static_cast<ObjectId>(id) + 1) << 24);
        auto &p = AddPlayer(world, id);
        p.Add<PeerSession>().session = sequence;
        p.Get<Racer>()->countdown = 0;
    }
    GameObject &Me() { return *Player(world, owner); }
    void Sync() { course.Reconcile(log, Timeline::RealTimeNow()); }
    void Resolve() { ResolveCourse(world, OwnedBy(owner), hooks); Sync(); }
};

void OfflineRound(Encoding encoding, bool delta) {
    std::printf("\nPEER WORLDS %s delta=%d\n", EncodingName(encoding), delta);
    std::vector<Peer::Event> log;
    std::vector<std::unique_ptr<Endpoint>> peers;
    for (int id = 0; id < 4; ++id) peers.push_back(std::make_unique<Endpoint>(id, encoding, delta, log));
    uint32_t tick = 0;
    auto exchange = [&]() {
        ++tick;
        for (auto &source : peers) {
            const auto message = source->encoder.Encode(source->world, tick, OwnedBy(source->owner));
            for (auto &target : peers) if (source != target) {
                ApplyOptions options; options.source = options.onlyOwner = source->owner;
                auto result = target->decoder.Apply(target->world, message, options);
                if (!result.ok) ++failures;
                target->world.Flush();
            }
        }
        for (auto &p : peers) p->Sync();
    };
    auto all = [&](const std::function<bool(World &)> &test) {
        for (auto &p : peers) if (!test(p->world)) return false;
        return true;
    };
    exchange();
    Check(all([](World &w) { return Players(w) == 4; }), "four peers see all runners");
    auto &a = *peers[0]; auto &b = *peers[1];
    a.Me().Get<InputState>()->buttons = InputState::Right;
    for (int i = 0; i < 30; ++i) {
        for (auto &p : peers) Step(p->world, 1.0f / 120, p->seconds, OwnedBy(p->owner), p->hooks);
        exchange();
    }
    Check(all([](World &w) { return Player(w, 0)->Get<Transform>()->Left() > 180; }), "owner movement reaches every peer");
    Check(b.Me().Get<Transform>()->Left() == 146, "another peer's input does not move the local runner");
    a.Me().Get<InputState>()->buttons = 0;

    auto &ledge = Potion(a.world);
    auto &lt = *ledge.Get<Transform>();
    Move(a.Me(), lt.Center().x - 22, lt.Bottom());
    a.Resolve(); exchange();
    Check(all([&](World &w) { auto *p = w.Find(ledge.Id()); return p->Get<PotionPlatform>()->opened && p->Get<Impact>()->amount > 0; }),
          "head bump opens the same potion ledge on every peer");
    Move(a.Me(), PotionArea(lt).Left(), lt.Top() - RUNNER_HEIGHT);
    a.Resolve(); exchange();
    Check(all([&](World &w) { return w.Find(ledge.Id())->Get<PotionPlatform>()->taken && Player(w, 0)->Get<Boost>()->remaining > 0; }),
          "one potion claim grants and replicates the boost");

    // Peer 1 stomps peer 0; only peer 1 bounces locally, and peer 0 applies
    // the boost-removal event before publishing its own corrected state.
    Move(a.Me(), 900); exchange();
    auto &victim = *Player(b.world, 0);
    Move(b.Me(), victim.Get<Transform>()->Left(), victim.Get<Transform>()->Top() - RUNNER_HEIGHT + 2);
    b.Me().Get<RigidBody>()->velocity.y = 100;
    b.Resolve(); exchange(); exchange();
    Check(b.Me().Get<RigidBody>()->velocity.y < 0 && all([](World &w) { return Player(w, 0)->Get<Boost>()->remaining == 0; }),
          "cross-peer stomp bounces the attacker and removes the victim's boost");

    for (int order = 1; order <= CHECKPOINTS; ++order) {
        a.world.Each<Checkpoint, Transform>([&](GameObject &, Checkpoint &c, Transform &t) {
            if (c.order == order) Move(a.Me(), t.Left() + 40);
        });
        a.Resolve();
    }
    Move(a.Me(), 1700, 1000); a.Resolve(); exchange();
    Check(all([](World &w) { auto *p = Player(w, 0); return p->Get<Racer>()->checkpoint == 3 && p->Get<Respawn>()->deaths == 1 && p->Get<Transform>()->Left() == 5590; }),
          "checkpoints and pit respawns agree across peers");
    a.Me().Get<Racer>()->protection = 0;
    a.world.Each<Hazard, Transform>([&](GameObject &, Hazard &h, Transform &t) {
        if (h.type == Hazard::Type::Saw && t.Left() < 1000) Move(a.Me(), t.Left(), t.Top());
    });
    a.Resolve(); exchange();
    Check(all([](World &w) { return Player(w, 0)->Get<Respawn>()->deaths == 2; }), "saw respawns replicate");
    Move(a.Me(), 6200); a.Resolve(); exchange();
    Check(all([](World &w) { return Player(w, 0)->Get<Racer>()->finished; }), "finish replicates");
    a.Me().Get<InputState>()->buttons = RESTART;
    Step(a.world, 1.0f / 120, a.seconds, OwnedBy(0), a.hooks); exchange();
    Check(all([](World &w) { auto *p = Player(w, 0); return !p->Get<Racer>()->finished && p->Get<Racer>()->checkpoint == 0 && p->Get<Racer>()->countdown > 2; }),
          "restart resets only the owner's run");
    Check(b.Me().Get<Racer>()->countdown == 0 && b.Me().Get<Racer>()->elapsed > 0, "other racers continue through a restart");

    Endpoint replacement(0, encoding, delta, log, 9000);
    replacement.Sync();
    Check(Potion(replacement.world).Get<PotionPlatform>()->taken && replacement.Me().Get<Boost>()->remaining == 0,
          "late join replays consumed potions without awarding an old session's boost");
}

void ConflictTests() {
    std::vector<Peer::Event> logA, logB;
    Endpoint a(0, Encoding::Binary, true, logA), b(1, Encoding::Binary, true, logB);
    const auto id = Potion(a.world).Id();
    const auto now = Timeline::RealTimeNow();
    const Peer::Event first{0, 1001, "take " + std::to_string(id) + " 1000 0 0 " + std::to_string(now - 100)};
    const Peer::Event second{1, 1101, "take " + std::to_string(id) + " 1100 0 0 " + std::to_string(now)};
    b.course.Reconcile({second}, now);
    Check(b.Me().Get<Boost>()->remaining > 0, "local pickup responds before gossip arrives");
    a.course.Reconcile({second, first}, now);
    b.course.Reconcile({first, second}, now);
    Check(a.Me().Get<Boost>()->remaining > 0 && b.Me().Get<Boost>()->remaining == 0,
          "simultaneous claims converge to one winner despite reversed delivery order");
    const auto remaining = a.Me().Get<Boost>()->remaining;
    a.course.Reconcile({first, first, second}, now + 1000000);
    Check(a.Me().Get<Boost>()->remaining == remaining, "duplicate gossip never refreshes a boost");
    Restart(a.world, a.Me());
    a.course.Reconcile({first, second}, now);
    Check(a.Me().Get<Boost>()->remaining == 0, "replayed claims cannot restore a boost after restart");

    std::vector<Peer::Event> logC;
    Endpoint c(0, Encoding::Text, false, logC);
    const Peer::Event stomp{1, 1102, "stomp 0 1000 0 1001"};
    c.course.Reconcile({stomp}, now);
    c.course.Reconcile({stomp, first}, now);
    Check(c.Me().Get<Boost>()->remaining == 0 && c.Me().Get<Boost>()->stomped > 0,
          "stomp arriving before its potion claim still removes the awarded boost");

    World world; BuildCourse(world); AddPlayer(world, 0).Get<Racer>()->countdown = 0;
    auto &remote = AddPlayer(world, 1); remote.Get<InputState>()->buttons = RESTART | InputState::Right;
    GiveBoost(remote); Move(remote, 1700, 1000);
    double seconds = 0; Step(world, 0.25f, seconds, OwnedBy(0));
    Check(remote.Get<Transform>()->Left() == 1700 && remote.Get<Transform>()->Top() == 1000
          && remote.Get<Boost>()->remaining == BOOST_SECONDS && remote.Get<Racer>()->countdown == 3
          && remote.Get<Respawn>()->deaths == 0,
          "owner filtering leaves remote physics, effects, timers and restart inputs untouched");
}

int NetworkTests() {
    net::Socket probe(net::SocketType::Publish);
    if (!probe.Bind("tcp://127.0.0.1:*")) {
        const auto error = probe.LastError();
        if (error.find("Operation not permitted") != std::string::npos || error.find("Permission denied") != std::string::npos) {
            std::printf("SKIP: local TCP sockets prohibited: %s\n", error.c_str()); return 77;
        }
        std::fprintf(stderr, "Socket preflight failed: %s\n", error.c_str()); return 1;
    }
    int port = 14150;
    for (auto encoding : {Encoding::Text, Encoding::Binary}) for (bool delta : {false, true}) {
        std::printf("\nLIVE PEERS %s delta=%d\n", EncodingName(encoding), delta);
        auto options = NetworkOptions(); options.encoding = encoding; options.delta = delta;
        std::vector<std::unique_ptr<PeerRace>> peers(4);
        std::mutex mutex;
        bool active[4] = {true, true, true, true};
        auto join = [&](int id) {
            peers[id] = std::make_unique<PeerRace>(port, options);
            if (!peers[id]->Join(id)) { Check(false, "peer claims slot"); return false; }
            Player(peers[id]->GetWorld(), id)->Get<Racer>()->countdown = 0;
            return true;
        };
        auto pump = [&](const std::function<bool()> &done, int timeout = 4000) {
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout);
            do {
                for (int id = 0; id < 4; ++id) if (peers[id] && active[id]) {
                    { std::lock_guard<std::mutex> lock(mutex); peers[id]->Step(0.004f); }
                    peers[id]->Tick(0.004f, mutex);
                }
                if (done()) return true;
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            } while (std::chrono::steady_clock::now() < deadline);
            return done();
        };
        bool joined = true;
        for (int id = 0; id < 4; ++id) joined = join(id) && joined;
        if (!joined) return 1;
        Check(pump([&]() { for (auto &p : peers) if (Players(p->GetWorld()) != 4) return false; return true; }),
              "four live peers discover each other without a server");
        auto &a = *Player(peers[0]->GetWorld(), 0);
        a.Get<InputState>()->buttons = InputState::Right;
        Check(pump([&]() { auto *p = Player(peers[3]->GetWorld(), 0); return p && p->Get<Transform>()->Left() > 180; }),
              "live owner movement reaches another peer");
        a.Get<InputState>()->buttons = 0;
        auto &ledge = Potion(peers[0]->GetWorld());
        Move(a, ledge.Get<Transform>()->Center().x - 22, ledge.Get<Transform>()->Bottom());
        Check(pump([&]() { for (auto &p : peers) if (!Potion(p->GetWorld()).Get<PotionPlatform>()->opened) return false; return true; }),
              "live event gossip opens the potion on all peers");
        Move(a, PotionArea(*ledge.Get<Transform>()).Left(), ledge.Get<Transform>()->Top() - RUNNER_HEIGHT);
        Check(pump([&]() { for (auto &p : peers) if (!Potion(p->GetWorld()).Get<PotionPlatform>()->taken) return false; return true; }),
              "live event gossip consumes the potion on all peers");
        a.Get<Racer>()->checkpoint = 1; a.Get<Respawn>()->spawnIndex = 101;
        Move(a, 1700, 1000);
        Check(pump([&]() { auto *p = Player(peers[3]->GetWorld(), 0); return p && p->Get<Respawn>()->deaths == 1 && p->Get<Transform>()->Left() == 2070; }),
              "live checkpoint respawn replicates");
        peers[0].reset();
        Check(pump([&]() { return Players(peers[1]->GetWorld()) == 3; }), "first peer can leave without stopping the race");
        active[2] = false;
        Check(pump([&]() { return Players(peers[1]->GetWorld()) == 2; }), "silent peer is removed after timeout");
        peers[2].reset();
        if (!join(0)) return 1;
        Check(pump([&]() { return Players(peers[0]->GetWorld()) == 3 && Potion(peers[0]->GetWorld()).Get<PotionPlatform>()->taken; }),
              "replacement peer receives surviving players and consumed potions");
        Check(Player(peers[0]->GetWorld(), 0)->Get<Boost>()->remaining == 0, "replacement does not inherit its predecessor's boost");
        port += 300;
    }
    return failures ? 1 : 0;
}
}

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    RegisterComponents();
    if (argc > 1 && std::string(argv[1]) == "--network") return NetworkTests();
    ConflictTests();
    for (auto encoding : {Encoding::Text, Encoding::Binary}) for (bool delta : {false, true}) OfflineRound(encoding, delta);
    return failures ? 1 : 0;
}
