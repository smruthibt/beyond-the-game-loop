// Section 1 - Timeline properties, driven against the real class with real
// wall-clock sleeps. Tolerances are loose on purpose: sleep is not exact.
#include "engine/Timeline.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

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

static void SleepMs(int ms) {
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("Timeline verification\n\n");

    // 1. A real-time-anchored timeline tracks real time.
    {
        Timeline t(nullptr, 1000); // 1000us tics == milliseconds
        const int64_t a = t.GetTime();
        SleepMs(100);
        const int64_t elapsed = t.GetTime() - a;
        Check(elapsed >= 90 && elapsed <= 140, "real time advances ~100ms", elapsed);
    }

    // 2. Pausing stops it dead.
    {
        Timeline t(nullptr, 1000);
        t.Pause();
        const int64_t a = t.GetTime();
        SleepMs(100);
        const int64_t elapsed = t.GetTime() - a;
        Check(elapsed == 0, "paused timeline does not advance", elapsed);
    }

    // 3. The paused span is skipped, not paid out on unpause.
    {
        Timeline t(nullptr, 1000);
        SleepMs(50);
        t.Pause();
        const int64_t atPause = t.GetTime();
        SleepMs(200);
        t.Unpause();
        const int64_t credited = t.GetTime() - atPause;
        Check(credited <= 10, "unpause does not credit the paused span", credited);
    }

    // 4. Half speed.
    {
        Timeline t(nullptr, 1000);
        t.SetScale(0.5);
        const int64_t a = t.GetTime();
        SleepMs(200);
        const int64_t elapsed = t.GetTime() - a;
        Check(elapsed >= 80 && elapsed <= 130, "scale 0.5 halves the rate", elapsed);
    }

    // 5. Double speed.
    {
        Timeline t(nullptr, 1000);
        t.SetScale(2.0);
        const int64_t a = t.GetTime();
        SleepMs(100);
        const int64_t elapsed = t.GetTime() - a;
        Check(elapsed >= 180 && elapsed <= 250, "scale 2.0 doubles the rate", elapsed);
    }

    // 6. The assignment's stated range is enforced.
    {
        Timeline t;
        t.SetScale(10.0);
        Check(t.GetScale() == Timeline::MAX_SCALE, "scale clamps to 2.0");
        t.SetScale(0.01);
        Check(t.GetScale() == Timeline::MIN_SCALE, "scale clamps to 0.5");
    }

    // 7. A scale change applies forward, never retroactively.
    {
        Timeline t(nullptr, 1000);
        SleepMs(100);
        t.SetScale(2.0);
        SleepMs(100);
        const int64_t total = t.GetTime(); // ~100 at 1x, then ~200 at 2x
        Check(total >= 270 && total <= 340, "scale change is not retroactive", total);
    }

    // 8. Anchoring composes, and pausing a parent stops its children.
    {
        Timeline parent(nullptr, 1);      // microsecond tics
        Timeline child(&parent, 1000);    // 1000 parent tics == 1ms
        const int64_t a = child.GetTime();
        SleepMs(100);
        const int64_t tracked = child.GetTime() - a;
        Check(tracked >= 90 && tracked <= 140, "child tracks its anchor", tracked);

        parent.Pause();
        const int64_t b = child.GetTime();
        SleepMs(100);
        const int64_t whileParentPaused = child.GetTime() - b;
        Check(whileParentPaused == 0, "child stops when its anchor pauses", whileParentPaused);
    }

    // 9. Scale composes down the chain: a 0.5x child of a 0.5x parent is 0.25x.
    {
        Timeline parent(nullptr, 1);
        parent.SetScale(0.5);
        Timeline child(&parent, 1000);
        child.SetScale(0.5);
        const int64_t a = child.GetTime();
        SleepMs(400);
        const int64_t elapsed = child.GetTime() - a;
        Check(elapsed >= 80 && elapsed <= 130, "nested scaling multiplies", elapsed);
    }

    // 10. Two timelines given the same origin read the same time - the basis
    //     of a shared world clock with no one in charge of it.
    {
        const int64_t origin = Timeline::RealTimeNow();
        SleepMs(30);
        Timeline a(nullptr, 1000);
        SleepMs(30);
        Timeline b(nullptr, 1000);          // constructed 30ms after a
        a.SetOrigin(origin);
        b.SetOrigin(origin);
        SleepMs(50);
        const int64_t ta = a.GetTime();
        const int64_t tb = b.GetTime();
        Check(ta >= 100 && ta <= 140, "SetOrigin measures from the given moment (~110ms)", ta);
        Check(std::llabs(ta - tb) <= 3, "two timelines with one origin agree", std::llabs(ta - tb));
    }

    // 11. Adopting an earlier origin jumps a late starter forward to match.
    {
        const int64_t origin = Timeline::RealTimeNow();
        SleepMs(100);
        Timeline late(nullptr, 1000);        // its own zero is 100ms after origin
        const int64_t before = late.GetTime();
        late.SetOrigin(origin);
        const int64_t after = late.GetTime();
        Check(before <= 5, "late starter reads ~0 on its own clock", before);
        Check(after >= 95 && after <= 130, "after adopting the origin it reads ~100ms", after);
    }

    // 12. A scaled child of a coarse parent keeps its time however often it is
    //     read. Before, every read rounded (parent delta * scale) down and threw
    //     the fraction away, so a 0.5x child of a 0.1s clock never moved.
    {
        Timeline tenths(nullptr, 100000);    // 0.1s tics
        Timeline readOften(&tenths, 1);   readOften.SetScale(0.5);
        Timeline readOnce(&tenths, 1);    readOnce.SetScale(0.5);
        for (int i = 0; i < 75; ++i) { readOften.GetTime(); SleepMs(16); }
        const int64_t parent = tenths.GetTime(), often = readOften.GetTime(), once = readOnce.GetTime();
        printf("  0.1s parent: %lld tics   0.5x child read every frame: %lld   read once: %lld\n",
               (long long)parent, (long long)often, (long long)once);
        Check(often >= parent / 2 - 1 && often <= parent / 2 + 1, "a 0.5x child of a coarse parent keeps half its time", often);
        Check(std::llabs(often - once) <= 1, "reading a timeline often does not change its value", std::llabs(often - once));
    }

    // 13. Changing a parent's tic changes only how the parent reports time. A
    //     child reads its parent's accumulated time, not its tics, so it keeps
    //     its seconds. Before, raising the tic threw a child 1000x forward.
    {
        Timeline parent(nullptr, 1);
        Timeline child(&parent, 1);
        SleepMs(150);
        const double before = child.GetSeconds();
        parent.SetTic(1000);                  // microseconds -> milliseconds
        const double raised = child.GetSeconds();
        parent.SetTic(1);                     // and back
        const double lowered = child.GetSeconds();
        printf("  child seconds: %.4f, after parent tic 1->1000: %.4f, after 1000->1: %.4f\n", before, raised, lowered);
        Check(raised >= before - 0.002 && raised <= before + 0.002,
              "raising a parent's tic leaves its child's time alone", (long long)(raised * 1000));
        Check(lowered >= raised && lowered <= raised + 0.002,
              "lowering it again does too", (long long)(lowered * 1000));
    }

    // 14. Loop iterations, as a Timeline: anchored to a counter that goes up by
    //     one per pass, with tic, anchoring, scale and pause behaving as usual.
    {
        std::atomic<int64_t> passes{0};
        Timeline loop(passes, 1);
        Timeline everyTen(&loop, 10);
        Timeline half(passes, 1);    half.SetScale(0.5);
        Timeline pausable(passes, 1);
        for (int i = 1; i <= 100; ++i) {
            ++passes;
            half.GetTime();                       // read every pass: the rounding case
            if (i == 40) pausable.Pause();
            if (i == 70) pausable.Unpause();
        }
        Check(loop.GetTime() == 100, "a counter-anchored timeline counts passes", loop.GetTime());
        Check(everyTen.GetTime() == 10, "anchored to it with tic 10, one tic per ten passes", everyTen.GetTime());
        Check(half.GetTime() == 50, "at 0.5x it counts half the passes, read on every one", half.GetTime());
        Check(pausable.GetTime() == 70, "paused for 30 passes, it skips those 30", pausable.GetTime());
        Check(loop.SecondsPerTic() == 0.0, "counts are not seconds");
    }

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED",
           failures, failures == 1 ? "" : "s");
    return failures != 0;
}
