// Milestone 3 Parts 1B and 1C: serialization and snapshots - text and binary,
// full and delta, inputs only, ownership rules, and bad input.
#include "engine/Snapshot.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

using namespace engine;

static int failures = 0;
static void Check(bool ok, const char *name, double a = 0, double b = 0) {
    if (ok) printf("  PASS  %s\n", name);
    else    printf("  FAIL  %s   (%g, %g)\n", name, a, b), ++failures;
}

struct Health : Component {
    ENGINE_COMPONENT(Health)
    int hp = 3;
    std::string label;
    void Write(Writer &w) const override { w.I32(hp); w.Str(label); }
    void Read(Reader &r) override { hp = r.I32(); label = r.Str(); }
};

// Every travelling component of an object, encoded in binary in name order:
// two objects with equal fingerprints hold bit-identical state.
static std::string Fingerprint(const GameObject &o) {
    std::vector<std::pair<std::string, std::string>> parts;
    for (const GameObject::Slot &slot : o.Components()) {
        if (!slot.component->TypeName()) continue;
        auto w = MakeWriter(Encoding::Binary);
        slot.component->Write(*w);
        parts.emplace_back(slot.component->TypeName(), w->Take());
    }
    std::sort(parts.begin(), parts.end());
    std::string out = o.Kind() + "|" + std::to_string(OwnerOf(o));
    for (auto &p : parts) out += "|" + p.first + ":" + p.second;
    return out;
}

static bool SameWorld(const World &a, const World &b) {
    if (a.Ids() != b.Ids()) return false;
    for (ObjectId id : a.Ids()) {
        if (Fingerprint(*a.Find(id)) != Fingerprint(*b.Find(id))) return false;
    }
    return true;
}

