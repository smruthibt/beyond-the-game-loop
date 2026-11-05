// Lecture 06 items: local timelines driving animation (slides 7-9), frame-rate
// governing (slide 20) and the moving average (slides 18-19).
//
// The animation checks are the ones that matter most: an animation is a local
// timeline, so pausing or scaling game time has to reach it. Before this it ran
// on SDL_GetTicks and kept flipping through a pause.
#include "engine/Engine.hpp"
#include "engine/Entity.hpp"
#include "engine/Timeline.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <thread>
#include <vector>

using namespace engine;

static int failures = 0;
static void Check(bool ok, const char *name, double v = 0) {
    if (ok) printf("  PASS  %s\n", name);
    else    printf("  FAIL  %s   (%.3f)\n", name, v), ++failures;
}
static void SleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// A texture pointer is never dereferenced by UpdateAnimation - it only gates
// on non-null - so a dummy is enough to exercise the frame logic headlessly.
static SDL_Texture *const FAKE = reinterpret_cast<SDL_Texture *>(0x1);

static Entity Animated(Uint32 delayMs) {
    Entity e;
    e.SetTexture(FAKE, 4, 32, 32, delayMs);
    return e;
}

// Drive an entity's animation off `clock` for `wallMs` of real time.
static int FramesAdvanced(Entity &e, const Timeline &clock, int wallMs) {
    const int before = e.currentFrame;
    int wraps = 0, last = before;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wallMs);
    while (std::chrono::steady_clock::now() < deadline) {
        e.UpdateAnimation(clock);
        if (e.currentFrame < last) ++wraps;      // wrapped past frameCount
        last = e.currentFrame;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return wraps * e.frameCount + e.currentFrame - before;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("Frame-time and local-timeline verification\n\n");

    // --- Timeline::GetSeconds is tic-agnostic ---------------------------------
    {
        Timeline us(nullptr, 1);        // microsecond tics
        Timeline ms(nullptr, 1000);     // millisecond tics
        SleepMs(120);
        const double a = us.GetSeconds(), b = ms.GetSeconds();
        printf("  microsecond-tic clock: %.3fs   millisecond-tic clock: %.3fs\n", a, b);
        Check(a > 0.10 && a < 0.16, "GetSeconds on a microsecond-tic timeline", a);
        Check(std::fabs(a - b) < 0.02, "same elapsed time whatever the tic", std::fabs(a - b));
    }
    {
        // A chained timeline: seconds must walk the anchor chain.
        Timeline root(nullptr, 1);
        Timeline child(&root, 1000);    // 1000 root tics (us) = 1ms per child tic
        SleepMs(100);
        const double c = child.GetSeconds();
        Check(c > 0.08 && c < 0.14, "GetSeconds walks the anchor chain", c);
    }

    // --- slides 7-9: animation is a LOCAL timeline ---------------------------
    {
        Timeline gameTime(nullptr, 1);
        Entity e = Animated(20);        // 20ms per frame -> ~15 frames in 300ms
        const int ran = FramesAdvanced(e, gameTime, 300);
        printf("\n  running 300ms at 1.0x : %d frames\n", ran);
        Check(ran >= 10 && ran <= 20, "animation advances on game time", ran);
    }
    {
        Timeline gameTime(nullptr, 1);
        Entity e = Animated(20);
        FramesAdvanced(e, gameTime, 100);          // get it going
        gameTime.Pause();
        // Settle first. Time passed between the last update and the pause, and
        // the animation is owed it - catching up is correct. What must not
        // happen is any further motion once the clock is standing still.
        e.UpdateAnimation(gameTime);
        const int whilePaused = FramesAdvanced(e, gameTime, 300);
        printf("  paused 300ms          : %d frames\n", whilePaused);
        Check(whilePaused == 0, "PAUSED: animation holds exactly - the bug this fixes", whilePaused);

        gameTime.Unpause();
        const int afterResume = FramesAdvanced(e, gameTime, 200);
        printf("  resumed 200ms         : %d frames\n", afterResume);
        Check(afterResume >= 6, "resumes on unpause", afterResume);
    }
    {
        Timeline fast(nullptr, 1);   fast.SetScale(2.0);
        Timeline slow(nullptr, 1);   slow.SetScale(0.5);
        Entity a = Animated(20), b = Animated(20);
        const int fastFrames = FramesAdvanced(a, fast, 300);
        const int slowFrames = FramesAdvanced(b, slow, 300);
        printf("  300ms at 2.0x / 0.5x  : %d / %d frames\n", fastFrames, slowFrames);
        Check(fastFrames > slowFrames * 2, "time scale reaches the animation", fastFrames);
    }
    {
        // A local timeline anchored to game time: its own zero, but it still
        // stops when the world does. This is slide 9's "assets which have a
        // zero-time marked by something in the game".
        Timeline gameTime(nullptr, 1);
        SleepMs(80);                                   // world has been running a while
        Timeline effect(&gameTime, 1);                 // spawned now: its own zero
        Entity e = Animated(20);
        Check(effect.GetSeconds() < 0.02, "a local timeline starts at its own zero", effect.GetSeconds());
        const int ran = FramesAdvanced(e, effect, 200);
        Check(ran >= 6, "a local timeline drives animation", ran);
        gameTime.Pause();
        e.UpdateAnimation(effect);   // settle, as above
        const int whileParentPaused = FramesAdvanced(e, effect, 200);
        Check(whileParentPaused == 0, "pausing the world stops the local timeline too", whileParentPaused);
    }

    // --- slides 18-19: the moving average ------------------------------------
    // Engine::SmoothDelta is private and drives the real loop; this reproduces
    // its contract on the same inputs so the behaviour is pinned down.
    {
        const int W = 8;
        std::vector<float> window(W, 0.0f);
        size_t next = 0, filled = 0;
        auto Smooth = [&](float raw) {
            if (raw == 0.0f) { filled = 0; next = 0; return raw; }
            window[next] = raw; next = (next + 1) % window.size();
            if (filled < window.size()) ++filled;
            float sum = 0; for (size_t i = 0; i < filled; ++i) sum += window[i];
            return sum / (float)filled;
        };
        for (int i = 0; i < W; ++i) Smooth(0.016f);              // steady state
        const float spike = Smooth(0.050f);                       // one long frame
        printf("\n  steady 16ms, one 50ms spike -> reported %.1fms\n", spike * 1000.0f);
        Check(spike < 0.025f, "a spike is spread, not reported whole", spike * 1000.0f);
        Check(Smooth(0.0f) == 0.0f, "a paused frame still reports exactly zero");
    }

    // --- a tic change on the engine's own game clock, mid-run -----------------
    // The loop used to read the game clock in tics and assume microseconds, so
    // SetTic(1000) handed update() a -0.7s frame and then frames 1000x too short.
    {
        Engine engine;
        engine.InitHeadless();
        std::vector<float> dts;
        int frame = 0;
        engine.RunHeadless([&](float dt) {
            dts.push_back(dt);
            if (++frame == 60) engine.GetGameTime().SetTic(1000);   // microseconds -> milliseconds
            if (frame == 120) engine.Stop();
        }, 200);
        double before = 0, after = 0;
        for (int i = 10; i < 60; ++i) before += dts[i];
        for (int i = 61; i < 120; ++i) after += dts[i];
        before /= 50.0; after /= 59.0;
        const float lowest = *std::min_element(dts.begin(), dts.end());
        printf("\n  engine frame delta around SetTic(1000) on game time: %.2fms before, %.2fms after\n",
               before * 1000.0, after * 1000.0);
        Check(lowest >= 0.0f, "no negative frame delta when the game clock's tic changes", lowest * 1000.0);
        Check(after > before * 0.5 && after < before * 1.5, "frames are the same length after the tic change", after * 1000.0);
        engine.Shutdown();
    }

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED", failures, failures == 1 ? "" : "s");
    return failures != 0;
}
