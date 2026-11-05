# Team 16 engine

A small 2D game engine on SDL3 and ZeroMQ. Milestone 1 gave it a window, a
loop, entities, physics, input, collision and scaling. Milestone 2 gave it
time, threads, and three ways to put a game on a network. Milestone 3 gave it
a component-based game object model, a networked scene built on it, and a
framework for measuring the network's performance.

Everything a game needs is in `engine/include/engine/`. A game is a single
`main.cpp` (or a few files) that links `engine` and nothing else, so one
teammate's game can never break another's. Maze Run (`game/maze_run/`) is the
game built on it.

```
engine/include/engine/
  Engine.hpp       window, renderer, the loop, headless mode, world mutex, job pool, loop count
  Timeline.hpp     time anchored to real time, another timeline or a counter; tic, pause, 0.5x-2.0x
  Entity.hpp       position, velocity, size, colour; sprite sheets animated on a timeline, tinted
  Physics.hpp      gravity (opt-in per entity) and velocity integration
  Collision.hpp    CheckCollision (overlap) and Resolve (push-out)
  Input.hpp        IsKeyPressed / IsKeyJustPressed over SDL keyboard state
  Scaling.hpp      Pixel vs Proportional world-to-screen mapping
  JobSystem.hpp    a persistent worker pool with a per-frame barrier
  Network.hpp      REQ/REP/PUB/SUB sockets that fail with a message, never a crash
  Server.hpp       the headless authoritative server (Sections 2, 4)
  Client.hpp       the network half of a windowed client (Sections 2, 4)
  Peer.hpp         the serverless mesh: shared clock, event log (Section 5)

  GameObject.hpp   an id, a kind and components - the object model (M3)
  World.hpp        owns the objects of a scene; queries by component; deferred destroy
  Components.hpp   Transform, RigidBody, Collider, Renderable, InputState, PathMover, SpawnPoint, ...
  Systems.hpp      what components do: moving paths, control, physics, death zones, scrolling, drawing
  Serialize.hpp    Writer/Reader, text and binary; the component registry
  Snapshot.hpp     a World as a message: full or delta, all components or a few
  NetScene.hpp     SceneServer, SceneClient, ScenePeer: a World kept in step on every endpoint

engine/examples/scene_demo.cpp     a small networked platformer using all of the above: copy from it
engine/bench/net_bench.cpp         the performance benchmark
engine/tools/net_experiments.py    runs the experiments and draws the charts
engine/tests/                      14 programs that check the engine on your machine (see Verifying)
```

## What Milestone 2 added

| Feature | Where | Section |
|---|---|---|
| Time as a value: real, game or loop-pass time; anchoring, tic, pause, 0.5x-2.0x, shared origins | `Timeline` | 1 |
| Every frame's `dt` taken from game time, capped at 0.05 s | `Engine::Run`, `RunHeadless` | 1 |
| Loop iterations as a clock, raw and as a `Timeline` | `Engine::GetIterationCount`, `GetIterationTime` | 1 |
| Animation on any timeline; sprites tinted per entity; drawing through a camera | `Entity` | 1 |
| Headless mode: no window, no renderer, no video subsystem | `Engine::InitHeadless`, `RunHeadless` | 2 |
| A background update thread beside the main loop, and a persistent job pool | `Engine::Run`, `JobSystem` | 3 |
| Frame-rate governing and delta smoothing | `Engine` | Lecture 06 |
| Sockets that never throw | `net::Socket` | 2, 4 |
| An authoritative server with one thread per client; a slot freed when its client leaves | `Server` | 2, 4 |
| Upstream sends paced by game time; a clean leave; survival when the server goes away | `Client` | 2, 4 |
| A serverless mesh: ids claimed by port, a shared clock, an event log every peer relays | `Peer` | 5 |

## What Milestone 3 added

| Feature | Where | Part |
|---|---|---|
| A game object model: objects are ids with components, no subclasses | `GameObject`, `World` | 1A |
| The components a platformer needs, and the systems that act on them | `Components.hpp`, `Systems.hpp` | 1A, 2 |
| Hidden objects that still collide: spawn points, death zones, scroll boundaries | `SpawnPoint`, `DeathZone`, `ScrollBoundary` | 2 |
| Every component writes itself once, read back as text or binary, bit-identical | `Serialize.hpp` | 1B, 1C |
| A World as a message: full or delta, every component or a chosen few | `Snapshot.hpp` | 1B, 1C |
| The object model on every endpoint, kept in step over client-server or peer-to-peer | `NetScene.hpp` | 1B |
| Any number of clients; a silent client (crash, `kill -9`) timed out and removed; heartbeats keep a paused one | `Server`, `Client` | 1B |
| One-way latency measured on the shared clock | `NetStats` | 1B |
| Every experiment variable as a flag; a benchmark and a runner with charts | `NetOptions`, `bench/net_bench.cpp`, `tools/net_experiments.py` | 1C |
| Triggers with enter/stay/exit (pickups, plates, doors, checkpoints) | `TriggerTracker`, `FindTriggerContacts` | 2 |
| Sprites that face the way a player runs; the keyboard mapped to buttons | `Renderable::flipX`, `ReadKeyboard` | 2 |
| A worked example: a small networked platformer | `examples/scene_demo.cpp` | 1B, 2 |

