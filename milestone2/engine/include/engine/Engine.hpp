#pragma once

#include <SDL3/SDL.h>

#include <functional>

namespace engine {

constexpr int DEFAULT_WINDOW_WIDTH = 1920;
constexpr int DEFAULT_WINDOW_HEIGHT = 1080;
constexpr SDL_Color DEFAULT_CLEAR_COLOR = {25, 60, 130, 255};

// Task 1: Core graphics setup. Owns SDL initialization, the window and
// renderer, and the main game loop - games call Init() once, then hand
// their per-frame update/render logic to Run().
class Engine {
public:
    bool Init(const char *title, int width = DEFAULT_WINDOW_WIDTH, int height = DEFAULT_WINDOW_HEIGHT);
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

    void Clear(SDL_Color color = DEFAULT_CLEAR_COLOR);
    void Present();

    // Runs the loop: polls events (closing the window stops the loop),
    // refreshes Input state, then calls update(deltaSeconds) followed by
    // render() every frame.
    void Run(const std::function<void(float deltaSeconds)> &update,
             const std::function<void()> &render);

private:
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    bool running = false;
    int baseWidth = DEFAULT_WINDOW_WIDTH;
    int baseHeight = DEFAULT_WINDOW_HEIGHT;
};

} // namespace engine
