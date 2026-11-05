#include "engine/Engine.hpp"

#include "engine/Input.hpp"
#include "engine/Scaling.hpp"

namespace engine {

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

void Engine::Run(const std::function<void(float)> &update, const std::function<void()> &render) {
    running = true;
    Uint64 lastTicks = SDL_GetPerformanceCounter();
    const Uint64 frequency = SDL_GetPerformanceFrequency();

    SDL_Event event;
    while (running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT) {
                running = false;
            }
        }

        Uint64 currentTicks = SDL_GetPerformanceCounter();
        float dt = (float)(currentTicks - lastTicks) / (float)frequency;
        lastTicks = currentTicks;

        // A stall (window resize/drag, breakpoint, alt-tab) can make dt spike.
        // Left unclamped, one frame of gravity integration can tunnel a fast
        // entity clean through a thin collider before it's ever seen to
        // overlap it. Capping dt keeps a stall from ever producing a jump
        // bigger than physics/collision can resolve.
        const float MAX_DT = 1.0f / 20.0f;
        if (dt > MAX_DT) {
            dt = MAX_DT;
        }

        Input::Update();
        update(dt);

        Clear();
        render();
        Present();
    }
}

} // namespace engine
