# Towerfall · Moonlit Trials

**Milestone 1 · Arena archery · C++17 / SDL3 / ZeroMQ**

Outlast the other archers, then claim the diamond on a moving platform. Towerfall combines platforming and limited-ammunition combat with solo practice and three multiplayer networking modes, built on the Team 16 engine.

## Highlights

- **Two to four archers:** practice against AI or launch a local multiplayer session.
- **Platforming combat:** jump between ledges, wall-jump, fire arrows, and recover ammunition.
- **Three network models:** client-server, hybrid, and serverless peer-to-peer.
- **Time controls:** pause or switch between 0.5×, 1×, and 2× speed, with a telemetry overlay for observing the engine.
- **Replayable simulation:** timestamped commands, a shared event journal, and round-result verification.
- **Automation:** a session launcher, gameplay tests, and multiplayer checks using real processes.

## Build and play

Run these commands from this milestone folder. You need a C++17 compiler, CMake 3.16+, SDL3, SDL3_image, libzmq, cppzmq, and pkg-config. Python 3 is needed for the launcher and multiplayer tests.

On macOS with Homebrew:

```sh
brew install cmake sdl3 sdl3_image zeromq cppzmq pkg-config
```

Build and start solo practice:

```sh
cmake -S . -B build-towerfall -DCMAKE_BUILD_TYPE=Release
cmake --build build-towerfall --parallel
./build-towerfall/game/towerfall
```

The `build-towerfall` directory matches the default path used by the Python tools. Solo mode fills the remaining archer slots with AI.

## How to play

Eliminate your opponents and collect the diamond to win. Only the last surviving archer can claim it; surviving alone is not enough. Watch your ammunition and use the arena's moving platforms to reach the prize.

| Key | Action |
| --- | --- |
| Left / Right | Move and face the bow |
| Space | Jump; wall-jump beside a wall |
| Enter | Shoot |
| Up / Down | Adjust bow aim during play |
| Up | Ready in the multiplayer lobby |
| Up + Down | Open options |
| R | Restart solo practice |
| H | Toggle help |
| Tab | Toggle telemetry |
| P | Pause or resume game time |
| Shift + 1 / 2 / 3 | Set speed to 0.5× / 1× / 2× |
| F11 | Toggle fullscreen |
| Esc | Open the quit prompt or close a menu |

In solo practice, time controls affect the arena. In networked play, they affect the local archer's personal timeline.

## Local multiplayer

The launcher starts the required processes and opens one window per player. Click a window to control that archer. These sessions run on one computer.

| Mode | How it works |
| --- | --- |
| `client` | A dedicated server manages the shared match |
| `hybrid` | A server resolves shared combat while peers exchange live player positions |
| `peer` | Peers exchange events and agree on results without a dedicated server |

Start three archers, with two controlled by AI:

```sh
python3 game/tools/launch.py --mode client --players 3 --bots 2
```

Try the other modes:

```sh
python3 game/tools/launch.py --mode hybrid --players 3 --bots 2
python3 game/tools/launch.py --mode peer --players 3 --bots 2
```

Run one session at a time. In peer mode, press **Up** in each human player's window to ready up. Press **Ctrl+C** in the launcher's terminal to close the session.

Use `--port 20000` if the default base port, `19400`, is occupied. Add `--demo` to demonstrate different player timeline speeds, or `--log` to print network traffic. For the executable's full options:

```sh
./build-towerfall/game/towerfall --help
```

## Tests

Run the C++ gameplay and simulation tests:

```sh
(cd build-towerfall && ctest --output-on-failure)
```

Run the multiplayer timing and combat scenarios across all three network modes:

```sh
python3 game/tests/multiplayer.py --mode all --output /tmp/towerfall-network
python3 game/tests/multiplayer.py --mode all --combat --output /tmp/towerfall-network
```

The Python checks use only the standard library. They launch local ZeroMQ processes and write logs and JSONL telemetry to the output directory. Use `--mode client`, `--mode hybrid`, or `--mode peer` to check one mode.

## Inside the project

| Path | Purpose |
| --- | --- |
| [include/engine/](include/engine/) | Public engine interfaces |
| [src/](src/) | Rendering loop, input, physics, collision, timelines, jobs, and networking |
| [game/src/Simulation.cpp](game/src/Simulation.cpp) | Arena movement, arrows, collisions, and simulation replay |
| [game/src/Match.cpp](game/src/Match.cpp) | Match phases and result agreement |
| [game/src/Transport.cpp](game/src/Transport.cpp) | Multiplayer transport and synchronization |
| [game/src/Presentation.cpp](game/src/Presentation.cpp) | Rendering and audio |
| [game/tests/](game/tests/) | Rules tests and multiplayer scenarios |

The engine builds as a reusable static library. Set `-DBUILD_TOWERFALL=OFF` when configuring CMake to build only the engine.
