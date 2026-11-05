// scene_demo - the smallest complete networked platformer on the Team 16
// engine, written to be copied from. Everything a Milestone 3 game needs is
// here once, in plain form:
//
//   three static platforms (each a different position, size and colour, one a
//   one-way ledge), three moving ones (horizontal, vertical, circular), hidden
//   spawn points, a hidden death zone under a pit, side-scrolling boundaries,
//   players that run, jump, face the way they run and can stand on each other,
//   a gem picked up through a trigger, a pressure plate that opens a gate by
//   taking its Collider away, and a game-defined component (TeamScore).
//
//   ./build/scene_demo server              headless; owns the world
//   ./build/scene_demo client              one per player, up to four (A/D or arrows run, W/Up/Space jump)
//
// Both take the engine's experiment flags (--encoding, --delta, --upstream);
// the client also takes --walk (it plays itself), --shot S --shot-name N (save
// the window after S seconds), --x --y --w --h (window placement).
#include "engine/Engine.hpp"
#include "engine/Input.hpp"
#include "engine/NetScene.hpp"

#include <SDL3/SDL_main.h>
#include <SDL3_image/SDL_image.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

using namespace engine;

namespace {

constexpr int JOIN_PORT = 6855, WORLD_PORT = 6856, CLIENT_PORT_BASE = 6900;
constexpr float LEVEL_WIDTH = 4800.0f, LEVEL_HEIGHT = 800.0f;

// A game's own component. It travels like the built-in ones once registered
// on every endpoint.
struct TeamScore : Component {
    ENGINE_COMPONENT(TeamScore)
    int gems = 0;
    void Write(Writer &w) const override { w.I32(gems); }
    void Read(Reader &r) override { gems = r.I32(); }
};

int Arg(int argc, char **argv, const char *flag, int fallback) {
    for (int i = 1; i + 1 < argc; ++i) if (std::strcmp(argv[i], flag) == 0) return std::atoi(argv[i + 1]);
    return fallback;
}
const char *StrArg(int argc, char **argv, const char *flag, const char *fallback) {
    for (int i = 1; i + 1 < argc; ++i) if (std::strcmp(argv[i], flag) == 0) return argv[i + 1];
    return fallback;
}
bool Flag(int argc, char **argv, const char *flag) {
    for (int i = 1; i < argc; ++i) if (std::strcmp(argv[i], flag) == 0) return true;
    return false;
}

GameObject &Box(World &w, const char *kind, float x, float y, float width, float height, SDL_Color color) {
    GameObject &o = w.Create(kind);
    o.Add<Transform>(x, y, width, height);
    o.Add<Renderable>(color, 1);
    o.Add<NetIdentity>();   // owned by the server, sent to everyone
    return o;
}

// The level. Built on the server; every client receives it.
void BuildLevel(World &w) {
    // Ground, with a pit between 1400 and 1700.
    Box(w, "ground", 0, 680, 1400, 120, SDL_Color{96, 92, 104, 255}).Add<Collider>();
    Box(w, "ground", 1700, 680, LEVEL_WIDTH - 1700, 120, SDL_Color{96, 92, 104, 255}).Add<Collider>();

    // Three static platforms, each a different position, size and colour.
    Box(w, "platform", 300, 560, 260, 24, SDL_Color{150, 105, 70, 255}).Add<Collider>();
    Box(w, "platform", 680, 450, 120, 40, SDL_Color{70, 120, 170, 255}).Add<Collider>();
    Box(w, "ledge", 940, 340, 220, 12, SDL_Color{90, 170, 100, 255}).Add<Collider>(Collider::Type::Solid, true);   // jump up through it

    // Three moving platforms: across the pit, a lift, and a circle.
    struct Mover { float x, y, dx, dy, radius, period; };
    const Mover movers[] = {{1420, 600, 140, 0, 0, 4}, {2100, 330, 0, 300, 0, 5}, {2700, 420, 0, 0, 110, 6}};
    for (const Mover &m : movers) {
        GameObject &o = Box(w, "mover", m.x, m.y, 140, 20, SDL_Color{220, 190, 70, 255});
        o.Add<Collider>();
        PathMover &p = o.Add<PathMover>();
        p.origin = {m.x, m.y};
        p.offset = {m.dx, m.dy};
        p.radius = m.radius;
        p.period = m.period;
        if (m.radius > 0) p.pattern = PathMover::Pattern::Circular;
    }

    // Hidden: four spawn points and the death zone under the pit. No Renderable.
    for (int i = 0; i < 4; ++i) {
        GameObject &s = w.Create("spawn");
        s.Add<Transform>(60.0f + 70.0f * static_cast<float>(i), 580.0f, 40.0f, 100.0f);
        s.Add<SpawnPoint>(i);
        s.Add<NetIdentity>();
    }
    GameObject &pit = w.Create("pit");
    pit.Add<Transform>(-500, 900, LEVEL_WIDTH + 1000, 300);
    pit.Add<DeathZone>();
    pit.Add<NetIdentity>();

    // A gem above the ledge, a plate, and the gate it opens.
    Box(w, "gem", 1040, 290, 20, 20, SDL_Color{255, 225, 60, 255}).Add<Collider>(Collider::Type::Trigger);
    Box(w, "plate", 3100, 672, 90, 8, SDL_Color{200, 70, 60, 255}).Add<Collider>(Collider::Type::Trigger);
    Box(w, "gate", 3400, 480, 30, 200, SDL_Color{170, 170, 190, 255}).Add<Collider>();

    GameObject &score = w.Create("score");
    score.Add<TeamScore>();
    score.Add<NetIdentity>();
}

const SDL_Color PLAYER_COLORS[4] = {{250, 200, 70, 255}, {90, 210, 255, 255}, {255, 120, 170, 255}, {150, 255, 140, 255}};

void AddPlayer(World &w, int id) {
    GameObject &p = w.Create("player");
    p.Add<Transform>(0, 0, 32, 48);
    p.Add<RigidBody>().gravity = true;
    p.Add<Collider>(Collider::Type::Solid, true);   // one-way: others can stand on you
    p.Add<InputState>();
    p.Add<PlatformerController>();
    p.Add<Respawn>().spawnIndex = id % 4;
    Renderable &r = p.Add<Renderable>(PLAYER_COLORS[id % 4], 2);
    r.sprite = "player";   // made by each client at startup (see MakePlayerSprite)
    p.Add<NetIdentity>(id);
    systems::PlaceAtSpawn(w, p, id % 4);
}

// --- server --------------------------------------------------------------------

int RunServer(int argc, char **argv, const NetOptions &options) {
    Engine engine;
    if (!engine.InitHeadless()) return 1;
    Server::Config cfg;
    cfg.joinPort = JOIN_PORT;
    cfg.worldPort = WORLD_PORT;
    cfg.clientPortBase = CLIENT_PORT_BASE;
    cfg.maxClients = Arg(argc, argv, "--max-clients", 4);
    cfg.publishEverySteps = 1;   // the world goes out right after each tick
    cfg.logTag = "scene-demo";
    SceneServer scene(engine, cfg, options);
    BuildLevel(scene.GetWorld());
    printf("[scene-demo] %s, %zu objects in the level\n", options.Describe().c_str(), scene.GetWorld().Count());

    double seconds = 0.0, gateTimer = 0.0;
    TriggerTracker triggers;
    SceneServer::Callbacks cb;
    cb.onJoin = [](World &w, int id) { AddPlayer(w, id); };
    cb.step = [&](World &w, float dt) {
        seconds += dt;
        systems::MovePaths(w, seconds);
        systems::ApplyControllers(w, scene.Authority());
        systems::StepPhysics(w, dt, PhysicsSettings{}, scene.Authority());
        systems::ApplyDeathZones(w, scene.Authority());

        // The server decides what players touch: it has everyone's position.
        bool plateHeld = false;
        for (const TriggerEvent &e : triggers.Update(w)) {
            GameObject *trigger = w.Find(e.trigger);
            if (!trigger) continue;
            if (trigger->Kind() == "gem" && e.type == TriggerEvent::Type::Enter) {
                w.Destroy(e.trigger);   // gone for everyone: the snapshot leaves it out
                w.Each<TeamScore>([](GameObject &, TeamScore &s) { ++s.gems; });
            }
            if (trigger->Kind() == "plate" && e.type != TriggerEvent::Type::Exit) plateHeld = true;
        }
        // The gate opens while the plate is held and stays open 2 s after.
        gateTimer = plateHeld ? 2.0 : std::max(0.0, gateTimer - dt);
        w.Each<Renderable>([&](GameObject &o, Renderable &r) {
            if (o.Kind() != "gate") return;
            const bool open = gateTimer > 0.0;
            if (open && o.Has<Collider>()) o.Remove<Collider>();   // the removal reaches every client
            if (!open && !o.Has<Collider>()) o.Add<Collider>();
            r.color.a = open ? 60 : 255;
        });
    };
    return scene.Run(cb) ? 0 : 1;
}

// --- client --------------------------------------------------------------------

// A plain arrow-ish figure facing right, white so the player colour tints it:
// Renderable::flipX turns it round when its owner runs left.
SDL_Texture *MakePlayerSprite(SDL_Renderer *renderer) {
    SDL_Surface *s = SDL_CreateSurface(16, 24, SDL_PIXELFORMAT_RGBA32);
    SDL_FillSurfaceRect(s, nullptr, SDL_MapSurfaceRGBA(s, 0, 0, 0, 0));
    SDL_Rect body = {2, 4, 11, 20};
    SDL_FillSurfaceRect(s, &body, SDL_MapSurfaceRGBA(s, 255, 255, 255, 255));
    SDL_Rect eye = {9, 7, 3, 3};
    SDL_FillSurfaceRect(s, &eye, SDL_MapSurfaceRGBA(s, 30, 30, 40, 255));
    SDL_Rect nose = {13, 11, 3, 4};
    SDL_FillSurfaceRect(s, &nose, SDL_MapSurfaceRGBA(s, 255, 255, 255, 255));
    SDL_Texture *t = SDL_CreateTextureFromSurface(renderer, s);
    SDL_SetTextureScaleMode(t, SDL_SCALEMODE_NEAREST);
    SDL_DestroySurface(s);
    return t;
}

void SaveShot(SDL_Renderer *renderer, const char *name) {
    SDL_Surface *shot = SDL_RenderReadPixels(renderer, nullptr);
    if (!shot) return;
    const char *base = SDL_GetBasePath();
    char dir[1024], path[1200];
    SDL_snprintf(dir, sizeof(dir), "%s../screenshots", base ? base : "");
    SDL_CreateDirectory(dir);
    SDL_snprintf(path, sizeof(path), "%s/%s.png", dir, name);
    if (IMG_SavePNG(shot, path)) SDL_Log("wrote %s", path);
    SDL_DestroySurface(shot);
}

int RunClient(int argc, char **argv, const NetOptions &options) {
    const int width = Arg(argc, argv, "--w", 1280), height = Arg(argc, argv, "--h", 720);
    const bool walk = Flag(argc, argv, "--walk");
    const double shotAfter = Arg(argc, argv, "--shot", -1);
    const std::string shotName = StrArg(argc, argv, "--shot-name", "scene_demo");

    Client::Config cfg;
    cfg.joinPort = JOIN_PORT;
    cfg.worldPort = WORLD_PORT;
    SceneClient net(cfg, options);
    if (!net.Join()) {   // before the window, so a failed join does not flash one
        fprintf(stderr, "scene_demo: %s\n", net.LastError().c_str());
        return 1;
    }

    Engine engine;
    if (!engine.Init("Scene Demo", width, height)) return 1;
    if (Arg(argc, argv, "--x", -1) >= 0) SDL_SetWindowPosition(engine.GetWindow(), Arg(argc, argv, "--x", 0), Arg(argc, argv, "--y", 0));
    engine.SetTargetFrameRate(120);
    TextureCache textures;
    textures.Put("player", MakePlayerSprite(engine.GetRenderer()));

    World &world = net.GetWorld();
    std::mutex &mutex = engine.GetWorldMutex();
    // The whole height of the level fits the window; the camera scrolls sideways.
    const float scale = static_cast<float>(height) / LEVEL_HEIGHT;
    ObjectId cameraId;
    {
        std::lock_guard<std::mutex> lock(mutex);
        GameObject &cam = world.Create("camera");   // local: a client-range id, never sent
        Camera &c = cam.Add<Camera>();
        c.view = {static_cast<float>(width) / scale, LEVEL_HEIGHT};
        c.boundsMin = {0.0f, 0.0f};
        c.boundsMax = {LEVEL_WIDTH, LEVEL_HEIGHT};
        cameraId = cam.Id();
        systems::AddScrollBoundaries(world, cameraId, c.view.x * 0.35f);   // hidden, 35% in from each side
    }

    const auto started = std::chrono::steady_clock::now();
    bool shotTaken = false;
    double t = 0.0;
    engine.Run(
        // Main thread: my input, my player, my camera.
        [&](float dt) {
            if (Input::IsKeyJustPressed(SDL_SCANCODE_ESCAPE)) engine.Stop();
            t += dt;
            std::lock_guard<std::mutex> lock(mutex);
            GameObject *me = net.MyPlayer();
            if (!me) return;
            InputState &in = *me->Get<InputState>();
            if (walk) in.buttons = InputState::Right | (std::fmod(t, 1.2) < 0.1 ? InputState::Jump : 0u);
            else systems::ReadKeyboard(in);
            systems::ApplyControllers(world, net.Authority());
            systems::StepPhysics(world, dt, PhysicsSettings{}, net.Authority());
            systems::ApplyDeathZones(world, net.Authority());
            net.StampLocal();
            world.Find(cameraId)->Get<Camera>()->target = me->Id();
            systems::UpdateCameras(world);
        },
        // Main thread: draw.
        [&]() {
            SDL_Renderer *r = engine.GetRenderer();
            SDL_SetRenderDrawColor(r, 28, 30, 40, 255);
            SDL_RenderClear(r);
            std::lock_guard<std::mutex> lock(mutex);
            const Camera &cam = *world.Find(cameraId)->Get<Camera>();
            systems::Render(world, r, cam, scale, engine.GetGameTime().GetSeconds(), &textures);

            int players = 0, gems = 0;
            world.Each<NetIdentity>([&](GameObject &o, NetIdentity &) { if (o.Kind() == "player") ++players; });
            world.Each<TeamScore>([&](GameObject &, TeamScore &s) { gems = s.gems; });
            const LatencySummary lat = net.Stats().Latency();
            char hud[200];
            SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
            SDL_snprintf(hud, sizeof hud, "SCENE DEMO   player %d   players %d   gems %d   %s",
                         net.Id(), players, gems, net.GetClient().Connected() ? "connected" : "waiting for server");
            SDL_RenderDebugText(r, 12, 12, hud);
            SDL_snprintf(hud, sizeof hud, "A/D run  W/Space jump   latency %.1f ms (p95 %.1f)   %s",
                         lat.meanMs, lat.p95Ms, options.Describe().c_str());
            SDL_RenderDebugText(r, 12, 28, hud);

            if (shotAfter >= 0 && !shotTaken &&
                std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() >= shotAfter) {
                shotTaken = true;
                SaveShot(r, shotName.c_str());
            }
        },
        // Background thread: the network.
        [&](float dt) { net.Tick(dt, mutex); });

    net.Leave();
    engine.Shutdown();
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    ComponentRegistry::RegisterBuiltins();
    ComponentRegistry::Register<TeamScore>();   // on every endpoint, before the network starts
    NetOptions options;
    std::string error;
    if (argc < 2 || !NetOptions::Parse(argc, argv, options, &error)) {
        fprintf(stderr, "usage: scene_demo server|client [--encoding text|binary] [--delta on|off] [--upstream state|input]\n%s\n",
                error.c_str());
        return 2;
    }
    if (std::strcmp(argv[1], "server") == 0) return RunServer(argc, argv, options);
    if (std::strcmp(argv[1], "client") == 0) return RunClient(argc, argv, options);
    fprintf(stderr, "scene_demo: server or client, not %s\n", argv[1]);
    return 2;
}
