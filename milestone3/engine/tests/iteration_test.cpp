// Section 1's "loop iterations" time scale, on the real engine loop: the raw
// pass count must match the loop EXACTLY and ignore the game clock; the same
// count as a Timeline must support tic, anchoring, scale and pause.
#include "engine/Engine.hpp"

#include <cstdio>
#include <functional>

using namespace engine;

static int failures = 0;
static void Check(bool ok, const char *name, long long measured = 0) {
    if (ok) {
        printf("  PASS  %s\n", name);
    } else {
        printf("  FAIL  %s   (measured %lld)\n", name, measured);
        ++failures;
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("Loop-iteration count verification\n\n");

    Engine engine;
    if (!engine.InitHeadless()) {
        printf("FAILED (could not init SDL headless)\n");
        return 1;
    }

    // --- an exact count, not an estimate ------------------------------------
    {
        const int64_t TARGET = 20;
        int64_t seen = 0;
        engine.RunHeadless(
            [&](float) {
                if (++seen >= TARGET) engine.Stop();
            },
            /*tickHz=*/500);

        printf("  ran to completion: %lld update() calls, engine reports %lld\n",
               (long long)seen, (long long)engine.GetIterationCount());
        Check(engine.GetIterationCount() == TARGET,
              "GetIterationCount() matches the loop's own pass count exactly",
              (long long)engine.GetIterationCount());
    }

    // --- independent of Timeline: a paused game clock does not pause it ----
    {
        const int64_t before = engine.GetIterationCount();
        const int64_t TARGET = before + 20;
        engine.GetGameTime().Pause();

        bool everyDtWasZero = true;
        engine.RunHeadless(
            [&](float dt) {
                if (dt != 0.0f) everyDtWasZero = false;
                if (engine.GetIterationCount() >= TARGET) engine.Stop();
            },
            /*tickHz=*/500);

        printf("  ran 20 more passes with the game timeline paused\n");
        Check(everyDtWasZero, "game time really was paused for every one of those passes");
        Check(engine.GetIterationCount() == TARGET,
              "a paused game timeline does not pause the iteration count",
              (long long)engine.GetIterationCount());
        engine.GetGameTime().Unpause();
    }

    // --- the same count as a Timeline ---------------------------------------
    {
        Timeline &loop = engine.GetIterationTime();
        Timeline everyFive(&loop, 5);
        const int64_t start = loop.GetTime();
        auto RunPasses = [&](int n, const std::function<void()> &eachPass) {
            const int64_t target = engine.GetIterationCount() + n;
            engine.RunHeadless(
                [&](float) {
                    if (eachPass) eachPass();
                    if (engine.GetIterationCount() >= target) engine.Stop();
                },
                /*tickHz=*/500);
        };

        RunPasses(20, nullptr);
        Check(loop.GetTime() - start == 20, "the iteration timeline advances one tic per pass", loop.GetTime() - start);
        Check(everyFive.GetTime() == 4, "a timeline anchored to it with tic 5 counts every fifth pass", everyFive.GetTime());

        loop.SetScale(0.5);
        const int64_t beforeHalf = loop.GetTime();
        RunPasses(20, [&]() { loop.GetTime(); });
        Check(loop.GetTime() - beforeHalf == 10, "at 0.5x it counts half the passes, read on every one", loop.GetTime() - beforeHalf);
        loop.SetScale(1.0);

        loop.Pause();
        const int64_t atPause = loop.GetTime(), countAtPause = engine.GetIterationCount();
        RunPasses(20, nullptr);
        Check(loop.GetTime() == atPause, "paused, it stops while the loop keeps running", loop.GetTime() - atPause);
        Check(engine.GetIterationCount() == countAtPause + 20, "the raw count does not pause with it", engine.GetIterationCount() - countAtPause);
        loop.Unpause();
    }

    engine.Shutdown();
    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures,
           failures == 1 ? "" : "s");
    return failures != 0;
}