## Build

This repository, from its root:

```
cmake -S . -B build
cmake --build build
```

In your own project, drop this `engine/` folder in and add two lines to your
`CMakeLists.txt`; `engine/CMakeLists.txt` finds SDL3, SDL3_image and ZeroMQ
itself, and passes on everything your game needs, C++17 included:

```cmake
add_subdirectory(engine)
target_link_libraries(my_game PRIVATE engine)
```

Paths below that start with `game/` or `tests/` are in the full repository,
as working examples.

Needs SDL3, SDL3_image, libzmq and cppzmq visible to CMake. On macOS:

```
brew install sdl3 sdl3_image zeromq cppzmq
```

On Ubuntu the packages are `libsdl3-dev libsdl3-image-dev libzmq3-dev cppzmq-dev`
(cppzmq is header-only; if your distribution lacks it, clone
`github.com/zeromq/cppzmq` and add its directory to `CMAKE_PREFIX_PATH`).
**We have only built on macOS so far.** If you are the first on Linux or
Windows, expect to spend time here and tell the team what you hit.

## The loop and the threads

```cpp
Engine engine;
engine.Init("My Game");                 // 1920x1080 window
engine.Run(update, render, background); // background is optional

// A server: no window, no renderer, no video subsystem.
engine.InitHeadless();
engine.RunHeadless(update, /*tickHz=*/60, background);
```

`Run` calls `update(dt)` then `render()` every frame on the **main thread**,
where SDL requires input and drawing to be. If you pass `background`, it runs
on a **second thread** against the same clock, every few milliseconds. The
intended split: the main thread owns the player and the view; the background
thread owns what moves on its own (patrolling enemies, or all the
networking). `RunHeadless` is the same loop without events or drawing,
calling `update` `tickHz` times a second. `engine.Stop()` ends either loop
from any thread; `Shutdown()` releases SDL afterwards.

Anything both threads touch goes behind `engine.GetWorldMutex()`. One mutex
per engine, so every game on it shares one convention. Hold it inside your
callbacks around the reads and writes - never across a network round trip.

`dt` comes from `engine.GetGameTime()`, a `Timeline` (see Time, below). Pause
it and `dt` is 0 on both threads; scale it and both stretch; no entity has to
know. The loop reads it in seconds, so changing the game timeline's tic is
safe, and caps it at 0.05 s, so a stall - a window drag, a breakpoint - cannot
carry a fast entity through a thin wall in one frame.

Two knobs on the main loop, both off by default (Lecture 06, slides 18-20):

```cpp
engine.SetTargetFrameRate(60);   // "do not exceed"; 0 leaves it uncapped
engine.SetDeltaSmoothing(8);     // report dt as the mean of the last 8 frames
```

Without a target the loop spins as fast as it can, which burns a core and makes
`dt` jittery. Smoothing spreads one long frame instead of delivering it whole;
a larger window is steadier but slower to notice a real change in load. A
paused timeline still yields exactly zero either way. The background thread
always gets the raw `dt`.

For finer-grained parallel work, `engine.GetJobs()` is a persistent pool,
adapted from the Module 6 ThreadExample: `Submit(job)` several times, then
`RunAndWait()`, which returns once every job has run. Jobs must touch disjoint
data, and `Submit` and `RunAndWait` belong to one producing thread. It costs
~6 microseconds a frame to dispatch and is only a win when the work is more
than that. Maze Run's server steps its enemies, then its bullets, across it.

`engine.GetIterationCount()` counts passes of the loop itself, whatever `dt`
was; pausing the game timeline stops the world, not the engine.
`engine.GetIterationTime()` is the same count as a `Timeline`.

## Time

A `Timeline` reports elapsed time in **tics**, and is anchored to one of three
things:

```cpp
Timeline real(nullptr, 1000);                        // real time: 1 tic = 1000 us = 1 ms
Timeline effect(&engine.GetGameTime(), 1);           // another timeline: pauses and scales with it
std::atomic<int64_t> turns{0};
Timeline turnTime(turns, 1);                         // a counter: 1 tic per increment
Timeline everySixty(&engine.GetIterationTime(), 60); // 1 tic per 60 loop passes
```

`tic` is how many of the anchor's tics make one of this timeline's -
microseconds for real time, increments for a counter.

**Reading it.** `GetTime()` is elapsed tics. `GetSeconds()` is the same time in
seconds whatever the tic and however deep the chain - hand any system any
timeline and it can work in seconds without knowing the units. It is zero on
a counter-anchored timeline: counts are not seconds. `SecondsPerTic()` says
what one tic is worth.

**Controlling it.**

- `Pause()` / `Unpause()`: the paused span is skipped, never paid back on
  unpause, and everything anchored below stops too.
