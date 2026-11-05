// engine::Client against engine::Server - both engine features, no game.
// Proves the client half of Sections 2 and 4: join, dedicated channel, the
// world arriving even while "paused", and the upstream rate following game
// time (60 / 30 / 120 / 0 per second at 1x / 0.5x / 2x / paused).
#include "engine/Client.hpp"
#include "engine/Engine.hpp"
#include "engine/Server.hpp"
#include "engine/Timeline.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

using namespace engine;

static int failures = 0;
static void Check(bool ok, const char *name, double v = 0) {
    if (ok) printf("  PASS  %s\n", name);
    else    printf("  FAIL  %s   (%.1f)\n", name, v), ++failures;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("engine::Client verification\n\n");

    Engine serverEngine;
    if (!serverEngine.InitHeadless()) return 1;
    Server::Config sc;
    sc.joinPort = 7655; sc.worldPort = 7656; sc.clientPortBase = 7700; sc.maxClients = 4; sc.logTag = "test-server";
    Server server(serverEngine, sc);

    std::atomic<int> statesReceived{0};
    std::atomic<int> worldsPublished{0};
    Server::Hooks hooks;
    hooks.onRequest = [&](int id, const std::string &req) {
        if (req.rfind("STATE", 0) == 0) ++statesReceived;
        if (req == "MAP") return std::string("MAP 42 walls");
        return "ACK " + std::to_string(id);
    };
    hooks.buildWorld = [&]() { ++worldsPublished; return std::string("WORLD everything is fine"); };
    hooks.step = [](float) {};
    std::thread serverThread([&]() { server.Run(hooks); });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    // --- Join and a one-shot request --------------------------------------------
    Client::Config cc;
    cc.joinPort = 7655; cc.worldPort = 7656;
    Client client(cc);
    Check(client.Join(), "Join succeeds against engine::Server");
    Check(client.Id() == 0 && client.ChannelPort() == 7700 && client.MaxClients() == 4,
          "id, channel port and maxClients parsed from WELCOME", client.Id());
    std::string reply;
    Check(client.Request("MAP", reply) && reply == "MAP 42 walls", "Request() gets the game's reply back");

    // --- Tick: world arrives, upstream is paced by game time --------------------
    // Drive Tick exactly as Engine's worker would: ~4ms wall sleeps, dt from a
    // Timeline we control. That makes the scale the only variable.
    auto RunFor = [&](Timeline &t, int wallMs, int &worlds) {
        int64_t last = t.GetTime();
        worlds = 0;
        int sends = 0;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wallMs);
        while (std::chrono::steady_clock::now() < deadline) {
            const int64_t now = t.GetTime();
            const float dt = (float)(now - last) / (float)Timeline::MICROSECONDS_PER_SECOND;
            last = now;
            if (client.Tick(dt, [&](const std::string &w) { if (w.rfind("WORLD", 0) == 0) ++worlds; },
                                [&]() { return std::string("STATE 1 2 3 4 1"); })) ++sends;
            std::this_thread::sleep_for(std::chrono::microseconds(4000));
        }
        return sends;
    };

    int worlds = 0;
    Timeline t1(nullptr, 1);
    const int at1x = RunFor(t1, 2000, worlds);
    printf("  1.0x : %d sends, %d worlds in 2s\n", at1x, worlds);
    Check(at1x >= 108 && at1x <= 132, "1.0x sends ~60/s", at1x / 2.0);
    Check(worlds >= 90, "world broadcast arrives (~60/s)", worlds);
    Check(client.Connected(), "Connected() after ACKs");
    Check(client.RoundTrips() == at1x, "every send got its ACK", client.RoundTrips());

    Timeline tHalf(nullptr, 1); tHalf.SetScale(0.5);
    const int atHalf = RunFor(tHalf, 2000, worlds);
    printf("  0.5x : %d sends, %d worlds in 2s\n", atHalf, worlds);
    Check(atHalf >= 52 && atHalf <= 68, "0.5x halves the message rate", atHalf / 2.0);
    Check(worlds >= 90, "...but the world still arrives at the server's rate", worlds);

    Timeline t2x(nullptr, 1); t2x.SetScale(2.0);
    const int at2x = RunFor(t2x, 2000, worlds);
    printf("  2.0x : %d sends in 2s\n", at2x);
    Check(at2x >= 210 && at2x <= 270, "2.0x doubles it", at2x / 2.0);

    Timeline tPaused(nullptr, 1); tPaused.Pause();
    const int paused = RunFor(tPaused, 1000, worlds);
    printf("  paused : %d sends, %d worlds in 1s\n", paused, worlds);
    Check(paused == 0, "paused sends nothing", paused);
    Check(worlds >= 45, "...and STILL hears the world - a shared world cannot be paused", worlds);

    Check(statesReceived.load() == at1x + atHalf + at2x, "server received exactly the sends made", statesReceived.load());

    server.Stop();
    serverThread.join();

    // --- the server goes away mid-game --------------------------------------------
    // The first send after it has gone times out; the REQ socket then refuses
    // every later one. That refusal used to be an uncaught exception: a crash.
    for (int i = 0; i < 3; ++i) {
        client.Tick(1.0f, nullptr, []() { return std::string("STATE 1 2 3 4 1"); });
    }
    Check(!client.Connected(), "with its server gone, the client reports disconnected instead of crashing");
    serverEngine.Shutdown();

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures != 0;
}
