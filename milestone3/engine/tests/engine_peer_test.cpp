// engine::Peer on its own - no game, no server. Three peers in one process:
// they claim distinct slots, converge on one epoch (earliest wins, not lowest
// id), hold one event log with payloads that contain spaces, and hear each
// other's game messages. Then the table fills and refuses politely.
#include "engine/Peer.hpp"
#include "engine/Timeline.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace engine;

static int failures = 0;
static void Check(bool ok, const char *name, long long v = 0) {
    if (ok) printf("  PASS  %s\n", name);
    else    printf("  FAIL  %s   (%lld)\n", name, v), ++failures;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("engine::Peer verification\n\n");

    Peer::Config cfg;
    cfg.portBase = 7800; cfg.maxPeers = 4; cfg.gossipMs = 100; cfg.topics = {"CHAT"};

    // --- claim: distinct ids, and the port is the id -----------------------------
    Peer p0(cfg), p1(cfg), p2(cfg);
    const int i2 = p2.Claim();          // "starts first" - claims slot 0
    const int i0 = p0.Claim();          // slot 1
    const int i1 = p1.Claim();          // slot 2
    Check(i2 == 0 && i0 == 1 && i1 == 2, "three peers claim slots 0, 1, 2 in start order", i2 * 100 + i0 * 10 + i1);
    { Peer dup(cfg); Check(dup.Claim(0) == -1 && !dup.LastError().empty(), "claiming a taken slot fails with a message"); }

    p0.ConnectAll(); p1.ConnectAll(); p2.ConnectAll();

    // --- shared clock: p2 enables first, so its epoch is earliest -----------------
    p2.EnableSharedClock();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    p0.EnableSharedClock();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    p1.EnableSharedClock();
    const int64_t earliest = p2.Epoch();

    // --- run all three for a while, recording game messages heard ---------------
    std::mutex m;
    std::set<std::string> chatHeardBy0, chatHeardBy1, chatHeardBy2;
    std::atomic<bool> stop{false};
    auto Run = [&](Peer &p, std::set<std::string> &chat) {
        while (!stop) {
            p.Tick([&](const std::string &topic, std::istringstream &body, int) {
                if (topic == "CHAT") { std::string rest; std::getline(body, rest); std::lock_guard<std::mutex> l(m); chat.insert(rest); }
            });
            std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
    };
    std::thread t0(Run, std::ref(p0), std::ref(chatHeardBy0));
    std::thread t1(Run, std::ref(p1), std::ref(chatHeardBy1));
    std::thread t2(Run, std::ref(p2), std::ref(chatHeardBy2));

    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // events with spaces in them, from two different owners
    p0.AppendEvent("fire 12.5 40.0 right");
    p1.AppendEvent("fire 99 99 up");
    p1.AppendEvent("fire 1 2 down");
    // a game message
    p2.Publish("CHAT", "hello from two");   // body arrives as " hello from two": getline keeps the separator, >> would skip it

    std::this_thread::sleep_for(std::chrono::milliseconds(700));
    stop = true;
    t0.join(); t1.join(); t2.join();

    // --- epoch converged, and to the EARLIEST, not the lowest id -----------------
    Check(p0.Epoch() == earliest && p1.Epoch() == earliest && p2.Epoch() == earliest, "all three hold one epoch");
    Check(p0.EpochProposer() == 0 && p1.EpochProposer() == 0, "the earliest starter's epoch won (it holds slot 0 here)", p0.EpochProposer());
    Check(p0.AgreesOnEpoch(0) && p0.AgreesOnEpoch(2) && p1.AgreesOnEpoch(0) && p1.AgreesOnEpoch(1),
          "AgreesOnEpoch() sees agreement across the mesh");
    Check(p0.EpochAdoptions() >= 1 && p1.EpochAdoptions() >= 1 && p2.EpochAdoptions() == 0,
          "late starters adopted; the earliest never had to");
    {
        const double a = p0.SharedSeconds(), b = p1.SharedSeconds(), c = p2.SharedSeconds();
        const double spread = std::max(a, std::max(b, c)) - std::min(a, std::min(b, c));
        Check(spread < 0.005, "shared clocks read the same time (<5ms apart)", (long long)(spread * 1e6));
    }

    // --- event log converged, with dedupe and spaces intact ------------------------
    Check(p0.EventCount() == 3 && p1.EventCount() == 3 && p2.EventCount() == 3,
          "every peer holds all 3 events, none duplicated by re-gossip", (long long)p2.EventCount());
    {
        bool payloadsIntact = false;
        for (const Peer::Event &e : p2.Events()) if (e.owner == 1 && e.payload == "fire 12.5 40.0 right") payloadsIntact = true;
        Check(payloadsIntact, "payloads with spaces survive length-prefixed gossip");
        std::set<std::pair<int, int64_t>> ids;
        for (const Peer::Event &e : p0.Events()) ids.insert({e.owner, e.seq});
        Check(ids.size() == 3, "(owner, seq) identities are distinct", (long long)ids.size());
    }
    Check(p0.TakeEventsDirty() && !p0.TakeEventsDirty(), "TakeEventsDirty() reports once, then clears");

    // --- game messages reach everyone but the sender ------------------------------
    Check(chatHeardBy0.count(" hello from two") && chatHeardBy1.count(" hello from two"), "Publish() reaches the other peers via onMessage", (long long)chatHeardBy0.size());
    Check(chatHeardBy2.empty(), "a peer does not hear itself");

    // --- a peer leaves, and a new process takes its slot ---------------------------
    // Both of these used to fail, leaving the newcomer's world different from
    // everyone else's: its event ids restarted at the departed peer's and were
    // dropped as duplicates, and nobody re-sent the departed peer's events.
    {
        Peer::Config c = cfg;
        c.portBase = 7900;
        auto Pump = [](std::vector<Peer *> peers, int ms) {
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
            while (std::chrono::steady_clock::now() < until) {
                for (Peer *p : peers) p->Tick(nullptr);
                std::this_thread::sleep_for(std::chrono::milliseconds(4));
            }
        };
        auto Ids = [](const Peer &p) {
            std::set<std::pair<int, int64_t>> s;
            for (const Peer::Event &e : p.Events()) s.insert({e.owner, e.seq});
            return s;
        };

        Peer stayer(c);
        stayer.Claim(1);
        stayer.ConnectAll();
        auto departed = std::make_unique<Peer>(c);
        departed->Claim(0);
        departed->ConnectAll();
        departed->AppendEvent("fired before leaving");
        departed->AppendEvent("fired again before leaving");
        Pump({&stayer, departed.get()}, 600);
        Check(stayer.EventCount() == 2, "the staying peer has the departing peer's 2 events", (long long)stayer.EventCount());

        departed.reset();
        Peer newcomer(c);
        Check(newcomer.Claim(0) == 0, "a newcomer takes the departed peer's slot");
        newcomer.ConnectAll();
        newcomer.AppendEvent("the newcomer fires");
        Pump({&stayer, &newcomer}, 800);

        Check(stayer.EventCount() == 3, "the others accept the newcomer's events", (long long)stayer.EventCount());
        Check(newcomer.EventCount() == 3, "the newcomer learns the departed peer's events", (long long)newcomer.EventCount());
        Check(Ids(stayer) == Ids(newcomer), "both hold the identical log");
    }

    // --- the table fills ----------------------------------------------------------
    Peer p3(cfg), p4(cfg);
    Check(p3.Claim() == 3, "fourth peer takes the last slot", p3.Claim());
    Check(p4.Claim() == -1 && p4.LastError().find("all 4") != std::string::npos, "fifth is refused with 'all 4 peer slots are taken'");

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures != 0;
}
