#pragma once

// What is simulated: the deterministic level, the players, one simulation
// tick, and the scheduled input traces. Two scenarios:
//
//   engine  the engine benchmark's level (floor, static and moving platforms,
//           four spawns, a death zone) and players, built from a seed with the
//           harness's own PRNG, simulated with the engine's systems.
//   game    Clockwork Dash's course and bench objects, simulated with the
//           game's own Step(), for the separate real-game suite.
//
// Every endpoint and the offline oracle build the same objects with the same
// ids, so state can be compared object by object.

#include "Common.hpp"
#include "engine/Systems.hpp"
#include "engine/World.hpp"

#include <memory>
#include <string>
#include <vector>

namespace rb {

constexpr engine::ObjectId PLAYER_BASE = 0x00F00000;    // engine scenario: player i is PLAYER_BASE + i
constexpr engine::ObjectId FIXTURE_BASE = 0x00E00000;   // validation markers
constexpr engine::ObjectId DENSITY_BASE = 0x00D00000;   // change-density objects
constexpr int DENSITY_OBJECTS = 100;

// The buttons each player holds, one value per block (both ticks of a block
// share it, so a single InputState sample per two-tick update carries the
// complete input history, every press and release edge included).
std::vector<uint32_t> MakeTrace(int seed, int owner, int blocks);

class Scenario {
public:
    explicit Scenario(const Options &options);
    virtual ~Scenario() = default;
    static std::unique_ptr<Scenario> Make(const Options &options);

    // Ids start at 1 in a fresh World. `networked` gives level objects a
    // NetIdentity (the server replicates them); otherwise they are local.
    virtual void BuildLevel(engine::World &world, bool networked) = 0;
    virtual void AddPlayers(engine::World &world) = 0;
    // One simulation tick (1-based) for the objects `authority` accepts.
    virtual void Tick(engine::World &world, long long tick, const engine::ObjectFilter &authority) = 0;

    // Validation fixtures on objects owned by `owner` (-1: the server),
    // after the tick's simulation.
    virtual void Fixtures(engine::World &, long long /*tick*/, int /*owner*/) {}
    // Change-density schedule, once per block before the publication.
    virtual void Density(engine::World &, long long /*block*/) {}

    uint32_t Buttons(int owner, long long block) const;
    uint64_t TraceHash() const;
    // Object categories, as a JSON object.
    virtual std::string Counts(const engine::World &world) const;

protected:
    const Options &opts;
    std::vector<std::vector<uint32_t>> traces;
};

// Schema fingerprint: every registered component name and wire id.
uint64_t SchemaHash();

} // namespace rb
