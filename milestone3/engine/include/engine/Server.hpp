#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace engine {

class Engine;

// Sections 2 and 4: the headless authoritative server, as an engine feature.
//
// One process, several threads, one direction per channel:
//
//   :joinPort           one REP thread     JOIN -> WELCOME id port maxClients session
//   :clientPortBase+id  one REP thread     per slot, synchronous; BYE frees it
//   :worldPort          one PUB thread     the authoritative world, at publishHz
//   main thread         Engine::RunHeadless   step(dt) from the server's Timeline
//
// Connections (Milestone 3): any number of clients can join - maxClients 0
// means no limit, and a slot's thread and port are made the first time the
// slot is used. A client that leaves says BYE. A client that cannot - it
// crashed, or was killed - goes silent, and after clientTimeoutMs the server
// frees its slot exactly as if it had said BYE, so onLeave removes its player
// and everyone else carries on. engine::Client sends a PING on wall time
// whenever it has said nothing else, so a client whose game is paused is
// never mistaken for a dead one.
//
// Each admission gets a session number, and engine::Client sends it with
// every request. A client that was timed out and comes back is told GONE
// instead of being allowed to speak for a slot that may now be someone
// else's. Requests without a session (raw sockets, older tools) are taken at
// face value.
//
// The asynchrony Section 4 asks for comes from the OS, not from 0MQ: every
// pair is a plain synchronous REQ/REP, but each client has its own thread, so a
// client that asks slowly blocks only the thread that answers it. ROUTER and
// DEALER, which would give this for free, are not used - as required.
//
// The server knows nothing about the game. What a request means, what the
// world looks like, and how it advances are all supplied through Hooks. The
// game guards its own state; the hooks are called from several threads at
// once and the server takes no lock on the game's behalf.
class Server {
public:
    struct Config {
        int joinPort = 0;
        int worldPort = 0;
        int clientPortBase = 0;
        int maxClients = 4;          // 0: no limit
        int publishHz = 60;
        int tickHz = 60;             // 0 or less: uncapped (benchmarks)
        int socketTimeoutMs = 500;
        int clientTimeoutMs = 3000;  // silence that counts as a disconnect; 0: never
        int worldQueue = 32;         // world messages held per client before the oldest are dropped
        // 0: the publisher thread sends the world at publishHz. K > 0: the
        // main loop sends it itself after every K-th step, so a loop
        // iteration includes building and sending the world - what a
        // benchmark of the loop needs to time.
        int publishEverySteps = 0;
        const char *logTag = "server";
    };

    struct Hooks {
        // A client was admitted with this id. Initialise its player. Join thread.
        std::function<void(int id)> onJoin;

        // A client left - it sent BYE, or the server is stopping. Remove its
        // player; the slot goes back to JOIN once this returns. Optional. That thread.
        std::function<void(int id)> onLeave;

        // A request arrived on this client's channel; return the reply. REQ/REP
        // must strictly alternate, so the reply must be non-empty. That thread.
        std::function<std::string(int id, const std::string &request)> onRequest;

        // The world to broadcast. Publisher thread.
        std::function<std::string()> buildWorld;

        // Advance the authoritative world by dt seconds of the server's game
        // time. Main thread, from Engine::RunHeadless.
        std::function<void(float dt)> step;
    };

    Server(Engine &engine, Config config);

    // Blocks until Stop(), SIGINT or SIGTERM. Returns false if a well-known
    // port could not be bound - usually because another server is running.
    bool Run(const Hooks &hooks);
    void Stop();

    int ClientPort(int id) const { return config.clientPortBase + id; }
    int MaxClients() const { return config.maxClients; }
    bool IsConnected(int id) const;
    int ConnectedCount() const;
    int TimedOutCount() const { return timedOut.load(); }

private:
    void JoinLoop(const Hooks &hooks);
    void ClientLoop(const Hooks &hooks, int id);
    void PublishLoop(const Hooks &hooks);
    void Leave(const Hooks &hooks, int id);
    void ReapSilent(const Hooks &hooks);
    void Log(const char *fmt, ...) const;

    Engine &engine;
    Config config;

    std::atomic<bool> running{false};
    std::atomic<bool> bindFailed{false};
    std::vector<std::thread> clientThreads;

    // The slot table is the only state the server itself owns. A slot's thread
    // outlives the client in it and serves whoever takes the slot next. It
    // grows as clients arrive.
    struct Slot {
        bool connected = false;
        bool served = false;       // has a thread and a bound port
        uint64_t session = 0;
        long long lastHeardMs = 0;
    };
    mutable std::mutex slotsMutex;
    std::vector<Slot> slots;
    uint64_t nextSession = 1;
    std::atomic<int> timedOut{0};
};

} // namespace engine
