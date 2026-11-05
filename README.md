# Beyond the Game Loop

**Three games on a homegrown 2D engine · C++17 / SDL3 / ZeroMQ**

This repository collects three milestone projects, each pairing the Team 16 engine with a game built on it. Between them, they cover a game loop with physics and collision, timelines and multithreading, three multiplayer network models, a component-based object model, and a benchmark suite for the networking.

By **Smruthi Bangalore Thandava Murthy** (`sbangal6`).

Each milestone folder is self-contained, with its own copy of the engine, so any of them builds on its own.

## The milestones

### [Milestone 1 · Towerfall · Moonlit Trials](milestone1/README.md)

Arena archery for two to four players. Outlast the other archers, then claim the diamond on a moving platform, either solo against AI or in local multiplayer.

**Engine focus:** timelines with pause and 0.5×–2× speed, a job system, and three network models: client-server, hybrid, and serverless peer-to-peer. A Python launcher starts multiplayer sessions, and the tests play matches in all three models using real processes.

### [Milestone 2 · Neon Grove Escape](milestone2/README.md)

A solo scrolling platformer through a glowing forest of fungal ruins. Collect six runes to unlock the Moon Gate, with checkpoints and ten lives to get you there.

**Engine focus:** the core loop, entities, physics, collision, input, and switching between pixel and proportional scaling. All visuals are drawn in code, and the executable includes logic and smoke tests.

### [Milestone 3 · Clockwork Dash](milestone3/README.md)

A time-trial platformer for up to four players. Hit three checkpoints in order, dodge saws and electric barriers, and grab potions for a temporary boost.

**Engine focus:** a component-based object model, and scenes kept in sync over client-server or peer-to-peer with text or binary, full or delta snapshots. A benchmark suite reproduces the measurements in the [project report](milestone3/report_sbangal6.pdf). The [engine guide](milestone3/engine/README.md) is the most complete reference for the engine's API.

## Build requirements

All three milestones are C++17 CMake projects on SDL3. The two networked milestones also need ZeroMQ.

| Requirement | Milestone 1 | Milestone 2 | Milestone 3 |
| --- | --- | --- | --- |
| C++17 compiler | ✓ | ✓ | ✓ |
| CMake | 3.16+ | 3.10+ | 3.16+ |
| SDL3 and SDL3_image | ✓ | ✓ | ✓ |
| libzmq, cppzmq, and pkg-config | ✓ | — | ✓ |
| Python 3 | Launcher and multiplayer tests | — | Benchmark scripts |
| matplotlib | — | — | Benchmark charts |

On macOS, one Homebrew command installs everything except matplotlib:

```sh
brew install cmake sdl3 sdl3_image zeromq cppzmq pkg-config
```

The projects have only been built on macOS. For Linux package names, see the [engine guide](milestone3/engine/README.md#build).

## Build and run

Each milestone builds from its own folder. For example, to play Clockwork Dash:

```sh
git clone https://github.com/smruthibt/beyond-the-game-loop.git
cd beyond-the-game-loop/milestone3
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/clockwork_dash
```

The other milestones follow the same steps with a different build directory and executable:

| Folder | Build directory | Run |
| --- | --- | --- |
| `milestone1/` | `build-towerfall` | `./build-towerfall/game/towerfall` |
| `milestone2/` | `build` | `./build/neon_grave_escape` |
| `milestone3/` | `build` | `./build/clockwork_dash` |

Milestone 1's Python tools expect the `build-towerfall` directory. Each milestone's README covers its controls, multiplayer modes, and tests.
