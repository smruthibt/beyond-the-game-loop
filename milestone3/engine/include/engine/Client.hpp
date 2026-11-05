#pragma once

#include "engine/Network.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace engine {

// Sections 2 and 4, client side: the network half of a windowed client, as an
// engine feature. It pairs with engine::Server.
//
//   Join()     JOIN on the well-known port -> WELCOME id port maxClients, then
//              connects the dedicated REQ channel and the world SUB.
//   Request()  one synchronous exchange on the channel - fetch the map, fire a
//              shot, anything the game needs an answer to.
//   Tick()     the per-frame network work, meant for the engine's worker
//              thread: drain the world broadcast, and when enough GAME time
//              has passed, send the game's state upstream and wait for the ACK.
//   Leave()    BYE on the channel: the server drops this player and frees the
//              slot for the next JOIN. The destructor calls it.
//
// The pacing is the point. Tick() is given the timeline's dt, so a client at
// 0.5x sends half as many 0MQ messages and at 2.0x twice as many - Section 4's
// requirement - and a paused client sends none while still hearing the world.
//
// The engine never looks inside a message. The game encodes its own state and
// decodes the world it is given.
class Client {
public:
    struct Config {
        int joinPort = 0;
        int worldPort = 0;
        const char *worldTopic = "WORLD";
        float sendIntervalSeconds = 1.0f / 60.0f;   // of game time
        int timeoutMs = 2000;
        // If nothing else has gone upstream for this long of WALL time - the
        // game is paused, say - Tick() sends a PING, so the server never
        // mistakes a quiet client for a dead one. 0: never.
        int heartbeatMs = 1000;
        int worldQueue = 32;   // world messages held before the oldest are dropped
    };

    explicit Client(Config config);
    ~Client();

    // Main thread, before the loop. False if the server did not answer.
    bool Join();

    // After the loop. Safe to call twice, and a no-op if Join() failed.
    void Leave();

    int Id() const { return id; }
    int ChannelPort() const { return channelPort; }
    int MaxClients() const { return maxClients; }
    const std::string &LastError() const { return lastError; }

    // One request, one reply, on this client's own channel. False on timeout.
    // Main thread before the loop, worker thread during it - never both.
    bool Request(const std::string &request, std::string &reply);

    // One worker iteration. `dt` is GAME time from the timeline.
    //   onWorld      called once per world message drained, oldest first
    //   encodeState  called only when it is time to send; returns the payload
    // Returns true if a state was sent this tick.
    bool Tick(float dt,
              const std::function<void(const std::string &world)> &onWorld,
              const std::function<std::string()> &encodeState);

    // Readable from any thread, for the HUD. True while the server is either
    // answering this client or broadcasting to it - a paused client sends
    // nothing and is still connected.
    bool Connected() const { return connected; }
    int RoundTrips() const { return roundTrips; }
    int SendsPerSecond() const { return sendsPerSecond; }
    // The server answered GONE: it dropped this client (it was silent past
    // the server's timeout) and the slot is no longer ours. Rejoin to play on.
    bool Dropped() const { return dropped; }
    int Heartbeats() const { return heartbeats; }

private:
    // Every channel exchange goes through here: it adds the session and
    // notices a GONE.
    bool Exchange(const std::string &payload, std::string &reply);

    Config config;
    int id = -1;
    uint64_t session = 0;
    int channelPort = 0;
    int maxClients = 0;
    bool left = false;
    std::string lastError;

    net::Socket channel{net::SocketType::Request};
    net::Socket world{net::SocketType::Subscribe};

    float sendAccumulator = 0.0f;
    int sendsThisWindow = 0;
    long long windowStartMs = 0;
    long long lastExchangeMs = 0;

    std::atomic<bool> connected{false};
    std::atomic<int> roundTrips{0};
    std::atomic<int> sendsPerSecond{0};
    std::atomic<bool> dropped{false};
    std::atomic<int> heartbeats{0};
};

} // namespace engine
