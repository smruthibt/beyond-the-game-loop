// Milestone 3 Part 1A: the game object model - GameObject, components, World.
#include "engine/Components.hpp"
#include "engine/World.hpp"

#include <cstdio>
#include <string>
#include <vector>

using namespace engine;

static int failures = 0;
static void Check(bool ok, const char *name) {
    if (ok) printf("  PASS  %s\n", name);
    else    printf("  FAIL  %s\n", name), ++failures;
}

// A component the engine has never heard of, the way a game would add one.
struct Health : Component {
    ENGINE_COMPONENT(Health)
    int hp = 3;
    explicit Health(int h = 3) : hp(h) {}
};
struct Tagged : Component {};   // no network name: local only

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    printf("Game object model verification\n\n");

    printf("GameObject\n");
    {
        World world;
        GameObject &a = world.Create("player", "alice");
        GameObject &b = world.Create("platform");
        Check(a.Id() == 1 && b.Id() == 2, "ids count up from 1");
        Check(a.Kind() == "player" && a.Name() == "alice", "kind and name are kept");

        a.Add<Transform>(10.0f, 20.0f, 30.0f, 40.0f);
        Check(a.Has<Transform>() && !a.Has<RigidBody>(), "Has() answers for what was added and only that");
        Check(a.Get<Transform>()->position.x == 10.0f && a.Get<Transform>()->size.y == 40.0f, "Get() returns the component with its values");
        Check(a.Get<RigidBody>() == nullptr, "Get() of a missing component is null");

        a.Add<Transform>(1.0f, 2.0f, 3.0f, 4.0f);
        Check(a.Components().size() == 1 && a.Get<Transform>()->position.x == 1.0f, "adding a type again replaces it, never duplicates");
        Check(&a.Get<Transform>()->Owner() == &a, "a component knows its owner");

        a.Add<Health>(7);
        Check(a.Get<Health>() && a.Get<Health>()->hp == 7, "a game-defined component works like a built-in one");
        Check(a.Remove<Health>() && !a.Has<Health>() && !a.Remove<Health>(), "Remove() takes it off once");
        Check(std::string(Transform::TYPE) == "Transform" && Tagged().TypeName() == nullptr, "network names: declared ones exist, undeclared is null");
    }

    printf("\nWorld queries\n");
    {
        World world;
        for (int i = 0; i < 6; ++i) {
            GameObject &o = world.Create(i % 2 ? "odd" : "even");
            o.Add<Transform>();
            if (i % 2) o.Add<RigidBody>();
            if (i % 3 == 0) o.Add<Collider>();
        }
        std::vector<ObjectId> both;
        world.Each<Transform, RigidBody>([&](GameObject &o, Transform &, RigidBody &) { both.push_back(o.Id()); });
        Check(both == std::vector<ObjectId>({2, 4, 6}), "Each<A,B> visits exactly the objects holding both, in id order");
        int n = 0;
        world.Each<RigidBody, Collider>([&](GameObject &, RigidBody &, Collider &) { ++n; });
        Check(n == 1, "Each with a rarer combination finds only that one (id 4)");
        int all = 0;
        world.Each<Transform>([&](GameObject &, Transform &t) { t.position.x = 5.0f; ++all; });
        bool written = true;
        world.Each<Transform>([&](GameObject &, Transform &t) { written = written && t.position.x == 5.0f; });
        Check(all == 6 && written, "components are visited by reference: a system's writes stick");
    }

    printf("\nDestroy is deferred to Flush\n");
    {
        World world;
        GameObject &a = world.Create("a");
        a.Add<Transform>();
        GameObject &b = world.Create("b");
        b.Add<Transform>();
        world.Destroy(a.Id());
        Check(world.Find(a.Id()) == nullptr, "a destroyed object is no longer found");
        Check(world.Count() == 1, "and no longer counted");
        int visited = 0;
        world.Each<Transform>([&](GameObject &, Transform &) { ++visited; });
        Check(visited == 1, "and no longer visited by queries");
        Check(a.IsDestroyed() && a.Has<Transform>(), "but a reference taken earlier is still valid until Flush");
        Check(world.Flush() == 1 && world.Flush() == 0, "Flush deletes it once");

        // Destroying from inside a query.
        for (int i = 0; i < 5; ++i) world.Create("x").Add<Transform>();
        world.Each<Transform>([&](GameObject &o, Transform &) { if (o.Kind() == "x") world.Destroy(o.Id()); });
        Check(world.Count() == 1 && world.Flush() == 5, "objects may destroy each other mid-query; Flush removes them after");
        Check(world.DestroyIf([](const GameObject &o) { return o.Kind() == "b"; }) == 1 && world.Count() == 0, "DestroyIf by predicate");
    }

    printf("\nIds from elsewhere\n");
    {
        World server;
        World client(1u << 30);
        GameObject &local = client.Create("camera");
        Check(local.Id() == (1u << 30), "a World can count from its own base (client-local objects)");
        GameObject &copy = client.CreateWithId(7, "player");
        Check(copy.Id() == 7 && client.Find(7) == &copy, "CreateWithId uses the id it is given");
        Check(&client.CreateWithId(7, "player") == &copy, "and returns the existing object if it is already there");

        World w;
        w.CreateWithId(1, "taken");
        GameObject &next = w.Create();
        Check(next.Id() == 2, "Create skips an id CreateWithId already used");

        w.Find(1)->Add<Health>(1);
        w.Destroy(1);
        GameObject &revived = w.CreateWithId(1, "again");
        w.Flush();
        Check(w.Find(1) == &revived && !revived.Has<Health>() && revived.Kind() == "again",
              "an object destroyed then re-created before Flush survives, emptied, with its new kind");
        (void)server;
    }

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASS", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
