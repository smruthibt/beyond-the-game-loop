#include "engine/Server.hpp"

#include "engine/Engine.hpp"
#include "engine/Network.hpp"

#include <chrono>
#include <csignal>
#include <cstdarg>
#include <cstdio>

namespace engine {

namespace {

// Signal handlers may only touch lock-free atomics. One server per process
// is the norm, so a file-scope flag is fine.
std::atomic<bool> g_signalled{false};
void OnSignal(int) { g_signalled = true; }

} // namespace

Server::Server(Engine &engineRef, Config cfg)
    : engine(engineRef), config(cfg),
      connected(static_cast<size_t>(cfg.maxClients), false),
      served(static_cast<size_t>(cfg.maxClients), false) {}

void Server::Log(const char *fmt, ...) const {
    printf("[%s] ", config.logTag);
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
    fflush(stdout);
}

bool Server::IsConnected(int id) const {
    std::lock_guard<std::mutex> lock(slotsMutex);
    return id >= 0 && id < config.maxClients && connected[static_cast<size_t>(id)];
}

int Server::ConnectedCount() const {
    std::lock_guard<std::mutex> lock(slotsMutex);
    int n = 0;
    for (bool c : connected) if (c) ++n;
    return n;
}

void Server::Stop() {
    running = false;
    engine.Stop();
}

// One of these per slot, on its own port. Strictly synchronous: receive a
// request, send exactly one reply, repeat. The parallelism is the thread, not
// the socket.
void Server::ClientLoop(const Hooks &hooks, int id) {
    net::Socket socket(net::SocketType::Reply);
    socket.SetReceiveTimeoutMs(config.socketTimeoutMs);
    if (!socket.Bind("tcp://*:" + std::to_string(ClientPort(id)))) {
        Log("cannot serve client %d: %s", id, socket.LastError().c_str());
        std::lock_guard<std::mutex> lock(slotsMutex);
        connected[static_cast<size_t>(id)] = false;
        served[static_cast<size_t>(id)] = false;
        return;
    }
    Log("client %d served on port %d", id, ClientPort(id));

    std::string request;
    while (running) {
        // A timeout is not an error: it is how this thread gets a chance to
        // notice `running` has gone false and leave.
        if (!socket.Receive(request)) continue;
        if (request == "BYE") {
            Leave(hooks, id);
            socket.Send("BYE");
            continue;
        }
        std::string reply = hooks.onRequest ? hooks.onRequest(id, request) : std::string("ACK");
        if (reply.empty()) reply = "ACK";   // REQ/REP must alternate; never leave the peer waiting
        socket.Send(reply);
    }

    Leave(hooks, id);
}

// Frees a slot once, on BYE or at shutdown, whichever comes first. The game
// drops the player before the slot is marked free, so JOIN cannot hand the
// slot to a newcomer while the old player is still in the world.
void Server::Leave(const Hooks &hooks, int id) {
    {
        std::lock_guard<std::mutex> lock(slotsMutex);
        if (!connected[static_cast<size_t>(id)]) return;
    }
    if (hooks.onLeave) hooks.onLeave(id);
    {
        std::lock_guard<std::mutex> lock(slotsMutex);
        connected[static_cast<size_t>(id)] = false;
    }
    Log("client %d left; slot %d is free", id, id);
}

// The only well-known port. Serial by design: joining happens once per client,
// so there is nothing to gain from making it concurrent.
void Server::JoinLoop(const Hooks &hooks) {
    net::Socket socket(net::SocketType::Reply);
    socket.SetReceiveTimeoutMs(config.socketTimeoutMs);
    if (!socket.Bind("tcp://*:" + std::to_string(config.joinPort))) {
        Log("cannot open the join channel: %s - is another server running?", socket.LastError().c_str());
        bindFailed = true;
        Stop();
        return;
    }
    Log("join channel listening on %d", config.joinPort);

    std::string request;
    while (running) {
        if (!socket.Receive(request)) continue;
        if (request.rfind("JOIN", 0) != 0) {
            socket.Send("ERR expected JOIN");
            continue;
        }

        // The lowest free slot. A slot that has been used before still has its
        // thread and port, and that thread serves the newcomer.
        int assigned = -1;
        bool spawn = false;
        {
            std::lock_guard<std::mutex> lock(slotsMutex);
            for (int i = 0; i < config.maxClients; ++i) {
                const size_t slot = static_cast<size_t>(i);
                if (connected[slot]) continue;
                assigned = i;
                connected[slot] = true;
                spawn = !served[slot];
                served[slot] = true;
                break;
            }
        }
        if (assigned < 0) {
            socket.Send("ERR server full");
            continue;
        }

        if (hooks.onJoin) hooks.onJoin(assigned);
        if (spawn) clientThreads.emplace_back(&Server::ClientLoop, this, std::cref(hooks), assigned);

        socket.Send("WELCOME " + std::to_string(assigned) + ' ' +
                    std::to_string(ClientPort(assigned)) + ' ' +
                    std::to_string(config.maxClients));
        Log("client %d joined", assigned);
    }
}

// Broadcasts the authoritative world to everyone at a fixed rate, independent
// of how fast any individual client is asking for anything.
void Server::PublishLoop(const Hooks &hooks) {
    net::Socket socket(net::SocketType::Publish);
    if (!socket.Bind("tcp://*:" + std::to_string(config.worldPort))) {
        Log("cannot open the world broadcast: %s", socket.LastError().c_str());
        bindFailed = true;
        Stop();
        return;
    }
    Log("world broadcast on %d", config.worldPort);

    const auto period = std::chrono::microseconds(1000000 / (config.publishHz > 0 ? config.publishHz : 60));
    while (running) {
        if (hooks.buildWorld) socket.Send(hooks.buildWorld());
        std::this_thread::sleep_for(period);
    }
}

bool Server::Run(const Hooks &hooks) {
    running = true;
    bindFailed = false;
    g_signalled = false;
    signal(SIGINT, OnSignal);
    signal(SIGTERM, OnSignal);

    std::thread joinThread(&Server::JoinLoop, this, std::cref(hooks));
    std::thread publishThread(&Server::PublishLoop, this, std::cref(hooks));
    Log("running - Ctrl-C to stop");

    engine.RunHeadless(
        [this, &hooks](float dt) {
            if (hooks.step) hooks.step(dt);
            if (g_signalled) Stop();
        },
        config.tickHz);

    running = false;
    joinThread.join();
    publishThread.join();
    for (std::thread &t : clientThreads) {
        if (t.joinable()) t.join();
    }
    clientThreads.clear();
    Log("stopped cleanly");
    return !bindFailed;
}

} // namespace engine
