#include "engine/NetScene.hpp"

#include "engine/Engine.hpp"
#include "engine/Timeline.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iterator>

namespace engine {

namespace {

long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

bool IsSnapshot(const std::string &message) {
    return !message.empty() && (message[0] == 'T' || message[0] == 'B') && message != "BYE";
}

// Everything owned by `owner` that has a stamp newer than the last one seen
// is one latency sample.
void SampleLatency(const World &world, int notOwner, std::map<ObjectId, int64_t> &lastStamp, NetStats &stats) {
    const int64_t now = Timeline::RealTimeNow();
    world.Each<NetIdentity>([&](const GameObject &object, const NetIdentity &net) {
        if (net.owner < 0 || net.owner == notOwner || net.stampMicros <= 0) return;
        int64_t &last = lastStamp[object.Id()];
        if (net.stampMicros > last) {
            last = net.stampMicros;
            stats.AddLatency(now - net.stampMicros);
        }
    });
}

// An object the game has not stamped since the last send is stamped now, so
// latency is measured whether or not the game calls StampLocal(). Returns the
// newest stamp now on this endpoint's objects.
int64_t StampUnstamped(World &world, int owner, int64_t lastSent) {
    const int64_t now = Timeline::RealTimeNow();
    int64_t newest = lastSent;
    world.Each<NetIdentity>([&](GameObject &, NetIdentity &n) {
        if (n.owner != owner) return;
        if (n.stampMicros <= lastSent) n.stampMicros = now;
        newest = std::max(newest, n.stampMicros);
    });
    return newest;
}

} // namespace

// --- NetOptions ------------------------------------------------------------------

bool NetOptions::Parse(int argc, char **argv, NetOptions &out, std::string *error) {
    auto fail = [error](const std::string &message) {
        if (error) *error = message;
        return false;
    };
    for (int i = 1; i + 1 < argc; ++i) {
        const std::string flag = argv[i], value = argv[i + 1];
        if (flag == "--topology") {
            if (value == "cs" || value == "client-server") out.topology = Topology::ClientServer;
            else if (value == "p2p" || value == "peer-to-peer") out.topology = Topology::PeerToPeer;
            else return fail("--topology takes cs or p2p, not " + value);
        } else if (flag == "--encoding") {
            if (!ParseEncoding(value, out.encoding)) return fail("--encoding takes text or binary, not " + value);
        } else if (flag == "--delta") {
            if (value == "on") out.delta = true;
            else if (value == "off") out.delta = false;
            else return fail("--delta takes on or off, not " + value);
        } else if (flag == "--upstream") {
            if (value == "state") out.upstream = Upstream::State;
            else if (value == "input") out.upstream = Upstream::Input;
            else return fail("--upstream takes state or input, not " + value);
        } else if (flag == "--keyframe") {
            out.keyframeEvery = std::atoi(value.c_str());
            if (out.keyframeEvery < 1) return fail("--keyframe takes a count of 1 or more");
        } else {
            continue;
        }
        ++i;
    }
    return true;
}

std::string NetOptions::Describe() const {
    std::string s = topology == Topology::PeerToPeer ? "p2p" : "cs";
    s += std::string(" ") + EncodingName(encoding);
    s += delta ? " delta" : " full";
    s += upstream == Upstream::Input ? " input" : " state";
    return s;
}

SnapshotOptions NetOptions::Downstream() const {
    return SnapshotOptions(encoding, delta, keyframeEvery);
}

SnapshotOptions NetOptions::Upward() const {
    if (upstream == Upstream::Input) return SnapshotOptions(encoding, delta, keyframeEvery, {InputState::TYPE});
    return SnapshotOptions(encoding, delta, keyframeEvery);
}

// --- NetStats --------------------------------------------------------------------

void NetStats::AddLatency(int64_t micros) {
    std::lock_guard<std::mutex> lock(mutex);
    if (samples.size() < MAX) samples.push_back(micros);
    else samples[next] = micros;
    next = (next + 1) % MAX;
}

LatencySummary NetStats::Latency() const {
    std::vector<int64_t> copy;
    {
        std::lock_guard<std::mutex> lock(mutex);
        copy = samples;
    }
    LatencySummary s;
    s.samples = copy.size();
    if (copy.empty()) return s;
    std::sort(copy.begin(), copy.end());
    double sum = 0;
    for (int64_t v : copy) sum += static_cast<double>(v);
    s.meanMs = sum / static_cast<double>(copy.size()) / 1000.0;
    s.p50Ms = static_cast<double>(copy[copy.size() / 2]) / 1000.0;
    s.p95Ms = static_cast<double>(copy[std::min(copy.size() - 1, copy.size() * 95 / 100)]) / 1000.0;
    s.maxMs = static_cast<double>(copy.back()) / 1000.0;
    return s;
}

void NetStats::ResetLatency() {
    std::lock_guard<std::mutex> lock(mutex);
    samples.clear();
    next = 0;
}

// --- SceneServer -----------------------------------------------------------------

SceneServer::SceneServer(Engine &engineRef, Server::Config config, NetOptions opts)
    : engine(engineRef), server(engineRef, config), options(opts), downstream(opts.Downstream()) {}

ObjectFilter SceneServer::Authority() const {
    if (options.upstream == Upstream::Input) return {};   // everything, players included
    return OwnedBy(-1);
}

bool SceneServer::Run(const Callbacks &callbacks) {
    std::mutex &mutex = engine.GetWorldMutex();
    Server::Hooks hooks;

    hooks.onJoin = [this, &callbacks, &mutex](int id) {
        std::lock_guard<std::mutex> lock(mutex);
        if (callbacks.onJoin) callbacks.onJoin(world, id);
        downstream.ForceKeyframe();   // the newcomer needs everything, now
    };

    hooks.onLeave = [this, &callbacks, &mutex](int id) {
        std::lock_guard<std::mutex> lock(mutex);
        if (callbacks.onLeave) callbacks.onLeave(world, id);
        world.DestroyIf([id](const GameObject &o) { return OwnerOf(o) == id; });
        upstream.ForgetSource(id);
    };

    hooks.onRequest = [this, &callbacks, &mutex](int id, const std::string &request) -> std::string {
        if (!IsSnapshot(request)) {
            std::lock_guard<std::mutex> lock(mutex);
            return callbacks.onRequest ? callbacks.onRequest(world, id, request) : std::string("ERR unknown request");
        }
        ++stats.messagesIn;
        stats.bytesIn += request.size();
        std::lock_guard<std::mutex> lock(inboxMutex);
        inbox.emplace_back(id, request);
        return "ACK";   // received; it is applied at the start of the next step
    };

    hooks.buildWorld = [this, &mutex]() {
        std::string message;
        {
            std::lock_guard<std::mutex> lock(mutex);
            const auto t0 = std::chrono::steady_clock::now();
            message = downstream.Encode(world, ++tick);
            lastSnapshot = downstream.LastStats();
            stats.encodeMicros += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count());
        }
        ++stats.messagesOut;
        stats.bytesOut += message.size();
        return message;
    };

