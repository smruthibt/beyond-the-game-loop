#include "engine/Systems.hpp"

#include "engine/Input.hpp"

#include <SDL3_image/SDL_image.h>

#include <algorithm>
#include <cmath>
#include <tuple>

namespace engine {

int OwnerOf(const GameObject &object) {
    const NetIdentity *net = object.Get<NetIdentity>();
    return net ? net->owner : -1;
}

ObjectFilter OwnedBy(int owner) {
    return [owner](const GameObject &object) { return OwnerOf(object) == owner; };
}

namespace systems {

namespace {

bool Accept(const ObjectFilter &filter, const GameObject &object) {
    return !filter || filter(object);
}

struct Solid {
    ObjectId id;
    const Transform *box;
    bool oneWay;
};

// How far below a surface's top a falling body may already be and still land
// on it, rather than being treated as inside it. Covers float error and a
// platform that rose a little between steps.
constexpr float LAND_TOLERANCE = 2.0f;

} // namespace

void MovePaths(World &world, double seconds) {
    world.Each<Transform, PathMover>([seconds](GameObject &, Transform &t, PathMover &p) {
        t.position = p.PositionAt(seconds);
    });
}

void ApplyControllers(World &world, const ObjectFilter &filter) {
    world.Each<InputState, PlatformerController, RigidBody>(
        [&filter](GameObject &object, InputState &in, PlatformerController &pc, RigidBody &body) {
            if (!Accept(filter, object)) return;
            float dir = 0.0f;
            if (in.Held(InputState::Left)) dir -= 1.0f;
            if (in.Held(InputState::Right)) dir += 1.0f;
            body.velocity.x = dir * pc.moveSpeed;
            if (dir != 0.0f) pc.facing = dir > 0.0f ? 1 : -1;
            if (pc.flipSprite) {
                if (Renderable *r = object.Get<Renderable>()) r->flipX = pc.facing < 0;
            }

            const bool jump = in.Held(InputState::Jump);
            if (jump && !pc.jumpLatched && body.grounded) {
                body.velocity.y = -pc.jumpSpeed;
                body.grounded = false;
                body.ground = NO_OBJECT;
            }
            pc.jumpLatched = jump;
        });
}

void StepPhysics(World &world, float dt, const PhysicsSettings &settings, const ObjectFilter &filter) {
    if (dt <= 0.0f) return;

    std::vector<Solid> solids;
    world.Each<Transform, Collider>([&solids](GameObject &object, Transform &t, Collider &c) {
        if (c.type == Collider::Type::Solid) solids.push_back(Solid{object.Id(), &t, c.oneWay});
    });

    const int steps = std::max(1, static_cast<int>(std::ceil(dt / settings.maxStep)));
    const float h = dt / static_cast<float>(steps);

    world.Each<Transform, RigidBody>([&](GameObject &object, Transform &t, RigidBody &body) {
        if (!Accept(filter, object)) return;

        // Ride whatever we stood on last step, however it moved since.
        if (body.grounded && body.ground != NO_OBJECT) {
            if (const GameObject *g = world.Find(body.ground)) {
                if (const Transform *gt = g->Get<Transform>()) {
                    t.position.x += gt->position.x - body.groundLast.x;
                    t.position.y += gt->position.y - body.groundLast.y;
                }
            }
        }

        bool grounded = false;
        ObjectId ground = NO_OBJECT;
        for (int i = 0; i < steps; ++i) {
            if (body.gravity) {
                body.velocity.y = std::min(body.velocity.y + settings.gravity * h, settings.maxFallSpeed);
            }

            // X: walls. One-way ledges never block sideways.
            t.position.x += body.velocity.x * h;
            for (const Solid &s : solids) {
                if (s.id == object.Id() || s.oneWay || !t.Overlaps(*s.box)) continue;
                const float pushLeft = t.Right() - s.box->Left();
                const float pushRight = s.box->Right() - t.Left();
                if (body.velocity.x > 0.0f || (body.velocity.x == 0.0f && pushLeft < pushRight)) {
                    t.position.x -= pushLeft;
                } else {
                    t.position.x += pushRight;
                }
                body.velocity.x = 0.0f;
            }

            // Y: floors, ceilings, ledges.
            const float bottomBefore = t.Bottom();
            t.position.y += body.velocity.y * h;
            for (const Solid &s : solids) {
                if (s.id == object.Id() || !t.Overlaps(*s.box)) continue;
                const bool fromAbove = body.velocity.y >= 0.0f && bottomBefore <= s.box->Top() + LAND_TOLERANCE;
                if (fromAbove) {
                    t.position.y = s.box->Top() - t.size.y;
                    body.velocity.y = 0.0f;
                    grounded = true;
                    ground = s.id;
                } else if (!s.oneWay) {
                    if (body.velocity.y < 0.0f) {
                        t.position.y = s.box->Bottom();   // head on a ceiling
                    } else {
                        // Inside it without having come down onto it - something
                        // moved into us. Out by the shallower way.
                        const float up = t.Bottom() - s.box->Top();
                        const float down = s.box->Bottom() - t.Top();
                        t.position.y += up < down ? -up : down;
                    }
                    body.velocity.y = 0.0f;
                }
            }
        }

        body.grounded = grounded;
        body.ground = ground;
        if (grounded) {
            if (const GameObject *g = world.Find(ground)) {
                if (const Transform *gt = g->Get<Transform>()) body.groundLast = gt->position;
            }
        }
    });
}

GameObject *FindSpawn(World &world, int index) {
    GameObject *exact = nullptr;
    GameObject *lowest = nullptr;
    int lowestIndex = 0;
    world.Each<Transform, SpawnPoint>([&](GameObject &object, Transform &, SpawnPoint &sp) {
        if (sp.index == index && !exact) exact = &object;
        if (!lowest || sp.index < lowestIndex) {
            lowest = &object;
            lowestIndex = sp.index;
        }
    });
    return exact ? exact : lowest;
}

bool PlaceAtSpawn(World &world, GameObject &object, int index) {
    GameObject *spawn = FindSpawn(world, index);
    Transform *t = object.Get<Transform>();
    if (!spawn || !t) return false;
    const Transform &sp = *spawn->Get<Transform>();
    t->position = {sp.Center().x - t->size.x * 0.5f, sp.Bottom() - t->size.y};
    if (RigidBody *body = object.Get<RigidBody>()) {
        body->velocity = {0.0f, 0.0f};
        body->grounded = false;
        body->ground = NO_OBJECT;
    }
    return true;
}

std::vector<RespawnEvent> ApplyDeathZones(World &world, const ObjectFilter &filter) {
    std::vector<std::pair<ObjectId, const Transform *>> zones;
    world.Each<Transform, DeathZone>([&zones](GameObject &object, Transform &t, DeathZone &) {
        zones.emplace_back(object.Id(), &t);
    });

    std::vector<RespawnEvent> events;
    if (zones.empty()) return events;
    world.Each<Transform, Respawn>([&](GameObject &object, Transform &t, Respawn &r) {
        if (!Accept(filter, object)) return;
        for (const auto &zone : zones) {
            if (!t.Overlaps(*zone.second)) continue;
            GameObject *spawn = FindSpawn(world, r.spawnIndex);
            if (!spawn) return;
            PlaceAtSpawn(world, object, r.spawnIndex);
            ++r.deaths;
            events.push_back(RespawnEvent{object.Id(), zone.first, spawn->Id()});
            return;
        }
    });
    return events;
}

std::vector<TriggerContact> FindTriggerContacts(World &world, const ObjectFilter &filter) {
    std::vector<std::pair<ObjectId, const Transform *>> triggers;
    world.Each<Transform, Collider>([&triggers](GameObject &object, Transform &t, Collider &c) {
        if (c.type == Collider::Type::Trigger) triggers.emplace_back(object.Id(), &t);
    });
    std::vector<TriggerContact> contacts;
    if (triggers.empty()) return contacts;
    world.Each<Transform, RigidBody>([&](GameObject &object, Transform &t, RigidBody &) {
        if (!Accept(filter, object)) return;
        for (const auto &trigger : triggers) {
            if (trigger.first != object.Id() && t.Overlaps(*trigger.second)) {
                contacts.push_back(TriggerContact{trigger.first, object.Id()});
            }
        }
    });
    return contacts;
}

void ReadKeyboard(InputState &input, const std::vector<KeyBinding> &bindings,
                  const std::function<bool(SDL_Scancode)> &isDown) {
    uint32_t buttons = 0;
    for (const KeyBinding &b : bindings) {
        if (isDown ? isDown(b.key) : Input::IsKeyPressed(b.key)) buttons |= b.button;
    }
    input.buttons = buttons;
}

namespace {

void PlaceBoundary(const Camera &cam, const ScrollBoundary &b, Transform &t) {
    switch (b.side) {
    case ScrollBoundary::Side::Left:
        t.position = {cam.position.x + b.inset - b.thickness, cam.position.y};
        t.size = {b.thickness, cam.view.y};
        break;
    case ScrollBoundary::Side::Right:
        t.position = {cam.position.x + cam.view.x - b.inset, cam.position.y};
        t.size = {b.thickness, cam.view.y};
        break;
    case ScrollBoundary::Side::Top:
        t.position = {cam.position.x, cam.position.y + b.inset - b.thickness};
        t.size = {cam.view.x, b.thickness};
        break;
    case ScrollBoundary::Side::Bottom:
        t.position = {cam.position.x, cam.position.y + cam.view.y - b.inset};
        t.size = {cam.view.x, b.thickness};
        break;
    }
}

void Clamp(Camera &cam) {
    if (cam.boundsMax.x > cam.boundsMin.x) {
        const float maxX = std::max(cam.boundsMin.x, cam.boundsMax.x - cam.view.x);
        cam.position.x = std::clamp(cam.position.x, cam.boundsMin.x, maxX);
    }
    if (cam.boundsMax.y > cam.boundsMin.y) {
        const float maxY = std::max(cam.boundsMin.y, cam.boundsMax.y - cam.view.y);
        cam.position.y = std::clamp(cam.position.y, cam.boundsMin.y, maxY);
    }
}

} // namespace

void UpdateCameras(World &world) {
    world.Each<Camera>([&world](GameObject &camObject, Camera &cam) {
        std::vector<std::pair<ScrollBoundary *, Transform *>> bounds;
        world.Each<ScrollBoundary, Transform>([&](GameObject &, ScrollBoundary &b, Transform &t) {
            if (b.camera == camObject.Id()) bounds.emplace_back(&b, &t);
        });
        for (auto &b : bounds) PlaceBoundary(cam, *b.first, *b.second);

        const GameObject *target = world.Find(cam.target);
        const Transform *tt = target ? target->Get<Transform>() : nullptr;
        if (tt) {
            // A boundary the target has touched - or, after a teleport, passed
            // - moves the camera by exactly the overlap.
            for (auto &b : bounds) {
                const Transform &edge = *b.second;
                switch (b.first->side) {
                case ScrollBoundary::Side::Left:
                    if (tt->Left() < edge.Right()) cam.position.x -= edge.Right() - tt->Left();
                    break;
                case ScrollBoundary::Side::Right:
                    if (tt->Right() > edge.Left()) cam.position.x += tt->Right() - edge.Left();
                    break;
                case ScrollBoundary::Side::Top:
                    if (tt->Top() < edge.Bottom()) cam.position.y -= edge.Bottom() - tt->Top();
                    break;
                case ScrollBoundary::Side::Bottom:
                    if (tt->Bottom() > edge.Top()) cam.position.y += tt->Bottom() - edge.Top();
                    break;
                }
            }
        }
        Clamp(cam);
        for (auto &b : bounds) PlaceBoundary(cam, *b.first, *b.second);
    });
}

int AddScrollBoundaries(World &world, ObjectId camera, float insetX, float insetY) {
    int made = 0;
    auto add = [&](ScrollBoundary::Side side, float inset, const char *name) {
        GameObject &o = world.Create("scroll-boundary", name);
        o.Add<Transform>();
        o.Add<ScrollBoundary>(camera, side, inset);
        ++made;
    };
    add(ScrollBoundary::Side::Left, insetX, "scroll-left");
    add(ScrollBoundary::Side::Right, insetX, "scroll-right");
    if (insetY > 0.0f) {
        add(ScrollBoundary::Side::Top, insetY, "scroll-top");
        add(ScrollBoundary::Side::Bottom, insetY, "scroll-bottom");
    }
    UpdateCameras(world);
    return made;
}

} // namespace systems

std::vector<TriggerEvent> TriggerTracker::Update(World &world, const ObjectFilter &filter) {
    std::vector<std::pair<ObjectId, ObjectId>> now;
    for (const TriggerContact &c : systems::FindTriggerContacts(world, filter)) now.emplace_back(c.trigger, c.other);
    std::sort(now.begin(), now.end());

    std::vector<TriggerEvent> events;
    size_t i = 0, j = 0;
    while (i < inside.size() || j < now.size()) {
        if (j == now.size() || (i < inside.size() && inside[i] < now[j])) {
            events.push_back(TriggerEvent{TriggerEvent::Type::Exit, inside[i].first, inside[i].second});
            ++i;
        } else if (i == inside.size() || now[j] < inside[i]) {
            events.push_back(TriggerEvent{TriggerEvent::Type::Enter, now[j].first, now[j].second});
            ++j;
        } else {
            events.push_back(TriggerEvent{TriggerEvent::Type::Stay, now[j].first, now[j].second});
            ++i;
            ++j;
        }
    }
    inside.swap(now);
    return events;
}

std::vector<KeyBinding> DefaultKeyBindings() {
    return {
        {SDL_SCANCODE_A, InputState::Left},   {SDL_SCANCODE_LEFT, InputState::Left},
        {SDL_SCANCODE_D, InputState::Right},  {SDL_SCANCODE_RIGHT, InputState::Right},
        {SDL_SCANCODE_W, InputState::Up},     {SDL_SCANCODE_UP, InputState::Up},
        {SDL_SCANCODE_S, InputState::Down},   {SDL_SCANCODE_DOWN, InputState::Down},
        {SDL_SCANCODE_SPACE, InputState::Jump}, {SDL_SCANCODE_W, InputState::Jump}, {SDL_SCANCODE_UP, InputState::Jump},
        {SDL_SCANCODE_E, InputState::Action}, {SDL_SCANCODE_RETURN, InputState::Action},
    };
}

SDL_Texture *TextureCache::Get(SDL_Renderer *renderer, const std::string &name) {
    if (name.empty() || !renderer) return nullptr;
    auto it = cache.find(name);
    if (it != cache.end()) return it->second;

    std::string dir = root;
    if (dir.empty()) {
        const char *base = SDL_GetBasePath();
        dir = std::string(base ? base : "") + "../media";
    }
    SDL_Texture *texture = IMG_LoadTexture(renderer, (dir + "/" + name).c_str());
    if (!texture) SDL_Log("[assets] %s/%s not found - drawing a flat colour", dir.c_str(), name.c_str());
    cache.emplace(name, texture);
    return texture;
}

namespace systems {

void Render(const World &world, SDL_Renderer *renderer, const Camera &camera, float scale,
            double seconds, TextureCache *textures) {
    std::vector<std::tuple<int, ObjectId, const Transform *, const Renderable *>> visible;
    world.Each<Transform, Renderable>([&](const GameObject &object, const Transform &t, const Renderable &r) {
        if (!r.visible) return;
        if (t.Right() < camera.position.x || t.Left() > camera.position.x + camera.view.x ||
            t.Bottom() < camera.position.y || t.Top() > camera.position.y + camera.view.y) return;
        visible.emplace_back(r.layer, object.Id(), &t, &r);
    });
    std::sort(visible.begin(), visible.end(), [](const auto &a, const auto &b) {
        return std::get<0>(a) != std::get<0>(b) ? std::get<0>(a) < std::get<0>(b) : std::get<1>(a) < std::get<1>(b);
    });

    for (const auto &item : visible) {
        const Transform &t = *std::get<2>(item);
        const Renderable &r = *std::get<3>(item);
        const SDL_FRect dst = {(t.position.x - camera.position.x) * scale, (t.position.y - camera.position.y) * scale,
                               t.size.x * scale, t.size.y * scale};
        SDL_Texture *texture = textures ? textures->Get(renderer, r.sprite) : nullptr;
        if (texture) {
            float tw = 0.0f, th = 0.0f;
            SDL_GetTextureSize(texture, &tw, &th);
            const float fw = r.frameWidth > 0 ? static_cast<float>(r.frameWidth) : tw / static_cast<float>(std::max(1, r.frames));
            const float fh = r.frameHeight > 0 ? static_cast<float>(r.frameHeight) : th;
            int frame = 0;
            if (r.frames > 1 && r.frameMs > 0) {
                frame = static_cast<int>(seconds * 1000.0 / r.frameMs) % r.frames;
                if (frame < 0) frame = 0;
            }
            const SDL_FRect src = {fw * static_cast<float>(frame), 0.0f, fw, fh};
            SDL_SetTextureColorMod(texture, r.color.r, r.color.g, r.color.b);
            SDL_SetTextureAlphaMod(texture, r.color.a);
            if (r.flipX) SDL_RenderTextureRotated(renderer, texture, &src, &dst, 0.0, nullptr, SDL_FLIP_HORIZONTAL);
            else SDL_RenderTexture(renderer, texture, &src, &dst);
        } else {
            SDL_SetRenderDrawColor(renderer, r.color.r, r.color.g, r.color.b, r.color.a);
            SDL_RenderFillRect(renderer, &dst);
        }
    }
}

} // namespace systems

} // namespace engine
