#include "engine/Input.hpp"

namespace engine {

std::vector<bool> Input::current;
std::vector<bool> Input::previous;
int Input::numKeys = 0;

namespace {
bool KeyDownIn(const std::vector<bool> &state, SDL_Scancode scancode) {
    // The cast handles a negative/unknown scancode by wrapping it out of range.
    return (size_t)scancode < state.size() && state[(size_t)scancode];
}
} // namespace

void Input::Update() {
    const bool *live = SDL_GetKeyboardState(&numKeys);
    if (!live || numKeys <= 0) {
        return;
    }
    previous = current;
    current.assign(live, live + numKeys);
}

bool Input::IsKeyPressed(SDL_Scancode scancode) {
    return KeyDownIn(current, scancode);
}

bool Input::IsKeyJustPressed(SDL_Scancode scancode) {
    return KeyDownIn(current, scancode) && !KeyDownIn(previous, scancode);
}

} // namespace engine
