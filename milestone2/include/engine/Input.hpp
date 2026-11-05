#pragma once

#include <SDL3/SDL.h>

#include <vector>

namespace engine {

// Task 4. Wraps SDL_GetKeyboardState so games never touch SDL events for
// input - they only ever ask Input whether a key is down.
class Input {
public:
    // Refreshes the cached keyboard state. Called once per frame by
    // Engine::Run, before the game's update callback.
    static void Update();

    // True on every frame the key is held down. Use for continuous actions
    // like walking or thrusting.
    static bool IsKeyPressed(SDL_Scancode scancode);

    // True only on the frame the key goes down. Edge detection lives in the
    // engine so that every game toggling something (the Task 6 scaling mode,
    // a pause key) doesn't have to carry its own "was it down last frame"
    // flag - every game doing that by hand is another chance to get it wrong.
    static bool IsKeyJustPressed(SDL_Scancode scancode);

private:
    // Snapshots, not SDL's live pointer: SDL updates its array in place, so
    // comparing this frame against the last one needs our own copy.
    static std::vector<bool> current;
    static std::vector<bool> previous;
    static int numKeys;
};

} // namespace engine