    hooks.step = [this, &callbacks, &mutex](float dt) {
        std::lock_guard<std::mutex> lock(mutex);
        ApplyInbox();
        if (callbacks.step) callbacks.step(world, dt);
        world.Flush();
    };

    return server.Run(hooks);
}

// Main thread, world locked.
void SceneServer::ApplyInbox() {
    {
        std::lock_guard<std::mutex> lock(inboxMutex);
        applying.swap(inbox);
    }
    for (const auto &message : applying) {
        const auto t0 = std::chrono::steady_clock::now();
        ApplyOptions apply;
        apply.source = message.first;
        apply.onlyOwner = message.first;   // a client speaks only for its own objects
        apply.createMissing = false;
        upstream.Apply(world, message.second, apply);
        ++stats.decoded;
        stats.decodeMicros += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0).count());
    }
    applying.clear();
}

// --- SceneClient -----------------------------------------------------------------

namespace {
// The world socket carries nothing but snapshots, which start with their
// encoding byte rather than a topic: subscribe to everything on it.
Client::Config ForSnapshots(Client::Config config) {
    config.worldTopic = "";
    return config;
}
} // namespace

SceneClient::SceneClient(Client::Config config, NetOptions opts)
    : client(ForSnapshots(config)), options(opts), up(opts.Upward()) {}

bool SceneClient::Join() {
    return client.Join();
}

GameObject *SceneClient::MyPlayer() {
    GameObject *mine = nullptr;
    const int id = Id();
    world.Each<NetIdentity>([&](GameObject &o, NetIdentity &n) {
        if (!mine && n.owner == id) mine = &o;
    });
    return mine;
}

