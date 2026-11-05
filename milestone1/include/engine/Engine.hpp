#pragma once

#include <SDL3/SDL.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

#include "engine/JobSystem.hpp"
#include "engine/Timeline.hpp"

namespace engine {

constexpr int DEFAULT_WINDOW_WIDTH = 1920;
constexpr int DEFAULT_WINDOW_HEIGHT = 1080;
constexpr SDL_Color DEFAULT_CLEAR_COLOR = {25, 60, 130, 255};

// Pool workers for the JobSystem, on top of the main thread and the
// background update thread. Two is enough to demonstrate distinct update
// subsets landing on distinct threads without oversubscribing a laptop.
constexpr int DEFAULT_JOB_WORKERS = 2;

// Task 1: Core graphics setup. Owns SDL initialization, the window and
// renderer, and the main game loop - games call Init() once, then hand
// their per-frame update/render logic to Run().
class Engine {
public:
    bool Init(const char *title, int width = DEFAULT_WINDOW_WIDTH, int height = DEFAULT_WINDOW_HEIGHT);

    // No window, no renderer, no video subsystem - just the timeline and the
    // loop. A dedicated server has no business opening a window, and on a
    // headless host SDL_INIT_VIDEO would fail outright.
    bool InitHeadless();

    void Shutdown();

    SDL_Window *GetWindow() const { return window; }
    SDL_Renderer *GetRenderer() const { return renderer; }

    // The design resolution: always 1920x1080 as specified in the rubric.
    // Games lay their world out in this coordinate space. The Scaling system
    // (Task 6) decides how that space maps onto the window after a resize.
    // On smaller displays, SDL clamps the window but the game world remains
    // 1920x1080 and content may be clipped.
    int GetBaseWidth() const { return baseWidth; }
    int GetBaseHeight() const { return baseHeight; }

    // Current window size, in the same units the renderer draws in.
    void GetWindowSize(int *width, int *height) const;

    // World-to-screen scale for this frame, per the current ScaleMode.
    // Entities are drawn with it; gameplay never sees it.
    float GetScale() const;

    // Game time (Section 1). Run() derives every frame's delta from this
    // timeline rather than the wall clock, so pausing it freezes every
    // entity that moves on delta time, and scaling it puts the whole
    // simulation into slow motion or fast forward with no change to any
    // other object. Games reach for it to bind pause/scale to keys.
    Timeline &GetGameTime() { return gameTime; }
    const Timeline &GetGameTime() const { return gameTime; }

    // A second, unrelated clock (Section 1's "loop iterations" example).
    // Advances by exactly one per pass of Run()/RunHeadless(), whatever that
    // pass's dt was - unlike gameTime, it does not answer to Pause() or
    // SetScale(), because it is not measuring elapsed time at all, just
    // counting passes of the loop.
    int64_t GetIterationCount() const { return iterationCount.load(); }

    // The same count as a Timeline anchored to it, so loop iterations get
    // tic, anchoring, pause and scale like any other time. Independent of game time.
    Timeline &GetIterationTime() { return iterationTime; }
    const Timeline &GetIterationTime() const { return iterationTime; }

    void Clear(SDL_Color color = DEFAULT_CLEAR_COLOR);
    void Present();

    // Runs the loop: polls events (closing the window stops the loop),
    // refreshes Input state, then calls update(deltaSeconds) followed by
    // render() every frame.
    //
    // `backgroundUpdate`, when supplied, runs on a second thread against the
    // same timeline (Section 3). Give it a disjoint subset of the world -
    // typically the entities that move on their own - and guard whatever the
    // two threads share with GetWorldMutex(). Input and rendering stay on the
    // main thread because SDL requires it, so the split is: this thread owns
    // the player and the view, the worker owns the rest of the simulation.
    void Run(const std::function<void(float deltaSeconds)> &update,
             const std::function<void()> &render,
             const std::function<void(float deltaSeconds)> &backgroundUpdate = nullptr);

    // The headless counterpart of Run(): same timeline-driven delta and same
    // optional worker thread, with no event polling and nothing drawn. Ticks
    // at `tickHz` rather than at a display's refresh rate.
    void RunHeadless(const std::function<void(float deltaSeconds)> &update,
                     int tickHz = 60,
                     const std::function<void(float deltaSeconds)> &backgroundUpdate = nullptr);

    // Guards world state shared between the main loop and the background
    // update thread - the role of the Module 6 ThreadExample's networkMutex.
    // One mutex per engine rather than one per game, so games on this engine
    // cannot each invent a different convention.
    std::mutex &GetWorldMutex() { return worldMutex; }

    // The job pool (Section 3). Intended use: the background update submits
    // one job per disjoint slice of the world - the enemies, the bullets - and
    // calls RunAndWait(), so distinct update subsets land on distinct threads.
    // Submit/RunAndWait belong to whichever single thread is producing;
    // in practice that is the background update thread.
    JobSystem &GetJobs() { return jobs; }

    // --- Frame-time quality (Lecture 06, slides 18-20) ----------------------
    // Two answers to the same problem: a frame delta that jumps around when
    // the machine is busy. They are independent and both are off by default.

    // Frame-rate governing: a "do not exceed" rate. When a frame finishes more
    // than a small window early, the loop waits. Without it the loop spins as
    // fast as it can, which burns a core and makes the delta jittery.
    // 0 leaves the loop uncapped. Main thread.
    void SetTargetFrameRate(int hz);
    int GetTargetFrameRate() const { return targetFrameRate; }

    // Moving average: report the delta as the mean of the last `samples`
    // frames, so one long frame is spread instead of landing whole. Larger
    // windows are steadier but slower to notice a real change in load;
    // 1 reports the raw delta. Main thread.
    //
    // A paused timeline still yields exactly zero - a mean of recent motion
    // must not keep the world drifting while it is stopped.
    void SetDeltaSmoothing(int samples);
    int GetDeltaSmoothing() const { return deltaSamples; }

    // Ends the loop from either thread.
    void Stop() { running = false; }

private:
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;

    // Read by the background thread to know when to stop, written by the main
    // thread when the window closes.
    std::atomic<bool> running{false};
    std::mutex worldMutex;
    int baseWidth = DEFAULT_WINDOW_WIDTH;
    int baseHeight = DEFAULT_WINDOW_HEIGHT;

    // Smooths `raw` per SetDeltaSmoothing. Main thread only - the window is
    // not guarded, and the background thread deliberately gets raw deltas.
    float SmoothDelta(float raw);

    int targetFrameRate = 0;
    int deltaSamples = 1;
    std::vector<float> deltaWindow;
    size_t deltaNext = 0;
    size_t deltaFilled = 0;

    // Anchored to real time, one tic per microsecond. Millisecond tics would
    // truncate ~0.7ms of every 60fps frame, which compounds into a few
    // percent of drift over a run.
    Timeline gameTime{nullptr, 1};

    // Section 1: counts loop passes, not time. Incremented once per pass in
    // Run() and RunHeadless(); read by GetIterationCount().
    std::atomic<int64_t> iterationCount{0};
    Timeline iterationTime{iterationCount, 1};   // declared after the counter it reads

    JobSystem jobs{DEFAULT_JOB_WORKERS};
};

} // namespace engine