static void BuildScene(World &w, int moving, int statics) {
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> pos(0.0f, 4000.0f);
    for (int i = 0; i < statics; ++i) {
        GameObject &o = w.Create("platform");
        o.Add<Transform>(pos(rng), pos(rng), 120.0f + i, 24.0f);
        o.Add<Collider>();
        o.Add<Renderable>(SDL_Color{120, static_cast<Uint8>(i), 60, 255}, 1);
        o.Add<NetIdentity>();
    }
    for (int i = 0; i < moving; ++i) {
        GameObject &o = w.Create("mover");
        o.Add<Transform>(pos(rng), pos(rng), 64.0f, 16.0f);
        PathMover &p = o.Add<PathMover>();
        p.origin = o.Get<Transform>()->position;
        p.offset = {pos(rng) * 0.1f, 0.0f};
        p.period = 2.0f + static_cast<float>(i % 5);
        p.pattern = i % 2 ? PathMover::Pattern::Circular : PathMover::Pattern::Linear;
        p.radius = 40.0f;
        o.Add<Collider>();
        o.Add<Renderable>(SDL_Color{200, 200, 40, 255}, 1);
        o.Add<NetIdentity>();
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    ComponentRegistry::RegisterBuiltins();
    ComponentRegistry::Register<Health>();
    printf("Serialization and snapshot verification\n\n");

    printf("Writers and readers\n");
    for (Encoding enc : {Encoding::Text, Encoding::Binary}) {
        const char *name = EncodingName(enc);
        auto w = MakeWriter(enc);
        w->U8(250); w->U16(65000); w->U32(4000000000u); w->I32(-123456); w->I64(-9000000000000LL);
        w->F32(3.14159274f); w->Str("two words % and\tmore"); w->Str(""); w->Bool(true);
        const std::string data = w->Take();
        auto r = MakeReader(enc, data);
        const bool ints = r->U8() == 250 && r->U16() == 65000 && r->U32() == 4000000000u && r->I32() == -123456 && r->I64() == -9000000000000LL;
        const float f = r->F32();
        const std::string s1 = r->Str(), s2 = r->Str();
        const bool b = r->Bool();
        char label[96];
        snprintf(label, sizeof label, "%s: every field type reads back exactly, strings with spaces/%%/tabs and empty", name);
        Check(ints && f == 3.14159274f && s1 == "two words % and\tmore" && s2.empty() && b && r->Ok() && r->AtEnd(), label);
    }
    {
        std::mt19937 rng(7);
        std::uniform_int_distribution<uint32_t> bits;
        bool exact = true;
        for (int i = 0; i < 20000; ++i) {
            uint32_t u = bits(rng);
            float v;
            std::memcpy(&v, &u, 4);
            if (!std::isfinite(v)) continue;
            auto w = MakeWriter(Encoding::Text);
            w->F32(v);
            const std::string d = w->Take();
            auto r = MakeReader(Encoding::Text, d);
            const float back = r->F32();
            exact = exact && std::memcmp(&back, &v, 4) == 0;
        }
        Check(exact, "text floats are bit-identical after a round trip (20,000 random floats)");
    }
    {
        auto w = MakeWriter(Encoding::Binary);
        w->U32(5);
        const std::string d = w->Take().substr(0, 3);
        auto r = MakeReader(Encoding::Binary, d);
        r->U32();
        auto t = MakeReader(Encoding::Text, std::string("12 abc"));
        const std::string bad = "12 abc";
        auto t2 = MakeReader(Encoding::Text, bad);
        t2->U32();
        t2->U32();
        Check(!r->Ok() && !t2->Ok(), "short binary and non-numeric text fail cleanly instead of crashing");
        (void)t;
    }

    printf("\nComponent round trips\n");
    for (Encoding enc : {Encoding::Text, Encoding::Binary}) {
        World a;
        GameObject &o = a.Create("everything");
        o.Add<Transform>(1.5f, -2.25f, 33.0f, 44.0f);
        RigidBody &rb = o.Add<RigidBody>();
        rb.velocity = {12.5f, -900.0f}; rb.gravity = true; rb.grounded = true;
        o.Add<Collider>(Collider::Type::Trigger, true);
        Renderable &r = o.Add<Renderable>(SDL_Color{1, 2, 3, 4}, -3);
        r.sprite = "tank sheet.png"; r.frames = 4; r.frameWidth = 64; r.frameHeight = 64; r.frameMs = 70; r.visible = false;
        o.Add<InputState>().buttons = InputState::Left | InputState::Jump;
        PlatformerController &pc = o.Add<PlatformerController>();
        pc.moveSpeed = 111.0f; pc.jumpSpeed = 222.0f;
        PathMover &pm = o.Add<PathMover>();
        pm.pattern = PathMover::Pattern::Circular; pm.origin = {5, 6}; pm.offset = {7, 8}; pm.radius = 9; pm.period = 10; pm.phase = 0.25f;
        o.Add<SpawnPoint>(4);
        o.Add<DeathZone>();
        Respawn &rs = o.Add<Respawn>();
        rs.spawnIndex = 2; rs.deaths = 5;
        o.Add<Health>().label = "boss hp";
        NetIdentity &n = o.Add<NetIdentity>(3);
        n.stampMicros = 123456789;

        SnapshotEncoder enc1({enc});
        const std::string msg = enc1.Encode(a, 1);
        World b;
        SnapshotDecoder dec;
        ApplyResult res = dec.Apply(b, msg);
        char label[96];
        snprintf(label, sizeof label, "%s: all 11 built-ins and a game component arrive identical", EncodingName(enc));
        Check(res.ok && res.created == 1 && SameWorld(a, b), label);
        snprintf(label, sizeof label, "%s: owner and time stamp travel in the object header", EncodingName(enc));
        Check(OwnerOf(*b.Find(o.Id())) == 3 && b.Find(o.Id())->Get<NetIdentity>()->stampMicros == 123456789, label);
    }

    printf("\nWhole scenes: text vs binary\n");
    {
        World server;
        BuildScene(server, 100, 400);
        systems::MovePaths(server, 1.234);
        SnapshotEncoder text({Encoding::Text}), binary({Encoding::Binary});
        const std::string tm = text.Encode(server, 9), bm = binary.Encode(server, 9);
        World fromText, fromBinary;
        SnapshotDecoder d1, d2;
        ApplyResult r1 = d1.Apply(fromText, tm), r2 = d2.Apply(fromBinary, bm);
        Check(r1.ok && r2.ok && r1.tick == 9 && r1.created == 500, "a 500-object scene decodes in both encodings");
        Check(SameWorld(server, fromText) && SameWorld(server, fromBinary), "and both decode to exactly the server's world");
        Check(bm.size() < tm.size(), "binary is smaller than text", static_cast<double>(bm.size()), static_cast<double>(tm.size()));
        printf("        sizes: text %zu bytes, binary %zu bytes (%.2fx)\n", tm.size(), bm.size(),
               static_cast<double>(tm.size()) / static_cast<double>(bm.size()));
        Check(tm[0] == 'T' && bm[0] == 'B', "the first byte names the encoding, so receivers need no setting");

        World plain;
        plain.Create("local").Add<Transform>();
        SnapshotEncoder e({Encoding::Text});
        e.Encode(plain, 1);
        Check(e.LastStats().objects == 0, "objects without a NetIdentity never travel");
    }

    printf("\nDelta streams\n");
    for (Encoding enc : {Encoding::Text, Encoding::Binary}) {
        World server;
        BuildScene(server, 10, 50);
        SnapshotOptions opts{enc, true, 30};
        SnapshotEncoder e(opts);
        World client;
        SnapshotDecoder d;
        uint32_t tick = 0;

        const std::string first = e.Encode(server, ++tick);
        Check(e.LastStats().keyframe && e.LastStats().objects == 60, "the first snapshot of a delta stream is a keyframe with everything");
        d.Apply(client, first);
        const std::string same = e.Encode(server, ++tick);
        Check(!e.LastStats().keyframe && e.LastStats().objects == 0, "nothing changed: an empty delta");
        Check(same.size() < 40, "and it is tiny", static_cast<double>(same.size()));

        systems::MovePaths(server, 0.7);
        const std::string moved = e.Encode(server, ++tick);
        Check(e.LastStats().objects == 10, "the movers moved: only those 10 are sent");
        d.Apply(client, moved);
        Check(SameWorld(server, client), "and the client matches the server again");

        // Removals, on a fresh stream.
        SnapshotEncoder e2(opts);
        World server2;
        BuildScene(server2, 10, 50);
        World client2;
        SnapshotDecoder d2;
        d2.Apply(client2, e2.Encode(server2, 1));
        server2.Destroy(5);
        server2.Flush();
        const std::string rm = e2.Encode(server2, 2);
        Check(e2.LastStats().removed == 1 && e2.LastStats().objects == 0, "a destroyed object is sent as one removal");
        d2.Apply(client2, rm);
        client2.Flush();
        Check(client2.Find(5) == nullptr && SameWorld(server2, client2), "and the client removes it");

        SnapshotEncoder e3({enc, true, 4});
        World w3;
        BuildScene(w3, 2, 2);
        int keys = 0;
        for (int i = 0; i < 12; ++i) {
            e3.Encode(w3, static_cast<uint32_t>(i));
            if (e3.LastStats().keyframe) ++keys;
        }
        Check(keys == 3, "keyframeEvery 4: a keyframe every 4th snapshot", keys, 3);
        SnapshotEncoder e5({enc, true, 100});
        const std::string key = e5.Encode(w3, 1), notKey = e5.Encode(w3, 2);
        Check(IsKeyframe(key) && !IsKeyframe(notKey) && !IsKeyframe("") && !IsKeyframe("garbage"),
              "IsKeyframe() tells a keyframe from a delta by its header alone");

        World late;
        SnapshotDecoder dl;
        SnapshotEncoder e4({enc, true, 10});
        World w4;
        BuildScene(w4, 3, 3);
        e4.Encode(w4, 1);
        systems::MovePaths(w4, 2.0);
        ApplyResult early = dl.Apply(late, e4.Encode(w4, 2));
        Check(early.ok && early.skipped && late.Count() == 0, "a client that joins mid-stream ignores deltas until a keyframe");
        e4.ForceKeyframe();
        dl.Apply(late, e4.Encode(w4, 3));
        Check(SameWorld(w4, late), "ForceKeyframe() brings it up to date at once");
    }

    printf("\nKeyframes remove what they leave out\n");
    {
        World server;
        BuildScene(server, 0, 5);
        SnapshotEncoder e({Encoding::Binary});
        World client;
        SnapshotDecoder d;
        d.Apply(client, e.Encode(server, 1));
        server.Destroy(2);
        server.Flush();
        ApplyResult r = d.Apply(client, e.Encode(server, 2));
        client.Flush();
        Check(r.removed == 1 && client.Find(2) == nullptr && client.Count() == 4, "an object missing from the next full snapshot is removed");

        GameObject &mine = client.Create("hud");   // client-local, never in any snapshot
        mine.Add<Transform>();
        d.Apply(client, e.Encode(server, 3));
        client.Flush();
        Check(client.Find(mine.Id()) != nullptr, "objects the source never sent are left alone");
    }

    printf("\nRemoving a component reaches the receiver\n");
    for (Encoding enc : {Encoding::Text, Encoding::Binary}) {
        for (bool delta : {false, true}) {
            World server;
            GameObject &gate = server.Create("gate");
            gate.Add<Transform>(500, 300, 40, 200);
            gate.Add<Collider>();
            gate.Add<Renderable>(SDL_Color{150, 150, 150, 255});
            gate.Add<NetIdentity>();
            SnapshotEncoder e({enc, delta, 60});
            World client;
            SnapshotDecoder d;
            d.Apply(client, e.Encode(server, 1));
            GameObject *copy = client.Find(gate.Id());
            copy->Add<Health>().label = "added here";   // the client dresses it itself
            copy->Add<Respawn>();                       // even with a component that travels
            gate.Remove<Collider>();                    // the server opens the gate
            d.Apply(client, e.Encode(server, 2));
            char label[120];
            snprintf(label, sizeof label, "%s %s: the server removes the gate's Collider - the client's copy loses it too",
                     EncodingName(enc), delta ? "delta" : "full");
            Check(copy && !copy->Has<Collider>() && copy->Has<Renderable>() && copy->Has<Transform>(), label);
            snprintf(label, sizeof label, "%s %s: components the client attached itself are left alone",
                     EncodingName(enc), delta ? "delta" : "full");
            Check(copy->Has<Health>() && copy->Has<Respawn>(), label);
            gate.Add<Collider>();                       // and closes it again
            d.Apply(client, e.Encode(server, 3));
            Check(copy->Has<Collider>(), "and adding it back reaches the client");
        }
    }
    {
        World server;
        GameObject &p = server.Create("player");
        p.Add<Transform>(1, 2, 3, 4);
        p.Add<InputState>();
        p.Add<NetIdentity>(1);
        World client;
        SnapshotDecoder d;
        d.Apply(client, SnapshotEncoder({Encoding::Binary}).Encode(server, 1));
        d.Apply(client, SnapshotEncoder({Encoding::Binary, false, 60, {"InputState"}}).Encode(server, 2));
        Check(client.Find(p.Id())->Has<Transform>(), "a whitelisted message (inputs only) never removes the components it leaves out");
    }

    printf("\nOwnership\n");
    {
        World server;
        GameObject &p1 = server.Create("player");
        p1.Add<Transform>(100, 100, 32, 48);
        p1.Add<NetIdentity>(1);
        GameObject &p2 = server.Create("player");
        p2.Add<Transform>(500, 100, 32, 48);
        p2.Add<NetIdentity>(2);
        SnapshotEncoder down({Encoding::Text});

        World client1;
        SnapshotDecoder d;
        ApplyOptions asClient1;
        asClient1.skipOwner = 1;
        d.Apply(client1, down.Encode(server, 1), asClient1);
        Check(client1.Count() == 2, "a client is told about its own player the first time");
        client1.Find(p1.Id())->Get<Transform>()->position.x = 160.0f;   // it moved locally
        server.Find(p2.Id())->Get<Transform>()->position.x = 520.0f;
        d.Apply(client1, down.Encode(server, 2), asClient1);
        Check(client1.Find(p1.Id())->Get<Transform>()->position.x == 160.0f, "after that its own player is never overwritten by the server's older copy");
        Check(client1.Find(p2.Id())->Get<Transform>()->position.x == 520.0f, "while other players keep updating");

        // Upstream: client 1 sends its player; the server accepts only that.
        SnapshotEncoder up({Encoding::Binary});
        client1.Find(p2.Id())->Get<Transform>()->position.x = 9999.0f;   // a lying client
        const std::string fromClient1 = up.Encode(client1, 3);
        SnapshotDecoder serverSide;
        ApplyOptions fromOne;
        fromOne.source = 1;
        fromOne.onlyOwner = 1;
        serverSide.Apply(server, fromClient1, fromOne);
        Check(server.Find(p1.Id())->Get<Transform>()->position.x == 160.0f, "the server takes client 1's word for client 1's player");
        Check(server.Find(p2.Id())->Get<Transform>()->position.x == 520.0f, "and ignores what client 1 says about anyone else");

        SnapshotEncoder mineOnly({Encoding::Text});
        mineOnly.Encode(client1, 4, OwnedBy(1));
        Check(mineOnly.LastStats().objects == 1, "an include filter sends only the objects this endpoint owns");
    }

    printf("\nInputs instead of state\n");
    {
        World server;
        GameObject &p = server.Create("player");
        p.Add<Transform>(100, 100, 32, 48);
        p.Add<RigidBody>().velocity = {5, 6};
        p.Add<InputState>();
        p.Add<NetIdentity>(2);

        World client;
        SnapshotDecoder d;
        d.Apply(client, SnapshotEncoder({Encoding::Binary}).Encode(server, 1));
        client.Find(p.Id())->Get<InputState>()->buttons = InputState::Right | InputState::Jump;
        client.Find(p.Id())->Get<Transform>()->position.x = 777.0f;

        SnapshotEncoder inputs({Encoding::Binary, false, 60, {"InputState"}});
        const std::string msg = inputs.Encode(client, 2, OwnedBy(2));
        SnapshotEncoder state({Encoding::Binary});
        const std::string full = state.Encode(client, 2, OwnedBy(2));
        Check(msg.size() < full.size(), "an inputs-only message is smaller than the full state", static_cast<double>(msg.size()), static_cast<double>(full.size()));
        SnapshotDecoder sd;
        ApplyOptions opts;
        opts.source = 2;
        opts.onlyOwner = 2;
        sd.Apply(server, msg, opts);
        Check(p.Get<InputState>()->buttons == (InputState::Right | InputState::Jump), "the server receives the buttons");
        Check(p.Get<Transform>()->position.x == 100.0f && p.Get<RigidBody>()->velocity.x == 5.0f,
              "and nothing else: the other components are untouched (the server simulates them)");
    }

    printf("\nBad input\n");
    {
        World w;
        SnapshotDecoder d;
        Check(!d.Apply(w, "").ok && !d.Apply(w, "X garbage").ok && !d.Apply(w, "T 1 1 1 1 2").ok, "empty, unknown and truncated messages are rejected");
        Check(!d.Apply(w, std::string("B\x01\x01", 3)).ok, "a truncated binary header is rejected");
        World s;
        s.Create("x").Add<NetIdentity>();
        std::string m = SnapshotEncoder({Encoding::Text}).Encode(s, 1);
        Check(d.Apply(w, m).ok, "a well-formed message is accepted");
        std::string unknown = "T 1 1 1 0 1 1 % -1 0 1 NoSuchComponent 0";
        Check(!d.Apply(w, unknown).ok, "a component this process never registered is rejected");
    }

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "ALL PASS", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