ObjectFilter SceneClient::Authority() const {
    if (options.upstream == Upstream::Input) return [](const GameObject &) { return false; };
    return OwnedBy(client.Id());
}

void SceneClient::StampLocal() {
    const int64_t now = Timeline::RealTimeNow();
    const int id = Id();
    world.Each<NetIdentity>([&](GameObject &, NetIdentity &n) {
        if (n.owner == id) n.stampMicros = now;
    });
}

void SceneClient::Tick(float dt, std::mutex &worldMutex) {
    const int id = Id();
    inbox.clear();
    client.Tick(
        dt,
        [&](const std::string &message) {
            ++stats.messagesIn;
            stats.bytesIn += message.size();
            inbox.push_back(message);
        },
        [&]() {
            std::string message;
            {
                std::lock_guard<std::mutex> lock(worldMutex);
                const auto t0 = std::chrono::steady_clock::now();
                lastSentStamp = StampUnstamped(world, id, lastSentStamp);
                message = up.Encode(world, ++tick, OwnedBy(id));
                stats.encodeMicros += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - t0).count());
            }
            ++stats.messagesOut;
            stats.bytesOut += message.size();
            return message;
        });

    // Everything before the newest keyframe in the queue is superseded by it:
    // a client that fell behind catches up in one step instead of decoding
    // its whole backlog.
    size_t first = 0;
    for (size_t i = inbox.size(); i-- > 0;) {
        if (IsKeyframe(inbox[i])) { first = i; break; }
    }
    stats.skipped += first;
    for (size_t i = first; i < inbox.size(); ++i) {
        ApplyOptions apply;
        apply.source = -1;
        // Sending state: my own player is mine, the server's copy is older.
        // Sending inputs: the server moves my player, so take its copy.
        apply.skipOwner = options.upstream == Upstream::State ? id : ANY_OWNER;
        std::lock_guard<std::mutex> lock(worldMutex);
        const auto t0 = std::chrono::steady_clock::now();
        const ApplyResult result = down.Apply(world, inbox[i], apply);
        if (result.ok && result.keyframe) hasWorld = true;
        world.Flush();
        ++stats.decoded;
        stats.decodeMicros += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0).count());
        // The first batch is whatever queued up between Join() and the first
        // Tick - while a window opened, say - and the queue keeps the OLDEST
        // messages when it fills. That wait is not network latency: note the
        // stamps it carries, measure from the next batch on.
        if (ticked) SampleLatency(world, id, lastStamp, stats);
        else SampleLatency(world, id, lastStamp, discard);
    }
    if (!inbox.empty()) ticked = true;
}

// --- ScenePeer -------------------------------------------------------------------

namespace {
constexpr const char *TOPIC_SNAP = "SNAP";
}

ScenePeer::ScenePeer(Config cfg, NetOptions opts)
    : config(cfg), options(opts), peer([&cfg] {
          Peer::Config p = cfg.peer;
          if (std::find(p.topics.begin(), p.topics.end(), TOPIC_SNAP) == p.topics.end()) p.topics.push_back(TOPIC_SNAP);
          return p;
      }()),
      up(opts.Upward()), state(SnapshotOptions(opts.encoding)),
      sendsSinceState(opts.keyframeEvery - 1) {}   // with inputs, the very first send is the full state

ScenePeer::~ScenePeer() {
    // Say goodbye, so the others drop this peer's objects at once instead of
    // after a timeout.
    if (peer.Id() >= 0) peer.Publish(TOPIC_SNAP, std::to_string(peer.Id()) + " BYE");
}

int ScenePeer::Claim(int requested) {
    const int id = peer.Claim(requested);
    if (id < 0) return id;
    peer.ConnectAll();
    world.SetNextId((static_cast<ObjectId>(id) + 1) << 24);
    lastSendMs = NowMs();
    return id;
}

ObjectFilter ScenePeer::Authority() const {
    const int id = peer.Id();
    if (options.upstream == Upstream::Input) {
        // Every player moves here: mine from my keys, the others from theirs.
        return [id](const GameObject &o) { const int owner = OwnerOf(o); return owner == id || (owner >= 0 && o.Has<InputState>()); };
    }
    return OwnedBy(id);
}

