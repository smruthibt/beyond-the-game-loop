#pragma once

#include "engine/Serialize.hpp"
#include "engine/Systems.hpp"
#include "engine/World.hpp"

#include <climits>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

namespace engine {

// Milestone 3, Parts 1B and 1C: the networked scene's messages.
//
// A snapshot is a set of objects - each with its id, kind, owner, a time
// stamp and its components - and, in a delta stream, a list of objects that
// have gone. Only objects with a NetIdentity are ever sent; everything else
// is local to its endpoint.
//
// Three things an experiment can vary, none of which changes the game:
//
//   encoding    Text or Binary - how each field is written.
//   delta       false: every snapshot carries every object (a keyframe).
//               true:  only objects whose components changed since the last
//               snapshot from this encoder, plus removals, with a keyframe
//               every `keyframeEvery` snapshots so a late joiner or a lost
//               message is repaired.
//   components  which component types travel. Empty means all registered
//               ones; {"InputState"} sends only what a player is pressing,
//               which is how an endpoint sends inputs instead of state.
//
// The first byte of every snapshot says which encoding it is in ('T' or
// 'B'), so a receiver decodes whatever it is sent without being told.
struct SnapshotOptions {
    Encoding encoding = Encoding::Text;
    bool delta = false;
    int keyframeEvery = 60;
    std::vector<std::string> components;

    SnapshotOptions() = default;
    SnapshotOptions(Encoding e, bool deltaStream = false, int keyframe = 60, std::vector<std::string> only = {})
        : encoding(e), delta(deltaStream), keyframeEvery(keyframe), components(std::move(only)) {}
};

struct SnapshotStats {
    size_t objects = 0;    // objects written
    size_t removed = 0;    // removals written
    size_t bytes = 0;
    bool keyframe = false;
};

class SnapshotEncoder {
public:
    explicit SnapshotEncoder(SnapshotOptions options = {});

    // Every object with a NetIdentity that `include` accepts (all, if empty).
    std::string Encode(const World &world, uint32_t tick, const ObjectFilter &include = {});

    // The next Encode is a keyframe - after a new client joins, say.
    void ForceKeyframe() { forceKeyframe = true; }

    const SnapshotStats &LastStats() const { return stats; }
    const SnapshotOptions &Options() const { return options; }

private:
    const ComponentInfo *Travels(ComponentKey key) const;
    void ResolveWhitelist();

    SnapshotOptions options;
    std::vector<const ComponentInfo *> allowed;   // the whitelist, once resolved
    bool resolved = false;
    std::unordered_map<ObjectId, std::string> lastSent;   // delta: each object's components as last sent
    int sinceKeyframe = 0;
    bool forceKeyframe = true;
    SnapshotStats stats;
    std::unique_ptr<Writer> body, objects, scratch;
    std::vector<std::pair<const ComponentInfo *, const Component *>> travelling;   // reused per object
};

// True if `message` is a keyframe - a full snapshot that replaces everything
// its source said before. Reads only the header. A receiver that has fallen
// behind can skip straight to the newest keyframe it holds.
bool IsKeyframe(const std::string &message);

constexpr int ANY_OWNER = INT_MIN;

struct ApplyOptions {
    // Who sent it: -1 the server, otherwise a client or peer id. Removals and
    // keyframes only ever touch objects this source told us about.
    int source = -1;
    // Objects this endpoint owns and has already: their state here is the
    // truth, so the incoming copy is read and dropped.
    int skipOwner = ANY_OWNER;
    // Accept only objects with this owner - a server listening to client 3
    // takes client 3's word for client 3's player and nothing else.
    int onlyOwner = ANY_OWNER;
    // Create objects this World does not have. A server sets it false for
    // what clients send: it made their players itself, and a late message
    // from a client that has left must not bring its player back.
    bool createMissing = true;
};

struct ApplyResult {
    bool ok = false;          // parsed completely
    bool skipped = false;     // a delta that arrived before any keyframe from its source
    bool keyframe = false;
    uint32_t tick = 0;
    int64_t sentMicros = 0;
    size_t created = 0, updated = 0, removed = 0;
    std::vector<ObjectId> createdIds;
};

class SnapshotDecoder {
public:
    SnapshotDecoder();

    // Applies one snapshot to `world`. New objects are created with the
    // sender's ids; removed ones are destroyed (deferred - call world.Flush()).
    ApplyResult Apply(World &world, const std::string &message, const ApplyOptions &options = {});

    // Forget everything heard from this source - it disconnected.
    void ForgetSource(int source);

private:
    std::map<int, std::vector<ObjectId>> known;   // per source, sorted: the objects it has told us about
    std::set<int> keyed;                        // sources we have had a keyframe from
};

} // namespace engine