- `SetScale(s)`: clamped to [0.5, 2.0]. It applies from the moment it is set,
  never retroactively, and compounds down a chain - a 0.5x timeline anchored
  to a 0.5x one runs at 0.25x.
- `SetTic(n)`: changes only how this timeline reports. Safe at any time.

**Shared origins.** `Timeline::RealTimeNow()` reads the real-time clock in
microseconds, and `SetOrigin(t)` re-zeroes a timeline so it measures from `t`
on its anchor's clock. Processes given the same origin read the same time with
no one in charge of it; that is how `Peer`'s shared clock works.

Two guarantees make these combine safely. Every timeline in a chain counts in
its root's units, and a child reads its parent's count, not its tics - so a
tic change never moves time on any timeline. And the fraction of each scaled
step is carried rather than dropped - so reading a timeline more or less often
never changes what it reports, and a 0.5x child of a coarse parent keeps
exactly half its time. Every method is safe from any thread.

## Entities, physics, collision, sprites

```cpp
Entity e;
e.position = {100, 200};  e.size = {40, 40};  e.velocity = {80, 0};
e.affectedByGravity = true;                 // off by default - top-down games never turn it on
Physics::SetGravity(2200.0f);               // once, for the world
Physics::Update(e, dt);                     // integrate

if (Collision::CheckCollision(a, b)) ...    // AABB overlap, bool
Vec2 push = Collision::Resolve(mover, wall); // smallest move that separates them
mover.position.x += push.x;  if (push.x) mover.velocity.x = 0;   // stop and slide
```

