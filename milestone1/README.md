# Neon Grove Escape

**Milestone 1 · Solo platformer · C++17 / SDL3**

Find six runes, survive a glowing forest of fungal ruins, and escape through the Moon Gate. Neon Grove Escape is a scrolling platformer built on the Team 16 engine, with checkpoints, patrolling enemies, disappearing platforms, and a limited supply of lives.

## Highlights

- **A scrolling course:** traverse a 6,000-unit-wide world with platforms, hazards, and a following camera.
- **Six runes, one exit:** collect every rune to unlock the Moon Gate.
- **Three checkpoints:** key rune pickups advance your respawn position.
- **Ten lives:** recover from a death with a countdown, or restart for a better run.
- **Reactive movement:** acceleration, air control, buffered jumps, and enemy stomps.
- **Procedural visuals:** glowing scenery, fireflies, and particle trails drawn in code.
- **Scaling demonstration:** switch between pixel and proportional rendering while playing.

## Build and play

Run these commands from this milestone folder. You need a C++17 compiler, CMake 3.10+, SDL3, and SDL3_image.

On macOS with Homebrew:

```sh
brew install cmake sdl3 sdl3_image
```

Build and launch:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/neon_grave_escape
```

**Naming note:** the game displays **Neon Grove Escape**, while its source directory and executable retain the name `neon_grave_escape`.

## How to play

Move right through the course, collect all six runes, and reach the Moon Gate. The gate remains locked until your collection is complete. Rune pickups also activate three checkpoints along the route.

Avoid hazards and enemies, time your jumps across disappearing platforms, and land on enemies from above to stomp them. After losing a life, press **Space** to begin the three-second respawn countdown. Losing all ten lives ends the run.

| Key | Action |
| --- | --- |
| A / D or Left / Right | Move |
| Space | Jump |
| Space after a death | Continue from the latest checkpoint |
| Space after game over | Start a new run |
| R | Restart the run at any time |
| T | Toggle pixel / proportional scaling |
| Esc | Quit |

The HUD shows your run time, remaining lives, rune count, gate status, checkpoint, and scaling mode. If the default 1920×1080 view is clipped on a smaller display, try proportional scaling with **T**.

## Built-in checks

The executable includes gameplay verification and a short rendering smoke test:

```sh
./build/neon_grave_escape --logic-test
./build/neon_grave_escape --smoke-test
```

The logic checks exercise gameplay behavior such as enemy movement, checkpoints, rune collection, and camera scrolling. The smoke test runs for three update frames and exits. Both initialize SDL and a window; this project does not register a separate CTest suite.

For frame captures and scaling comparisons:

| Command | Output |
| --- | --- |
| `./build/neon_grave_escape --capture-frame` | `/tmp/neon_grove_escape.bmp` |
| `./build/neon_grave_escape --capture-pixel-resized` | `/tmp/neon_grove_escape_pixel_resized.bmp` |
| `./build/neon_grave_escape --capture-proportional` | `/tmp/neon_grove_escape_proportional.bmp` |

The resized captures use a 1024×576 window.

## Inside the project

| Path | Purpose |
| --- | --- |
| [CMakeLists.txt](CMakeLists.txt) | Builds the engine library and game executable |
| [engine/include/engine/](engine/include/engine/) | Engine interfaces for entities, input, physics, collision, and scaling |
| [engine/src/](engine/src/) | Engine implementation and SDL application loop |
| [game/neon_grave_escape/main.cpp](game/neon_grave_escape/main.cpp) | Course layout, gameplay, procedural rendering, and built-in checks |

The game uses the engine for its application loop, entities, input, physics, collision, and world-to-screen scaling. Course rules and presentation live in the game source, keeping the engine reusable.
