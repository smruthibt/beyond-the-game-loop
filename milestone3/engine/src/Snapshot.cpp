#include "engine/Snapshot.hpp"

#include "engine/Components.hpp"
#include "engine/Timeline.hpp"

#include <algorithm>
#include <iterator>

namespace engine {

namespace {

constexpr uint8_t VERSION = 1;
constexpr uint8_t FLAG_KEYFRAME = 1;
constexpr uint8_t FLAG_DELTA = 2;
// Every object in the message carries ALL its travelling components (no
// whitelist), so a component the receiver got from the network earlier and
// that is missing now has been removed at the source.
constexpr uint8_t FLAG_COMPLETE = 4;


} // namespace

SnapshotEncoder::SnapshotEncoder(SnapshotOptions opts)
    : options(std::move(opts)),
      body(MakeWriter(options.encoding)),
      objects(MakeWriter(options.encoding)),
      scratch(MakeWriter(options.encoding)) {
    ComponentRegistry::RegisterBuiltins();
    if (options.keyframeEvery < 1) options.keyframeEvery = 1;
}

// The whitelist is looked up at the first Encode, not in the constructor, so
// a game component registered after the encoder was made is still found. A
// whitelist never falls back to "everything": a name that is not registered
// simply matches nothing.
void SnapshotEncoder::ResolveWhitelist() {
    if (resolved) return;
    for (const std::string &name : options.components) {
        if (const ComponentInfo *info = ComponentRegistry::ByName(name)) allowed.push_back(info);
    }
    resolved = true;
}

// By key, not by name: this runs for every component of every object in every
// snapshot, and a pointer compare is all it needs. NetIdentity travels in each
// object's header, not as a component.
const ComponentInfo *SnapshotEncoder::Travels(ComponentKey key) const {
    if (key == KeyOf<NetIdentity>()) return nullptr;
    if (options.components.empty()) return ComponentRegistry::ByKey(key);
    for (const ComponentInfo *info : allowed) {
        if (info->key == key) return info;
    }
    return nullptr;
}

std::string SnapshotEncoder::Encode(const World &world, uint32_t tick, const ObjectFilter &include) {
    ResolveWhitelist();
    const bool keyframe = !options.delta || forceKeyframe || sinceKeyframe + 1 >= options.keyframeEvery;
    forceKeyframe = false;
    sinceKeyframe = keyframe ? 0 : sinceKeyframe + 1;

    stats = SnapshotStats{};
    stats.keyframe = keyframe;
    objects->Clear();
    std::set<ObjectId> present;

    world.Each<NetIdentity>([&](const GameObject &object, const NetIdentity &net) {
        if (include && !include(object)) return;

        // The object's components, encoded once: they are both what is sent
        // and, in a delta stream, what the next snapshot compares against.
        scratch->Clear();
        travelling.clear();
        for (const GameObject::Slot &slot : object.Components()) {
            if (const ComponentInfo *info = Travels(slot.key)) travelling.emplace_back(info, slot.component.get());
        }
        scratch->U8(static_cast<uint8_t>(travelling.size()));
        for (const auto &t : travelling) {
            scratch->Tag(*t.first);
            t.second->Write(*scratch);
        }

        if (options.delta) {
            present.insert(object.Id());
            auto it = lastSent.find(object.Id());
            if (!keyframe && it != lastSent.end() && it->second == scratch->Data()) return;   // unchanged
            lastSent[object.Id()] = scratch->Data();
        }

        objects->U32(object.Id());
        objects->Str(object.Kind());
        objects->I32(net.owner);
        objects->I64(net.stampMicros);
        objects->Raw(scratch->Data());
        ++stats.objects;
    });

    // Gone since the last snapshot. A keyframe says it by leaving them out.
    std::vector<ObjectId> removed;
    if (options.delta) {
        for (auto it = lastSent.begin(); it != lastSent.end();) {
            if (present.count(it->first)) { ++it; continue; }
            if (!keyframe) removed.push_back(it->first);
            it = lastSent.erase(it);
        }
        std::sort(removed.begin(), removed.end());
    }
    stats.removed = removed.size();

    body->Clear();
    body->U8(VERSION);
    body->U8(static_cast<uint8_t>((keyframe ? FLAG_KEYFRAME : 0) | (options.delta ? FLAG_DELTA : 0) |
                                  (options.components.empty() ? FLAG_COMPLETE : 0)));
    body->U32(tick);
    body->I64(Timeline::RealTimeNow());
    body->U32(static_cast<uint32_t>(stats.objects));
    body->Raw(objects->Data());
    body->U32(static_cast<uint32_t>(removed.size()));
    for (ObjectId id : removed) body->U32(id);

    std::string message = options.encoding == Encoding::Binary ? "B" : "T ";
    message += body->Data();
    stats.bytes = message.size();
    return message;
}

SnapshotDecoder::SnapshotDecoder() {
    ComponentRegistry::RegisterBuiltins();
}

bool IsKeyframe(const std::string &message) {
    if (message.size() < 3) return false;
    if (message[0] == 'B') return (static_cast<uint8_t>(message[2]) & FLAG_KEYFRAME) != 0;
    if (message[0] != 'T') return false;
    std::unique_ptr<Reader> in = MakeReader(Encoding::Text, message, 2);
    in->U8();   // version
    const uint8_t flags = in->U8();
    return in->Ok() && (flags & FLAG_KEYFRAME) != 0;
}

ApplyResult SnapshotDecoder::Apply(World &world, const std::string &message, const ApplyOptions &opts) {
    ApplyResult result;
    if (message.empty() || (message[0] != 'T' && message[0] != 'B')) return result;
    const Encoding encoding = message[0] == 'B' ? Encoding::Binary : Encoding::Text;
    std::unique_ptr<Reader> in = MakeReader(encoding, message, encoding == Encoding::Binary ? 1 : 2);

    const uint8_t version = in->U8();
    const uint8_t flags = in->U8();
    result.tick = in->U32();
    result.sentMicros = in->I64();
    const uint32_t count = in->U32();
    if (!in->Ok() || version != VERSION) return result;
    result.keyframe = (flags & FLAG_KEYFRAME) != 0;
    const bool complete = (flags & FLAG_COMPLETE) != 0;
    std::vector<ComponentKey> received;

    // A delta means nothing until we hold the keyframe it builds on.
    if (!result.keyframe && (flags & FLAG_DELTA) && !keyed.count(opts.source)) {
        result.ok = true;
        result.skipped = true;
        return result;
    }

    std::vector<ObjectId> &mine = known[opts.source];   // sorted
    std::vector<ObjectId> seen;
    seen.reserve(count);

    for (uint32_t i = 0; i < count && in->Ok(); ++i) {
        const ObjectId id = in->U32();
        const std::string kind = in->Str();
        const int owner = in->I32();
        const int64_t stamp = in->I64();
        const uint8_t components = in->U8();
        if (!in->Ok()) break;

        GameObject *object = world.Find(id);
        bool apply = true;
        if (opts.onlyOwner != ANY_OWNER && owner != opts.onlyOwner) apply = false;
        if (opts.skipOwner != ANY_OWNER && owner == opts.skipOwner && object) apply = false;
        if (!object && !opts.createMissing) apply = false;
        // An object that exists here with another owner cannot be taken over.
        if (object && opts.onlyOwner != ANY_OWNER && OwnerOf(*object) != opts.onlyOwner) apply = false;

        if (apply && !object) {
            object = &world.CreateWithId(id, kind);
            ++result.created;
            result.createdIds.push_back(id);
        } else if (apply) {
            ++result.updated;
        }

        received.clear();
        for (uint8_t c = 0; c < components && in->Ok(); ++c) {
            const ComponentInfo *info = in->Tag();
            if (!info) break;
            if (apply) {
                Component *target = object->Find(info->key);
                if (!target) {
                    std::unique_ptr<Component> made = info->make();
                    target = made.get();
                    object->Attach(info->key, std::move(made));
                }
                target->Read(*in);
                object->MarkReplicated(info->key);
                received.push_back(info->key);
            } else {
                std::unique_ptr<Component> scratch = info->make();   // consume it, keep nothing
                scratch->Read(*in);
            }
        }

        // The sender no longer has a component it gave us before: take it
        // off here too (a gate's Collider when the gate opens).
        if (apply && complete && in->Ok()) {
            std::vector<ComponentKey> gone;
            for (const GameObject::Slot &slot : object->Components()) {
                if (slot.replicated && std::find(received.begin(), received.end(), slot.key) == received.end()) {
                    gone.push_back(slot.key);
                }
            }
            for (ComponentKey key : gone) object->Detach(key);
        }

        if (apply) {
            NetIdentity *net = object->Get<NetIdentity>();
            if (!net) net = &object->Add<NetIdentity>();
            net->owner = owner;
            net->stampMicros = stamp;
        }
        // Even an object we skipped is one this source still vouches for.
        if (apply || (opts.skipOwner != ANY_OWNER && owner == opts.skipOwner)) seen.push_back(id);
    }

    const uint32_t removedCount = in->U32();
    std::vector<ObjectId> removedIds;
    for (uint32_t i = 0; i < removedCount && in->Ok(); ++i) removedIds.push_back(in->U32());
    if (!in->Ok()) return result;

    auto destroy = [&](ObjectId id) {
        GameObject *object = world.Find(id);
        if (object && (opts.onlyOwner == ANY_OWNER || OwnerOf(*object) == opts.onlyOwner)) {
            world.Destroy(id);
            ++result.removed;
        }
    };

    // Encoders write objects in id order, so `seen` is normally sorted already.
    if (!std::is_sorted(seen.begin(), seen.end())) std::sort(seen.begin(), seen.end());

    if (result.keyframe) {
        // Everything this source told us about before and has now left out is gone.
        std::vector<ObjectId> gone;
        std::set_difference(mine.begin(), mine.end(), seen.begin(), seen.end(), std::back_inserter(gone));
        for (ObjectId id : gone) destroy(id);
        mine.swap(seen);
        keyed.insert(opts.source);
    } else {
        std::vector<ObjectId> merged;
        merged.reserve(mine.size() + seen.size());
        std::set_union(mine.begin(), mine.end(), seen.begin(), seen.end(), std::back_inserter(merged));
        mine.swap(merged);
        for (ObjectId id : removedIds) {
            auto it = std::lower_bound(mine.begin(), mine.end(), id);
            if (it != mine.end() && *it == id) {
                mine.erase(it);
                destroy(id);
            }
        }
    }

    result.ok = true;
    return result;
}

void SnapshotDecoder::ForgetSource(int source) {
    known.erase(source);
    keyed.erase(source);
}

} // namespace engine