`Resolve` is the only collision *response* the engine offers: push out along
the shallower axis. What happens after - stop, bounce, die - is yours.
`CheckCollision` counts exactly-touching edges as overlapping (SDL's rule);
`Resolve` returns zero when flush, so a box held against a wall does not jitter.
`e.active = false` takes an entity out of both drawing and collision.

**Sprites.** Animations run on a timeline, not the wall clock:

```cpp
e.SetTexture(sheet, 4, 32, 32, 100);        // 4 frames side by side, 100ms each
e.UpdateAnimation(engine.GetGameTime());    // pauses and scales with the world

Timeline flash(&engine.GetGameTime(), 1);   // a LOCAL timeline: its own zero,
e.UpdateAnimation(flash);                   // but it stops when the world does
```

The current frame is a pure function of that clock and the animation's own
zero, so a paused clock holds the frame exactly - there is no counter to drift.

A sprite is drawn multiplied by the entity's `color`, so one greyscale sheet
serves every color; white, the default, leaves it as drawn. The tint is set on
every draw, so entities sharing a texture keep their own colors:

```cpp
e.color = {90, 210, 255, 255};              // this entity's tint
e.Draw(renderer, engine.GetScale(), {camX, camY});   // camera: the world point at the top-left
```

An entity with no texture is drawn as a rectangle filled with `color`.
Maze Run's tanks (`game/maze_run/mz_tank.hpp`) use all of this: one greyscale
sheet, a tint per player, and treads animated on a timeline that pauses when
the tank stands still.

**Scaling.** `Draw` applies one world-to-screen factor to position and size
together, so what overlaps in the world overlaps on screen and gameplay never
has to know about it. `Scaling::Toggle()` switches between `Pixel` (one world
unit is one screen pixel; a bigger window shows more world) and `Proportional`
(the 1920x1080 design resolution fills the window). `engine.GetScale()` is this
frame's factor.

## Input

```cpp
Input::IsKeyPressed(SDL_SCANCODE_A)       // held - use for movement
Input::IsKeyJustPressed(SDL_SCANCODE_SPACE) // this frame only - use for jump, fire, toggles
```

Edge detection is in the engine so games do not each keep a
`wasDownLastFrame` flag. `Engine::Run` refreshes input once per frame before
`update`; do not read it from the background thread.

## Networking: three shapes, one engine

Every message is a plain string. The engine moves them; you decide what they
say. `game/maze_run/mz_protocol.hpp` is an example of the encode/decode
helpers a game writes.

### Sockets: `net::Socket`

`Server`, `Client` and `Peer` are built on this; a game rarely needs it.

```cpp
net::Socket s(net::SocketType::Request);   // Request, Reply, Publish, Subscribe - no ROUTER or DEALER
if (!s.Bind("tcp://*:5555")) SDL_Log("%s", s.LastError().c_str());
s.SetReceiveTimeoutMs(500);                // without one, a dead peer hangs Receive forever
s.Send("hello");
std::string reply;
if (!s.Receive(reply)) { /* timed out, or LastError() says why */ }
```

Nothing here throws: `Bind` and `Connect` return false with the reason in
`LastError()`, and so do `Send` and `Receive`. A SUB socket must `Subscribe()`
(an empty topic takes everything) or it hears nothing. Unsent messages are
dropped at exit instead of holding the process open.

### Client-server: `Server` + `Client`

The server is headless and authoritative: it owns whatever all players must
agree on. Each client owns only its own player.

```cpp
// server
Server::Config c; c.joinPort = 6555; c.worldPort = 6556; c.clientPortBase = 6600;
Server server(engine, c);
Server::Hooks h;
h.onJoin     = [&](int id) { /* create a player */ };
h.onLeave    = [&](int id) { /* remove it */ };
h.onRequest  = [&](int id, const std::string &req) -> std::string { /* parse, act, reply */ };
h.buildWorld = [&]() -> std::string { /* encode what everyone must see */ };
h.step       = [&](float dt) { /* advance the world */ };
server.Run(h);           // blocks until Stop(), Ctrl-C or SIGTERM; false if a port was taken
```

| `Server::Config` | Default | Meaning |
|---|---|---|
| `joinPort`, `worldPort`, `clientPortBase` | set these | JOIN on one, the world on another, client `n` on `clientPortBase + n` |
| `maxClients` | 4 | slots; 0 means no limit |
| `publishHz` | 60 | world broadcasts per second, from the publisher thread |
| `publishEverySteps` | 0 | K > 0: the main loop sends the world itself after every K-th `step` instead |
| `tickHz` | 60 | `step` calls per second; 0 or less runs uncapped |
| `socketTimeoutMs` | 500 | how often an idle thread wakes to notice `Stop()` |
| `clientTimeoutMs` | 3000 | silence that counts as a disconnect; 0 never times out |
| `worldQueue` | 32 | world messages held per client before the oldest are dropped |
| `logTag` | `"server"` | prefix on the server's log lines |

Under the hood: one REP thread for joins, **one REP thread per client slot**,
one PUB thread for the world, and `step` on the main thread from the server's
own Timeline. A slow client blocks only the thread that serves it - that is
the asynchrony Section 4 asks for, built from threads and synchronous pairs,
not from ROUTER/DEALER. The hooks run on those threads, several at once, so
guard your world with your own mutex:

| Hook | Called on |
|---|---|
| `onJoin` | the join thread |
| `onRequest` | that client's own thread |
| `onLeave` | that client's thread when it sends `BYE`, or at shutdown |
| `buildWorld` | the publisher thread |
| `step` | the main thread |

`IsConnected(id)`, `ConnectedCount()` and `ClientPort(id)` report the slot
table. `BYE` is the engine's own request and never reaches `onRequest`.

```cpp
// client
Client::Config c; c.joinPort = 6555; c.worldPort = 6556;
Client net(c);
if (!net.Join()) { SDL_Log("%s", net.LastError().c_str()); return 1; }
std::string map; net.Request("MAP", map);     // any one-shot ask, on my own channel

// in Run's background callback:
net.Tick(dt,
    [&](const std::string &world) { /* decode, store under the world mutex */ },
    [&]() { /* return my state, encoded */ });
```

| `Client::Config` | Default | Meaning |
|---|---|---|
| `joinPort`, `worldPort` | set these | the server's JOIN and world ports |
| `worldTopic` | `"WORLD"` | the broadcast topic to subscribe to |
| `sendIntervalSeconds` | 1/60 | **game** time between upstream sends; 0 sends on every `Tick` |
| `timeoutMs` | 2000 | how long to wait for a reply |
| `heartbeatMs` | 1000 | **wall** time after which an idle client sends a `PING` |
| `worldQueue` | 32 | world messages held before the oldest are dropped |

`Tick` drains the world broadcast every call and sends your state once per
`sendIntervalSeconds` of **game** time. So at 0.5x you send half as often, at
2x twice, and paused you send nothing - while still hearing the world. That is
the point. `Tick` returns true when it sent.

`Connected()` is true while the server is answering or broadcasting to you, so
a paused client, which sends nothing, still counts as connected.
`RoundTrips()` and `SendsPerSecond()` are there for a HUD. Call `Join` and any
early `Request` on the main thread before the loop; during it, `Tick` and
`Request` belong to the background thread, never both threads at once.

A `Client` sends `BYE` when it is destroyed. The server calls `onLeave` and
hands the slot to the next `JOIN`, so closing a window, Ctrl-C or `kill` frees
the slot with no game code. A client that cannot say `BYE` - it crashed - goes
silent, and after `clientTimeoutMs` the server does the same for it. An idle
client (paused, say) sends a `PING` every `heartbeatMs` of wall time, so it is
never mistaken for a dead one. Every request carries the session number its
`WELCOME` gave it, so a client that was timed out and comes back is answered
`GONE` (`Dropped()` turns true) instead of speaking for a slot that may now be
someone else's. If the server goes away, the client stops sending and reports
`Connected() == false`; it does not crash.

### Hybrid peer-to-peer: `Server` + `Client` + `Peer`

Same as above, plus each client publishes its own position straight to the
others. The server keeps the world; players stop routing through it.

```cpp
Peer::Config p; p.portBase = 6700; p.topics = {"PEER"};
Peer peer(p);
peer.Claim(net.Id());     // the server gave me an id; bind that slot
peer.ConnectAll();
// background: after a successful net.Tick send,
peer.Publish("PEER", myEncodedState);
peer.Tick([&](const std::string &topic, std::istringstream &body, int) { /* store */ });
```

### Pure peer-to-peer: `Peer` alone

No server. Nothing is in charge.

```cpp
Peer peer(p);
int myId = peer.Claim();          // first free slot: the port IS the id
peer.ConnectAll();
peer.EnableSharedClock();          // a Timeline whose origin the peers agree on
double now = peer.SharedSeconds(); // the same number on every peer
```

| `Peer::Config` | Default | Meaning |
|---|---|---|
| `portBase` | set this | peer `n` publishes on `portBase + n` |
| `maxPeers` | 4 | slots |
| `gossipMs` | 250 | wall-clock milliseconds between gossip rounds |
| `topics` | none | your game's topics to subscribe to, besides the engine's own |

`Claim()` takes the first free slot; `Claim(n)` takes exactly slot `n` or
returns -1, with the reason in `LastError()`. Binding the port is the claim, so
ids are unique with nobody handing them out, and a slot frees when its process
exits. Claim before opening a window, so a full table does not flash one up.

**The shared clock.** Its origin is an **epoch** agreed by gossiping the
minimum: each peer proposes "now", adopts anything earlier it hears, and ties
go to the lower id. It converges within one gossip period. A world that is a
pure function of this clock - a patrol as a triangle wave of `now` - is the
identical world on every peer with nothing exchanged. `SharedClock()` is the
`Timeline` itself; `Epoch()`, `EpochProposer()` and `EpochAdoptions()` describe
the agreement, and `HeardFrom(id)` and `AgreesOnEpoch(id)` tell a HUD who is in
step.

**The event log.** For a world that *changes*:

```cpp
peer.AppendEvent("fire 12.5 40.0 right");   // stamp it with SharedSeconds() yourself
if (peer.TakeEventsDirty()) {
    for (const Peer::Event &e : peer.Events()) { /* e.owner, e.seq, e.payload */ }
    /* recompute derived state - deterministically, in canonical order, at a fixed step */
}
```

Each peer re-sends every event it knows, anyone's, every gossip; receivers
deduplicate by `(owner, seq)`. Lost messages are re-sent, and a late joiner
learns everything - even what a peer that has since left did, since the peers
still here relay it. `seq` starts at the peer's start time in microseconds, so
a newcomer that takes a departed peer's slot never reuses its ids; an event in
your slot with a seq below `peer.FirstSeq()` was fired by your predecessor.
`EventCount()` counts what you hold. If the derived state is a pure function of
the events, every peer computes the same deaths at the same instants and
nobody ever asks "who killed it?" - see `game/maze_run/mz_world.hpp`.

**Talking.** `Publish(topic, payload)` goes to every other peer at once.
`Tick(onMessage, epochExtras, extraGossip)` drains everything queued - the
engine handles EPOCH and EVENTS itself and hands your topics to `onMessage` -
then gossips once per `gossipMs` of **wall** time. `onMessage`'s `fromId` is
known only for EPOCH; for your own topics it is -1, so put the sender in the
payload, as Maze Run does. Wall, not game time: a peer that has paused its
own world still keeps the mesh alive. Give `epochExtras` a callback to attach a
few numbers to your EPOCH gossip (they reach the other peers' `onMessage` with
topic `"EPOCH"`), and `extraGossip` for anything else you want repeated.
`MessagesHeard()` counts traffic.

## The game object model

A **`GameObject`** is an id, a kind (`"player"`, `"platform"`), a name, and a
set of components. It has no behaviour and no subclasses: what it is comes
from which components it holds, and what it does comes from **systems** -
plain functions that walk the World and act on every object holding the
components they read. A new kind of object is a new combination; a new
behaviour is a new component and a system. Neither touches the engine.

A **`World`** owns a scene's objects. Every endpoint has one.

```cpp
#include "engine/Systems.hpp"
using namespace engine;

World world;
GameObject &ledge = world.Create("platform");
ledge.Add<Transform>(300, 700, 200, 20);
ledge.Add<Collider>(Collider::Type::Solid, /*oneWay=*/true);
ledge.Add<Renderable>(SDL_Color{120, 90, 60, 255});

GameObject &pit = world.Create("deathzone");        // no Renderable: hidden
pit.Add<Transform>(0, 1100, 5000, 200);
pit.Add<DeathZone>();

world.Each<Transform, RigidBody>([](GameObject &o, Transform &t, RigidBody &b) { /* ... */ });
world.Destroy(pit.Id());   // deferred: skipped from now on, deleted at world.Flush()
```

- `Create` hands out ids counting up from where the World starts (`World(1u << 30)`
  keeps a client's local objects clear of the server's ids). `CreateWithId`
  takes an id chosen elsewhere - how a client holds the server's objects.
- `Destroy` is deferred to `Flush()`, so systems may destroy objects while
  iterating, and pointers stay valid until the frame's `Flush`.
- Not internally locked: guard it with `GetWorldMutex()` like any shared state.

### Components

| Component | What it is | Travels |
|---|---|---|
| `Transform` | position and size: the box everything else uses | yes |
| `RigidBody` | velocity, gravity on or off, grounded (and what on) | yes |
| `Collider` | `Solid` (optionally one-way, a ledge you jump up through) or `Trigger` | yes |
| `Renderable` | colour, layer, sprite file and frames, `flipX` (mirror); `visible`. None, or not visible, is hidden | yes |
| `InputState` | the buttons held, as bits: the whole of a player's intent | yes |
| `PlatformerController` | run speed, jump speed, `facing`; with `flipSprite` it mirrors the Renderable to face the way it runs | yes |
| `PathMover` | a pattern as a function of time: `Linear` (horizontal or vertical) or `Circular` | yes |
| `SpawnPoint` | a hidden place a player (re)appears; `index` | yes |
| `DeathZone` | a hidden area that sends `Respawn` objects back to a spawn point | yes |
| `Respawn` | marks objects death zones act on; which spawn point; deaths | yes |
| `NetIdentity` | part of the networked scene; `owner` (-1 = the authority); a time stamp | in the header |
| `Camera` | the view: position, size, target, bounds | no (each client its own) |
| `ScrollBoundary` | a hidden edge that rides a camera; touching it scrolls the scene | no |

Your own: derive from `Component`, name it with `ENGINE_COMPONENT(Name)`,
write and read the fields that should travel, and register it on every
endpoint before the network starts:

```cpp
struct Health : engine::Component {
    ENGINE_COMPONENT(Health)
    int hp = 3;
    void Write(engine::Writer &w) const override { w.I32(hp); }
    void Read(engine::Reader &r) override { hp = r.I32(); }
};
engine::ComponentRegistry::Register<Health>();
```

A component without `ENGINE_COMPONENT` (like `Camera`) never leaves its process.

### Systems

| System | Reads | Does |
|---|---|---|
| `systems::MovePaths(world, seconds)` | `Transform`, `PathMover` | places each moving object for a clock: the server's, or a shared one |
| `systems::ApplyControllers(world)` | `InputState`, `PlatformerController`, `RigidBody` | run left/right; jump from the ground on a fresh press |
| `systems::StepPhysics(world, dt)` | `Transform`, `RigidBody`, `Collider` | gravity; X then Y against solids; land, bump, pass up through one-way ledges; ride moving platforms |
| `systems::ApplyDeathZones(world)` | `DeathZone`, `Respawn`, `SpawnPoint` | anything in a death zone goes to its spawn point; returns what happened |
| `TriggerTracker::Update(world)` | `Collider` (Trigger), `Transform`, `RigidBody` | which movers entered, stayed in or left which triggers since last time |
| `systems::ReadKeyboard(input)` | the keyboard | sets an `InputState` from key bindings (`DefaultKeyBindings()`, or your own) |
| `systems::UpdateCameras(world)` | `Camera`, `ScrollBoundary` | a target touching a boundary scrolls the camera by exactly the overlap |
| `systems::Render(world, renderer, camera, scale, seconds, &textures)` | `Transform`, `Renderable` | draws what is visible, lowest layer first |

Each takes an optional filter so an endpoint can act only on what it owns:
`OwnedBy(id)`, or a `SceneClient`'s `Authority()`. `AddScrollBoundaries`
makes a camera's hidden side boundaries; `PlaceAtSpawn` puts an object on a
spawn point.

**Triggers.** Give an object a `Collider(Collider::Type::Trigger)` and keep one
`TriggerTracker`. Each `Update` returns `Enter` once when a mover arrives,
`Stay` while it is there and `Exit` when it leaves or the trigger is destroyed.
A gem is "destroy on `Enter`"; a pressure plate is "open while anything
`Stay`s"; a checkpoint is "set `Respawn::spawnIndex` on `Enter`".

```cpp
TriggerTracker triggers;   // kept between frames
for (const TriggerEvent &e : triggers.Update(world)) {
    GameObject *t = world.Find(e.trigger);
    if (t && t->Kind() == "gem" && e.type == TriggerEvent::Type::Enter) world.Destroy(e.trigger);
}
```

**Sprites.** `Renderable::sprite` names a file under `media/`, loaded once by a
`TextureCache`; `TextureCache::Put(name, texture)` registers one the game made
itself. Draw sprites facing right: `flipX` mirrors them, and a
`PlatformerController` sets it from the way its object last ran.

## A networked scene: `SceneServer`, `SceneClient`, `ScenePeer`

Every endpoint holds a World; these keep the Worlds in step by sending
**snapshots**. A snapshot carries each object with a `NetIdentity` - its id,
kind, owner, time stamp and components. Everything else stays local.

**Server.** Build the level, say how to make a player, and run:

```cpp
Engine engine;
engine.InitHeadless();
Server::Config cfg;
cfg.joinPort = 6555; cfg.worldPort = 6556; cfg.clientPortBase = 6600;
cfg.maxClients = 4;            // 0: no limit
cfg.publishEverySteps = 1;     // send the world right after each tick: lowest latency
SceneServer scene(engine, cfg, options);
BuildLevel(scene.GetWorld());  // every level object gets a NetIdentity
SceneServer::Callbacks cb;
cb.onJoin = [](World &w, int id) { MakePlayer(w, id); };            // NetIdentity(id)
cb.step = [&](World &w, float dt) {
    systems::MovePaths(w, seconds += dt);
    systems::ApplyControllers(w, scene.Authority());
    systems::StepPhysics(w, dt, PhysicsSettings{}, scene.Authority());
};
scene.Run(cb);   // a client that leaves, or goes silent, has its objects removed for you
```

**Client.** Join, then call `Tick` from the engine's background thread:

```cpp
SceneClient net(clientConfig, options);
net.Join();
engine.Run(
    [&](float dt) {                                 // main thread
        std::lock_guard<std::mutex> lock(engine.GetWorldMutex());
        if (GameObject *me = net.MyPlayer()) { /* InputState from the keyboard */ }
        systems::ApplyControllers(net.GetWorld(), net.Authority());
        systems::StepPhysics(net.GetWorld(), dt, PhysicsSettings{}, net.Authority());
        net.StampLocal();                           // latency is measured from here
    },
    [&]() { /* systems::Render(net.GetWorld(), ...) */ },
    [&](float dt) { net.Tick(dt, engine.GetWorldMutex()); });   // background thread
```

**Peers.** No server: every peer builds the level itself (before `Claim`, so
the ids match), claims a slot, adds its own player, and calls `Tick`.
Moving platforms run on the peers' shared clock, so nobody owns them. A peer
can only send objects it owns, so something shared that **changes** - a gem
anyone can collect, a door anyone can open - has no automatic way to stay in
step between peers. Build such a game client-server, or carry those changes
yourself on `GetPeer()`'s event log.

**Who decides what.** The authority (the server) owns everything shared. A
client owns its own player when it sends state, and the server accepts a
client's word only for that client's objects. When clients send inputs
instead, the server moves their players too, and `Authority()` tells each
client to simulate nothing.

Because a client sending state owns its player, the server's changes to that
object never reach it. Keep what the server decides about a player - gems
collected, a score, a "you were stomped" - on an object the **server** owns
(a scoreboard, or one per player), and have the client read it from there.
`scene_demo`'s `TeamScore` is that pattern.

**What a snapshot carries.** New objects, changed components, and removals of
both: when the server destroys an object it disappears everywhere, and when it
takes a component off an object - a gate's `Collider` when the gate opens -
the clients' copies lose it too, so a client simulating its own player walks
through the open gate. Components a client attached itself (to dress an
object by kind) are never removed by the network. A message that carries only
some components (`--upstream input`) never removes any.

**Disconnects.** `BYE`, closing the window, Ctrl-C and `kill` all leave
politely. A client that crashes or is `kill -9`'d goes silent, and after
`clientTimeoutMs` (3 s) the server removes it exactly as if it had left. A
paused client sends a `PING` on wall time, so it is never mistaken for a dead
one. A timed-out client that comes back is told `GONE` (`Client::Dropped()`).
Peers do the same with each other.

**Latency.** Each object carries the moment its owner last stamped it, and
every endpoint on one machine reads the same steady clock, so
`Stats().Latency()` is a measured one-way latency: mean, median, 95th
percentile and maximum. Client-server at 60 Hz: about 14 ms mean; peers: about 5 ms.

### What an experiment can change: `NetOptions`

The same flags work in every program that calls `NetOptions::Parse`:

| Flag | Values | Changes |
|---|---|---|
| `--topology` | `cs`, `p2p` | a server, or peers with no server |
| `--encoding` | `text`, `binary` | how each field is written |
| `--delta` | `off`, `on` | every object every time, or only what changed (plus a keyframe every `--keyframe N`) |
| `--upstream` | `state`, `input` | endpoints send their objects, or only their `InputState` |

None of them changes the game: every combination ends with the same World on
every screen (`netscene_test` checks six of them).

## The worked example: `scene_demo`

`engine/examples/scene_demo.cpp` is a whole networked platformer in about 350
lines, built only from the pieces above: three static platforms (one a
one-way ledge), three moving ones (horizontal, vertical, circular), hidden
spawn points, a hidden death zone under a pit, side-scrolling, players that
run, jump, face the way they run and can stand on each other, a gem picked up
through a trigger, a pressure plate that opens a gate by removing its
`Collider`, and a game-defined component.

```
./build/scene_demo server
./build/scene_demo client          # in up to four more terminals; A/D run, W or Space jump
./build/scene_demo client --walk   # plays itself
```

Both take the experiment flags (`--encoding binary --delta on`, ...). Copy it
as the start of a game.

## Measuring: `net_bench` and `net_experiments.py`

`net_bench server|client|peer` (from `engine/bench/`) runs one endpoint of a networked scene with an
uncapped loop on a generated level - a floor, `--static S` platforms,
`--moving M` moving platforms in three patterns, spawn points and a death
zone - and prints one `RESULT` line. The server (or, with no server, peer 0)
times `--iterations N` loop iterations, each of which simulates the world,
builds the snapshot and sends it.

```
python3 engine/tools/net_experiments.py --compare encoding
python3 engine/tools/net_experiments.py --compare delta --base "--encoding binary"
python3 engine/tools/net_experiments.py --compare topology --reps 5 --iterations 100000
```

The runner builds an optimized copy in `build-release/` (the default build
has no optimization), runs three experiments - moving platforms 10/100/500,
clients 1/2/4, static platforms 10/100/500 - for both sides of the
comparison, `--reps` times each, and writes to `results/`: every run, a
summary with the mean, variance and standard deviation, and SVG charts.

## Thread rules at a glance

| Call | From |
|---|---|
| `Input`, rendering, `Entity::Draw` | the main thread only (SDL's rule) |
| `Timeline`, any method | any thread |
| Shared world state | any thread, under `GetWorldMutex()` |
| `JobSystem::Submit`, `RunAndWait` | one producing thread |
| `Client::Join` | the main thread, before the loop |
| `Client::Tick`, `Client::Request` | the background thread during the loop - never two threads at once |
| `Server` hooks | the threads in the hook table, concurrently |
| `Peer::Tick`, `Peer::Publish` | the background thread |
| `Peer::AppendEvent` and the `Peer` getters | any thread |
| `World` and every system | any one thread at a time, under `GetWorldMutex()` |
| `SceneServer` callbacks | the main loop, with the world mutex already held |
| `SceneClient::Join` | the main thread, before the loop |
| `SceneClient::Tick`, `ScenePeer::Tick` | the background thread; they take the mutex you pass |
| `ComponentRegistry::Register` | startup, before any network thread runs |

## Verifying

`engine/tests/` holds 14 headless programs, one per claim, that need no game
and no running server - each starts whatever it needs, on its own ports
(5596-5599 and 7555-7999). They are off by default, so they never clash with a game's own
targets; turn them on and run them:

```
cmake -S . -B build -DENGINE_TESTS=ON
cmake --build build
./build/netscene_test        # and the others: each ends with ALL PASS or FAILED
```

| Test | Checks |
|---|---|
| `timeline_test`, `frametime_test`, `iteration_test`, `integration_test` | time: anchoring, tic, pause, 0.5x-2.0x, loop iterations, game-time-paced sending |
| `net_test`, `engine_server_test`, `engine_client_test`, `engine_peer_test` | sockets, the server and client, the peer mesh, slots, BYE |
| `jobsystem_test`, `collision_test` | the job pool; collision push-out |
| `object_model_test` | GameObject, components, World, deferred destroy, ids |
| `systems_test` | moving paths, physics, platforms, control, triggers, death zones, scrolling, drawing |
| `snapshot_test` | text and binary encoding, full and delta snapshots, removals, ownership |
| `netscene_test` | a server, clients and peers over real sockets in six setting combinations; latency; disconnects |

In the team repository `tests/README.md` describes each one in detail, beside
the games' own tests. Build with `-fsanitize=thread` and run anything
networked for fifteen seconds before you trust a change to the threading.

## Changes since Milestone 2 that can affect an existing game

Code written against the Milestone 2 engine still compiles and runs: `Entity`,
`Server`, `Client` and `Peer` keep their interfaces. A few behaviours changed:

- **`Server` times out silent clients** after `clientTimeoutMs` (3 s). An
  `engine::Client` sends a `PING` by itself when idle, but only from `Tick()`:
  keep calling `Tick` every frame, paused or not. Set `clientTimeoutMs = 0`
  for the old behaviour.
- **Requests to a freed slot are answered `GONE`** instead of reaching your
  `onRequest`, and `WELCOME` carries one more number (a session id) at its end.
- **`RunHeadless(update, 0)` and a `tickHz` of 0 or less now mean uncapped** -
  as fast as the loop can go. Pass a rate (60) if you passed 0 before.
- **World broadcasts keep at most 32 queued messages per client**
  (`worldQueue`); older ones are dropped rather than delivered late.
- **`Peer::Claim(id)` retries for a quarter of a second** before saying a
  requested slot is taken.
- **`Renderable`/`PlatformerController` gained fields** (`flipX`, `facing`,
  `flipSprite`), so snapshots from an older build will not decode: every
  endpoint must run the same engine.

## Things to know before you build on it

- **Slots.** `Server` hands out the lowest free id and reuses a slot when its
  client leaves, by `BYE` or by going silent past `clientTimeoutMs`.
  `maxClients` 0 means no limit. `Peer` has `maxPeers` ports, freed on exit.
- **One host.** Servers and peers listen on every interface, but `Client`
  and `Peer` connect to `localhost`, hard-coded. The shared clock uses
  `steady_clock`, which is per-machine; peers on different hosts would need
  the epoch corrected for clock offset.
- **Bare `Peer`s never time out.** `ScenePeer` drops a peer that has been
  silent past `peerTimeoutMs`; a game using `Peer` directly has to do it itself.
- **The default build is not optimized.** Pass `-DCMAKE_BUILD_TYPE=Release`
  when you time anything (`engine/tools/net_experiments.py` does).
- **The event log grows.** Every gossip re-sends all of it. Fine for a session;
  a long game would want compaction.
- **`Input` is main-thread only.** SDL's rule, not ours.
