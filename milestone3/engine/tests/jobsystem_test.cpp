// Section 3 - the JobSystem adapted from the Module 6 ThreadExample.
//
// Test 1 is the ThreadExample's own Part 2 check, reproduced exactly: fifty
// jobs, the even ones increment a counter, expect 25. The rest probe what the
// example does not - repeated frames, late workers, and what the persistence
// actually costs.
#include "engine/JobSystem.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

using namespace engine;

static int failures = 0;
static void Check(bool ok, const char *name, long long measured = 0) {
    if (ok) printf("  PASS  %s\n", name);
    else    printf("  FAIL  %s   (measured %lld)\n", name, measured), ++failures;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("JobSystem verification\n\n");

    // --- 1. The ThreadExample's Part 2, verbatim ----------------------------
    {
        JobSystem jobs(2);
        std::atomic<long long> counter{0};
        for (int i = 0; i < 50; ++i) {
            jobs.Submit([i, &counter]() {
                if (i % 2 == 0) counter++;
            });
        }
        jobs.RunAndWait();
        printf("  Expected from jobs: 25 | Actual: %lld\n", counter.load());
        Check(counter == 25, "ThreadExample Part 2: even jobs counted once each", counter);
    }

    // --- 2. Every job runs exactly once -------------------------------------
    {
        JobSystem jobs(3);
        const int N = 2000;
        std::vector<std::atomic<int>> ran(N);
        for (auto &r : ran) r = 0;
        for (int i = 0; i < N; ++i) {
            jobs.Submit([i, &ran]() { ran[i]++; });
        }
        jobs.RunAndWait();
        int missed = 0, doubled = 0;
        for (auto &r : ran) { if (r == 0) ++missed; if (r > 1) ++doubled; }
        Check(missed == 0 && doubled == 0, "2000 jobs: none missed, none run twice", missed * 1000 + doubled);
    }

    // --- 3. Work actually lands on more than one thread ---------------------
    {
        JobSystem jobs(2);
        std::mutex m;
        std::set<std::thread::id> seen;
        for (int i = 0; i < 400; ++i) {
            jobs.Submit([&m, &seen]() {
                // Enough work that the producer cannot finish it all alone
                // before the pool wakes up.
                volatile double x = 0;
                for (int k = 0; k < 20000; ++k) x += k * 0.5;
                std::lock_guard<std::mutex> lock(m);
                seen.insert(std::this_thread::get_id());
            });
        }
        jobs.RunAndWait();
        printf("  distinct threads that ran jobs: %zu (pool of 2 + producer)\n", seen.size());
        Check(seen.size() >= 2, "jobs run on distinct threads", (long long)seen.size());
    }

    // --- 4. Frame after frame, with the pool staying alive ------------------
    // This is what the ThreadExample never does, and where a late-waking
    // worker from frame N could corrupt frame N+1 if the protocol were wrong.
    {
        JobSystem jobs(2);
        bool allFramesCorrect = true;
        for (int frame = 0; frame < 2000; ++frame) {
            std::atomic<int> sum{0};
            const int jobsThisFrame = 1 + (frame % 7);   // vary the count
            for (int j = 0; j < jobsThisFrame; ++j) {
                jobs.Submit([&sum]() { sum++; });
            }
            jobs.RunAndWait();
            if (sum != jobsThisFrame) { allFramesCorrect = false; break; }
        }
        Check(allFramesCorrect, "2000 consecutive frames, varying job counts, all exact");
    }

    // --- 5. An empty frame is a no-op, not a hang ---------------------------
    {
        JobSystem jobs(2);
        jobs.RunAndWait();
        Check(true, "RunAndWait with no jobs returns immediately");
    }

    // --- 6. What persistence costs - the number for the writeup --------------
    {
        JobSystem jobs(2);
        const int FRAMES = 2000;

        // Two trivial jobs, the shape of the gulls+ghosts update.
        std::atomic<int> sink{0};
        auto t0 = std::chrono::steady_clock::now();
        for (int f = 0; f < FRAMES; ++f) {
            jobs.Submit([&sink]() { sink++; });
            jobs.Submit([&sink]() { sink++; });
            jobs.RunAndWait();
        }
        auto t1 = std::chrono::steady_clock::now();
        const double perFrameUs =
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count() / 1000.0 / FRAMES;

        // Same two jobs, called directly on one thread.
        auto t2 = std::chrono::steady_clock::now();
        for (int f = 0; f < FRAMES; ++f) { sink++; sink++; }
        auto t3 = std::chrono::steady_clock::now();
        const double directUs =
            std::chrono::duration_cast<std::chrono::nanoseconds>(t3 - t2).count() / 1000.0 / FRAMES;

        // And what the ThreadExample's one-shot model would cost: spawn, drain, join.
        auto t4 = std::chrono::steady_clock::now();
        for (int f = 0; f < 200; ++f) {
            std::atomic<size_t> next{0};
            std::vector<std::function<void()>> list = {[&sink]{ sink++; }, [&sink]{ sink++; }};
            auto worker = [&]{ while (true) { size_t i = next.fetch_add(1); if (i >= list.size()) break; list[i](); } };
            std::thread a(worker), b(worker);
            a.join(); b.join();
        }
        auto t5 = std::chrono::steady_clock::now();
        const double oneShotUs =
            std::chrono::duration_cast<std::chrono::nanoseconds>(t5 - t4).count() / 1000.0 / 200;

        printf("\n  per-frame cost of dispatching 2 trivial jobs:\n");
        printf("    direct call, one thread        : %8.2f us\n", directUs);
        printf("    persistent pool (ours)         : %8.2f us\n", perFrameUs);
        printf("    one-shot spawn+join (example)  : %8.2f us\n\n", oneShotUs);

        Check(perFrameUs < oneShotUs, "persistent pool is cheaper than spawning per frame");
        Check(perFrameUs < 8333.0 * 0.25, "dispatch fits comfortably inside a 120Hz tick");
    }

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED",
           failures, failures == 1 ? "" : "s");
    return failures != 0;
}
