// The correctness oracle: one process, no networking, the same level, players,
// traces, fixtures and change schedule, stepped tick by tick. Its canonical
// state at each checkpoint is what every endpoint of every strategy must hold.
#include "Endpoint.hpp"

#include <cstdio>

namespace rb {

using namespace engine;

int RunOracle(const Options &o) {
    if (o.out.empty()) {
        std::fprintf(stderr, "oracle: --out is required\n");
        return 2;
    }
    const int64_t t0 = NowNs();
    std::unique_ptr<Scenario> scenario = Scenario::Make(o);
    World world;
    scenario->BuildLevel(world, o.Native());
    scenario->AddPlayers(world);
    std::FILE *out = std::fopen(o.out.c_str(), "w");
    if (!out) {
        std::fprintf(stderr, "oracle: cannot write %s\n", o.out.c_str());
        return 2;
    }
    const std::string counts = scenario->Counts(world);
    WriteCheckpoint(out, 0, world);

    // The input traces as an artifact: per player, each block where the
    // buttons change (run-length encoded).
    if (std::FILE *trace = std::fopen((o.out + ".trace.csv").c_str(), "w")) {
        std::fprintf(trace, "player,from_block,buttons\n");
        for (int p = 0; p < o.players; ++p) {
            uint32_t last = 0xFFFFFFFFu;
            for (long long b = 1; b <= o.FinalBlock(); ++b) {
                const uint32_t buttons = scenario->Buttons(p, b);
                if (buttons != last) std::fprintf(trace, "%d,%lld,%u\n", p, b, buttons);
                last = buttons;
            }
        }
        std::fclose(trace);
    }

    // Workload evidence: how often players stand on something other than the
    // floor (collisions with platforms) and how often they respawn.
    const ObjectId floorId = 1;
    long long platformTicks = 0, airTicks = 0, jumpsStarted = 0;
    std::vector<int> deathsBefore;
    const long long final = o.FinalBlock();
    for (long long b = 1; b <= final; ++b) {
        for (long long t = 2 * b - 1; t <= 2 * b; ++t) {
            std::vector<float> vy;
            world.Each<InputState, NetIdentity>([&](GameObject &, InputState &in, NetIdentity &n) {
                in.buttons = scenario->Buttons(n.owner, b);
            });
            scenario->Tick(world, t, {});
            if (o.fixtures) {
                if (o.Native()) {
                    scenario->Fixtures(world, t, -1);
                } else if (!o.InputUp()) {
                    for (int owner = 0; owner < o.players; ++owner) scenario->Fixtures(world, t, owner);
                }
            }
            world.Flush();
            world.Each<RigidBody, InputState>([&](GameObject &, RigidBody &body, InputState &) {
                if (body.grounded && body.ground != floorId) ++platformTicks;
                if (!body.grounded) ++airTicks;
                if (body.velocity.y < -700.0f) ++jumpsStarted;   // ticks spent rising fast after a jump
            });
        }
        scenario->Density(world, b);
        if (b == o.warmupBlocks || b == final || (o.checkpointEvery > 0 && b % o.checkpointEvery == 0)) {
            WriteCheckpoint(out, b, world);
        }
    }
    std::fclose(out);

    std::string deaths = "[";
    world.Each<Respawn, NetIdentity>([&](GameObject &, Respawn &r, NetIdentity &n) {
        deaths += (deaths.size() > 1 ? "," : "") + std::string("[") + std::to_string(n.owner) + "," + std::to_string(r.deaths) + "]";
    });
    deaths += "]";
    Json j;
    j.Str("role", "oracle")
        .Str("options", o.Describe())
        .Str("trace_hash", Hex64(scenario->TraceHash()))
        .Str("schema_hash", Hex64(SchemaHash()))
        .Raw("counts", counts)
        .Raw("deaths", deaths)
        .Int("platform_ticks", platformTicks)
        .Int("air_ticks", airTicks)
        .Int("fast_rise_ticks", jumpsStarted)
        .Int("elapsed_ns", NowNs() - t0);
    std::printf("RESULT %s\n", j.Text().c_str());
    return 0;
}

} // namespace rb
