#include "engine/Engine.hpp"

#include "engine/Input.hpp"
#include "engine/Scaling.hpp"

#include <chrono>
#include <thread>

namespace engine {

namespace {

// A stall (window resize/drag, breakpoint, alt-tab) can make a frame delta
// spike. Left unclamped, one frame of gravity integration can tunnel a fast
// entity clean through a thin collider before it is ever seen to overlap it.
// Both the main loop and the background thread clamp against this.
constexpr float MAX_DT = 1.0f / 20.0f;

// The background thread's sleep between ticks. Without one it would spin a
// core flat out and starve the render thread. 4ms is chosen so the worker
// ticks faster than anything that runs on it needs: a networked client at
// 2.0x time scale wants to send 120 messages a second, and it can only send
// once per tick. (sleep_for overshoots on macOS - 8333us lands nearer 11ms -
// so the nominal 250Hz here is really ~150Hz, still comfortably above 120.)
constexpr int WORKER_PERIOD_MICROSECONDS = 4000;

// Slide 20's "small window": if a frame comes in less than this early, let it
// through rather than sleeping. Sub-millisecond sleeps overshoot by more than
// they save, so chasing the last fraction costs accuracy instead of buying it.
constexpr auto GOVERNOR_WINDOW = std::chrono::microseconds(1200);

// One frame's delta, read off a timeline and clamped. Both loops and the
// worker go through this so they can never disagree about what a frame is.
// Read in seconds, not tics, so a game can change the timeline's tic mid-run.
float FrameDelta(Timeline &timeline, double &lastSeconds) {
    const double now = timeline.GetSeconds();
    float dt = (float)(now - lastSeconds);
    lastSeconds = now;
    // A tic change can round the reading down by up to one tic.
    if (dt < 0.0f) dt = 0.0f;
    return dt > MAX_DT ? MAX_DT : dt;
}

// `backgroundUpdate` is copied rather than captured by reference: the thread
// outlives this call, and only the Engine members it borrows are guaranteed
// to still be alive when it runs.
std::thread SpawnWorker(std::atomic<bool> &running, Timeline &gameTime,
                        std::function<void(float)> backgroundUpdate) {
    if (!backgroundUpdate) {
        return std::thread();
    }
    return std::thread([&running, &gameTime, backgroundUpdate = std::move(backgroundUpdate)]() {
        double workerLastTime = gameTime.GetSeconds();
        while (running) {
            backgroundUpdate(FrameDelta(gameTime, workerLastTime));
            std::this_thread::sleep_for(std::chrono::microseconds(WORKER_PERIOD_MICROSECONDS));
        }
    });
}

} // namespace

void Engine::SetTargetFrameRate(int hz) {
    targetFrameRate = hz > 0 ? hz : 0;
}

void Engine::SetDeltaSmoothing(int samples) {
    deltaSamples = samples > 1 ? samples : 1;
    deltaWindow.assign((size_t)deltaSamples, 0.0f);
    deltaNext = 0;
    deltaFilled = 0;
}

float Engine::SmoothDelta(float raw) {
    // Zero means the timeline is paused. Report it untouched and forget the
    // window: averaging a pause against recent motion would keep the world
    // creeping while it is supposed to be stopped.
    if (deltaSamples <= 1 || raw == 0.0f) {
        deltaFilled = 0;
        deltaNext = 0;
        return raw;
    }
    deltaWindow[deltaNext] = raw;
    deltaNext = (deltaNext + 1) % deltaWindow.size();
    if (deltaFilled < deltaWindow.size()) {
        ++deltaFilled;
    }
    float sum = 0.0f;
    for (size_t i = 0; i < deltaFilled; ++i) {
        sum += deltaWindow[i];
    }
    return sum / (float)deltaFilled;
}

bool Engine::Init(const char *title, int width, int height) {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        SDL_Log("Couldn't initialize SDL: %s", SDL_GetError());
        return false;
    }

    if (!SDL_CreateWindowAndRenderer(title, width, height, SDL_WINDOW_RESIZABLE, &window, &renderer)) {
        SDL_Log("Couldn't create window/renderer: %s", SDL_GetError());
        SDL_Quit();
        return false;
    }

