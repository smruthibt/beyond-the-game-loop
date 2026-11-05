# Clockwork Dash

**Milestone 3 · Multiplayer time-trial platformer · C++17 / SDL3 / ZeroMQ**

Race through a clockwork obstacle course, hit every checkpoint, and reach the finish gate. Clockwork Dash pairs a playable platformer with a component-based engine, two multiplayer topologies, and a reproducible networking benchmark suite.

Built by **Smruthi Bangalore Thandava Murthy** (`sbangal6`) using the **Team 16 engine**.

## Highlights

- **Solo and multiplayer:** practice alone or race with up to four players in client-server or peer-to-peer mode.
- **A timed obstacle course:** three ordered checkpoints, moving hazards, saws, and electric barriers.
- **Temporary power-ups:** reveal potions beneath copper ledges for a seven-second size and ground-speed boost.
- **Player interaction:** stomp another runner to remove their boost.
- **Component-based simulation:** game objects, reusable systems, triggers, cameras, and respawning.
- **Configurable synchronization:** text or binary encoding, with full or delta snapshots.
- **Benchmark tooling:** scripted experiments, analysis, and the accompanying [project report](report_sbangal6.pdf).

## Build and play

Run these commands from this milestone folder. You need a C++17 compiler, CMake 3.16+, SDL3, SDL3_image, libzmq, cppzmq, and pkg-config. Benchmark scripts also require Python 3; chart generation uses matplotlib.

On macOS with Homebrew:

```sh
brew install cmake sdl3 sdl3_image zeromq cppzmq pkg-config
```

Build and start solo practice:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
./build/clockwork_dash
```

## How to play

After the three-second countdown, pass all three checkpoints in order and reach the finish gate. Falling or touching an active hazard returns you to your latest checkpoint while the race timer continues.

Jump onto ledges from the side. Hit a copper ledge from below to reveal a potion, then collect it for a temporary boost. Landing on another runner's head removes their boost.

| Key | Action |
| --- | --- |
| A / D or Left / Right | Move |
| Space, W, or Up | Jump |
| R | Restart your own run |
| Esc | Quit |

## Local multiplayer

Each player runs in a separate process on the same computer. Click a player's window to control it.

### Client-server

Start the headless server in one terminal:

```sh
./build/clockwork_dash server
```

In a separate terminal for each player, run:

```sh
./build/clockwork_dash client
```

The server owns the shared simulation; clients send player inputs.

### Peer-to-peer

Start each peer in its own terminal. No dedicated server is required:

```sh
# Terminal 1
./build/clockwork_dash peer --id 0
```

```sh
# Terminal 2
./build/clockwork_dash peer --id 1
```

Use IDs `2` and `3` for additional players, or omit `--id` to claim the first free slot. Peers exchange the state they own.

### Network options

| Option | Default | Purpose |
| --- | --- | --- |
| `--port-base N` | `8650` | Choose the local TCP base port |
| `--encoding text` or `--encoding binary` | `binary` | Select the snapshot encoding |
| `--delta on` or `--delta off` | `off` | Send delta or full snapshots |

Use matching port, encoding, and delta settings for every endpoint in a session. For example, add `--port-base 9000` to each command if the default ports are occupied.

List the remaining options, including window layout and screenshot capture:

```sh
./build/clockwork_dash --help
```

## Tests

Run the registered gameplay and multiplayer checks:

```sh
(cd build && ctest --output-on-failure)
```

The suite includes gameplay rules, client-server multiplayer, peer gameplay, and peer multiplayer. Network checks require local socket access and can report a skipped result when it is unavailable.

The engine has additional standalone test executables. Enable them with:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DENGINE_TESTS=ON
cmake --build build --parallel
```

Run individual engine checks directly, for example:

```sh
./build/timeline_test
./build/object_model_test
./build/snapshot_test
```

These engine executables are not registered with CTest. See the [engine documentation](engine/README.md) for the full verification guide and API details.

## Reproduce the benchmarks

The benchmark harness compiles the included engine and game sources. It compares networking configurations and produces the measurements used in the [report](report_sbangal6.pdf).

Build the harness:

```sh
cmake -S benchmark -B benchmark/build -DCMAKE_BUILD_TYPE=Release
cmake --build benchmark/build --parallel
```

With Python 3 and matplotlib available, create and run a campaign:

```sh
./benchmark/build/rerun_faults
python3 benchmark/tools/campaign.py create --name final
python3 benchmark/tools/campaign.py run --name final --suites faults,smoke,validation,primary
python3 benchmark/tools/analyze.py --name final
python3 benchmark/tools/audit_primary.py --name final
python3 benchmark/tools/report_tables.py --name final
```

Results are written to `benchmark/results/final/`, which is ignored by Git. Choose a new campaign name if that directory already exists, and use it consistently in every command.

The report's primary suite contains four comparisons, nine conditions, and ten seed-matched runs per side: **720 runs**. A full campaign takes several hours; keep the computer on AC power and otherwise idle for comparable measurements.

## Inside the project

| Path | Purpose |
| --- | --- |
| [engine/](engine/) | Object model, systems, serialization, snapshots, networking, and engine tests |
| [game/clockwork_dash/](game/clockwork_dash/) | Course, race rules, rendering, peer synchronization, and game tests |
| [benchmark/harness/](benchmark/harness/) | Benchmark processes, scenarios, and correctness checks |
| [benchmark/tools/](benchmark/tools/) | Campaign scheduling, analysis, audits, and report tables |
| [report_sbangal6.pdf](report_sbangal6.pdf) | Project report and performance results |

The engine can also be reused independently through its CMake `engine` target. Its [README](engine/README.md) documents integration, components, timelines, serialization, and networking.
