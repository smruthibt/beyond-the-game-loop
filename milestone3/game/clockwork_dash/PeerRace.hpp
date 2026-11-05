#pragma once

#include "Game.hpp"

#include <map>
#include <set>
#include <tuple>

namespace clockwork {

// Shared course changes use the engine's replayable, deduplicated event log.
// Player motion stays owner-authoritative; no peer is a host or leader.
class PeerCourse {
public:
    PeerCourse(engine::World &world, int owner, std::function<void(const std::string &)> append);
    CourseEvents Hooks();
    void Reconcile(const std::vector<engine::Peer::Event> &events, int64_t now);

private:
    struct Claim {
        int owner = -1;
        int64_t seq = 0, session = 0, time = 0;
        int run = 0, deaths = 0;
        auto Order() const { return std::make_tuple(time, owner, seq); }
    };
    engine::World &world;
    int owner;
    std::function<void(const std::string &)> append;
    std::set<engine::ObjectId> requested;
    std::set<std::pair<int, int64_t>> seenBumps, awards;
    std::map<engine::ObjectId, Claim> winners;
};

class PeerRace {
public:
    PeerRace(int port, const engine::NetOptions &options);
    bool Join(int requested = -1);
    void Step(float dt); // caller holds the world mutex
    void Tick(float dt, std::mutex &mutex) { scene.Tick(dt, mutex); }
    int Id() const { return scene.Id(); }
    int PeersHeard() const { return scene.PeersHeard(); }
    engine::World &GetWorld() { return scene.GetWorld(); }
    const std::string &LastError() const { return scene.LastError(); }
    double Seconds() { return scene.GetPeer().SharedSeconds(); }

private:
    void Sync();
    engine::ScenePeer scene;
    std::unique_ptr<PeerCourse> course;
    CourseEvents hooks;
};

} // namespace clockwork
