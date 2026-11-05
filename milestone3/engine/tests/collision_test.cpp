// Collision::Resolve - the engine's one piece of collision response.
#include "engine/Collision.hpp"
#include "engine/Entity.hpp"

#include <cmath>
#include <cstdio>

using namespace engine;

static int failures = 0;
static void Check(bool ok, const char *name, float a = 0, float b = 0) {
    if (ok) printf("  PASS  %s\n", name);
    else    printf("  FAIL  %s   (%.2f, %.2f)\n", name, a, b), ++failures;
}

static Entity Box(float x, float y, float w, float h) {
    Entity e; e.position = {x, y}; e.size = {w, h}; return e;
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("Collision::Resolve verification\n\n");

    // Not touching -> no push.
    { Vec2 m = Collision::Resolve(Box(0,0,40,40), Box(100,100,40,40));
      Check(m.x == 0 && m.y == 0, "separated boxes: zero vector", m.x, m.y); }

    // Mover overlapping a wall's left face by 6px -> pushed left 6.
    { Vec2 m = Collision::Resolve(Box(66,0,40,40), Box(100,-100,20,300));
      Check(std::fabs(m.x + 6) < 0.01f && m.y == 0, "shallow X penetration pushes out on X", m.x, m.y); }

    // Mover overlapping a floor's top by 4px -> pushed up 4.
    { Vec2 m = Collision::Resolve(Box(0,64,40,40), Box(-100,100,300,20));
      Check(m.x == 0 && std::fabs(m.y + 4) < 0.01f, "shallow Y penetration pushes out on Y", m.x, m.y); }

    // Mover approaching from the right side of a wall -> pushed right.
    { Vec2 m = Collision::Resolve(Box(114,0,40,40), Box(100,-100,20,300));
      Check(m.x > 0 && m.y == 0, "push direction follows which side the mover is on", m.x, m.y); }

    // Corner: 30px deep on X, 10px deep on Y -> only Y is corrected (the slide).
    { Vec2 m = Collision::Resolve(Box(90,70,40,40), Box(100,100,200,200));
      Check(m.x == 0 && std::fabs(m.y + 10) < 0.01f, "corner: only the shallow axis is corrected", m.x, m.y); }

    // Applying the vector leaves the mover flush against the wall, and a
    // second Resolve asks for nothing more - the response is stable, so a box
    // held against a wall does not jitter. (SDL counts exactly-touching edges
    // as intersecting, so CheckCollision is not the right test here.)
    { Entity mover = Box(66,0,40,40); Entity wall = Box(100,-100,20,300);
      Vec2 m = Collision::Resolve(mover, wall);
      mover.position.x += m.x; mover.position.y += m.y;
      const float gap = wall.position.x - (mover.position.x + mover.size.x);
      Vec2 again = Collision::Resolve(mover, wall);
      Check(std::fabs(gap) < 0.01f && again.x == 0 && again.y == 0,
            "after the push: flush against the wall, second Resolve is zero", gap, again.x); }

    // A box that slid into a wall while moving diagonally keeps its motion
    // along the wall: only the into-wall component is removed.
    { Entity mover = Box(66,50,40,40); Entity wall = Box(100,-100,20,300);
      mover.velocity = {200.0f, 150.0f};
      Vec2 m = Collision::Resolve(mover, wall);
      if (m.x != 0) mover.velocity.x = 0;   // the game's response: kill into-wall velocity
      if (m.y != 0) mover.velocity.y = 0;
      Check(mover.velocity.x == 0 && mover.velocity.y == 150.0f,
            "slide: into-wall velocity zeroed, along-wall velocity kept", mover.velocity.x, mover.velocity.y); }

    // Inactive entities never push.
    { Entity a = Box(66,0,40,40); Entity b = Box(100,-100,20,300); b.active = false;
      Vec2 m = Collision::Resolve(a, b);
      Check(m.x == 0 && m.y == 0, "inactive solid is ignored", m.x, m.y); }

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASSED",
           failures, failures == 1 ? "" : "s");
    return failures != 0;
}
