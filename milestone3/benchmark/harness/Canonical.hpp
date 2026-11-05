#pragma once

// Canonical state: a topology-independent, transport-independent text form of
// every networked object, used to prove that both sides of a comparison
// simulated exactly the same game.
//
// One line per object with a NetIdentity: id, kind, owner, then each
// travelling component (sorted by name) with its fields as typed tokens.
// Floats are written as hexadecimal floats, so equality is bit-exact and a
// difference can be measured. The NetIdentity time stamp is excluded.
//
// Runtime-only fields that the engine never sends (the jump latch, what a
// body stands on) are written as separate "R" lines and compared only on the
// endpoint that simulates that object.

#include "engine/World.hpp"

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace rb {

std::string CanonicalObject(const engine::GameObject &object);
std::string CanonicalRuntime(const engine::GameObject &object);   // empty if the object has no runtime state

// Writes one checkpoint: "B <block>", the object lines, then the runtime lines.
void WriteCheckpoint(std::FILE *out, long long block, const engine::World &world);

class OracleFile {
public:
    bool Load(const std::string &path, std::string &error);
    bool Has(long long block) const { return blocks.count(block) != 0; }

    struct Line {
        int owner = -1;
        std::string text;
    };
    struct Block {
        std::map<uint32_t, Line> objects;
        std::map<uint32_t, std::string> runtime;
    };
    const Block *Find(long long block) const;

private:
    std::map<long long, Block> blocks;
};

struct VerifyResult {
    long long checks = 0;              // checkpoints verified
    long long compared = 0;            // object lines compared
    long long mismatched = 0;
    long long missing = 0;             // in the oracle, absent here
    long long extra = 0;               // here, absent from the oracle
    long long runtimeCompared = 0;
    long long runtimeMismatched = 0;
    long long unavailable = 0;         // checkpoint absent from the oracle file
    double maxFloatError = 0.0;        // largest |endpoint - oracle| over float fields
    std::vector<std::string> first;    // first few differences, for the log

    bool Ok() const { return mismatched == 0 && missing == 0 && extra == 0 && runtimeMismatched == 0 && unavailable == 0; }
    std::string ToJson() const;
};

using OwnerFilter = std::function<bool(int owner)>;
using SimFilter = std::function<bool(const engine::GameObject &)>;

// Compares the objects whose owner `owners` accepts against the oracle at
// `block`; runtime lines for the objects `simulated` accepts.
void Verify(const OracleFile &oracle, long long block, const engine::World &world, const OwnerFilter &owners,
            const SimFilter &simulated, VerifyResult &result);

} // namespace rb
