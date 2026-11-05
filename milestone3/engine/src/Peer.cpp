#include "engine/Peer.hpp"

#include <chrono>
#include <thread>

namespace engine {

namespace {

constexpr const char *TOPIC_EPOCH = "EPOCH";
constexpr const char *TOPIC_EVENTS = "EVENTS";

long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

// The one rule every peer applies identically. Earlier epoch wins; on a tie,
// the lower id. No peer is special, so no peer has to be alive for the rule
// to keep working.
bool EarlierEpoch(int64_t t0, int proposer, int64_t thanT0, int thanProposer) {
    return t0 < thanT0 || (t0 == thanT0 && proposer < thanProposer);
}

} // namespace

Peer::Peer(Config cfg)
    : config(cfg),
      peerEpoch(static_cast<size_t>(cfg.maxPeers), 0),
      peerHeard(static_cast<size_t>(cfg.maxPeers), false) {
    sub.SetReceiveTimeoutMs(0);   // a poll: Tick never blocks on a peer that may never speak
    sub.Subscribe(TOPIC_EPOCH);
    sub.Subscribe(TOPIC_EVENTS);
    for (const std::string &t : config.topics) sub.Subscribe(t);
    lastGossipMs = NowMs() - config.gossipMs;   // gossip on the first Tick
}

int Peer::Claim(int requested) {
    if (requested >= config.maxPeers) {
        lastError = "id " + std::to_string(requested) + " is outside 0.." + std::to_string(config.maxPeers - 1);
        return -1;
    }
    if (requested >= 0) {
        // 0MQ releases a closed socket's port on its I/O thread, a moment
        // after close returns. A peer that takes over a slot the instant its
        // previous holder left - a restart, a replacement - would find the
        // port still bound, so a requested slot gets a few tries before it is
        // reported taken.
        for (int attempt = 0; attempt < 10; ++attempt) {
            if (pub.Bind("tcp://*:" + std::to_string(config.portBase + requested))) return id = requested;
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        lastError = pub.LastError() + " - is another peer already running as id " + std::to_string(requested) + "?";
        return -1;
    }
    for (int i = 0; i < config.maxPeers; ++i) {
        if (pub.Bind("tcp://*:" + std::to_string(config.portBase + i))) return id = i;
    }
    lastError = "all " + std::to_string(config.maxPeers) + " peer slots are taken (ports " +
                std::to_string(config.portBase) + "-" + std::to_string(config.portBase + config.maxPeers - 1) + ")";
    return -1;
}

void Peer::ConnectAll() {
    for (int i = 0; i < config.maxPeers; ++i) {
        if (i != id) sub.Connect("tcp://localhost:" + std::to_string(config.portBase + i));
    }
}

// --- shared clock ------------------------------------------------------------

void Peer::EnableSharedClock() {
    std::lock_guard<std::mutex> lock(mutex);
    clockEnabled = true;
    epoch = Timeline::RealTimeNow();
    epochProposer = id;
    sharedClock.SetOrigin(epoch);
}

double Peer::SharedSeconds() {
    return sharedClock.GetSeconds();
}

int64_t Peer::Epoch() const { std::lock_guard<std::mutex> lock(mutex); return epoch; }
int Peer::EpochProposer() const { std::lock_guard<std::mutex> lock(mutex); return epochProposer; }
int Peer::EpochAdoptions() const { std::lock_guard<std::mutex> lock(mutex); return epochAdoptions; }

bool Peer::HeardFrom(int peerId) const {
    std::lock_guard<std::mutex> lock(mutex);
    return peerId >= 0 && peerId < config.maxPeers && peerHeard[static_cast<size_t>(peerId)];
}

bool Peer::AgreesOnEpoch(int peerId) const {
    std::lock_guard<std::mutex> lock(mutex);
    return peerId >= 0 && peerId < config.maxPeers && peerHeard[static_cast<size_t>(peerId)] &&
           peerEpoch[static_cast<size_t>(peerId)] == epoch;
}

void Peer::OnEpoch(std::istringstream &in,
                   const std::function<void(const std::string &, std::istringstream &, int)> &onMessage) {
    int64_t theirEpoch = 0;
    int theirProposer = -1, from = -1;
    if (!(in >> theirEpoch >> theirProposer >> from)) return;
    if (from < 0 || from >= config.maxPeers) return;
    {
        std::lock_guard<std::mutex> lock(mutex);
        peerEpoch[static_cast<size_t>(from)] = theirEpoch;
        peerHeard[static_cast<size_t>(from)] = true;
        if (clockEnabled && EarlierEpoch(theirEpoch, theirProposer, epoch, epochProposer)) {
            epoch = theirEpoch;
            epochProposer = theirProposer;
            ++epochAdoptions;
            sharedClock.SetOrigin(theirEpoch);   // the world jumps to where everyone else's is
        }
    }
    if (onMessage) onMessage(TOPIC_EPOCH, in, from);   // body now sits at the game's extras
}

// --- event log ---------------------------------------------------------------

void Peer::AppendEvent(const std::string &payload) {
    std::lock_guard<std::mutex> lock(mutex);
    Event e;
    e.owner = id;
    e.seq = nextSeq++;
    e.payload = payload;
    eventIds.insert({e.owner, e.seq});
    events.push_back(e);
    eventsDirty = true;
}

bool Peer::TakeEventsDirty() {
    std::lock_guard<std::mutex> lock(mutex);
    const bool d = eventsDirty;
    eventsDirty = false;
    return d;
}

std::vector<Peer::Event> Peer::Events() const {
    std::lock_guard<std::mutex> lock(mutex);
    return events;
}

size_t Peer::EventCount() const {
    std::lock_guard<std::mutex> lock(mutex);
    return events.size();
}

// One "EVENTS <owner> <count> (<seq> <bytes> <payload>)*" per owner - payloads
// are length-prefixed so a game may put anything in them, spaces included.
// Every owner's, not just mine: a peer that has left can no longer re-send its
// own, and without this a late joiner would never learn them.
std::vector<std::string> Peer::EncodeKnownEvents() const {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<std::string> batches;
    for (int owner = 0; owner < config.maxPeers; ++owner) {
        size_t count = 0;
        for (const Event &e : events) if (e.owner == owner) ++count;
        if (count == 0) continue;
        std::ostringstream out;
        out << TOPIC_EVENTS << ' ' << owner << ' ' << count;
        for (const Event &e : events) {
            if (e.owner != owner) continue;
            out << ' ' << e.seq << ' ' << e.payload.size() << ' ' << e.payload;
        }
        batches.push_back(out.str());
    }
    return batches;
}

void Peer::OnEvents(std::istringstream &in) {
    int owner = -1;
    size_t count = 0;
    if (!(in >> owner >> count)) return;
    if (owner < 0 || owner >= config.maxPeers) return;
    std::lock_guard<std::mutex> lock(mutex);
    for (size_t i = 0; i < count; ++i) {
        int64_t seq = 0;
        size_t bytes = 0;
        if (!(in >> seq >> bytes)) break;
        in.get();   // the single space before the payload
        std::string payload(bytes, '\0');
        in.read(&payload[0], static_cast<std::streamsize>(bytes));
        if (in.gcount() != static_cast<std::streamsize>(bytes)) break;
        if (eventIds.insert({owner, seq}).second) {
            events.push_back({owner, seq, payload});
            eventsDirty = true;
        }
    }
}

// --- talking -------------------------------------------------------------------

void Peer::Publish(const std::string &topic, const std::string &payload) {
    pub.Send(topic + ' ' + payload);
}

void Peer::Tick(const std::function<void(const std::string &, std::istringstream &, int)> &onMessage,
                const std::function<std::string()> &epochExtras,
                const std::function<std::vector<std::string>()> &extraGossip) {
    // 1. Hear everyone. Drain everything queued.
    std::string message;
    while (sub.Receive(message)) {
        ++messagesHeard;
        std::istringstream in(message);
        std::string topic;
        in >> topic;
        if (topic == TOPIC_EPOCH)       OnEpoch(in, onMessage);
        else if (topic == TOPIC_EVENTS) OnEvents(in);
        else if (onMessage)             onMessage(topic, in, -1);
    }

    // 2. Gossip on wall time. Wall, not game: this is the network's heartbeat
    //    and must keep going while a peer's own game time is paused.
    const long long now = NowMs();
    if (now - lastGossipMs < config.gossipMs) return;
    lastGossipMs = now;

    bool clock;
    int64_t e; int p;
    {
        std::lock_guard<std::mutex> lock(mutex);
        clock = clockEnabled; e = epoch; p = epochProposer;
    }
    if (clock) {
        std::ostringstream out;
        out << e << ' ' << p << ' ' << id;
        if (epochExtras) out << ' ' << epochExtras();
        Publish(TOPIC_EPOCH, out.str());
    }
    for (const std::string &batch : EncodeKnownEvents()) {
        pub.Send(batch);
    }
    if (extraGossip) {
        for (const std::string &g : extraGossip()) pub.Send(g);
    }
}

} // namespace engine
