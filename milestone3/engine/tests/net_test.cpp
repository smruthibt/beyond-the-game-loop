// Exercises the engine's Socket wrapper: REQ/REP across threads, PUB/SUB with
// topic filtering, and the receive timeout that keeps a dead peer from wedging
// a REQ socket forever. Also the reason LINGER is 0: this test hung at exit
// before that fix.
#include "engine/Network.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>

using namespace engine::net;

static int failures = 0;
static void Check(bool ok, const char *name) {
    printf(ok ? "  PASS  %s\n" : "  FAIL  %s\n", name);
    if (!ok) ++failures;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("Network wrapper verification\n\n");

    // --- REQ/REP round trip, server on its own thread ----------------------
    {
        std::atomic<bool> ready{false};
        std::thread server([&ready]() {
            Socket rep(SocketType::Reply);
            rep.Bind("tcp://*:5599");
            ready = true;
            std::string request;
            for (int i = 0; i < 3; ++i) {
                if (rep.Receive(request)) {
                    rep.Send("echo:" + request);
                }
            }
        });
        while (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(1));

        Socket req(SocketType::Request);
        req.SetReceiveTimeoutMs(2000);
        req.Connect("tcp://localhost:5599");

        bool allOk = true;
        for (int i = 0; i < 3; ++i) {
            const std::string payload = "msg" + std::to_string(i);
            req.Send(payload);
            std::string reply;
            if (!req.Receive(reply) || reply != "echo:" + payload) allOk = false;
        }
        Check(allOk, "REQ/REP round trip across threads (3 exchanges)");
        server.join();
    }

    // --- PUB/SUB, with the slow-joiner problem handled ---------------------
    {
        std::atomic<bool> stop{false};
        std::thread publisher([&stop]() {
            Socket pub(SocketType::Publish);
            pub.Bind("tcp://*:5598");
            // A SUB that has only just connected misses early messages, so
            // publish repeatedly rather than once.
            while (!stop) {
                pub.Send("PLAT 1 0 100.0 200.0");
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });

        Socket sub(SocketType::Subscribe);
        sub.Connect("tcp://localhost:5598");
        sub.Subscribe("PLAT");
        sub.SetReceiveTimeoutMs(3000);

        std::string update;
        const bool got = sub.Receive(update);
        Check(got && update.rfind("PLAT", 0) == 0, "PUB/SUB delivers a topic-filtered message");

        stop = true;
        publisher.join();
    }

    // --- SUB topic filtering actually filters ------------------------------
    {
        std::atomic<bool> stop{false};
        std::thread publisher([&stop]() {
            Socket pub(SocketType::Publish);
            pub.Bind("tcp://*:5597");
            while (!stop) {
                pub.Send("OTHER should-not-arrive");
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        });

        Socket sub(SocketType::Subscribe);
        sub.Connect("tcp://localhost:5597");
        sub.Subscribe("PLAT");          // never matches what is being sent
        sub.SetReceiveTimeoutMs(400);

        std::string update;
        Check(!sub.Receive(update), "SUB filters out non-matching topics");

        stop = true;
        publisher.join();
    }

    // --- a receive timeout returns rather than hanging ---------------------
    {
        Socket req(SocketType::Request);
        req.SetReceiveTimeoutMs(300);
        req.Connect("tcp://localhost:5596");   // nothing is listening here
        req.Send("anyone?");

        const auto start = std::chrono::steady_clock::now();
        std::string reply;
        const bool got = req.Receive(reply);
        const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();

        Check(!got && waited >= 250 && waited < 1500, "Receive times out instead of hanging");
    }

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED",
           failures, failures == 1 ? "" : "s");
    return failures != 0;
}
