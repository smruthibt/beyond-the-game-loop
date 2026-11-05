// Milestone 3: the systems that give the object model its behaviour - moving
// platforms, platformer control, physics, spawn points, death zones,
// side-scrolling boundaries and rendering.
#include "engine/Systems.hpp"

#include <cmath>
#include <cstdio>

using namespace engine;

static int failures = 0;
static void Check(bool ok, const char *name, float a = 0, float b = 0) {
    if (ok) printf("  PASS  %s\n", name);
    else    printf("  FAIL  %s   (%.3f, %.3f)\n", name, a, b), ++failures;
}
static bool Near(float a, float b, float eps = 0.01f) { return std::fabs(a - b) < eps; }

static GameObject &Floor(World &w, float x, float y, float width, float height, bool oneWay = false) {
    GameObject &f = w.Create("platform");
    f.Add<Transform>(x, y, width, height);
    f.Add<Collider>(Collider::Type::Solid, oneWay);
    return f;
}
static GameObject &Body(World &w, float x, float y, bool gravity = true) {
    GameObject &p = w.Create("player");
    p.Add<Transform>(x, y, 32.0f, 48.0f);
    p.Add<RigidBody>().gravity = gravity;
    return p;
}
static void Run(World &w, float seconds, float dt = 1.0f / 60.0f) {
    for (float t = 0; t < seconds; t += dt) systems::StepPhysics(w, dt);
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("Systems verification\n\n");

    printf("Moving platforms (PathMover)\n");
    {
        World w;
        GameObject &h = w.Create("platform");
        h.Add<Transform>();
        PathMover &ph = h.Add<PathMover>();
        ph.origin = {100.0f, 200.0f};
        ph.offset = {300.0f, 0.0f};
        ph.period = 4.0f;
        GameObject &c = w.Create("platform");
        c.Add<Transform>();
        PathMover &pc = c.Add<PathMover>();
        pc.pattern = PathMover::Pattern::Circular;
        pc.origin = {500.0f, 500.0f};
        pc.radius = 80.0f;
        pc.period = 2.0f;

        systems::MovePaths(w, 0.0);
        const Vec2 h0 = h.Get<Transform>()->position;
        systems::MovePaths(w, 2.0);
        const Vec2 hHalf = h.Get<Transform>()->position;
        systems::MovePaths(w, 4.0);
        const Vec2 hFull = h.Get<Transform>()->position;
        Check(Near(h0.x, 100.0f) && Near(hHalf.x, 400.0f) && Near(hFull.x, 100.0f) && Near(hHalf.y, 200.0f),
              "linear: at the origin, then origin+offset half a period later, then back", h0.x, hHalf.x);

        bool onCircle = true;
        for (double t = 0; t < 2.0; t += 0.1) {
            systems::MovePaths(w, t);
            const Vec2 p = c.Get<Transform>()->position;
            onCircle = onCircle && Near(std::hypot(p.x - 500.0f, p.y - 500.0f), 80.0f, 0.05f);
        }
        Check(onCircle, "circular: always `radius` from the origin");
        systems::MovePaths(w, 0.5);
        const Vec2 quarter = c.Get<Transform>()->position;
        Check(Near(quarter.x, 500.0f, 0.05f) && Near(quarter.y, 580.0f, 0.05f), "circular: a quarter period is a quarter turn", quarter.x, quarter.y);

        systems::MovePaths(w, 1.3);
        const Vec2 once = h.Get<Transform>()->position;
        systems::MovePaths(w, 1.3);
        Check(Near(once.x, h.Get<Transform>()->position.x), "position is a pure function of time: same time, same place");
    }

    printf("\nPhysics\n");
    {
        World w;
        Floor(w, 0, 500, 1000, 40);
        GameObject &p = Body(w, 100, 100);
        Run(w, 2.0f);
        const Transform &t = *p.Get<Transform>();
        Check(Near(t.Bottom(), 500.0f) && p.Get<RigidBody>()->grounded && p.Get<RigidBody>()->velocity.y == 0.0f,
              "a falling body lands on the floor and stops, grounded", t.Bottom(), 500.0f);

        GameObject &wall = Floor(w, 300, 300, 40, 200);
        p.Get<RigidBody>()->velocity.x = 400.0f;
        Run(w, 1.0f);
        Check(Near(t.Right(), wall.Get<Transform>()->Left()) && p.Get<RigidBody>()->velocity.x == 0.0f,
              "a wall stops it flush against its face", t.Right(), wall.Get<Transform>()->Left());
    }
    {
        World w;
        Floor(w, 0, 100, 1000, 20);                 // ceiling
        GameObject &p = Body(w, 100, 200, false);
        p.Get<RigidBody>()->velocity.y = -600.0f;
        Run(w, 0.5f);
        Check(Near(p.Get<Transform>()->Top(), 120.0f) && p.Get<RigidBody>()->velocity.y == 0.0f,
              "moving up into a ceiling: stopped under it", p.Get<Transform>()->Top(), 120.0f);
    }
    {
        World w;
        Floor(w, 0, 600, 1000, 40);
        Floor(w, 0, 400, 1000, 16, true);           // a one-way ledge between
        GameObject &p = Body(w, 100, 552);          // standing on the floor, under the ledge
        Run(w, 0.2f);
        p.Get<RigidBody>()->velocity.y = -900.0f;   // jump up through it
        bool passed = false;
        for (int i = 0; i < 120; ++i) {
            systems::StepPhysics(w, 1.0f / 60.0f);
            if (p.Get<Transform>()->Bottom() < 400.0f) passed = true;
        }
        Check(passed, "a one-way ledge lets a body jump up through it");
        Check(Near(p.Get<Transform>()->Bottom(), 400.0f) && p.Get<RigidBody>()->grounded,
              "and then holds it when it lands from above", p.Get<Transform>()->Bottom(), 400.0f);
    }
    {
        World w;
        GameObject &plat = Floor(w, 100, 500, 200, 20);
        PathMover &pm = plat.Add<PathMover>();
        pm.origin = {100.0f, 500.0f};
        pm.offset = {400.0f, 0.0f};
        pm.period = 8.0f;
        systems::MovePaths(w, 0.0);
        GameObject &p = Body(w, 150, 452);
        Run(w, 0.1f);
        const float startX = p.Get<Transform>()->position.x;
        const float platStart = plat.Get<Transform>()->position.x;
        double now = 0.0;
        for (int i = 0; i < 120; ++i) {
            now += 1.0 / 60.0;
            systems::MovePaths(w, now);
            systems::StepPhysics(w, 1.0f / 60.0f);
        }
        const float platMoved = plat.Get<Transform>()->position.x - platStart;
        const float riderMoved = p.Get<Transform>()->position.x - startX;
        Check(platMoved > 50.0f && Near(riderMoved, platMoved, 0.5f) && p.Get<RigidBody>()->grounded,
              "a moving platform carries the body standing on it", riderMoved, platMoved);
    }
    {
        World w;
        Floor(w, 0, 500, 2000, 40);
        GameObject &p = Body(w, 100, 452);
        p.Add<InputState>();
        p.Add<PlatformerController>();
        Run(w, 0.1f);
        InputState &in = *p.Get<InputState>();
        in.Set(InputState::Right, true);
        systems::ApplyControllers(w);
        Check(Near(p.Get<RigidBody>()->velocity.x, 320.0f), "Right held: runs right at moveSpeed");
        in.Set(InputState::Right, false);
        in.Set(InputState::Left, true);
        systems::ApplyControllers(w);
        Check(Near(p.Get<RigidBody>()->velocity.x, -320.0f), "Left held: runs left");
        in.buttons = InputState::Jump;
        systems::ApplyControllers(w);
        Check(Near(p.Get<RigidBody>()->velocity.y, -760.0f), "Jump on the ground: jumps");
        systems::StepPhysics(w, 1.0f / 60.0f);
        p.Get<RigidBody>()->velocity.y = 0.0f;
        systems::ApplyControllers(w);
        Check(p.Get<RigidBody>()->velocity.y == 0.0f, "Jump still held, in the air: no second jump");
        for (int i = 0; i < 120; ++i) { systems::ApplyControllers(w); systems::StepPhysics(w, 1.0f / 60.0f); }
        const bool landed = p.Get<RigidBody>()->grounded;
        systems::ApplyControllers(w);
        Check(landed && p.Get<RigidBody>()->velocity.y == 0.0f, "still held after landing: no bounce until it is pressed again");
        in.buttons = 0;
        systems::ApplyControllers(w);
        in.buttons = InputState::Jump;
        systems::ApplyControllers(w);
        Check(p.Get<RigidBody>()->velocity.y < 0.0f, "released and pressed again: jumps");

        GameObject &other = Body(w, 600, 452);
        other.Add<InputState>().buttons = InputState::Right;
        other.Add<PlatformerController>();
        other.Add<NetIdentity>(3);
        p.Add<NetIdentity>(1);
        systems::ApplyControllers(w, OwnedBy(1));
        Check(other.Get<RigidBody>()->velocity.x == 0.0f, "a filter limits a system to the objects this endpoint owns");
    }

    printf("\nTriggers\n");
    {
        World w;
        GameObject &plate = w.Create("plate");
        plate.Add<Transform>(100, 480, 80, 20);
        plate.Add<Collider>(Collider::Type::Trigger);
        GameObject &gem = w.Create("gem");
        gem.Add<Transform>(400, 440, 20, 20);
        gem.Add<Collider>(Collider::Type::Trigger);
        GameObject &p = Body(w, 110, 450, false);
        GameObject &q = Body(w, 900, 450, false);
        TriggerTracker tracker;
        auto events = tracker.Update(w);
        Check(events.size() == 1 && events[0].type == TriggerEvent::Type::Enter && events[0].trigger == plate.Id() && events[0].other == p.Id(),
              "a body arriving on a plate: one Enter, naming both");
        events = tracker.Update(w);
        Check(events.size() == 1 && events[0].type == TriggerEvent::Type::Stay, "still there next update: Stay");
        p.Get<Transform>()->position.x = 395.0f;   // off the plate, onto the gem
        events = tracker.Update(w);
        bool exitPlate = false, enterGem = false;
        for (auto &e : events) {
            exitPlate = exitPlate || (e.type == TriggerEvent::Type::Exit && e.trigger == plate.Id());
            enterGem = enterGem || (e.type == TriggerEvent::Type::Enter && e.trigger == gem.Id());
        }
        Check(events.size() == 2 && exitPlate && enterGem, "moving across: Exit the plate and Enter the gem in one update");
        w.Destroy(gem.Id());
        w.Flush();
        events = tracker.Update(w);
        Check(events.size() == 1 && events[0].type == TriggerEvent::Type::Exit, "a trigger destroyed (a gem picked up) reports Exit");
        q.Get<Transform>()->position.x = 110.0f;
        q.Add<NetIdentity>(2);
        p.Add<NetIdentity>(1);
        Check(systems::FindTriggerContacts(w, OwnedBy(1)).empty() && systems::FindTriggerContacts(w).size() == 1,
              "a filter limits which bodies count");
        GameObject &solidBox = w.Create("box");
        solidBox.Add<Transform>(110, 450, 40, 40);
        solidBox.Add<Collider>();
        Check(systems::FindTriggerContacts(w).size() == 1, "solid colliders are not triggers");
    }

    printf("\nFacing and the keyboard\n");
    {
        World w;
        GameObject &p = Body(w, 0, 0, false);
        p.Add<InputState>().buttons = InputState::Left;
        p.Add<PlatformerController>();
        p.Add<Renderable>();
        systems::ApplyControllers(w);
        Check(p.Get<PlatformerController>()->facing == -1 && p.Get<Renderable>()->flipX, "running left: facing -1 and the sprite is mirrored");
        p.Get<InputState>()->buttons = 0;
        systems::ApplyControllers(w);
        Check(p.Get<Renderable>()->flipX, "standing still keeps the last facing");
        p.Get<InputState>()->buttons = InputState::Right;
        systems::ApplyControllers(w);
        Check(!p.Get<Renderable>()->flipX && p.Get<PlatformerController>()->facing == 1, "running right turns it back");
        p.Get<PlatformerController>()->flipSprite = false;
        p.Get<InputState>()->buttons = InputState::Left;
        systems::ApplyControllers(w);
        Check(!p.Get<Renderable>()->flipX, "flipSprite off: the game controls flipX itself");

        InputState in;
        systems::ReadKeyboard(in, DefaultKeyBindings(), [](SDL_Scancode k) { return k == SDL_SCANCODE_D || k == SDL_SCANCODE_SPACE; });
        Check(in.buttons == (InputState::Right | InputState::Jump), "D + Space: Right and Jump");
        systems::ReadKeyboard(in, DefaultKeyBindings(), [](SDL_Scancode k) { return k == SDL_SCANCODE_UP; });
        Check(in.buttons == (InputState::Up | InputState::Jump), "one key can press two buttons (Up is Up and Jump)");
        systems::ReadKeyboard(in, {{SDL_SCANCODE_J, InputState::Left}}, [](SDL_Scancode k) { return k == SDL_SCANCODE_J; });
        Check(in.buttons == InputState::Left, "custom bindings: a second player on J");
        systems::ReadKeyboard(in, DefaultKeyBindings(), [](SDL_Scancode) { return false; });
        Check(in.buttons == 0, "nothing held: no buttons");
    }

    printf("\nSpawn points and death zones\n");
    {
        World w;
        GameObject &s0 = w.Create("spawn");
        s0.Add<Transform>(100, 400, 40, 100);
        s0.Add<SpawnPoint>(0);
        GameObject &s2 = w.Create("spawn");
        s2.Add<Transform>(900, 300, 40, 100);
        s2.Add<SpawnPoint>(2);
        GameObject &pit = w.Create("pit");
        pit.Add<Transform>(0, 1000, 5000, 200);
        pit.Add<Collider>(Collider::Type::Trigger);
        pit.Add<DeathZone>();
        Check(!pit.Has<Renderable>() && !s0.Has<Renderable>(), "spawn points and death zones carry no Renderable: hidden");

        GameObject &p = Body(w, 3000, 1050);
        p.Add<Respawn>().spawnIndex = 2;
        p.Get<RigidBody>()->velocity = {200.0f, 500.0f};
        auto events = systems::ApplyDeathZones(w);
        const Transform &t = *p.Get<Transform>();
        Check(events.size() == 1 && events[0].spawn == s2.Id() && events[0].zone == pit.Id(), "entering the zone is reported once, with zone and spawn");
        Check(Near(t.Center().x, 920.0f) && Near(t.Bottom(), 400.0f), "the body stands on its designated spawn point", t.Center().x, t.Bottom());
        Check(p.Get<RigidBody>()->velocity.x == 0.0f && p.Get<RigidBody>()->velocity.y == 0.0f && p.Get<Respawn>()->deaths == 1,
              "stopped, and the death counted");
        Check(systems::ApplyDeathZones(w).empty(), "outside the zone: nothing happens");

        p.Get<Respawn>()->spawnIndex = 7;
        p.Get<Transform>()->position = {10, 1100};
        systems::ApplyDeathZones(w);
        Check(Near(t.Center().x, 120.0f), "no spawn point with that index: the lowest-indexed one", t.Center().x, 120.0f);

        GameObject &noRespawn = Body(w, 10, 1100, false);
        systems::ApplyDeathZones(w);
        Check(Near(noRespawn.Get<Transform>()->position.y, 1100.0f), "only objects with Respawn are affected");
    }

    printf("\nSide-scrolling boundaries\n");
    {
        World w;
        GameObject &camObj = w.Create("camera");
        Camera &cam = camObj.Add<Camera>();
        cam.view = {1000.0f, 600.0f};
        cam.boundsMin = {0.0f, 0.0f};
        cam.boundsMax = {5000.0f, 600.0f};
        GameObject &p = Body(w, 450, 300, false);
        cam.target = p.Id();
        Check(systems::AddScrollBoundaries(w, camObj.Id(), 300.0f) == 2, "two hidden side boundaries are made for the camera");
        GameObject *right = w.FindByName("scroll-right");
        Check(right && !right->Has<Renderable>() && Near(right->Get<Transform>()->Left(), 700.0f),
              "the right boundary sits `inset` in from the view's right edge", right ? right->Get<Transform>()->Left() : -1, 700.0f);

        p.Get<Transform>()->position.x = 700.0f;   // walks 32px into the right boundary
        systems::UpdateCameras(w);
        Check(Near(cam.position.x, 32.0f), "touching it translates the scene by exactly the overlap", cam.position.x, 32.0f);
        Check(Near(right->Get<Transform>()->Left(), 732.0f) && !p.Get<Transform>()->Overlaps(*right->Get<Transform>()),
              "the boundary moves with the camera and the player is no longer inside it");

        p.Get<Transform>()->position.x = 2000.0f;
        systems::UpdateCameras(w);
        Check(Near(cam.position.x, 1332.0f), "after a long walk the camera still trails by the inset", cam.position.x, 1332.0f);

        p.Get<Transform>()->position.x = 1500.0f;  // walks back left, into the left boundary (at 1632)
        systems::UpdateCameras(w);
        Check(Near(cam.position.x, 1200.0f), "the left boundary scrolls the other way", cam.position.x, 1200.0f);

        p.Get<Transform>()->position.x = 10.0f;
        systems::UpdateCameras(w);
        Check(Near(cam.position.x, 0.0f), "the camera never shows outside its bounds", cam.position.x, 0.0f);
    }

    printf("\nRendering (software renderer)\n");
    {
        SDL_Surface *surface = SDL_CreateSurface(200, 100, SDL_PIXELFORMAT_RGBA32);
        SDL_Renderer *r = SDL_CreateSoftwareRenderer(surface);
        World w;
        GameObject &red = w.Create("box");
        red.Add<Transform>(10, 10, 40, 40);
        red.Add<Renderable>(SDL_Color{255, 0, 0, 255}, 0);
        GameObject &green = w.Create("box");
        green.Add<Transform>(30, 10, 40, 40);
        green.Add<Renderable>(SDL_Color{0, 255, 0, 255}, 5);   // higher layer, drawn on top
        GameObject &hidden = w.Create("box");
        hidden.Add<Transform>(120, 10, 40, 40);
        hidden.Add<Renderable>(SDL_Color{0, 0, 255, 255}).visible = false;
        GameObject &zone = w.Create("zone");
        zone.Add<Transform>(120, 60, 40, 30);
        zone.Add<DeathZone>();

        SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
        SDL_RenderClear(r);
        Camera cam;
        cam.view = {200.0f, 100.0f};
        systems::Render(w, r, cam, 1.0f, 0.0);
        SDL_RenderPresent(r);
        auto px = [&](int x, int y) {
            Uint8 rr, gg, bb, aa;
            SDL_ReadSurfacePixel(surface, x, y, &rr, &gg, &bb, &aa);
            return (rr << 16) | (gg << 8) | bb;
        };
        Check(px(15, 20) == 0xff0000, "a visible object is drawn in its colour");
        Check(px(40, 20) == 0x00ff00, "a higher layer is drawn over a lower one");
        Check(px(130, 20) == 0 && px(130, 70) == 0, "a hidden Renderable and an object without one draw nothing");

        cam.position = {20.0f, 0.0f};
        SDL_RenderClear(r);
        systems::Render(w, r, cam, 1.0f, 0.0);
        SDL_RenderPresent(r);
        Check(px(15, 20) == 0x00ff00 && px(2, 20) == 0xff0000, "the camera translates the whole scene on screen");

        // A sprite whose left half is red and right half blue, made here and
        // handed to the cache by name.
        SDL_Surface *art = SDL_CreateSurface(8, 4, SDL_PIXELFORMAT_RGBA32);
        SDL_FillSurfaceRect(art, nullptr, SDL_MapSurfaceRGBA(art, 0, 0, 255, 255));
        SDL_Rect leftHalf = {0, 0, 4, 4};
        SDL_FillSurfaceRect(art, &leftHalf, SDL_MapSurfaceRGBA(art, 255, 0, 0, 255));
        TextureCache textures;
        textures.Put("half.png", SDL_CreateTextureFromSurface(r, art));
        World sw;
        GameObject &sprite = sw.Create("sprite");
        sprite.Add<Transform>(0, 0, 80, 40);
        Renderable &sr = sprite.Add<Renderable>();
        sr.sprite = "half.png";
        Camera c2;
        c2.view = {200.0f, 100.0f};
        SDL_RenderClear(r);
        systems::Render(sw, r, c2, 1.0f, 0.0, &textures);
        SDL_RenderPresent(r);
        const bool normal = px(10, 20) == 0xff0000 && px(70, 20) == 0x0000ff;
        sr.flipX = true;
        SDL_RenderClear(r);
        systems::Render(sw, r, c2, 1.0f, 0.0, &textures);
        SDL_RenderPresent(r);
        Check(normal && px(10, 20) == 0x0000ff && px(70, 20) == 0xff0000, "flipX mirrors a sprite left to right");
        SDL_DestroySurface(art);
        SDL_DestroyRenderer(r);
        SDL_DestroySurface(surface);
    }

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASS", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
