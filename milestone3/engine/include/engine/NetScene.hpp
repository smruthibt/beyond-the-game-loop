#pragma once

#include "engine/Client.hpp"
#include "engine/Peer.hpp"
#include "engine/Server.hpp"
#include "engine/Snapshot.hpp"
#include "engine/Systems.hpp"
#include "engine/World.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace engine {

class Engine;

// Milestone 3, Part 1B: the object model on the network. Every endpoint -
// the server, each client, each peer - holds a World, and these classes keep
// the Worlds in step by sending snapshots (Snapshot.hpp) over the engine's
// existing sockets: Server/Client for client-server, Peer for peer-to-peer.
//
// Part 1C: everything an experiment might compare is a NetOptions field, set
// from the command line with the same flags in every program, so one build
// can run any combination:
//
//   --topology cs|p2p        client-server, or peers with no server
//   --encoding text|binary   how fields are written
//   --delta on|off           every object every time, or only what changed
//   --upstream state|input   endpoints send their objects' state, or only the
//                            buttons held - the authority then simulates them
//   --keyframe N             in delta streams, a full snapshot every N
//
// None of them changes the game: the same World ends up on every screen.
enum class Topology : uint8_t { ClientServer, PeerToPeer };
enum class Upstream : uint8_t { State, Input };

struct NetOptions {
    Topology topology = Topology::ClientServer;
    Encoding encoding = Encoding::Text;
    bool delta = false;
    Upstream upstream = Upstream::State;
    int keyframeEvery = 60;

    // Reads the flags above and leaves every other argument alone. False,
    // with a message, on a value it does not know.
    static bool Parse(int argc, char **argv, NetOptions &out, std::string *error = nullptr);
    std::string Describe() const;   // e.g. "cs binary delta input"

    SnapshotOptions Downstream() const;   // the authority's world
    SnapshotOptions Upward() const;       // an endpoint's own objects
};

struct LatencySummary {
    size_t samples = 0;
    double meanMs = 0, p50Ms = 0, p95Ms = 0, maxMs = 0;
};

// Counters every endpoint keeps, for HUDs and experiments. Safe to read from
// any thread.
class NetStats {
public:
    std::atomic<uint64_t> messagesOut{0}, messagesIn{0};
    std::atomic<uint64_t> bytesOut{0}, bytesIn{0};
    std::atomic<uint64_t> encodeMicros{0}, decodeMicros{0};
    std::atomic<uint64_t> decoded{0};   // messages actually decoded (decodeMicros is their total)
    std::atomic<uint64_t> skipped{0};   // messages superseded by a newer keyframe before decoding

    // One-way latency: from the moment an object's owner stamped it to the
    // moment another endpoint applied it. All endpoints on one machine read
    // the same steady clock, so this is measured, not estimated.
    void AddLatency(int64_t micros);
    LatencySummary Latency() const;
    void ResetLatency();

private:
    mutable std::mutex mutex;
    std::vector<int64_t> samples;   // the most recent, at most MAX
    size_t next = 0;
    static constexpr size_t MAX = 4096;
};

// The authoritative server. It owns the World; clients join, send their own
// objects, and receive everything.
class SceneServer {
public:
    struct Callbacks {
        // A client joined: make its player (NetIdentity owner = clientId).
        std::function<void(World &, int clientId)> onJoin;
        // A client left, by BYE or by going silent. Its objects are destroyed
        // right after this returns. Optional.
        std::function<void(World &, int clientId)> onLeave;
        // The game's tick, on the server's clock.
        std::function<void(World &, float dt)> step;
        // Requests that are not snapshots, for the game to answer. Optional.
        std::function<std::string(World &, int clientId, const std::string &request)> onRequest;
    };

    SceneServer(Engine &engine, Server::Config config, NetOptions options);

    // Blocks until Stop(), Ctrl-C or SIGTERM. Every callback is called with
    // the engine's world mutex held.
    bool Run(const Callbacks &callbacks);
    void Stop() { server.Stop(); }

    // Guard with engine.GetWorldMutex() outside the callbacks.
    World &GetWorld() { return world; }
    Server &GetServer() { return server; }
    const NetStats &Stats() const { return stats; }
    const NetOptions &Options() const { return options; }
    const SnapshotStats &LastSnapshot() const { return lastSnapshot; }