void ScenePeer::StampLocal() {
    const int64_t now = Timeline::RealTimeNow();
    const int id = Id();
    world.Each<NetIdentity>([&](GameObject &, NetIdentity &n) {
        if (n.owner == id) n.stampMicros = now;
    });
}

int ScenePeer::PeersHeard() const {
    std::lock_guard<std::mutex> lock(heardMutex);
    return static_cast<int>(lastHeardMs.size());
}

void ScenePeer::Send(std::mutex &worldMutex, bool fullState) {
    const int id = Id();
    std::string message;
    {
        std::lock_guard<std::mutex> lock(worldMutex);
        const auto t0 = std::chrono::steady_clock::now();
        lastSentStamp = StampUnstamped(world, id, lastSentStamp);
        message = (fullState ? state : up).Encode(world, ++tick, OwnedBy(id));
        stats.encodeMicros += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - t0).count());
    }
    peer.Publish(TOPIC_SNAP, std::to_string(id) + ' ' + message);
    ++stats.messagesOut;
    stats.bytesOut += message.size();
    lastSendMs = NowMs();
}

void ScenePeer::OnSnapshot(int sender, const std::string &snapshot, std::mutex &worldMutex) {
    {
        std::lock_guard<std::mutex> lock(heardMutex);
        lastHeardMs[sender] = NowMs();
    }
    if (snapshot == "BYE") {
        std::lock_guard<std::mutex> lock(heardMutex);
        lastHeardMs.erase(sender);
    }
    std::lock_guard<std::mutex> lock(worldMutex);
    if (snapshot == "BYE") {
        world.DestroyIf([sender](const GameObject &o) { return OwnerOf(o) == sender; });
        world.Flush();
        down.ForgetSource(sender);
        return;
    }
    ++stats.messagesIn;
    stats.bytesIn += snapshot.size();
    const auto t0 = std::chrono::steady_clock::now();
    ApplyOptions apply;
    apply.source = sender;
    apply.onlyOwner = sender;
    down.Apply(world, snapshot, apply);
    world.Flush();
    ++stats.decoded;
    stats.decodeMicros += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - t0).count());
    SampleLatency(world, Id(), lastStamp, stats);
}

void ScenePeer::Tick(float dt, std::mutex &worldMutex) {
    // 1. Hear everyone.
    peer.Tick([&](const std::string &topic, std::istringstream &body, int) {
        if (topic != TOPIC_SNAP) return;
        int sender = -1;
        body >> sender;
        body.get();   // the space after the id
        const std::string rest((std::istreambuf_iterator<char>(body)), std::istreambuf_iterator<char>());
        if (sender >= 0 && sender != Id()) OnSnapshot(sender, rest, worldMutex);
    });

    // 2. My objects, paced by game time like a client's upstream. With
    //    inputs, a full-state correction every keyframeEvery sends keeps the
    //    others' simulation of my player from drifting.
    sendAccumulator += dt;
    if (sendAccumulator >= config.sendIntervalSeconds) {
        sendAccumulator -= config.sendIntervalSeconds;
        if (sendAccumulator > config.sendIntervalSeconds) sendAccumulator = config.sendIntervalSeconds;
        bool full = false;
        if (options.upstream == Upstream::Input && ++sendsSinceState >= options.keyframeEvery) {
            sendsSinceState = 0;
            full = true;
        }
        Send(worldMutex, full);
    }

    // 3. A heartbeat on wall time when paused: the full state, so a peer that
    //    joined meanwhile has it too.
    if (NowMs() - lastSendMs >= config.heartbeatMs) Send(worldMutex, true);

    // 4. Peers that have gone silent are gone: their objects go with them.
    std::vector<int> silent;
    {
        std::lock_guard<std::mutex> lock(heardMutex);
        const long long now = NowMs();
        for (auto it = lastHeardMs.begin(); it != lastHeardMs.end();) {
            if (now - it->second > config.peerTimeoutMs) {
                silent.push_back(it->first);
                it = lastHeardMs.erase(it);
            } else {
                ++it;
            }
        }
    }
    if (!silent.empty()) {
        std::lock_guard<std::mutex> lock(worldMutex);
        for (int sender : silent) {
            world.DestroyIf([sender](const GameObject &o) { return OwnerOf(o) == sender; });
            down.ForgetSource(sender);
        }
        world.Flush();
    }
}

} // namespace engine
