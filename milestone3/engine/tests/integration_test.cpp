// Section 1 - drives Timeline + Physics + Entity exactly the way Engine::Run
// does, to show that pause and scale reach entity movement without any entity
// knowing about either.
#include "engine/Entity.hpp"
#include "engine/Physics.hpp"
#include "engine/Timeline.hpp"

#include <chrono>
#include <cstdio>
#include <thread>

using namespace engine;

static int failures = 0;

static void Check(bool ok, const char *name, double measured = 0.0) {
    printf(ok ? "  PASS  %s\n" : "  FAIL  %s   (measured %.1f)\n", name, measured);
    if (!ok) ++failures;
}

// One frame of Engine::Run's delta computation, verbatim.
static float FrameDelta(Timeline &t, double &lastSeconds) {
    const double now = t.GetSeconds();
    float dt = (float)(now - lastSeconds);
    lastSeconds = now;
    if (dt < 0.0f) dt = 0.0f;
    const float MAX_DT = 1.0f / 20.0f;
    return dt > MAX_DT ? MAX_DT : dt;
}

// Spins a ~60fps loop for `wallMs` of real time and returns distance travelled.
static float RunFor(Timeline &t, Entity &e, int wallMs) {
    const float startY = e.position.y;
    double last = t.GetSeconds();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wallMs);
    while (std::chrono::steady_clock::now() < deadline) {
        Physics::Update(e, FrameDelta(t, last));
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
    return e.position.y - startY;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("Timeline -> Physics integration\n\n");
    Physics::SetGravity(0.0f); // isolate: constant velocity, no acceleration

    Timeline gameTime(nullptr, 1); // microsecond tics, as Engine holds it
    Entity e;
    e.velocity = {0.0f, 100.0f};   // 100 px/s downward

    const float moved = RunFor(gameTime, e, 300);
    Check(moved > 20.0f && moved < 45.0f, "runs at 1.0x (~30px in 300ms)", moved);

    gameTime.Pause();
    const float whilePaused = RunFor(gameTime, e, 300);
    Check(whilePaused == 0.0f, "paused: entity is stationary", whilePaused);

    gameTime.Unpause();
    const float afterResume = RunFor(gameTime, e, 300);
    Check(afterResume > 20.0f && afterResume < 45.0f, "resumes from where it stopped", afterResume);

    gameTime.SetScale(2.0);
    const float fast = RunFor(gameTime, e, 300);
    Check(fast > 45.0f && fast < 85.0f, "2.0x moves ~twice as far in the same wall time", fast);

    gameTime.SetScale(0.5);
    const float slow = RunFor(gameTime, e, 300);
    Check(slow > 8.0f && slow < 25.0f, "0.5x moves ~half as far", slow);

    // --- Section 1 meets Section 4: game-time pacing of network sends -------
    // This is the client's upstream loop, reproduced: the worker ticks at
    // 120Hz wall time, accumulates GAME-time dt, and sends once per 1/60 s of
    // game time. So the timeline scale should move the send rate directly -
    // which is the exact sentence in Section 4.
    printf("\nGame-time send pacing (the client's worker loop)\n\n");
    {
        auto MeasureSendsPerSecond = [](double scale, bool paused) {
            Timeline t(nullptr, 1);
            t.SetScale(scale);
            if (paused) t.Pause();

            double last = t.GetSeconds();
            float accumulator = 0.0f;
            int sends = 0;
            const float SEND_INTERVAL = 1.0f / 60.0f;

            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (std::chrono::steady_clock::now() < deadline) {
                accumulator += FrameDelta(t, last);
                if (accumulator >= SEND_INTERVAL) {
                    accumulator -= SEND_INTERVAL;
                    if (accumulator > SEND_INTERVAL) accumulator = SEND_INTERVAL;
                    ++sends;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(4000)); // the engine's worker tick
            }
            return sends / 2.0;
        };

        const double at1x   = MeasureSendsPerSecond(1.0, false);
        const double atHalf = MeasureSendsPerSecond(0.5, false);
        const double at2x   = MeasureSendsPerSecond(2.0, false);
        const double paused = MeasureSendsPerSecond(1.0, true);

        printf("  scale 1.0x : %5.1f sends/s\n", at1x);
        printf("  scale 0.5x : %5.1f sends/s\n", atHalf);
        printf("  scale 2.0x : %5.1f sends/s\n", at2x);
        printf("  paused     : %5.1f sends/s\n\n", paused);

        Check(at1x >= 54 && at1x <= 66,     "1.0x sends ~60/s", at1x);
        Check(atHalf >= 26 && atHalf <= 34, "0.5x halves the 0MQ message rate", atHalf);
        Check(at2x >= 105 && at2x <= 135,   "2.0x doubles it", at2x);
        Check(paused == 0.0,                "paused sends nothing", paused);
    }

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED",
           failures, failures == 1 ? "" : "s");
    return failures != 0;
}