    // What this server simulates: its own objects, and every player too when
    // clients send inputs instead of state.
    ObjectFilter Authority() const;

private:
    void ApplyInbox();

    Engine &engine;
    Server server;
    NetOptions options;
    World world;
    SnapshotEncoder downstream;
    SnapshotDecoder upstream;
    // What clients sent since the last step. Their threads only queue it; the
    // main loop applies it at the start of a step, so the world lock is never
    // contended and every message lands at a tick boundary, in arrival order.
    std::mutex inboxMutex;
    std::vector<std::pair<int, std::string>> inbox, applying;
    uint32_t tick = 0;
    SnapshotStats lastSnapshot;
    NetStats stats;
};

// One client's view of the server's World, plus its own player.
class SceneClient {
public:
    SceneClient(Client::Config config, NetOptions options);

    // Main thread, before the loop.
    bool Join();
    const std::string &LastError() const { return client.LastError(); }
    void Leave() { client.Leave(); }

    int Id() const { return client.Id(); }
    World &GetWorld() { return world; }   // guard with the engine's world mutex
    // This client's player, once the server has sent it. World locked.
    GameObject *MyPlayer();
    // What this client simulates: its own objects when it sends state,
    // nothing when it sends inputs (the server moves its player then).
    ObjectFilter Authority() const;

    // Marks this client's objects as sampled now - call it after applying
    // the frame's input, and the latency the others measure starts at the
    // key press rather than at the send. World locked.
    void StampLocal();

    // The engine's worker thread, every iteration.
    void Tick(float dt, std::mutex &worldMutex);

    bool HasWorld() const { return hasWorld; }
    Client &GetClient() { return client; }
    const NetStats &Stats() const { return stats; }

private:
    void MeasureLatency();

    Client client;
    NetOptions options;
    World world{1u << 30};   // local objects count from here; the server's keep their ids
    SnapshotEncoder up;
    SnapshotDecoder down;
    uint32_t tick = 0;
    std::atomic<bool> hasWorld{false};
    std::map<ObjectId, int64_t> lastStamp;
    int64_t lastSentStamp = 0;
    std::vector<std::string> inbox;   // one Tick's worth of world messages
    bool ticked = false;              // a first batch has been applied
    NetStats stats, discard;
};

// A peer: no server. Each peer owns its own objects and sends them to every
// other peer; everything shared - the level, the moving platforms - every peer
// builds and moves itself, from the same seed and the same shared clock.
class ScenePeer {
public:
    struct Config {
        Peer::Config peer;
        float sendIntervalSeconds = 1.0f / 60.0f;   // of game time
        int heartbeatMs = 500;                      // wall time
        int peerTimeoutMs = 3000;                   // silence that counts as gone
    };

    ScenePeer(Config config, NetOptions options);
    ~ScenePeer();

    // Build the shared level BEFORE this: it moves Create() into this peer's
    // own id range ((id + 1) << 24), so the level's ids match on every peer
    // and each peer's own objects never collide with another's.
    int Claim(int requested = -1);
    const std::string &LastError() const { return peer.LastError(); }

    int Id() const { return peer.Id(); }
    World &GetWorld() { return world; }
    Peer &GetPeer() { return peer; }
    // Its own objects; with inputs, every player (the others from their buttons).
    ObjectFilter Authority() const;
    void StampLocal();

    // The engine's worker thread, every iteration.
    void Tick(float dt, std::mutex &worldMutex);

    int PeersHeard() const;
    const NetStats &Stats() const { return stats; }

private:
    void Send(std::mutex &worldMutex, bool fullState);
    void OnSnapshot(int sender, const std::string &snapshot, std::mutex &worldMutex);

    Config config;
    NetOptions options;
    Peer peer;
    World world;
    SnapshotEncoder up;      // per the options
    SnapshotEncoder state;   // always full state: heartbeats, and corrections in input mode
    SnapshotDecoder down;
    uint32_t tick = 0;
    float sendAccumulator = 0.0f;
    int sendsSinceState = 0;
    long long lastSendMs = 0;
    std::map<int, long long> lastHeardMs;
    std::map<ObjectId, int64_t> lastStamp;
    int64_t lastSentStamp = 0;
    mutable std::mutex heardMutex;
    NetStats stats;
};

} // namespace engine
