// engine::Server on its own - no game, no protocol, just the contract:
// join hands out ids and ports, each client gets its own thread, hooks fire
// where they should, the world is broadcast, a full table is refused, and
// Stop() brings every thread home.
#include "engine/Engine.hpp"
#include "engine/Network.hpp"
#include "engine/Server.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace engine;
using namespace engine::net;

static int failures = 0;
static void Check(bool ok, const char *name, long long v = 0) {
    if (ok) printf("  PASS  %s\n", name);
    else    printf("  FAIL  %s   (%lld)\n", name, v), ++failures;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("engine::Server verification\n\n");

    Engine engine;
    if (!engine.InitHeadless()) { printf("  headless init failed\n"); return 1; }

    Server::Config cfg;
    cfg.joinPort = 7555; cfg.worldPort = 7556; cfg.clientPortBase = 7600;
    cfg.maxClients = 3; cfg.publishHz = 60; cfg.tickHz = 120; cfg.logTag = "test-server";
    Server server(engine, cfg);

    // --- hooks record what happened, and on which thread ----------------------
    std::mutex m;
    std::vector<int> joined;
    std::set<std::thread::id> requestThreads;
    std::atomic<int> steps{0};
    std::atomic<int> worldsBuilt{0};

    std::vector<int> leftIds;

    Server::Hooks hooks;
    hooks.onJoin = [&](int id) { std::lock_guard<std::mutex> l(m); joined.push_back(id); };
    hooks.onLeave = [&](int id) { std::lock_guard<std::mutex> l(m); leftIds.push_back(id); };
    hooks.onRequest = [&](int id, const std::string &req) {
        { std::lock_guard<std::mutex> l(m); requestThreads.insert(std::this_thread::get_id()); }
        return "REPLY " + std::to_string(id) + " " + req;
    };
    hooks.buildWorld = [&]() { ++worldsBuilt; return std::string("WORLD tick ") + std::to_string(steps.load()); };
    hooks.step = [&](float) { ++steps; };

    std::atomic<bool> runReturned{false};
    bool runResult = false;
    std::thread serverThread([&]() { runResult = server.Run(hooks); runReturned = true; });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // --- three clients join -------------------------------------------------------
    struct C { int id = -1, port = 0, max = 0; Socket *chan = nullptr; };
    std::vector<C> clients;
    std::vector<Socket *> owned;
    for (int i = 0; i < 3; ++i) {
        Socket join(SocketType::Request);
        join.SetReceiveTimeoutMs(2000);
        join.Connect("tcp://localhost:7555");
        join.Send("JOIN");
        std::string welcome;
        C c;
        if (join.Receive(welcome)) {
            std::istringstream in(welcome); std::string verb;
            in >> verb >> c.id >> c.port >> c.max;
        }
        clients.push_back(c);
    }
    bool allJoined = true, portsRight = true, maxRight = true;
    std::set<int> ids;
    for (const C &c : clients) {
        if (c.id < 0) allJoined = false;
        if (c.port != 7600 + c.id) portsRight = false;
        if (c.max != 3) maxRight = false;
        ids.insert(c.id);
    }
    Check(allJoined, "three clients joined");
    Check(ids.size() == 3, "ids are distinct", (long long)ids.size());
    Check(portsRight, "WELCOME port is clientPortBase + id");
    Check(maxRight, "WELCOME carries maxClients");
    { std::lock_guard<std::mutex> l(m); Check(joined.size() == 3, "onJoin fired once per client", (long long)joined.size()); }

    // --- each client talks on its own channel; the replies name the right id ----
    bool repliesRight = true;
    for (C &c : clients) {
        c.chan = new Socket(SocketType::Request);
        owned.push_back(c.chan);
        c.chan->SetReceiveTimeoutMs(2000);
        c.chan->Connect("tcp://localhost:" + std::to_string(c.port));
        for (int k = 0; k < 5; ++k) {
            c.chan->Send("hello" + std::to_string(k));
            std::string reply;
            if (!c.chan->Receive(reply) || reply != "REPLY " + std::to_string(c.id) + " hello" + std::to_string(k)) repliesRight = false;
        }
    }
    Check(repliesRight, "onRequest reply is routed back on the right channel with the right id");
    { std::lock_guard<std::mutex> l(m);
      Check(requestThreads.size() == 3, "requests from 3 clients ran on 3 distinct threads", (long long)requestThreads.size()); }

    // --- the world is broadcast ---------------------------------------------------
    {
        Socket sub(SocketType::Subscribe);
        sub.Subscribe("WORLD");
        sub.SetReceiveTimeoutMs(1000);
        sub.Connect("tcp://localhost:7556");
        std::string w;
        Check(sub.Receive(w) && w.rfind("WORLD tick", 0) == 0, "buildWorld output arrives on the PUB channel");
    }
    Check(steps.load() > 20, "step() is being driven from RunHeadless", steps.load());
    Check(worldsBuilt.load() > 5, "buildWorld() is being driven from the publisher", worldsBuilt.load());
    Check(server.ConnectedCount() == 3, "ConnectedCount is 3", server.ConnectedCount());

    // --- a fourth join is refused, not crashed ------------------------------------
    {
        Socket join(SocketType::Request);
        join.SetReceiveTimeoutMs(2000);
        join.Connect("tcp://localhost:7555");
        join.Send("JOIN");
        std::string reply;
        Check(join.Receive(reply) && reply == "ERR server full", "join beyond maxClients is refused");
    }

    // --- a client that says BYE frees its slot for the next JOIN -------------------
    {
        C &leaver = clients[1];
        std::string reply;
        leaver.chan->Send("BYE");
        Check(leaver.chan->Receive(reply) && reply == "BYE", "BYE is answered");
        {
            std::lock_guard<std::mutex> l(m);
            Check(leftIds.size() == 1 && leftIds[0] == leaver.id, "onLeave fired once, for that client", (long long)leftIds.size());
        }
        Check(server.ConnectedCount() == 2 && !server.IsConnected(leaver.id), "its slot is free", server.ConnectedCount());

        Socket join(SocketType::Request);
        join.SetReceiveTimeoutMs(2000);
        join.Connect("tcp://localhost:7555");
        join.Send("JOIN");
        std::string welcome, verb;
        int newId = -1, newPort = 0;
        if (join.Receive(welcome)) { std::istringstream in(welcome); in >> verb >> newId >> newPort; }
        Check(verb == "WELCOME" && newId == leaver.id, "the next JOIN is admitted into the freed slot", newId);

        Socket chan(SocketType::Request);
        chan.SetReceiveTimeoutMs(2000);
        chan.Connect("tcp://localhost:" + std::to_string(newPort));
        chan.Send("hi");
        Check(chan.Receive(reply) && reply == "REPLY " + std::to_string(newId) + " hi",
              "the slot's thread serves the newcomer on the same port");
    }

    // --- Stop brings everything home ----------------------------------------------
    server.Stop();
    for (int i = 0; i < 50 && !runReturned; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    Check(runReturned.load(), "Run() returned after Stop()");
    if (runReturned) serverThread.join(); else serverThread.detach();
    Check(runResult, "Run() reported success (no bind failures)");
    Check(server.ConnectedCount() == 0, "all client slots released on stop", server.ConnectedCount());

    for (Socket *s : owned) delete s;
    engine.Shutdown();

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures != 0;
}