    // Deliberately no SDL_SetRenderLogicalPresentation: letterboxing would
    // rescale the whole canvas to the window behind the game's back, which
    // makes ScaleMode::Pixel ("constant, pixel-based size") impossible to
    // express - objects would already grow and shrink with the window, so
    // both scaling modes would be proportional and Task 6's toggle would
    // only differ by a constant. Owning the world-to-screen mapping in the
    // Scaling system is what lets the two modes actually mean what they say.
    //
    // The design resolution is always 1920x1080, as specified in the rubric.
    // On displays smaller than this, the window will be clamped by SDL and
    // content may be clipped, but the game world remains in 1920x1080 space.
    baseWidth = DEFAULT_WINDOW_WIDTH;
    baseHeight = DEFAULT_WINDOW_HEIGHT;

    return true;
}

bool Engine::InitHeadless() {
    // SDL_Init(0) starts SDL's core without the video subsystem. The shared
    // Entity and Physics types still carry SDL structs, so SDL has to be up -
    // but a dedicated server must never open a window, and on a headless host
    // SDL_INIT_VIDEO would fail outright.
    if (!SDL_Init(0)) {
        SDL_Log("Couldn't initialize SDL: %s", SDL_GetError());
        return false;
    }

    // No window to measure, so the design resolution stands as the world size
    // every client agrees on.
    baseWidth = DEFAULT_WINDOW_WIDTH;
    baseHeight = DEFAULT_WINDOW_HEIGHT;
    return true;
}

void Engine::Shutdown() {
    if (renderer) {
        SDL_DestroyRenderer(renderer);
        renderer = nullptr;
    }
    if (window) {
        SDL_DestroyWindow(window);
        window = nullptr;
    }
    SDL_Quit();
}

void Engine::GetWindowSize(int *width, int *height) const {
    SDL_GetWindowSize(window, width, height);
}

float Engine::GetScale() const {
    int windowWidth = baseWidth;
    int windowHeight = baseHeight;
    SDL_GetWindowSize(window, &windowWidth, &windowHeight);
    return Scaling::GetFactor(windowWidth, windowHeight, baseWidth, baseHeight);
}

void Engine::Clear(SDL_Color color) {
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
    SDL_RenderClear(renderer);
}

void Engine::Present() {
    SDL_RenderPresent(renderer);
}

void Engine::Run(const std::function<void(float)> &update,
                 const std::function<void()> &render,
                 const std::function<void(float)> &backgroundUpdate) {
    running = true;

    // Section 3: a second thread over a disjoint slice of the world. It reads
    // the same timeline, so pausing or rescaling time stops or stretches both
    // threads at once - the worker needs no pause handling of its own.
    std::thread worker = SpawnWorker(running, gameTime, backgroundUpdate);

    // Delta comes from the game timeline, not the wall clock. Pausing the
    // timeline makes this delta zero, which is what freezes every entity
    // that moves on delta time without any of them knowing about pause.
    double lastTime = gameTime.GetSeconds();

    SDL_Event event;
    while (running) {
        const auto frameStart = std::chrono::steady_clock::now();
        ++iterationCount;

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }
        }

        const float dt = SmoothDelta(FrameDelta(gameTime, lastTime));

        Input::Update();
        update(dt);

        Clear();
        render();
        Present();

        // Frame-rate governing. Sleep only while we are more than the window
        // ahead of the target, then let the last sliver through.
        if (targetFrameRate > 0) {
            const auto target = frameStart + std::chrono::microseconds(
                Timeline::MICROSECONDS_PER_SECOND / targetFrameRate);
            const auto now = std::chrono::steady_clock::now();
            if (now + GOVERNOR_WINDOW < target) {
                std::this_thread::sleep_for(target - now - GOVERNOR_WINDOW);
            }
        }
    }

    running = false;
    if (worker.joinable()) {
        worker.join();
    }
}

void Engine::RunHeadless(const std::function<void(float)> &update, int tickHz,
                         const std::function<void(float)> &backgroundUpdate) {
    running = true;
    std::thread worker = SpawnWorker(running, gameTime, backgroundUpdate);

    // tickHz <= 0: no sleep at all. A benchmark wants to know how fast the
    // loop CAN go, so it must not be held to a rate.
    const bool uncapped = tickHz <= 0;
    const auto period = std::chrono::microseconds(uncapped ? 0 : Timeline::MICROSECONDS_PER_SECOND / tickHz);

    double lastTime = gameTime.GetSeconds();
    while (running) {
        ++iterationCount;
        update(SmoothDelta(FrameDelta(gameTime, lastTime)));
        if (!uncapped) std::this_thread::sleep_for(period);
    }

    running = false;
    if (worker.joinable()) {
        worker.join();
    }
}

} // namespace engine
